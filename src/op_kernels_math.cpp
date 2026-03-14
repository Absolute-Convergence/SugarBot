#include "op_kernels_math.h"

#include <cmath>
#include <deque>
#include <limits>

namespace sugar::op_kernels_math {

namespace {
double qnan() { return std::numeric_limits<double>::quiet_NaN(); }
}

std::vector<double> rolling_max(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.empty()) return out;

    std::deque<std::size_t> dq;
    for (std::size_t i = 0; i < x.size(); ++i) {
        while (!dq.empty() && dq.front() + n <= i) dq.pop_front();
        while (!dq.empty() && x[dq.back()] <= x[i]) dq.pop_back();
        dq.push_back(i);
        if (i + 1 >= n) out[i] = x[dq.front()];
    }
    return out;
}

std::vector<double> rolling_min(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.empty()) return out;

    std::deque<std::size_t> dq;
    for (std::size_t i = 0; i < x.size(); ++i) {
        while (!dq.empty() && dq.front() + n <= i) dq.pop_front();
        while (!dq.empty() && x[dq.back()] >= x[i]) dq.pop_back();
        dq.push_back(i);
        if (i + 1 >= n) out[i] = x[dq.front()];
    }
    return out;
}

std::vector<double> sma(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    double sum = 0.0;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) ++bad;
        else sum += x[i];
    }
    out[n - 1] = (bad == 0) ? (sum / static_cast<double>(n)) : qnan();

    for (std::size_t i = n; i < x.size(); ++i) {
        const double oldv = x[i - n];
        const double newv = x[i];
        if (!std::isfinite(oldv)) --bad; else sum -= oldv;
        if (!std::isfinite(newv)) ++bad; else sum += newv;
        out[i] = (bad == 0) ? (sum / static_cast<double>(n)) : qnan();
    }
    return out;
}

std::vector<double> ema(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    const double alpha = 2.0 / (static_cast<double>(n) + 1.0);
    std::size_t run = 0;
    long double sum = 0.0L;
    bool seeded = false;

    for (std::size_t i = 0; i < x.size(); ++i) {
        const double v = x[i];
        if (!std::isfinite(v)) {
            run = 0;
            sum = 0.0L;
            seeded = false;
            continue;
        }
        if (!seeded) {
            sum += static_cast<long double>(v);
            ++run;
            if (run == n) {
                out[i] = static_cast<double>(sum / static_cast<long double>(n));
                seeded = true;
            }
        }
        else {
            out[i] = alpha * v + (1.0 - alpha) * out[i - 1];
        }
    }
    return out;
}

std::vector<double> roc(std::span<const double> x, std::size_t k) {
    std::vector<double> out(x.size(), qnan());
    if (k == 0 || x.size() <= k) return out;
    for (std::size_t i = k; i < x.size(); ++i) {
        const double prev = x[i - k];
        const double cur = x[i];
        if (prev == 0.0 || !std::isfinite(prev) || !std::isfinite(cur)) continue;
        out[i] = (cur / prev - 1.0) * 100.0;
    }
    return out;
}

std::vector<double> stddev(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    long double sum = 0.0L;
    long double sumsq = 0.0L;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        if (!std::isfinite(v)) { ++bad; continue; }
        const long double lv = static_cast<long double>(v);
        sum += lv;
        sumsq += lv * lv;
    }
    auto emit = [&](std::size_t i) {
        if (bad != 0) { out[i] = qnan(); return; }
        const long double mean = sum / static_cast<long double>(n);
        long double var = (sumsq / static_cast<long double>(n)) - (mean * mean);
        if (var < 0.0L) var = 0.0L;
        out[i] = std::sqrt(static_cast<double>(var));
        };
    emit(n - 1);

    for (std::size_t i = n; i < x.size(); ++i) {
        const double oldv = x[i - n];
        const double newv = x[i];
        if (!std::isfinite(oldv)) --bad;
        else {
            const long double lv = static_cast<long double>(oldv);
            sum -= lv;
            sumsq -= lv * lv;
        }
        if (!std::isfinite(newv)) ++bad;
        else {
            const long double lv = static_cast<long double>(newv);
            sum += lv;
            sumsq += lv * lv;
        }
        emit(i);
    }
    return out;
}

std::vector<double> stddev_ema(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;

    const auto mean = ema(x, n);
    std::vector<double> dev2(x.size(), qnan());
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(mean[i])) continue;
        const double d = x[i] - mean[i];
        dev2[i] = d * d;
    }
    const auto var = ema(std::span<const double>(dev2.data(), dev2.size()), n);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!std::isfinite(var[i])) continue;
        const double sd = std::sqrt(var[i]);
        out[i] = std::isfinite(sd) ? sd : qnan();
    }
    return out;
}

std::vector<double> zscore_sma(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;
    const auto mean = sma(x, n);
    const auto sd = stddev(x, n);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) continue;
        out[i] = (x[i] - mean[i]) / sd[i];
    }
    return out;
}

std::vector<double> zscore_ema(std::span<const double> x, std::size_t n) {
    std::vector<double> out(x.size(), qnan());
    if (n == 0 || x.size() < n) return out;
    const auto mean = ema(x, n);
    const auto sd = stddev_ema(x, n);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) continue;
        out[i] = (x[i] - mean[i]) / sd[i];
    }
    return out;
}

} // namespace sugar::op_kernels_math
