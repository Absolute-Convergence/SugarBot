#pragma once
#include <vector>
#include "candle.h"
#include <span>


namespace sugar {																				
																								// adding more to namespace::sugar
																								
	class CandleSeries {																		// stores the data loaded from load_candles_csv
	public:																						// accessors
		CandleSeries() = default;																// set constructor to default

		// parameter for Pass By Value class object initialization (member initializer list) moves into 'rows'
		explicit CandleSeries(std::vector<Candle> rows)
			: rows_(std::move(rows))															// initialize rows_ by moving the passed vector in
		{
			// constructor body, ie: can now use rows_
			closes_.reserve(rows_.size());														// reserves vector with rows_.size() elements
			for (const auto& c : rows_) {														// 
				closes_.push_back(c.close);														//
			}
		}
																								
		const std::vector<Candle>& rows() const { 
			return rows_; }																		// CandleSeries member function to return a read only const ref to data member 'rows_'

		const std::vector<double>& volumes() const noexcept {
			return volumes_;
		}

		std::size_t size() const { 
			return rows_.size();																// CandleSeries member function to return a size_t copy of vector::rows_.size() by value, will not mutate (const)
		}										

		// return cached closes by const reference
		const std::vector<double>& closes() const noexcept {
			return closes_;
		}

		const Candle& operator[](std::size_t i) const { return rows_[i]; }						// overload the [] operator to return an indexed candle value.


	private:																					
		std::vector<Candle> rows_;																// CandleSeries (only) Data member
		std::vector<double> closes_;															// cached close prices
		std::vector<double> volumes_;
	};


} // namespace sugar
