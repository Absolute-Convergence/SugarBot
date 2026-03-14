#include "rsi_kernels.h"

#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    namespace {

        inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

        std::vector<double> rsi_kernel(std::span<const double> close, std::size_t n) {
            std::vector<double> out(close.size(), qnan());
            if (n == 0 || close.size() <= n) {
                return out;
            }

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

    } // namespace

    std::vector<double> rsi_over_series(const std::vector<double>& v, std::size_t n) {
        return rsi_kernel(std::span<const double>(v.data(), v.size()), n);
    }

} // namespace sugar
