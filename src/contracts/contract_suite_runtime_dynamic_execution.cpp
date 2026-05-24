#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "engine/plan_compiler.h"
#include "engine/program_evaluator.h"
#include "engine/strategy_executor.h"
#include "ingest/run_manifest.h"
#include "program_evaluator_internal.h"
#include "ops/op_registry.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sweep/evaluation_report.h"
#include "sweep/dynamic_execution.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_sweep.h"

namespace sugar::contract_suite_internal {

    namespace {
        PlanSpec make_valid_dynamic_plan() {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
            };
            spec.bool_nodes = {
                NodeSpec{ "entry", "cmp.ge", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "entry";

            PlanExecutionSpec execution{};
            execution.mode = PlanExecutionMode::DeclaredDynamic;
            execution.subgraphs = {
                PlanExecutionSubgraphSpec{ "signals", { "close", "entry" }, {} },
            };
            execution.routes = {
                PlanExecutionRouteSpec{
                    "main",
                    "entry",
                    { PlanExecutionRouteCaseSpec{ "when_entry", true, { "signals" } } },
                    { "signals" },
                },
            };
            execution.outputs = {
                PlanExecutionOutputSpec{ "entry_long", "entry" },
            };
            spec.execution = std::move(execution);
            return spec;
        }

        NodeSpec bool_const_node(std::string id, bool value) {
            NodeSpec node{};
            node.id = std::move(id);
            node.op = "core.bool_const";
            node.params["value"] = value;
            return node;
        }

        NodeSpec numeric_const_node(std::string id, double value) {
            NodeSpec node{};
            node.id = std::move(id);
            node.op = "core.const";
            node.params["value"] = value;
            return node;
        }

        PlanSpec make_constant_route_dynamic_plan() {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
            };
            spec.bool_nodes = {
                bool_const_node("route_true", true),
                bool_const_node("legacy_entry", false),
                NodeSpec{ "dynamic_entry", "cmp.ge", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "legacy_entry";

            PlanExecutionSpec execution{};
            execution.mode = PlanExecutionMode::DeclaredDynamic;
            execution.subgraphs = {
                PlanExecutionSubgraphSpec{ "fallback", { "legacy_entry" }, {} },
                PlanExecutionSubgraphSpec{ "selected", { "close", "dynamic_entry" }, {} },
            };
            execution.routes = {
                PlanExecutionRouteSpec{
                    "main",
                    "route_true",
                    { PlanExecutionRouteCaseSpec{ "selected_when_true", true, { "selected" } } },
                    { "fallback" },
                },
            };
            execution.outputs = {
                PlanExecutionOutputSpec{ "entry_long", "dynamic_entry" },
            };
            spec.execution = std::move(execution);
            return spec;
        }

        PlanSpec make_constant_route_equivalent_static_plan() {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
            };
            spec.bool_nodes = {
                NodeSpec{ "dynamic_entry", "cmp.ge", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "dynamic_entry";
            return spec;
        }

        PlanSpec make_row_varying_selected_output_plan() {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
                numeric_const_node("threshold", 102.0),
            };
            spec.bool_nodes = {
                bool_const_node("legacy_entry", false),
                NodeSpec{ "route_high", "cmp.ge", { "close", "threshold" }, {} },
                NodeSpec{ "dynamic_entry", "cmp.ge", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "legacy_entry";

            PlanExecutionSpec execution{};
            execution.mode = PlanExecutionMode::DeclaredDynamic;
            execution.subgraphs = {
                PlanExecutionSubgraphSpec{ "fallback", { "legacy_entry" }, {} },
                PlanExecutionSubgraphSpec{ "selected", { "close", "dynamic_entry" }, {} },
            };
            execution.routes = {
                PlanExecutionRouteSpec{
                    "main",
                    "route_high",
                    { PlanExecutionRouteCaseSpec{ "when_high", true, { "selected" } } },
                    { "fallback" },
                },
            };
            execution.outputs = {
                PlanExecutionOutputSpec{ "entry_long", "dynamic_entry" },
            };
            spec.execution = std::move(execution);
            return spec;
        }

        PlanSpec make_row_varying_fallback_output_plan() {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
                numeric_const_node("threshold", 102.0),
            };
            spec.bool_nodes = {
                bool_const_node("fallback_entry", true),
                bool_const_node("selected_entry", false),
                NodeSpec{ "route_high", "cmp.ge", { "close", "threshold" }, {} },
            };
            spec.bind.entry_long = "selected_entry";

            PlanExecutionSpec execution{};
            execution.mode = PlanExecutionMode::DeclaredDynamic;
            execution.subgraphs = {
                PlanExecutionSubgraphSpec{ "fallback", { "fallback_entry" }, {} },
                PlanExecutionSubgraphSpec{ "selected", { "close", "selected_entry" }, {} },
            };
            execution.routes = {
                PlanExecutionRouteSpec{
                    "main",
                    "route_high",
                    { PlanExecutionRouteCaseSpec{ "when_high", true, { "selected" } } },
                    { "fallback" },
                },
            };
            execution.outputs = {
                PlanExecutionOutputSpec{ "entry_long", "fallback_entry" },
            };
            spec.execution = std::move(execution);
            return spec;
        }

        PlanSpec make_sweep_dynamic_plan() {
            PlanSpec spec = make_row_varying_selected_output_plan();
            spec.params["threshold"] = 102.0;
            for (auto& node : spec.numeric_nodes) {
                if (node.id == "threshold") {
                    node.params["value"] = ParamRef{ "threshold" };
                }
            }

            PlanSweepSpec sweep{};
            sweep.enabled = true;
            sweep.top_k = 3;
            sweep.max_evals = 3;
            sweep.objective = "pnl";
            SweepParamRange threshold_range{};
            threshold_range.values = { 101.0, 102.0, 103.0 };
            threshold_range.is_int = false;
            sweep.params["threshold"] = std::move(threshold_range);
            spec.sweep = std::move(sweep);
            return spec;
        }

        TopologySpec make_route_topology_spec(std::size_t row_count) {
            TopologySpec graph{};
            graph.name = "row.route.graph";
            graph.bearing = TopologyBearing::QueryBearing;
            graph.family = TopologyFamily::RelationalGraph;
            graph.domain_scope = TopologyDomainScope::RowDomain;
            graph.query_kinds = { TopologyQueryKind::NeighborSet };
            graph.directed = true;
            graph.node_count = row_count;
            graph.artifact_ref = TopologyArtifactRef{
                "row.route.graph.v1",
                "inline://contracts/dynamic_execution/row.route.graph",
                "inline_static_graph",
                "sha256:row-route-graph-v1",
                "contract_fixture"
            };
            graph.static_edges = {
                { 0, 1, 1.0 },
                { 0, 2, 1.0 },
                { 1, 2, 1.0 },
                { 2, 0, 1.0 },
                { 2, 3, 1.0 },
            };
            return graph;
        }

        PlanSpec make_topology_route_dynamic_plan() {
            PlanSpec spec{};
            spec.version = 1;
            spec.name = "contract_dynamic_topology_route";
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
                NodeSpec{ "degree", "topology.degree", {}, {
                    { "topology", std::string("row.route.graph") },
                } },
                numeric_const_node("dense_threshold", 1.5),
            };
            spec.bool_nodes = {
                bool_const_node("fallback_entry", true),
                NodeSpec{ "route_dense", "cmp.gt", { "degree", "dense_threshold" }, {} },
                NodeSpec{ "selected_entry", "cmp.ge", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "selected_entry";

            PlanExecutionSpec execution{};
            execution.mode = PlanExecutionMode::DeclaredDynamic;
            execution.subgraphs = {
                PlanExecutionSubgraphSpec{ "fallback", { "fallback_entry" }, {} },
                PlanExecutionSubgraphSpec{ "dense_rows", { "selected_entry" }, {} },
            };
            execution.routes = {
                PlanExecutionRouteSpec{
                    "topology_degree_gate",
                    "route_dense",
                    { PlanExecutionRouteCaseSpec{ "dense_graph_row", true, { "dense_rows" } } },
                    { "fallback" },
                },
            };
            execution.outputs = {
                PlanExecutionOutputSpec{ "entry_long", "selected_entry" },
            };
            spec.execution = std::move(execution);
            return spec;
        }

        bool make_topology_route_runtime(
            std::shared_ptr<align::AlignedBundle>& bundle,
            ColumnRef& primary,
            ResolvedRuntimeInput& runtime) {
            const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            if (!make_single_series_dataview_with_index_schema(
                    close,
                    IndexSchema::make_time_only(),
                    bundle,
                    primary,
                    static_cast<TimeKey>(20240101))) {
                return false;
            }

            auto catalog = std::make_shared<TopologyCatalog>();
            std::string add_err;
            if (!catalog->add(make_route_topology_spec(close.size()), &add_err)) {
                return false;
            }
            bundle->topology_catalog = catalog;

            PreprocessReport preprocess{};
            preprocess.produced_frameset = true;
            preprocess.has_time_axis = true;
            preprocess.has_spatial_axis = false;
            preprocess.has_entity_axis = false;
            runtime = ResolvedRuntimeInput{};
            runtime.source_kind = RuntimeObservationSourceKind::AppSuppliedObservations;
            runtime.aligned_bundle = bundle;
            runtime.topology_catalog = catalog;
            runtime.primary_ref = primary;
            runtime.primary_series_index = 0;
            runtime.primary_selection_mode = RuntimePrimarySelectionMode::SingleSeriesImplicit;
            runtime.run_manifest = make_run_manifest(
                preprocess,
                primary.series,
                catalog,
                RuntimePrimarySelectionMode::SingleSeriesImplicit);
            return true;
        }

        std::vector<std::pair<std::size_t, std::uint64_t>>
        dynamic_candidate_route_digests(const PlanSweepReport& report) {
            std::vector<std::pair<std::size_t, std::uint64_t>> out;
            out.reserve(report.top_candidates.size());
            for (const auto& candidate : report.top_candidates) {
                out.emplace_back(
                    candidate.candidate_index,
                    candidate.result.dynamic_execution.route_decision_digest);
            }
            return out;
        }

        std::size_t count_occurrences(
            std::string_view text,
            std::string_view needle) {
            if (needle.empty()) {
                return 0;
            }
            std::size_t count = 0;
            std::size_t pos = 0;
            while ((pos = text.find(needle, pos)) != std::string_view::npos) {
                ++count;
                pos += needle.size();
            }
            return count;
        }

        bool same_bool_buffer(
            const BoolBuffer& left,
            const BoolBuffer& right) {
            return left.size() == right.size() &&
                std::equal(left.begin(), left.end(), right.begin());
        }

        bool bool_buffer_equals(
            const BoolBuffer& actual,
            std::initializer_list<std::uint8_t> expected) {
            return actual.size() == expected.size() &&
                std::equal(actual.begin(), actual.end(), expected.begin());
        }

        bool compile_and_eval_plan_buffers(
            const PlanSpec& spec,
            const DataView& dv,
            const ColumnRef& primary,
            CompileResult& compiled,
            ProgramEvaluationBuffers& buffers,
            std::string& err) {
            const auto registry = make_default_registry();
            CompileEnv env{};
            if (!compile_plan(spec, registry, env, compiled, err)) {
                return false;
            }
            return evaluate_strategy_plan_program_buffers(
                dv,
                primary,
                compiled.plan,
                buffers,
                err);
        }
    }

    bool test_dynamic_execution_static_validation_contracts(ContractAsserts& t) {
        t.out << "[dynamic_execution_static_validation] declared dynamic execution schema and static validation\n";

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "close", "core.close", {}, {} },
            };
            spec.bool_nodes = {
                NodeSpec{ "entry", "cmp.gte", { "close", "close" }, {} },
            };
            spec.bind.entry_long = "entry";

            const auto absent_report = validate_plan_dynamic_execution(spec);
            t.expect(!absent_report.execution_section_present &&
                    !absent_report.dynamic_enabled &&
                    absent_report.valid &&
                    absent_report.issues.empty(),
                "absent execution section preserves static PlanSpec behavior");

            PlanExecutionSpec static_execution{};
            static_execution.mode = PlanExecutionMode::Static;
            spec.execution = static_execution;
            const auto static_report = validate_plan_dynamic_execution(spec);
            t.expect(static_report.execution_section_present &&
                    !static_report.dynamic_enabled &&
                    static_report.valid &&
                    static_report.issues.empty(),
                "explicit static execution section remains inert");
            t.expect(plan_execution_mode_name(PlanExecutionMode::DeclaredDynamic) ==
                    std::string_view("declared_dynamic"),
                "PlanExecutionMode exposes stable text names");
        }

        {
            PlanSpec spec{};
            std::string err;
            const bool ok = parse_plan_spec_json(
                R"json({
                  "version": 1,
                  "numeric_nodes": [
                    { "id": "close", "op": "core.close" }
                  ],
                  "bool_nodes": [
                    { "id": "entry", "op": "cmp.gte", "args": ["close", "close"] }
                  ],
                  "bindings": { "entry_long": "entry" },
                  "execution": {
                    "mode": "declared_dynamic",
                    "subgraphs": [
                      { "id": "signals", "nodes": ["close", "entry"] }
                    ],
                    "routes": [
                      {
                        "id": "main",
                        "condition": "entry",
                        "cases": [
                          { "id": "when_entry", "when": true, "activate": ["signals"] }
                        ],
                        "fallback": ["signals"]
                      }
                    ],
                    "outputs": { "entry_long": "entry" },
                    "bounds": {
                      "max_routes": 4,
                      "max_active_subgraphs": 2,
                      "max_selected_outputs": 4,
                      "decision_sample_limit": 8
                    }
                  }
                })json",
                spec,
                err);

