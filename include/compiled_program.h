#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "op_registry.h"

namespace sugar {

    using NumericBuffer = std::vector<double>;
    using BoolBuffer = std::vector<std::uint8_t>;

    inline constexpr std::uint8_t kBoolFalse = 0u;
    inline constexpr std::uint8_t kBoolTrue = 1u;
    inline constexpr std::size_t kInvalidProgramSlot = static_cast<std::size_t>(-1);

    enum class ProgramOutputShape : std::uint8_t {
        RowWiseBuffer = 0,
    };

    enum class ProgramKernelAccess : std::uint8_t {
        RandomAccessAlignedRows = 0,
    };

    enum class ProgramScheduling : std::uint8_t {
        TopologicalNodeOrder = 0,
    };

    // Stable compiled-program contract:
    // - Every slot is a row-aligned buffer over the active DataView row domain.
    // - Scalar params/constants are broadcast into row buffers by their producing node.
    // - Missing numeric inputs are represented as NaN in aligned buffers.
    // - Missing bool inputs are represented as 0/false.
    // - Cross-entity addressing is resolved before evaluation through bound leaves/ColumnRef wiring.
    // - Warmup/lookback is expressed by NaN/false prefixes plus StrategyPlan.first_valid_index.
    // - Kernels may random-access aligned buffers but must remain pure and deterministic.
    // - Node order is already topological and defines deterministic execution order.
    // - Output buffers are evaluator-owned for a single run; reuse is an evaluator implementation detail.
    struct ProgramInputRef {
        ValueType type{ ValueType::Num };
        std::size_t slot{ 0 };
    };

    struct NumericProgramNode {
        std::string op_name{};
        std::vector<ProgramInputRef> inputs{};
        ResolvedParamMap params{};
        std::size_t output_slot{ 0 };
    };

    struct BoolProgramNode {
        std::string op_name{};
        std::vector<ProgramInputRef> inputs{};
        ResolvedParamMap params{};
        std::size_t output_slot{ 0 };
    };

    struct CompiledProgram {
        ProgramOutputShape output_shape{ ProgramOutputShape::RowWiseBuffer };
        ProgramKernelAccess kernel_access{ ProgramKernelAccess::RandomAccessAlignedRows };
        ProgramScheduling scheduling{ ProgramScheduling::TopologicalNodeOrder };
        std::vector<NumericProgramNode> numeric_nodes{};
        std::vector<BoolProgramNode> bool_nodes{};
        std::size_t numeric_slot_count{ 0 };
        std::size_t bool_slot_count{ 0 };
        std::size_t entry_long_slot{ kInvalidProgramSlot };
        std::size_t exit_long_slot{ kInvalidProgramSlot };
        std::size_t entry_short_slot{ kInvalidProgramSlot };
        std::size_t exit_short_slot{ kInvalidProgramSlot };
        std::size_t allow_trade_slot{ kInvalidProgramSlot };
    };

} // namespace sugar
