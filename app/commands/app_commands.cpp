#include "commands/app_commands.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reporting/app_reporters.h"
#include "runtime/app_runtime.h"

#include "app_support/feature_dump.h"
#include "contracts/contract_suite.h"
#include "ingest/utils.h"
#include "sweep/evaluation_report.h"
#include "sweep/dynamic_execution.h"
#include "sweep/panel_runner.h"
#include "sweep/strategy_menu.h"

namespace {

    sugar::RankingObjectiveSpec make_drawdown_weighted_pnl_objective(double dd_weight) {
        sugar::RankingObjectiveSpec objective{};
        objective.primary.source = sugar::ObjectiveValueSource::ReportedMetric;
        objective.primary.name = "pnl";
        objective.direction = sugar::ObjectiveDirection::Maximize;
        sugar::ObjectivePenaltySpec penalty{};
        penalty.value.source = sugar::ObjectiveValueSource::ReportedMetric;
        penalty.value.name = "max_drawdown";
        penalty.weight = dd_weight;
        objective.penalties.push_back(std::move(penalty));
        return objective;
    }

    std::optional<std::pair<sugar::TimeKey, sugar::TimeKey>> time_minmax(const sugar::NumericFrame& f) {
        if (!f.has_time_axis()) return std::nullopt;
        const auto t = f.time();
        if (t.empty()) return std::nullopt;
        const auto [mn, mx] = std::minmax_element(t.begin(), t.end());
        return std::make_pair(*mn, *mx);
    }

    sugar::PanelRankingSpec default_panel_ranking_spec() {
        sugar::PanelRankingSpec spec{};
        spec.objective = make_drawdown_weighted_pnl_objective(0.25);
        return spec;
    }

    std::size_t observation_world_series_count(const sugar::ResolvedObservationWorld& world) {
        if (world.aligned_bundle) {
            return world.aligned_bundle->series_keys.size();
        }
        if (world.frameset) {
            return world.frameset->by_entity.size();
        }
        return 0;
    }

    bool observation_world_is_multi_series(const sugar::ResolvedObservationWorld& world) {
        return observation_world_series_count(world) > 1;
    }

    bool resolve_target_entity_series_key(
        const sugar::ResolvedObservationWorld& world,
        const std::string& target_entity,
        std::string& out_series_key,
        std::string& err) {
        out_series_key.clear();
        if (target_entity.empty()) {
            err = "logic.run.target_entity is empty";
            return false;
        }

        std::vector<std::string> matches;
        for (const auto& [series_key, entity_key] : world.series_key_to_entity) {
            if (entity_key == target_entity) {
                matches.push_back(series_key);
            }
        }

        if (matches.empty()) {
            err = "logic.run.target_entity '" + target_entity + "' is not present in the observation world";
            return false;
        }
        if (matches.size() > 1) {
            std::sort(matches.begin(), matches.end());
            err = "logic.run.target_entity '" + target_entity + "' is ambiguous across multiple series";
            return false;
        }

        out_series_key = matches.front();
        return true;
    }

    bool resolve_world_primary_col_id(
        const sugar::ResolvedObservationWorld& world,
        sugar::ColumnId& out,
        std::string& err) {
        if (!world.aligned_bundle || !world.aligned_bundle->schema || !world.ingest_spec.has_value()) {
            err = "observation world is missing aligned schema";
            return false;
        }
        const std::string& primary_name = world.ingest_spec->primary_value_col;
        if (!world.aligned_bundle->schema->columns().find(primary_name, out)) {
            err = "layout primary_value_col '" + primary_name + "' is not present in aligned schema";
            return false;
        }
        return true;
    }

    bool load_layout_and_world(
        const std::string& layout_path,
        const std::string& csv_path,
        sugar::LayoutSpec& layout,
        sugar::ResolvedObservationWorld& world,
        std::string& err) {
        std::string layout_err;
        if (!sugar::load_layout_spec_file(layout_path, layout, layout_err)) {
            err = "failed to load layout spec: " + layout_err;
            return false;
        }

        std::string world_err;
        if (!sugar::build_resolved_observation_world(layout, csv_path, world, world_err)) {
            err = world_err;
            return false;
        }

        return true;
    }

    bool load_logic_for_report(
        const std::string& logic_path,
        sugar::PlanSpec& spec,
        std::string& err) {
        bool has_embedded_observations = false;
        std::string plan_err;
        if (!sugar::app::load_noninteractive_planspec(
                logic_path,
                spec,
                has_embedded_observations,
                plan_err)) {
            err = "failed to load logic spec: " + plan_err;
            return false;
        }
        if (has_embedded_observations) {
            err = "logic.json embeds legacy observation inputs. That compatibility path is interactive-only.";
            return false;
        }
        return true;
    }

