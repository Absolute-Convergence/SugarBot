#pragma once

#include <optional>
#include <string>

#include "ingest/layout_spec.h"
#include "ingest/observation_capability.h"
#include "ingest/run_pipeline.h"
#include "service/service_types.h"
#include "sweep/plan_capability.h"
#include "sweep/plan_spec.h"

namespace sugar::service {

    struct ValidatedRequest {
        LayoutSpec layout{};
        PlanSpec plan{};
        ResolvedObservationWorld world{};
        std::optional<ResolvedRuntimeInput> runtime{};
        std::optional<ColumnId> primary_col_id{};
        std::optional<PanelRankingSpec> ranking{};
        std::optional<std::size_t> top_k{};
        PlanCapabilityReport plan_report{};
        ValidationMetadata metadata{};
    };

    [[nodiscard]] std::size_t estimate_sweep_runtime_minutes(
        std::size_t sweep_candidate_count,
        std::size_t estimated_row_count,
        PlanSweepEngineKind engine);
    [[nodiscard]] bool metadata_has_long_sweep_warning(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas);
    [[nodiscard]] std::string make_sweep_budget_warning_message(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas);
    [[nodiscard]] std::string make_sweep_budget_rejection_message(
        const ValidationMetadata& metadata,
        const QuotaConfig& quotas);

    bool validate_submit_request_shape(
        const SubmitRequest& request,
        const QuotaConfig& quotas,
        ValidationMetadata* out_metadata,
        std::string& err);

    bool validate_staged_request(
        const std::filesystem::path& layout_path,
        const std::filesystem::path& logic_path,
        const std::filesystem::path& data_path,
        const QuotaConfig& quotas,
        ValidatedRequest& out,
        std::string& err);

} // namespace sugar::service
