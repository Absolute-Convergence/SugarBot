#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sugar::op_kernels {

std::vector<double> ms_diff(std::span<const double> a, std::span<const double> b);
std::vector<double> ms_ratio(std::span<const double> a, std::span<const double> b);
std::vector<double> ms_zscore(std::span<const double> x, std::size_t n);

} // namespace sugar::op_kernels
