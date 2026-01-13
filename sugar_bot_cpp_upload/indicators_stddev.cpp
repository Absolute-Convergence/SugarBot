#include "indicators_stddev.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    // Rolling/windowed stddev, population variance (divide by n).
    std::vector<double> stddev_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) return out;

        long double sum = 0.0L;
        long double sumsq = 0.0L;

        for (std::size_t i = 0; i < n; ++i) {
            const double x = v[i];
            sum += static_cast<long double>(x);
            sumsq += static_cast<long double>(x) * static_cast<long double>(x);
        }

        auto emit = [&](std::size_t i) {
            const long double mean = sum / static_cast<long double>(n);
            long double var = (sumsq / static_cast<long double>(n)) - (mean * mean);
            if (var < 0.0L) var = 0.0L; // clamp tiny negative from rounding
            out[i] = std::sqrt(static_cast<double>(var));
            };

        emit(n - 1);

        for (std::size_t i = n; i < v.size(); ++i) {
            const long double x_new = static_cast<long double>(v[i]);
            const long double x_old = static_cast<long double>(v[i - n]);
            sum += x_new - x_old;
            sumsq += x_new * x_new - x_old * x_old;
            emit(i);
        }

        return out;
    }

    std::vector<double> StdDevIndicator::compute(const CandleSeries& series) const {
        const auto v = series.closes(); // closes() returns by value (allocates)
        return stddev_over_series(v, n_);
    }

    // EMA-based stddev 
    std::vector<double> stddev_ema_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) return out; // match ema_over_series guard convention

        const auto mean = ema_over_series(v, n);
        const std::size_t mean0 = n - 1; // first defined mean index

        // Need n defined dev2 values to seed variance EMA:
        // dev2 defined starting at mean0, so need v.size() >= mean0 + n == 2n - 1
        if (v.size() < mean0 + n) return out;

        std::vector<double> dev2_tail;
        dev2_tail.reserve(v.size() - mean0);
        for (std::size_t i = mean0; i < v.size(); ++i) {
            const double d = v[i] - mean[i]; // mean[i] defined for i >= mean0
            dev2_tail.push_back(d * d);
        }

        const auto var_tail = ema_over_series(dev2_tail, n);

        for (std::size_t t = 0; t < var_tail.size(); ++t) {
            const std::size_t i = mean0 + t;
            if (i >= out.size()) break;

            if (!std::isfinite(var_tail[t])) {
                out[i] = qnan();
                continue;
            }

            const double sd = std::sqrt(var_tail[t]);
            out[i] = (sd == 0.0 || !std::isfinite(sd)) ? qnan() : sd;
        }

        return out;
    }

    std::vector<double> StdDevEMAIndicator::compute(const CandleSeries& series) const {
        const auto v = series.closes(); // closes() returns by value (allocates)
        return stddev_ema_over_series(v, n_);
    }

} // namespace sugar
