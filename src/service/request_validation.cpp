#include "service/request_validation.h"

#include <algorithm>
#include <limits>

#include "ops/op_registry.h"
#include "sweep/sweep.h"

namespace sugar::service {

    namespace {
        bool plan_spec_has_sweep(const PlanSpec& plan);
        constexpr std::size_t kBaselineSweepRows = 1000;
        constexpr std::size_t kBaselineExhaustiveCandidatesPerMinute = 200000;
        constexpr std::size_t kBaselineCoarseFineCandidatesPerMinute = 20000;
        constexpr std::size_t kBaselineMonteCarloCandidatesPerMinute = 30000;
        constexpr std::size_t kBaselinePosteriorCandidatesPerMinute = 10000;

        std::size_t estimate_payload_data_row_count(std::string_view csv_text) {
            if (csv_text.empty()) {
                return 0;
            }

            std::size_t physical_lines = static_cast<std::size_t>(
                std::count(csv_text.begin(), csv_text.end(), '\n'));
            if (!csv_text.empty() && csv_text.back() != '\n') {
                ++physical_lines;
            }
            if (physical_lines == 0) {
                return 0;
            }
            return physical_lines > 1 ? (physical_lines - 1) : 0;
        }

        std::size_t sweep_warning_candidate_count(const QuotaConfig& quotas) {
            if (quotas.max_sweep_candidate_count == 0 ||
                quotas.sweep_warning_minutes == 0 ||
                quotas.sweep_reject_minutes == 0 ||
                quotas.sweep_warning_minutes >= quotas.sweep_reject_minutes) {
                return 0;
            }
            const std::size_t numerator =
                quotas.max_sweep_candidate_count * quotas.sweep_warning_minutes;
            return numerator / quotas.sweep_reject_minutes;
        }

        std::size_t expected_sweep_evaluation_count(
            const PlanSweepSpec& sweep,
            const PlanSpecSweepDims& dims) {
            const std::size_t total_combinations = dims.total_combinations();
            const bool total_overflow =
                total_combinations == std::numeric_limits<std::size_t>::max();

            if (sweep.engine == PlanSweepEngineKind::MonteCarlo) {
                const std::size_t requested_draws =
                    sweep.monte_carlo.draws_explicit
                    ? sweep.monte_carlo.draws
                    : sweep.max_evals;
                std::size_t count = sweep.max_evals > 0
                    ? std::min(requested_draws, sweep.max_evals)
                    : requested_draws;
                if (sweep.monte_carlo.dedupe && !total_overflow) {
                    count = std::min(count, total_combinations);
                }
                return count;
            }
            if (sweep.engine == PlanSweepEngineKind::SuccessiveHalving) {
                if (total_overflow) {
                    return sweep.max_evals;
                }
                return std::min(total_combinations, sweep.max_evals);
            }
            if (sweep.engine == PlanSweepEngineKind::BayesianPosterior) {
                if (total_overflow) {
                    return sweep.max_evals;
                }
                return std::min(total_combinations, sweep.max_evals);
            }

            if (total_overflow) {
                return sweep.max_evals;
            }
            return sweep.max_evals > 0
                ? std::min(total_combinations, sweep.max_evals)
                : total_combinations;
        }

        bool fill_sweep_policy_metadata(
            const PlanSpec& plan,
            const QuotaConfig& quotas,
            std::size_t estimated_row_count,
            ValidationMetadata& metadata,
            std::string& err) {
            metadata.sweep_candidate_count = 0;
            metadata.estimated_sweep_minutes = 0;
            metadata.long_sweep_warning = false;

            if (!plan_spec_has_sweep(plan)) {
                err.clear();
                return true;
            }

            PlanSpecSweepDims dims{};
            if (!build_planspec_sweep_dims(*plan.sweep, dims, err)) {
                err = "logic sweep is invalid: " + err;
                return false;
            }

            std::size_t candidate_count = expected_sweep_evaluation_count(*plan.sweep, dims);
            metadata.sweep_candidate_count = candidate_count;
            metadata.estimated_sweep_minutes = estimate_sweep_runtime_minutes(
                candidate_count,
                estimated_row_count,
                plan.sweep->engine);
            metadata.long_sweep_warning = metadata_has_long_sweep_warning(metadata, quotas);

            if (quotas.max_sweep_candidate_count > 0 &&
                candidate_count > quotas.max_sweep_candidate_count) {
                err = make_sweep_budget_rejection_message(metadata, quotas);
                return false;
            }

            err.clear();
            return true;
        }

