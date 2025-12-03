#include <iostream>
#include <iomanip>
#include <vector>
#include <tuple>
#include <cmath>
#include <chrono>
#include <limits>

#include "csv.h"
#include "utils.h"
#include "series.h"
#include "backtester.h"
#include "sweep.h"
#include "strategy_diff_cross.h"
#include "strategy_swing_breakout.h"
#include "indicators_sma.h"
#include "indicators_composite.h"

// Inclusive integer range: [start, stop] step>0
static std::vector<std::size_t> make_range(std::size_t start, std::size_t stop, std::size_t step) {
    std::vector<std::size_t> v;
    if (step == 0 || start > stop) return v;
    v.reserve((stop - start) / step + 1);
    for (std::size_t x = start; x <= stop; x += step) v.push_back(x);
    return v;
}

// Inclusive double range with tiny epsilon to avoid FP miss on the last point
static std::vector<double> make_drange(double start, double stop, double step) {
    std::vector<double> v;
    if (step <= 0 || start > stop) return v;
    for (double x = start; x <= stop + 1e-12; x += step) v.push_back(x);
    return v;
}

static void run_swing_breakout(const sugar::CandleSeries& candles) {
    using sugar::SwingBreakoutStrategy;
    using sugar::Backtester;

    // Core parameters — tune manually for now
    const std::size_t leftBars = 2;
    const std::size_t rightBars = 2;
    const bool        useEmaStop = true;
    const int         daysAbove10 = 2;
    const double      pctGainThreshold = 4.0;
    const int         daysForGain = 3;
    const double      maxLossPct = 8.0;

    SwingBreakoutStrategy strat{
        leftBars, rightBars, useEmaStop,
        daysAbove10, pctGainThreshold, daysForGain, maxLossPct
    };

    Backtester bt;
    auto result = bt.run(strat, candles);

    std::cout << "Swing Breakout Strategy\n"
        << "-----------------------\n"
        << "PnL: " << result.pnl << "%\n"
        << "Trades: " << result.trades << "\n"
        << "Max DD: " << result.max_drawdown << "%\n\n";
}

