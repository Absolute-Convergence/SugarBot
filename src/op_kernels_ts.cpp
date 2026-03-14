#include "op_kernels_ts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "op_kernels_math.h"

namespace sugar::op_kernels {

namespace {
double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

template <typename Op>
std::vector<double> zip_nanpad(std::span<const double> a, std::span<const double> b, Op op) {
    const std::size_t n = std::max(a.size(), b.size());
    std::vector<double> out(n, qnan());
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (i < a.size()) ? a[i] : qnan();
        const double y = (i < b.size()) ? b[i] : qnan();
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        out[i] = op(x, y);
    }
    return out;
}
} // namespace

std::vector<double> ts_add(std::span<const double> a, std::span<const double> b) { return zip_nanpad(a, b, [](double x, double y) { return x + y; }); }
std::vector<double> ts_sub(std::span<const double> a, std::span<const double> b) { return zip_nanpad(a, b, [](double x, double y) { return x - y; }); }
std::vector<double> ts_mul(std::span<const double> a, std::span<const double> b) { return zip_nanpad(a, b, [](double x, double y) { return x * y; }); }
std::vector<double> ts_min(std::span<const double> a, std::span<const double> b) { return zip_nanpad(a, b, [](double x, double y) { return std::min(x, y); }); }
std::vector<double> ts_max(std::span<const double> a, std::span<const double> b) { return zip_nanpad(a, b, [](double x, double y) { return std::max(x, y); }); }

std::vector<double> ts_div(std::span<const double> a, std::span<const double> b) {
    const std::size_t n = std::max(a.size(), b.size());
    std::vector<double> out(n, qnan());
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (i < a.size()) ? a[i] : qnan();
        const double y = (i < b.size()) ? b[i] : qnan();
        if (!std::isfinite(x) || !std::isfinite(y) || y == 0.0) continue;
        out[i] = x / y;
    }
    return out;
}

std::vector<double> ts_neg(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) out[i] = std::isfinite(x[i]) ? -x[i] : qnan();
    return out;
}

std::vector<double> ts_abs(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) out[i] = std::isfinite(x[i]) ? std::abs(x[i]) : qnan();
    return out;
}

std::vector<double> ts_log(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || x[i] <= 0.0) continue;
        out[i] = std::log(x[i]);
    }
    return out;
}

std::vector<double> ts_exp(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i])) continue;
        const double e = std::exp(x[i]);
        out[i] = std::isfinite(e) ? e : qnan();
    }
    return out;
}

std::vector<double> ts_sqrt(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || x[i] < 0.0) continue;
        out[i] = std::sqrt(x[i]);
    }
    return out;
}

std::vector<double> ts_pow_const(std::span<const double> x, double p) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i])) continue;
        const double r = std::pow(x[i], p);
        out[i] = std::isfinite(r) ? r : qnan();
    }
    return out;
}

std::vector<double> ts_shift(std::span<const double> x, std::size_t k, double fill) {
    std::vector<double> out(x.size(), fill);
    if (k == 0) return std::vector<double>(x.begin(), x.end());
    for (std::size_t i = k; i < x.size(); ++i) out[i] = x[i - k];
    return out;
}

std::vector<double> ts_delta(std::span<const double> x) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(x[i - 1])) continue;
        out[i] = x[i] - x[i - 1];
    }
    return out;
}

std::vector<double> ts_clamp_const(std::span<const double> x, double lo, double hi) {
    std::vector<double> out(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i])) continue;
        out[i] = std::min(std::max(x[i], lo), hi);
    }
    return out;
}

std::vector<double> ts_clamp(std::span<const double> x, std::span<const double> lo, std::span<const double> hi) {
    const std::size_t n = std::max({ x.size(), lo.size(), hi.size() });
    std::vector<double> out(n, qnan());
    for (std::size_t i = 0; i < n; ++i) {
        const double xv = (i < x.size()) ? x[i] : qnan();
        const double lv = (i < lo.size()) ? lo[i] : qnan();
        const double hv = (i < hi.size()) ? hi[i] : qnan();
        if (!std::isfinite(xv) || !std::isfinite(lv) || !std::isfinite(hv)) continue;
        out[i] = std::min(std::max(xv, lv), hv);
    }
    return out;
}

