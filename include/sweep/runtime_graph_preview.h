#pragma once

#include <cstddef>
#include <iosfwd>
#include <string_view>

#include "ingest/data_view.h"
#include "ingest/observation_capability.h"
#include "ingest/run_manifest.h"

namespace sugar {

    void print_runtime_graph_preview(
        const DataView& dv,
        const ColumnRef& primary,
        const RunManifest& manifest,
        std::string_view source_label,
        std::ostream& out,
        std::size_t graph_width = 48,
        const ObservationCapabilityReport* capability_report = nullptr);

} // namespace sugar
