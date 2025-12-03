#pragma once
#include "strategy.h"


namespace sugar {


	class Backtester {																//
	public:																			//
		BacktestResult run(IStrategy& strategy, const CandleSeries& data) {			//
			return strategy.run(data);												//
		}
	};


} // namespace sugar