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

#include "indicator.h"
#include "indicators_composite_bool.h"

#include "strategy_menu.h"

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

#ifdef _DEBUG

static bool prompt_yes_no(std::istream& in, std::ostream& out, std::string_view question, bool default_no = true) {
    out << question << (default_no ? " [y/N]: " : " [Y/n]: ");
    out.flush();

    std::string line;
    std::getline(in, line);
    if (line.empty()) return !default_no;

    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
    if (c == 'y') return true;
    if (c == 'n') return false;
    return !default_no;
}

static void run_series_contract_test(const sugar::CandleSeries& series, std::ostream& out) {
    const auto& rows = series.rows();
    const auto& closes = series.closes();
    const auto& vols = series.volumes();

    out << "\n[Series Contract Test] AoS(rows) vs cached columns\n";
    out << "  rows.size()   = " << rows.size() << "\n";
    out << "  closes.size() = " << closes.size() << "\n";
    out << "  vols.size()   = " << vols.size() << "\n";

    // After Hybrid AoS + cached SoA columns refactor, uncomment these:
    // out << "  opens.size()  = " << series.opens().size() << "\n";
    // out << "  highs.size()  = " << series.highs().size() << "\n";
    // out << "  lows.size()   = " << series.lows().size() << "\n";

    auto fail_size = [&](const char* name, std::size_t got, std::size_t exp) {
        out << "  [FAIL] " << name << " size mismatch: " << got << " vs rows " << exp << "\n";
        };

    if (closes.size() != rows.size()) fail_size("closes()", closes.size(), rows.size());
    else out << "  [OK]   closes() size aligned\n";

    if (vols.size() != rows.size()) fail_size("volumes()", vols.size(), rows.size());
    else out << "  [OK]   volumes() size aligned\n";

    if (rows.empty()) {
        out << "  [SKIP] empty series\n";
        out << "[Series Contract Test] Done.\n\n";
        return;
    }

    auto check_values = [&](const char* name,
        const std::vector<double>& col,
        auto getter) {
            if (col.size() != rows.size()) return; // already reported size mismatch
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (col[i] != getter(rows[i])) {
                    char buf[9];
                    sugar::format_yyyymmdd(rows[i].date, buf);
                    out << "  [FAIL] " << name << " value mismatch at i=" << i
                        << " (date " << std::string_view(buf, 8) << ")\n";
                    out << "         col=" << std::fixed << std::setprecision(8) << col[i]
                        << " rows=" << getter(rows[i]) << "\n";
                    return;
                }
            }
            out << "  [OK]   " << name << " values aligned with rows[]\n";
        };

    check_values("closes()", closes, [](const sugar::Candle& c) { return c.close; });
    check_values("volumes()", vols, [](const sugar::Candle& c) { return c.volume; });

    out << "[Series Contract Test] Done.\n\n";
}