int main(int argc, char** argv) {
    try {
        // ---------------------------------------------------------------------
        // CSV path selection
        // ---------------------------------------------------------------------
        const std::string path = (argc >= 2)
            ? argv[1]
            : "C:/Dev/sugar_Bot/data/BTCUSD_420.csv";

         // NOTE: Example of a release-build CSV override. Commented out so the
         // current behavior stays the same. If you enable this, make `path`
         // non-const above and move the prompt inside #ifndef _DEBUG if desired.
        
         #ifndef _DEBUG
         std::cout << "Enter CSV path (or leave blank for default '"
                   << path << "'):\n";
         std::string user_path;
         std::getline(std::cin, user_path);
         if (!user_path.empty()) {
             path = user_path; // (remove const from 'path' to allow this)
         }
         #endif

        auto candles = sugar::load_candles_csv(path);
        sugar::CandleSeries series{ std::move(candles) };

        // Print tail to verify parse
        std::cout << std::fixed << std::setprecision(2);
        const auto& rows = series.rows();
        std::size_t count = std::min<std::size_t>(rows.size(), 20);
        std::cout << "Print data tail to verify csv loaded to user:\n";
        for (std::size_t i = rows.size() - count; i < rows.size(); ++i) {
            const auto& c = rows[i];
            char buf[9];
            sugar::format_yyyymmdd(c.date, buf);
            std::cout << std::string_view(buf, 8)
                << ", " << c.open
                << ", " << c.high
                << ", " << c.low
                << ", " << c.close
                << ", vol=" << c.volume << "\n";
        }
        std::cout << "Loaded " << rows.size() << " candle(s) from '" << path << "'\n";

        // Optional breakout demo:
        // run_swing_breakout(series);

        // ---------------------------------------------------------------------
        // Sweep example for ROC(SMA) diff cross
        // ---------------------------------------------------------------------

        // USER-CUSTOMIZABLE SWEEP PARAMETERS (old behavior, now commented)
        // -----------------------------------------------------------------
        // If you want to let users configure the sweep ranges at runtime,
        // you can replace the hard-coded ranges below with something like:
        //
        // std::size_t fast_min, fast_max, fast_step;
        // std::size_t slow_min, slow_max, slow_step;
        // std::size_t roc_min,  roc_max,  roc_step;
        // double      thr_min,  thr_max,  thr_step;
        //
        // std::cout << "Fast SMA (min max step): ";
        // std::cin  >> fast_min >> fast_max >> fast_step;
        //
        // std::cout << "Slow SMA (min max step): ";
        // std::cin  >> slow_min >> slow_max >> slow_step;
        //
        // std::cout << "ROC length (min max step): ";
        // std::cin  >> roc_min >> roc_max >> roc_step;
        //
        // std::cout << "Threshold % (min max step): ";
        // std::cin  >> thr_min >> thr_max >> thr_step;
        //
        // auto fasts      = make_range(fast_min, fast_max, fast_step);
        // auto slows      = make_range(slow_min, slow_max, slow_step);
        // auto rocs       = make_range(roc_min,  roc_max,  roc_step);
        // auto thresholds = make_drange(thr_min, thr_max, thr_step);
        //
        // (Remember to handle invalid input / empty ranges in production.)

        // Current default sweep ranges (as before)
        auto fasts = make_range(45, 55, 1);
        auto slows = make_range(55, 65, 1);
        auto rocs = make_range(95, 105, 1);
        auto thresholds = make_drange(0.10, 0.20, 0.01);

        // naive count (upper bound)
        const std::size_t naive =
            fasts.size() * slows.size() * rocs.size() * thresholds.size();

        // effective fast/slow pairs where fast < slow
        std::size_t pairs = 0;
        for (auto s : slows)
            for (auto f : fasts)
                if (f < s) ++pairs;

        const std::size_t effective = pairs * rocs.size() * thresholds.size();
        std::cerr << "[Info] Grid size: naive=" << naive
            << " combos, effective=" << effective << " combos\n";

        const std::size_t combos = naive;
        if (combos > 1000000) {
            std::cerr << "[Note] Large grid (" << combos
                << " combos). Consider narrowing ranges or steps.\n"
                << "Continue anyway? y/n \n";
            char a = 'n';
            std::cin >> a;
            if (a != 'y') {
                std::cerr << "Exiting program with no calculation.\n";
                return 0;
            }
        }

        // Decide mode once (runtime toggle)
        using Mode = sugar::DiffCrossStrategy::Mode;
        Mode mode = Mode::LongShort;   // or Mode::LongOnly

        auto t0 = std::chrono::high_resolution_clock::now();
        auto best = sugar::sweep_roc_sma(series, fasts, slows, rocs, thresholds, mode);
        auto t1 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dt = t1 - t0;

        auto& [bf, bs, br, btval] = best.params;

        std::cout << "\nSweep took " << dt.count() << "s across "
            << effective << " combos.\n";

        std::cout << "\nBest sweep:\n";
        std::cout << " fast=" << bf << " slow=" << bs
            << " roc=" << br << " thresh=" << btval << "%\n";
        std::cout << " PnL: " << best.best.pnl << "%, Trades: "
            << best.best.trades
            << ", Max DD: " << best.best.max_drawdown << "%\n";

        if (best.best.best_start_date > 0) {
            char b[9];
            sugar::format_yyyymmdd(best.best.best_start_date, b);
            std::cout << " First usable signal date: "
                << std::string_view(b, 8) << "\n";
        }

        // -----------------------------------------------------------------
        // Run OOP strategy via Backtester using best sweep params
        // -----------------------------------------------------------------

        // Use the best parameters from the sweep
        std::size_t fast_period = bf;
        std::size_t slow_period = bs;
        std::size_t roc_len = br;
        double      thresh = btval;

        // Build indicator pipeline: ROC(SMA_fast) vs ROC(SMA_slow)
        auto fast_sma = std::make_shared<sugar::SMAIndicator>(fast_period);
        auto slow_sma = std::make_shared<sugar::SMAIndicator>(slow_period);

        auto fast_roc = sugar::map_roc(fast_sma, roc_len);
        auto slow_roc = sugar::map_roc(slow_sma, roc_len);

        // Strategy in the same mode as the sweep
        sugar::DiffCrossStrategy strat{ fast_roc, slow_roc, thresh, mode };

        sugar::Backtester bt;
        auto res = bt.run(strat, series);

        std::cout << "\nStrategy ROC(SMA) diff-cross results:\n";
        sugar::print_backtest_summary(res, std::cout, " ");

        if (res.best_start_date > 0) {
            char buf[9];
            sugar::format_yyyymmdd(res.best_start_date, buf);
            std::cout << " Pretty start date: "
                << std::string_view(buf, 8) << "\n";
        }

        // Pause for user to read data (nice for double-clicked .exe)
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
