#pragma once
#include <cstddef>
#include <ostream>
#include <string_view>

namespace sugar {

	struct BacktestResult {
		double pnl{};											// cumulative % return across closed trades
		std::size_t trades{};									// round-trip trades
		double max_drawdown{};									// max peak-to-trough drawdown (%)
		int best_start_date{};									// first usable signal date (YYYYMMDD)

		std::size_t long_trades{};								// NEW
		std::size_t short_trades{};								// NEW

		double pnl_long{};										// NEW: sum of % from long trades
		double pnl_short{};										// NEW: sum of % from short trades
	};

	// Canonical way to print a BacktestResult so all reporting is consistent.
	inline void print_backtest_summary(const BacktestResult& r,
		std::ostream& os,
		std::string_view indent = {})
	{
		os << indent << "PnL: " << r.pnl << "%\n"
			<< indent << "Trades: " << r.trades << "\n"
			<< indent << "Max DD: " << r.max_drawdown << "%\n";

		if (r.best_start_date > 0) {
			os << indent
				<< "First usable signal date (YYYYMMDD): "
				<< r.best_start_date << "\n";
		}

		os << indent << "Long trades: " << r.long_trades
			<< ", Long PnL: " << r.pnl_long << "%\n"
			<< indent << "Short trades: " << r.short_trades
			<< ", Short PnL: " << r.pnl_short << "%\n";
	}

} // namespace sugar
