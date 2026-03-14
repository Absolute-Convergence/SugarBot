#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "align.h"
#include "data_spec.h"
#include "data_view.h"
#include "numeric_frame.h"
#include "plan_spec.h"
#include "run_manifest.h"

namespace sugar {

    enum class RuntimeObservationSourceKind {
        Unknown = 0,
        AppSuppliedObservations,
        LegacyEmbeddedPlanCompat,
    };

    bool preprocess_to_frameset(const DataSpec& spec,
        const std::string& csv_path,
        FrameSet& out,
        PreprocessReport& out_report,
        std::string& err,
        const std::optional<align::AlignConfig>& align_cfg = std::nullopt,
        std::shared_ptr<FrameSchema> schema = {});

    bool align_frameset(const FrameSet& fs,
        const align::AlignConfig& cfg,
        std::shared_ptr<align::AlignedBundle>& out,
        std::string& err);

    std::string pick_primary_entity_key(const FrameSet& fs);

    struct ResolvedRuntimeInput {
        RuntimeObservationSourceKind source_kind{ RuntimeObservationSourceKind::Unknown };
        std::optional<DataSpec> ingest_spec{};
        std::string primary_input_path{};
        std::shared_ptr<FrameSet> frameset{};
        std::shared_ptr<align::AlignedBundle> aligned_bundle{};
        std::shared_ptr<TopologyCatalog> topology_catalog{};
        RunManifest run_manifest{};
        std::string primary_entity_key{};
        ColumnRef primary_ref{};
        std::size_t primary_series_index{ static_cast<std::size_t>(-1) };
        std::unordered_map<std::string, std::string> series_key_to_entity{};
    };

    bool resolve_planspec_primary_col_name(
        const PlanSpec& spec,
        const ParamMap* overrides,
        std::string& out_name,
        std::string& err);

    bool plan_spec_has_embedded_observation_inputs(
        const PlanSpec& spec,
        bool& out_has,
        std::string& err);

    bool build_resolved_runtime_input(
        const DataSpec& spec,
        const std::string& csv_path,
        const std::string& primary_col_name,
        const align::AlignConfig& cfg,
        ResolvedRuntimeInput& out,
        std::string& err);

    // Compatibility adapter only: resolve legacy embedded observation inputs
    // (for example older `series_inputs` JSON) into canonical DataView runtime
    // input. Not a primary execution contract.
    bool build_embedded_plan_observation_compat_runtime(
        const PlanSpec& spec,
        ResolvedRuntimeInput& out,
        std::string& err);

} // namespace sugar
