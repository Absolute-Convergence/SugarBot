#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sugar::op_kernels {

std::vector<double> core_select(std::span<const std::uint8_t> condition,
    std::span<const double> if_true,
    std::span<const double> if_false);

} // namespace sugar::op_kernels