static void run_indicator_contract_test(const sugar::CandleSeries& series, std::ostream& out) {
    const std::size_t n = 14;
    const std::size_t n_dc = 20;

    const auto& rows = series.rows();

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

    sugar::ATRIndicator atr(n);
    sugar::RSIIndicator rsi(n);

    const auto v_atr = atr.compute(series);
    const auto v_rsi = rsi.compute(series);

#if SUGAR_HAVE_STDDEV
    sugar::StdDevIndicator    sd_sma(n);
    sugar::StdDevEMAIndicator sd_ema(n);
    const auto v_sd_sma = sd_sma.compute(series);
    const auto v_sd_ema = sd_ema.compute(series);
#else
    const std::vector<double> v_sd_sma;
    const std::vector<double> v_sd_ema;
#endif

#if SUGAR_HAVE_ZSCORE
    sugar::ZScoreSMAIndicator z_sma(n);
    sugar::ZScoreEMAIndicator z_ema(n);
    const auto v_z_sma = z_sma.compute(series);
    const auto v_z_ema = z_ema.compute(series);
#else
    const std::vector<double> v_z_sma;
    const std::vector<double> v_z_ema;
#endif

#if SUGAR_HAVE_DONCHIAN
    sugar::DonchianIndicator dc_u(n_dc, sugar::DonchianOutput::Upper);
    sugar::DonchianIndicator dc_l(n_dc, sugar::DonchianOutput::Lower);
    sugar::DonchianIndicator dc_m(n_dc, sugar::DonchianOutput::Middle);
    sugar::DonchianIndicator dc_w(n_dc, sugar::DonchianOutput::Width);
    const auto v_dc_u = dc_u.compute(series);
    const auto v_dc_l = dc_l.compute(series);
    const auto v_dc_m = dc_m.compute(series);
    const auto v_dc_w = dc_w.compute(series);
#else
    const std::vector<double> v_dc_u;
    const std::vector<double> v_dc_l;
    const std::vector<double> v_dc_m;
    const std::vector<double> v_dc_w;
#endif

    const std::size_t exp_sd_z_sma = (n > 0) ? (n - 1) : 0;
    const std::size_t exp_sd_z_ema = (2 * n >= 2) ? (2 * n - 2) : 0;
    const std::size_t exp_atr = (n > 0) ? (n - 1) : 0;
    const std::size_t exp_rsi = n;
    const std::size_t exp_dc = (n_dc > 0) ? (n_dc - 1) : 0;

    const std::size_t obs_atr = first_finite_index(v_atr);
    const std::size_t obs_rsi = first_finite_index(v_rsi);
#if SUGAR_HAVE_DONCHIAN
    const std::size_t obs_dc = first_finite_index(v_dc_u);
#else
    const std::size_t obs_dc = v_dc_u.size();
#endif

    out << "\n[Indicator Contract Test] n=" << n << " | donchian n_dc=" << n_dc << "\n";
    out << "Expected first finite:\n";
#if SUGAR_HAVE_STDDEV && SUGAR_HAVE_ZSCORE
    out << "  StdDev(SMA) & ZScore(SMA): index " << exp_sd_z_sma << "\n";
    out << "  StdDev(EMA) & ZScore(EMA): index " << exp_sd_z_ema << "\n";
#elif SUGAR_HAVE_STDDEV
    out << "  StdDev(SMA): index " << exp_sd_z_sma << "\n";
    out << "  StdDev(EMA): index " << exp_sd_z_ema << "\n";
#elif SUGAR_HAVE_ZSCORE
    out << "  ZScore(SMA): index " << exp_sd_z_sma << "\n";
    out << "  ZScore(EMA): index " << exp_sd_z_ema << "\n";
#else
    out << "  (StdDev/ZScore not in this build)\n";
#endif
    out << "  ATR(" << n << "): index " << exp_atr << "\n";
    out << "  RSI(" << n << "): index " << exp_rsi << "\n";
#if SUGAR_HAVE_DONCHIAN
    out << "  Donchian(" << n_dc << "): index " << exp_dc << "\n";
#else
    out << "  Donchian(" << n_dc << "): (not in this build)\n";
#endif

    out << "Observed first finite:\n";
    out << "  ATR: " << (obs_atr < v_atr.size() ? std::to_string(obs_atr) : std::string("never")) << "\n";
    out << "  RSI: " << (obs_rsi < v_rsi.size() ? std::to_string(obs_rsi) : std::string("never")) << "\n";
    if (!v_dc_u.empty())
        out << "  Donchian: " << (obs_dc < v_dc_u.size() ? std::to_string(obs_dc) : std::string("never")) << "\n";
    else
        out << "  Donchian: (not in this build)\n";

    auto print_row = [&](std::size_t i) {
        if (i >= rows.size()) return;
        char buf[9];
        sugar::format_yyyymmdd(rows[i].date, buf);

        out << std::setw(5) << i << "  "
            << std::string_view(buf, 8)
            << "  close=" << std::fixed << std::setprecision(2) << rows[i].close
            << "  sd_sma=" << (v_sd_sma.empty() ? "NA" : pv(v_sd_sma[i]))
            << "  sd_ema=" << (v_sd_ema.empty() ? "NA" : pv(v_sd_ema[i]))
            << "  z_sma=" << (v_z_sma.empty() ? "NA" : pv(v_z_sma[i]))
            << "  z_ema=" << (v_z_ema.empty() ? "NA" : pv(v_z_ema[i]))
            << "  atr=" << pv(v_atr[i])
            << "  rsi=" << pv(v_rsi[i])
            << "  dc_u=" << (v_dc_u.empty() ? "NA" : pv(v_dc_u[i]))
            << "  dc_l=" << (v_dc_l.empty() ? "NA" : pv(v_dc_l[i]))
            << "  dc_m=" << (v_dc_m.empty() ? "NA" : pv(v_dc_m[i]))
            << "  dc_w=" << (v_dc_w.empty() ? "NA" : pv(v_dc_w[i]))
            << "\n";
        };

    const std::size_t i1 = exp_sd_z_sma;
    const std::size_t i2 = exp_sd_z_ema;

    out << "\n---- Head (first 12 rows) ----\n";
    for (std::size_t i = 0; i < rows.size() && i < 12; ++i) print_row(i);

    out << "\n---- Around n-1 (SMA/ATR first defined) ----\n";
    for (std::size_t i = (i1 > 3 ? i1 - 3 : 0); i < rows.size() && i <= i1 + 3; ++i) print_row(i);

    out << "\n---- Around 2n-2 (EMA stddev/z first defined) ----\n";
    for (std::size_t i = (i2 > 3 ? i2 - 3 : 0); i < rows.size() && i <= i2 + 3; ++i) print_row(i);

    out << "\n---- Around RSI first defined (index n) ----\n";
    for (std::size_t i = (exp_rsi > 3 ? exp_rsi - 3 : 0); i < rows.size() && i <= exp_rsi + 3; ++i) print_row(i);

    if (!v_dc_u.empty()) {
        out << "\n---- Around Donchian first defined (index n_dc-1) ----\n";
        for (std::size_t i = (exp_dc > 3 ? exp_dc - 3 : 0); i < rows.size() && i <= exp_dc + 3; ++i) print_row(i);
    }

    out << "\n[Indicator Contract Test] Done.\n\n";
}

