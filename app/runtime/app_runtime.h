#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ingest/align.h"
#include "ingest/data_spec.h"
#include "ingest/data_view.h"
#include "ingest/numeric_frame.h"
#include "ingest/run_manifest.h"
#include "ingest/run_pipeline.h"

#include "cli/app_cli.h"

namespace sugar {
    struct CustomPlanSpecRunOptions;
    struct PlanSweepRuntimeOptions;
}

namespace sugar::app {

    using AlignedRuntimeInput = sugar::ResolvedRuntimeInput;

    struct RuntimeSession {
        AlignedRuntimeInput runtime{};
        std::vector<std::string> notices{};
    };

    std::string default_csv_path_string();

    bool preprocess_to_frameset(const DataSpec& spec,
        const std::string& csv_path,
        FrameSet& out,
        PreprocessReport& out_report,
        std::string& err,
        const std::optional<align::AlignConfig>& align_cfg = std::nullopt,
        std::shared_ptr<FrameSchema> schema = {},
        TabularFileFormat input_format = TabularFileFormat::Auto);

    bool align_frameset(const FrameSet& fs,
        const align::AlignConfig& cfg,
        std::shared_ptr<align::AlignedBundle>& out,
        std::string& err);

    std::string pick_primary_entity_key(const FrameSet& fs);

    bool build_aligned_runtime_input(const DataSpec& spec,
        const std::string& csv_path,
        const std::string& primary_col_name,
        const align::AlignConfig& cfg,
        AlignedRuntimeInput& out,
        std::string& err,
        TabularFileFormat input_format = TabularFileFormat::Auto);

    bool build_interactive_runtime_session(const InteractiveCommand& cmd,
        const std::string& csv_path,
        RuntimeSession& out,
        std::string& err);

    bool build_run_panel_runtime_input(const RunPanelCommand& cmd,
        AlignedRuntimeInput& out,
        std::string& err);

    bool build_embedded_observation_runtime(const PlanSpec& spec,
        AlignedRuntimeInput& out,
        std::string& err);

    bool load_noninteractive_planspec(
        const std::string& plan_path,
        PlanSpec& out_spec,
        bool& out_has_embedded_observations,
        std::string& err);

    bool load_custom_planspec_bundle(const sugar::CustomPlanSpecRunOptions& opt,
        PlanSpec& out_spec,
        sugar::PlanSweepRuntimeOptions& out_rt,
        bool& out_has_embedded_observations,
        std::string& err);

} // namespace sugar::app
