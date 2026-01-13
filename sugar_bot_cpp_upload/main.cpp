#include <iostream>
#include <iomanip>
#include <vector>
#include <tuple>
#include <cmath>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <sstream>
#include <algorithm>
#include <filesystem>

#include "indicators_atr.h"
#include "indicators_rsi.h"
#include "indicators_stddev.h"
#include "indicators_zscore.h"
#include "csv.h"
#include "utils.h"
#include "series.h"
#include "backtester.h"
#include "sweep.h"
#include "strategy_diff_cross.h"
#include "indicators_sma.h"
#include "indicators_composite.h"


// Helper: parse "min max step" from a line, with type T (size_t or double)
template <typename T>
bool parse_triple_line(const std::string& line, T& a, T& b, T& c) {
    std::istringstream iss(line);
    if (!(iss >> a >> b >> c)) {
        return false;
    }
    return true;
}

// Alias for std::filesystems
namespace fs = std::filesystem;

// Convert Windows path (C:\Dev\X or C:/Dev/X) to WSL path (/mnt/c/Dev/X) when running on Linux.
// On Windows builds, it returns the input unchanged.
static inline std::string normalize_path_for_runtime(std::string in) {
    in = sugar::strip_wrapping_quotes(std::move(in));

#ifdef __linux__
    // Backslashes -> slashes
    std::replace(in.begin(), in.end(), '\\', '/');

    // Handle drive letter paths: C:/... -> /mnt/c/...
    if (in.size() >= 3 && std::isalpha(static_cast<unsigned char>(in[0])) && in[1] == ':' && in[2] == '/') {
        char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(in[0])));
        std::string rest = in.substr(3); // skip "C:/"
        return "/mnt/" + std::string(1, drive) + "/" + rest;
    }
#endif

    return in;
}


// Pick a default CSV path that works on both Windows and WSL.
// Prefer: ./data/BTCUSD_420.csv relative to current working directory.
// Fallback: hard-coded WSL mount or Windows path depending on platform.
static inline fs::path default_csv_path() {
    fs::path rel = fs::path("data") / "BTCUSD_420.csv";
    if (fs::exists(rel)) return rel;

#ifdef __linux__
    return fs::path("/mnt/c/Dev/sugar_Bot/data/BTCUSD_420.csv");
#else
    return fs::path("C:/Dev/sugar_Bot/data/BTCUSD_420.csv");
#endif
}