// -----------------------------------------------------------------------------
// Composition layer tests (numeric + boolean)
//
// Goals:
// - Verify numeric composite graphs (indicator-of-indicator) work and align.
// - Verify boolean composite graphs (comparators/crosses + boolean ops) work.
// - Verify SeriesView-first evaluation works for these graphs:
//     CandleSeries path vs SeriesView path (including close-only vector mode).
//
// Conventions:
// - NaN inputs suppress boolean signals (false) and propagate through numeric graphs.
// - Output sizes should align to the input series length.
// -----------------------------------------------------------------------------
static void run_composition_contract_test(const sugar::CandleSeries& series, std::ostream& out) {
    const auto& rows = series.rows();
    const std::size_t N = rows.size();
    if (N == 0) {
        out << "\n[Composition Contract Test] (skip: empty series)\n\n";
        return;
    }

    auto first_finite_index = [](const std::vector<double>& v) -> std::size_t {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (std::isfinite(v[i])) return i;
        }
        return v.size();
        };

    auto equal_nan = [](double a, double b) {
        if (!std::isfinite(a) && !std::isfinite(b)) return true;
        return a == b;
        };

    auto count_mismatches = [&](const std::vector<double>& a, const std::vector<double>& b) {
        const std::size_t n = std::min(a.size(), b.size());
        std::size_t mism = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!equal_nan(a[i], b[i])) ++mism;
        }
        // If sizes differ, count the tail as mismatches.
        mism += (a.size() > n) ? (a.size() - n) : 0;
        mism += (b.size() > n) ? (b.size() - n) : 0;
        return mism;
        };

    auto count_true = [](const sugar::BoolSeries& b) {
        std::size_t c = 0;
        for (auto x : b) c += (x != 0);
        return c;
        };

    auto pv = [](double x) -> std::string {
        if (!std::isfinite(x)) return "NaN";
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << x;
        return oss.str();
        };

    // Build views
    const auto view_full = sugar::SeriesView::from_series(series);
    const auto view_close_only = sugar::SeriesView::from_close(series.closes());

    // -------------------------
    // Numeric composition graphs
    // -------------------------
    // Graph A: roc( ema( close, 14 ), 5 )
    // Warm-up expectation (best-effort semantics):
    //   EMA(14) first finite at 13
    //   ROC(5)  first finite at 13 + 5 = 18
    const std::size_t n_ema = 14;
    const std::size_t n_roc = 5;
    const std::size_t exp_first_A = (n_ema > 0 ? (n_ema - 1) : 0) + n_roc;

    sugar::IndicatorPtr close = std::make_shared<sugar::CloseIndicator>();
    sugar::IndicatorPtr ema14 = sugar::map_ema(close, n_ema);
    sugar::IndicatorPtr roc5_of_ema14 = sugar::map_roc(ema14, n_roc);

    const auto A_series = roc5_of_ema14->compute(series);
    const auto A_view_full = roc5_of_ema14->compute(view_full);
    const auto A_view_close = roc5_of_ema14->compute(view_close_only);

    // Graph B: zscore_sma( roc(close, 10), 20 )
    // Warm-up expectation:
    //   ROC(10) first finite at 10
    //   ZScore(SMA,20) first finite at (20-1) once inputs are finite -> 10 + 19 = 29
    const std::size_t n_rocB = 10;
    const std::size_t n_zB = 20;
    const std::size_t exp_first_B = n_rocB + (n_zB > 0 ? (n_zB - 1) : 0);

    sugar::IndicatorPtr roc10 = sugar::map_roc(close, n_rocB);
    sugar::IndicatorPtr zscore_of_roc10 = std::make_shared<sugar::MapIndicator>(
        roc10,
        [n_zB](const std::vector<double>& v) { return sugar::zscore_sma_over_series(v, n_zB); }
    );

    const auto B_series = zscore_of_roc10->compute(series);
    const auto B_view_full = zscore_of_roc10->compute(view_full);
    const auto B_view_close = zscore_of_roc10->compute(view_close_only);

    // -------------------------
    // Boolean composition graphs
    // -------------------------
    // Signal S1: roc(ema(close,14),5) > 0
    auto S1 = sugar::bgt(roc5_of_ema14, 0.0);

    // Signal S2: cross_up( ema(close,14), sma(close,30) )
    sugar::IndicatorPtr sma30 = sugar::map_sma(close, 30);
    auto S2 = sugar::cross_up(ema14, sma30);

    // Combined: S = S1 AND NOT(S2)
    auto S = sugar::band(S1, sugar::bnot(S2));

    const auto S_series = S->compute(series);
    const auto S_view_full = S->compute(view_full);
    const auto S_view_close = S->compute(view_close_only);

    // -------------------------
    // Report
    // -------------------------
    out << "\n[Composition Contract Test]\n";

    // Numeric sizes
    auto chk_size = [&](const char* name, std::size_t got) {
        if (got != N) out << "  [FAIL] " << name << " size " << got << " != series.size() " << N << "\n";
        else out << "  [OK]   " << name << " size aligned (" << got << ")\n";
        };

    chk_size("A_series", A_series.size());
    chk_size("A_view_full", A_view_full.size());
    chk_size("A_view_close", A_view_close.size());
    chk_size("B_series", B_series.size());
    chk_size("B_view_full", B_view_full.size());
    chk_size("B_view_close", B_view_close.size());

    // Numeric equality (CandleSeries vs SeriesView should match because both ultimately see same closes)
    const auto mism_A_full = count_mismatches(A_series, A_view_full);
    const auto mism_A_close = count_mismatches(A_series, A_view_close);
    const auto mism_B_full = count_mismatches(B_series, B_view_full);
    const auto mism_B_close = count_mismatches(B_series, B_view_close);

    out << "  Graph A: roc(ema(close,14),5)\n";
    out << "    first finite expected ~" << exp_first_A << " | observed series="
        << (first_finite_index(A_series) < A_series.size() ? std::to_string(first_finite_index(A_series)) : std::string("never"))
        << "\n";
    out << "    mismatches: series vs view_full=" << mism_A_full
        << ", series vs view_close=" << mism_A_close << "\n";

    out << "  Graph B: zscore_sma(roc(close,10),20)\n";
    out << "    first finite expected ~" << exp_first_B << " | observed series="
        << (first_finite_index(B_series) < B_series.size() ? std::to_string(first_finite_index(B_series)) : std::string("never"))
        << "\n";
    out << "    mismatches: series vs view_full=" << mism_B_full
        << ", series vs view_close=" << mism_B_close << "\n";

    // Boolean sizes + basic stats
    auto chk_bsize = [&](const char* name, std::size_t got) {
        if (got != N) out << "  [FAIL] " << name << " size " << got << " != series.size() " << N << "\n";
        else out << "  [OK]   " << name << " size aligned (" << got << ")\n";
        };

    chk_bsize("S_series", S_series.size());
    chk_bsize("S_view_full", S_view_full.size());
    chk_bsize("S_view_close", S_view_close.size());

    const auto t_series = count_true(S_series);
    const auto t_full = count_true(S_view_full);
    const auto t_close = count_true(S_view_close);

    out << "  Bool graph: (roc(ema(close,14),5) > 0) AND NOT(cross_up(ema14, sma30))\n";
    out << "    true counts: series=" << t_series
        << ", view_full=" << t_full
        << ", view_close=" << t_close << "\n";

    // Spot-check a few indices near the expected warm-up boundary for Graph A.
    auto print_numeric_row = [&](std::size_t i) {
        if (i >= N) return;
        char buf[9];
        sugar::format_yyyymmdd(rows[i].date, buf);
        out << "    i=" << std::setw(5) << i
            << " date=" << std::string_view(buf, 8)
            << " A=" << pv(A_series[i])
            << " B=" << pv(B_series[i])
            << " S=" << static_cast<int>(S_series[i])
            << "\n";
        };

    out << "  Spot-check around Graph A expected first finite (" << exp_first_A << ")\n";
    for (std::size_t i = (exp_first_A > 3 ? exp_first_A - 3 : 0); i < N && i <= exp_first_A + 3; ++i) {
        print_numeric_row(i);
    }

    out << "[Composition Contract Test] Done.\n\n";
}