    bool contains_text(const std::string& text, std::string_view needle) {
        return text.find(needle) != std::string::npos;
    }

    std::string classify_layout_load_error(const std::string& message) {
        if (contains_text(message, "time_kind")) {
            return "dataspec.invalid_time_kind";
        }
        if (contains_text(message, "Failed to open file")) {
            return "layout.open_failed";
        }
        if (contains_text(message, "line ") && contains_text(message, "col ")) {
            return "layout.parse_failed";
        }
        if (contains_text(message, "`data_spec` is not a supported wrapper key")) {
            return "layout.unsupported_wrapper";
        }
        if (contains_text(message, "layout.ingest:")) {
            return "dataspec.invalid";
        }
        return "layout.invalid";
    }

    std::string classify_observation_world_error(
        const sugar::ResolvedObservationWorld& world,
        const std::string& message) {
        if (!world.run_manifest.preprocess.preprocess_error_code.empty()) {
            return world.run_manifest.preprocess.preprocess_error_code;
        }
        if (contains_text(message, "alignment failed")) {
            return "alignment.failed";
        }
        if (contains_text(message, "layout topology registration failed")) {
            return "layout.topology_registration_failed";
        }
        return "observation_world.invalid";
    }

    const char* valid_run_scope_values_text() {
        return "valid values: primary_series or per_entity_ranked";
    }

    std::string missing_run_scope_hint_text() {
        return std::string("multi-entity runs require logic.run.scope in logic.json; ") +
            valid_run_scope_values_text();
    }

    void print_obs_report_next_step(
        std::ostream& err,
        const sugar::app::AppCommand& cmd) {
        err << "Next step: run `sugarbot --obs-report "
            << cmd.run.layout_path << " " << cmd.run.csv_path
            << "` to isolate CSV/layout ingest and observation-world issues.\n";
    }

    void print_plan_report_next_step(
        std::ostream& err,
        const sugar::app::AppCommand& cmd) {
        err << "Next step: run `sugarbot --plan-report "
            << cmd.run.layout_path << " " << cmd.run.logic_path << " " << cmd.run.csv_path
            << "` to isolate PlanSpec bindings, operators, run scope, and capability mismatches.\n";
    }

    void print_align_dump_next_step(
        std::ostream& err,
        const sugar::app::AppCommand& cmd,
        const sugar::LayoutSpec& layout) {
        const std::string series_key_col =
            layout.ingest.entity_col.has_value() && !layout.ingest.entity_col->empty()
            ? *layout.ingest.entity_col
            : std::string("<series-key-col>");
        err << "Next step: run `sugarbot --align-dump "
            << cmd.run.layout_path << " " << cmd.run.csv_path
            << " --series-key-col " << series_key_col
            << "` to inspect aligned series/entity lanes.\n";
    }

    void print_observation_stage_header(std::ostream& out, const char* label) {
        out << "=== " << label << " ===\n";
    }

    void print_prebinding_assessment_header(std::ostream& out, bool has_post_binding_stage) {
        print_observation_stage_header(
            out,
            has_post_binding_stage
            ? "[1/2] Pre-binding observation assessment"
            : "[1/1] Pre-binding observation assessment");
    }

    void print_runtime_binding_header(std::ostream& out) {
        print_observation_stage_header(out, "DAG runtime binding");
    }

    void print_postbinding_assessment_header(std::ostream& out) {
        print_observation_stage_header(out, "[2/2] Post-binding runtime assessment");
    }

    void print_plan_execution_validation_report(
        const sugar::PlanSpec& spec,
        const sugar::PlanDynamicExecutionValidationReport& report,
        std::ostream& out,
        std::string_view label) {
        if (!report.execution_section_present) {
            return;
        }

        const auto& execution = *spec.execution;
        out << "Execution validation";
        if (!label.empty()) {
            out << " (" << label << ")";
        }
        out << "\n";
        out << "  mode: " << sugar::plan_execution_mode_name(execution.mode) << "\n";
        out << "  status: " << (report.valid ? "valid" : "invalid") << "\n";
        out << "  subgraphs: " << execution.subgraphs.size() << "\n";
        out << "  routes: " << execution.routes.size() << "\n";
        out << "  outputs: " << execution.outputs.size() << "\n";
        out << "  bounds.max_routes: " << execution.bounds.max_routes << "\n";
        out << "  bounds.max_active_subgraphs: " << execution.bounds.max_active_subgraphs << "\n";
        out << "  bounds.max_selected_outputs: " << execution.bounds.max_selected_outputs << "\n";
        out << "  bounds.decision_sample_limit: " << execution.bounds.decision_sample_limit << "\n";
        if (!report.dynamic_enabled) {
            out << "  static_validator: skipped (mode is static)\n";
        }
        else {
            out << "  static_validator: " << (report.valid ? "passed" : "failed") << "\n";
        }
        if (!report.issues.empty()) {
            out << "  issues:\n";
            for (const auto& issue : report.issues) {
                out << "    - code: "
                    << sugar::plan_dynamic_execution_validation_code_name(issue.code)
                    << " subject: " << issue.subject
                    << " detail: " << issue.detail << "\n";
            }
        }
        out << "\n";
    }

} // namespace

