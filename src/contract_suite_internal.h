#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "align.h"
#include "data_spec.h"
#include "data_view.h"
#include "numeric_frame.h"
#include "op_registry.h"
#include "plan_spec.h"
#include "series.h"
#include "strategy_executor.h"

namespace sugar::contract_suite_internal {

    struct ContractAsserts {
        explicit ContractAsserts(std::ostream& o) : out(o) {}

        bool expect(bool ok, const std::string& label);

        int passed{ 0 };
        int failed{ 0 };
        std::ostream& out;
    };

    struct ContractSection {
        const char* key;
        const char* label;
        bool (*run)(ContractAsserts&);
    };

    struct ContractSectionGroup {
        const char* key;
        const char* label;
        std::vector<const char*> members;
    };

    bool nearly_equal(double a, double b, double eps = 1e-10);
    bool equal_or_both_nonfinite(double a, double b, double eps = 1e-10);
    std::size_t first_finite_index(const std::vector<double>& v);
    std::string normalize_section_key(std::string s);

    std::string param_value_to_string(const ParamValue& v);
    std::string resolved_param_to_string(const ResolvedParam& v);
    std::string canonical_graph_signature(const PlanSpec& spec);
    bool build_resolved_binding_signature(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        std::string& out_sig,
        std::string& err);

    bool write_text_file(const std::filesystem::path& p, const std::string& s);
    bool read_text_file(const std::filesystem::path& p, std::string& out);
    std::filesystem::path contract_tmp_dir();
    bool make_single_series_dataview_from_close(
        std::span<const double> close,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time = static_cast<TimeKey>(20240101));
    bool make_single_series_dataview_from_frame(
        NumericFrame frame,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out);

    CandleSeries make_simple_candles(std::size_t n);
    bool backtest_equal(const BacktestResult& a, const BacktestResult& b);
    bool run_contract_section_group(
        ContractAsserts& t,
        const ContractSectionGroup& group,
        const std::vector<ContractSection>& sections);

    bool test_preprocess_missing_policy(ContractAsserts& t);
    bool test_dataspec_input_profile_contract(ContractAsserts& t);
    bool test_iso8601_timekeys(ContractAsserts& t);
    bool test_alignment_contracts(ContractAsserts& t);
    bool test_row_storage_contracts(ContractAsserts& t);
    bool test_index_schema_propagation(ContractAsserts& t);
    bool test_topology_contracts(ContractAsserts& t);
    bool test_spatial_only_preprocess_contracts(ContractAsserts& t);
    bool test_observation_capability_contracts(ContractAsserts& t);
    bool test_plan_capability_contracts(ContractAsserts& t);
    bool test_candle_adapter_contracts(ContractAsserts& t);
    bool test_kernel_warmup_contracts(ContractAsserts& t);
    bool test_axis_domain_runtime_contracts(ContractAsserts& t);
    bool test_axis_query_runtime_contracts(ContractAsserts& t);
    bool test_topology_design_load_contracts(ContractAsserts& t);
    bool test_topology_overload_rejection_contracts(ContractAsserts& t);
    bool test_topology_metadata_preservation_contracts(ContractAsserts& t);
    bool test_locality_design_load_contracts(ContractAsserts& t);
    bool test_locality_overload_rejection_contracts(ContractAsserts& t);
    bool test_locality_metadata_preservation_contracts(ContractAsserts& t);
    bool test_operator_axis_contracts(ContractAsserts& t);
    bool test_planspec_compile_determinism(ContractAsserts& t);
    bool test_executor_determinism(ContractAsserts& t);
    bool test_planspec_axis_validation(ContractAsserts& t);
    bool test_compile_bridge_design_load_contracts(ContractAsserts& t);
    bool test_compile_bridge_overload_rejection_contracts(ContractAsserts& t);
    bool test_planspec_compile_bridge_runtime(ContractAsserts& t);
    bool test_ts_operator_smoke_matrix(ContractAsserts& t);
    bool test_runtime_no_legacy_composite_include(ContractAsserts& t);

} // namespace sugar::contract_suite_internal