std::vector<double> ts_normalize_minmax_global(std::span<const double> x) {
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (const double v : x) {
        if (!std::isfinite(v)) continue;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    std::vector<double> out(x.size(), qnan());
    if (!std::isfinite(mn) || !std::isfinite(mx)) return out;
    const double denom = mx - mn;
    if (denom == 0.0 || !std::isfinite(denom)) return out;

    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i])) continue;
        out[i] = (x[i] - mn) / denom;
    }
    return out;
}

std::vector<double> ts_ratio_of_ma(std::span<const double> close, std::span<const double> ma) {
    return ts_div(close, ma);
}

std::vector<double> ts_roc(std::span<const double> x, std::size_t k) {
    return op_kernels_math::roc(x, k);
}

std::vector<double> ts_sma(std::span<const double> x, std::size_t n) {
    return op_kernels_math::sma(x, n);
}

std::vector<double> ts_ema(std::span<const double> x, std::size_t n) {
    return op_kernels_math::ema(x, n);
}

std::vector<double> ts_roll_max(std::span<const double> x, std::size_t n) {
    return op_kernels_math::rolling_max(x, n);
}

std::vector<double> ts_roll_min(std::span<const double> x, std::size_t n) {
    return op_kernels_math::rolling_min(x, n);
}

std::vector<double> ts_donchian_upper(std::span<const double> x, std::size_t n) {
    return op_kernels_math::rolling_max(x, n);
}

std::vector<double> ts_donchian_lower(std::span<const double> x, std::size_t n) {
    return op_kernels_math::rolling_min(x, n);
}

std::vector<double> ts_donchian_mid(std::span<const double> x, std::size_t n) {
    const auto hi = op_kernels_math::rolling_max(x, n);
    const auto lo = op_kernels_math::rolling_min(x, n);
    const std::size_t m = std::max(hi.size(), lo.size());
    std::vector<double> out(m, qnan());
    for (std::size_t i = 0; i < m; ++i) {
        const double h = (i < hi.size()) ? hi[i] : qnan();
        const double l = (i < lo.size()) ? lo[i] : qnan();
        if (!std::isfinite(h) || !std::isfinite(l)) continue;
        out[i] = 0.5 * (h + l);
    }
    return out;
}

std::vector<double> ts_donchian_width(std::span<const double> x, std::size_t n) {
    const auto hi = op_kernels_math::rolling_max(x, n);
    const auto lo = op_kernels_math::rolling_min(x, n);
    const std::size_t m = std::max(hi.size(), lo.size());
    std::vector<double> out(m, qnan());
    for (std::size_t i = 0; i < m; ++i) {
        const double h = (i < hi.size()) ? hi[i] : qnan();
        const double l = (i < lo.size()) ? lo[i] : qnan();
        if (!std::isfinite(h) || !std::isfinite(l)) continue;
        out[i] = h - l;
    }
    return out;
}

std::vector<double> ts_normalize_minmax(std::span<const double> x, std::size_t n) {
    const auto rmin = op_kernels_math::rolling_min(x, n);
    const auto rmax = op_kernels_math::rolling_max(x, n);
    const std::size_t m = std::max({ x.size(), rmin.size(), rmax.size() });
    std::vector<double> out(m, qnan());

    for (std::size_t i = 0; i < m; ++i) {
        const double v = (i < x.size()) ? x[i] : qnan();
        const double mn = (i < rmin.size()) ? rmin[i] : qnan();
        const double mx = (i < rmax.size()) ? rmax[i] : qnan();
        if (!std::isfinite(v) || !std::isfinite(mn) || !std::isfinite(mx)) continue;
        const double denom = mx - mn;
        if (!std::isfinite(denom)) continue;
        if (denom == 0.0) out[i] = 0.0;
        else out[i] = (v - mn) / denom;
    }
    return out;
}