namespace sugar::app {

    int run_obs_report_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (cmd.obs_report.json_output) {
            std::string layout_err;
            if (!sugar::load_layout_spec_file(cmd.obs_report.layout_path, layout, layout_err)) {
                print_observation_report_error_json(
                    classify_layout_load_error(layout_err),
                    "failed to load layout spec: " + layout_err,
                    nullptr,
                    out);
                return 1;
            }

            std::string world_err;
            if (!sugar::build_resolved_observation_world(
                    layout,
                    cmd.obs_report.csv_path,
                    world,
                    world_err)) {
                const auto code = classify_observation_world_error(world, world_err);
                const auto* report =
                    !world.run_manifest.preprocess.preprocess_error_code.empty() ||
                        !world.run_manifest.preprocess.preprocess_error_message.empty()
                    ? &world.run_manifest.preprocess
                    : nullptr;
                print_observation_report_error_json(
                    code,
                    world_err,
                    report,
                    out);
                return 1;
            }

            print_observation_report_json(world, out);
            return 0;
        }

        if (!load_layout_and_world(
                cmd.obs_report.layout_path,
                cmd.obs_report.csv_path,
                layout,
                world,
                load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        out << "[Info] obs-report active.\n";
        print_observation_capability_report(world, out);
        return 0;
    }

    int run_plan_report_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (!load_layout_and_world(cmd.plan_report.layout_path, cmd.plan_report.csv_path, layout, world, load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        sugar::PlanSpec spec{};
        std::string logic_err;
        if (!load_logic_for_report(cmd.plan_report.logic_path, spec, logic_err)) {
            err << "Error: " << logic_err << "\n";
            return 1;
        }

        out << "[Info] plan-report active.\n";
        const bool will_bind_runtime = !spec.run.has_value() ? !observation_world_is_multi_series(world) :
            spec.run->scope == sugar::PlanRunScope::PrimarySeries;
        print_prebinding_assessment_header(out, will_bind_runtime);
        print_observation_capability_report(world, out);
        const auto execution_report = sugar::validate_plan_dynamic_execution(spec);
        if (!execution_report.valid) {
            print_plan_execution_validation_report(spec, execution_report, out, "pre-compatibility");
            return 1;
        }

        const bool multi_series = observation_world_is_multi_series(world);
        const auto facts = sugar::make_observation_world_facts(world);
        auto print_execution_report = [&](std::string_view label) {
            print_plan_execution_validation_report(spec, execution_report, out, label);
        };

        auto print_facts_report = [&]() {
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, facts);
            print_plan_capability_report(plan_report, out, "pre-binding observation-world");
            print_execution_report("pre-binding observation-world");
            print_plan_data_quality_notes(spec, world.run_manifest.preprocess, out);
            print_plan_binding_notes(spec, out);
            return plan_report;
        };

        if (!spec.run.has_value()) {
            if (multi_series) {
                const auto plan_report = print_facts_report();
                err << "Error: multi-entity plan-report requires logic.run.scope for runtime-targeted diagnostics; "
                    << valid_run_scope_values_text() << "\n";
                return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out, "post-binding runtime-target");
            print_execution_report("post-binding runtime-target");
            print_plan_data_quality_notes(spec, runtime.run_manifest.preprocess, out);
            print_plan_binding_notes(spec, out);
            return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope == sugar::PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.objective_spec.has_value()) {
                const auto plan_report = print_facts_report();
                err << "Error: logic.run.top_k, logic.run.objective, and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked\n";
                return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
            }

            sugar::RuntimeTargetSpec target{};
            target.require_primary_series_on_multi_series = multi_series;
            if (multi_series) {
                if (run.target_entity.empty()) {
                    const auto plan_report = print_facts_report();
                    err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    const auto plan_report = print_facts_report();
                    err << "Error: " << resolve_err << "\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                target.primary_series_key = std::move(series_key);
            }
            else if (!run.target_entity.empty()) {
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    const auto plan_report = print_facts_report();
                    err << "Error: " << resolve_err << "\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                target.primary_series_key = std::move(series_key);
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    target,
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            out << "[Info] plan-report scope: primary_series";
            if (!runtime.primary_entity_key.empty()) {
                out << " target_entity=" << runtime.primary_entity_key;
            }
            out << "\n";
            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out, "post-binding runtime-target");
            print_execution_report("post-binding runtime-target");
            print_plan_data_quality_notes(spec, runtime.run_manifest.preprocess, out);
            print_plan_binding_notes(spec, out);
            return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
        }

        const auto plan_report = print_facts_report();
        if (!multi_series) {
            err << "Error: logic.run.scope=per_entity_ranked requires multi-entity observation input\n";
            return 1;
        }
        if (!run.top_k.has_value()) {
            err << "Error: logic.run.top_k is required when logic.run.scope=per_entity_ranked\n";
            return 1;
        }
        if (!run.target_entity.empty()) {
            err << "Error: logic.run.target_entity does not apply when logic.run.scope=per_entity_ranked\n";
            return 1;
        }

        sugar::PanelRankingSpec ranking{};
        std::string ranking_err;
        if (!sugar::build_panel_ranking_spec(run, ranking, ranking_err)) {
            err << "Error: " << ranking_err << "\n";
            return 1;
        }

        out << "[Info] plan-report scope: per_entity_ranked top_k=" << *run.top_k
            << " ranking_objective=" << sugar::describe_ranking_objective(
                sugar::build_panel_ranking_objective(ranking))
            << "\n";

        return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
    }

    int run_feature_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (!load_layout_and_world(cmd.feature_dump.layout_path, cmd.feature_dump.csv_path, layout, world, load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        sugar::PlanSpec spec{};
        std::string logic_err;
        if (!load_logic_for_report(cmd.feature_dump.logic_path, spec, logic_err)) {
            err << "Error: " << logic_err << "\n";
            return 1;
        }

        const bool multi_series = observation_world_is_multi_series(world);

        if (!spec.run.has_value()) {
            if (multi_series) {
                err << "Error: multi-entity feature-dump requires logic.run.scope=primary_series in logic.json\n";
                return 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            std::string dump_err;
            if (!dump_feature_table(runtime, spec, out, dump_err)) {
                err << "Error: " << dump_err << "\n";
                return 1;
            }
            return 0;
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope != sugar::PlanRunScope::PrimarySeries) {
            err << "Error: feature-dump supports only single-series input or logic.run.scope=primary_series\n";
            return 1;
        }
        if (run.top_k.has_value() || run.objective_spec.has_value()) {
            err << "Error: feature-dump does not use logic.run.top_k, logic.run.objective, or logic.run.ranking_*\n";
            return 1;
        }

        sugar::RuntimeTargetSpec target{};
        target.require_primary_series_on_multi_series = multi_series;
        if (multi_series) {
            if (run.target_entity.empty()) {
                err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                return 1;
            }
            std::string series_key;
            std::string resolve_err;
            if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                err << "Error: " << resolve_err << "\n";
                return 1;
            }
            target.primary_series_key = std::move(series_key);
        }
        else if (!run.target_entity.empty()) {
            std::string series_key;
            std::string resolve_err;
            if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                err << "Error: " << resolve_err << "\n";
                return 1;
            }
            target.primary_series_key = std::move(series_key);
        }

        sugar::ResolvedRuntimeInput runtime{};
        std::string bind_err;
        if (!sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                target,
                runtime,
                bind_err)) {
            err << "Error: " << bind_err << "\n";
            return 1;
        }

        std::string dump_err;
        if (!dump_feature_table(runtime, spec, out, dump_err)) {
            err << "Error: " << dump_err << "\n";
            return 1;
        }
        return 0;
    }

