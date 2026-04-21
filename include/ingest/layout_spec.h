#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ingest/data_spec.h"
#include "ingest/topology_spec.h"

namespace sugar {

    struct LayoutSpec {
        DataSpec ingest{};
        std::vector<TopologySpec> topologies{};
    };

    bool parse_layout_spec_json(std::string_view json, LayoutSpec& out, std::string& err);
    bool load_layout_spec_file(const std::string& path, LayoutSpec& out, std::string& err);

} // namespace sugar
