#include "strategy_zscore_mean_reversion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <unordered_set>
#include <vector>

#include "indicator_graph.h"
#include "indicator_graph.h"
#include "strategy_executor.h"
#include "series_view.h"
#include "utils.h"
#include "sweep.h"
#include "op_kernels_math.h"

namespace sugar {

    namespace {

        inline std::size_t warmup_index_for_plan(const ZScoreMeanReversionOptions& opt) {
            // ZScoreSMA: first defined at (n-1)
            // ZScoreEMA: first defined at (2n-2)
            if (opt.z_n == 0) return 0;
            return opt.use_ema ? (2 * opt.z_n - 2) : (opt.z_n - 1);
        }

        // Signature used to suppress plateau duplicates in Top-K.
        struct ResultSig {
            std::size_t trades{};
            std::size_t long_trades{};
            std::size_t short_trades{};
            TimeKey best_start_date{};
            bool has_best_start_date{};
            int pnl_bp{};
            int dd_bp{};

            bool operator==(const ResultSig& o) const {
                return trades == o.trades
                    && long_trades == o.long_trades
                    && short_trades == o.short_trades
                    && best_start_date == o.best_start_date
                    && has_best_start_date == o.has_best_start_date
                    && pnl_bp == o.pnl_bp
                    && dd_bp == o.dd_bp;
            }
        };

        struct ResultSigHash {
            std::size_t operator()(const ResultSig& s) const noexcept {
                auto h = std::hash<std::size_t>{}(s.trades);
                auto mix = [&h](std::size_t v) {
                    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                };
                mix(std::hash<std::size_t>{}(s.long_trades));
                mix(std::hash<std::size_t>{}(s.short_trades));
                mix(std::hash<TimeKey>{}(s.best_start_date));
                mix(std::hash<bool>{}(s.has_best_start_date));
                mix(std::hash<int>{}(s.pnl_bp));
                mix(std::hash<int>{}(s.dd_bp));
                return h;
            }
        };

        // Build [min..max] inclusive steps for ints
        static std::vector<std::size_t> build_int_values(std::size_t mn, std::size_t mx, std::size_t step) {
            std::vector<std::size_t> v;
            step = std::max<std::size_t>(1, step);
            if (mx < mn) std::swap(mx, mn);
            for (std::size_t x = mn; x <= mx; x += step) v.push_back(x);
            if (v.empty()) v.push_back(mn);
            return v;
        }

        // Build [min..max] inclusive-ish steps for doubles
        static std::vector<double> build_double_values(double mn, double mx, double step) {
            std::vector<double> v;
            step = std::max(1e-12, step);
            if (mx < mn) std::swap(mx, mn);

            // Avoid floating drift: count steps by rounding span/step
            const double span = mx - mn;
            const std::size_t n = (span <= 0.0) ? 1 : (static_cast<std::size_t>(std::llround(span / step)) + 1);

            v.reserve(std::max<std::size_t>(1, n));
            for (std::size_t i = 0; i < n; ++i) {
                v.push_back(mn + static_cast<double>(i) * step);
            }

            // Ensure we hit mx if we're very close
            if (!v.empty() && std::fabs(v.back() - mx) > 1e-9) v.push_back(mx);
            if (v.empty()) v.push_back(mn);
            return v;
        }

        static BacktestResult invalid_result() {
            BacktestResult r{};
            r.pnl = std::numeric_limits<double>::quiet_NaN();
            r.max_drawdown = std::numeric_limits<double>::quiet_NaN();
            r.trades = 0;
            r.long_trades = 0;
            r.short_trades = 0;
            return r;
        }