    bool planspec_has_sweep(const sugar::PlanSpec& spec) {
        return spec.sweep.has_value() &&
            (spec.sweep->enabled || !spec.sweep->params.empty());
    }

    int run_custom_plan_spec_with_projection(
        const sugar::DataView& dv,
        const sugar::ColumnRef& primary,
        const sugar::PlanSpec& spec,
        const sugar::PlanSweepRuntimeOptions& rt,
        sugar::EvaluationReportProjection projection,
        std::ostream& out,
        const sugar::RunProvenanceSummary& run_provenance) {
        if (projection == sugar::EvaluationReportProjection::Full) {
            const bool ran = sugar::run_custom_plan_spec(
                dv,
                primary,
                spec,
                rt,
                out,
                nullptr,
                nullptr,
                &run_provenance);
            return ran ? 0 : 1;
        }

        std::ostringstream execution_out;
        sugar::BacktestResult result{};
        sugar::PlanSweepReport sweep_report{};
        const bool ran = sugar::run_custom_plan_spec(
            dv,
            primary,
            spec,
            rt,
            execution_out,
            &result,
            &sweep_report,
            &run_provenance);
        if (!ran) {
            if ((projection == sugar::EvaluationReportProjection::Json ||
                    projection == sugar::EvaluationReportProjection::PosteriorDiagnostics) &&
                planspec_has_sweep(spec) &&
                sweep_report.requested_sweep.engine ==
                    sugar::PlanSweepEngineKind::BayesianPosterior &&
                sweep_report.posterior.requested) {
                sugar::print_evaluation_report(
                    sugar::make_sweep_evaluation_report(std::move(sweep_report)),
                    projection,
                    out);
                return 1;
            }
            out << execution_out.str();
            return 1;
        }

        if (planspec_has_sweep(spec)) {
            sugar::print_evaluation_report(
                sugar::make_sweep_evaluation_report(std::move(sweep_report)),
                projection,
                out);
            return 0;
        }

        sugar::print_evaluation_report(
            sugar::make_run_evaluation_report(
                sugar::build_run_report(result, nullptr, &run_provenance)),
            projection,
            out);
        return 0;
    }

