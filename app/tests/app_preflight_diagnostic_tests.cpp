#include "tests/app_test_support.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "ingest/layout_spec.h"
#include "reporting/app_reporters.h"
#include "runtime/app_runtime.h"
#include "sweep/runtime_graph_preview.h"

namespace sugar::app::tests {

    namespace {

        bool test_feature_dump_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] feature-dump smoke\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_feature_dump_fixture(fixture, fixture_err);
            t.check(fixture_ok, "feature-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::FeatureDump;
            cmd.feature_dump.layout_path = fixture.layout_path;
            cmd.feature_dump.logic_path = fixture.logic_path;
            cmd.feature_dump.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_feature_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "feature-dump command returns success");
            t.check(err.str().empty(), "feature-dump command produces no error output");
            t.check(text.find("series_key,entity_key,row_index,time_key,close,sma2") == 0,
                "feature-dump output starts with source-column labels for raw CSV-backed features");
            t.check(text.find("__single__,__single__,0,20240101,10") != std::string::npos,
                "feature-dump output includes the first row");
            t.check(text.find("__single__,__single__,1,20240102,12,11") != std::string::npos,
                "feature-dump output includes computed feature values");
            return t.failed == 0;
        }

        bool test_prep_dump_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] prep-dump smoke\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "prep-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = fixture.spec_path;
            cmd.prep_dump.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();
            t.check(rc == 0, "prep-dump command returns success");
            t.check(err.str().empty(), "prep-dump command produces no error output");
            t.check(text.find("DataSpec summary") != std::string::npos, "prep-dump output includes DataSpec summary");
            return t.failed == 0;
        }

        bool test_prep_dump_spatial_output_is_row_domain_honest(TestContext& t) {
            t.out << "[app_dispatch] prep-dump spatial row-domain reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only prep-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "spatial-only prep-dump command returns success");
            t.check(err.str().empty(), "spatial-only prep-dump command produces no error output");
            t.check(text.find("time_col: <none>") != std::string::npos,
                "spatial-only prep-dump reports missing time_col");
            t.check(text.find("time_kind: <none>") != std::string::npos,
                "spatial-only prep-dump reports missing time_kind");
            t.check(text.find("row_domain: 0..2") != std::string::npos,
                "spatial-only prep-dump reports row domain instead of time range");
            t.check(text.find("time: 0..2") == std::string::npos,
                "spatial-only prep-dump avoids fabricated time summary");
            return t.failed == 0;
        }

        bool test_prep_dump_reports_observation_capabilities(TestContext& t) {
            t.out << "[app_dispatch] prep-dump observation capability reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only capability fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "prep-dump capability command returns success");
            t.check(err.str().empty(), "prep-dump capability command produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "prep-dump output includes observation capability header");
            t.check(text.find("Row domain: valid") != std::string::npos,
                "prep-dump reports row-domain validity for spatial-only data");
            t.check(text.find("Temporal: invalid - no semantic time axis") != std::string::npos,
                "prep-dump explains temporal invalidity for spatial-only data");
            t.check(text.find("Spatial: valid") != std::string::npos,
                "prep-dump explains spatial validity for spatial-only data");
            t.check(text.find("Spatiotemporal: invalid - no semantic time axis") != std::string::npos,
                "prep-dump explains spatiotemporal invalidity for spatial-only data");
            t.check(text.find("Spatial panel: conditional - provide entity grouping schema") != std::string::npos,
                "prep-dump explains spatial-panel conditionality for spatial-only single-series data");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "prep-dump exposes built-in spatial topology inventory");
            return t.failed == 0;
        }

        bool test_prep_dump_reports_panel_capability_incompatibility(TestContext& t) {
            t.out << "[app_dispatch] prep-dump panel capability incompatibility\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel capability fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "spatial-panel prep-dump command returns success");
            t.check(err.str().empty(), "spatial-panel prep-dump command produces no error output");
            t.check(text.find("Spatial panel: valid") != std::string::npos,
                "prep-dump explains spatial panel validity for spatial-only panel data");
            t.check(text.find("Temporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "prep-dump explains temporal panel invalidity for spatial-only panel data");
            t.check(text.find("Spatiotemporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "prep-dump explains spatiotemporal panel invalidity for spatial-only panel data");
            return t.failed == 0;
        }

        bool test_runtime_graph_preview_spatial_output_is_row_domain_honest(TestContext& t) {
            t.out << "[runtime_graph_preview] spatial row-domain reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only runtime-preview fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatial-only runtime-preview fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            AlignedRuntimeInput runtime{};
            const bool runtime_ok = sugar::app::build_aligned_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "spatial-only runtime-preview fixture builds runtime input");
            if (!runtime_ok) return false;

            const sugar::DataView dv(*runtime.aligned_bundle);
            const auto capability_report = sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(runtime));
            std::ostringstream out;
            sugar::print_runtime_graph_preview(
                dv, runtime.primary_ref, runtime.run_manifest, "spatial-test", out, 8, &capability_report);
            const std::string text = out.str();

            t.check(text.find("Observation capability") != std::string::npos,
                "spatial-only runtime preview includes observation capability header");
            t.check(text.find("Row domain: valid") != std::string::npos,
                "spatial-only runtime preview reports row-domain validity");
            t.check(text.find("Temporal: invalid - no semantic time axis") != std::string::npos,
                "spatial-only runtime preview explains temporal invalidity");
            t.check(text.find("Spatial: valid") != std::string::npos,
                "spatial-only runtime preview explains spatial validity");
            t.check(text.find("Spatial panel: conditional - provide entity grouping schema") != std::string::npos,
                "spatial-only runtime preview explains spatial-panel conditionality");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "spatial-only runtime preview exposes built-in spatial topology inventory");
            t.check(text.find("time_kind: <none>") != std::string::npos,
                "spatial-only runtime preview reports missing time kind");
            t.check(text.find("time_resolution: <none>") != std::string::npos,
                "spatial-only runtime preview reports missing time resolution");
            t.check(text.find("row_domain: 0 .. 2") != std::string::npos,
                "spatial-only runtime preview reports row domain");
            t.check(text.find("latest: 30.0000 @ row#2") != std::string::npos,
                "spatial-only runtime preview anchors latest value to row domain");
            t.check(text.find("time_range:") == std::string::npos,
                "spatial-only runtime preview avoids fabricated time range");
            t.check(text.find("rows: 0") == std::string::npos,
                "spatial-only runtime preview no longer collapses to zero rows");
            return t.failed == 0;
        }

        bool test_align_dump_reports_observation_capabilities(TestContext& t) {
            t.out << "[app_dispatch] align-dump observation capability reporting\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "temporal-panel align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = fixture.spec_path;
            cmd.align_dump.csv_path = fixture.csv_path;
            cmd.align_dump.series_key_col = "symbol";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "temporal-panel align-dump returns success");
            t.check(err.str().empty(), "temporal-panel align-dump produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "align-dump output includes observation capability header");
            t.check(text.find("Temporal panel: valid") != std::string::npos,
                "align-dump reports temporal panel validity");
            t.check(text.find("Spatial panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "align-dump reports spatial panel conditionality");
            t.check(text.find("Spatiotemporal panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "align-dump reports spatiotemporal panel conditionality");
            t.check(text.find("builtin.temporal.window") != std::string::npos,
                "align-dump exposes built-in temporal topology inventory");
            return t.failed == 0;
        }

        bool test_align_dump_rejects_non_temporal_multiseries_alignment(TestContext& t) {
            t.out << "[app_dispatch] align-dump rejects non-temporal multi-series alignment\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = spec_path;
            cmd.align_dump.csv_path = csv_path;
            cmd.align_dump.series_key_col = "region";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();
            t.check(rc == 1, "spatial-panel align-dump returns failure for unsupported non-temporal multi-series alignment");
            t.check(text.find("Observation capability") != std::string::npos,
                "spatial-panel align-dump still reports observation capability before policy failure");
            t.check(text.find("Spatial panel: valid") != std::string::npos,
                "spatial-panel align-dump reports spatial panel validity before policy failure");
            t.check(text.find("Temporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "spatial-panel align-dump explains temporal panel incompatibility");
            t.check(err.str().find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
                "spatial-panel align-dump reports explicit non-temporal alignment policy failure");
            return t.failed == 0;
        }

        bool test_align_dump_honors_dataspec_non_temporal_policy(TestContext& t) {
            t.out << "[app_dispatch] align-dump honors DataSpec non-temporal multi-series policy\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err, true);
            t.check(fixture_ok, "policy-enabled spatial-panel align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = spec_path;
            cmd.align_dump.csv_path = csv_path;
            cmd.align_dump.series_key_col = "region";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "policy-enabled spatial-panel align-dump returns success");
            t.check(err.str().empty(), "policy-enabled spatial-panel align-dump produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "policy-enabled spatial-panel align-dump still reports observation capability");
            t.check(text.find("series_count=2") != std::string::npos,
                "policy-enabled spatial-panel align-dump reports aligned series count");
            t.check(text.find("series[0]=A") != std::string::npos || text.find("series[1]=A") != std::string::npos,
                "policy-enabled spatial-panel align-dump reports stable aligned series identities");
            return t.failed == 0;
        }

        bool test_plan_capability_report_surfaces_topology_conditions(TestContext& t) {
            t.out << "[app_reporters] plan capability topology reporting\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "plan capability topology fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(fixture.spec_path, ds, err);
            t.check(load_ok, "plan capability topology fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, fixture.csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "plan capability topology fixture builds runtime input");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("topology_names: builtin.spatial.metric_2d") != std::string::npos,
                "plan capability report summarizes resolved topology names");
            t.check(text.find("topology=builtin.spatial.metric_2d") != std::string::npos,
                "plan capability invalid-node detail includes resolved topology name");
            t.check(text.find("missing required topology family: spatial_metric_2d") != std::string::npos,
                "plan capability report explains missing required topology family");
            t.check(text.find("provide spatial neighborhood/index schema") != std::string::npos,
                "plan capability topology-family failure preserves providable spatial hint");
            t.check(text.find("missing named topology: 'builtin.spatial.metric_2d'") != std::string::npos,
                "plan capability report explains missing built-in named topology when spatial topology is absent");
            return t.failed == 0;
        }

        bool test_plan_capability_report_surfaces_locality_subject_provenance(TestContext& t) {
            t.out << "[app_reporters] plan capability locality-subject provenance\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatiotemporal_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatiotemporal locality-subject fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatiotemporal locality-subject fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "spatiotemporal locality-subject fixture builds runtime input");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 0.2},
                    {"subject_series", std::string("B")}
                } },
            };

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("status: valid") != std::string::npos,
                "plan capability report remains valid for explicit locality-subject binding");
            t.check(text.find("topology_usage:") != std::string::npos,
                "plan capability report prints topology usage summary");
            t.check(text.find("mean (spatial.radius_mean): topology=builtin.spatial.metric_2d") != std::string::npos,
                "plan capability topology usage summary includes resolved topology name");
            t.check(text.find("locality=series_key(B) -> series=B, entity=B") != std::string::npos,
                "plan capability topology usage summary includes resolved locality subject provenance");
            return t.failed == 0;
        }

        bool test_plan_capability_report_hints_same_series_alternative_for_ms_diff(TestContext& t) {
            t.out << "[app_reporters] cross-series hint for ms.diff on single-series data\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "single-series ms.diff hint fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "single-series ms.diff hint layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "single-series ms.diff hint observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "single-series ms.diff hint runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "spread", "ms.diff", { "c", "c" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("requires multiple aligned series") != std::string::npos,
                "plan capability report explains the cross-series requirement");
            t.check(text.find("--align-dump") != std::string::npos,
                "plan capability report points users to align-dump for series_count discovery");
            t.check(text.find("use ts.sub instead") != std::string::npos,
                "plan capability report suggests ts.sub for same-series subtraction");
            return t.failed == 0;
        }

        bool test_plan_capability_report_hints_bool_op_node_list(TestContext& t) {
            t.out << "[app_reporters] bool op in numeric_nodes hint\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "bool-op node-list hint fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "bool-op node-list hint layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "bool-op node-list hint observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "bool-op node-list hint runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "z", "core.const", {}, { {"value", 0.0} } },
                sugar::NodeSpec{ "bad", "cmp.gt", { "c", "z" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("operator type mismatch") != std::string::npos,
                "plan capability report still classifies the node-list type mismatch");
            t.check(text.find("move this bool operator to `bool_nodes`") != std::string::npos,
                "plan capability report tells users which node list should contain the bool op");
            t.check(text.find("core.select") != std::string::npos,
                "plan capability report points core.select users at the bool-consuming numeric path");
            return t.failed == 0;
        }

        bool test_align_dump_accepts_wrapped_layout_spec(TestContext& t) {
            t.out << "[app_dispatch] align-dump accepts wrapped LayoutSpec\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_panel_ranked_fixture(fixture, fixture_err);
            t.check(fixture_ok, "wrapped LayoutSpec align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = fixture.layout_path;
            cmd.align_dump.csv_path = fixture.csv_path;
            cmd.align_dump.series_key_col = "symbol";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "wrapped LayoutSpec align-dump returns success");
            t.check(err.str().empty(), "wrapped LayoutSpec align-dump produces no error output");
            t.check(text.find("Alignment dump") != std::string::npos,
                "wrapped LayoutSpec align-dump reaches the aligned output surface");
            t.check(text.find("series_count=2") != std::string::npos,
                "wrapped LayoutSpec align-dump reports the aligned series count");
            t.check(text.find("builtin.temporal.window") != std::string::npos,
                "wrapped LayoutSpec align-dump preserves topology guidance");
            return t.failed == 0;
        }

        bool test_plan_capability_report_hints_bool_node_numeric_bridge(TestContext& t) {
            t.out << "[app_reporters] bool node used in numeric arg hint\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "bool-node numeric-bridge fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "bool-node numeric-bridge layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "bool-node numeric-bridge observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "bool-node numeric-bridge runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "z", "core.const", {}, { {"value", 0.0} } },
                sugar::NodeSpec{ "bad", "ts.sma", { "flag" }, { {"n", 2.0} } },
            };
            spec.bool_nodes = {
                sugar::NodeSpec{ "flag", "cmp.gt", { "c", "z" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("flag is a bool node; to use bool output in a numeric context, use `core.select`") != std::string::npos,
                "plan capability report explains how to bridge bool nodes into numeric context");
            t.check(text.find("unknown numeric node: flag") == std::string::npos,
                "plan capability report avoids the misleading unknown-numeric-node wording for bool references");
            return t.failed == 0;
        }

        bool test_runtime_graph_preview_restores_stream_formatting_for_run_reports(TestContext& t) {
            t.out << "[app_reporters] runtime preview restores stream formatting\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "runtime-preview formatting fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "runtime-preview formatting layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "runtime-preview formatting observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "runtime-preview formatting runtime binds");
            if (!runtime_ok) return false;

            sugar::BacktestResult result{};
            result.pnl = 12.5;
            result.trades = 20;
            result.max_drawdown = 3.25;
            result.best_start_date = 20211123;
            result.has_best_start_date = true;
            result.trade_hash = 13693342942469747168ull;

            const sugar::DataView dv(*runtime.aligned_bundle);
            std::ostringstream out;
            sugar::print_runtime_graph_preview(
                dv,
                runtime.primary_ref,
                runtime.run_manifest,
                "formatting-test",
                out,
                8,
                nullptr);
            sugar::print_backtest_summary(result, out);
            const std::string text = out.str();

            t.check(text.find("Trades: 20\n") != std::string::npos,
                "run summary keeps integer trade counts after runtime preview output");
            t.check(text.find("trades [core_metric]: 20\n") != std::string::npos,
                "metric catalog keeps integer trade counts after runtime preview output");
            t.check(text.find("first_usable_signal_time_key [core_metric]: 20211123\n") != std::string::npos,
                "metric catalog prints time-key metrics without scientific notation");
            t.check(text.find("trade_hash [core_metric]: 13693342942469747168\n") != std::string::npos,
                "metric catalog preserves exact trade_hash integers instead of scientific notation");
            t.check(text.find("20.0000") == std::string::npos,
                "runtime preview formatting no longer leaks fixed precision into later run reports");
            return t.failed == 0;
        }

        bool test_layout_parser_rejects_data_spec_wrapper_with_explicit_hint(TestContext& t) {
            t.out << "[layout_spec] unsupported data_spec wrapper hint\n";
            const std::string json =
                "{\n"
                "  \"data_spec\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\"\n"
                "  }\n"
                "}\n";

            sugar::LayoutSpec layout{};
            std::string err;
            const bool ok = sugar::parse_layout_spec_json(json, layout, err);
            t.check(!ok, "layout parser rejects unsupported data_spec wrapper");
            t.check(err.find("raw DataSpec object") != std::string::npos,
                "layout parser error points users at raw dataspec root shape");
            t.check(err.find("`ingest`") != std::string::npos,
                "layout parser error points users at the supported ingest wrapper");
            return t.failed == 0;
        }

        bool test_layout_parser_hints_wrapped_layout_field_placement(TestContext& t) {
            t.out << "[layout_spec] wrapped layout misplaced root field hint\n";
            const std::string json =
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\"\n"
                "  },\n"
                "  \"alignment_calendar\": \"Intersection\",\n"
                "  \"topologies\": []\n"
                "}\n";

            sugar::LayoutSpec layout{};
            std::string err;
            const bool ok = sugar::parse_layout_spec_json(json, layout, err);
            t.check(!ok, "layout parser rejects misplaced wrapped alignment key");
            t.check(err.find("alignment_calendar") != std::string::npos &&
                    err.find("alignment.calendar") != std::string::npos,
                "layout parser points wrapped users at alignment.calendar");
            return t.failed == 0;
        }

        bool test_layout_parser_hints_header_policy_first_row(TestContext& t) {
            t.out << "[layout_spec] header_policy first_row hint\n";
            const std::string json =
                "{\n"
                "  \"time_col\": \"date\",\n"
                "  \"time_kind\": \"YyyyMmDd\",\n"
                "  \"header_policy\": \"first_row\",\n"
                "  \"value_cols\": [\"close\"],\n"
                "  \"primary_value_col\": \"close\"\n"
                "}\n";

            sugar::LayoutSpec layout{};
            std::string err;
            const bool ok = sugar::parse_layout_spec_json(json, layout, err);
            t.check(!ok, "layout parser rejects intuitive but invalid header_policy value");
            t.check(err.find("RequiredHeaders") != std::string::npos &&
                    err.find("first row contains column names") != std::string::npos,
                "layout parser explains the header_policy replacement for first_row");
            return t.failed == 0;
        }

    } // namespace

    void register_preflight_diagnostic_tests(AppTestRegistry& tests) {
        tests.push_back(test_feature_dump_dispatch_smoke);
        tests.push_back(test_prep_dump_dispatch_smoke);
        tests.push_back(test_prep_dump_spatial_output_is_row_domain_honest);
        tests.push_back(test_prep_dump_reports_observation_capabilities);
        tests.push_back(test_prep_dump_reports_panel_capability_incompatibility);
        tests.push_back(test_runtime_graph_preview_spatial_output_is_row_domain_honest);
        tests.push_back(test_align_dump_reports_observation_capabilities);
        tests.push_back(test_align_dump_rejects_non_temporal_multiseries_alignment);
        tests.push_back(test_align_dump_honors_dataspec_non_temporal_policy);
        tests.push_back(test_plan_capability_report_surfaces_topology_conditions);
        tests.push_back(test_plan_capability_report_surfaces_locality_subject_provenance);
        tests.push_back(test_plan_capability_report_hints_same_series_alternative_for_ms_diff);
        tests.push_back(test_plan_capability_report_hints_bool_op_node_list);
        tests.push_back(test_align_dump_accepts_wrapped_layout_spec);
        tests.push_back(test_plan_capability_report_hints_bool_node_numeric_bridge);
        tests.push_back(test_runtime_graph_preview_restores_stream_formatting_for_run_reports);
        tests.push_back(test_layout_parser_rejects_data_spec_wrapper_with_explicit_hint);
        tests.push_back(test_layout_parser_hints_wrapped_layout_field_placement);
        tests.push_back(test_layout_parser_hints_header_policy_first_row);
    }

} // namespace sugar::app::tests
