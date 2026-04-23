#include "service/request_validation.h"

#include <algorithm>

#include "ops/op_registry.h"
#include "sweep/sweep.h"

namespace sugar::service {

    namespace {
        bool plan_spec_has_sweep(const PlanSpec& plan);

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

        bool fill_sweep_policy_metadata(
            const PlanSpec& plan,
            const QuotaConfig& quotas,
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

            std::size_t candidate_count = dims.total_combinations();
            if (plan.sweep->max_evals > 0 && candidate_count > plan.sweep->max_evals) {
                candidate_count = plan.sweep->max_evals;
            }
            metadata.sweep_candidate_count = candidate_count;
            metadata.estimated_sweep_minutes = estimate_sweep_runtime_minutes(candidate_count, quotas);
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

        bool resolve_run_ranking_spec(
            const PlanRunSpec& run,
            PanelRankingSpec& out,
            std::string& err) {
            const PlanRunRankingMetric metric =
                run.ranking_metric.value_or(PlanRunRankingMetric::Pnl);
            switch (metric) {
            case PlanRunRankingMetric::Pnl:
                if (run.ranking_dd_weight.has_value()) {
                    err = "logic.run.ranking_dd_weight applies only when logic.run.ranking_metric=pnl_dd";
                    return false;
                }
                out.metric = PanelRankingMetric::Pnl;
                out.dd_weight = 0.0;
                err.clear();
                return true;
            case PlanRunRankingMetric::PnlDd:
                if (!run.ranking_dd_weight.has_value()) {
                    err = "logic.run.ranking_dd_weight is required when logic.run.ranking_metric=pnl_dd";
                    return false;
                }
                out.metric = PanelRankingMetric::PnlDd;
                out.dd_weight = *run.ranking_dd_weight;
                err.clear();
                return true;
            }
            err = "logic.run.ranking_metric is invalid";
            return false;
        }

        bool plan_spec_has_sweep(const PlanSpec& plan) {
            return plan.sweep.has_value() &&
                (plan.sweep->enabled || !plan.sweep->params.empty());
        }
    } // namespace

    std::size_t estimate_sweep_runtime_minutes(
        std::size_t sweep_candidate_count,
        const QuotaConfig& quotas) {
        if (sweep_candidate_count == 0 ||
            quotas.max_sweep_candidate_count == 0 ||
            quotas.sweep_reject_minutes == 0) {
            return 0;
        }
        const std::size_t numerator =
            (sweep_candidate_count * quotas.sweep_reject_minutes) +
            (quotas.max_sweep_candidate_count - 1);
        return numerator / quotas.max_sweep_candidate_count;
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
            "warning: estimated sweep duration " +
            std::to_string(metadata.estimated_sweep_minutes) +
            " minutes exceeds the hosted warning threshold of " +
            std::to_string(quotas.sweep_warning_minutes) +
            " minutes (candidate-count estimate: " +
            std::to_string(metadata.sweep_candidate_count) +
            " of " +
            std::to_string(quotas.max_sweep_candidate_count) +
            ")";
    }

    std::string make_sweep_budget_rejection_message(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas) {
        return
            "request exceeds hosted sweep budget: estimated sweep duration " +
            std::to_string(metadata.estimated_sweep_minutes) +
            " minutes exceeds the " +
            std::to_string(quotas.sweep_reject_minutes) +
            "-minute limit (candidate-count estimate: " +
            std::to_string(metadata.sweep_candidate_count) +
            " of " +
            std::to_string(quotas.max_sweep_candidate_count) +
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
        if (!fill_sweep_policy_metadata(plan, quotas, metadata, err)) {
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
        if (!fill_sweep_policy_metadata(out.plan, quotas, out.metadata, err)) {
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
            err.clear();
            return true;
        }

        const PlanRunSpec& run = *out.plan.run;
        if (run.scope == PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.ranking_metric.has_value() || run.ranking_dd_weight.has_value()) {
                err = "logic.run.top_k and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked";
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
        if (!resolve_run_ranking_spec(run, ranking, err)) {
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

        err.clear();
        return true;
    }

} // namespace sugar::service
