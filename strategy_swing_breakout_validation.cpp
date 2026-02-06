#include "strategy_swing_breakout_validation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>
#include <cctype>   // std::tolower, std::isspace
#include <cstring>  // std::memcmp


#include "indicators_composite_numeric.h"
#include "indicators_composite_bool.h"
#include "strategy_executor.h"
#include "signals_precomputed.h"          // PrecomputedBoolSignal
#include "indicator_precomputed.h"        // your PrecomputedIndicator
#include "utils.h"
#include "sweep.h"

namespace sugar {

    namespace {
        inline double NaN() { return std::numeric_limits<double>::quiet_NaN(); }
        inline bool finite(double x) { return std::isfinite(x); }

        // Strict pivot-high confirmation at pivot bar p = i-rightBars (Pine-style delay).
        // Returns flag series aligned to bar i (flag true at the *confirmation bar* i).
        static std::vector<std::uint8_t> strict_pivothigh_confirm(const SeriesView& v, int leftBars, int rightBars) {
            const std::size_t n = v.size();
            std::vector<std::uint8_t> out(n, 0);

            if (leftBars < 1) leftBars = 1;
            if (rightBars < 1) rightBars = 1;
            if (n == 0) return out;

            auto H = v.high();
            if (H.empty()) return out;

            for (std::size_t i = 0; i < n; ++i) {
                if (i < static_cast<std::size_t>(rightBars)) continue;

                const std::size_t p = i - static_cast<std::size_t>(rightBars);
                if (p < static_cast<std::size_t>(leftBars)) continue;
                if (p + static_cast<std::size_t>(rightBars) >= n) continue;

                const double pivotHigh = H[p];
                bool ok = true;

                // left window (bars BEFORE pivot)
                for (std::size_t k = p - static_cast<std::size_t>(leftBars); k < p; ++k) {
                    if (!(H[k] < pivotHigh)) { ok = false; break; }
                }
                if (!ok) continue;

                // right window (bars AFTER pivot)
                for (std::size_t k = p + 1; k <= p + static_cast<std::size_t>(rightBars); ++k) {
                    if (!(H[k] < pivotHigh)) { ok = false; break; }
                }
                if (ok) out[i] = 1;
            }
            return out;
        }

        // Faithful Pine engine compiled into two series: entry and exit signals.
        // This is the "state wire factory".
        static void compute_pine_signals(
            const SeriesView& v,
            const SwingBreakoutValidationOptions& opt,
            BoolSeries& entry_long,
            BoolSeries& exit_long,
            std::size_t& first_valid_idx)
        {
            const std::size_t n = v.size();
            entry_long.assign(n, 0);
            exit_long.assign(n, 0);
            first_valid_idx = 0;

            if (n == 0) return;

            // Build OHLCV spans
            auto O = v.open();
            auto H = v.high();
            auto L = v.low();
            auto C = v.close();
            auto V = v.volume();
            if (C.empty()) return;

            // --- Compute EMA10 (Pine ta.ema(close,10)) ---
            // We'll reuse your numeric graph kernel here in-place to keep this function self-contained & faithful.
            std::vector<double> ema10(n, NaN());
            {
                const int N = 10;
                const double alpha = 2.0 / (N + 1.0);
                double prev = C[0];
                ema10[0] = prev;
                for (std::size_t i = 1; i < n; ++i) {
                    prev = alpha * C[i] + (1.0 - alpha) * prev;
                    ema10[i] = prev;
                }
            }

            // --- Volume SMA for filter ---
            std::vector<double> avgVol(n, NaN());
            if (!V.empty() && opt.volumeAvgPeriod > 0) {
                const int N = opt.volumeAvgPeriod;
                double sum = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    sum += V[i];
                    if (i >= static_cast<std::size_t>(N)) sum -= V[i - N];
                    if (i + 1 >= static_cast<std::size_t>(N)) avgVol[i] = sum / N;
                }
            }