        bool payload_name_is_safe_leaf(
            const ArtifactPayload& payload,
            const char* label,
            std::string& err) {
            if (payload.client_name.empty()) {
                err = std::string(label) + " artifact client name is empty";
                return false;
            }

            const std::filesystem::path path(payload.client_name);
            if (path.is_absolute()) {
                err = std::string(label) + " artifact client name must not be absolute";
                return false;
            }
            if (payload.client_name.find('/') != std::string::npos ||
                payload.client_name.find('\\') != std::string::npos) {
                err = std::string(label) + " artifact client name must be a single filename";
                return false;
            }
            if (path.has_parent_path()) {
                err = std::string(label) + " artifact client name must be a single filename";
                return false;
            }
            if (payload.client_name.find("..") != std::string::npos) {
                err = std::string(label) + " artifact client name must not contain parent traversal";
                return false;
            }
            return true;
        }

        bool payload_name_has_expected_extension(
            const ArtifactPayload& payload,
            const char* label,
            const char* extension,
            std::string& err) {
            if (std::filesystem::path(payload.client_name).extension() != extension) {
                err = std::string(label) + " artifact client name must end with " + extension;
                return false;
            }
            return true;
        }

        bool observation_world_is_multi_series(const ResolvedObservationWorld& world) {
            if (world.aligned_bundle) {
                return world.aligned_bundle->series_keys.size() > 1;
            }
            if (world.frameset) {
                return world.frameset->by_entity.size() > 1;
            }
            return false;
        }

        bool resolve_target_entity_series_key(
            const ResolvedObservationWorld& world,
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
            err.clear();
            return true;
        }

        bool resolve_world_primary_col_id(
            const ResolvedObservationWorld& world,
            ColumnId& out,
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
            err.clear();
            return true;
        }

        bool plan_spec_has_sweep(const PlanSpec& plan) {
            return plan.sweep.has_value() &&
                (plan.sweep->enabled || !plan.sweep->params.empty());
        }
    } // namespace

    std::size_t estimate_sweep_runtime_minutes(
        std::size_t sweep_candidate_count,
        std::size_t estimated_row_count,
        PlanSweepEngineKind engine) {
        if (sweep_candidate_count == 0) {
            return 0;
        }

        const std::size_t baseline_candidates_per_minute =
            engine == PlanSweepEngineKind::CoarseFine
            ? kBaselineCoarseFineCandidatesPerMinute
            : engine == PlanSweepEngineKind::MonteCarlo
            ? kBaselineMonteCarloCandidatesPerMinute
            : engine == PlanSweepEngineKind::SuccessiveHalving
            ? kBaselineCoarseFineCandidatesPerMinute
            : engine == PlanSweepEngineKind::BayesianPosterior
            ? kBaselinePosteriorCandidatesPerMinute
            : kBaselineExhaustiveCandidatesPerMinute;

        const std::size_t scaled_rows =
            std::max<std::size_t>(1, std::max(estimated_row_count, kBaselineSweepRows));
        const std::size_t numerator =
            sweep_candidate_count * scaled_rows +
            (baseline_candidates_per_minute * kBaselineSweepRows) - 1;
        const std::size_t denominator =
            baseline_candidates_per_minute * kBaselineSweepRows;
        return std::max<std::size_t>(1, numerator / denominator);
    }

    bool metadata_has_long_sweep_warning(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas) {
        const std::size_t warning_candidates = sweep_warning_candidate_count(quotas);
        return warning_candidates > 0 &&
            metadata.sweep_candidate_count > warning_candidates &&
            metadata.sweep_candidate_count <= quotas.max_sweep_candidate_count;
    }

    std::string make_sweep_budget_warning_message(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas) {
        if (!metadata_has_long_sweep_warning(metadata, quotas)) {
            return {};
        }

        return
            "warning: sweep candidate estimate " +
            std::to_string(metadata.sweep_candidate_count) +
            " exceeds the hosted warning budget of " +
            std::to_string(sweep_warning_candidate_count(quotas)) +
            " candidates (limit " +
            std::to_string(quotas.max_sweep_candidate_count) +
            ", rough runtime estimate about " +
            std::to_string(metadata.estimated_sweep_minutes) +
            " minute" +
            (metadata.estimated_sweep_minutes == 1 ? "" : "s") +
            ")";
    }