std::vector<double> ts_true_range_ohlc(std::span<const double> high, std::span<const double> low, std::span<const double> close) {
    const std::size_t n = close.size();
    std::vector<double> tr(n, qnan());
    if (n == 0 || high.size() < n || low.size() < n) return tr;

    const double h0 = high[0];
    const double l0 = low[0];
    if (std::isfinite(h0) && std::isfinite(l0)) tr[0] = h0 - l0;

    for (std::size_t i = 1; i < n; ++i) {
        const double h = high[i];
        const double l = low[i];
        const double pc = close[i - 1];
        if (!std::isfinite(h) || !std::isfinite(l) || !std::isfinite(pc)) continue;

        const double hl = h - l;
        const double hc = std::fabs(h - pc);
        const double lc = std::fabs(l - pc);
        tr[i] = std::max({ hl, hc, lc });
    }

    return tr;
}

std::vector<double> ts_true_range_delta(std::span<const double> x) {
    std::vector<double> tr(x.size(), qnan());
    if (x.empty()) return tr;
    tr[0] = std::isfinite(x[0]) ? 0.0 : qnan();
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(x[i - 1])) continue;
        tr[i] = std::fabs(x[i] - x[i - 1]);
    }
    return tr;
}

std::vector<double> ts_atr_wilder(std::span<const double> tr, std::size_t n) {
    std::vector<double> out(tr.size(), qnan());
    if (n == 0 || tr.size() < n) return out;

    std::size_t run = 0;
    double seed_sum = 0.0;
    bool seeded = false;
    double atr = qnan();

    for (std::size_t i = 0; i < tr.size(); ++i) {
        const double v = tr[i];
        if (!std::isfinite(v)) {
            run = 0;
            seed_sum = 0.0;
            seeded = false;
            atr = qnan();
            continue;
        }

        if (!seeded) {
            seed_sum += v;
            ++run;
            if (run == n) {
                atr = seed_sum / static_cast<double>(n);
                out[i] = atr;
                seeded = true;
            }
        }
        else {
            atr = (atr * (static_cast<double>(n) - 1.0) + v) / static_cast<double>(n);
            out[i] = atr;
        }
    }

    return out;
}

std::vector<double> ts_atr_from_series(std::span<const double> x, std::size_t n) {
    const auto tr = ts_true_range_delta(x);
    return ts_atr_wilder(std::span<const double>(tr.data(), tr.size()), n);
}

std::vector<double> ts_rsi_wilder(std::span<const double> close, std::size_t n) {
    std::vector<double> out(close.size(), qnan());
    if (n == 0 || close.size() <= n) return out;

    auto rsi_from_avgs = [](double g, double l) {
        if (l == 0.0) return 100.0;
        if (g == 0.0) return 0.0;
        const double rs = g / l;
        return 100.0 - (100.0 / (1.0 + rs));
    };

    std::size_t run = 0;
    double gain_sum = 0.0;
    double loss_sum = 0.0;
    double avg_gain = 0.0;
    double avg_loss = 0.0;
    bool seeded = false;

    for (std::size_t i = 1; i < close.size(); ++i) {
        const double cur = close[i];
        const double prev = close[i - 1];
        if (!std::isfinite(cur) || !std::isfinite(prev)) {
            run = 0;
            gain_sum = 0.0;
            loss_sum = 0.0;
            seeded = false;
            continue;
        }

        const double d = cur - prev;
        const double gain = (d > 0.0) ? d : 0.0;
        const double loss = (d < 0.0) ? -d : 0.0;

        if (!seeded) {
            gain_sum += gain;
            loss_sum += loss;
            ++run;
            if (run == n) {
                avg_gain = gain_sum / static_cast<double>(n);
                avg_loss = loss_sum / static_cast<double>(n);
                out[i] = rsi_from_avgs(avg_gain, avg_loss);
                seeded = true;
            }
        }
        else {
            avg_gain = (avg_gain * (static_cast<double>(n) - 1.0) + gain) / static_cast<double>(n);
            avg_loss = (avg_loss * (static_cast<double>(n) - 1.0) + loss) / static_cast<double>(n);
            out[i] = rsi_from_avgs(avg_gain, avg_loss);
        }
    }

    return out;
}

} // namespace sugar::op_kernels