            // --- Volume climax MA ---
            std::vector<double> volClimaxMA(n, NaN());
            if (!V.empty() && opt.volumeClimaxMAPeriod > 0) {
                const int N = opt.volumeClimaxMAPeriod;
                double sum = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    sum += V[i];
                    if (i >= static_cast<std::size_t>(N)) sum -= V[i - N];
                    if (i + 1 >= static_cast<std::size_t>(N)) volClimaxMA[i] = sum / N;
                }
            }

            // --- Strict swing highs (confirmation at bar i, pivot at i-rightBars) ---
            auto isStrictSwingHigh_conf = strict_pivothigh_confirm(v, opt.leftBars, opt.rightBars);

            // --- Pine state vars ---
            bool boFlagged = false;
            int trendState = 0; // 0=not in uptrend, 1=in uptrend (Pine)
            double lastSwingHigh = NaN();

            double breakoutLow = NaN();
            double breakoutPrice = NaN();
            std::size_t breakoutBar = 0;
            int daysAbove10 = 0;
            bool validationPassed = false;
            double entryPrice = NaN();

            double realBreakoutPrice = NaN();
            std::size_t lastExitBar = std::numeric_limits<std::size_t>::max();

            std::size_t climaxBlackoutUntil = 0;
            bool inVolumeBlackout = false;

            // first_valid_idx: we need enough history to confirm pivots and compute vol MAs.
            // Pivot confirmation delay costs rightBars; volume MA costs periods. We'll take the max conservative.
            std::size_t warm = 0;
            warm = std::max(warm, static_cast<std::size_t>(9));
            warm = std::max(warm, static_cast<std::size_t>(std::max(0, opt.rightBars)));
            warm = std::max(warm, static_cast<std::size_t>(std::max(0, opt.volumeAvgPeriod - 1)));
            warm = std::max(warm, static_cast<std::size_t>(std::max(0, opt.volumeClimaxMAPeriod - 1)));
            first_valid_idx = warm;

