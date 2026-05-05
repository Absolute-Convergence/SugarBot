#include "contract_suite_internal.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "legacy/legacy_candle_frame_adapter.h"
#include "ingest/observation_capability.h"
#include "ops/op_kernels_math.h"
#include "op_kernels_ms.h"
#include "op_kernels_spatial.h"
#include "op_kernels_ts.h"
#include "ingest/preprocess.h"
#include "ingest/run_manifest.h"
#include "ingest/run_pipeline.h"

namespace sugar::contract_suite_internal {

    namespace {
        TopologySpec make_contract_semantic_topology_spec() {
            TopologySpec spec{};
            spec.name = "entity.semantic.graph";
            spec.origin = TopologyOrigin::LayoutDeclared;
            spec.bearing = TopologyBearing::MetadataOnly;
            spec.family = TopologyFamily::SemanticGraph;
            spec.domain_scope = TopologyDomainScope::ObservationDomain;
            spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            spec.directed = true;
            spec.weighted = true;
            spec.artifact_ref = TopologyArtifactRef{
                "entity.semantic.graph.v1",
                "artifacts/entity.semantic.graph.parquet",
                "parquet",
                "sha256:entity-semantic-v1",
                "python_pandas"
            };
            spec.metadata.emplace_back("space", "semantic");
            return spec;
        }

        TopologySpec make_contract_probabilistic_topology_spec() {
            TopologySpec spec{};
            spec.name = "state.probabilistic.graph";
            spec.origin = TopologyOrigin::LayoutDeclared;
            spec.bearing = TopologyBearing::MetadataOnly;
            spec.family = TopologyFamily::ProbabilisticGraph;
            spec.domain_scope = TopologyDomainScope::ObservationDomain;
            spec.query_kinds = { TopologyQueryKind::TransitionSet };
            spec.directed = true;
            spec.weighted = true;
            spec.metadata.emplace_back("space", "probabilistic");
            return spec;
        }
    } // namespace

    bool test_topology_catalog_identity_contracts(ContractAsserts& t) {
        t.out << "[topology_catalog_identity] topology catalog identity and namespace contracts\n";

        TopologyCatalog catalog{};
        t.expect(catalog.empty(), "topology catalog starts empty");
        t.expect(catalog.size() == 0, "topology catalog empty size is zero");

        const auto temporal = make_builtin_temporal_topology_spec();
        t.expect(temporal.valid(), "temporal topology spec with name is valid");
        t.expect(temporal.supports_query_kind(TopologyQueryKind::OrderedWindow),
            "temporal topology spec reports supported ordered-window queries");
        t.expect(!temporal.supports_query_kind(TopologyQueryKind::MetricRadius),
            "temporal topology spec rejects unsupported query kinds");
        t.expect(temporal.origin == TopologyOrigin::BuiltinRuntime,
            "temporal topology spec records builtin runtime origin");
        t.expect(temporal.bearing == TopologyBearing::RuntimeBearing,
            "temporal topology spec records runtime-bearing execution status");

        t.expect(catalog.add(temporal), "topology catalog accepts builtin topology registration");
        t.expect(catalog.add(make_contract_semantic_topology_spec()),
            "topology catalog accepts unique declared topology");

        t.expect(catalog.size() == 2, "topology catalog stores multiple named topology specs");
        t.expect(catalog.has("builtin.temporal.window"), "topology catalog resolves built-in topology by name");
        t.expect(catalog.has_family(TopologyFamily::TemporalWindow), "topology catalog resolves temporal family");
        t.expect(catalog.has_family(TopologyFamily::SemanticGraph), "topology catalog resolves semantic family");

        const auto* semantic = catalog.find("entity.semantic.graph");
        t.expect(semantic != nullptr, "topology catalog finds semantic graph spec");
        if (semantic) {
            t.expect(semantic->directed, "semantic graph spec preserves directed flag");
            t.expect(semantic->weighted, "semantic graph spec preserves weighted flag");
            t.expect(semantic->artifact_ref.has_value(), "semantic graph spec preserves artifact provenance");
            t.expect(semantic->origin == TopologyOrigin::LayoutDeclared,
                "semantic graph spec records declared origin");
            t.expect(semantic->bearing == TopologyBearing::MetadataOnly,
                "semantic graph spec records metadata-only bearing");
            if (semantic->artifact_ref.has_value()) {
                t.expect(semantic->artifact_ref->stable_id == "entity.semantic.graph.v1",
                    "semantic graph spec records stable artifact id provenance");
                t.expect(semantic->artifact_ref->producer == "python_pandas",
                    "semantic graph spec records future Python/Pandas provenance");
            }
        }

        const auto names = catalog.list_names();
        t.expect(names.size() == 2 &&
            names[0] == "builtin.temporal.window" &&
            names[1] == "entity.semantic.graph",
            "topology catalog preserves insertion order for stable name listing");

        auto replacement = make_contract_semantic_topology_spec();
        replacement.weighted = false;
        replacement.metadata.emplace_back("revision", "2");
        std::string add_err;
        t.expect(!catalog.add(replacement, &add_err),
            "adding duplicate declared topology name is rejected explicitly");
        t.expect(add_err.find("duplicate topology name") != std::string::npos,
            "duplicate declared topology rejection reports explicit reason");
        t.expect(catalog.size() == 2, "rejecting duplicate declared topology leaves catalog size unchanged");
        semantic = catalog.find("entity.semantic.graph");
        if (semantic) {
            t.expect(semantic->weighted, "duplicate declared topology rejection preserves original entry fields");
        }

        const auto snapshot = catalog.snapshot();
        t.expect(catalog.add(make_contract_probabilistic_topology_spec()),
            "topology catalog remains mutable for new unique declared names");
        t.expect(snapshot.size() == 2, "topology catalog snapshot is detached from later mutations");
        t.expect(catalog.size() == 3, "topology catalog remains mutable after snapshot");
        t.expect(std::string(topology_family_name(TopologyFamily::SemanticGraph)) == "semantic_graph",
            "topology family naming helper is stable");
        t.expect(std::string(topology_query_kind_name(TopologyQueryKind::TransitionSet)) == "transition_set",
            "topology query naming helper is stable");

        TopologySpec reserved_name{};
        reserved_name.name = "builtin.temporal.window";
        reserved_name.origin = TopologyOrigin::LayoutDeclared;
        reserved_name.bearing = TopologyBearing::MetadataOnly;
        reserved_name.family = TopologyFamily::SemanticGraph;
        reserved_name.domain_scope = TopologyDomainScope::ObservationDomain;
        reserved_name.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
        add_err.clear();
        t.expect(!catalog.add(reserved_name, &add_err),
            "declared topology cannot claim reserved builtin namespace");
        t.expect(add_err.find("reserved for builtin runtime topology") != std::string::npos,
            "reserved builtin namespace rejection is explicit");

        return true;
    }

