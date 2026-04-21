#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ingest/index_schema.h"
#include "ingest/locality_subject.h"
#include "ingest/topology_spec.h"
#include "sweep/plan_spec.h"

namespace sugar {

    struct CompileEnvStorage;
    class DataView;

    enum class OpType : std::uint8_t { Numeric = 0, Bool = 1 };
    enum class ValueType : std::uint8_t { Num = 0, Bool = 1 };
    enum class OpAxisScope : std::uint8_t {
        RowWise = 0,
        TimeSeries = 1,
        CrossSeries = 2,
        SpatialLocal = 3,
        SpatioTemporal = 4,
    };
    using ResolvedParam = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string>;
    using ResolvedParamMap = std::unordered_map<std::string, ResolvedParam>;

    struct OpParamSpec {
        std::string name;
        std::string type;
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
        std::string warmup_desc;
        OpAxisScope axis_scope{ OpAxisScope::RowWise };
        std::vector<AxisKind> required_axes{};
        std::vector<TopologyFamily> required_topology_families{};
        std::vector<TopologyQueryKind> required_topology_query_kinds{};
        std::string default_topology_name{};
        std::string topology_binding_param{};
        LocalitySubjectKind default_locality_subject_kind{ LocalitySubjectKind::PrimarySeries };
        std::string default_locality_subject_value{};
        LocalitySubjectKind bound_locality_subject_kind{ LocalitySubjectKind::SeriesKey };
        std::string locality_subject_binding_param{};
    };

    struct CompileEnv {
        std::shared_ptr<const CompileEnvStorage> storage{};

        bool has_storage() const { return static_cast<bool>(storage); }
    };

    struct OpEntry {
        OpSpec spec;
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

        std::string describe_text(bool include_modules_header = true) const;
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

    bool resolve_bound_topology_name(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        std::string& out,
        std::string& err);

    bool resolve_bound_topology_name(const OpEntry& op,
        const ResolvedParamMap& params,
        std::string& out,
        std::string& err);

    bool resolve_bound_locality_subject(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        ResolvedLocalitySubject& out,
        std::string& err);

    bool resolve_bound_locality_subject(const OpEntry& op,
        const ResolvedParamMap& params,
        ResolvedLocalitySubject& out,
        std::string& err);

    bool resolve_runtime_locality_subject(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        const DataView& dv,
        std::string_view compatibility_primary_series,
        RuntimeLocalitySubject& out,
        std::string& err);

    bool resolve_runtime_locality_subject(const OpEntry& op,
        const ResolvedParamMap& params,
        const DataView& dv,
        std::string_view compatibility_primary_series,
        RuntimeLocalitySubject& out,
        std::string& err);

    std::optional<double> as_f64(const ResolvedParamMap& m, const std::string& key);
    std::optional<std::int64_t> as_i64(const ResolvedParamMap& m, const std::string& key);
    std::optional<bool> as_bool(const ResolvedParamMap& m, const std::string& key);
    std::optional<std::string> as_str(const ResolvedParamMap& m, const std::string& key);

} // namespace sugar
