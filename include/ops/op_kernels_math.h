#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sugar::op_kernels_math {

std::vector<double> rolling_max(std::span<const double> x, std::size_t n);
std::vector<double> rolling_min(std::span<const double> x, std::size_t n);

std::vector<double> sma(std::span<const double> x, std::size_t n);
std::vector<double> ema(std::span<const double> x, std::size_t n);
std::vector<double> roc(std::span<const double> x, std::size_t k);

std::vector<double> stddev(std::span<const double> x, std::size_t n);
std::vector<double> stddev_ema(std::span<const double> x, std::size_t n);

std::vector<double> zscore_sma(std::span<const double> x, std::size_t n);
std::vector<double> zscore_ema(std::span<const double> x, std::size_t n);

} // namespace sugar::op_kernels_math