    bool test_topology_sidecar_mounting_contracts(ContractAsserts& t) {
        t.out << "[topology_sidecar_mounting] topology sidecar mounting contracts\n";

        {
            auto bundle = std::make_shared<align::AlignedBundle>();
            bundle->index_schema = IndexSchema::make_time_only();
            bundle->topology_catalog = std::make_shared<TopologyCatalog>();
            bundle->topology_catalog->add(make_contract_semantic_topology_spec());

            t.expect(bundle->has_index_schema(), "aligned bundle can carry index_schema beside topology catalog");
            t.expect(bundle->topology_catalog->has_family(TopologyFamily::SemanticGraph),
                "aligned bundle topology sidecar can carry non-axis semantic topology");
            t.expect(!bundle->index_schema->has_kind(AxisKind::Spatial),
                "non-axis topology sidecar does not leak into IndexSchema axis kinds");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            NumericFrame frame(*schema, std::vector<TimeKey>{ 1, 2, 3 });
            auto close = frame.ensure_col(close_id);
            close[0] = 10.0;
            close[1] = 11.0;
            close[2] = 12.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "temporal align_frames initializes topology catalog sidecar");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->size() == 1,
                    "temporal align_frames mounts one built-in runtime topology");
                t.expect(bundle.topology_catalog->has(builtin_temporal_topology_name()),
                    "temporal align_frames mounts built-in temporal topology");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            NumericFrame frame(*schema, static_cast<std::size_t>(3));
            auto close = frame.ensure_col(close_id);
            close[0] = 20.0;
            close[1] = 21.0;
            close[2] = 22.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "row-domain align_frames initializes topology catalog on non-temporal runtime too");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->empty(),
                    "row-domain align_frames topology catalog also starts empty");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            const ColumnId x_id = schema->columns().get_or_add("x");
            const ColumnId y_id = schema->columns().get_or_add("y");
            NumericFrame frame(*schema, std::vector<TimeKey>{ 1, 2, 3 });
            frame.set_index_schema(IndexSchema::make_time_only());
            if (frame.index_schema()) {
                frame.index_schema()->add_spatial_axis_2d("x", "y");
            }
            auto close = frame.ensure_col(close_id);
            auto x = frame.ensure_col(x_id);
            auto y = frame.ensure_col(y_id);
            close[0] = 10.0;
            close[1] = 11.0;
            close[2] = 12.0;
            x[0] = 0.0;
            x[1] = 1.0;
            x[2] = 2.0;
            y[0] = 0.0;
            y[1] = 0.0;
            y[2] = 0.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "spatio-temporal align_frames initializes topology catalog sidecar");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->size() == 3,
                    "spatio-temporal align_frames mounts temporal, spatial, and composite built-ins");
                t.expect(bundle.topology_catalog->has(builtin_temporal_topology_name()),
                    "spatio-temporal align_frames mounts built-in temporal topology");
                t.expect(bundle.topology_catalog->has(builtin_spatial_topology_name()),
                    "spatio-temporal align_frames mounts built-in spatial topology");
                t.expect(bundle.topology_catalog->has(builtin_spatiotemporal_topology_name()),
                    "spatio-temporal align_frames mounts built-in spatio-temporal topology");
            }
        }

        return true;
    }

    bool test_topology_manifest_provenance_contracts(ContractAsserts& t) {
        t.out << "[topology_manifest_provenance] topology manifest and runtime provenance contracts\n";

        {
            PreprocessReport preprocess{};
            preprocess.has_time_axis = true;
            preprocess.time_kind = TimeKind::YyyyMmDd;
            preprocess.time_resolution = "calendar_date_yyyymmdd";
            preprocess.series_keys = { "A" };

            auto catalog = std::make_shared<TopologyCatalog>();
            catalog->add(make_contract_semantic_topology_spec());

            auto manifest = make_run_manifest(preprocess, "A", catalog);
            t.expect(manifest.primary_series_key == "A", "run manifest preserves primary series key");
            t.expect(manifest.topologies.size() == 1, "run manifest snapshots topology catalog into manifest provenance");
            if (!manifest.topologies.empty()) {
                t.expect(manifest.topologies.front().name == "entity.semantic.graph",
                    "run manifest preserves topology name provenance");
                t.expect(manifest.topologies.front().origin == TopologyOrigin::LayoutDeclared,
                    "run manifest preserves topology origin provenance");
                t.expect(manifest.topologies.front().bearing == TopologyBearing::MetadataOnly,
                    "run manifest preserves topology bearing provenance");
                t.expect(manifest.topologies.front().artifact_ref.has_value(),
                    "run manifest preserves topology artifact provenance");
                if (manifest.topologies.front().artifact_ref.has_value()) {
                    t.expect(manifest.topologies.front().artifact_ref->stable_id == "entity.semantic.graph.v1",
                        "run manifest preserves topology artifact stable id");
                }
            }

            catalog->add(make_contract_probabilistic_topology_spec());
            t.expect(manifest.topologies.size() == 1,
                "run manifest topology provenance stays detached from later catalog mutation");
        }

        {
            RunManifest manifest{};
            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            append_observation_capability_snapshot(
                diagnose_observation_capabilities(facts),
                manifest);

            const auto it = std::find_if(
                manifest.capability_outcomes.begin(),
                manifest.capability_outcomes.end(),
                [](const CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "observation_surface" &&
                        outcome.subject == "Temporal";
                });
            t.expect(it != manifest.capability_outcomes.end(),
                "run manifest records observation-surface capability outcomes");
            if (it != manifest.capability_outcomes.end()) {
                t.expect(it->status == "valid",
                    "run manifest preserves observation-surface capability status");
            }
        }

        {
            const auto dir = contract_tmp_case_dir("topology_manifest_runtime_input");
            std::filesystem::create_directories(dir);
            const auto csv = dir / "topology_runtime_input.csv";
            const std::string body =
                "date,close\n"
                "20240101,100\n"
                "20240102,101\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write topology runtime-input fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;

            ResolvedRuntimeInput runtime{};
            std::string err;
            const bool ok = build_resolved_runtime_input(spec, csv.string(), "close", cfg, runtime, err);
            t.expect(ok, "build_resolved_runtime_input succeeds with topology sidecar wiring");
            if (ok) {
                t.expect(runtime.aligned_bundle != nullptr, "resolved runtime input owns aligned bundle");
                t.expect(runtime.topology_catalog != nullptr, "resolved runtime input exposes topology catalog");
                t.expect(runtime.topology_catalog == runtime.aligned_bundle->topology_catalog,
                    "resolved runtime input shares topology catalog with aligned bundle");
                t.expect(runtime.run_manifest.topologies.size() == 1,
                    "resolved runtime input manifest snapshots mounted built-in topology provenance by default");
                if (!runtime.run_manifest.topologies.empty()) {
                    t.expect(runtime.run_manifest.topologies.front().name == builtin_temporal_topology_name(),
                        "resolved runtime input manifest preserves mounted built-in topology name");
                    t.expect(runtime.run_manifest.topologies.front().artifact_ref.has_value(),
                        "resolved runtime input manifest preserves built-in topology artifact ref");
                    if (runtime.run_manifest.topologies.front().artifact_ref.has_value()) {
                        t.expect(runtime.run_manifest.topologies.front().artifact_ref->stable_id == "builtin.temporal.window.v1",
                            "resolved runtime input manifest preserves built-in topology artifact stable id");
                    }
                }
                const auto cap_it = std::find_if(
                    runtime.run_manifest.capability_outcomes.begin(),
                    runtime.run_manifest.capability_outcomes.end(),
                    [](const CapabilityOutcomeSnapshot& outcome) {
                        return outcome.domain == "observation_surface" &&
                            outcome.subject == "Temporal";
                    });
                t.expect(cap_it != runtime.run_manifest.capability_outcomes.end(),
                    "resolved runtime input manifest records observation capability outcomes");

                runtime.topology_catalog->add(make_contract_probabilistic_topology_spec());
                t.expect(runtime.run_manifest.topologies.size() == 1,
                    "resolved runtime input manifest remains detached after later topology catalog mutation");
            }
            else {
                t.expect(!err.empty(), "topology runtime-input fixture reports explicit failure text");
            }
        }

        return true;
    }

    bool test_topology_legacy_compat_provenance_contracts(ContractAsserts& t) {
        t.out << "[topology_legacy_compat] legacy embedded topology provenance contracts\n";

        const auto dir = contract_tmp_case_dir("topology_legacy_compat");
        const auto data_spec_path = dir / "embedded_single_frame_dataspec.json";
        const auto main_csv = dir / "embedded_single_frame_main.csv";
        const auto hedge_csv = dir / "embedded_single_frame_hedge.csv";
        const auto plan_path = dir / "embedded_single_frame_plan.json";

        const std::string data_spec_body =
            "{\n"
            "  \"header_policy\": \"RequiredHeaders\",\n"
            "  \"time_col\": \"date\",\n"
            "  \"time_kind\": \"YyyyMmDd\",\n"
            "  \"value_cols\": [\"close\"],\n"
            "  \"primary_value_col\": \"close\",\n"
            "  \"sort_by_time\": true,\n"
            "  \"dedupe\": \"Error\"\n"
            "}\n";
        const std::string main_csv_body =
            "date,close\n"
            "20240101,100\n"
            "20240102,101\n";
        const std::string hedge_csv_body =
            "date,close\n"
            "20240101,90\n"
            "20240102,91\n";
        const std::string plan_body =
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"embedded_single_frame_provenance\",\n"
            "  \"series_inputs\": [\n"
            "    {\n"
            "      \"key\": \"main\",\n"
            "      \"path\": \"embedded_single_frame_main.csv\",\n"
            "      \"data_spec\": \"embedded_single_frame_dataspec.json\"\n"
            "    },\n"
            "    {\n"
            "      \"key\": \"hedge\",\n"
            "      \"path\": \"embedded_single_frame_hedge.csv\",\n"
            "      \"data_spec\": \"embedded_single_frame_dataspec.json\"\n"
            "    }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"\",\n"
            "    \"exit_long\": \"\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";

        const bool fixture_ok =
            write_text_file(data_spec_path, data_spec_body) &&
            write_text_file(main_csv, main_csv_body) &&
            write_text_file(hedge_csv, hedge_csv_body) &&
            write_text_file(plan_path, plan_body);
        t.expect(fixture_ok, "embedded single-frame runtime provenance fixture is writable");
        if (!fixture_ok) return true;

        PlanSpec spec{};
        std::string err;
        const bool load_ok = load_plan_spec_file(plan_path.string(), spec, err);
        t.expect(load_ok, "embedded single-frame PlanSpec fixture loads");
        if (!load_ok) return true;

        ResolvedRuntimeInput runtime{};
        const bool runtime_ok = build_embedded_plan_observation_compat_runtime(spec, runtime, err);
        t.expect(runtime_ok, "embedded single-frame legacy runtime compatibility builds successfully");
        if (runtime_ok) {
            t.expect(runtime.primary_ref.series == "main",
                "embedded single-frame legacy runtime keeps first series as primary");
            t.expect(runtime.primary_entity_key == "main",
                "embedded single-frame legacy runtime preserves primary provenance as series key");
            t.expect(runtime.series_key_to_entity["main"] == "main",
                "embedded single-frame legacy runtime preserves main series provenance distinctly");
            t.expect(runtime.series_key_to_entity["hedge"] == "hedge",
                "embedded single-frame legacy runtime preserves secondary series provenance distinctly");

            const std::size_t main_index = runtime.aligned_bundle->find_series_index("main");
            const std::size_t hedge_index = runtime.aligned_bundle->find_series_index("hedge");
            t.expect(main_index != static_cast<std::size_t>(-1) &&
                runtime.aligned_bundle->entity_key_for_series_index(main_index) == "main",
                "embedded single-frame aligned bundle keeps main entity provenance aligned with series key");
            t.expect(hedge_index != static_cast<std::size_t>(-1) &&
                runtime.aligned_bundle->entity_key_for_series_index(hedge_index) == "hedge",
                "embedded single-frame aligned bundle keeps hedge entity provenance aligned with series key");
        }
        else {
            t.expect(!err.empty(), "embedded single-frame runtime compatibility reports explicit failure text");
        }

        return true;
    }

    bool test_target_resolution_design_load_contracts(ContractAsserts& t) {
        t.out << "[target_resolution_design_loads] shared runtime target-resolution design loads\n";

        const auto dir = contract_tmp_case_dir("target_resolution_design");
        std::filesystem::create_directories(dir);

        {
            const auto csv = dir / "panel.csv";
            const std::string body =
                "symbol,date,close\n"
                "AAA,20240101,10\n"
                "AAA,20240102,11\n"
                "BBB,20240101,20\n"
                "BBB,20240102,21\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write explicit target-resolution fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.entity_col = "symbol";
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            RuntimeTargetSpec explicit_target{};
            explicit_target.primary_series_key = "BBB";
            const auto explicit_options = make_strict_runtime_build_options(spec, explicit_target);

            ResolvedRuntimeInput explicit_runtime{};
            std::string explicit_err;
            const bool explicit_ok = build_resolved_runtime_input(
                spec,
                csv.string(),
                "close",
                explicit_options,
                explicit_runtime,
                explicit_err);
            t.expect(explicit_ok, "shared runtime build accepts explicit primary series on multi-series input");
            if (explicit_ok) {
                t.expect(explicit_runtime.primary_ref.series == "BBB",
                    "shared runtime build preserves explicit primary series binding");
                t.expect(explicit_runtime.primary_entity_key == "BBB",
                    "shared runtime build preserves explicit primary entity selection");
                t.expect(explicit_runtime.primary_selection_mode == RuntimePrimarySelectionMode::ExplicitSeries,
                    "shared runtime build records explicit primary selection mode");
                t.expect(explicit_runtime.run_manifest.primary_series_key == "BBB",
                    "shared runtime manifest preserves explicit primary series key");
                t.expect(explicit_runtime.run_manifest.primary_selection_mode ==
                        RuntimePrimarySelectionMode::ExplicitSeries,
                    "shared runtime manifest preserves explicit primary selection mode");
            }
            else {
                t.expect(!explicit_err.empty(), "explicit target-resolution failure text stays explicit");
            }

            const auto compat_options = make_compatibility_runtime_build_options(spec);

            ResolvedRuntimeInput compat_runtime{};
            std::string compat_err;
            const bool compat_ok = build_resolved_runtime_input(
                spec,
                csv.string(),
                "close",
                compat_options,
                compat_runtime,
                compat_err);
            t.expect(compat_ok, "compatibility runtime build still supports heuristic primary selection");
            if (compat_ok) {
                t.expect(compat_runtime.primary_ref.series == "AAA",
                    "compatibility runtime build keeps deterministic heuristic primary selection");
                t.expect(compat_runtime.primary_selection_mode == RuntimePrimarySelectionMode::HeuristicCompatibility,
                    "compatibility runtime build marks heuristic selection as compatibility behavior");
                t.expect(compat_runtime.run_manifest.primary_selection_mode ==
                        RuntimePrimarySelectionMode::HeuristicCompatibility,
                    "compatibility runtime manifest preserves heuristic selection provenance");
            }
            else {
                t.expect(!compat_err.empty(), "compatibility target-resolution failure text stays explicit");
            }
        }

        {
            const auto csv = dir / "single.csv";
            const std::string body =
                "date,close\n"
                "20240101,100\n"
                "20240102,101\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write single-series target-resolution fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            const auto options = make_strict_runtime_build_options(spec);

            ResolvedRuntimeInput runtime{};
            std::string err;
            const bool ok = build_resolved_runtime_input(
                spec,
                csv.string(),
                "close",
                options,
                runtime,
                err);
            t.expect(ok, "shared runtime build allows implicit target selection on single-series input");
            if (ok) {
                t.expect(runtime.primary_ref.series == "__single__",
                    "single-series target resolution preserves synthetic singleton series key");
                t.expect(runtime.primary_selection_mode == RuntimePrimarySelectionMode::SingleSeriesImplicit,
                    "single-series target resolution records implicit singleton mode");
                t.expect(runtime.run_manifest.primary_selection_mode ==
                        RuntimePrimarySelectionMode::SingleSeriesImplicit,
                    "single-series manifest preserves implicit singleton target mode");
            }
            else {
                t.expect(!err.empty(), "single-series target-resolution failure text stays explicit");
            }
        }

        return true;
    }

    bool test_target_resolution_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[target_resolution_overload_rejection] shared runtime target-resolution overload rejection\n";

        const auto dir = contract_tmp_case_dir("target_resolution_overload");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "panel.csv";
        const std::string body =
            "symbol,date,close\n"
            "AAA,20240101,10\n"
            "AAA,20240102,11\n"
            "BBB,20240101,20\n"
            "BBB,20240102,21\n";
        if (!write_text_file(csv, body)) {
            return t.expect(false, "failed to write target-resolution overload fixture");
        }

        DataSpec spec{};
        spec.header_policy = HeaderPolicy::RequiredHeaders;
        spec.entity_col = "symbol";
        spec.time_col = "date";
        spec.time_kind = TimeKind::YyyyMmDd;
        spec.value_cols = { "close" };
        spec.primary_value_col = "close";
        spec.sort_by_time = true;
        spec.dedupe = DataSpec::DedupePolicy::Error;

        const auto missing_target = make_strict_runtime_build_options(spec);

        ResolvedRuntimeInput missing_runtime{};
        std::string missing_err;
        const bool missing_ok = build_resolved_runtime_input(
            spec,
            csv.string(),
            "close",
            missing_target,
            missing_runtime,
            missing_err);
        t.expect(!missing_ok, "shared runtime build rejects multi-series input without explicit target");
        t.expect(missing_err.find("requires explicit primary series selection") != std::string::npos,
            "missing explicit multi-series target rejection stays explicit");

        RuntimeBuildOptions unknown_target = missing_target;
        unknown_target.target.primary_series_key = "MISSING";

        ResolvedRuntimeInput unknown_runtime{};
        std::string unknown_err;
        const bool unknown_ok = build_resolved_runtime_input(
            spec,
            csv.string(),
            "close",
            unknown_target,
            unknown_runtime,
            unknown_err);
        t.expect(!unknown_ok, "shared runtime build rejects explicit primary series that is not present");
        t.expect(unknown_err.find("explicit primary series 'MISSING' is not present") != std::string::npos,
            "unknown explicit primary series rejection stays explicit");

        return true;
    }

    bool test_candle_adapter_contracts(ContractAsserts& t) {
        t.out << "[candle_adapters] candle/cache + frame/DataView conversion\n";

        const CandleSeries base = make_simple_candles(8);
        const auto& rows = base.rows();

        t.expect(base.size() == rows.size(), "CandleSeries size matches row count");
        t.expect(base.closes().size() == rows.size(), "CandleSeries close cache size matches rows");
        t.expect(base.opens().size() == rows.size(), "CandleSeries open cache size matches rows");
        t.expect(base.highs().size() == rows.size(), "CandleSeries high cache size matches rows");
        t.expect(base.lows().size() == rows.size(), "CandleSeries low cache size matches rows");
        t.expect(base.volumes().size() == rows.size(), "CandleSeries volume cache size matches rows");
        t.expect(base.dates().size() == rows.size(), "CandleSeries date cache size matches rows");

        bool cache_values_ok = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Candle& c = rows[i];
            cache_values_ok =
                cache_values_ok &&
                equal_or_both_nonfinite(base.opens()[i], c.open) &&
                equal_or_both_nonfinite(base.highs()[i], c.high) &&
                equal_or_both_nonfinite(base.lows()[i], c.low) &&
                equal_or_both_nonfinite(base.closes()[i], c.close) &&
                equal_or_both_nonfinite(base.volumes()[i], c.volume) &&
                (base.dates()[i] == c.date);
        }
        t.expect(cache_values_ok, "CandleSeries caches preserve row-field parity");

        FrameSchema schema;
        NumericFrame frame = frame_from_candles(base, schema);
        ColumnId open_id{};
        ColumnId high_id{};
        ColumnId low_id{};
        ColumnId close_id{};
        ColumnId volume_id{};
        const bool has_open = schema.columns().find("open", open_id);
        const bool has_high = schema.columns().find("high", high_id);
        const bool has_low = schema.columns().find("low", low_id);
        const bool has_close = schema.columns().find("close", close_id);
        const bool has_volume = schema.columns().find("volume", volume_id);
        t.expect(has_open, "frame_from_candles exposes open column");
        t.expect(has_high, "frame_from_candles exposes high column");
        t.expect(has_low, "frame_from_candles exposes low column");
        t.expect(has_close, "frame_from_candles exposes close column");
        t.expect(has_volume, "frame_from_candles exposes volume column");
        if (!has_open || !has_high || !has_low || !has_close || !has_volume) return true;

        const auto open = frame.col(open_id);
        const auto high = frame.col(high_id);
        const auto low = frame.col(low_id);
        const auto close = frame.col(close_id);
        const auto volume = frame.col(volume_id);
        const auto time = frame.time();
        t.expect(open.size() == base.opens().size(), "NumericFrame open span size matches CandleSeries");
        t.expect(high.size() == base.highs().size(), "NumericFrame high span size matches CandleSeries");
        t.expect(low.size() == base.lows().size(), "NumericFrame low span size matches CandleSeries");
        t.expect(close.size() == base.closes().size(), "NumericFrame close span size matches CandleSeries");
        t.expect(volume.size() == base.volumes().size(), "NumericFrame volume span size matches CandleSeries");
        t.expect(time.size() == base.dates().size(), "NumericFrame time span size matches CandleSeries");

        bool frame_ok = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            frame_ok =
                frame_ok &&
                equal_or_both_nonfinite(open[i], base.opens()[i]) &&
                equal_or_both_nonfinite(high[i], base.highs()[i]) &&
                equal_or_both_nonfinite(low[i], base.lows()[i]) &&
                equal_or_both_nonfinite(close[i], base.closes()[i]) &&
                equal_or_both_nonfinite(volume[i], base.volumes()[i]) &&
                (time[i] == base.dates()[i]);
        }
        t.expect(frame_ok, "frame_from_candles preserves OHLCV/time values in NumericFrame");

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_frame(std::move(frame), bundle, primary);
        t.expect(okdv && bundle != nullptr, "candle-derived NumericFrame converts into aligned DataView");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(primary.series == "__single__", "candle-derived DataView primary series name is stable");
        t.expect(dv.size() == base.size(), "candle-derived DataView size matches CandleSeries rows");
        t.expect(dv.has_col(primary), "candle-derived DataView exposes primary close column");
        if (dv.has_col(primary)) {
            const auto primary_close = dv.col(primary);
            bool close_ok = primary_close.size() == base.closes().size();
            for (std::size_t i = 0; i < primary_close.size() && close_ok; ++i) {
                close_ok = equal_or_both_nonfinite(primary_close[i], base.closes()[i]);
            }
            t.expect(close_ok, "candle-derived DataView preserves close values");
        }
        const auto bundle_time = dv.time();
        t.expect(bundle_time.size() == base.dates().size(), "candle-derived DataView time size matches CandleSeries");
        if (bundle_time.size() == base.dates().size()) {
            bool time_ok = true;
            for (std::size_t i = 0; i < bundle_time.size(); ++i) {
                time_ok = time_ok && (bundle_time[i] == base.dates()[i]);
            }
            t.expect(time_ok, "candle-derived DataView preserves time values");
        }

        return true;
    }

    bool test_kernel_warmup_contracts(ContractAsserts& t) {
        t.out << "[kernel_warmup] warmup + derived-series invariants\n";

        std::vector<double> close;
        std::vector<double> high;
        std::vector<double> low;
        close.reserve(64);
        high.reserve(64);
        low.reserve(64);
        for (std::size_t i = 0; i < 64; ++i) {
            const double c = 100.0 + static_cast<double>(i);
            close.push_back(c);
            high.push_back(c + 0.2);
            low.push_back(c - 0.3);
        }

        const std::size_t n = 14;
        const std::size_t n_dc = 20;
        const std::size_t exp_sd_sma = n - 1;
        const std::size_t exp_sd_ema = (2 * n) - 2;
        const std::size_t exp_atr = n - 1;
        const std::size_t exp_rsi = n;
        const std::size_t exp_dc = n_dc - 1;

        const auto close_span = std::span<const double>(close.data(), close.size());
        const auto high_span = std::span<const double>(high.data(), high.size());
        const auto low_span = std::span<const double>(low.data(), low.size());
        const auto tr = op_kernels::ts_true_range_ohlc(high_span, low_span, close_span);
        const auto atr = op_kernels::ts_atr_wilder(std::span<const double>(tr.data(), tr.size()), n);
        const auto rsi = op_kernels::ts_rsi_wilder(close_span, n);
        const auto sd_sma = op_kernels_math::stddev(close_span, n);
        const auto sd_ema = op_kernels_math::stddev_ema(close_span, n);
        const auto z_sma = op_kernels_math::zscore_sma(close_span, n);
        const auto z_ema = op_kernels_math::zscore_ema(close_span, n);
        const auto close_roll_max = op_kernels::ts_roll_max(close_span, n_dc);
        const auto dc_u = op_kernels::ts_roll_max(high_span, n_dc);
        const auto dc_l = op_kernels::ts_roll_min(low_span, n_dc);
        const auto percentile_hi = op_kernels::ts_percentile(close_span, n_dc, 100.0);
        const auto entropy = op_kernels::ts_entropy(close_span, 4, 4);
        const auto autocorrelation = op_kernels::ts_autocorrelation(close_span, 6, 1);
        const auto hurst = op_kernels::ts_hurst(close_span, 8);
        const auto macd = op_kernels::ts_macd_histogram(close_span, 12, 26, 9);
        const auto bollinger_width = op_kernels::ts_bollinger_width(close_span, n, 2.0);
        const auto regime_change = op_kernels::ts_regime_change(close_span, 8);
        const auto volatility_ratio = op_kernels::ts_volatility_ratio(close_span, 5, 10);
        const auto sma_vals = op_kernels_math::sma(close_span, n);
        const auto fast_sd = op_kernels_math::stddev(close_span, 5);
        const auto slow_sd = op_kernels_math::stddev(close_span, 10);

        std::vector<double> dc_m(close.size(), std::numeric_limits<double>::quiet_NaN());
        std::vector<double> dc_w(close.size(), std::numeric_limits<double>::quiet_NaN());
        for (std::size_t i = 0; i < close.size(); ++i) {
            if (!std::isfinite(dc_u[i]) || !std::isfinite(dc_l[i])) continue;
            dc_m[i] = (dc_u[i] + dc_l[i]) * 0.5;
            dc_w[i] = dc_u[i] - dc_l[i];
        }

        t.expect(first_finite_index(sd_sma) == exp_sd_sma, "stddev_sma warmup matches contract");
        t.expect(first_finite_index(z_sma) == exp_sd_sma, "zscore_sma warmup matches contract");
        t.expect(first_finite_index(sd_ema) == exp_sd_ema, "stddev_ema warmup matches contract");
        t.expect(first_finite_index(z_ema) == exp_sd_ema, "zscore_ema warmup matches contract");
        t.expect(first_finite_index(atr) == exp_atr, "atr_wilder warmup matches contract");
        t.expect(first_finite_index(rsi) == exp_rsi, "rsi_wilder warmup matches contract");
        t.expect(first_finite_index(dc_u) == exp_dc, "donchian upper warmup matches contract");
        t.expect(first_finite_index(dc_l) == exp_dc, "donchian lower warmup matches contract");
        t.expect(first_finite_index(dc_m) == exp_dc, "donchian mid warmup matches contract");
        t.expect(first_finite_index(dc_w) == exp_dc, "donchian width warmup matches contract");
        t.expect(first_finite_index(percentile_hi) == exp_dc, "percentile warmup matches contract");
        t.expect(first_finite_index(entropy) == 3u, "entropy warmup matches contract");
        t.expect(first_finite_index(autocorrelation) == 5u, "autocorrelation warmup matches contract");
        t.expect(first_finite_index(hurst) == 7u, "hurst warmup matches contract");
        t.expect(first_finite_index(macd) == 33u, "macd histogram warmup matches contract");
        t.expect(first_finite_index(bollinger_width) == exp_sd_sma, "bollinger width warmup matches contract");
        t.expect(first_finite_index(regime_change) == 7u, "regime change warmup matches contract");
        t.expect(first_finite_index(volatility_ratio) == 9u, "volatility ratio warmup matches contract");

        bool dc_formula_ok = true;
        bool percentile_ok = true;
        bool entropy_ok = true;
        bool autocorrelation_ok = true;
        bool hurst_ok = true;
        bool bollinger_ok = true;
        bool regime_change_ok = true;
        bool volatility_ratio_ok = true;
        for (std::size_t i = exp_dc; i < close.size(); ++i) {
            dc_formula_ok =
                dc_formula_ok &&
                equal_or_both_nonfinite(dc_m[i], (dc_u[i] + dc_l[i]) * 0.5) &&
                equal_or_both_nonfinite(dc_w[i], dc_u[i] - dc_l[i]);
            percentile_ok =
                percentile_ok &&
                equal_or_both_nonfinite(percentile_hi[i], close_roll_max[i]);
        }
        for (std::size_t i = 3; i < close.size(); ++i) {
            entropy_ok = entropy_ok && nearly_equal(entropy[i], std::log(4.0));
        }
        for (std::size_t i = 5; i < close.size(); ++i) {
            autocorrelation_ok = autocorrelation_ok && nearly_equal(autocorrelation[i], 1.0);
        }
        for (std::size_t i = 7; i < close.size(); ++i) {
            hurst_ok = hurst_ok && std::isfinite(hurst[i]) && hurst[i] > 0.0;
            regime_change_ok = regime_change_ok && std::isfinite(regime_change[i]) && regime_change[i] > 1.0;
        }
        for (std::size_t i = exp_sd_sma; i < close.size(); ++i) {
            if (!std::isfinite(sd_sma[i]) || !std::isfinite(sma_vals[i]) || sma_vals[i] == 0.0) continue;
            bollinger_ok = bollinger_ok &&
                equal_or_both_nonfinite(bollinger_width[i], (4.0 * sd_sma[i]) / std::fabs(sma_vals[i]));
        }
        for (std::size_t i = 9; i < close.size(); ++i) {
            if (!std::isfinite(fast_sd[i]) || !std::isfinite(slow_sd[i]) || slow_sd[i] == 0.0) continue;
            volatility_ratio_ok = volatility_ratio_ok &&
                equal_or_both_nonfinite(volatility_ratio[i], fast_sd[i] / slow_sd[i]);
        }
        t.expect(dc_formula_ok, "donchian mid/width remain derived from upper/lower bands");
        t.expect(percentile_ok, "percentile q=100 remains derived from rolling max");
        t.expect(entropy_ok, "entropy returns natural-log Shannon entropy for uniform rolling bins");
        t.expect(autocorrelation_ok, "autocorrelation returns 1 for perfectly linear rolling windows");
        t.expect(hurst_ok, "hurst returns finite positive rolling R/S estimates for trending finite windows");
        t.expect(bollinger_ok, "bollinger width remains derived from stddev and |sma|");
        t.expect(regime_change_ok, "regime change returns finite positive CUSUM scores for trending finite windows");
        t.expect(volatility_ratio_ok, "volatility ratio remains derived from fast/slow stddev");

        {
            const std::vector<double> other = {
                200.0, 202.0, 204.0, 206.0, 208.0, 210.0, 212.0, 214.0
            };
            const auto corr = op_kernels::ms_correlation(
                std::span<const double>(close.data(), other.size()),
                std::span<const double>(other.data(), other.size()),
                4);
            t.expect(first_finite_index(corr) == 3u, "ms.correlation warmup matches contract");
            bool corr_ok = true;
            for (std::size_t i = 3; i < corr.size(); ++i) {
                corr_ok = corr_ok && nearly_equal(corr[i], 1.0);
            }
            t.expect(corr_ok, "ms.correlation returns 1 for perfectly linearly correlated windows");
        }

        {
            const std::vector<double> spatial_src = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 1.0, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };
            const SpatialNeighborhoodQuery2D q(
                std::span<const double>(x.data(), x.size()),
                std::span<const double>(y.data(), y.size()));
            const auto spatial_mean = op_kernels::spatial_radius_mean(
                std::span<const double>(spatial_src.data(), spatial_src.size()),
                q,
                1.1,
                true);
            t.expect(first_finite_index(spatial_mean) == 0, "spatial.radius_mean warmup matches child_warmup contract");
            const bool spatial_ok =
                nearly_equal(spatial_mean[0], 15.0) &&
                nearly_equal(spatial_mean[1], 15.0) &&
                nearly_equal(spatial_mean[2], 30.0);
            t.expect(spatial_ok, "spatial.radius_mean follows radius-neighborhood mean contract");
        }

        {
            const std::vector<double> st_src = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<TimeKey> time = {
                static_cast<TimeKey>(20240101),
                static_cast<TimeKey>(20240102),
                static_cast<TimeKey>(20240103),
                static_cast<TimeKey>(20240104)
            };
            const std::vector<double> x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };
            const SpatioTemporalNeighborhoodQuery2D q(
                std::span<const TimeKey>(time.data(), time.size()),
                std::span<const double>(x.data(), x.size()),
                std::span<const double>(y.data(), y.size()));
            const auto st_mean = op_kernels::st_radius_mean(
                std::span<const double>(st_src.data(), st_src.size()),
                q,
                2,
                0.5);
            t.expect(first_finite_index(st_mean) == 1, "st.radius_mean warmup matches child_warmup + (last_n-1) contract");
            const bool st_ok =
                !std::isfinite(st_mean[0]) &&
                nearly_equal(st_mean[1], 15.0) &&
                nearly_equal(st_mean[2], 30.0) &&
                nearly_equal(st_mean[3], 35.0);
            t.expect(st_ok, "st.radius_mean follows spatio-temporal neighborhood mean contract");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
