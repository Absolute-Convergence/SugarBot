#include "contracts/contract_suite.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>

#include "contract_suite_internal.h"

namespace sugar::contract_suite_internal {

    bool ContractAsserts::expect(bool ok, const std::string& label) {
        if (ok) {
            ++passed;
            out << "  [PASS] " << label << "\n";
        }
        else {
            ++failed;
            out << "  [FAIL] " << label << "\n";
        }
        return ok;
    }

    bool nearly_equal(double a, double b, double eps) {
        return std::fabs(a - b) <= eps;
    }

    bool equal_or_both_nonfinite(double a, double b, double eps) {
        if (!std::isfinite(a) && !std::isfinite(b)) return true;
        return nearly_equal(a, b, eps);
    }

    std::size_t first_finite_index(const std::vector<double>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (std::isfinite(v[i])) return i;
        }
        return v.size();
    }

    std::string normalize_section_key(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    }

    template <class T>
    std::string join_vec(const std::vector<T>& v, std::string_view sep = ",") {
        std::ostringstream oss;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) oss << sep;
            oss << v[i];
        }
        return oss.str();
    }

    std::string param_value_to_string(const ParamValue& v) {
        return std::visit([](const auto& x) -> std::string {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::nullptr_t>) return "null";
            else if constexpr (std::is_same_v<X, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<X, std::int64_t>) return "i64:" + std::to_string(x);
            else if constexpr (std::is_same_v<X, double>) {
                std::ostringstream oss;
                oss << "f64:" << std::setprecision(17) << x;
                return oss.str();
            }
            else if constexpr (std::is_same_v<X, std::string>) return "str:" + x;
            else return "ref:" + x.name;
        }, v);
    }

    std::string resolved_param_to_string(const ResolvedParam& v) {
        return std::visit([](const auto& x) -> std::string {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::nullptr_t>) return "null";
            else if constexpr (std::is_same_v<X, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<X, std::int64_t>) return "i64:" + std::to_string(x);
            else if constexpr (std::is_same_v<X, double>) {
                std::ostringstream oss;
                oss << "f64:" << std::setprecision(17) << x;
                return oss.str();
            }
            else return "str:" + x;
        }, v);
    }

    std::string canonical_graph_signature(const PlanSpec& spec) {
        auto node_sig = [](const NodeSpec& n) {
            std::vector<std::string> keys;
            keys.reserve(n.params.size());
            for (const auto& kv : n.params) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());

            std::ostringstream oss;
            oss << n.id << "|" << n.op << "|args:[" << join_vec(n.args, ",") << "]|params:{";
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i) oss << ",";
                const auto& k = keys[i];
                const auto it = n.params.find(k);
                oss << k << "=" << param_value_to_string(it->second);
            }
            oss << "}";
            return oss.str();
        };

        std::vector<std::string> num;
        num.reserve(spec.numeric_nodes.size());
        for (const auto& n : spec.numeric_nodes) num.push_back(node_sig(n));
        std::sort(num.begin(), num.end());

        std::vector<std::string> boo;
        boo.reserve(spec.bool_nodes.size());
        for (const auto& n : spec.bool_nodes) boo.push_back(node_sig(n));
        std::sort(boo.begin(), boo.end());

        std::vector<std::string> pkeys;
        pkeys.reserve(spec.params.size());
        for (const auto& kv : spec.params) pkeys.push_back(kv.first);
        std::sort(pkeys.begin(), pkeys.end());

        std::ostringstream oss;
        oss << "name=" << spec.name
            << "|version=" << spec.version
            << "|mode=" << static_cast<int>(spec.trade_mode)
            << "|params:{";
        for (std::size_t i = 0; i < pkeys.size(); ++i) {
            if (i) oss << ",";
            const auto& k = pkeys[i];
            oss << k << "=" << param_value_to_string(spec.params.at(k));
        }
        oss << "}|numeric:[" << join_vec(num, "||")
            << "]|bool:[" << join_vec(boo, "||")
            << "]|bind:{"
            << spec.bind.entry_long << ","
            << spec.bind.exit_long << ","
            << spec.bind.entry_short << ","
            << spec.bind.exit_short << ","
            << spec.bind.allow_trade
            << "}";
        return oss.str();
    }

    bool build_resolved_binding_signature(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        std::string& out_sig,
        std::string& err) {
        auto collect = [&](const std::vector<NodeSpec>& nodes, const char* kind, std::vector<std::string>& out) -> bool {
            std::vector<const NodeSpec*> ordered;
            ordered.reserve(nodes.size());
            for (const auto& n : nodes) ordered.push_back(&n);
            std::sort(ordered.begin(), ordered.end(),
                [](const NodeSpec* a, const NodeSpec* b) { return a->id < b->id; });

            for (const NodeSpec* np : ordered) {
                const auto* op = registry.get(np->op);
                if (!op) {
                    err = std::string("missing op while resolving ") + kind + " node '" + np->id + "': " + np->op;
                    return false;
                }
                ResolvedParamMap rpm;
                if (!resolve_and_validate_params(*op, np->params, spec.params, nullptr, rpm, err)) {
                    err = "resolve params failed for node '" + np->id + "': " + err;
                    return false;
                }

                std::vector<std::string> keys;
                keys.reserve(rpm.size());
                for (const auto& kv : rpm) keys.push_back(kv.first);
                std::sort(keys.begin(), keys.end());

                std::ostringstream oss;
                oss << kind << ":" << np->id << "|" << np->op << "|";
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i) oss << ",";
                    const auto& k = keys[i];
                    oss << k << "=" << resolved_param_to_string(rpm.at(k));
                }
                out.push_back(oss.str());
            }
            return true;
        };

        std::vector<std::string> sig_parts;
        if (!collect(spec.numeric_nodes, "num", sig_parts)) return false;
        if (!collect(spec.bool_nodes, "bool", sig_parts)) return false;
        std::sort(sig_parts.begin(), sig_parts.end());
        out_sig = join_vec(sig_parts, "||");
        return true;
    }

    bool write_text_file(const std::filesystem::path& p, const std::string& s) {
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f << s;
        return static_cast<bool>(f);
    }

    bool read_text_file(const std::filesystem::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::ostringstream oss;
        oss << f.rdbuf();
        out = oss.str();
        return true;
    }

    std::filesystem::path contract_tmp_dir() {
        return std::filesystem::temp_directory_path() / "sugar_contract_suite";
    }

    std::filesystem::path contract_tmp_case_dir(std::string_view tag) {
        static std::atomic<std::uint64_t> seq{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto n = seq.fetch_add(1, std::memory_order_relaxed) + 1;
        std::filesystem::path dir = contract_tmp_dir() /
            (std::string(tag) + "_" + std::to_string(stamp) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir);
        return dir;
    }

    bool make_single_series_dataview_from_frame(
        NumericFrame frame,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out) {
        ColumnId close_id{};
        if (!frame.schema().columns().find("close", close_id) || !frame.has_col(close_id)) {
            return false;
        }

        auto schema = std::make_shared<FrameSchema>(frame.schema());
        NumericFrame owned(*schema, frame.row_count());
        if (!frame.time().empty()) {
            owned = NumericFrame(*schema, std::vector<TimeKey>(frame.time().begin(), frame.time().end()));
        }
        owned.set_index_schema(frame.index_schema());

        for (std::size_t i = 0; i < schema->columns().size(); ++i) {
            const ColumnId id{ static_cast<std::uint32_t>(i) };
            if (!frame.has_col(id)) continue;
            const auto src = frame.col(id);
            auto dst = owned.ensure_col(id);
            std::copy(src.begin(), src.end(), dst.begin());
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.emplace_back("__single__", std::move(owned));

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        auto bundle = std::make_shared<align::AlignedBundle>(
            align::align_frames(schema, inputs, cfg));

        primary_out.series = "__single__";
        primary_out.col = close_id;
        bundle_out = std::move(bundle);
        return true;
    }

    bool make_single_series_dataview_from_close(
        std::span<const double> close,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time) {
        FrameSchema schema;
        std::vector<TimeKey> time;
        time.reserve(close.size());
        for (std::size_t i = 0; i < close.size(); ++i) {
            time.push_back(start_time + static_cast<TimeKey>(i));
        }

        NumericFrame frame(schema, std::move(time));
        frame.set_index_schema(IndexSchema::make_time_only());
        const ColumnId close_id = schema.columns().get_or_add("close");
        auto out = frame.ensure_col(close_id);
        std::copy(close.begin(), close.end(), out.begin());
        return make_single_series_dataview_from_frame(std::move(frame), bundle_out, primary_out);
    }

    CandleSeries make_simple_candles(std::size_t n) {
        std::vector<Candle> rows;
        rows.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double close = 100.0 + static_cast<double>(i);
            Candle c{};
            c.date = static_cast<TimeKey>(20240101 + static_cast<int>(i));
            c.open = close - 0.1;
            c.high = close + 0.2;
            c.low = close - 0.3;
            c.close = close;
            c.volume = 1000.0 + static_cast<double>(i);
            rows.push_back(c);
        }
        return CandleSeries(std::move(rows));
    }

    bool backtest_equal(const BacktestResult& a, const BacktestResult& b) {
        return nearly_equal(a.pnl, b.pnl) &&
            a.trades == b.trades &&
            nearly_equal(a.max_drawdown, b.max_drawdown) &&
            a.best_start_date == b.best_start_date &&
            a.has_best_start_date == b.has_best_start_date &&
            a.long_trades == b.long_trades &&
            a.short_trades == b.short_trades &&
            nearly_equal(a.pnl_long, b.pnl_long) &&
            nearly_equal(a.pnl_short, b.pnl_short) &&
            a.trade_hash == b.trade_hash;
    }

    bool run_contract_section_group(
        ContractAsserts& t,
        const ContractSectionGroup& group,
        const std::vector<ContractSection>& sections) {
        t.out << "[" << group.key << "] " << group.label << "\n";
        bool ok = true;
        for (const char* member_key : group.members) {
            const auto it = std::find_if(
                sections.begin(),
                sections.end(),
                [&](const ContractSection& section) {
                    return normalize_section_key(section.key) == normalize_section_key(member_key);
                });
            if (it == sections.end()) {
                ok = t.expect(false, std::string("group member section is registered: ") + member_key) && ok;
                continue;
            }
            ok = it->run(t) && ok;
        }
        return ok;
    }

} // namespace sugar::contract_suite_internal