static void maybe_run_debug_test_harness(const sugar::CandleSeries& series, std::istream& in, std::ostream& out) {
#ifdef _DEBUG
    if (!prompt_yes_no(in, out, "Run debug test harness now?", true)) {
        out << "\n";
        return;
    }

    out << "\n=== Running Debug Test Harness ===\n";
    run_series_contract_test(series, out);
    run_indicator_contract_test(series, out);
    run_composition_contract_test(series, out);
    out << "=== Debug Test Harness Complete ===\n\n";
#else
    (void)series; (void)in; (void)out;
#endif
}

#endif // _DEBUG

int main(int argc, char** argv) {
    try {
        // ---------------------------------------------------------------------
        // 0) CSV path handling
        // ---------------------------------------------------------------------
        std::string csv_path = first_non_option_arg(argc, argv);
        if (csv_path.empty()) {
            csv_path = default_csv_path().string();
        }
        csv_path = normalize_path_for_runtime(std::move(csv_path));

#ifndef _DEBUG
        // Release: prompt for optional path override
        std::cout << "CSV file path [default: " << csv_path << "]: \n"
            << "Enter new path to change default -> ";
        {
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                csv_path = normalize_path_for_runtime(std::move(line));
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

        // Requirement: mode toggle in main at runtime.
        opt.mode = sugar::prompt_trade_mode(
            std::cin, std::cout,
            sugar::TradeMode::LongShort
        );

#ifdef _DEBUG
        const bool customize = prompt_yes_no(std::cin, std::cout,
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

        const bool ran = sugar::run_strategy_template(series, opt, std::cout);
        if (!ran) {
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