#pragma once

#include <iosfwd>
#include <string_view>

#include "ingest/align.h"
#include "ingest/data_spec.h"
#include "ingest/numeric_frame.h"
#include "ingest/observation_capability.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"

namespace sugar::app {

    void print_preprocess_diagnostics(
        const sugar::PreprocessReport& report,
        std::ostream& out);

    void print_data_spec_summary(const sugar::DataSpec& ds, std::ostream& out);

    void print_column_registry(const sugar::FrameSchema& schema, std::ostream& out);

    void print_frame_summary(const sugar::NumericFrame& f,
        std::ostream& out,
        std::string_view prefix = {});

    void print_panel_result(const sugar::PanelResult& r, std::ostream& out);

    void print_align_dump(const sugar::align::AlignedBundle& b, std::ostream& out);

    void print_observation_capability_report(
        const sugar::ObservationCapabilityReport& report,
        std::ostream& out);

    void print_observation_capability_report(
        const sugar::ResolvedRuntimeInput& runtime,
        std::ostream& out);

    void print_observation_capability_report(
        const sugar::ResolvedObservationWorld& world,
        std::ostream& out);

    void print_plan_capability_report(
        const sugar::PlanCapabilityReport& report,
        std::ostream& out);

    void print_plan_capability_report(
        const sugar::PlanSpec& spec,
        const sugar::ResolvedRuntimeInput& runtime,
        std::ostream& out);

} // namespace sugar::app