namespace sugar {

    bool run_contract_suite(std::ostream& out, const ContractSuiteOptions& options) {
        using namespace contract_suite_internal;

        static const std::vector<ContractSection> kSections = {
            { "missing_policy", "preprocess missing token policies", &test_preprocess_missing_policy },
            { "csv_row_skip_diagnostics", "legacy/raw-table CSV row-skip diagnostics", &test_csv_row_skip_diagnostics_contracts },
            { "json_unicode_policy", "JSON unicode surrogate rejection policy", &test_json_unicode_policy_contracts },
            { "service_invalid_request_rejection", "service-core invalid request rejection", &test_service_invalid_request_rejection_contracts },
            { "service_result_ownership", "service-core result ownership", &test_service_result_ownership_contracts },
            { "service_cancellation", "service-core queued cancellation state transitions", &test_service_cancellation_contracts },
            { "service_running_cancellation", "service-core cooperative running-job cancellation", &test_service_running_cancellation_contracts },
            { "service_artifact_sandbox_cleanup", "service-core artifact sandboxing and cleanup", &test_service_artifact_sandbox_cleanup_contracts },
            { "service_local_request_replay", "service-core deterministic local request replay", &test_service_local_request_replay_contracts },
            { "service_scheduler_fairness", "service-core fair scheduler ordering", &test_service_scheduler_fairness_contracts },
            { "service_background_worker_pool", "service-core bounded background worker pool", &test_service_background_worker_pool_contracts },
            { "service_background_worker_cancellation", "service-core background worker cancellation propagation", &test_service_background_worker_cancellation_contracts },
            { "service_cross_client_isolation", "service-core cross-client ownership isolation under workers", &test_service_cross_client_isolation_contracts },
            { "service_scheduler_starvation_resistance", "service-core scheduler starvation resistance", &test_service_scheduler_starvation_resistance_contracts },
            { "service_multi_client_stress", "service-core higher-load multi-client stress", &test_service_multi_client_stress_contracts },
            { "service_server_backpressure", "service-core server-wide backpressure and recovery", &test_service_server_backpressure_contracts },
            { "service_protocol_framing", "service protocol framing and malformed-request handling", &test_service_protocol_framing_contracts },
            { "service_protocol_dispatch", "service protocol request dispatch surface", &test_service_protocol_dispatch_contracts },
            { "service_protocol_network_socket", "service protocol socket transport behavior", &test_service_protocol_network_socket_contracts },
            { "service_protocol_live_multi_client", "service protocol live multi-client TCP concurrency", &test_service_protocol_live_multi_client_contracts },
            { "service_protocol_live_cancel_polling", "service protocol live TCP cancel and status polling", &test_service_protocol_live_cancel_polling_contracts },
            { "dataspec_input_profile", "DataSpec input_profile contract", &test_dataspec_input_profile_contract },
            { "iso8601_timekeys", "ISO-8601 TimeKey fidelity", &test_iso8601_timekeys },
            { "alignment_calendar_policies", "alignment union/intersection calendar contracts", &test_alignment_calendar_policy_contracts },
            { "alignment_schema_validation", "alignment schema validation contracts", &test_alignment_schema_validation_contracts },
            { "alignment_non_temporal_design_loads", "alignment non-temporal design-load contracts", &test_alignment_non_temporal_design_load_contracts },
            { "alignment_non_temporal_policy", "alignment non-temporal policy rejection contracts", &test_alignment_non_temporal_policy_contracts },
            { "alignment_policy_metadata_preservation", "alignment policy metadata/provenance preservation", &test_alignment_policy_metadata_preservation_contracts },
            { "row_storage_numeric_domain", "row-domain NumericFrame/AlignedBundle storage contracts", &test_row_storage_numeric_domain_contracts },
            { "row_storage_spatial_runtime", "row-domain spatial-only runtime storage contracts", &test_row_storage_spatial_runtime_contracts },
            { "index_schema", "index schema propagation", &test_index_schema_propagation },
            { "topology_catalog_identity", "topology catalog identity and namespace contracts", &test_topology_catalog_identity_contracts },
            { "topology_sidecar_mounting", "topology sidecar mounting contracts", &test_topology_sidecar_mounting_contracts },
            { "topology_manifest_provenance", "topology manifest and runtime provenance contracts", &test_topology_manifest_provenance_contracts },
            { "topology_legacy_compat", "legacy embedded topology provenance contracts", &test_topology_legacy_compat_provenance_contracts },
            { "spatial_ingest", "spatial-only preprocess and ingest contracts", &test_spatial_only_preprocess_contracts },
            { "observation_capability_spatial_surfaces", "spatial observation-surface capability diagnosis", &test_observation_capability_spatial_surface_contracts },
            { "observation_capability_temporal_extensions", "temporal observation-surface extension diagnosis", &test_observation_capability_temporal_extension_contracts },
            { "observation_capability_row_domain", "row-domain observation capability diagnosis", &test_observation_capability_row_domain_contracts },
            { "plan_capability_facts", "fact-only plan/operator capability diagnosis", &test_plan_capability_fact_contracts },
            { "plan_capability_runtime_binding", "runtime-aware plan binding diagnosis", &test_plan_capability_runtime_binding_contracts },
            { "plan_capability_runtime_topology", "runtime-aware plan topology diagnosis", &test_plan_capability_runtime_topology_contracts },
            { "plan_capability_runtime_locality", "runtime-aware locality-subject diagnosis", &test_plan_capability_runtime_locality_contracts },
            { "multi_entity_ref_binding_overload_rejection", "multi-entity runtime binding overload rejection", &test_multi_entity_ref_binding_overload_rejection_contracts },
            { "candle_adapters", "CandleSeries cache + NumericFrame/DataView conversion", &test_candle_adapter_contracts },
            { "kernel_warmup", "kernel warmup and derived-series invariants", &test_kernel_warmup_contracts },
            { "axis_domains", "temporal/spatial/spatiotemporal runtime domains", &test_axis_domain_runtime_contracts },
            { "axis_query_temporal", "temporal axis payload and neighborhood-query contracts", &test_axis_query_temporal_contracts },
            { "axis_query_spatial", "spatial axis payload and neighborhood-query contracts", &test_axis_query_spatial_contracts },
            { "axis_query_spatiotemporal", "spatiotemporal axis payload and neighborhood-query contracts", &test_axis_query_spatiotemporal_contracts },
            { "topology_design_loads", "current built-in topology resolution design loads", &test_topology_design_load_contracts },
            { "topology_overload_rejection", "current built-in topology overload rejection contracts", &test_topology_overload_rejection_contracts },
            { "topology_metadata_preservation", "current topology metadata preservation contracts", &test_topology_metadata_preservation_contracts },
            { "locality_design_loads", "current locality execution design loads", &test_locality_design_load_contracts },
            { "locality_overload_rejection", "current locality overload rejection contracts", &test_locality_overload_rejection_contracts },
            { "locality_metadata_preservation", "current locality metadata preservation contracts", &test_locality_metadata_preservation_contracts },
            { "multi_entity_runner_design_loads", "neutral multi-entity runner design loads", &test_multi_entity_runner_design_load_contracts },
            { "multi_entity_runner_overload_rejection", "neutral multi-entity runner overload rejection", &test_multi_entity_runner_overload_rejection_contracts },
            { "operator_axes", "operator axis-contract classification", &test_operator_axis_contracts },
            { "compile_determinism", "PlanSpec compile determinism", &test_planspec_compile_determinism },
            { "compile_warmup_semantics", "PlanSpec execution warmup boundary semantics", &test_compile_warmup_semantics_contracts },
            { "axis_validation", "PlanSpec axis validation", &test_planspec_axis_validation },
            { "compile_bridge_design_loads", "current compile-bridge runtime design loads", &test_compile_bridge_design_load_contracts },
            { "compile_bridge_overload_rejection", "current compile-bridge runtime overload rejection contracts", &test_compile_bridge_overload_rejection_contracts },
            { "compile_bridge_registry_surface", "compile-bridge registry surface contracts", &test_compile_bridge_registry_surface_contracts },
            { "compile_bridge_temporal_runtime", "compile-bridge temporal runtime contracts", &test_compile_bridge_temporal_runtime_contracts },
            { "compile_bridge_spatial_runtime", "compile-bridge spatial runtime contracts", &test_compile_bridge_spatial_runtime_contracts },
            { "compile_bridge_subject_routing", "compile-bridge locality subject-routing contracts", &test_compile_bridge_subject_routing_contracts },
            { "compile_bridge_spatiotemporal_runtime", "compile-bridge spatiotemporal runtime contracts", &test_compile_bridge_spatiotemporal_runtime_contracts },
            { "compile_bridge_sweep_runtime", "compile-bridge sweep runtime contracts", &test_compile_bridge_sweep_runtime_contracts },
            { "compile_bridge_failure_path", "compile-bridge explicit failure-path contracts", &test_compile_bridge_failure_path_contracts },
            { "target_resolution_design_loads", "shared runtime target-resolution design loads", &test_target_resolution_design_load_contracts },
            { "target_resolution_overload_rejection", "shared runtime target-resolution overload rejection", &test_target_resolution_overload_rejection_contracts },
            { "numeric_portability", "cross-architecture numeric portability contracts", &test_numeric_portability_contracts },
            { "rowwise_runtime_branching", "row-wise numeric branching contracts", &test_rowwise_runtime_branching_contracts },
            { "ts_alias_runtime_parity", "TS runtime alias parity smoke contracts", &test_ts_alias_runtime_parity_contracts },
            { "ts_runtime_equivalence_smoke", "TS runtime representative equivalence smoke contracts", &test_ts_runtime_equivalence_smoke_contracts },
            { "assertion_hygiene", "test assertion-hygiene source scan", &test_assertion_hygiene_contracts },
            { "runtime_gates", "runtime no-legacy wrapper include gate", &test_runtime_no_legacy_composite_include },
            { "executor_determinism", "executor determinism", &test_executor_determinism },
        };
        static const std::vector<ContractSectionGroup> kSectionGroups = {
            {
                "service_core",
                "phase-2 local service-core contracts",
                {
                    "service_invalid_request_rejection",
                    "service_result_ownership",
                    "service_cancellation",
                    "service_running_cancellation",
                    "service_artifact_sandbox_cleanup",
                    "service_local_request_replay"
                }
            },
            {
                "service_concurrency",
                "phase-4 fair scheduler and bounded worker-pool service contracts",
                {
                    "service_scheduler_fairness",
                    "service_background_worker_pool",
                    "service_background_worker_cancellation",
                    "service_cross_client_isolation",
                    "service_scheduler_starvation_resistance",
                    "service_multi_client_stress",
                    "service_server_backpressure"
                }
            },
            {
                "service_protocol_surface",
                "phase-3 protocol framing and dispatch surface",
                {
                    "service_protocol_framing",
                    "service_protocol_dispatch"
                }
            },
            {
                "service_network_surface",
                "live socket and TCP hosted-service regression surface",
                {
                    "service_protocol_network_socket",
                    "service_protocol_live_multi_client",
                    "service_protocol_live_cancel_polling"
                }
            },
            {
                "alignment",
                "compatibility wrapper for split alignment contracts",
                { "alignment_calendar_policies", "alignment_schema_validation", "alignment_non_temporal_design_loads", "alignment_non_temporal_policy", "alignment_policy_metadata_preservation" }
            },
            {
                "row_storage",
                "compatibility wrapper for split row-domain storage contracts",
                { "row_storage_numeric_domain", "row_storage_spatial_runtime" }
            },
            {
                "topology_contracts",
                "compatibility wrapper for split topology catalog/runtime sidecar contracts",
                { "topology_catalog_identity", "topology_sidecar_mounting", "topology_manifest_provenance", "topology_legacy_compat" }
            },
            {
                "observation_capabilities",
                "compatibility wrapper for split observation capability diagnosis",
                { "observation_capability_spatial_surfaces", "observation_capability_temporal_extensions", "observation_capability_row_domain" }
            },
            {
                "plan_capabilities",
                "compatibility wrapper for split plan/operator capability diagnosis",
                { "plan_capability_facts", "plan_capability_runtime_binding", "plan_capability_runtime_topology", "plan_capability_runtime_locality", "multi_entity_ref_binding_overload_rejection" }
            },
            {
                "runtime_design_loads",
                "load-rated runtime seams that should carry real execution today",
                {
                    "axis_domains",
                    "alignment_non_temporal_design_loads",
                    "target_resolution_design_loads",
                    "topology_design_loads",
                    "locality_design_loads",
                    "multi_entity_runner_design_loads",
                    "compile_bridge_design_loads"
                }
            },
            {
                "runtime_overload_rejection",
                "load-rated overload cases that should reject explicitly today",
                {
                    "axis_validation",
                    "alignment_non_temporal_policy",
                    "target_resolution_overload_rejection",
                    "multi_entity_ref_binding_overload_rejection",
                    "topology_overload_rejection",
                    "locality_overload_rejection",
                    "multi_entity_runner_overload_rejection",
                    "compile_bridge_overload_rejection"
                }
            },
            {
                "metadata_preservation",
                "metadata-bearing seams that must preserve truth without pretending to execute",
                { "index_schema", "alignment_policy_metadata_preservation", "topology_metadata_preservation", "locality_metadata_preservation" }
            },
            {
                "compile_bridge_runtime",
                "compatibility wrapper for split compile-bridge runtime execution contracts",
                {
                    "compile_bridge_registry_surface",
                    "compile_bridge_temporal_runtime",
                    "compile_bridge_spatial_runtime",
                    "compile_bridge_subject_routing",
                    "compile_bridge_spatiotemporal_runtime",
                    "compile_bridge_sweep_runtime",
                    "compile_bridge_failure_path"
                }
            },
            {
                "axis_queries",
                "compatibility wrapper for split axis payload and neighborhood-query contracts",
                { "axis_query_temporal", "axis_query_spatial", "axis_query_spatiotemporal" }
            },
            {
                "ts_smoke",
                "compatibility wrapper for split TS smoke contracts",
                { "ts_alias_runtime_parity", "ts_runtime_equivalence_smoke" }
            },
        };

        std::vector<std::string> requested;
        requested.reserve(options.sections.size());
        for (const auto& section : options.sections) {
            requested.push_back(normalize_section_key(section));
        }

        struct SelectedEntry {
            const ContractSection* section{ nullptr };
            const ContractSectionGroup* group{ nullptr };
        };

        std::vector<SelectedEntry> selected;
        selected.reserve(kSections.size());
        if (requested.empty()) {
            for (const auto& section : kSections) {
                selected.push_back(SelectedEntry{ &section, nullptr });
            }
        }
        else {
            for (const auto& key : requested) {
                bool found = false;
                for (const auto& section : kSections) {
                    if (normalize_section_key(section.key) == key) {
                        selected.push_back(SelectedEntry{ &section, nullptr });
                        found = true;
                        break;
                    }
                }
                if (found) continue;

                for (const auto& group : kSectionGroups) {
                    if (normalize_section_key(group.key) == key) {
                        selected.push_back(SelectedEntry{ nullptr, &group });
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    out << "[FAIL] Unknown contract section: " << key << "\n";
                    return false;
                }
            }
        }

        out << "=== SugarBot Contract Suite ===\n";
        out << "Running deterministic platform-contract checks";
        out << (requested.empty() ? " (all sections):\n" : " (selected sections):\n");
        for (const SelectedEntry& entry : selected) {
            if (entry.section) {
                out << "  - [" << entry.section->key << "] " << entry.section->label << "\n";
            }
            else if (entry.group) {
                out << "  - [" << entry.group->key << "] " << entry.group->label << "\n";
            }
        }
        out << "\n";

        ContractAsserts t(out);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (selected[i].section) {
                selected[i].section->run(t);
            }
            else if (selected[i].group) {
                run_contract_section_group(t, *selected[i].group, kSections);
            }
            if (i + 1 < selected.size()) out << "\n";
        }

        out << "\n=== Contract Suite Summary ===\n";
        out << "Passed: " << t.passed << "\n";
        out << "Failed: " << t.failed << "\n";
        return t.failed == 0;
    }

} // namespace sugar
