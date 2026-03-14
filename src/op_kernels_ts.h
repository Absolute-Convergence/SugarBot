#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sugar::op_kernels {

std::vector<double> ts_add(std::span<const double> a, std::span<const double> b);
std::vector<double> ts_sub(std::span<const double> a, std::span<const double> b);
std::vector<double> ts_mul(std::span<const double> a, std::span<const double> b);
std::vector<double> ts_div(std::span<const double> a, std::span<const double> b);
std::vector<double> ts_min(std::span<const double> a, std::span<const double> b);
std::vector<double> ts_max(std::span<const double> a, std::span<const double> b);

std::vector<double> ts_neg(std::span<const double> x);
std::vector<double> ts_abs(std::span<const double> x);
std::vector<double> ts_log(std::span<const double> x);
std::vector<double> ts_exp(std::span<const double> x);
std::vector<double> ts_sqrt(std::span<const double> x);
std::vector<double> ts_pow_const(std::span<const double> x, double p);

std::vector<double> ts_shift(std::span<const double> x, std::size_t k, double fill);
std::vector<double> ts_delta(std::span<const double> x);

std::vector<double> ts_clamp_const(std::span<const double> x, double lo, double hi);
std::vector<double> ts_clamp(std::span<const double> x, std::span<const double> lo, std::span<const double> hi);
std::vector<double> ts_normalize_minmax_global(std::span<const double> x);
std::vector<double> ts_ratio_of_ma(std::span<const double> close, std::span<const double> ma);
std::vector<double> ts_roc(std::span<const double> x, std::size_t k);
std::vector<double> ts_sma(std::span<const double> x, std::size_t n);
std::vector<double> ts_ema(std::span<const double> x, std::size_t n);
std::vector<double> ts_roll_max(std::span<const double> x, std::size_t n);
std::vector<double> ts_roll_min(std::span<const double> x, std::size_t n);
std::vector<double> ts_donchian_upper(std::span<const double> x, std::size_t n);
std::vector<double> ts_donchian_lower(std::span<const double> x, std::size_t n);
std::vector<double> ts_donchian_mid(std::span<const double> x, std::size_t n);
std::vector<double> ts_donchian_width(std::span<const double> x, std::size_t n);
std::vector<double> ts_normalize_minmax(std::span<const double> x, std::size_t n);
std::vector<double> ts_true_range_ohlc(std::span<const double> high, std::span<const double> low, std::span<const double> close);
std::vector<double> ts_true_range_delta(std::span<const double> x);
std::vector<double> ts_atr_wilder(std::span<const double> tr, std::size_t n);
std::vector<double> ts_atr_from_series(std::span<const double> x, std::size_t n);
std::vector<double> ts_rsi_wilder(std::span<const double> close, std::size_t n);

} // namespace sugar::op_kernels
