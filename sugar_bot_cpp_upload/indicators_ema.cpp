#include "indicators_ema.h"
#include <numeric> 
#include <limits>
#include <cmath>


namespace sugar {

	
	static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }							// file-local helper returning quiet NaN sentinel (marks "not yet defined" values).
																													// static = internal linkage; inline optional here

	std::vector<double> EMAIndicator::compute(const CandleSeries& series) const {								// Override of Indicator::compute; trailing const must match header declaration,
																													// returns vector of doubles, accepts CandleSeries by const ref,
																													// Note: override keyword appears only in the class (header), not here

		const auto& v = series.closes();																		// extract closing data from series, store in v
		std::vector<double> out(v.size(), qnan());																// call vector constructor, reserve v.size() elements, fill with quiet_NaN
		const std::size_t n = n_;																				// local copy of private data member n_
		if (n == 0 || v.empty()) return out;																	// guard: empty series param exit early (prevents undefined math)


		const double alpha = 2.0 / (static_cast<double>(n) + 1.0);												// alpha -> α = 2/(n+1)​, gives the decay rate; bigger n -> more smoothing
																														// Formula: EMA_t = α * x_t + (1-α) * EMA_{t-1}
																														// x_t is the close price at index t
																														// equivalent to exponentially weighted sum over past closes

		// normal case: series length>= n (it usually is) -> compute SMA seed, then roll EMA
		if (v.size() >= n) {																					
			double seed = 0.0;																					// seed = average of first n closes, stored at index n-1
			for (std::size_t i = 0; i < n; ++i) seed += v[i];													// sum through n - 1 terms in the Candle Series (v)
			seed /= static_cast<double>(n);																		// generate the first valid value for the EMA, store in seed 
			out[n - 1] = seed;																					// initialize the first valid EMA value in out[n - 1]
			for (std::size_t i = n; i < v.size(); ++i)															// loop through the rest of v
				out[i] = alpha * v[i] + (1.0 - alpha) * out[i - 1];												// Recurrence: out[i] = α*v[i] + (1-α)*out[i-1] for i >= n to end
		}
																												// short series fallback 
		else {																									
			out[0] = v[0];																						// seed EMA with first close, then apply recurrence for remaining bars
			for (std::size_t i = 1; i < v.size(); ++i)															// iterate for all the data given
				out[i] = alpha * v[i] + (1.0 - alpha) * out[i - 1];												// generates a usable "best effort" EMA without an SMA seed,
																													// Note: this is an edge case usually only done in broad parameter sweeps. 
		}
		return out;																								// return index-aligned EMA; NaNs mark warm-up region where no seed yet exists
	}

	std::vector<double> ema_over_series(const std::vector<double>& v, std::size_t n) {							// 
		std::vector<double> out(v.size(), qnan());																// 
		if (n == 0 || v.size() < n) return out;																	// 

		const double alpha = 2.0 / (static_cast<double>(n) + 1.0);												// 

		double seed_sum = std::accumulate(v.begin(), v.begin() + n, 0.0);										//
		double seed = seed_sum / static_cast<double>(n);														//
		out[n - 1] = seed;																						//

		// Continue EMA from index n
		for (std::size_t i = n; i < v.size(); ++i) {															// 
			out[i] = alpha * v[i] + (1.0 - alpha) * out[i - 1];													// 
		}
		return out;																								// 
	}

} // namespace sugar