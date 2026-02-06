#pragma once
#include <cstddef>
#include <ostream>
#include <string_view>
#include <cstdint>

namespace sugar {

	struct BacktestResult {
		double pnl{};											// cumulative % return across closed trades
		std::size_t trades{};									// round-trip trades
		double max_drawdown{};									// max peak-to-trough drawdown (%)
		int best_start_date{};									// first usable signal date (YYYYMMDD)

		std::size_t long_trades{};								// 
		std::size_t short_trades{};								// 

		double pnl_long{};										// sum of % from long trades
		double pnl_short{};										// sum of % from short trades
		std::uint64_t trade_hash{};								// behavior signature
	};

	// Canonical way to print a BacktestResult so all reporting is consistent.
	inline void print_backtest_summary(const BacktestResult& r,
		std::ostream& os,
		std::string_view indent = {})
	{
		os << indent << "PnL: " << r.pnl << "%\n"
			<< indent << "Trades: " << r.trades << "\n"
			<< indent << "Max DD: " << r.max_drawdown << "%\n"
			<< indent << "Trade hash: " << r.trade_hash << "\n";

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
