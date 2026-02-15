#include "indicators_stddev.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    namespace {

        inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

        // Single-kernel truth: rolling/windowed stddev (population variance, divide by n).
        // - Output aligned to input.
        // - Warm-up: first (n-1) entries are NaN.
        // - Best-effort for vector-only composition:
        //   if any element in the current window is non-finite, output is NaN until the window is fully finite again.
        std::vector<double> stddev_kernel(std::span<const double> x, std::size_t n) {
            std::vector<double> out(x.size(), qnan());
            if (n == 0 || x.size() < n) {
                return out;
            }

            long double sum = 0.0L;
            long double sumsq = 0.0L;
            std::size_t bad = 0; // count of non-finite values in the current window

            for (std::size_t i = 0; i < n; ++i) {
                const double v = x[i];
                if (!std::isfinite(v)) {
                    ++bad;
                    continue;
                }
                const long double lv = static_cast<long double>(v);
                sum += lv;
                sumsq += lv * lv;
            }

            auto emit = [&](std::size_t i) {
                if (bad != 0) {
                    out[i] = qnan();
                    return;
                }
                const long double mean = sum / static_cast<long double>(n);
                long double var = (sumsq / static_cast<long double>(n)) - (mean * mean);
                if (var < 0.0L) var = 0.0L; // clamp tiny negative from rounding
                out[i] = std::sqrt(static_cast<double>(var));
                };

            emit(n - 1);

            for (std::size_t i = n; i < x.size(); ++i) {
                const double v_new = x[i];
                const double v_old = x[i - n];

                if (!std::isfinite(v_old)) {
                    --bad;
                }
                else {
                    const long double lv_old = static_cast<long double>(v_old);
                    sum -= lv_old;
                    sumsq -= lv_old * lv_old;
                }

                if (!std::isfinite(v_new)) {
                    ++bad;
                }
                else {
                    const long double lv_new = static_cast<long double>(v_new);
                    sum += lv_new;
                    sumsq += lv_new * lv_new;
                }

                emit(i);
            }

            return out;
        }

        // Best-effort EMA over a series with NaN/inf reset.
        // - Output aligned to input.
        // - Warm-up: first (n-1) entries are NaN; index (n-1) seeded from SMA of the last n finite values.
        // - If a non-finite value is encountered, EMA state resets and will reseed after the next run of n finite values.
        std::vector<double> ema_best_effort_kernel(std::span<const double> x, std::size_t n) {
            std::vector<double> out(x.size(), qnan());
            if (n == 0 || x.size() < n) {
                return out;
            }

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
                    // Seed from the SMA of the last n finite samples in the current contiguous run.
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

        // Single-kernel truth: EMA-based stddev.
        // Uses NaN-resilient EMA kernels to support vector-only indicator composition.
        std::vector<double> stddev_ema_kernel(std::span<const double> x, std::size_t n) {
            std::vector<double> out(x.size(), qnan());
            if (n == 0 || x.size() < n) {
                return out;
            }

            const auto mean = ema_best_effort_kernel(x, n);

            std::vector<double> dev2(x.size(), qnan());
            for (std::size_t i = 0; i < x.size(); ++i) {
                if (!std::isfinite(x[i]) || !std::isfinite(mean[i])) {
                    dev2[i] = qnan();
                    continue;
                }
                const double d = x[i] - mean[i];
                dev2[i] = d * d;
            }

            const auto var = ema_best_effort_kernel(std::span<const double>(dev2.data(), dev2.size()), n);

            for (std::size_t i = 0; i < out.size(); ++i) {
                if (!std::isfinite(var[i])) {
                    out[i] = qnan();
                    continue;
                }
                const double sd = std::sqrt(var[i]);
                out[i] = std::isfinite(sd) ? sd : qnan();
            }

            return out;
        }

    } // namespace

    std::vector<double> stddev_over_series(const std::vector<double>& v, std::size_t n) {
        return stddev_kernel(std::span<const double>(v.data(), v.size()), n);
    }

    std::vector<double> stddev_ema_over_series(const std::vector<double>& v, std::size_t n) {
        return stddev_ema_kernel(std::span<const double>(v.data(), v.size()), n);
    }

    std::vector<double> StdDevIndicator::compute(const CandleSeries& series) const {
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> StdDevIndicator::compute(const SeriesView& view) const {
        return stddev_kernel(view.close(), n_);
    }

    std::vector<double> StdDevEMAIndicator::compute(const CandleSeries& series) const {
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> StdDevEMAIndicator::compute(const SeriesView& view) const {
        return stddev_ema_kernel(view.close(), n_);
    }

} // namespace sugar
