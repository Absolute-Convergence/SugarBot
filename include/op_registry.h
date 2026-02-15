#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <iosfwd>

#include "indicators_composite_numeric.h"
#include "indicators_composite_bool.h"
#include "indicator_precomputed.h"
#include "signals_precomputed.h"
#include "plan_spec.h" // ParamMap/ParamValue

namespace sugar {

    enum class OpType : std::uint8_t { Numeric = 0, Bool = 1 };
    enum class ValueType : std::uint8_t { Num = 0, Bool = 1 };

    using ResolvedParam = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;
    using ResolvedParamMap = std::unordered_map<std::string, ResolvedParam>;

    struct OpParamSpec {
        std::string name;
        std::string type; // "int", "float", "bool", "string"
        bool required{ false };
        std::optional<ResolvedParam> default_value;
        std::optional<double> min_num;
        std::optional<double> max_num;
    };

    struct OpSpec {
        std::string name;
        OpType op_type{ OpType::Numeric };
        std::vector<ValueType> inputs;
        ValueType output{ ValueType::Num };
        std::vector<OpParamSpec> params;

        std::string module;
        bool is_macro{ false };

        // Human/AI-facing warmup description (since lambdas aren't introspectable).
        // Example: "max(child_warmups) + (n-1)"
        std::string warmup_desc;
    };

    struct CompileEnv {
        std::unordered_map<std::string, IndicatorPtr> numeric_leaves;
        std::unordered_map<std::string, BoolSignalPtr> bool_leaves;
    };

    struct OpEntry {
        OpSpec spec;

        std::function<IndicatorPtr(const std::vector<IndicatorPtr>&, const ResolvedParamMap&, const CompileEnv&)> build_num;
        std::function<BoolSignalPtr(const std::vector<BoolSignalPtr>&,
            const std::vector<IndicatorPtr>&,
            const ResolvedParamMap&, const CompileEnv&)> build_bool;

        std::function<std::size_t(const std::vector<std::size_t>& child_warmups, const ResolvedParamMap&)> warmup_num;
        std::function<std::size_t(const std::vector<std::size_t>& bool_child_warmups,
            const std::vector<std::size_t>& num_child_warmups,
            const ResolvedParamMap&)> warmup_bool;
    };

    class OperatorRegistry {
    public:
        bool has(const std::string& op_name) const;
        const OpEntry* get(const std::string& op_name) const;

        std::vector<std::string> list_ops() const;
        std::vector<std::string> list_modules() const;

        void add(OpEntry e);

        // ---- NEW: AI reference manual dump ----
        // Returns a stable, human/AI-readable text describing all available ops.
        // This is intentionally "capabilities of this build", so missing indicator packs
        // simply won't appear.
        std::string describe_text(bool include_modules_header = true) const;

        // Writes the same description to a stream.
        void print_description(std::ostream& os, bool include_modules_header = true) const;

    private:
        std::unordered_map<std::string, OpEntry> ops_;
    };

    OperatorRegistry make_default_registry();

    bool resolve_and_validate_params(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        ResolvedParamMap& out,
        std::string& err);

    std::optional<double> as_f64(const ResolvedParamMap& m, const std::string& key);
    std::optional<std::int64_t> as_i64(const ResolvedParamMap& m, const std::string& key);
    std::optional<bool> as_bool(const ResolvedParamMap& m, const std::string& key);
    std::optional<std::string> as_str(const ResolvedParamMap& m, const std::string& key);

} // namespace sugar
