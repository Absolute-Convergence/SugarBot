#include "indicators_sma.h"
#include <limits>
#include <numeric>


namespace sugar {


	static inline double qnan() {																	// File-local helper returning a quiet NaN for double,
																										// static = internal linkage (only this .cpp), 
																										
		return std::numeric_limits<double>::quiet_NaN();											// returns numeric_limits<T> member function quiet_NaN() for doubles,
																										// quiet_NaN() from <limits> gives a quiet NaN sentinel for double
	}																								

	std::vector<double> SMAIndicator::compute(const CandleSeries& series) const {					// inherited function 'compute' in daughter class SMAIndicator, 
																										// takes an immutable const ref CandleSeries parameter, 
																										// returns vector of doubles, Override of Indicator::compute; const must match the header declaration.
																										// Note: override keyword appears only in the class (header), not here.

		const auto& v = series.closes();															// Copy closes by value (safe, cheap with RVO); keep local view read-only
		std::vector<double> out(v.size(), qnan());													// vector count–value constructor: (make a vector of v.size() elements, each initialized to the value qnan()),
																										// pre-size output to input length; seed all positions with NaN (warm-up gaps)

		const std::size_t n = n_;																	// local copy of configured period (member n_ set via ctor)
		if (n == 0 || v.size() < n) return out;														// guard: no valid SMA if period is 0 or insufficient data -> return NaN-filled out

		double window_sum = std::accumulate(v.begin(), v.begin() + n, 0.0);							// sum the first n closes to seed the sliding window
		out[n - 1] = window_sum / static_cast<double>(n);											// first defined SMA is at index n-1
		for (std::size_t i = n; i < v.size(); ++i) {												// slide the window in O(1): add v[i], remove v[i-n], write SMA at i
			window_sum += v[i] - v[i - n];															// update rolling sum and write current SMA
			out[i] = window_sum / static_cast<double>(n);											// update rolling sum and write current SMA
		}
		return out;																					// return index-aligned SMA series (NaNs for warm-up region)
	}


	std::vector<double> sma_over_series(const std::vector<double>& v, std::size_t n) {				// 
		std::vector<double> out(v.size(), qnan());													// 
		if (n == 0 || v.size() < n) return out;														// 

		double window_sum = std::accumulate(v.begin(), v.begin() + n, 0.0);							// 
		out[n - 1] = window_sum / static_cast<double>(n);											// 

		for (std::size_t i = n; i < v.size(); ++i) {												// 
			window_sum += v[i] - v[i - n];															// 
			out[i] = window_sum / static_cast<double>(n);											// 
		}
		return out;																					// 
	}



} // namespace sugar