    int run_unified_cli_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        std::string layout_err;
        if (!sugar::load_layout_spec_file(cmd.run.layout_path, layout, layout_err)) {
            err << "Error: failed to load layout spec: " << layout_err << "\n";
            print_obs_report_next_step(err, cmd);
            return 1;
        }

        sugar::PlanSpec spec{};
        bool has_embedded_observations = false;
        std::string plan_err;
        if (!sugar::app::load_noninteractive_planspec(
                cmd.run.logic_path,
                spec,
                has_embedded_observations,
                plan_err)) {
            err << "Error: failed to load logic spec: " << plan_err << "\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }
        if (has_embedded_observations) {
            err << "Error: logic.json embeds legacy observation inputs. That compatibility path is interactive-only.\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }

        sugar::ResolvedObservationWorld world{};
        std::string world_err;
        if (!sugar::build_resolved_observation_world(layout, cmd.run.csv_path, world, world_err)) {
            err << "Error: " << world_err << "\n";
            print_obs_report_next_step(err, cmd);
            return 1;
        }

        const bool json_projection =
            sugar::evaluation_report_projection_is_machine_readable(
                cmd.run.report_projection);
        const bool topology_projection =
            cmd.run.report_projection == sugar::EvaluationReportProjection::TopologyDiagnostics;
        const bool focused_projection = json_projection || topology_projection;
        if (!focused_projection) {
            out << "[Info] noninteractive artifact run active.\n";
        }
        const bool will_bind_runtime = !spec.run.has_value() ? !observation_world_is_multi_series(world) :
            spec.run->scope == sugar::PlanRunScope::PrimarySeries;
        if (!focused_projection) {
            print_prebinding_assessment_header(out, will_bind_runtime);
            print_observation_capability_report(world, out);
        }

        const bool multi_series = observation_world_is_multi_series(world);
        const auto facts = sugar::make_observation_world_facts(world);

        if (!spec.run.has_value()) {
            if (multi_series) {
                err << "Error: " << missing_run_scope_hint_text() << "\n";
                print_plan_report_next_step(err, cmd);
                print_align_dump_next_step(err, cmd, layout);
                return 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            if (!focused_projection) {
                print_runtime_binding_header(out);
            }
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                print_obs_report_next_step(err, cmd);
                return 1;
            }

            if (!focused_projection) {
                print_postbinding_assessment_header(out);
                print_observation_capability_report(runtime, out);
            }
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            if (!focused_projection ||
                (topology_projection && plan_report.status != sugar::PlanCapabilityStatus::Valid)) {
                print_plan_capability_report(plan_report, out);
                print_plan_data_quality_notes(spec, runtime.run_manifest.preprocess, out);
            }
            if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
                err << "Error: logic.json is not compatible with the resolved runtime target\n";
                print_plan_report_next_step(err, cmd);
                return 1;
            }
            sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);

            const sugar::DataView dv(*runtime.aligned_bundle);
            sugar::PlanSweepRuntimeOptions rt{};
            const auto run_provenance = sugar::build_run_provenance_summary(runtime.run_manifest);
            return run_custom_plan_spec_with_projection(
                dv,
                runtime.primary_ref,
                spec,
                rt,
                cmd.run.report_projection,
                out,
                run_provenance);
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope == sugar::PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.objective_spec.has_value()) {
                err << "Error: logic.run.top_k, logic.run.objective, and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked\n";
                print_plan_report_next_step(err, cmd);
                return 1;
            }

