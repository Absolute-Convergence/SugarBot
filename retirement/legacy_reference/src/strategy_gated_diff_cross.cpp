#include "strategy_gated_diff_cross.h"

/*
===============================================================================
 IMPLEMENTATION NOTES

 This file is intentionally long and explicit because it acts as a
 *reference implementation* for:

   - parameterized strategy execution
   - Top-K sweeps
   - coarse → fine local refinement
   - result deduplication
   - deterministic reruns

 Future strategies may reuse the *structure* without reusing the *mechanics*.
===============================================================================
*/

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>

#include "indicator_graph.h"
#include "indicator_precomputed.h"
#include "indicator_graph.h"
#include "op_kernels_ts.h"
#include "strategy_executor.h"
#include "series_view.h"
#include "sweep.h"
#include "backtester.h"
#include "utils.h"

namespace sugar {

    namespace {

        // Result signature used to suppress plateau duplicates in Top-K.
        // Nearby parameter combos can produce identical discrete trades,
        // yielding identical PnL/DD/trade counts. Deduping by this signature
        // keeps Top-K diverse without strategy-specific hacks.
        struct ResultSig {
            std::size_t trades{};
            std::size_t long_trades{};
            std::size_t short_trades{};
            TimeKey best_start_date{};
            bool has_best_start_date{};
            int pnl_bp{}; // PnL in basis points (percent * 100)
            int dd_bp{};  // DD in basis points (percent * 100)

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
                    // basic hash combine
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

        struct GDCSweepDiag {
            std::size_t coarse_candidates{ 0 };
            std::size_t stage1_kept{ 0 };
            std::size_t fine_candidates{ 0 };
        };

        // Captured per-thread so run_* can report whether coarse→fine actually happened.
        thread_local GDCSweepDiag g_last_gdc_diag{};

        struct GateStats {
            double allow_rate_pct{ 0.0 };
            std::size_t entry_signals{ 0 };
            std::size_t entries_blocked{ 0 };
        };

        GateStats compute_gate_stats(const CandleSeries& series, const StrategyPlan& plan) {
            SeriesView view = SeriesView::from_series(series);

            BoolSeries allow;
            if (plan.signals.allow_trade) {
                allow = plan.signals.allow_trade->compute(view);
            }
            else {
                allow.assign(view.size(), 1u);
            }

            BoolSeries eL;
            if (plan.signals.entry_long) eL = plan.signals.entry_long->compute(view);
            BoolSeries eS;
            if (plan.signals.entry_short) eS = plan.signals.entry_short->compute(view);

            const std::size_t n = view.size();

            std::size_t allow_count = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t a = (i < allow.size()) ? allow[i] : 1u;
                if (a) ++allow_count;
            }

            std::size_t entry_count = 0;
            std::size_t blocked = 0;

            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t a = (i < allow.size()) ? allow[i] : 1u;
                const std::uint8_t l = (i < eL.size()) ? eL[i] : 0u;
                const std::uint8_t s = (i < eS.size()) ? eS[i] : 0u;

                const bool entry = (l != 0u) || (s != 0u);
                if (entry) {
                    ++entry_count;
                    if (!a) ++blocked;
                }
            }

            GateStats gs{};
            gs.allow_rate_pct = (n == 0) ? 0.0 : (100.0 * static_cast<double>(allow_count) / static_cast<double>(n));
            gs.entry_signals = entry_count;
            gs.entries_blocked = blocked;
            return gs;
        }

        inline std::size_t warmup_index_for_plan(const GatedDiffCrossOptions& opt) {
            // SMA/EMA: first valid at (n-1)
            // ATR: first valid at (n-1)
            // Cross uses i-1, so we bump by +1.
            const std::size_t w = std::max({ opt.fast_n, opt.slow_n, opt.atr_n });
            return w; // (max(n)-1) + 1
        }

        IndicatorPtr make_ma_indicator(IndicatorPtr base, std::size_t n, bool use_ema) {
            return std::make_shared<MapIndicator>(
                std::move(base),
                [n, use_ema](const std::vector<double>& v) {
                    const auto s = std::span<const double>(v.data(), v.size());
                    return use_ema ? op_kernels::ts_ema(s, n) : op_kernels::ts_sma(s, n);
                });
        }