    std::string make_sweep_budget_rejection_message(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas) {
        return
            "request exceeds hosted sweep candidate budget: estimated candidate count " +
            std::to_string(metadata.sweep_candidate_count) +
            " exceeds the configured limit of " +
            std::to_string(quotas.max_sweep_candidate_count) +
            " (rough runtime estimate about " +
            std::to_string(metadata.estimated_sweep_minutes) +
            " minute" +
            (metadata.estimated_sweep_minutes == 1 ? "" : "s") +
            ")";
    }

    bool validate_submit_request_shape(
        const SubmitRequest& request,
        const QuotaConfig& quotas,
        ValidationMetadata* out_metadata,
        std::string& err) {
        ValidationMetadata metadata{};
        if (out_metadata) {
            *out_metadata = ValidationMetadata{};
        }

        auto require_payload = [&](const ArtifactPayload& payload, const char* label) -> bool {
            if (payload.content.empty()) {
                err = std::string(label) + " artifact content is empty";
                return false;
            }
            if (payload.content.size() > quotas.max_upload_size_bytes) {
                err = std::string(label) + " artifact exceeds max upload size";
                return false;
            }
            return true;
        };

        if (!require_payload(request.layout, "layout")) {
            return false;
        }
        if (!require_payload(request.logic, "logic")) {
            return false;
        }
        if (!require_payload(request.data, "data")) {
            return false;
        }
        if (!payload_name_is_safe_leaf(request.layout, "layout", err) ||
            !payload_name_is_safe_leaf(request.logic, "logic", err) ||
            !payload_name_is_safe_leaf(request.data, "data", err)) {
            return false;
        }
        if (!payload_name_has_expected_extension(request.layout, "layout", ".json", err) ||
            !payload_name_has_expected_extension(request.logic, "logic", ".json", err) ||
            !payload_name_has_expected_extension(request.data, "data", ".csv", err)) {
            return false;
        }

        const auto newline_count = static_cast<std::size_t>(
            std::count(request.data.content.begin(), request.data.content.end(), '\n'));
        if (newline_count > quotas.max_parsed_row_count + 1) {
            err = "data artifact exceeds max parsed row count estimate";
            return false;
        }

        PlanSpec plan{};
        if (!parse_plan_spec_json(request.logic.content, plan, err)) {
            err = "failed to parse logic spec: " + err;
            return false;
        }
        if (!fill_sweep_policy_metadata(
                plan,
                quotas,
                estimate_payload_data_row_count(request.data.content),
                metadata,
                err)) {
            return false;
        }

        if (out_metadata) {
            *out_metadata = metadata;
        }
        err.clear();
        return true;
    }

