#include "debug_harness.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "series.h"
#include "series_view.h"
#include "utils.h"

#include "indicator.h"
#include "indicators_atr.h"
#include "indicators_rsi.h"
#include "indicators_donchian.h"
#include "indicators_stddev.h"
#include "indicators_zscore.h"

#include "indicators_composite_numeric.h"
#include "indicators_composite_bool.h"

namespace sugar {

#ifdef _DEBUG

static void run_series_contract_test(const CandleSeries& series, std::ostream& out) {
    const auto& rows = series.rows();
    const auto& closes = series.closes();
    const auto& vols = series.volumes();

    out << "\n[Series Contract Test] AoS(rows) vs cached columns\n";
    out << "  rows.size()   = " << rows.size() << "\n";
    out << "  closes.size() = " << closes.size() << "\n";
    out << "  vols.size()   = " << vols.size() << "\n";

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
                    format_yyyymmdd(rows[i].date, buf);
                    out << "  [FAIL] " << name << " value mismatch at i=" << i
                        << " (date " << std::string_view(buf, 8) << ")\n";
                    out << "         col=" << std::fixed << std::setprecision(8) << col[i]
                        << " rows=" << getter(rows[i]) << "\n";
                    return;
                }
            }
            out << "  [OK]   " << name << " values aligned with rows[]\n";
        };

    check_values("closes()", closes, [](const Candle& c) { return c.close; });
    check_values("volumes()", vols, [](const Candle& c) { return c.volume; });

    out << "[Series Contract Test] Done.\n\n";
}

static void run_indicator_contract_test(const CandleSeries& series, std::ostream& out) {
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

    ATRIndicator atr(n);
    RSIIndicator rsi(n);

    const auto v_atr = atr.compute(series);
    const auto v_rsi = rsi.compute(series);

#if SUGAR_HAVE_STDDEV
    StdDevIndicator    sd_sma(n);
    StdDevEMAIndicator sd_ema(n);
    const auto v_sd_sma = sd_sma.compute(series);
    const auto v_sd_ema = sd_ema.compute(series);
#else
    const std::vector<double> v_sd_sma;
    const std::vector<double> v_sd_ema;
#endif

#if SUGAR_HAVE_ZSCORE
    ZScoreSMAIndicator z_sma(n);
    ZScoreEMAIndicator z_ema(n);
    const auto v_z_sma = z_sma.compute(series);
    const auto v_z_ema = z_ema.compute(series);
#else
    const std::vector<double> v_z_sma;
    const std::vector<double> v_z_ema;
#endif

#if SUGAR_HAVE_DONCHIAN
    DonchianIndicator dc_u(n_dc, DonchianOutput::Upper);
    DonchianIndicator dc_l(n_dc, DonchianOutput::Lower);
    DonchianIndicator dc_m(n_dc, DonchianOutput::Middle);
    DonchianIndicator dc_w(n_dc, DonchianOutput::Width);
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
        format_yyyymmdd(rows[i].date, buf);

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

static void run_composition_contract_test(const CandleSeries& series, std::ostream& out) {
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
        mism += (a.size() > n) ? (a.size() - n) : 0;
        mism += (b.size() > n) ? (b.size() - n) : 0;
        return mism;
    };

    auto count_true = [](const BoolSeries& b) {
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

    const auto view_full = SeriesView::from_series(series);
    const auto view_close_only = SeriesView::from_close(series.closes());

    // Graph A: roc( ema( close, 14 ), 5 )
    const std::size_t n_ema = 14;
    const std::size_t n_roc = 5;
    const std::size_t exp_first_A = (n_ema > 0 ? (n_ema - 1) : 0) + n_roc;

    IndicatorPtr close = std::make_shared<CloseIndicator>();
    IndicatorPtr ema14 = map_ema(close, n_ema);
    IndicatorPtr roc5_of_ema14 = map_roc(ema14, n_roc);

    const auto A_series = roc5_of_ema14->compute(series);
    const auto A_view_full = roc5_of_ema14->compute(view_full);
    const auto A_view_close = roc5_of_ema14->compute(view_close_only);

    // Graph B: zscore_sma( roc(close, 10), 20 )
    const std::size_t n_rocB = 10;
    const std::size_t n_zB = 20;
    const std::size_t exp_first_B = n_rocB + (n_zB > 0 ? (n_zB - 1) : 0);

    IndicatorPtr roc10 = map_roc(close, n_rocB);
    IndicatorPtr zscore_of_roc10 = std::make_shared<MapIndicator>(
        roc10,
        [n_zB](const std::vector<double>& v) { return zscore_sma_over_series(v, n_zB); }
    );

    const auto B_series = zscore_of_roc10->compute(series);
    const auto B_view_full = zscore_of_roc10->compute(view_full);
    const auto B_view_close = zscore_of_roc10->compute(view_close_only);

    // Bool: (roc(ema(close,14),5) > 0) AND NOT(cross_up(ema14, sma30))
    auto S1 = bgt(roc5_of_ema14, 0.0);
    IndicatorPtr sma30 = map_sma(close, 30);
    auto S2 = cross_up(ema14, sma30);
    auto S = band(S1, bnot(S2));

    const auto S_series = S->compute(series);
    const auto S_view_full = S->compute(view_full);
    const auto S_view_close = S->compute(view_close_only);

    out << "\n[Composition Contract Test]\n";

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

    auto print_numeric_row = [&](std::size_t i) {
        if (i >= N) return;
        char buf[9];
        format_yyyymmdd(rows[i].date, buf);
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

void maybe_run_debug_test_harness(const CandleSeries& series, std::istream& in, std::ostream& out) {
    if (!prompt_yes_no(in, out, "Run debug test harness now?", true)) {
        out << "\n";
        return;
    }

    out << "\n=== Running Debug Test Harness ===\n";
    run_series_contract_test(series, out);
    run_indicator_contract_test(series, out);
    run_composition_contract_test(series, out);
    out << "=== Debug Test Harness Complete ===\n\n";
}

#else // _DEBUG

void maybe_run_debug_test_harness(const CandleSeries&, std::istream&, std::ostream&) {
    // no-op in release
}

#endif

} // namespace sugar
