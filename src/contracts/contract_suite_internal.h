#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ingest/align.h"
#include "ingest/data_spec.h"
#include "ingest/data_view.h"
#include "ingest/numeric_frame.h"
#include "ops/op_registry.h"
#include "sweep/plan_spec.h"
#include "engine/strategy_executor.h"
#include "legacy/series.h"

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
    std::filesystem::path contract_tmp_case_dir(std::string_view tag);
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
    bool test_csv_row_skip_diagnostics_contracts(ContractAsserts& t);
    bool test_json_unicode_policy_contracts(ContractAsserts& t);
    bool test_service_invalid_request_rejection_contracts(ContractAsserts& t);
    bool test_service_upload_path_abuse_contracts(ContractAsserts& t);
    bool test_service_session_object_probe_abuse_contracts(ContractAsserts& t);
    bool test_service_session_upload_quota_abuse_contracts(ContractAsserts& t);
    bool test_service_expensive_sweep_policy_abuse_contracts(ContractAsserts& t);
    bool test_service_result_ownership_contracts(ContractAsserts& t);
    bool test_service_cancellation_contracts(ContractAsserts& t);
    bool test_service_running_cancellation_contracts(ContractAsserts& t);
    bool test_service_artifact_sandbox_cleanup_contracts(ContractAsserts& t);
    bool test_service_local_request_replay_contracts(ContractAsserts& t);
    bool test_service_scheduler_fairness_contracts(ContractAsserts& t);
    bool test_service_background_worker_pool_contracts(ContractAsserts& t);
    bool test_service_background_worker_cancellation_contracts(ContractAsserts& t);
    bool test_service_cross_client_isolation_contracts(ContractAsserts& t);
    bool test_service_scheduler_starvation_resistance_contracts(ContractAsserts& t);
    bool test_service_multi_client_stress_contracts(ContractAsserts& t);
    bool test_service_server_backpressure_contracts(ContractAsserts& t);
    bool test_service_protocol_framing_contracts(ContractAsserts& t);
    bool test_service_protocol_dispatch_contracts(ContractAsserts& t);
    bool test_service_protocol_network_socket_contracts(ContractAsserts& t);
    bool test_service_protocol_live_client_run_contracts(ContractAsserts& t);
    bool test_service_protocol_live_multi_client_contracts(ContractAsserts& t);
    bool test_service_protocol_live_cancel_polling_contracts(ContractAsserts& t);
    bool test_service_protocol_remote_bind_policy_contracts(ContractAsserts& t);
    bool test_service_protocol_slow_client_resilience_contracts(ContractAsserts& t);
    bool test_dataspec_input_profile_contract(ContractAsserts& t);
    bool test_iso8601_timekeys(ContractAsserts& t);
    bool test_alignment_calendar_policy_contracts(ContractAsserts& t);
    bool test_alignment_schema_validation_contracts(ContractAsserts& t);
    bool test_alignment_non_temporal_design_load_contracts(ContractAsserts& t);
    bool test_alignment_non_temporal_policy_contracts(ContractAsserts& t);
    bool test_alignment_policy_metadata_preservation_contracts(ContractAsserts& t);
    bool test_row_storage_numeric_domain_contracts(ContractAsserts& t);
    bool test_row_storage_spatial_runtime_contracts(ContractAsserts& t);
    bool test_index_schema_propagation(ContractAsserts& t);
    bool test_topology_catalog_identity_contracts(ContractAsserts& t);
    bool test_topology_sidecar_mounting_contracts(ContractAsserts& t);
    bool test_topology_manifest_provenance_contracts(ContractAsserts& t);
    bool test_topology_legacy_compat_provenance_contracts(ContractAsserts& t);
    bool test_spatial_only_preprocess_contracts(ContractAsserts& t);
    bool test_observation_capability_spatial_surface_contracts(ContractAsserts& t);
    bool test_observation_capability_temporal_extension_contracts(ContractAsserts& t);
    bool test_observation_capability_row_domain_contracts(ContractAsserts& t);
    bool test_plan_capability_fact_contracts(ContractAsserts& t);
    bool test_plan_capability_runtime_binding_contracts(ContractAsserts& t);
    bool test_plan_capability_runtime_topology_contracts(ContractAsserts& t);
    bool test_plan_capability_runtime_locality_contracts(ContractAsserts& t);
    bool test_multi_entity_ref_binding_overload_rejection_contracts(ContractAsserts& t);
    bool test_candle_adapter_contracts(ContractAsserts& t);
    bool test_kernel_warmup_contracts(ContractAsserts& t);
    bool test_axis_domain_runtime_contracts(ContractAsserts& t);
    bool test_axis_query_temporal_contracts(ContractAsserts& t);
    bool test_axis_query_spatial_contracts(ContractAsserts& t);
    bool test_axis_query_spatiotemporal_contracts(ContractAsserts& t);
    bool test_topology_design_load_contracts(ContractAsserts& t);
    bool test_topology_overload_rejection_contracts(ContractAsserts& t);
    bool test_topology_metadata_preservation_contracts(ContractAsserts& t);
    bool test_locality_design_load_contracts(ContractAsserts& t);
    bool test_locality_overload_rejection_contracts(ContractAsserts& t);
    bool test_locality_metadata_preservation_contracts(ContractAsserts& t);
    bool test_multi_entity_runner_design_load_contracts(ContractAsserts& t);
    bool test_multi_entity_runner_overload_rejection_contracts(ContractAsserts& t);
    bool test_operator_axis_contracts(ContractAsserts& t);
    bool test_planspec_compile_determinism(ContractAsserts& t);
    bool test_compile_warmup_semantics_contracts(ContractAsserts& t);
    bool test_executor_determinism(ContractAsserts& t);
    bool test_planspec_axis_validation(ContractAsserts& t);
    bool test_compile_bridge_design_load_contracts(ContractAsserts& t);
    bool test_compile_bridge_overload_rejection_contracts(ContractAsserts& t);
    bool test_compile_bridge_registry_surface_contracts(ContractAsserts& t);
    bool test_compile_bridge_temporal_runtime_contracts(ContractAsserts& t);
    bool test_compile_bridge_spatial_runtime_contracts(ContractAsserts& t);
    bool test_compile_bridge_subject_routing_contracts(ContractAsserts& t);
    bool test_compile_bridge_spatiotemporal_runtime_contracts(ContractAsserts& t);
    bool test_compile_bridge_sweep_runtime_contracts(ContractAsserts& t);
    bool test_compile_bridge_failure_path_contracts(ContractAsserts& t);
    bool test_target_resolution_design_load_contracts(ContractAsserts& t);
    bool test_target_resolution_overload_rejection_contracts(ContractAsserts& t);
    bool test_numeric_portability_contracts(ContractAsserts& t);
    bool test_rowwise_runtime_branching_contracts(ContractAsserts& t);
    bool test_ts_alias_runtime_parity_contracts(ContractAsserts& t);
    bool test_ts_runtime_equivalence_smoke_contracts(ContractAsserts& t);
    bool test_assertion_hygiene_contracts(ContractAsserts& t);
    bool test_runtime_no_legacy_composite_include(ContractAsserts& t);

} // namespace sugar::contract_suite_internal