        // Sweeper core (index-based, coarse→fine using shared sweep framework)
        static TopK<ZScoreMeanReversionParams> sweep_zscore_mean_reversion_topk(
            const CandleSeries& series,
            const ZScoreMeanReversionSweepRanges& ranges,
            const ExecSettings& exec)
        {
            // Precompute value grids
            const auto n_vals = build_int_values(ranges.z_n_min, ranges.z_n_max, ranges.z_n_step);
            const auto enter_vals = build_double_values(ranges.z_enter_min, ranges.z_enter_max, ranges.z_enter_step);
            const auto exit_vals = build_double_values(ranges.z_exit_min, ranges.z_exit_max, ranges.z_exit_step);

            std::vector<TradeMode> modes;
            modes.reserve(2);
            if (ranges.include_long_only) modes.push_back(TradeMode::LongOnly);
            if (ranges.include_long_short) modes.push_back(TradeMode::LongShort);
            if (modes.empty()) modes.push_back(TradeMode::LongShort);

            const std::size_t N = n_vals.size();
            const std::size_t E = enter_vals.size();
            const std::size_t X = exit_vals.size();
            const std::size_t M = modes.size();

            auto pack = [&](std::size_t ni, std::size_t ei, std::size_t xi, std::size_t mi) -> std::size_t {
                return (((ni * E) + ei) * X + xi) * M + mi;
            };

            auto unpack = [&](std::size_t idx, std::size_t& ni, std::size_t& ei, std::size_t& xi, std::size_t& mi) {
                mi = idx % M; idx /= M;
                xi = idx % X; idx /= X;
                ei = idx % E; idx /= E;
                ni = idx;
            };

            const std::size_t param_count = N * E * X * M;

            auto idx_to_params = [&](std::size_t idx) -> ZScoreMeanReversionParams {
                std::size_t ni{}, ei{}, xi{}, mi{};
                unpack(idx, ni, ei, xi, mi);
                return ZScoreMeanReversionParams{
                    n_vals[ni],
                    enter_vals[ei],
                    exit_vals[xi],
                    modes[mi],
                    ranges.use_ema
                };
            };

            auto is_valid = [&](const ZScoreMeanReversionParams& p) -> bool {
                if (p.z_n < 2) return false;
                if (!(p.z_enter >= 0.0) || !(p.z_exit >= 0.0)) return false;
                // Strongly preferred: exit <= enter (otherwise you’ll often insta-exit)
                if (p.z_exit > p.z_enter) return false;
                return true;
            };

            auto eval = [&](const CandleSeries& srs, const ZScoreMeanReversionParams& p) -> BacktestResult {
                ZScoreMeanReversionOptions opt{};
                opt.z_n = p.z_n;
                opt.z_enter = p.z_enter;
                opt.z_exit = p.z_exit;
                opt.use_ema = p.use_ema;
                opt.mode = p.mode;
                opt.exec = exec;
                return run_zscore_mean_reversion(srs, opt);
            };

            auto eval_i = [&](const CandleSeries& srs, std::size_t idx) -> BacktestResult {
                const auto p = idx_to_params(idx);
                if (!is_valid(p)) return invalid_result();
                return eval(srs, p);
            };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return default_sweep_score(r, ranges.score_policy);
            };

            // -------------------------
            // Coarse stage index subset
            // -------------------------
            const std::size_t legacy_mult_int = 5;
            const double legacy_mult_dbl = 2.0;

            auto coarse_step_int = [&](std::size_t lo, std::size_t hi, std::size_t base_step) -> std::size_t {
                base_step = std::max<std::size_t>(1, base_step);
                if (ranges.coarse_mesh_count == 0 || hi <= lo) return std::max<std::size_t>(1, base_step * legacy_mult_int);
                const std::size_t span = hi - lo;
                const std::size_t denom = (ranges.coarse_mesh_count <= 1) ? 1 : (ranges.coarse_mesh_count - 1);
                std::size_t raw = std::max<std::size_t>(1, span / denom);
                const std::size_t k = (raw + base_step - 1) / base_step;
                return std::max<std::size_t>(1, k * base_step);
            };