        IndicatorPtr make_binary_op(
            IndicatorPtr lhs,
            IndicatorPtr rhs,
            std::function<std::vector<double>(std::span<const double>, std::span<const double>)> kernel) {
            return std::make_shared<BinaryMapIndicator>(
                std::move(lhs),
                std::move(rhs),
                [kernel](const std::vector<double>& a, const std::vector<double>& b) {
                    return kernel(
                        std::span<const double>(a.data(), a.size()),
                        std::span<const double>(b.data(), b.size()));
                });
        }

        IndicatorPtr make_unary_op(
            IndicatorPtr base,
            std::function<std::vector<double>(std::span<const double>)> kernel) {
            return std::make_shared<MapIndicator>(
                std::move(base),
                [kernel](const std::vector<double>& v) {
                    return kernel(std::span<const double>(v.data(), v.size()));
                });
        }

        IndicatorPtr make_atr_indicator(std::size_t n) {
            return make_precomputed("gdc.atr", [n](const SeriesView& view) {
                const double qnan = std::numeric_limits<double>::quiet_NaN();
                const auto close = view.close();
                const auto high = view.has_high() ? view.high() : close;
                const auto low = view.has_low() ? view.low() : close;

                std::vector<double> tr(close.size(), qnan);
                if (close.empty()) return tr;

                const double h0 = high[0];
                const double l0 = low[0];
                if (std::isfinite(h0) && std::isfinite(l0)) {
                    tr[0] = h0 - l0;
                }

                for (std::size_t i = 1; i < close.size(); ++i) {
                    const double h = high[i];
                    const double l = low[i];
                    const double pc = close[i - 1];
                    if (!std::isfinite(h) || !std::isfinite(l) || !std::isfinite(pc)) {
                        tr[i] = qnan;
                        continue;
                    }

                    const double hl = h - l;
                    const double hc = std::fabs(h - pc);
                    const double lc = std::fabs(l - pc);
                    tr[i] = std::max({ hl, hc, lc });
                }

                return op_kernels::ts_atr_wilder(std::span<const double>(tr.data(), tr.size()), n);
            });
        }

    } // namespace

    // ============================================================================
    // 2) PLAN BUILDER — Strategy identity
    //
    // If it affects how trades enter/exit, it belongs here.
    // ============================================================================

    StrategyPlan build_gated_diff_cross_plan(const GatedDiffCrossOptions& opt) {
        // Base price series
        IndicatorPtr close = std::make_shared<CloseIndicator>();

        // Choose MA type
        IndicatorPtr fast = make_ma_indicator(close, opt.fast_n, opt.use_ema);
        IndicatorPtr slow = make_ma_indicator(close, opt.slow_n, opt.use_ema);

        // Diff (fast - slow)
        IndicatorPtr diff = make_binary_op(fast, slow, op_kernels::ts_sub);

        // Gates:
        //  1) abs(diff) >= diff_min
        //  2) ATR / close >= atr_frac_min
        IndicatorPtr diff_abs = make_unary_op(diff, op_kernels::ts_abs);
        BoolSignalPtr diff_ok = bge(diff_abs, opt.diff_min);

        IndicatorPtr atr = make_atr_indicator(opt.atr_n);
        IndicatorPtr atr_frac = make_binary_op(atr, close, op_kernels::ts_div);
        BoolSignalPtr atr_ok = bge(atr_frac, opt.atr_frac_min);

        // Cross conditions
        //
        // IMPORTANT: avoid reusing a BoolSignalPtr after it has been passed into
        // composition helpers (band/bor/band_not/etc.). Those helpers typically
        // take pointers by value and std::move into the composed node, which can
        // leave a reused local pointer null and silently disable a leg (ex: exits).
        //
        // Create independent signal objects for entry vs exit.
        BoolSignalPtr cross_up_entry = cross_up(fast, slow);
        BoolSignalPtr cross_dn_entry = cross_down(fast, slow);
        BoolSignalPtr cross_dn_exit = cross_down(fast, slow);
        BoolSignalPtr cross_up_exit = cross_up(fast, slow);

        // Entry conditions
        BoolSignalPtr long_enter = band(band(cross_up_entry, diff_ok), atr_ok);
        BoolSignalPtr short_enter = band(band(cross_dn_entry, diff_ok), atr_ok);

        // Exits
        BoolSignalPtr long_exit = cross_dn_exit;
        BoolSignalPtr short_exit = cross_up_exit;

        StrategyPlan plan{};
        plan.name = "GatedDiffCross";
        plan.mode = opt.mode;

        plan.signals.entry_long = std::move(long_enter);
        plan.signals.exit_long = std::move(long_exit);
        plan.signals.entry_short = std::move(short_enter);
        plan.signals.exit_short = std::move(short_exit);
        plan.signals.allow_trade = std::move(atr_ok); // global gate for reporting / future

        plan.first_valid_index = warmup_index_for_plan(opt);
        return plan;
    }


    // ============================================================================
    // 3) RUNNER — Thin execution wrapper
    // ============================================================================
    BacktestResult run_gated_diff_cross(const CandleSeries& series,
        const GatedDiffCrossOptions& opt)
    {
        StrategyPlan plan = build_gated_diff_cross_plan(opt);
        StrategyExecutor exec;
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        if (!make_single_series_dataview_from_candles(series, bundle, primary)) {
            return BacktestResult{};
        }
        const DataView dv(*bundle);
        return exec.run(dv, primary, plan, opt.exec);
    }

    // ------------------------------------------------------------
    // UI prompt helpers
    // ------------------------------------------------------------

    std::string gated_diff_cross_params_to_string(const GatedDiffCrossParams& p) {
        std::ostringstream oss;
        oss << "fast=" << p.fast_n
            << " slow=" << p.slow_n
            << " diff_min=" << std::fixed << std::setprecision(6) << p.diff_min
            << " atr_n=" << p.atr_n
            << " atr_frac_min=" << std::fixed << std::setprecision(6) << p.atr_frac_min
            << " mode=" << ((p.mode == TradeMode::LongOnly) ? "Long" : "LongShort")
            << " ma=" << (p.use_ema ? "EMA" : "SMA");
        return oss.str();
    }

    void prompt_gated_diff_cross_sweep_ranges(std::istream& in,
        std::ostream& out,
        GatedDiffCrossSweepRanges& r)
    {
        out << "\n--- Gated Diff Cross sweep ranges ---\n";
        prompt_range(in, out, "Fast MA (min max step)", r.fast_min, r.fast_max, r.fast_step);
        prompt_range(in, out, "Slow MA (min max step)", r.slow_min, r.slow_max, r.slow_step);
        prompt_range(in, out, "Diff deadband (min max step)", r.diff_min_min, r.diff_min_max, r.diff_min_step);
        prompt_range(in, out, "ATR period (min max step)", r.atr_n_min, r.atr_n_max, r.atr_n_step);
        prompt_range(in, out, "ATR fraction gate (min max step)", r.atr_frac_min_min, r.atr_frac_min_max, r.atr_frac_min_step);
        // Defaults preserve current behavior if the user just hits Enter.
        prompt_scalar(in, out,
            "Coarse mesh count (higher value = higher density scan: n^5 grid density, default n = 5)",
            r.coarse_mesh_count);
        prompt_scalar(in, out,
            "Fine local radius (higher value = larger local searches."
            "\n   fine search space (2n+1)^(5), default n = 1, (recommended 1 or 2)",
            r.fine_radius);
    }

    // ============================================================================
    // 4–5) SWEEP LOGIC — Optimizer policy
    //
    // Everything below is SEARCH / OPTIMIZATION.
    // No trading semantics should be introduced here.
    // ============================================================================


    // ------------------------------------------------------------
    // Sweep
    // ------------------------------------------------------------
    TopK<GatedDiffCrossParams>
        sweep_gated_diff_cross_topk(const CandleSeries& series,
            const GatedDiffCrossSweepRanges& ranges,
            const ExecSettings& exec)
    {
        auto eval = [&](const CandleSeries& srs, const GatedDiffCrossParams& p) -> BacktestResult {
            GatedDiffCrossOptions opt{};
            opt.fast_n = p.fast_n;
            opt.slow_n = p.slow_n;
            opt.diff_min = p.diff_min;
            opt.atr_n = p.atr_n;
            opt.atr_frac_min = p.atr_frac_min;
            opt.mode = p.mode;
            opt.use_ema = p.use_ema;
            opt.exec = exec;
            return run_gated_diff_cross(srs, opt);
            };

        auto score_fn = [&](const BacktestResult& r) -> double {
            return default_sweep_score(r, ranges.score_policy);
            };

        // ---------------------------------------------------------------------
        // We define a *fine* (base-step aligned) cartesian grid as the full space.
        // Stage-1 runs on a coarse SUBSET of indices (mesh / legacy multipliers).
        // Stage-2 explores neighborhoods around seeds using neighbor-hops.
        // ---------------------------------------------------------------------

        const auto fast_vals_fine = make_range(ranges.fast_min, ranges.fast_max, std::max<std::size_t>(1, ranges.fast_step));
        const auto slow_vals_fine = make_range(ranges.slow_min, ranges.slow_max, std::max<std::size_t>(1, ranges.slow_step));
        const auto atr_vals_fine = make_range(ranges.atr_n_min, ranges.atr_n_max, std::max<std::size_t>(1, ranges.atr_n_step));

        const auto diff_vals_fine = make_drange(ranges.diff_min_min, ranges.diff_min_max, ranges.diff_min_step);
        const auto atrf_vals_fine = make_drange(ranges.atr_frac_min_min, ranges.atr_frac_min_max, ranges.atr_frac_min_step);

        std::vector<TradeMode> modes;
        if (ranges.include_long_only)  modes.push_back(TradeMode::LongOnly);
        if (ranges.include_long_short) modes.push_back(TradeMode::LongShort);
        if (modes.empty()) {
            return TopK<GatedDiffCrossParams>(ranges.top_k);
        }

        const std::size_t F = fast_vals_fine.size();
        const std::size_t S = slow_vals_fine.size();
        const std::size_t D = diff_vals_fine.size();
        const std::size_t A = atr_vals_fine.size();
        const std::size_t AF = atrf_vals_fine.size();
        const std::size_t M = modes.size();

        const std::size_t param_count = F * S * D * A * AF * M;
        g_last_gdc_diag.coarse_candidates = 0; // will be stage1_indices.size()

        auto pack = [&](std::size_t fi, std::size_t si, std::size_t di, std::size_t ai, std::size_t afi, std::size_t mi) -> std::size_t {
            // mixed radix packing
            std::size_t idx = fi;
            idx = idx * S + si;
            idx = idx * D + di;
            idx = idx * A + ai;
            idx = idx * AF + afi;
            idx = idx * M + mi;
            return idx;
            };

        auto unpack = [&](std::size_t idx, std::size_t& fi, std::size_t& si, std::size_t& di, std::size_t& ai, std::size_t& afi, std::size_t& mi) {
            mi = idx % M; idx /= M;
            afi = idx % AF; idx /= AF;
            ai = idx % A; idx /= A;
            di = idx % D; idx /= D;
            si = idx % S; idx /= S;
            fi = idx % F;
            };

        auto idx_to_params = [&](std::size_t idx) -> GatedDiffCrossParams {
            std::size_t fi{}, si{}, di{}, ai{}, afi{}, mi{};
            unpack(idx, fi, si, di, ai, afi, mi);
            return GatedDiffCrossParams{
                fast_vals_fine[fi],
                slow_vals_fine[si],
                diff_vals_fine[di],
                atr_vals_fine[ai],
                atrf_vals_fine[afi],
                modes[mi],
                ranges.use_ema
            };
            };

        auto is_valid = [&](const GatedDiffCrossParams& p) {
            if (p.fast_n == 0 || p.slow_n == 0) return false;
            if (p.fast_n >= p.slow_n) return false;
            if (p.atr_n == 0) return false;
            return true;
            };

        auto invalid_result = []() -> BacktestResult {
            BacktestResult r{};
            r.pnl = std::numeric_limits<double>::quiet_NaN();
            r.max_drawdown = std::numeric_limits<double>::quiet_NaN();
            r.trades = 0;
            r.long_trades = 0;
            r.short_trades = 0;
            return r;
            };

        // Index-based eval: early-out invalid candidates WITHOUT running a backtest.
        auto eval_i = [&](const CandleSeries& srs, std::size_t idx) -> BacktestResult {
            auto p = idx_to_params(idx);
            if (!is_valid(p)) return invalid_result();
            return eval(srs, p);
            };

        // Coarse stage: pick a subset of indices at a stride that matches coarse mesh intent.
        const std::size_t legacy_mult_int = 5;
        const double legacy_mult_dbl = 2.0;

        auto coarse_step_int = [&](std::size_t lo, std::size_t hi, std::size_t base_step) -> std::size_t {
            base_step = std::max<std::size_t>(1, base_step);
            if (ranges.coarse_mesh_count == 0 || hi <= lo) {
                return std::max<std::size_t>(1, base_step * legacy_mult_int);
            }
            const std::size_t span = hi - lo;
            const std::size_t denom = (ranges.coarse_mesh_count <= 1) ? 1 : (ranges.coarse_mesh_count - 1);
            std::size_t raw = std::max<std::size_t>(1, span / denom);
            const std::size_t k = (raw + base_step - 1) / base_step;
            return std::max<std::size_t>(1, k * base_step);
            };

        auto coarse_step_dbl = [&](double lo, double hi, double base_step) -> double {
            base_step = std::max(1e-12, base_step);
            if (ranges.coarse_mesh_count == 0 || hi <= lo) {
                return base_step * legacy_mult_dbl;
            }
            const double span = hi - lo;
            const std::size_t denom = (ranges.coarse_mesh_count <= 1) ? 1 : (ranges.coarse_mesh_count - 1);
            const double raw = std::max(base_step, span / static_cast<double>(denom));
            const std::size_t k = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(raw / base_step)));
            return static_cast<double>(k) * base_step;
            };

        const std::size_t coarse_fast_step = coarse_step_int(ranges.fast_min, ranges.fast_max, ranges.fast_step);
        const std::size_t coarse_slow_step = coarse_step_int(ranges.slow_min, ranges.slow_max, ranges.slow_step);
        const std::size_t coarse_atr_step = coarse_step_int(ranges.atr_n_min, ranges.atr_n_max, ranges.atr_n_step);
        const double coarse_diff_step = coarse_step_dbl(ranges.diff_min_min, ranges.diff_min_max, ranges.diff_min_step);
        const double coarse_atrf_step = coarse_step_dbl(ranges.atr_frac_min_min, ranges.atr_frac_min_max, ranges.atr_frac_min_step);

        auto stride_int = [&](std::size_t coarse_step, std::size_t base_step) -> std::size_t {
            base_step = std::max<std::size_t>(1, base_step);
            return std::max<std::size_t>(1, coarse_step / base_step);
            };
        auto stride_dbl = [&](double coarse_step, double base_step) -> std::size_t {
            base_step = std::max(1e-12, base_step);
            const double raw = coarse_step / base_step;
            return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(raw)));
            };

        const std::size_t f_stride = stride_int(coarse_fast_step, ranges.fast_step);
        const std::size_t s_stride = stride_int(coarse_slow_step, ranges.slow_step);
        const std::size_t a_stride = stride_int(coarse_atr_step, ranges.atr_n_step);
        const std::size_t d_stride = stride_dbl(coarse_diff_step, ranges.diff_min_step);
        const std::size_t af_stride = stride_dbl(coarse_atrf_step, ranges.atr_frac_min_step);

        std::vector<std::size_t> stage1_indices;
        stage1_indices.reserve((F / f_stride + 1) * (S / s_stride + 1));

        for (std::size_t fi = 0; fi < F; fi += f_stride) {
            for (std::size_t si = 0; si < S; si += s_stride) {
                for (std::size_t di = 0; di < D; di += d_stride) {
                    for (std::size_t ai = 0; ai < A; ai += a_stride) {
                        for (std::size_t afi = 0; afi < AF; afi += af_stride) {
                            for (std::size_t mi = 0; mi < M; ++mi) {
                                const auto p = GatedDiffCrossParams{ fast_vals_fine[fi], slow_vals_fine[si], diff_vals_fine[di], atr_vals_fine[ai], atrf_vals_fine[afi], modes[mi], ranges.use_ema };
                                if (!is_valid(p)) continue;
                                stage1_indices.push_back(pack(fi, si, di, ai, afi, mi));
                            }
                        }
                    }
                }
            }
        }

        g_last_gdc_diag.coarse_candidates = stage1_indices.size();

        // Neighbor definition: one "grid step" in each axis.
        // Stage-2 radius is controlled by opt.stage2_hops.
        auto neighbors = [&](std::size_t idx) -> std::vector<std::size_t> {
            std::size_t fi{}, si{}, di{}, ai{}, afi{}, mi{};
            unpack(idx, fi, si, di, ai, afi, mi);

            std::vector<std::size_t> out;
            out.reserve(1 + 2 * 5);
            out.push_back(idx); // include self

            auto push = [&](std::size_t nfi, std::size_t nsi, std::size_t ndi, std::size_t nai, std::size_t nafi, std::size_t nmi) {
                out.push_back(pack(nfi, nsi, ndi, nai, nafi, nmi));
                };

            if (fi > 0)       push(fi - 1, si, di, ai, afi, mi);
            if (fi + 1 < F)   push(fi + 1, si, di, ai, afi, mi);
            if (si > 0)       push(fi, si - 1, di, ai, afi, mi);
            if (si + 1 < S)   push(fi, si + 1, di, ai, afi, mi);
            if (di > 0)       push(fi, si, di - 1, ai, afi, mi);
            if (di + 1 < D)   push(fi, si, di + 1, ai, afi, mi);
            if (ai > 0)       push(fi, si, di, ai - 1, afi, mi);
            if (ai + 1 < A)   push(fi, si, di, ai + 1, afi, mi);
            if (afi > 0)      push(fi, si, di, ai, afi - 1, mi);
            if (afi + 1 < AF) push(fi, si, di, ai, afi + 1, mi);
            // mode dimension is small; keep it fixed (no neighbor) for now.

            return out;
            };

        const std::size_t k_stage1 = std::max<std::size_t>(ranges.top_k, 25);
        const std::size_t k_stage2 = std::max<std::size_t>(ranges.top_k, 50);

        // Construct a sweep object (framework v1).
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
        cfg.opt.suppression_hops = std::max<std::size_t>(1, static_cast<std::size_t>(std::max(0, ranges.fine_radius)));
        cfg.opt.stage2_hops = std::max<std::size_t>(1, static_cast<std::size_t>(std::max(0, ranges.fine_radius)));

        CoarseFineIndexSweep sweep(cfg);
        auto progress_cb = make_ostream_progress_cb(std::cout);
        const auto run = sweep.run(series, problem, score_fn, progress_cb);
        std::cout << "\n";

        g_last_gdc_diag.stage1_kept = run.has_two_stage ? run.two_stage.stage1_best_first.size() : 0;
        g_last_gdc_diag.fine_candidates = run.has_two_stage ? run.two_stage.stage2_best_first.size() : 0;

        // Final output: map indices -> params and dedupe by ResultSig so reported Top-K is actually diverse.
        auto result_key = [](const GatedDiffCrossParams&, const BacktestResult& r) -> ResultSig {
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

        TopK<GatedDiffCrossParams> out(ranges.top_k);
        std::unordered_set<ResultSig, ResultSigHash> seen_sig;
        seen_sig.reserve(ranges.top_k * 8);

        const auto& stage2 = run.has_two_stage ? run.two_stage.stage2_best_first : run.best_first;

        std::size_t kept = 0;
        for (const auto& c : stage2) {
            auto p = idx_to_params(c.index);
            const auto sig = result_key(p, c.result);
            if (!seen_sig.insert(sig).second) continue;

            out.consider(c.score, c.result, p);

            if (++kept >= ranges.top_k) break;
        }

        return out;
    }

    // ------------------------------------------------------------------------
    // Reporting + deterministic rerun
    // ------------------------------------------------------------------------
    bool run_gated_diff_cross_sweep_best(const CandleSeries& series,
        const GatedDiffCrossSweepRanges& ranges,
        TradeMode run_mode,
        const ExecSettings& exec,
        std::ostream& out)
    {
        // Apply runtime mode filter (this is the actual fix that makes menu selection matter).
        auto filtered = ranges;
        if (run_mode == TradeMode::LongOnly) {
            filtered.include_long_only = true;
            filtered.include_long_short = false;
        }
        else {
            filtered.include_long_only = false;
            filtered.include_long_short = true;
        }

        out << "\n[Gated Diff Cross Sweep]\n";
        out << "  fast: " << filtered.fast_min << " -> " << filtered.fast_max << " step " << filtered.fast_step << "\n";
        out << "  slow: " << filtered.slow_min << " -> " << filtered.slow_max << " step " << filtered.slow_step << "\n";
        out << "  diff_min: " << filtered.diff_min_min << " -> " << filtered.diff_min_max << " step " << filtered.diff_min_step << "\n";
        out << "  atr_n: " << filtered.atr_n_min << " -> " << filtered.atr_n_max << " step " << filtered.atr_n_step << "\n";
        out << "  atr_frac_min: " << filtered.atr_frac_min_min << " -> " << filtered.atr_frac_min_max << " step " << filtered.atr_frac_min_step << "\n";
        out << "  modes: "
            << (filtered.include_long_only ? "LongOnly " : "")
            << (filtered.include_long_short ? "LongShort" : "")
            << "\n";
        out << "  score: pnl - " << filtered.score_policy.dd_penalty << "*dd"
            << " (min_trades=" << filtered.score_policy.min_trades << ")\n";
        out << "  top_k: " << filtered.top_k << "\n";
        out << "  coarse_mesh_count: " << filtered.coarse_mesh_count << " (5 = default)\n";
        out << "  fine_radius: " << filtered.fine_radius << " (1=default)\n";

        auto topk = sweep_gated_diff_cross_topk(series, filtered, exec);
        auto best = topk.sorted_best_first();

        out << "\n  [diag] coarse_candidates=" << g_last_gdc_diag.coarse_candidates
            << " stage1_kept=" << g_last_gdc_diag.stage1_kept
            << " fine_candidates=" << g_last_gdc_diag.fine_candidates
            << "\n";

        if (best.empty()) {
            out << "\nNo sweep candidates produced a finite score.\n";
            return false;
        }

        out << "\nTop candidates:\n";
        const std::size_t show = std::min<std::size_t>(best.size(), filtered.top_k);
        for (std::size_t i = 0; i < show; ++i) {
            out << "\n  #" << (i + 1)
                << " score=" << best[i].score
                << " | " << gated_diff_cross_params_to_string(best[i].params) << "\n";
            print_backtest_summary(best[i].result, out, "    ");
        }

        auto winner = best.front().params;

        // Ensure the actual re-run respects the menu-selected mode.
        // (Winner is from the filtered sweep, so this should already match.)
        winner.mode = run_mode;

        GatedDiffCrossOptions opt{};
        opt.fast_n = winner.fast_n;
        opt.slow_n = winner.slow_n;
        opt.diff_min = winner.diff_min;
        opt.atr_n = winner.atr_n;
        opt.atr_frac_min = winner.atr_frac_min;
        opt.mode = winner.mode;
        opt.use_ema = winner.use_ema;
        opt.exec = exec;

        out << "\nBest Gated Diff Cross (re-run): " << gated_diff_cross_params_to_string(winner) << "\n";
        const auto final_res = run_gated_diff_cross(series, opt);
        print_backtest_summary(final_res, out, "  ");

        // Gate diagnostics (uses allow_trade signal only)
        {
            const auto plan = build_gated_diff_cross_plan(opt);
            const auto gs = compute_gate_stats(series, plan);
            out << "  [gate] allow_rate=" << gs.allow_rate_pct << "%"
                << "  entry_signals=" << gs.entry_signals
                << " blocked=" << gs.entries_blocked
                << "\n";
        }

        return true;
    }

} // namespace sugar
