#include "indicators_atr.h"
#include "atr_kernels.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace sugar {

    std::vector<double> ATRIndicator::compute(const CandleSeries& series) const {
        // Keep wrapper API stable while delegating ATR math to canonical kernels.
        return atr_over_tr(true_range_over_series(series), n_);
    }

    std::vector<double> ATRIndicator::compute(const SeriesView& view) const {
        const double qnan = std::numeric_limits<double>::quiet_NaN();

        // Close-only views use generic TR kernel (absolute delta) by contract.
        if (!view.has_high() || !view.has_low()) {
            const std::vector<double> close(view.close().begin(), view.close().end());
            return atr_over_series(close, n_);
        }

        const auto high = view.high();
        const auto low = view.low();
        const auto close = view.close();
        std::vector<double> tr(close.size(), qnan);
        if (close.empty()) return tr;

        const double h0 = high[0];
        const double l0 = low[0];
        if (std::isfinite(h0) && std::isfinite(l0)) {
            tr[0] = h0 - l0;
        }

        for (std::size_t i = 1; i < close.size(); ++i) {
            const double h = high[i];
            const double l = low[i];
            const double pc = close[i - 1];
            if (!std::isfinite(h) || !std::isfinite(l) || !std::isfinite(pc)) {
                continue;
            }
            const double hl = h - l;
            const double hc = std::fabs(h - pc);
            const double lc = std::fabs(l - pc);
            tr[i] = std::max({ hl, hc, lc });
        }

        return atr_over_tr(tr, n_);
    }

} // namespace sugar
