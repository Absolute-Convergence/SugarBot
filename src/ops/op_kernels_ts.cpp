#include "op_kernels_ts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ops/op_kernels_math.h"

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

double rolling_percentile_value(std::span<const double> x, std::size_t begin, std::size_t n, double q) {
    std::vector<double> window;
    window.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[begin + i];
        if (!std::isfinite(v)) return qnan();
        window.push_back(v);
    }

    std::sort(window.begin(), window.end());
    const double normalized_q = std::clamp(q, 0.0, 100.0) / 100.0;
    const double position = normalized_q * static_cast<double>(window.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(position));
    const auto hi = static_cast<std::size_t>(std::ceil(position));
    if (lo == hi) return window[lo];

    const double weight_hi = position - static_cast<double>(lo);
    const double weight_lo = 1.0 - weight_hi;
    return (window[lo] * weight_lo) + (window[hi] * weight_hi);
}

double rolling_entropy_value(std::span<const double> x, std::size_t begin, std::size_t n, std::size_t bins) {
    if (n == 0 || bins == 0) return qnan();

    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[begin + i];
        if (!std::isfinite(v)) return qnan();
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) return qnan();
    if (mn == mx) return 0.0;

    std::vector<std::size_t> counts(bins, 0);
    const double denom = mx - mn;
    for (std::size_t i = 0; i < n; ++i) {
        const double normalized = (x[begin + i] - mn) / denom;
        const auto idx = std::min<std::size_t>(
            static_cast<std::size_t>(std::floor(normalized * static_cast<double>(bins))),
            bins - 1);
        ++counts[idx];
    }

    double entropy = 0.0;
    const double total = static_cast<double>(n);
    for (const std::size_t count : counts) {
        if (count == 0) continue;
        const double p = static_cast<double>(count) / total;
        entropy -= p * std::log(p);
    }
    return std::isfinite(entropy) ? entropy : qnan();
}

double rolling_autocorrelation_value(std::span<const double> x, std::size_t begin, std::size_t n, std::size_t lag) {
    if (n < 2 || lag == 0 || lag >= n) return qnan();

    const std::size_t pairs = n - lag;
    double mean_cur = 0.0;
    double mean_lag = 0.0;
    for (std::size_t i = lag; i < n; ++i) {
        const double cur = x[begin + i];
        const double prev = x[begin + i - lag];
        if (!std::isfinite(cur) || !std::isfinite(prev)) return qnan();
        mean_cur += cur;
        mean_lag += prev;
    }
    mean_cur /= static_cast<double>(pairs);
    mean_lag /= static_cast<double>(pairs);

    double cov = 0.0;
    double var_cur = 0.0;
    double var_lag = 0.0;
    for (std::size_t i = lag; i < n; ++i) {
        const double dc = x[begin + i] - mean_cur;
        const double dl = x[begin + i - lag] - mean_lag;
        cov += dc * dl;
        var_cur += dc * dc;
        var_lag += dl * dl;
    }

    const double denom = std::sqrt(var_cur * var_lag);
    if (denom == 0.0 || !std::isfinite(denom)) return qnan();
    const double corr = cov / denom;
    return std::isfinite(corr) ? corr : qnan();
}

double rolling_hurst_value(std::span<const double> x, std::size_t begin, std::size_t n) {
    if (n < 2) return qnan();

    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[begin + i];
        if (!std::isfinite(v)) return qnan();
        mean += v;
    }
    mean /= static_cast<double>(n);

    double cumulative = 0.0;
    double min_cumulative = std::numeric_limits<double>::infinity();
    double max_cumulative = -std::numeric_limits<double>::infinity();
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double centered = x[begin + i] - mean;
        cumulative += centered;
        min_cumulative = std::min(min_cumulative, cumulative);
        max_cumulative = std::max(max_cumulative, cumulative);
        sum_sq += centered * centered;
    }

    const double range = max_cumulative - min_cumulative;
    const double stddev = std::sqrt(sum_sq / static_cast<double>(n));
    if (range <= 0.0 || stddev <= 0.0 || !std::isfinite(range) || !std::isfinite(stddev)) {
        return qnan();
    }

    const double hurst = std::log(range / stddev) / std::log(static_cast<double>(n));
    return std::isfinite(hurst) ? hurst : qnan();
}