            for (std::size_t i = 0; i < n; ++i) {

                // Update lastSwingHigh when a strict swing high is confirmed at bar i
                if (isStrictSwingHigh_conf[i]) {
                    const std::size_t pivotBar = (i >= static_cast<std::size_t>(opt.rightBars))
                        ? (i - static_cast<std::size_t>(opt.rightBars))
                        : i;
                    lastSwingHigh = H[pivotBar];

                    // Pine: Reset boFlagged ONLY when not in uptrend
                    if (trendState != 1) boFlagged = false;
                }

                // --- Volume climax detection (context-aware, uses REAL breakout baseline) ---
                bool isVolumeClimax = false;
                if (opt.useVolumeClimax && trendState == 1 && finite(realBreakoutPrice)) {
                    const double pctGainFromRealBO = ((C[i] - realBreakoutPrice) / realBreakoutPrice) * 100.0;
                    const bool isExtended = pctGainFromRealBO >= opt.minGainForClimax;

                    if (isExtended && finite(volClimaxMA[i]) && !V.empty()) {
                        const bool currentVolSpike = V[i] > (opt.volumeClimaxThreshold * volClimaxMA[i]);
                        const bool reversalBar = (O.empty() || H.empty() || L.empty())
                            ? false
                            : ((O[i] - C[i]) > ((H[i] - L[i]) * 0.7));

                        if (currentVolSpike || reversalBar) {
                            isVolumeClimax = true;
                            climaxBlackoutUntil = i + static_cast<std::size_t>(std::max(0, opt.climaxBlackoutDays));
                        }
                    }
                }

                // Update blackout status
                inVolumeBlackout = (climaxBlackoutUntil > 0) && (i < climaxBlackoutUntil);

                // --- Swing failure flags (exit) ---
                bool isSwingFailure = false;

                // 8% stop loss
                if (trendState == 1 && finite(entryPrice)) {
                    const double currentLossPct = ((entryPrice - C[i]) / entryPrice) * 100.0;
                    if (currentLossPct >= opt.maxLossPct) {
                        isSwingFailure = true;
                    }
                }

                // Validation low violation ONLY
                if (!isSwingFailure && trendState == 1 && !validationPassed && finite(breakoutLow)) {
                    if (!L.empty() && (L[i] < breakoutLow)) {
                        isSwingFailure = true;
                    }
                }

                // Validation: days above EMA10
                if (trendState == 1 && !validationPassed) {
                    if (C[i] > ema10[i]) daysAbove10 += 1;
                    else daysAbove10 = 0;
                }

                if (trendState == 1 && !validationPassed && i >= breakoutBar) {
                    const int barsSinceBreakout = static_cast<int>(i - breakoutBar);
                    const double pctGain = finite(breakoutPrice)
                        ? ((C[i] - breakoutPrice) / breakoutPrice) * 100.0
                        : -1e9;

                    // If we didn't validate within the allowed window, invalidate the breakout.
                    if (!isSwingFailure && barsSinceBreakout > opt.daysForGain) {
                        isSwingFailure = true;
                    }

                    if (!isSwingFailure
                        && daysAbove10 >= opt.daysAbove10Day
                        && pctGain >= opt.pctGainThreshold
                        && barsSinceBreakout <= opt.daysForGain)
                    {
                        validationPassed = true;
                    }
                }

                // 10-day stop active in both phases
                if (!isSwingFailure && trendState == 1 && opt.use10DayStop) {
                    if (C[i] < ema10[i]) {
                        isSwingFailure = true;
                    }
                }

                // --- Breakout detection when NOT in uptrend ---
                bool isBreakout = false;

                const bool volumeOk =
                    (!opt.useVolumeFilter)
                    || (!V.empty() && finite(avgVol[i]) && V[i] > (avgVol[i] * opt.volumeMultiplier));

                if (finite(lastSwingHigh)
                    && !H.empty()
                    && (H[i] > lastSwingHigh)
                    && trendState != 1
                    && !boFlagged
                    && volumeOk
                    && !inVolumeBlackout)
                {
                    if (C[i] < lastSwingHigh) {
                        // "failed breakout" (intrabar poke)
                        isSwingFailure = true;
                        // trendState stays 0
                        boFlagged = true;
                    }
                    else {
                        isBreakout = true;

                        // Pine sets internal state immediately
                        trendState = 1;
                        boFlagged = true;

                        entryPrice = C[i];

                        // real breakout baseline reset logic
                        const std::size_t daysSinceExit =
                            (lastExitBar == std::numeric_limits<std::size_t>::max())
                            ? static_cast<std::size_t>(999)
                            : (i - lastExitBar);

                        if (!finite(realBreakoutPrice) || static_cast<int>(daysSinceExit) > opt.realBOResetDays) {
                            realBreakoutPrice = C[i];
                        }

                        breakoutLow = L.empty() ? NaN() : L[i];
                        breakoutPrice = C[i];
                        breakoutBar = i;
                        daysAbove10 = (C[i] > ema10[i]) ? 1 : 0;
                        validationPassed = false;
                    }
                }

                // --- Emit entry/exit signals (faithful to Pine order) ---
                // Pine issues volume climax close separately
                // Well map:
                // - entry_long true on breakout bar
                // - exit_long true on swing failure OR volume climax
                if (isBreakout) entry_long[i] = 1;

                if (isSwingFailure || (isVolumeClimax && trendState == 1)) {
                    exit_long[i] = 1;

                    // Pine resets on close
                    lastExitBar = i;
                    trendState = 0;
                    boFlagged = false;
                    breakoutLow = NaN();
                    breakoutPrice = NaN();
                    breakoutBar = 0;
                    daysAbove10 = 0;
                    validationPassed = false;
                    entryPrice = NaN();
                }
            }
        }

        // Could be usefull later helper keep for now
        /*
        // Helper: expose OHLCV as IndicatorPtr using your numeric PrecomputedIndicator
        static IndicatorPtr make_col(std::string name,
            std::function<std::span<const double>(const SeriesView&)> col)
        {
            return make_precomputed(std::move(name), [col](const SeriesView& v) {
                auto s = col(v);
                return std::vector<double>(s.begin(), s.end());
                });
        }
        */

    } // namespace

    StrategyPlan build_swing_breakout_validation_plan(const SwingBreakoutValidationOptions& opt) {
        // Faithful Pine behavior: process_orders_on_close=true
        // This should match: SameBar + SameBarClose (you can override externally).
        StrategyPlan plan{};
        plan.name = "SwingBreakoutValidation";
        plan.mode = TradeMode::LongOnly;

        // Build entry/exit as precomputed bool signals (state machine compiled)
        std::size_t first_valid = 0;

        auto entry = make_precomputed_bool("entry_long",
            [opt, &first_valid](const SeriesView& v) mutable {
                BoolSeries en, ex;
                compute_pine_signals(v, opt, en, ex, first_valid);
                return en;
            });

        auto exit = make_precomputed_bool("exit_long",
            [opt](const SeriesView& v) {
                BoolSeries en, ex;
                std::size_t fv = 0;
                compute_pine_signals(v, opt, en, ex, fv);
                return ex;
            });

        plan.signals.entry_long = std::move(entry);
        plan.signals.exit_long = std::move(exit);
        plan.first_valid_index = first_valid;

        return plan;
    }

    BacktestResult run_swing_breakout_validation(const CandleSeries& series,
        const SwingBreakoutValidationOptions& opt)
    {
        StrategyPlan plan = build_swing_breakout_validation_plan(opt);
        StrategyExecutor ex;
        return ex.run(series, plan, opt.exec);
    }

    // --- Sweep scaffolding (same pattern as ZScore; can be expanded) ---

    std::string swing_breakout_validation_params_to_string(const SwingBreakoutValidationParams& p) {
        std::ostringstream oss;
        oss << "L=" << p.leftBars
            << " R=" << p.rightBars
            << " EMAstop=" << (p.use10DayStop ? "1" : "0")
            << " daysAbove=" << p.daysAbove10Day
            << " gain%=" << p.pctGainThreshold
            << " gainDays=" << p.daysForGain
            << " maxLoss%=" << p.maxLossPct
            << " volF=" << (p.useVolumeFilter ? "1" : "0")
            << " volN=" << p.volumeAvgPeriod
            << " volMult=" << p.volumeMultiplier
            << " climax=" << (p.useVolumeClimax ? "1" : "0")
            << " cThr=" << p.volumeClimaxThreshold
            << " cMA=" << p.volumeClimaxMAPeriod
            << " blackout=" << p.climaxBlackoutDays
            << " minGainClx=" << p.minGainForClimax
            << " realReset=" << p.realBOResetDays;
        return oss.str();
    }

    void prompt_swing_breakout_validation_sweep_ranges(std::istream& in,
        std::ostream& out,
        SwingBreakoutValidationSweepRanges& r)
    {
        auto prompt_bool = [&](const char* label, bool& v) {
            out << label << " [current: " << (v ? "true" : "false") << "] (y/n or Enter): ";
            out.flush();

            std::string line;
            std::getline(in, line);

            // trim
            auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!line.empty() && is_ws((unsigned char)line.front())) line.erase(line.begin());
            while (!line.empty() && is_ws((unsigned char)line.back()))  line.pop_back();

            if (line.empty()) return;

            char c = (char)std::tolower((unsigned char)line[0]);
            if (c == 'y' || c == '1' || c == 't') v = true;
            else if (c == 'n' || c == '0' || c == 'f') v = false;
            else out << "  (invalid; keeping current)\n";
            };

        out << "\n--- Swing Breakout Validation sweep ranges ---\n";

        // Pivot
        prompt_range(in, out, "leftBars (min max step)", r.left_min, r.left_max, r.left_step);
        prompt_range(in, out, "rightBars (min max step)", r.right_min, r.right_max, r.right_step);

        // Validation
        prompt_bool("use10DayStop", r.use10DayStop);

        prompt_range(in, out, "daysAbove10Day (min max step)",
            r.daysAbove10_min, r.daysAbove10_max, r.daysAbove10_step);

        prompt_range(in, out, "pctGainThreshold (min max step)",
            r.pctGain_min, r.pctGain_max, r.pctGain_step);

        prompt_range(in, out, "daysForGain (min max step)",
            r.daysForGain_min, r.daysForGain_max, r.daysForGain_step);

        prompt_range(in, out, "maxLossPct (min max step)",
            r.maxLoss_min, r.maxLoss_max, r.maxLoss_step);

        // Volume filter
        prompt_bool("useVolumeFilter", r.useVolumeFilter);
        if (r.useVolumeFilter) {
            prompt_range(in, out, "volumeAvgPeriod (min max step)",
                r.volAvg_min, r.volAvg_max, r.volAvg_step);

            prompt_range(in, out, "volumeMultiplier (min max step)",
                r.volMult_min, r.volMult_max, r.volMult_step);
        }

        // Volume climax
        prompt_bool("useVolumeClimax", r.useVolumeClimax);
        if (r.useVolumeClimax) {
            prompt_range(in, out, "volumeClimaxThreshold (min max step)",
                r.climaxThresh_min, r.climaxThresh_max, r.climaxThresh_step);

            prompt_range(in, out, "volumeClimaxMAPeriod (min max step)",
                r.climaxMA_min, r.climaxMA_max, r.climaxMA_step);

            prompt_range(in, out, "climaxBlackoutDays (min max step)",
                r.blackout_min, r.blackout_max, r.blackout_step);

            prompt_range(in, out, "minGainForClimax (min max step)",
                r.minGainClimax_min, r.minGainClimax_max, r.minGainClimax_step);
        }

        // Real breakout reset
        prompt_range(in, out, "realBOResetDays (min max step)",
            r.realReset_min, r.realReset_max, r.realReset_step);

        // Search knobs
        prompt_scalar(in, out, "top_k", r.top_k);

        out << "Score policy: score = pnl - dd_penalty*dd\n";
        prompt_scalar(in, out, "dd_penalty", r.score_policy.dd_penalty);
        prompt_scalar(in, out, "min_trades", r.score_policy.min_trades);
    }

    bool run_swing_breakout_validation_sweep_best(const CandleSeries& series,
        const SwingBreakoutValidationSweepRanges& r,
        const ExecSettings& exec,
        std::ostream& out)
    {
        // Build a discrete grid of params (explicitly) so we can use sweep_topk_unique.
        std::vector<SwingBreakoutValidationParams> grid;
        grid.reserve(4096);

        auto irange = [](int a, int b, int step) {
            std::vector<int> v;
            step = std::max(1, step);
            if (b < a) std::swap(a, b);
            for (int x = a; x <= b; x += step) v.push_back(x);
            return v;
            };

        auto drange = [](double a, double b, double step) {
            std::vector<double> v;
            step = std::max(step, 1e-12);
            if (b < a) std::swap(a, b);
            for (double x = a; x <= b + 1e-9; x += step) v.push_back(x);
            return v;
            };

        const auto lefts = irange(r.left_min, r.left_max, r.left_step);
        const auto rights = irange(r.right_min, r.right_max, r.right_step);

        const auto daysAbove = irange(r.daysAbove10_min, r.daysAbove10_max, r.daysAbove10_step);
        const auto gainPct = drange(r.pctGain_min, r.pctGain_max, r.pctGain_step);
        const auto gainDays = irange(r.daysForGain_min, r.daysForGain_max, r.daysForGain_step);
        const auto maxLoss = drange(r.maxLoss_min, r.maxLoss_max, r.maxLoss_step);
        const auto realReset = irange(r.realReset_min, r.realReset_max, r.realReset_step);

        // Expose "static knobs": if a feature is OFF, still iterate a single dummy value.
        const auto volAvg = r.useVolumeFilter ? irange(r.volAvg_min, r.volAvg_max, r.volAvg_step)
            : std::vector<int>{ r.volAvg_min };
        const auto volMult = r.useVolumeFilter ? drange(r.volMult_min, r.volMult_max, r.volMult_step)
            : std::vector<double>{ r.volMult_min };

        const auto cThr = r.useVolumeClimax ? drange(r.climaxThresh_min, r.climaxThresh_max, r.climaxThresh_step)
            : std::vector<double>{ r.climaxThresh_min };
        const auto cMA = r.useVolumeClimax ? irange(r.climaxMA_min, r.climaxMA_max, r.climaxMA_step)
            : std::vector<int>{ r.climaxMA_min };
        const auto blackout = r.useVolumeClimax ? irange(r.blackout_min, r.blackout_max, r.blackout_step)
            : std::vector<int>{ r.blackout_min };
        const auto minGainC = r.useVolumeClimax ? drange(r.minGainClimax_min, r.minGainClimax_max, r.minGainClimax_step)
            : std::vector<double>{ r.minGainClimax_min };

        // Build grid
        for (int L : lefts)
            for (int R : rights)
                for (int dA : daysAbove)
                    for (double g : gainPct)
                        for (int dg : gainDays)
                            for (double ml : maxLoss)
                                for (int rr : realReset)
                                    for (int vN : volAvg)
                                        for (double vM : volMult)
                                            for (double thr : cThr)
                                                for (int cN : cMA)
                                                    for (int bo : blackout)
                                                        for (double mg : minGainC)
                                                        {
                                                            SwingBreakoutValidationParams p{};
                                                            p.leftBars = L;
                                                            p.rightBars = R;

                                                            p.use10DayStop = r.use10DayStop;
                                                            p.daysAbove10Day = dA;
                                                            p.pctGainThreshold = g;
                                                            p.daysForGain = dg;
                                                            p.maxLossPct = ml;

                                                            p.useVolumeFilter = r.useVolumeFilter;
                                                            p.volumeAvgPeriod = vN;
                                                            p.volumeMultiplier = vM;

                                                            p.useVolumeClimax = r.useVolumeClimax;
                                                            p.volumeClimaxThreshold = thr;
                                                            p.volumeClimaxMAPeriod = cN;
                                                            p.climaxBlackoutDays = bo;
                                                            p.minGainForClimax = mg;

                                                            p.realBOResetDays = rr;

                                                            grid.push_back(p);
                                                        }

        // Eval + score
        auto eval = [&](const CandleSeries& data, const SwingBreakoutValidationParams& p) {
            SwingBreakoutValidationOptions opt{};
            opt.leftBars = p.leftBars;
            opt.rightBars = p.rightBars;

            opt.use10DayStop = p.use10DayStop;
            opt.daysAbove10Day = p.daysAbove10Day;
            opt.pctGainThreshold = p.pctGainThreshold;
            opt.daysForGain = p.daysForGain;
            opt.maxLossPct = p.maxLossPct;

            opt.useVolumeFilter = p.useVolumeFilter;
            opt.volumeAvgPeriod = p.volumeAvgPeriod;
            opt.volumeMultiplier = p.volumeMultiplier;

            opt.useVolumeClimax = p.useVolumeClimax;
            opt.volumeClimaxThreshold = p.volumeClimaxThreshold;
            opt.volumeClimaxMAPeriod = p.volumeClimaxMAPeriod;
            opt.climaxBlackoutDays = p.climaxBlackoutDays;
            opt.minGainForClimax = p.minGainForClimax;

            opt.realBOResetDays = p.realBOResetDays;

            opt.mode = TradeMode::LongOnly;
            opt.exec = exec;

            return run_swing_breakout_validation(data, opt);
            };

        auto score = [&](const BacktestResult& res) {
            return default_sweep_score(res, r.score_policy);
            };

        // -----------------------------
        // ---- Uniqueness behavior ----
        // -----------------------------

        using Key = std::uint64_t;

        auto key_fn = [&](const SwingBreakoutValidationParams&, const BacktestResult& res) -> Key {
            // IMPORTANT: ensure trade_hash is set in StrategyExecutor/BacktestResult.
            // 0 means "no trades" if you followed the earlier recommendation.
            // That will collapse all no-trade candidates into one (usually desired).
            return res.trade_hash;
            };

        struct BestByHash {
            double score = -std::numeric_limits<double>::infinity();
            BacktestResult result{};
            SwingBreakoutValidationParams params{};
            bool has = false;
        };

        // -----------------------------
        // Basin-aware Top-K (Option B)
        //   - Basin key = bucketed PARAMS (stable region), not trade_hash
        //   - Keep up to MAX_PER_BASIN candidates per basin
        //   - Enforce MIN_PARAM_DISTANCE inside each basin
        //   - Keep GLOBAL_BEST as insurance
        // -----------------------------

        constexpr std::size_t MAX_PER_BASIN = 3;
        constexpr std::size_t MIN_PARAM_DISTANCE = 4;

        auto quant_i = [](int v, int vmin, int step) -> int {
            step = std::max(1, step);
            return (v - vmin) / step;
            };
        auto quant_d = [](double v, double vmin, double step) -> int {
            step = std::max(step, 1e-12);
            return static_cast<int>(std::llround((v - vmin) / step));
            };

        // Stable basin key (bucketed by sweep step indices)
        struct BasinKey {
            int L, R;
            int dA;
            int g;
            int dg;
            int ml;
            int volOn;
            int volN;
            int volMult;
            int clxOn;
            int clxThr;
            int clxMA;
            int blackout;
            int minGainClx;
            int realReset;

            bool operator==(const BasinKey& o) const {
                return std::memcmp(this, &o, sizeof(BasinKey)) == 0;
            }
        };

        struct BasinKeyHash {
            std::size_t operator()(const BasinKey& k) const noexcept {
                // simple 64-bit mix
                auto hmix = [](std::size_t h, std::size_t x) {
                    return h ^ (x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
                    };
                std::size_t h = 0;
                const int* p = reinterpret_cast<const int*>(&k);
                for (std::size_t i = 0; i < sizeof(BasinKey) / sizeof(int); ++i) {
                    h = hmix(h, static_cast<std::size_t>(p[i]));
                }
                return h;
            }
        };

        // Parameter distance in step units (cheap and effective)
        auto param_distance = [&](const SwingBreakoutValidationParams& a,
            const SwingBreakoutValidationParams& b) -> std::size_t {
                std::size_t d = 0;
                d += (std::size_t)std::llabs((long long)a.leftBars - (long long)b.leftBars);
                d += (std::size_t)std::llabs((long long)a.rightBars - (long long)b.rightBars);
                d += (std::size_t)std::llabs((long long)a.daysAbove10Day - (long long)b.daysAbove10Day);
                d += (std::size_t)std::llabs((long long)a.daysForGain - (long long)b.daysForGain);
                d += (std::size_t)std::llabs((long long)a.realBOResetDays - (long long)b.realBOResetDays);

                // doubles as step indices
                d += (std::size_t)std::llabs((long long)quant_d(a.pctGainThreshold, r.pctGain_min, r.pctGain_step) -
                    (long long)quant_d(b.pctGainThreshold, r.pctGain_min, r.pctGain_step));
                d += (std::size_t)std::llabs((long long)quant_d(a.maxLossPct, r.maxLoss_min, r.maxLoss_step) -
                    (long long)quant_d(b.maxLossPct, r.maxLoss_min, r.maxLoss_step));

                if (a.useVolumeFilter || b.useVolumeFilter) {
                    d += (std::size_t)std::llabs((long long)quant_i(a.volumeAvgPeriod, r.volAvg_min, r.volAvg_step) -
                        (long long)quant_i(b.volumeAvgPeriod, r.volAvg_min, r.volAvg_step));
                    d += (std::size_t)std::llabs((long long)quant_d(a.volumeMultiplier, r.volMult_min, r.volMult_step) -
                        (long long)quant_d(b.volumeMultiplier, r.volMult_min, r.volMult_step));
                }
                if (a.useVolumeClimax || b.useVolumeClimax) {
                    d += (std::size_t)std::llabs((long long)quant_d(a.volumeClimaxThreshold, r.climaxThresh_min, r.climaxThresh_step) -
                        (long long)quant_d(b.volumeClimaxThreshold, r.climaxThresh_min, r.climaxThresh_step));
                    d += (std::size_t)std::llabs((long long)quant_i(a.volumeClimaxMAPeriod, r.climaxMA_min, r.climaxMA_step) -
                        (long long)quant_i(b.volumeClimaxMAPeriod, r.climaxMA_min, r.climaxMA_step));
                    d += (std::size_t)std::llabs((long long)quant_i(a.climaxBlackoutDays, r.blackout_min, r.blackout_step) -
                        (long long)quant_i(b.climaxBlackoutDays, r.blackout_min, r.blackout_step));
                    d += (std::size_t)std::llabs((long long)quant_d(a.minGainForClimax, r.minGainClimax_min, r.minGainClimax_step) -
                        (long long)quant_d(b.minGainForClimax, r.minGainClimax_min, r.minGainClimax_step));
                }
                return d;
            };

        auto basin_key = [&](const SwingBreakoutValidationParams& p, const BacktestResult&) -> BasinKey {
            BasinKey k{};
            k.L = quant_i(p.leftBars, r.left_min, r.left_step);
            k.R = quant_i(p.rightBars, r.right_min, r.right_step);
            k.dA = quant_i(p.daysAbove10Day, r.daysAbove10_min, r.daysAbove10_step);
            k.g = quant_d(p.pctGainThreshold, r.pctGain_min, r.pctGain_step);
            k.dg = quant_i(p.daysForGain, r.daysForGain_min, r.daysForGain_step);
            k.ml = quant_d(p.maxLossPct, r.maxLoss_min, r.maxLoss_step);

            k.volOn = p.useVolumeFilter ? 1 : 0;
            k.volN = quant_i(p.volumeAvgPeriod, r.volAvg_min, r.volAvg_step);
            k.volMult = quant_d(p.volumeMultiplier, r.volMult_min, r.volMult_step);

            k.clxOn = p.useVolumeClimax ? 1 : 0;
            k.clxThr = quant_d(p.volumeClimaxThreshold, r.climaxThresh_min, r.climaxThresh_step);
            k.clxMA = quant_i(p.volumeClimaxMAPeriod, r.climaxMA_min, r.climaxMA_step);
            k.blackout = quant_i(p.climaxBlackoutDays, r.blackout_min, r.blackout_step);
            k.minGainClx = quant_d(p.minGainForClimax, r.minGainClimax_min, r.minGainClimax_step);

            k.realReset = quant_i(p.realBOResetDays, r.realReset_min, r.realReset_step);
            return k;
            };

        struct Cand {
            double score = -std::numeric_limits<double>::infinity();
            BacktestResult result{};
            SwingBreakoutValidationParams params{};
        };

        struct Basin {
            std::size_t first_seen = 0;
            std::vector<Cand> best; // sorted best-first
        };

        auto progress = make_ostream_progress_cb(out, std::chrono::milliseconds(250), "  [swing sweep] ");

        BasinPolicy pol{};
        pol.max_per_basin = MAX_PER_BASIN;
        pol.min_param_distance = MIN_PARAM_DISTANCE;
        pol.keep_global_best = true;
        pol.dedupe_trade_hash = true;

        std::size_t basin_count = 0;
        auto top = sweep_topk_basins<SwingBreakoutValidationParams, BasinKey>(
            series,
            grid,
            eval,
            score,
            r.top_k,
            basin_key,
            param_distance,
            pol,
            progress,
            BasinKeyHash{},
            std::equal_to<BasinKey>{},
            &basin_count);

        out << "\n";
        out << "Basins: " << basin_count << "\n";

        const auto best = top.sorted_best_first();
        if (best.empty()) {
            out << "No candidates.\n";
            return false;
        }

        out << "Top candidates:\n";
        for (std::size_t j = 0; j < best.size(); ++j) {
            out << "\n  #" << (j + 1) << " score=" << best[j].score
                << " | " << swing_breakout_validation_params_to_string(best[j].params) << "\n";
            print_backtest_summary(best[j].result, out, "    ");
        }

        return true;
    }


} // namespace sugar