            sugar::RuntimeTargetSpec target{};
            target.require_primary_series_on_multi_series = multi_series;
            if (multi_series) {
                if (run.target_entity.empty()) {
                    err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                    print_plan_report_next_step(err, cmd);
                    print_align_dump_next_step(err, cmd, layout);
                    return 1;
                }
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    err << "Error: " << resolve_err << "\n";
                    print_align_dump_next_step(err, cmd, layout);
                    return 1;
                }
                target.primary_series_key = std::move(series_key);
            }
            else if (!run.target_entity.empty()) {
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    err << "Error: " << resolve_err << "\n";
                    print_obs_report_next_step(err, cmd);
                    return 1;
                }
                target.primary_series_key = std::move(series_key);
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            if (!focused_projection) {
                print_runtime_binding_header(out);
            }
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    target,
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                print_align_dump_next_step(err, cmd, layout);
                return 1;
            }

            if (!focused_projection) {
                out << "[Info] run scope: primary_series";
                if (!runtime.primary_entity_key.empty()) {
                    out << " target_entity=" << runtime.primary_entity_key;
                }
                out << "\n";
            }
            if (!focused_projection) {
                print_postbinding_assessment_header(out);
                print_observation_capability_report(runtime, out);
            }
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            if (!focused_projection ||
                (topology_projection && plan_report.status != sugar::PlanCapabilityStatus::Valid)) {
                print_plan_capability_report(plan_report, out);
                print_plan_data_quality_notes(spec, runtime.run_manifest.preprocess, out);
            }
            if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
                err << "Error: logic.json is not compatible with the resolved runtime target\n";
                print_plan_report_next_step(err, cmd);
                return 1;
            }
            sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);

            const sugar::DataView dv(*runtime.aligned_bundle);
            sugar::PlanSweepRuntimeOptions rt{};
            const auto run_provenance = sugar::build_run_provenance_summary(runtime.run_manifest);
            return run_custom_plan_spec_with_projection(
                dv,
                runtime.primary_ref,
                spec,
                rt,
                cmd.run.report_projection,
                out,
                run_provenance);
        }

        if (!multi_series) {
            err << "Error: logic.run.scope=per_entity_ranked requires multi-entity observation input\n";
            print_obs_report_next_step(err, cmd);
            print_plan_report_next_step(err, cmd);
            return 1;
        }
        if (!run.top_k.has_value()) {
            err << "Error: logic.run.top_k is required when logic.run.scope=per_entity_ranked\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }
        if (!run.target_entity.empty()) {
            err << "Error: logic.run.target_entity does not apply when logic.run.scope=per_entity_ranked\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }

        sugar::PanelRankingSpec ranking{};
        std::string ranking_err;
        if (!sugar::build_panel_ranking_spec(run, ranking, ranking_err)) {
            err << "Error: " << ranking_err << "\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }

        const auto plan_report = sugar::diagnose_plan_capabilities(spec, facts);
        if (!focused_projection ||
            (topology_projection && plan_report.status != sugar::PlanCapabilityStatus::Valid)) {
            print_plan_capability_report(plan_report, out);
            print_plan_data_quality_notes(spec, world.run_manifest.preprocess, out);
        }
        if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
            err << "Error: logic.json is not compatible with the resolved observation world\n";
            print_plan_report_next_step(err, cmd);
            return 1;
        }
        sugar::append_plan_capability_snapshot(plan_report, world.run_manifest);

        sugar::ColumnId primary_col{};
        std::string primary_err;
        if (!resolve_world_primary_col_id(world, primary_col, primary_err)) {
            err << "Error: " << primary_err << "\n";
            print_obs_report_next_step(err, cmd);
            return 1;
        }

        if (!focused_projection) {
            out << "[Info] run scope: per_entity_ranked top_k=" << *run.top_k
                << " ranking_objective=" << sugar::describe_ranking_objective(
                    sugar::build_panel_ranking_objective(ranking))
                << "\n";
        }

        const sugar::DataView dv(*world.aligned_bundle);
        sugar::PlanSweepRuntimeOptions rt{};
        const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            spec,
            rt,
            primary_col,
            *run.top_k,
            ranking);
        const auto panel_provenance = sugar::build_run_provenance_summary(world.run_manifest);
        if (cmd.run.report_projection == sugar::EvaluationReportProjection::Full) {
            print_panel_result(pres, out, &panel_provenance);
        }
        else {
            sugar::print_evaluation_report(
                sugar::make_panel_evaluation_report(
                    sugar::build_panel_report(pres, &panel_provenance)),
                cmd.run.report_projection,
                out);
        }
        if (pres.entities_ok == 0) {
            err << "Error: ranked run produced no successful entity results\n";
            print_plan_report_next_step(err, cmd);
            print_align_dump_next_step(err, cmd, layout);
            return 1;
        }
        return 0;
    }

    int run_contract_suite_command(const AppCommand& cmd, std::ostream& out, std::ostream&) {
        const bool ok = sugar::run_contract_suite(out, cmd.contract_suite);
        return ok ? 0 : 1;
    }

    int run_prep_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::DataSpec ds;
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.prep_dump.spec_path, ds, load_err)) {
            err << "Error: failed to load DataSpec: " << load_err << "\n";
            return 1;
        }

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.prep_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_data_spec_summary(ds, out);
        print_preprocess_diagnostics(report, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);
        print_column_registry(*fs.schema, out);

        if (!report.produced_frameset) {
            out << "Preprocess output: NumericFrame\n";
            print_frame_summary(fs.by_entity.begin()->second, out, "  ");
            return 0;
        }

        out << "Preprocess output: FrameSet\n";
        out << "  entity_count: " << fs.by_entity.size() << "\n";

        std::vector<std::pair<std::string, std::size_t>> entities;
        entities.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            entities.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(entities.begin(), entities.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t topn = std::min<std::size_t>(10, entities.size());
        out << "  top entities by rows (up to 10):\n";
        for (std::size_t i = 0; i < topn; ++i) {
            const auto& [ekey, rc] = entities[i];
            auto it = fs.by_entity.find(ekey);
            if (it == fs.by_entity.end()) continue;
            out << "    " << ekey << ": rows=" << rc;
            if (rc > 0) {
                if (const auto mm = time_minmax(it->second)) {
                    const auto [mn, mx] = *mm;
                    out << " time=" << mn << ".." << mx;
                }
                else {
                    out << " row_domain=0.." << (rc - 1);
                }
            }
            out << "\n";
        }

        for (const auto& [ekey, frame] : fs.by_entity) {
            out << "  entity: " << ekey << "\n";
            print_frame_summary(frame, out, "    ");
        }

        return 0;
    }

    int run_align_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        std::string load_err;
        if (!sugar::load_layout_spec_file(cmd.align_dump.spec_path, layout, load_err)) {
            err << "Error: failed to load layout spec: " << load_err << "\n";
            return 1;
        }
        sugar::DataSpec ds = layout.ingest;
        ds.entity_col = cmd.align_dump.series_key_col;

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.align_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }
        if (!report.produced_frameset) {
            err << "Error: --align-dump requires multiple series; preprocess did not produce FrameSet\n";
            return 1;
        }
        if (fs.by_entity.size() < 2) {
            err << "Error: need at least 2 series for alignment dump\n";
            return 1;
        }

        print_preprocess_diagnostics(report, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);

        std::vector<std::pair<std::string, std::size_t>> ranked;
        ranked.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            ranked.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t nsel = std::min<std::size_t>(5, ranked.size());
        std::vector<std::pair<std::string, sugar::NumericFrame>> inputs;
        inputs.reserve(nsel);
        for (std::size_t i = 0; i < nsel; ++i) {
            const auto& key = ranked[i].first;
            auto it = fs.by_entity.find(key);
            if (it != fs.by_entity.end()) {
                inputs.emplace_back(it->first, it->second);
            }
        }
        if (inputs.size() < 2) {
            err << "Error: failed to select at least 2 series for alignment dump\n";
            return 1;
        }

        sugar::align::AlignConfig cfg{};
        cfg.calendar = cmd.align_dump.calendar;
        cfg.missing = sugar::align::MissingPolicy::NaN;
        cfg.non_temporal_multi_series = ds.alignment_non_temporal_multi_series;
        sugar::FrameSet selected{};
        selected.schema = fs.schema;
        selected.index_schema = fs.index_schema;
        for (auto& kv : inputs) {
            selected.by_entity.emplace(kv.first, std::move(kv.second));
        }

        std::shared_ptr<sugar::align::AlignedBundle> bundle{};
        std::string align_err;
        if (!sugar::app::align_frameset(selected, cfg, bundle, align_err)) {
            err << "Error: " << align_err << "\n";
            return 1;
        }

        print_align_dump(*bundle, out);
        return 0;
    }

    int run_panel_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        const sugar::StrategySource source = sugar::prompt_strategy_source(in, out);
        if (source == sugar::StrategySource::Canceled) {
            out << "[Info] Panel command cancelled.\n";
            return 0;
        }
        if (source == sugar::StrategySource::CustomPlanSpec) {
            sugar::CustomPlanSpecRunOptions custom_opt{};
            sugar::prompt_custom_user_planspec(in, out, custom_opt);
            if (custom_opt.plan_spec_path.empty()) {
                out << "[Info] Custom strategy cancelled (no PlanSpec JSON provided).\n";
                return 0;
            }

            sugar::PlanSpec spec{};
            sugar::PlanSweepRuntimeOptions rt{};
            bool has_embedded_observations = false;
            std::string spec_err;
            if (!load_custom_planspec_bundle(custom_opt, spec, rt, has_embedded_observations, spec_err)) {
                err << "Error: failed to load PlanSpec runtime: " << spec_err << "\n";
                return 1;
            }

            bool use_embedded_observations = false;
            if (has_embedded_observations) {
                out << "[Info] PlanSpec embeds legacy observation inputs. --run-panel custom execution can use either those "
                    << "embedded observations or the CLI DataSpec/CSV input.\n";
                use_embedded_observations = sugar::prompt_yes_no(
                    in,
                    out,
                    "Use embedded legacy observations instead of --run-panel DataSpec/CSV input?",
                    true);
            }

            if (use_embedded_observations) {
                AlignedRuntimeInput runtime{};
                std::string runtime_err;
                if (!build_embedded_observation_runtime(spec, runtime, runtime_err)) {
                    err << "Error: " << runtime_err << "\n";
                    return 1;
                }
                if (!cmd.run_panel.primary_col.empty()) {
                    out << "[Info] --primary is ignored when using embedded legacy observations.\n";
                }

                out << "[Info] custom panel execution using embedded legacy observations.\n";
                print_observation_capability_report(runtime, out);
                const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
                sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
                print_plan_capability_report(plan_report, out);
                const sugar::DataView panel_dv(*runtime.aligned_bundle);
                const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
                    panel_dv,
                    spec,
                    rt,
                    runtime.primary_ref.col,
                    cmd.run_panel.topk,
                    default_panel_ranking_spec());
                const auto panel_provenance = sugar::build_run_provenance_summary(runtime.run_manifest);
                print_panel_result(pres, out, &panel_provenance);
                return 0;
            }

            AlignedRuntimeInput runtime{};
            std::string runtime_err;
            if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
                err << "Error: " << runtime_err << "\n";
                return 1;
            }

            out << "[Info] custom panel execution using --run-panel DataSpec/CSV observations.\n";
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
            print_plan_capability_report(plan_report, out);
            const sugar::DataView panel_dv(*runtime.aligned_bundle);
            const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
                panel_dv,
                spec,
                rt,
                runtime.primary_ref.col,
                cmd.run_panel.topk,
                default_panel_ranking_spec());
            const auto panel_provenance = sugar::build_run_provenance_summary(runtime.run_manifest);
            print_panel_result(pres, out, &panel_provenance);
            return 0;
        }

        const sugar::StrategyTemplate templ = sugar::prompt_strategy_template(in, out);
        if (templ == sugar::StrategyTemplate::Canceled) {
            out << "[Info] Panel command cancelled.\n";
            return 0;
        }
        const sugar::TradeMode mode = sugar::prompt_trade_mode(in, out, sugar::TradeMode::LongShort);

        sugar::PlanSpec spec{};
        std::string map_err;
        if (!sugar::load_template_planspec(templ, spec, map_err)) {
            err << "Error: template PlanSpec mapping failed: " << map_err << "\n";
            return 1;
        }

        AlignedRuntimeInput runtime{};
        std::string runtime_err;
        if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_observation_capability_report(runtime, out);
        const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
        sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
        print_plan_capability_report(plan_report, out);
        const sugar::DataView panel_dv(*runtime.aligned_bundle);
        sugar::PlanSweepRuntimeOptions rt{};
        rt.override_mode = true;
        rt.custom_mode = mode;
        const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
            panel_dv,
            spec,
            rt,
            runtime.primary_ref.col,
            cmd.run_panel.topk,
            default_panel_ranking_spec());
        const auto panel_provenance = sugar::build_run_provenance_summary(runtime.run_manifest);
        print_panel_result(pres, out, &panel_provenance);
        return 0;
    }

} // namespace sugar::app