            t.expect(ok && err.empty(), "PlanSpec parser accepts declared_dynamic execution section");
            if (ok) {
                t.expect(spec.execution.has_value() &&
                        spec.execution->mode == PlanExecutionMode::DeclaredDynamic &&
                        spec.execution->subgraphs.size() == 1 &&
                        spec.execution->routes.size() == 1 &&
                        spec.execution->outputs.size() == 1,
                    "declared_dynamic parse preserves subgraphs, routes, and outputs");

                const auto report = validate_plan_dynamic_execution(spec);
                t.expect(report.execution_section_present &&
                        report.dynamic_enabled &&
                        report.valid &&
                        report.issues.empty(),
                    "minimal declared_dynamic execution validates before runtime wiring");
            }
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->routes[0].condition = "close";
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::UnknownBoolConditionRef),
                "dynamic route conditions must reference bool nodes");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->subgraphs[0].nodes.push_back("missing");
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::UnknownNodeRef),
                "dynamic subgraphs reject unknown PlanSpec node ids");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->routes[0].cases[0].activate_subgraphs = { "missing" };
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::UnknownSubgraphRef),
                "dynamic routes reject unknown activated subgraphs");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->subgraphs.push_back(
                PlanExecutionSubgraphSpec{ "duplicate_owner", { "entry" }, {} });
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::AmbiguousNodeOwnership),
                "dynamic validation rejects nodes owned by multiple subgraphs");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->routes[0].fallback_subgraphs.clear();
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::MissingRouteFallback),
                "dynamic routes require deterministic fallback subgraphs");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->subgraphs.push_back(
                PlanExecutionSubgraphSpec{ "post", { }, { "signals" } });
            spec.execution->subgraphs[0].depends_on = { "post" };
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::CyclicSubgraphDependency),
                "dynamic subgraph dependencies reject cycles");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->bounds.max_routes = 1;
            auto second_route = spec.execution->routes[0];
            second_route.id = "secondary";
            spec.execution->routes.push_back(second_route);
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::RouteCapExceeded),
                "dynamic validation enforces route-count bounds");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->bounds.max_active_subgraphs = 1;
            spec.execution->subgraphs.push_back(
                PlanExecutionSubgraphSpec{ "risk", { }, {} });
            spec.execution->routes[0].cases[0].activate_subgraphs = { "signals", "risk" };
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::ActiveSubgraphCapExceeded),
                "dynamic validation enforces active-subgraph bounds");
        }

        {
            auto spec = make_valid_dynamic_plan();
            spec.execution->outputs.clear();
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::MissingStrategyBindingOutput),
                "dynamic execution requires explicit outputs for active strategy bindings");
        }

        {
            auto spec = make_constant_route_dynamic_plan();
            spec.execution->outputs[0].node = "route_true";
            const auto report = validate_plan_dynamic_execution(spec);
            t.expect(!report.valid &&
                    plan_dynamic_execution_report_has_issue(
                        report,
                        PlanDynamicExecutionValidationCode::MissingOutputNodeOwnership),
                "dynamic execution outputs must be owned by a declared subgraph");
        }

        return t.failed == 0;
    }

    bool test_dynamic_execution_compile_metadata_contracts(ContractAsserts& t) {
        t.out << "[dynamic_execution_compile_metadata] compiled metadata and constant-route equivalence\n";

        const auto registry = make_default_registry();
        CompileEnv env{};

        {
            const auto spec = make_constant_route_dynamic_plan();
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(spec, registry, env, compiled, err);
            t.expect(ok && err.empty(), "constant-route declared_dynamic plan compiles");
            if (ok && compiled.plan.storage) {
                const auto& program = compiled.plan.storage->program;
                const auto& metadata = program.dynamic_execution;
                t.expect(metadata.enabled &&
                        metadata.mode == CompiledDynamicExecutionMode::DeclaredDynamic &&
                        metadata.constant_route_only,
                    "compiled program records declared_dynamic constant-route mode");
                t.expect(metadata.subgraphs.size() == 2 &&
                        metadata.routes.size() == 1 &&
                        metadata.outputs.size() == 1,
                    "compiled dynamic metadata records subgraphs, routes, and outputs");
                if (!metadata.routes.empty()) {
                    t.expect(metadata.routes[0].condition_node_id == "route_true" &&
                            metadata.routes[0].condition_slot != kInvalidProgramSlot &&
                            metadata.routes[0].condition_is_constant &&
                            metadata.routes[0].constant_condition_value &&
                            metadata.routes[0].selected_subgraphs.size() == 1 &&
                            metadata.routes[0].selected_subgraphs[0] == 1,
                        "constant route selects the declared matching subgraph deterministically");
                }
                if (!metadata.outputs.empty()) {
                    t.expect(metadata.outputs[0].id == "entry_long" &&
                            metadata.outputs[0].node_id == "dynamic_entry" &&
                            metadata.outputs[0].ref.type == ValueType::Bool &&
                            metadata.outputs[0].ref.slot == program.entry_long_slot &&
                            metadata.outputs[0].owning_subgraph == 1,
                        "execution.outputs selects the final strategy binding slot");
                }
            }
        }

        {
            auto invalid = make_constant_route_dynamic_plan();
            invalid.execution->routes[0].fallback_subgraphs = { "missing" };
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(invalid, registry, env, compiled, err);
            t.expect(!ok &&
                    err.find("declared_dynamic execution validation failed") != std::string::npos &&
                    err.find("unknown_subgraph_ref") != std::string::npos,
                "compile rejects invalid declared_dynamic metadata before evaluator wiring");
        }

        {
            const auto row_varying = make_row_varying_selected_output_plan();
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(row_varying, registry, env, compiled, err);
            t.expect(ok && err.empty() && compiled.plan.storage &&
                    compiled.plan.storage->program.dynamic_execution.enabled &&
                    !compiled.plan.storage->program.dynamic_execution.constant_route_only,
                "row-varying declared_dynamic routes compile on the static superset schedule");
            if (ok && compiled.plan.storage) {
                const auto& metadata = compiled.plan.storage->program.dynamic_execution;
                t.expect(metadata.routes.size() == 1 &&
                        !metadata.routes[0].condition_is_constant &&
                        metadata.routes[0].selected_subgraphs.empty(),
                    "row-varying route metadata keeps selection row-local");
            }
        }

        {
            auto static_spec = make_constant_route_dynamic_plan();
            static_spec.execution->mode = PlanExecutionMode::Static;
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(static_spec, registry, env, compiled, err);
            t.expect(ok && compiled.plan.storage &&
                    !compiled.plan.storage->program.dynamic_execution.enabled,
                "explicit static execution section does not emit dynamic metadata");
        }

        {
            const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "constant-route equivalence fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                ProgramSignalOutputs dynamic_signals{};
                ProgramSignalOutputs static_signals{};
                std::string dynamic_err;
                std::string static_err;
                const bool dynamic_ok = compile_and_eval_plan_signals(
                    make_constant_route_dynamic_plan(),
                    dv,
                    primary,
                    dynamic_signals,
                    dynamic_err);
                const bool static_ok = compile_and_eval_plan_signals(
                    make_constant_route_equivalent_static_plan(),
                    dv,
                    primary,
                    static_signals,
                    static_err);

                t.expect(dynamic_ok && static_ok &&
                        same_bool_buffer(dynamic_signals.entry_long, static_signals.entry_long) &&
                        same_bool_buffer(dynamic_signals.exit_long, static_signals.exit_long) &&
                        same_bool_buffer(dynamic_signals.entry_short, static_signals.entry_short) &&
                        same_bool_buffer(dynamic_signals.exit_short, static_signals.exit_short) &&
                        same_bool_buffer(dynamic_signals.allow_trade, static_signals.allow_trade),
                    "constant-route declared_dynamic outputs match equivalent static plan signals");
            }
        }

        {
            const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "row-varying route fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                ProgramSignalOutputs selected_signals{};
                ProgramSignalOutputs fallback_signals{};
                std::string selected_err;
                std::string fallback_err;
                const bool selected_ok = compile_and_eval_plan_signals(
                    make_row_varying_selected_output_plan(),
                    dv,
                    primary,
                    selected_signals,
                    selected_err);
                const bool fallback_ok = compile_and_eval_plan_signals(
                    make_row_varying_fallback_output_plan(),
                    dv,
                    primary,
                    fallback_signals,
                    fallback_err);

                t.expect(selected_ok &&
                        bool_buffer_equals(selected_signals.entry_long,
                            { kBoolFalse, kBoolFalse, kBoolTrue, kBoolTrue }),
                    "row-varying route activates selected output only on matching rows");
                t.expect(fallback_ok &&
                        bool_buffer_equals(fallback_signals.entry_long,
                            { kBoolTrue, kBoolTrue, kBoolFalse, kBoolFalse }),
                    "row-varying route activates fallback output deterministically when no case matches");
            }
        }

        {
            const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "row-varying route mask fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                CompileResult compiled{};
                ProgramEvaluationBuffers buffers{};
                std::string err;
                const bool ok = compile_and_eval_plan_buffers(
                    make_row_varying_selected_output_plan(),
                    dv,
                    primary,
                    compiled,
                    buffers,
                    err);
                t.expect(ok && err.empty(),
                    "row-varying dynamic plan evaluates with route masks");
                if (ok && compiled.plan.storage) {
                    const auto& program = compiled.plan.storage->program;
                    const auto& metadata = program.dynamic_execution;
                    const auto close_node = std::find_if(
                        metadata.subgraphs[1].nodes.begin(),
                        metadata.subgraphs[1].nodes.end(),
                        [](const auto& node) { return node.node_id == "close"; });
                    const auto entry_node = std::find_if(
                        metadata.subgraphs[1].nodes.begin(),
                        metadata.subgraphs[1].nodes.end(),
                        [](const auto& node) { return node.node_id == "dynamic_entry"; });
                    const bool close_masked =
                        close_node != metadata.subgraphs[1].nodes.end() &&
                        close_node->slot < buffers.numeric_slots.size() &&
                        buffers.numeric_slots[close_node->slot].size() == 4 &&
                        std::isnan(buffers.numeric_slots[close_node->slot][0]) &&
                        std::isnan(buffers.numeric_slots[close_node->slot][1]) &&
                        buffers.numeric_slots[close_node->slot][2] == 102.0 &&
                        buffers.numeric_slots[close_node->slot][3] == 103.0;
                    const bool entry_masked =
                        entry_node != metadata.subgraphs[1].nodes.end() &&
                        entry_node->slot < buffers.bool_slots.size() &&
                        bool_buffer_equals(
                            buffers.bool_slots[entry_node->slot],
                            { kBoolFalse, kBoolFalse, kBoolTrue, kBoolTrue });
                    t.expect(close_masked && entry_masked,
                        "inactive dynamic subgraph buffers are masked per row");

                    const auto& summary = buffers.dynamic_execution;
                    t.expect(summary.enabled &&
                            summary.mode == "declared_dynamic" &&
                            summary.total_decisions == 4 &&
                            summary.emitted_decision_samples == 4 &&
                            summary.suppressed_decisions == 0 &&
                            summary.fallback_decisions == 2 &&
                            summary.inactive_output_rows == 2 &&
                            summary.max_active_subgraphs_per_row == 1 &&
                            summary.max_active_subgraph_cap == 8 &&
                            summary.bounded_by_declared_caps &&
                            summary.route_decision_digest != 0,
                        "dynamic route summary records bounded row-local decisions");
                    t.expect(summary.routes.size() == 1 &&
                            summary.routes[0].cases.size() == 1 &&
                            summary.routes[0].cases[0].decisions == 2 &&
                            summary.routes[0].fallback_decisions == 2 &&
                            summary.outputs.size() == 1 &&
                            summary.outputs[0].active_rows == 2 &&
                            summary.outputs[0].inactive_rows == 2,
                        "dynamic route summary records cases and output activity");
                }
            }
        }

        {
            const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "dynamic report fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                CompileResult compiled{};
                std::string compile_err;
                const bool compile_ok = compile_plan(
                    make_row_varying_selected_output_plan(),
                    registry,
                    env,
                    compiled,
                    compile_err);
                BacktestResult result{};
                std::string run_err;
                StrategyExecutor exec;
                const bool run_ok = compile_ok &&
                    exec.run(
                        dv,
                        primary,
                        compiled.plan,
                        compiled.exec,
                        result,
                        run_err);
                t.expect(run_ok && run_err.empty(),
                    "dynamic route report fixture executes through StrategyExecutor");
                if (run_ok) {
                    const auto report = build_run_report(result);
                    std::ostringstream text;
                    print_run_report_summary(report, text);
                    std::ostringstream json;
                    print_evaluation_report(
                        make_run_evaluation_report(report),
                        EvaluationReportProjection::Json,
                        json);
                    const auto text_report = text.str();
                    const auto json_report = json.str();
                    t.expect(text_report.find("Dynamic execution:") != std::string::npos &&
                            text_report.find("route_decision_digest") != std::string::npos &&
                            text_report.find("inactive_output_rows: 2") != std::string::npos,
                        "dynamic route diagnostics appear in full text reports");
                    t.expect(json_report.find("\"dynamic_execution\"") != std::string::npos &&
                            json_report.find("\"route_decision_digest\"") != std::string::npos &&
                            json_report.find("\"inactive_output_rows\":2") != std::string::npos &&
                            json_report.find("\"decision_samples\"") != std::string::npos,
                        "dynamic route diagnostics appear in JSON reports");
                }
            }
        }

        return t.failed == 0;
    }

    bool test_dynamic_execution_runtime_hardening_contracts(ContractAsserts& t) {
        t.out << "[dynamic_execution_runtime_hardening] sweep, panel, cancellation, and report-size hardening\n";

        const auto registry = make_default_registry();
        CompileEnv env{};

        {
            std::vector<double> close;
            close.reserve(10);
            for (std::size_t i = 0; i < 10; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "dynamic report-size fixture builds a DataView");
            if (view_ok && bundle) {
                PlanSpec capped = make_row_varying_selected_output_plan();
                capped.execution->bounds.decision_sample_limit = 2;
                const DataView dv(*bundle);
                CompileResult compiled{};
                std::string compile_err;
                const bool compile_ok = compile_plan(
                    capped,
                    registry,
                    env,
                    compiled,
                    compile_err);
                BacktestResult result{};
                std::string run_err;
                StrategyExecutor exec;
                const bool run_ok = compile_ok &&
                    exec.run(
                        dv,
                        primary,
                        compiled.plan,
                        compiled.exec,
                        result,
                        run_err);
                t.expect(run_ok && run_err.empty(),
                    "decision-sample capped dynamic plan executes");
                if (run_ok) {
                    const auto& summary = result.dynamic_execution;
                    t.expect(summary.total_decisions == 10 &&
                            summary.decision_sample_limit == 2 &&
                            summary.emitted_decision_samples == 2 &&
                            summary.suppressed_decisions == 8 &&
                            summary.decision_samples.size() == 2,
                        "dynamic decision samples stay bounded by execution.bounds.decision_sample_limit");

                    const auto report = build_run_report(result);
                    std::ostringstream text;
                    print_run_report_summary(report, text);
                    std::ostringstream json;
                    print_evaluation_report(
                        make_run_evaluation_report(report),
                        EvaluationReportProjection::Json,
                        json);
                    const std::string text_report = text.str();
                    const std::string json_report = json.str();
                    t.expect(text_report.find("decision_samples: 2/2 suppressed=8") != std::string::npos &&
                            count_occurrences(text_report, "sample row=") == 2,
                        "dynamic text report prints only the bounded decision sample set");
                    t.expect(json_report.find("\"emitted_decision_samples\":2") != std::string::npos &&
                            json_report.find("\"suppressed_decisions\":8") != std::string::npos &&
                            count_occurrences(json_report, "\"route_id\"") == 2,
                        "dynamic JSON report prints only the bounded decision sample set");
                }
            }
        }

        {
            std::vector<double> close{ 100.0, 101.0, 102.0, 103.0, 104.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "dynamic sweep fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                PlanSweepRuntimeOptions opt{};
                std::ostringstream sweep_log;
                BacktestResult best{};
                PlanSweepReport sweep_report{};
                const bool sweep_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    make_sweep_dynamic_plan(),
                    opt,
                    sweep_log,
                    &best,
                    &sweep_report);
                t.expect(sweep_ok,
                    "declared_dynamic plan executes through sweep runtime");
                if (sweep_ok) {
                    const auto candidate_reports =
                        build_sweep_candidate_reports(
                            sweep_report.top_candidates,
                            &sweep_report.resolved_objective,
                            &sweep_report.run_provenance);
                    const bool all_dynamic = !candidate_reports.empty() &&
                        std::all_of(
                            candidate_reports.begin(),
                            candidate_reports.end(),
                            [](const SweepCandidateReport& report) {
                                return report.run.execution.dynamic_execution.enabled &&
                                    report.run.execution.dynamic_execution.total_decisions > 0 &&
                                    report.run.execution.dynamic_execution.route_decision_digest != 0;
                            });
                    t.expect(sweep_report.evaluations_run == 3 &&
                            candidate_reports.size() == 3 &&
                            all_dynamic,
                        "sweep candidate reports preserve dynamic execution diagnostics per candidate");

                    std::ostringstream json;
                    print_evaluation_report(
                        make_sweep_evaluation_report(sweep_report),
                        EvaluationReportProjection::Json,
                        json);
                    t.expect(
                        count_occurrences(json.str(), "\"dynamic_execution\"") >= 3,
                        "sweep JSON report carries dynamic execution diagnostics on candidate run reports");
                    t.expect(
                        sweep_log.str().find("Dynamic execution:") != std::string::npos,
                        "full sweep text report carries dynamic execution diagnostics for ranked candidates");
                }
            }
        }

        {
            const std::vector<double> primary_close{ 100.0, 103.0, 104.0, 105.0 };
            const std::vector<double> secondary_close{ 100.0, 101.0, 102.0, 103.0 };
            const std::vector<double> primary_x{ 0.0, 0.0, 0.0, 0.0 };
            const std::vector<double> primary_y{ 0.0, 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x{ 1.0, 1.0, 1.0, 1.0 };
            const std::vector<double> secondary_y{ 1.0, 1.0, 1.0, 1.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(view_ok && static_cast<bool>(bundle),
                "dynamic panel fixture builds a two-series DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                PanelRankingSpec ranking{};
                ranking.objective.primary.source = ObjectiveValueSource::ReportedMetric;
                ranking.objective.primary.name = "pnl";
                ranking.objective.direction = ObjectiveDirection::Maximize;
                PlanSweepRuntimeOptions opt{};
                const auto panel_result =
                    run_ranked_multi_entity_custom_planspec_over_dataview(
                        dv,
                        make_row_varying_selected_output_plan(),
                        opt,
                        primary.col,
                        2,
                        ranking);
                t.expect(!panel_result.canceled &&
                        panel_result.failures.empty() &&
                        panel_result.topk.size() == 2,
                    "declared_dynamic plan executes through ranked panel runtime");
                const PanelReport panel_report = build_panel_report(panel_result);
                const bool panel_rows_dynamic =
                    panel_report.top_rows.size() == 2 &&
                    std::all_of(
                        panel_report.top_rows.begin(),
                        panel_report.top_rows.end(),
                        [](const PanelEntityReport& row) {
                            return row.run.execution.dynamic_execution.enabled &&
                                row.run.execution.dynamic_execution.total_decisions > 0;
                        });
                t.expect(panel_rows_dynamic,
                    "panel report preserves dynamic execution diagnostics per ranked row");

                std::ostringstream text;
                print_panel_report_summary(panel_report, text);
                std::ostringstream json;
                print_evaluation_report(
                    make_panel_evaluation_report(panel_report),
                    EvaluationReportProjection::Json,
                    json);
                t.expect(text.str().find("dynamic_execution mode=declared_dynamic") != std::string::npos &&
                        text.str().find("route_decision_digest=") != std::string::npos,
                    "panel text report exposes compact dynamic execution diagnostics");
                t.expect(count_occurrences(json.str(), "\"dynamic_execution\"") == 2,
                    "panel JSON report carries dynamic execution diagnostics on each ranked row");
            }
        }

        {
            std::vector<double> close{ 100.0, 101.0, 102.0, 103.0 };
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool view_ok = make_single_series_dataview_with_index_schema(
                close,
                IndexSchema::make_time_only(),
                bundle,
                primary);
            t.expect(view_ok && static_cast<bool>(bundle),
                "dynamic canceled-sweep fixture builds a DataView");
            if (view_ok && bundle) {
                const DataView dv(*bundle);
                std::size_t cancel_checks = 0;
                PlanSweepRuntimeOptions opt{};
                opt.cancel_requested = [&]() {
                    ++cancel_checks;
                    return cancel_checks > 10;
                };
                std::ostringstream sweep_log;
                BacktestResult best{};
                PlanSweepReport sweep_report{};
                const bool sweep_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    make_sweep_dynamic_plan(),
                    opt,
                    sweep_log,
                    &best,
                    &sweep_report);
                t.expect(!sweep_ok &&
                        sweep_report.canceled &&
                        sweep_report.evaluations_run == 1,
                    "canceled dynamic sweep returns a partial report after one evaluated candidate");
                if (sweep_report.canceled) {
                    t.expect(sweep_report.candidate_outcomes.completed == 1 &&
                            !sweep_report.top_candidates.empty() &&
                            sweep_report.top_candidates.front().result.dynamic_execution.enabled,
                        "partial canceled sweep report preserves completed candidate dynamic diagnostics");
                    std::ostringstream json;
                    print_evaluation_report(
                        make_sweep_evaluation_report(sweep_report),
                        EvaluationReportProjection::Json,
                        json);
                    const std::string json_report = json.str();
                    t.expect(json_report.find("\"canceled\":true") != std::string::npos &&
                            json_report.find("\"dynamic_execution\"") != std::string::npos &&
                            json_report.find("\"proposal_trace\"") != std::string::npos,
                        "canceled dynamic sweep JSON remains reportable with route diagnostics and trace metadata");
                }
            }
        }

        return t.failed == 0;
    }

    bool test_dynamic_execution_topology_routing_contracts(ContractAsserts& t) {
        t.out << "[dynamic_execution_topology_routing] topology-informed declared dynamic route selectors\n";

        const auto registry = make_default_registry();
        const auto spec = make_topology_route_dynamic_plan();
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        ResolvedRuntimeInput runtime{};
        const bool runtime_ok = make_topology_route_runtime(bundle, primary, runtime);
        t.expect(runtime_ok && static_cast<bool>(bundle),
            "topology route fixture builds a DataView with a row-domain graph");
        if (!runtime_ok || !bundle) {
            return false;
        }

        const DataView dv(*bundle);
        auto capability = diagnose_plan_capabilities(spec, registry, runtime);
        t.expect(capability.status == PlanCapabilityStatus::Valid,
            "plan capability accepts a topology-local route selector");
        append_plan_capability_snapshot(capability, runtime.run_manifest);
        const auto* degree_usage =
            find_topology_usage(runtime.run_manifest, "degree", "row.route.graph");
        t.expect(degree_usage &&
                degree_usage->runtime_resolved &&
                degree_usage->required_topology_query_kinds ==
                    std::vector<std::string>{
                        std::string(topology_query_kind_name(TopologyQueryKind::NeighborSet))
                    },
            "topology route selector records runtime-resolved topology usage");
        if (degree_usage) {
            t.expect(degree_usage->topology_bearing == "query_bearing" &&
                    degree_usage->artifact_ref.stable_id == "row.route.graph.v1",
                "topology route selector preserves query-bearing artifact provenance");
        }

        ProgramSignalOutputs first_signals{};
        std::string first_err;
        const bool first_ok = compile_and_eval_plan_signals(
            spec,
            dv,
            primary,
            first_signals,
            first_err);
        t.expect(first_ok && first_err.empty(),
            "topology-informed dynamic route evaluates through compiled signals");
        if (!first_ok) {
            t.out << "    eval error: " << first_err << "\n";
            return false;
        }
        t.expect(bool_buffer_equals(
                first_signals.entry_long,
                { kBoolTrue, kBoolFalse, kBoolTrue, kBoolFalse }),
            "topology.degree route activates the selected subgraph only on dense graph rows");

        CompileResult compiled{};
        ProgramEvaluationBuffers buffers{};
        std::string buffer_err;
        const bool buffers_ok = compile_and_eval_plan_buffers(
            spec,
            dv,
            primary,
            compiled,
            buffers,
            buffer_err);
        t.expect(buffers_ok && buffer_err.empty(),
            "topology-informed dynamic route exposes evaluator buffers");
        if (buffers_ok) {
            const auto& summary = buffers.dynamic_execution;
            t.expect(summary.enabled &&
                    summary.total_decisions == 4 &&
                    summary.routes.size() == 1 &&
                    summary.routes[0].true_decisions == 2 &&
                    summary.routes[0].fallback_decisions == 2 &&
                    summary.routes[0].cases.size() == 1 &&
                    summary.routes[0].cases[0].decisions == 2 &&
                    summary.outputs.size() == 1 &&
                    summary.outputs[0].active_rows == 2 &&
                    summary.outputs[0].inactive_rows == 2 &&
                    summary.inactive_output_rows == 2 &&
                    summary.route_decision_digest != 0,
                "dynamic execution summary records topology-driven route decisions");
        }

        ProgramEvaluationBuffers repeated_buffers{};
        std::string repeated_err;
        CompileResult repeated_compiled{};
        const bool repeated_ok = compile_and_eval_plan_buffers(
            spec,
            dv,
            primary,
            repeated_compiled,
            repeated_buffers,
            repeated_err);
        t.expect(repeated_ok &&
                buffers_ok &&
                repeated_buffers.dynamic_execution.route_decision_digest ==
                    buffers.dynamic_execution.route_decision_digest,
            "topology-informed route decisions are deterministic across repeated evaluation");

        PlanSweepRuntimeOptions opt{};
        std::ostringstream run_log;
        BacktestResult result{};
        const bool run_ok = run_custom_plan_spec(
            dv,
            primary,
            spec,
            opt,
            run_log,
            &result,
            nullptr,
            nullptr);
        t.expect(run_ok &&
                result.dynamic_execution.enabled &&
                result.dynamic_execution.total_decisions == 4 &&
                result.dynamic_execution.inactive_output_rows == 2,
            "DataView-first runtime validation allows topology-local dynamic route selectors");

        auto missing = spec;
        for (auto& node : missing.numeric_nodes) {
            if (node.id == "degree") {
                node.params["topology"] = std::string("missing.route.graph");
            }
        }
        std::ostringstream missing_log;
        BacktestResult missing_result{};
        const bool missing_ok = run_custom_plan_spec(
            dv,
            primary,
            missing,
            opt,
            missing_log,
            &missing_result,
            nullptr,
            nullptr);
        t.expect(!missing_ok &&
                missing_log.str().find("required topology 'missing.route.graph' is not available") !=
                    std::string::npos,
            "DataView-first runtime validation rejects missing topology route selectors explicitly");

        return t.failed == 0;
    }

    bool test_dynamic_execution_replay_compatibility_contracts(ContractAsserts& t) {
        t.out << "[dynamic_execution_replay_compatibility] dynamic sweep replay and determinism compatibility\n";

        const std::vector<double> close{ 100.0, 101.0, 102.0, 103.0, 104.0 };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool view_ok = make_single_series_dataview_with_index_schema(
            close,
            IndexSchema::make_time_only(),
            bundle,
            primary);
        t.expect(view_ok && static_cast<bool>(bundle),
            "dynamic replay fixture builds a DataView");
        if (!view_ok || !bundle) {
            return false;
        }

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};
        std::ostringstream first_log;
        BacktestResult first_best{};
        PlanSweepReport first_report{};
        const bool first_ok = run_custom_plan_spec(
            dv,
            primary,
            make_sweep_dynamic_plan(),
            opt,
            first_log,
            &first_best,
            &first_report);
        t.expect(first_ok && first_report.evaluations_run == 3,
            "declared_dynamic sweep replay fixture executes bounded candidate set");
        if (!first_ok) {
            return false;
        }

        const auto& trace = first_report.proposal_trace;
        t.expect(trace.captured &&
                trace.full_event_stream_captured &&
                trace.trace_digest_available &&
                trace.event_order_valid &&
                trace.observation_payload_complete &&
                trace.exact_replay_from_report &&
                trace.non_replayable_reasons.empty() &&
                trace.proposal_count == 3 &&
                trace.observation_count == 3,
            "dynamic sweep proposal trace is complete enough for exact replay");
        t.expect(!first_report.top_candidates.empty() &&
                std::all_of(
                    first_report.top_candidates.begin(),
                    first_report.top_candidates.end(),
                    [](const SweepRankedCandidate& candidate) {
                        return candidate.result.dynamic_execution.enabled &&
                            candidate.result.dynamic_execution.route_decision_digest != 0;
                    }),
            "dynamic sweep candidates retain route diagnostics beside replay trace");

        std::ostringstream replay_text;
        print_evaluation_report(
            make_sweep_evaluation_report(first_report),
            EvaluationReportProjection::ReplayTrace,
            replay_text);
        std::ostringstream replay_json;
        print_evaluation_report(
            make_sweep_evaluation_report(first_report),
            EvaluationReportProjection::Json,
            replay_json);
        t.expect(replay_text.str().find("Sweep replay trace:") != std::string::npos &&
                replay_text.str().find("exact_replay_from_report=true") != std::string::npos &&
                replay_text.str().find("observation_payload_complete=true") != std::string::npos,
            "dynamic sweep replay projection exposes exact replay status");
        t.expect(replay_json.str().find("\"proposal_trace\"") != std::string::npos &&
                replay_json.str().find("\"exact_replay_from_report\":true") != std::string::npos &&
                replay_json.str().find("\"dynamic_execution\"") != std::string::npos,
            "dynamic sweep JSON carries replay trace and candidate route diagnostics together");

        std::ostringstream second_log;
        BacktestResult second_best{};
        PlanSweepReport second_report{};
        const bool second_ok = run_custom_plan_spec(
            dv,
            primary,
            make_sweep_dynamic_plan(),
            opt,
            second_log,
            &second_best,
            &second_report);
        t.expect(second_ok &&
                second_report.proposal_trace.trace_digest_available &&
                second_report.proposal_trace.trace_digest ==
                    first_report.proposal_trace.trace_digest,
            "dynamic sweep proposal trace digest is deterministic across repeated runs");
        t.expect(second_ok &&
                dynamic_candidate_route_digests(second_report) ==
                    dynamic_candidate_route_digests(first_report),
            "dynamic sweep candidate route digests are deterministic across repeated runs");

        return t.failed == 0;
    }

} // namespace sugar::contract_suite_internal