    bool validate_staged_request(
        const std::filesystem::path& layout_path,
        const std::filesystem::path& logic_path,
        const std::filesystem::path& data_path,
        const QuotaConfig& quotas,
        ValidatedRequest& out,
        std::string& err) {
        out = ValidatedRequest{};

        if (!load_layout_spec_file(layout_path.string(), out.layout, err)) {
            err = "failed to load layout spec: " + err;
            return false;
        }
        if (!load_plan_spec_file(logic_path.string(), out.plan, err)) {
            err = "failed to load logic spec: " + err;
            return false;
        }

        bool has_embedded_observations = false;
        if (!plan_spec_has_embedded_observation_inputs(out.plan, has_embedded_observations, err)) {
            err = "failed to inspect logic spec observations: " + err;
            return false;
        }
        if (has_embedded_observations) {
            err = "logic.json embeds legacy observation inputs. That compatibility path is not allowed in service mode";
            return false;
        }

        if (!build_resolved_observation_world(out.layout, data_path.string(), out.world, err)) {
            return false;
        }

        const auto facts = make_observation_world_facts(out.world);
        out.metadata.series_count = facts.series_count;
        out.metadata.row_count = facts.row_count;
        out.metadata.multi_series = observation_world_is_multi_series(out.world);
        out.metadata.input_warning_count = out.world.run_manifest.preprocess.input_warnings.size();

        if (facts.row_count > quotas.max_parsed_row_count) {
            err = "request exceeds max parsed row count";
            return false;
        }

        const bool has_sweep = plan_spec_has_sweep(out.plan);
        if (!fill_sweep_policy_metadata(
                out.plan,
                quotas,
                out.metadata.row_count,
                out.metadata,
                err)) {
            return false;
        }

        if (!out.plan.run.has_value()) {
            if (out.metadata.multi_series) {
                out.plan_report = diagnose_plan_capabilities(out.plan, facts);
                out.metadata.incompatible_nodes = out.plan_report.incompatible_nodes;
                out.metadata.incompatible_bindings = out.plan_report.incompatible_bindings;
                err = "multi-entity requests require logic.run.scope in logic.json";
                return false;
            }

            ResolvedRuntimeInput runtime{};
            if (!bind_resolved_runtime_target(
                    out.world,
                    out.layout.ingest.primary_value_col,
                    RuntimeTargetSpec{},
                    runtime,
                    err)) {
                return false;
            }

            out.runtime = runtime;
            out.plan_report = diagnose_plan_capabilities(out.plan, runtime);
            out.metadata.resolved_mode = has_sweep
                ? ResolvedExecutionMode::PrimarySeriesSweep
                : ResolvedExecutionMode::PrimarySeriesRun;
            out.metadata.incompatible_nodes = out.plan_report.incompatible_nodes;
            out.metadata.incompatible_bindings = out.plan_report.incompatible_bindings;
            out.metadata.valid = out.plan_report.status == PlanCapabilityStatus::Valid;
            if (!out.metadata.valid) {
                err = "logic.json is not compatible with the resolved runtime target";
                return false;
            }
            append_plan_capability_snapshot(out.plan_report, out.runtime->run_manifest);
            err.clear();
            return true;
        }

        const PlanRunSpec& run = *out.plan.run;
        if (run.scope == PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.objective_spec.has_value()) {
                err = "logic.run.top_k, logic.run.objective, and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked";
                return false;
            }

            RuntimeTargetSpec target{};
            target.require_primary_series_on_multi_series = out.metadata.multi_series;
            if (out.metadata.multi_series && run.target_entity.empty()) {
                err = "logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data";
                return false;
            }
            if (out.metadata.multi_series || !run.target_entity.empty()) {
                std::string series_key{};
                if (!resolve_target_entity_series_key(out.world, run.target_entity, series_key, err)) {
                    return false;
                }
                target.primary_series_key = std::move(series_key);
            }

            ResolvedRuntimeInput runtime{};
            if (!bind_resolved_runtime_target(
                    out.world,
                    out.layout.ingest.primary_value_col,
                    target,
                    runtime,
                    err)) {
                return false;
            }

            out.runtime = runtime;
            out.plan_report = diagnose_plan_capabilities(out.plan, runtime);
            out.metadata.resolved_mode = has_sweep
                ? ResolvedExecutionMode::PrimarySeriesSweep
                : ResolvedExecutionMode::PrimarySeriesRun;
            out.metadata.incompatible_nodes = out.plan_report.incompatible_nodes;
            out.metadata.incompatible_bindings = out.plan_report.incompatible_bindings;
            out.metadata.valid = out.plan_report.status == PlanCapabilityStatus::Valid;
            if (!out.metadata.valid) {
                err = "logic.json is not compatible with the resolved runtime target";
                return false;
            }
            append_plan_capability_snapshot(out.plan_report, out.runtime->run_manifest);
            err.clear();
            return true;
        }

        if (!out.metadata.multi_series) {
            err = "logic.run.scope=per_entity_ranked requires multi-entity observation input";
            return false;
        }
        if (!run.top_k.has_value()) {
            err = "logic.run.top_k is required when logic.run.scope=per_entity_ranked";
            return false;
        }
        if (!run.target_entity.empty()) {
            err = "logic.run.target_entity does not apply when logic.run.scope=per_entity_ranked";
            return false;
        }

        PanelRankingSpec ranking{};
        if (!build_panel_ranking_spec(run, ranking, err)) {
            return false;
        }

        ColumnId primary_col{};
        if (!resolve_world_primary_col_id(out.world, primary_col, err)) {
            return false;
        }

        out.ranking = ranking;
        out.top_k = *run.top_k;
        out.primary_col_id = primary_col;
        out.plan_report = diagnose_plan_capabilities(out.plan, facts);
        out.metadata.resolved_mode = has_sweep
            ? ResolvedExecutionMode::PerEntityRankedSweep
            : ResolvedExecutionMode::PerEntityRankedRun;
        out.metadata.incompatible_nodes = out.plan_report.incompatible_nodes;
        out.metadata.incompatible_bindings = out.plan_report.incompatible_bindings;
        out.metadata.valid = out.plan_report.status == PlanCapabilityStatus::Valid;
        if (!out.metadata.valid) {
            err = "logic.json is not compatible with the resolved observation world";
            return false;
        }
        append_plan_capability_snapshot(out.plan_report, out.world.run_manifest);

        err.clear();
        return true;
    }

} // namespace sugar::service