double rolling_regime_change_value(std::span<const double> x, std::size_t begin, std::size_t n) {
    if (n < 2) return qnan();

    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[begin + i];
        if (!std::isfinite(v)) return qnan();
        mean += v;
    }
    mean /= static_cast<double>(n);

    double sum_sq = 0.0;
    double cumulative = 0.0;
    double max_abs_cumulative = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double centered = x[begin + i] - mean;
        sum_sq += centered * centered;
        cumulative += centered;
        max_abs_cumulative = std::max(max_abs_cumulative, std::fabs(cumulative));
    }

    const double stddev = std::sqrt(sum_sq / static_cast<double>(n));
    const double denom = stddev * std::sqrt(static_cast<double>(n));
    if (denom == 0.0 || !std::isfinite(denom)) return qnan();
    const double score = max_abs_cumulative / denom;
    return std::isfinite(score) ? score : qnan();
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

std::vector<double> ts_percentile(std::span<const double> x, std::size_t n, double q) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    for (std::size_t i = n - 1; i < x.size(); ++i) {
        out[i] = rolling_percentile_value(x, i + 1 - n, n, q);
    }
    return out;
}

std::vector<double> ts_entropy(std::span<const double> x, std::size_t n, std::size_t bins) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || bins == 0 || x.size() < n) return out;

    for (std::size_t i = n - 1; i < x.size(); ++i) {
        out[i] = rolling_entropy_value(x, i + 1 - n, n, bins);
    }
    return out;
}

std::vector<double> ts_autocorrelation(std::span<const double> x, std::size_t n, std::size_t lag) {
    std::vector<double> out(x.size(), qnan());
    if (n < 2 || lag == 0 || lag >= n || x.size() < n) return out;

    for (std::size_t i = n - 1; i < x.size(); ++i) {
        out[i] = rolling_autocorrelation_value(x, i + 1 - n, n, lag);
    }
    return out;
}

std::vector<double> ts_hurst(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n < 2 || x.size() < n) return out;

    for (std::size_t i = n - 1; i < x.size(); ++i) {
        out[i] = rolling_hurst_value(x, i + 1 - n, n);
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

std::vector<double> ts_macd_histogram(std::span<const double> x, std::size_t fast, std::size_t slow, std::size_t signal) {
    if (fast == 0 || slow == 0 || signal == 0) {
        return std::vector<double>(x.size(), qnan());
    }

    const auto fast_ema = op_kernels_math::ema(x, fast);
    const auto slow_ema = op_kernels_math::ema(x, slow);
    const auto macd_line = ts_sub(
        std::span<const double>(fast_ema.data(), fast_ema.size()),
        std::span<const double>(slow_ema.data(), slow_ema.size()));
    const auto signal_line = op_kernels_math::ema(
        std::span<const double>(macd_line.data(), macd_line.size()),
        signal);
    return ts_sub(
        std::span<const double>(macd_line.data(), macd_line.size()),
        std::span<const double>(signal_line.data(), signal_line.size()));
}

std::vector<double> ts_bollinger_width(std::span<const double> x, std::size_t n, double k) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    const auto mean = op_kernels_math::sma(x, n);
    const auto sd = op_kernels_math::stddev(x, n);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!std::isfinite(mean[i]) || !std::isfinite(sd[i])) continue;
        const double denom = std::fabs(mean[i]);
        if (denom == 0.0) continue;
        out[i] = (2.0 * k * sd[i]) / denom;
    }
    return out;
}

std::vector<double> ts_regime_change(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n < 2 || x.size() < n) return out;

    for (std::size_t i = n - 1; i < x.size(); ++i) {
        out[i] = rolling_regime_change_value(x, i + 1 - n, n);
    }
    return out;
}

std::vector<double> ts_volatility_ratio(std::span<const double> x, std::size_t fast_n, std::size_t slow_n) {
    std::vector<double> out(x.size(), qnan());
    if (fast_n == 0 || slow_n == 0) return out;

    const auto fast = op_kernels_math::stddev(x, fast_n);
    const auto slow = op_kernels_math::stddev(x, slow_n);
    const std::size_t m = std::max(fast.size(), slow.size());
    out.assign(m, qnan());

    for (std::size_t i = 0; i < m; ++i) {
        const double fast_sd = (i < fast.size()) ? fast[i] : qnan();
        const double slow_sd = (i < slow.size()) ? slow[i] : qnan();
        if (!std::isfinite(fast_sd) || !std::isfinite(slow_sd) || slow_sd == 0.0) continue;
        out[i] = fast_sd / slow_sd;
    }
    return out;
}

} // namespace sugar::op_kernels
