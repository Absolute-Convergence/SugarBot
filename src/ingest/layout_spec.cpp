#include "ingest/layout_spec.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace sugar {

    namespace {
        using namespace sugar::json_min;

        std::string lower_copy(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool parse_topology_bearing(const std::string& s, TopologyBearing& out) {
            const std::string x = lower_copy(s);
            if (x == "metadata_only" || x == "metadataonly") {
                out = TopologyBearing::MetadataOnly;
                return true;
            }
            if (x == "query_bearing" || x == "querybearing") {
                out = TopologyBearing::QueryBearing;
                return true;
            }
            if (x == "runtime_bearing" || x == "runtimebearing") {
                out = TopologyBearing::RuntimeBearing;
                return true;
            }
            return false;
        }

        bool parse_alignment_calendar(const std::string& s, align::CalendarPolicy& out) {
            const std::string x = lower_copy(s);
            if (x == "intersection") {
                out = align::CalendarPolicy::Intersection;
                return true;
            }
            if (x == "union") {
                out = align::CalendarPolicy::Union;
                return true;
            }
            return false;
        }

        bool parse_alignment_non_temporal_multi_series(
            const std::string& s,
            align::NonTemporalMultiSeriesPolicy& out) {
            const std::string x = lower_copy(s);
            if (x == "reject") {
                out = align::NonTemporalMultiSeriesPolicy::Reject;
                return true;
            }
            if (x == "equallengthrowdomain" ||
                x == "equal_length_row_domain" ||
                x == "equal_length") {
                out = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;
                return true;
            }
            return false;
        }

        bool parse_topology_family(const std::string& s, TopologyFamily& out) {
            const std::string x = lower_copy(s);
            if (x == "temporal_window" || x == "temporalwindow") {
                out = TopologyFamily::TemporalWindow;
                return true;
            }
            if (x == "spatial_metric_2d" || x == "spatialmetric2d") {
                out = TopologyFamily::SpatialMetric2D;
                return true;
            }
            if (x == "spatiotemporal_composite" || x == "spatiotemporalcomposite") {
                out = TopologyFamily::SpatioTemporalComposite;
                return true;
            }
            if (x == "relational_graph" || x == "relationalgraph") {
                out = TopologyFamily::RelationalGraph;
                return true;
            }
            if (x == "semantic_graph" || x == "semanticgraph") {
                out = TopologyFamily::SemanticGraph;
                return true;
            }
            if (x == "probabilistic_graph" || x == "probabilisticgraph") {
                out = TopologyFamily::ProbabilisticGraph;
                return true;
            }
            if (x == "composite") {
                out = TopologyFamily::Composite;
                return true;
            }
            return false;
        }

        bool parse_topology_domain_scope(const std::string& s, TopologyDomainScope& out) {
            const std::string x = lower_copy(s);
            if (x == "row_domain" || x == "rowdomain") {
                out = TopologyDomainScope::RowDomain;
                return true;
            }
            if (x == "series_local" || x == "serieslocal") {
                out = TopologyDomainScope::SeriesLocal;
                return true;
            }
            if (x == "entity_panel" || x == "entitypanel") {
                out = TopologyDomainScope::EntityPanel;
                return true;
            }
            if (x == "observation_domain" || x == "observationdomain") {
                out = TopologyDomainScope::ObservationDomain;
                return true;
            }
            return false;
        }

        bool parse_topology_query_kind(const std::string& s, TopologyQueryKind& out) {
            const std::string x = lower_copy(s);
            if (x == "selection") { out = TopologyQueryKind::Selection; return true; }
            if (x == "ordered_window" || x == "orderedwindow") { out = TopologyQueryKind::OrderedWindow; return true; }
            if (x == "metric_radius" || x == "metricradius") { out = TopologyQueryKind::MetricRadius; return true; }
            if (x == "metric_k_nearest" || x == "metricknearest") { out = TopologyQueryKind::MetricKNearest; return true; }
            if (x == "intersection") { out = TopologyQueryKind::Intersection; return true; }
            if (x == "neighbor_set" || x == "neighborset") { out = TopologyQueryKind::NeighborSet; return true; }
            if (x == "weighted_neighbor_set" || x == "weightedneighborset") { out = TopologyQueryKind::WeightedNeighborSet; return true; }
            if (x == "transition_set" || x == "transitionset") { out = TopologyQueryKind::TransitionSet; return true; }
            return false;
        }

        bool parse_topology_artifact_ref(
            const Value& value,
            TopologyArtifactRef& out,
            std::string& err) {
            if (!value.is_obj()) {
                err = "layout.topologies[].artifact_ref must be object";
                return false;
            }

            const auto& o = value.as_obj();
            auto read_string = [&](const char* key, std::string& dst) -> bool {
                if (const auto* p = obj_get(o, key)) {
                    if (!p->is_str()) {
                        err = std::string("layout.topologies[].artifact_ref.") + key + " must be string";
                        return false;
                    }
                    dst = p->as_str();
                }
                return true;
            };

            if (!read_string("stable_id", out.stable_id)) return false;
            if (!read_string("locator", out.locator)) return false;
            if (!read_string("format", out.format)) return false;
            if (!read_string("digest", out.digest)) return false;
            if (!read_string("producer", out.producer)) return false;
            return true;
        }

        bool parse_topology_metadata(
            const Value& value,
            std::vector<std::pair<std::string, std::string>>& out,
            std::string& err) {
            if (!value.is_obj()) {
                err = "layout.topologies[].metadata must be object";
                return false;
            }

            out.clear();
            for (const auto& kv : value.as_obj()) {
                if (!kv.second.is_str()) {
                    err = "layout.topologies[].metadata values must be strings";
                    return false;
                }
                out.emplace_back(kv.first, kv.second.as_str());
            }
            return true;
        }

        bool parse_layout_topology(const Value& value, TopologySpec& out, std::string& err) {
            if (!value.is_obj()) {
                err = "layout.topologies[] entries must be objects";
                return false;
            }

            const auto& o = value.as_obj();
            TopologySpec spec{};
            spec.origin = TopologyOrigin::LayoutDeclared;
            spec.bearing = TopologyBearing::MetadataOnly;

            const auto* name = obj_get(o, "name");
            if (!name || !name->is_str()) {
                err = "layout.topologies[].name must be string";
                return false;
            }
            spec.name = name->as_str();

            if (const auto* bearing = obj_get(o, "bearing")) {
                if (!bearing->is_str()) {
                    err = "layout.topologies[].bearing must be string";
                    return false;
                }
                if (!parse_topology_bearing(bearing->as_str(), spec.bearing)) {
                    err = "layout.topologies[].bearing must be metadata_only|query_bearing|runtime_bearing";
                    return false;
                }
            }

            if (const auto* family = obj_get(o, "family")) {
                if (!family->is_str()) {
                    err = "layout.topologies[].family must be string";
                    return false;
                }
                if (!parse_topology_family(family->as_str(), spec.family)) {
                    err = "layout.topologies[].family is invalid";
                    return false;
                }
            }

            if (const auto* scope = obj_get(o, "domain_scope")) {
                if (!scope->is_str()) {
                    err = "layout.topologies[].domain_scope must be string";
                    return false;
                }
                if (!parse_topology_domain_scope(scope->as_str(), spec.domain_scope)) {
                    err = "layout.topologies[].domain_scope is invalid";
                    return false;
                }
            }

            if (const auto* directed = obj_get(o, "directed")) {
                if (!directed->is_bool()) {
                    err = "layout.topologies[].directed must be bool";
                    return false;
                }
                spec.directed = directed->as_bool();
            }

            if (const auto* weighted = obj_get(o, "weighted")) {
                if (!weighted->is_bool()) {
                    err = "layout.topologies[].weighted must be bool";
                    return false;
                }
                spec.weighted = weighted->as_bool();
            }

            if (const auto* query_kinds = obj_get(o, "query_kinds")) {
                if (!query_kinds->is_arr()) {
                    err = "layout.topologies[].query_kinds must be array";
                    return false;
                }
                for (const auto& item : query_kinds->as_arr()) {
                    if (!item.is_str()) {
                        err = "layout.topologies[].query_kinds entries must be strings";
                        return false;
                    }
                    TopologyQueryKind kind{};
                    if (!parse_topology_query_kind(item.as_str(), kind)) {
                        err = "layout.topologies[].query_kinds contains invalid value";
                        return false;
                    }
                    spec.query_kinds.push_back(kind);
                }
            }

            if (const auto* artifact_ref = obj_get(o, "artifact_ref")) {
                TopologyArtifactRef ref{};
                if (!parse_topology_artifact_ref(*artifact_ref, ref, err)) {
                    return false;
                }
                spec.artifact_ref = std::move(ref);
            }

            if (const auto* metadata = obj_get(o, "metadata")) {
                if (!parse_topology_metadata(*metadata, spec.metadata, err)) {
                    return false;
                }
            }

            out = std::move(spec);
            return true;
        }
    } // namespace

    bool parse_layout_spec_json(std::string_view json, LayoutSpec& out, std::string& err) {
        using namespace sugar::json_min;

        Value root;
        ParseError perr;
        if (!parse(json, root, perr)) {
            err = perr.message + " (line " + std::to_string(perr.line) + ", col " + std::to_string(perr.col) + ")";
            return false;
        }
        if (!root.is_obj()) {
            err = "LayoutSpec root must be object";
            return false;
        }

        const auto& o = root.as_obj();
        LayoutSpec parsed{};

        if (obj_get(o, "data_spec") != nullptr) {
            err = "layout.json must be either a raw DataSpec object or a wrapped LayoutSpec with `ingest`; `data_spec` is not a supported wrapper key";
            return false;
        }

        const bool wrapped_layout =
            obj_get(o, "ingest") != nullptr ||
            obj_get(o, "alignment") != nullptr ||
            obj_get(o, "topologies") != nullptr;

        if (wrapped_layout) {
            const auto* ingest = obj_get(o, "ingest");
            if (!ingest) {
                err = "layout.ingest object is required when using wrapped LayoutSpec";
                return false;
            }
            if (!parse_data_spec_value(*ingest, parsed.ingest, err)) {
                err = "layout.ingest: " + err;
                return false;
            }

            if (const auto* alignment = obj_get(o, "alignment")) {
                if (!alignment->is_obj()) {
                    err = "layout.alignment must be object";
                    return false;
                }
                const auto& ao = alignment->as_obj();
                if (const auto* calendar = obj_get(ao, "calendar")) {
                    if (!calendar->is_str()) {
                        err = "layout.alignment.calendar must be string";
                        return false;
                    }
                    if (!parse_alignment_calendar(calendar->as_str(), parsed.ingest.alignment_calendar)) {
                        err = "layout.alignment.calendar is invalid";
                        return false;
                    }
                }
                if (const auto* non_temporal = obj_get(ao, "non_temporal_multi_series")) {
                    if (!non_temporal->is_str()) {
                        err = "layout.alignment.non_temporal_multi_series must be string";
                        return false;
                    }
                    if (!parse_alignment_non_temporal_multi_series(
                            non_temporal->as_str(),
                            parsed.ingest.alignment_non_temporal_multi_series)) {
                        err = "layout.alignment.non_temporal_multi_series is invalid";
                        return false;
                    }
                }
            }

            if (const auto* topologies = obj_get(o, "topologies")) {
                if (!topologies->is_arr()) {
                    err = "layout.topologies must be array";
                    return false;
                }
                for (const auto& item : topologies->as_arr()) {
                    TopologySpec spec{};
                    if (!parse_layout_topology(item, spec, err)) {
                        return false;
                    }
                    parsed.topologies.push_back(std::move(spec));
                }
            }
        }
        else {
            if (!parse_data_spec_value(root, parsed.ingest, err)) {
                return false;
            }
        }

        out = std::move(parsed);
        return true;
    }

    bool load_layout_spec_file(const std::string& path, LayoutSpec& out, std::string& err) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            err = "Failed to open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return parse_layout_spec_json(ss.str(), out, err);
    }

} // namespace sugar