int main(int argc, char** argv) {
    try {
        // ---------------------------------------------------------------------
        // 0) Build-mode dependent setup (paths + ranges)
        // ---------------------------------------------------------------------
        const std::string default_diff_path = default_csv_path().string();
        const std::string default_swing_path = default_diff_path;


        std::string diff_path;
        std::string swing_path;

        // MODE toggle: choose once, use for both sweep + final run
        using Mode = sugar::DiffCrossStrategy::Mode;
        Mode mode = Mode::LongShort;   // Change to LongOnly if you want

        // ---- File paths -----------------------------------------------------
#ifdef _DEBUG
        // Debug: hard-coded / argv-based
        diff_path = (argc >= 2) ? argv[1] : default_diff_path;
        diff_path = normalize_path_for_runtime(diff_path);
        // swing_path = diff_path; // use the same file for breakout by default
#else
        // Release: Ask user for both paths (with safe defaults)
        diff_path = (argc >= 2) ? argv[1] : default_diff_path;
        swing_path = default_swing_path;

        std::cout << "CSV file path [default: " << diff_path << "]: \n" 
            << "Enter new path to change default -> ";
        {
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                diff_path = normalize_path_for_runtime(line);
            }
        }
#endif

#ifdef __linux__
        if (diff_path.size() >= 2 && std::isalpha(diff_path[0]) && diff_path[1] == ':') {
            std::cerr << "[Hint] You entered a Windows path. "
                << "Under WSL, use /mnt/<drive>/... or let the program convert it.\n";
        }
#endif

        // ---- Load data ------------------------------------------------------
        auto diff_candles = sugar::load_candles_csv(diff_path);
        sugar::CandleSeries diff_series{ std::move(diff_candles) };

        // auto swing_candles = sugar::load_candles_csv(swing_path);
        // sugar::CandleSeries swing_series{ std::move(swing_candles) };

        const auto& rows = diff_series.rows();
        std::cout << std::fixed << std::setprecision(2);

        std::cout << "CSV Preview:\n";

        if (rows.empty()) {
            std::cout << "  [Error] CSV contains no candles!\n";
        }
        else if (rows.size() == 1) {
            // Only 1 candle → print just the last (first == last)
            const auto& c = rows[0];
            char buf[9];
            sugar::format_yyyymmdd(c.date, buf);

            std::cout << "  Only candle:\n";
            std::cout << "   " << std::string_view(buf, 8)
                << ", " << c.open
                << ", " << c.high
                << ", " << c.low
                << ", " << c.close
                << ", vol=" << c.volume << "\n";
        }
        else {
            // At least 2 candles → print first AND last
            const auto& first = rows.front();
            const auto& last = rows.back();

            char buf1[9], buf2[9];
            sugar::format_yyyymmdd(first.date, buf1);
            sugar::format_yyyymmdd(last.date, buf2);

            std::cout << "  First candle:\n";
            std::cout << "   " << std::string_view(buf1, 8)
                << ", " << first.open
                << ", " << first.high
                << ", " << first.low
                << ", " << first.close
                << ", vol=" << first.volume << "\n";

            std::cout << "  Last candle:\n";
            std::cout << "   " << std::string_view(buf2, 8)
                << ", " << last.open
                << ", " << last.high
                << ", " << last.low
                << ", " << last.close
                << ", vol=" << last.volume << "\n";
        }

        std::cout << "Loaded " << rows.size() << " candle(s) from '"
            << diff_path << "'\n\n";


#ifdef _DEBUG

        // -----------------------------------------------------------------------------
        // Optional: indicator sanity test using the existing CandleSeries pipeline
        // Run with: sugar_Bot --test-indicators
        // -----------------------------------------------------------------------------
        bool run_indicator_test = true;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--test-indicators") {
                run_indicator_test = true;
                break;
            }
        }

        if (run_indicator_test) {
            const std::size_t n = 14; // change if desired

            // --- Helpers --------------------------------------------------------------
            auto first_finite_index = [](const std::vector<double>& v) -> std::size_t {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (std::isfinite(v[i])) return i;
                }
                return v.size();
                };

            auto pv = [](double x) -> std::string {
                if (!std::isfinite(x)) return "NaN";
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6) << x;
                return oss.str();
                };

            // --- Indicators -----------------------------------------------------------
            sugar::StdDevIndicator    sd_sma(n);
            sugar::StdDevEMAIndicator sd_ema(n);
            sugar::ZScoreSMAIndicator z_sma(n);
            sugar::ZScoreEMAIndicator z_ema(n);

            sugar::ATRIndicator atr(n);
            sugar::RSIIndicator rsi(n);

            // --- Compute --------------------------------------------------------------
            const auto v_sd_sma = sd_sma.compute(diff_series);
            const auto v_sd_ema = sd_ema.compute(diff_series);
            const auto v_z_sma = z_sma.compute(diff_series);
            const auto v_z_ema = z_ema.compute(diff_series);

            const auto v_atr = atr.compute(diff_series);
            const auto v_rsi = rsi.compute(diff_series);

            // --- Expected / Observed warm-up -----------------------------------------
            const std::size_t exp_sd_z_sma = (n > 0) ? (n - 1) : 0;        // StdDev(SMA), ZScore(SMA)
            const std::size_t exp_sd_z_ema = (2 * n >= 2) ? (2 * n - 2) : 0; // StdDev(EMA), ZScore(EMA)
            const std::size_t exp_atr = (n > 0) ? (n - 1) : 0;        // ATR contract
            const std::size_t exp_rsi = n;                            // RSI contract (per your header)

            const std::size_t obs_atr = first_finite_index(v_atr);
            const std::size_t obs_rsi = first_finite_index(v_rsi);

            std::cout << "\n[Indicator Test] period n=" << n << "\n";
            std::cout << "Expected first finite:\n";
            std::cout << "  StdDev(SMA) & ZScore(SMA): index " << exp_sd_z_sma << "\n";
            std::cout << "  StdDev(EMA) & ZScore(EMA): index " << exp_sd_z_ema << "\n";
            std::cout << "  ATR(" << n << "): index " << exp_atr << "  (header contract)\n";
            std::cout << "  RSI(" << n << "): index " << exp_rsi << "  (header contract)\n";

            std::cout << "Observed first finite:\n";
            if (obs_atr < v_atr.size()) std::cout << "  ATR: index " << obs_atr << "\n";
            else                        std::cout << "  ATR: never finite (all NaN?)\n";

            if (obs_rsi < v_rsi.size()) std::cout << "  RSI: index " << obs_rsi << "\n";
            else                        std::cout << "  RSI: never finite (all NaN?)\n";

            // --- Row printer ----------------------------------------------------------
            auto print_row = [&](std::size_t i) {
                if (i >= rows.size()) return;

                char buf[9];
                sugar::format_yyyymmdd(rows[i].date, buf);

                std::cout
                    << std::setw(5) << i << "  "
                    << std::string_view(buf, 8)
                    << "  close=" << std::fixed << std::setprecision(2) << rows[i].close
                    << "  sd_sma=" << pv(v_sd_sma[i])
                    << "  sd_ema=" << pv(v_sd_ema[i])
                    << "  z_sma=" << pv(v_z_sma[i])
                    << "  z_ema=" << pv(v_z_ema[i])
                    << "  atr=" << pv(v_atr[i])
                    << "  rsi=" << pv(v_rsi[i])
                    << "\n";
                };

            // --- Print windows --------------------------------------------------------
            const std::size_t i1 = exp_sd_z_sma;
            const std::size_t i2 = exp_sd_z_ema;

            std::cout << "\n---- Head (first 12 rows) ----\n";
            for (std::size_t i = 0; i < rows.size() && i < 12; ++i) print_row(i);

            std::cout << "\n---- Around n-1 (SMA/ATR first defined) ----\n";
            for (std::size_t i = (i1 > 3 ? i1 - 3 : 0); i < rows.size() && i <= i1 + 3; ++i) print_row(i);

            std::cout << "\n---- Around 2n-2 (EMA stddev/z first defined) ----\n";
            for (std::size_t i = (i2 > 3 ? i2 - 3 : 0); i < rows.size() && i <= i2 + 3; ++i) print_row(i);

            std::cout << "\n---- Around RSI first defined (index n) ----\n";
            for (std::size_t i = (exp_rsi > 3 ? exp_rsi - 3 : 0); i < rows.size() && i <= exp_rsi + 3; ++i) print_row(i);

            std::cout << "\n[Indicator Test] Done.\n\n";
        }
