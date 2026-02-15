#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "csv.h"
#include "utils.h"
#include "series.h"

#include "debug_harness.h"

#include "op_registry.h"

#include "strategy_menu.h"

namespace fs = std::filesystem;

// Path normalization is implemented in utils (sugar::normalize_path_for_runtime).

// Pick a default CSV path that works on both Windows and WSL.
static inline fs::path default_csv_path() {
    fs::path rel = fs::path("data") / "BTCUSD_420.csv";
    if (fs::exists(rel)) return rel;

#ifdef __linux__
    return fs::path("/mnt/c/Dev/sugar_Bot/data/BTCUSD_420.csv");
#else
    return fs::path("C:/Dev/sugar_Bot/data/BTCUSD_420.csv");
#endif
}

// Return the first argv token that isn't an option (doesn't start with "--").
// Useful because Debug runs frequently pass "--test-indicators".
static inline std::string first_non_option_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (!a.starts_with("--")) return std::string(a);
    }
    return {};
}

static inline bool has_flag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (a == flag) return true;
    }
    return false;
}

static void print_csv_preview(const sugar::CandleSeries& series, std::string_view path) {
    const auto& rows = series.rows();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "CSV Preview:\n";

    if (rows.empty()) {
        std::cout << "  [Error] CSV contains no candles!\n";
        return;
    }

    auto print_one = [&](std::string_view label, const sugar::Candle& c) {
        char buf[9];
        sugar::format_yyyymmdd(c.date, buf);
        std::cout << "  " << label << ":\n";
        std::cout << "   " << std::string_view(buf, 8)
            << ", " << c.open
            << ", " << c.high
            << ", " << c.low
            << ", " << c.close
            << ", vol=" << c.volume << "\n";
        };

    if (rows.size() == 1) {
        print_one("Only candle", rows.front());
    }
    else {
        print_one("First candle", rows.front());
        print_one("Last candle", rows.back());
    }

    std::cout << "Loaded " << rows.size() << " candle(s) from '" << path << "'\n\n";
}



int main(int argc, char** argv) {
    try {
        // ---------------------------------------------------------------------
        // 0) Operator registry dump (capabilities of this build)
        // ---------------------------------------------------------------------
        // Usage:
        //   sugar_Bot --dump-ops
        //
        // This prints an AI-ingestible reference manual of all registered ops
        // (name, signature, params, warmup) and exits immediately.
        //
        // NOTE: We do this BEFORE CSV loading / prompts so it's scriptable.
        if (has_flag(argc, argv, "--dump-ops") ||
            has_flag(argc, argv, "--dump_ops") ||
            has_flag(argc, argv, "--dumpops"))
        {
            const auto reg = sugar::make_default_registry();
            std::cout << reg.describe_text(true /*include_modules_header*/);
            return 0;
        }
        // ---------------------------------------------------------------------
        // 0) CSV path handling
        // ---------------------------------------------------------------------
        std::string csv_path = first_non_option_arg(argc, argv);
        if (csv_path.empty()) {
            csv_path = default_csv_path().string();
        }
        csv_path = sugar::normalize_path_for_runtime(std::move(csv_path));

#ifndef _DEBUG
        // Release: prompt for optional path override
        std::cout << "CSV file path [default: " << csv_path << "]: \n"
            << "Enter new path to change default -> ";
        {
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                csv_path = sugar::normalize_path_for_runtime(std::move(line));
            }
        }
#endif

        // ---------------------------------------------------------------------
        // 1) Load data
        // ---------------------------------------------------------------------
        auto candles = sugar::load_candles_csv(csv_path);
        sugar::CandleSeries series{ std::move(candles) };
        print_csv_preview(series, csv_path);

        if (series.rows().empty()) {
            return 0;
        }
#ifdef _DEBUG
        // ---------------------------------------------------------------------
        // 2) Optional indicator sanity harness (debug-only, interactive)
        // ---------------------------------------------------------------------
        maybe_run_debug_test_harness(series, std::cin, std::cout);
#endif
        // ---------------------------------------------------------------------
        // 3) Strategy template menu (runtime)
        // ---------------------------------------------------------------------
        sugar::StrategyRunOptions opt;
        opt.templ = sugar::prompt_strategy_template(std::cin, std::cout);

        // Custom Strategy 99 is PlanSpec-driven and has its own prompt flow.
        // All built-in templates use the standard runtime prompts (mode + optional sweeps).
        if (opt.templ == sugar::StrategyTemplate::Custom_User_PlanSpec) {
            sugar::prompt_custom_user_planspec(std::cin, std::cout, opt);
        }
        else {
            // Requirement: mode toggle in main at runtime.
            opt.mode = sugar::prompt_trade_mode(
                std::cin, std::cout,
                sugar::TradeMode::LongShort
            );

#ifdef _DEBUG
            const bool customize = sugar::prompt_yes_no(std::cin, std::cout,
                "Customize sweep ranges?", true /*default_no*/);
#else
            const bool customize = true;
#endif

            if (customize) {
                switch (opt.templ) {
                case sugar::StrategyTemplate::Donchian_Breakout:
                    sugar::prompt_donchian_sweep(std::cin, std::cout, opt);
                    break;
                case sugar::StrategyTemplate::Gated_Diff_Cross:
                    sugar::prompt_gated_diff_cross_sweep(std::cin, std::cout, opt);
                    break;
                case sugar::StrategyTemplate::ZScore_MeanReversion:
                    sugar::prompt_zscore_mean_reversion_sweep_ranges(std::cin, std::cout, opt);
                    break;
                case sugar::StrategyTemplate::Swing_Breakout_Validation:
                    sugar::prompt_swing_breakout_validation_sweep(std::cin, std::cout, opt);
                    break;
                default:
                    break;
                }
            }
        }

        const bool ran = sugar::run_strategy_template(series, opt, std::cout);
        if (!ran && opt.templ != sugar::StrategyTemplate::Custom_User_PlanSpec) {
            std::cout << "\n[Info] Selected template is not implemented yet.\n";
        }

#ifndef _DEBUG
        // Pause in release so console users can read output.
        std::cout << "\nPress Enter to exit...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
#endif

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