            auto coarse_step_dbl = [&](double lo, double hi, double base_step) -> double {
                base_step = std::max(1e-12, base_step);
                if (ranges.coarse_mesh_count == 0 || hi <= lo) return base_step * legacy_mult_dbl;
                const double span = hi - lo;
                const std::size_t denom = (ranges.coarse_mesh_count <= 1) ? 1 : (ranges.coarse_mesh_count - 1);
                const double raw = std::max(base_step, span / static_cast<double>(denom));
                const std::size_t k = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(raw / base_step)));
                return static_cast<double>(k) * base_step;
            };

            const std::size_t coarse_n_step = coarse_step_int(ranges.z_n_min, ranges.z_n_max, ranges.z_n_step);
            const double coarse_enter_step = coarse_step_dbl(ranges.z_enter_min, ranges.z_enter_max, ranges.z_enter_step);
            const double coarse_exit_step  = coarse_step_dbl(ranges.z_exit_min, ranges.z_exit_max, ranges.z_exit_step);

            auto stride_int = [&](std::size_t coarse_step, std::size_t base_step) -> std::size_t {
                base_step = std::max<std::size_t>(1, base_step);
                return std::max<std::size_t>(1, coarse_step / base_step);
            };
            auto stride_dbl = [&](double coarse_step, double base_step) -> std::size_t {
                base_step = std::max(1e-12, base_step);
                const double raw = coarse_step / base_step;
                return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(raw)));
            };

            const std::size_t n_stride = stride_int(coarse_n_step, ranges.z_n_step);
            const std::size_t e_stride = stride_dbl(coarse_enter_step, ranges.z_enter_step);
            const std::size_t x_stride = stride_dbl(coarse_exit_step, ranges.z_exit_step);

            std::vector<std::size_t> stage1_indices;
            stage1_indices.reserve((N / n_stride + 1) * (E / e_stride + 1) * (X / x_stride + 1));

            for (std::size_t ni = 0; ni < N; ni += n_stride) {
                for (std::size_t ei = 0; ei < E; ei += e_stride) {
                    for (std::size_t xi = 0; xi < X; xi += x_stride) {
                        for (std::size_t mi = 0; mi < M; ++mi) {
                            const auto p = ZScoreMeanReversionParams{ n_vals[ni], enter_vals[ei], exit_vals[xi], modes[mi], ranges.use_ema };
                            if (!is_valid(p)) continue;
                            stage1_indices.push_back(pack(ni, ei, xi, mi));
                        }
                    }
                }
            }

            // Neighborhood definition: +/-1 step in each axis (mode held fixed)
            auto neighbors = [&](std::size_t idx) -> std::vector<std::size_t> {
                std::size_t ni{}, ei{}, xi{}, mi{};
                unpack(idx, ni, ei, xi, mi);

                std::vector<std::size_t> out;
                out.reserve(1 + 2 * 3);
                out.push_back(idx);

                auto push = [&](std::size_t nni, std::size_t nei, std::size_t nxi, std::size_t nmi) {
                    out.push_back(pack(nni, nei, nxi, nmi));
                };

                if (ni > 0)     push(ni - 1, ei, xi, mi);
                if (ni + 1 < N) push(ni + 1, ei, xi, mi);

                if (ei > 0)     push(ni, ei - 1, xi, mi);
                if (ei + 1 < E) push(ni, ei + 1, xi, mi);

                if (xi > 0)     push(ni, ei, xi - 1, mi);
                if (xi + 1 < X) push(ni, ei, xi + 1, mi);

                return out;
            };

            const std::size_t k_stage1 = std::max<std::size_t>(ranges.top_k, 25);
            const std::size_t k_stage2 = std::max<std::size_t>(ranges.top_k, 50);

            SweepIndexProblem problem{};
            problem.param_count = param_count;
            problem.eval = eval_i;
            problem.neighbors = neighbors;

            CoarseFineIndexConfig cfg{};
            cfg.stage1_indices = std::make_shared<std::vector<std::size_t>>(std::move(stage1_indices));
            cfg.k_stage1 = k_stage1;
            cfg.k_stage2 = k_stage2;
            cfg.opt.depollute_stage1 = true;
            cfg.opt.dedupe_stage2 = true;

            const std::size_t hops = std::max<std::size_t>(1, static_cast<std::size_t>(std::max(0, ranges.fine_radius)));
            cfg.opt.suppression_hops = hops;
            cfg.opt.stage2_hops = hops;

            CoarseFineIndexSweep sweep(cfg);

            // Progress: print to stdout (matches GDC behavior)
            auto progress_cb = make_ostream_progress_cb(std::cout);
            const auto run = sweep.run(series, problem, score_fn, progress_cb);
            std::cout << "\n";

            // Deduplicate by ResultSig to keep Top-K diverse
            auto result_key = [](const ZScoreMeanReversionParams&, const BacktestResult& r) -> ResultSig {
                return ResultSig{
                    r.trades,
                    r.long_trades,
                    r.short_trades,
                    r.best_start_date,
                    r.has_best_start_date,
                    static_cast<int>(std::lround(r.pnl * 100.0)),
                    static_cast<int>(std::lround(r.max_drawdown * 100.0))
                };
            };

            TopK<ZScoreMeanReversionParams> out_top(ranges.top_k);
            std::unordered_set<ResultSig, ResultSigHash> seen;
            seen.reserve(run.best_first.size());

            for (const auto& c : run.best_first) {
                const auto p = idx_to_params(c.index);
                const auto key = result_key(p, c.result);
                if (!seen.insert(key).second) continue;
                out_top.consider(c.score, c.result, p);
                if (out_top.sorted_best_first().size() >= ranges.top_k) break;
            }

            return out_top;
        }

    } // namespace

    // ============================================================================
    // 2) PLAN BUILDER — Strategy identity
    // ============================================================================
    StrategyPlan build_zscore_mean_reversion_plan(const ZScoreMeanReversionOptions& opt) {
        IndicatorPtr z = std::make_shared<MapIndicator>(
            std::make_shared<CloseIndicator>(),
            [n = opt.z_n, use_ema = opt.use_ema](const std::vector<double>& v) {
                const auto span = std::span<const double>(v.data(), v.size());
                return use_ema
                    ? op_kernels_math::zscore_ema(span, n)
                    : op_kernels_math::zscore_sma(span, n);
            });

        // Entries
        //   Long  when z <= -enter
        //   Short when z >= +enter
        BoolSignalPtr long_enter  = ble(z, -opt.z_enter);
        BoolSignalPtr short_enter = bge(z,  opt.z_enter);

        // Exits (symmetric band towards zero)
        //   Exit long  when z >= -exit
        //   Exit short when z <= +exit
        BoolSignalPtr long_exit  = bge(z, -opt.z_exit);
        BoolSignalPtr short_exit = ble(z,  opt.z_exit);

        StrategyPlan plan{};
        plan.name = "ZScoreMeanReversion";
        plan.mode = opt.mode;

        plan.signals.entry_long  = std::move(long_enter);
        plan.signals.exit_long   = std::move(long_exit);
        plan.signals.entry_short = std::move(short_enter);
        plan.signals.exit_short  = std::move(short_exit);

        plan.first_valid_index = warmup_index_for_plan(opt);
        return plan;
    }

    // ============================================================================
    // 3) RUNNER — Thin execution wrapper
    // ============================================================================
    BacktestResult run_zscore_mean_reversion(const CandleSeries& series,
        const ZScoreMeanReversionOptions& opt)
    {
        StrategyPlan plan = build_zscore_mean_reversion_plan(opt);
        StrategyExecutor exec;
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        if (!make_single_series_dataview_from_candles(series, bundle, primary)) {
            return BacktestResult{};
        }
        const DataView dv(*bundle);
        return exec.run(dv, primary, plan, opt.exec);
    }

    // ============================================================================
    // UI prompt helpers
    // ============================================================================
    std::string zscore_mean_reversion_params_to_string(const ZScoreMeanReversionParams& p) {
        std::ostringstream oss;
        oss << "z_n=" << p.z_n
            << " enter=" << std::fixed << std::setprecision(6) << p.z_enter
            << " exit=" << std::fixed << std::setprecision(6) << p.z_exit
            << " mode=" << ((p.mode == TradeMode::LongOnly) ? "Long" : "LongShort")
            << " z=" << (p.use_ema ? "EMA" : "SMA");
        return oss.str();
    }

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        ZScoreMeanReversionSweepRanges& r)
    {
        out << "\n--- ZScore Mean Reversion sweep ranges ---\n";
        prompt_range(in, out, "Z period n (min max step)", r.z_n_min, r.z_n_max, r.z_n_step);
        prompt_range(in, out, "Entry threshold |z| >= enter (min max step)", r.z_enter_min, r.z_enter_max, r.z_enter_step);
        prompt_range(in, out, "Exit threshold (min max step)", r.z_exit_min, r.z_exit_max, r.z_exit_step);

        prompt_scalar(in, out,
            "Coarse mesh count (0=legacy. 5=typical coarse density)",
            r.coarse_mesh_count);
        prompt_scalar(in, out,
            "Fine local radius (1=default, higher=more local exploration)",
            r.fine_radius);

        prompt_scalar(in, out, "Top-K results to keep", r.top_k);

        out << "Score policy: score = pnl - dd_penalty*dd\n";
        prompt_scalar(in, out, "dd_penalty", r.score_policy.dd_penalty);
        prompt_scalar(in, out, "min_trades", r.score_policy.min_trades);
    }

    // ============================================================================
    // 5) SWEEP ORCHESTRATION — Optimization + reporting
    // ============================================================================
    bool run_zscore_mean_reversion_sweep_best(const CandleSeries& series,
        const ZScoreMeanReversionSweepRanges& ranges,
        TradeMode run_mode,
        const ExecSettings& exec,
        std::ostream& out)
    {
        // Apply runtime mode filter so the menu selection actually matters.
        auto filtered = ranges;
        if (run_mode == TradeMode::LongOnly) {
            filtered.include_long_only = true;
            filtered.include_long_short = false;
        } else {
            filtered.include_long_only = false;
            filtered.include_long_short = true;
        }

        out << "\n[ZScore Mean Reversion Sweep]\n";
        out << "  z_n: " << filtered.z_n_min << " -> " << filtered.z_n_max << " step " << filtered.z_n_step << "\n";
        out << "  enter: " << filtered.z_enter_min << " -> " << filtered.z_enter_max << " step " << filtered.z_enter_step << "\n";
        out << "  exit: " << filtered.z_exit_min << " -> " << filtered.z_exit_max << " step " << filtered.z_exit_step << "\n";
        out << "  modes: " << (filtered.include_long_only ? "LongOnly " : "") << (filtered.include_long_short ? "LongShort" : "") << "\n";
        out << "  z_type: " << (filtered.use_ema ? "EMA" : "SMA") << "\n";
        out << "  score: pnl - " << filtered.score_policy.dd_penalty << "*dd"
            << " (min_trades=" << filtered.score_policy.min_trades << ")\n";
        out << "  top_k: " << filtered.top_k << "\n";
        out << "  coarse_mesh_count: " << filtered.coarse_mesh_count << " (5 = typical)\n";
        out << "  fine_radius: " << filtered.fine_radius << " (1=default)\n";

        auto topk = sweep_zscore_mean_reversion_topk(series, filtered, exec);
        auto best = topk.sorted_best_first();

        if (best.empty()) {
            out << "\nNo sweep candidates produced a finite score.\n";
            return false;
        }

        out << "\nTop candidates:\n";
        const std::size_t show = std::min<std::size_t>(best.size(), filtered.top_k);
        for (std::size_t i = 0; i < show; ++i) {
            out << "\n  #" << (i + 1)
                << " score=" << best[i].score
                << " | " << zscore_mean_reversion_params_to_string(best[i].params) << "\n";
            print_backtest_summary(best[i].result, out, "    ");
        }

        auto winner = best.front().params;
        winner.mode = run_mode; // enforce

        ZScoreMeanReversionOptions opt{};
        opt.z_n = winner.z_n;
        opt.z_enter = winner.z_enter;
        opt.z_exit = winner.z_exit;
        opt.use_ema = winner.use_ema;
        opt.mode = winner.mode;
        opt.exec = exec;

        out << "\nBest ZScore Mean Reversion (re-run): " << zscore_mean_reversion_params_to_string(winner) << "\n";
        const auto final_res = run_zscore_mean_reversion(series, opt);
        print_backtest_summary(final_res, out, "  ");

        return true;
    }

} // namespace sugar
