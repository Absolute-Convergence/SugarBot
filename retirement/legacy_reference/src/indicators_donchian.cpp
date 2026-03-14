#include "indicators_donchian.h"
#include <limits>
#include <deque>
#include <cmath>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    struct DonchianBands {
        std::vector<double> upper;
        std::vector<double> lower;
    };
    
    // ---------------------------------------------------------------------
    // Generic rolling extrema (vector-generic)
    //
    // Policy:
    // - Warm-up: indices < (n-1) are NaN; first defined at (n-1)
    // - Guard: n==0 or v.size() < n -> all NaN
    // - NaN handling: if a non-finite value is observed, the internal deque
    //   state is reset and outputs are NaN until we have n consecutive finite
    //   values again.
    // ---------------------------------------------------------------------

    static std::vector<double> rolling_extrema_over_series(
        const std::vector<double>& v,
        std::size_t n,
        bool want_max)
    {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) return out;

        std::deque<std::size_t> dq; // indices, monotonic by value
        std::size_t finite_run = 0; // consecutive finite samples since last reset

        for (std::size_t i = 0; i < v.size(); ++i) {
            const double x = v[i];
            if (!std::isfinite(x)) {
                dq.clear();
                finite_run = 0;
                out[i] = qnan();
                continue;
            }

            ++finite_run;

            if (want_max) {
                while (!dq.empty() && v[dq.back()] <= x) dq.pop_back();
            }
            else {
                while (!dq.empty() && v[dq.back()] >= x) dq.pop_back();
            }
            dq.push_back(i);

            const std::size_t window_start = (i + 1 >= n) ? (i + 1 - n) : 0;
            while (!dq.empty() && dq.front() < window_start) dq.pop_front();

            out[i] = (finite_run >= n) ? v[dq.front()] : qnan();
        }

        return out;
    }

    std::vector<double> rolling_max_over_series(const std::vector<double>& v, std::size_t n) {
        return rolling_extrema_over_series(v, n, /*want_max=*/true);
    }

    std::vector<double> rolling_min_over_series(const std::vector<double>& v, std::size_t n) {
        return rolling_extrema_over_series(v, n, /*want_max=*/false);
    }


    // Non-Generic DonChian Leaf indicator 
    // Compute upper/lower in one pass (O(N)) using monotonic deques.
    static DonchianBands donchian_bands_over_series(const CandleSeries& series, std::size_t n) {
        DonchianBands bands;
        bands.upper.assign(series.size(), qnan());
        bands.lower.assign(series.size(), qnan());

        if (n == 0 || series.size() < n) return bands;

        std::deque<std::size_t> dq_high; // indices, decreasing highs (front = max)
        std::deque<std::size_t> dq_low;  // indices, increasing lows  (front = min)

        for (std::size_t i = 0; i < series.size(); ++i) {
            const double hi = series[i].high;
            const double lo = series[i].low;

            // Maintain dq_high: pop while new hi >= back hi
            while (!dq_high.empty() && series[dq_high.back()].high <= hi) {
                dq_high.pop_back();
            }
            dq_high.push_back(i);

            // Maintain dq_low: pop while new lo <= back lo
            while (!dq_low.empty() && series[dq_low.back()].low >= lo) {
                dq_low.pop_back();
            }
            dq_low.push_back(i);

            // Evict indices that are out of window [i-n+1, i]
            const std::size_t window_start = (i + 1 >= n) ? (i + 1 - n) : 0;

            while (!dq_high.empty() && dq_high.front() < window_start) {
                dq_high.pop_front();
            }
            while (!dq_low.empty() && dq_low.front() < window_start) {
                dq_low.pop_front();
            }

            // Emit once we have a full window
            if (i + 1 >= n) {
                bands.upper[i] = series[dq_high.front()].high;
                bands.lower[i] = series[dq_low.front()].low;
            }
        }

        return bands;
    }

    std::vector<double> donchian_upper_over_series(const CandleSeries& series, std::size_t n) {
        return donchian_bands_over_series(series, n).upper;
    }

    std::vector<double> donchian_lower_over_series(const CandleSeries& series, std::size_t n) {
        return donchian_bands_over_series(series, n).lower;
    }

    std::vector<double> donchian_middle_over_series(const CandleSeries& series, std::size_t n) {
        const auto bands = donchian_bands_over_series(series, n);
        std::vector<double> out(series.size(), qnan());

        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!std::isfinite(bands.upper[i]) || !std::isfinite(bands.lower[i])) {
                out[i] = qnan();
            }
            else {
                out[i] = (bands.upper[i] + bands.lower[i]) * 0.5;
            }
        }

        return out;
    }

    std::vector<double> donchian_width_over_series(const CandleSeries& series, std::size_t n) {
        const auto bands = donchian_bands_over_series(series, n);
        std::vector<double> out(series.size(), qnan());

        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!std::isfinite(bands.upper[i]) || !std::isfinite(bands.lower[i])) {
                out[i] = qnan();
            }
            else {
                out[i] = bands.upper[i] - bands.lower[i];
            }
        }

        return out;
    }

    std::vector<double> DonchianIndicator::compute(const CandleSeries& series) const {
        switch (out_) {
        case DonchianOutput::Upper:  return donchian_upper_over_series(series, n_);
        case DonchianOutput::Lower:  return donchian_lower_over_series(series, n_);
        case DonchianOutput::Middle: return donchian_middle_over_series(series, n_);
        case DonchianOutput::Width:  return donchian_width_over_series(series, n_);
        default:                     return std::vector<double>(series.size(), qnan());
        }
    }

    std::vector<double> DonchianIndicator::compute(const SeriesView& view) const {
        const auto hi = view.has_high() ? view.high() : view.close();
        const auto lo = view.has_low() ? view.low() : view.close();

        const std::vector<double> hi_vec(hi.begin(), hi.end());
        const std::vector<double> lo_vec(lo.begin(), lo.end());

        const auto upper = rolling_max_over_series(hi_vec, n_);
        const auto lower = rolling_min_over_series(lo_vec, n_);

        switch (out_) {
        case DonchianOutput::Upper:
            return upper;
        case DonchianOutput::Lower:
            return lower;
        case DonchianOutput::Middle: {
            std::vector<double> out(view.size(), qnan());
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (!std::isfinite(upper[i]) || !std::isfinite(lower[i])) {
                    out[i] = qnan();
                }
                else {
                    out[i] = (upper[i] + lower[i]) * 0.5;
                }
            }
            return out;
        }
        case DonchianOutput::Width: {
            std::vector<double> out(view.size(), qnan());
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (!std::isfinite(upper[i]) || !std::isfinite(lower[i])) {
                    out[i] = qnan();
                }
                else {
                    out[i] = upper[i] - lower[i];
                }
            }
            return out;
        }
        default:
            return std::vector<double>(view.size(), qnan());
        }
    }

} // namespace sugar