#endif

        // ---------------------------------------------------------------------
        // 1) Sweep for ROC(SMA) diff-cross
        // ---------------------------------------------------------------------
        std::vector<std::size_t> fasts;
        std::vector<std::size_t> slows;
        std::vector<std::size_t> rocs;
        std::vector<double>      thresholds;

#ifdef _DEBUG
        // Debug: fixed ranges (fast dev loop)
        fasts = sugar::make_range(45, 55, 1);
        slows = sugar::make_range(55, 65, 1);
        rocs = sugar::make_range(95, 105, 1);
        thresholds = sugar::make_drange(0.10, 0.20, 0.01);
#else
        // Release: customizable ranges with safe defaults
        std::size_t fast_min = 45, fast_max = 55, fast_step = 1;
        std::size_t slow_min = 55, slow_max = 65, slow_step = 1;
        std::size_t roc_min = 95, roc_max = 105, roc_step = 1;
        double      thr_min = 0.10, thr_max = 0.20, thr_step = 0.01;

        std::cout << "\nConfigure sweep ranges (press Enter for defaults).\n";

        {
            std::cout << "Fast SMA (min max step) [default: 45 55 1]: ";
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                std::size_t a, b, c;
                if (parse_triple_line(line, a, b, c) && a <= b && c > 0) {
                    fast_min = a; fast_max = b; fast_step = c;
                }
                else {
                    std::cout << " Invalid input, using defaults.\n";
                }
            }
        }

        {
            std::cout << "Slow SMA (min max step) [default: 55 65 1]: ";
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                std::size_t a, b, c;
                if (parse_triple_line(line, a, b, c) && a <= b && c > 0) {
                    slow_min = a; slow_max = b; slow_step = c;
                }
                else {
                    std::cout << " Invalid input, using defaults.\n";
                }
            }
        }

        {
            std::cout << "ROC length (min max step) [default: 95 105 1]: ";
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                std::size_t a, b, c;
                if (parse_triple_line(line, a, b, c) && a <= b && c > 0) {
                    roc_min = a; roc_max = b; roc_step = c;
                }
                else {
                    std::cout << " Invalid input, using defaults.\n";
                }
            }
        }

        {
            std::cout << "Threshold % (min max step) [default: 0.10 0.20 0.01]: ";
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                double a, b, c;
                if (parse_triple_line(line, a, b, c) && a <= b && c > 0.0) {
                    thr_min = a; thr_max = b; thr_step = c;
                }
                else {
                    std::cout << " Invalid input, using defaults.\n";
                }
            }
        }

        fasts = sugar::make_range(fast_min, fast_max, fast_step);
        slows = sugar::make_range(slow_min, slow_max, slow_step);
        rocs = sugar::make_range(roc_min, roc_max, roc_step);
        thresholds = sugar::make_drange(thr_min, thr_max, thr_step);
#endif




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
            // eat remaining newline so later getline calls behave
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        auto best = sugar::sweep_roc_sma(diff_series, fasts, slows, rocs, thresholds, mode);
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

        // Run DiffCrossStrategy with best sweep params 
        std::size_t fast_period = bf;
        std::size_t slow_period = bs;
        std::size_t roc_len = br;
        double      thresh = btval;

        // Build indicator pipeline: ROC(SMA_fast) vs ROC(SMA_slow)
        auto fast_sma = std::make_shared<sugar::SMAIndicator>(fast_period);
        auto slow_sma = std::make_shared<sugar::SMAIndicator>(slow_period);

        auto fast_roc = sugar::map_roc(fast_sma, roc_len);
        auto slow_roc = sugar::map_roc(slow_sma, roc_len);

        sugar::DiffCrossStrategy strat{ fast_roc, slow_roc, thresh, mode };

        sugar::Backtester bt;
        auto res = bt.run(strat, diff_series);

        std::cout << "\nStrategy ROC(SMA) diff-cross results:\n";
        sugar::print_backtest_summary(res, std::cout, " ");

        if (res.best_start_date > 0) {
            char buf[9];
            sugar::format_yyyymmdd(res.best_start_date, buf);
            std::cout << " Pretty start date: "
                << std::string_view(buf, 8) << "\n";
        }

        // Pause for user to read data 
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();

        return 0;
    }

    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
