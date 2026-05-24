#include "ingest/layout_spec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
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
            if (x == "hierarchy" || x == "hierarchical" || x == "hierarchy_tree" || x == "hierarchytree") {
                out = TopologyFamily::Hierarchy;
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

        bool parse_non_negative_size(
            const Value& value,
            std::size_t& out,
            const std::string& field_name,
            std::string& err) {
            if (!value.is_num()) {
                err = field_name + " must be number";
                return false;
            }

            if (value.is_i64()) {
                const std::int64_t n = value.as_i64();
                if (n < 0) {
                    err = field_name + " must be >= 0";
                    return false;
                }
                out = static_cast<std::size_t>(n);
                return true;
            }

            const double n = value.as_f64();
            if (!std::isfinite(n) || n < 0.0 || std::floor(n) != n ||
                n > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
                err = field_name + " must be a non-negative integer";
                return false;
            }
            out = static_cast<std::size_t>(n);
            return true;
        }

        bool parse_time_key_number(
            const Value& value,
            TimeKey& out,
            const std::string& field_name,
            std::string& err) {
            if (!value.is_num()) {
                err = field_name + " must be number";
                return false;
            }

            if (value.is_i64()) {
                out = static_cast<TimeKey>(value.as_i64());
                return true;
            }

            const double n = value.as_f64();
            if (!std::isfinite(n) || std::floor(n) != n ||
                n < static_cast<double>(std::numeric_limits<TimeKey>::min()) ||
                n > static_cast<double>(std::numeric_limits<TimeKey>::max())) {
                err = field_name + " must be an integer time key";
                return false;
            }
            out = static_cast<TimeKey>(n);
            return true;
        }

        bool parse_static_graph_edges(
            const Value& value,
            std::vector<StaticGraphEdge>& out,
            std::string& err) {
            if (!value.is_arr()) {
                err = "layout.topologies[].edges must be array";
                return false;
            }

            out.clear();
            for (const auto& item : value.as_arr()) {
                if (!item.is_obj()) {
                    err = "layout.topologies[].edges[] entries must be objects";
                    return false;
                }
                const auto& edge_obj = item.as_obj();

                const auto* source = obj_get(edge_obj, "source");
                if (!source) source = obj_get(edge_obj, "src");
                const auto* target = obj_get(edge_obj, "target");
                if (!target) target = obj_get(edge_obj, "dst");
                if (!source || !target) {
                    err = "layout.topologies[].edges[] requires source and target";
                    return false;
                }

                StaticGraphEdge edge{};
                if (!parse_non_negative_size(*source, edge.source, "layout.topologies[].edges[].source", err)) {
                    return false;
                }
                if (!parse_non_negative_size(*target, edge.target, "layout.topologies[].edges[].target", err)) {
                    return false;
                }

                if (const auto* weight = obj_get(edge_obj, "weight")) {
                    if (!weight->is_num()) {
                        err = "layout.topologies[].edges[].weight must be number";
                        return false;
                    }
                    edge.weight = weight->number_as_f64();
                    if (!std::isfinite(edge.weight)) {
                        err = "layout.topologies[].edges[].weight must be finite";
                        return false;
                    }
                }

                if (const auto* valid_from = obj_get(edge_obj, "valid_from")) {
                    TimeKey time_key{};
                    if (!parse_time_key_number(
                            *valid_from,
                            time_key,
                            "layout.topologies[].edges[].valid_from",
                            err)) {
                        return false;
                    }
                    edge.valid_from = time_key;
                }

                if (const auto* valid_to = obj_get(edge_obj, "valid_to")) {
                    TimeKey time_key{};
                    if (!parse_time_key_number(
                            *valid_to,
                            time_key,
                            "layout.topologies[].edges[].valid_to",
                            err)) {
                        return false;
                    }
                    edge.valid_to = time_key;
                }

                if (edge.valid_from && edge.valid_to && *edge.valid_to <= *edge.valid_from) {
                    err = "layout.topologies[].edges[].valid_to must be greater than valid_from";
                    return false;
                }

                if (const auto* version = obj_get(edge_obj, "version")) {
                    if (!version->is_str()) {
                        err = "layout.topologies[].edges[].version must be string";
                        return false;
                    }
                    edge.version = version->as_str();
                }

                out.push_back(edge);
            }
            return true;
        }

        bool parse_semantic_embeddings(
            const Value& value,
            std::vector<SemanticEmbeddingVector>& out,
            std::string& err) {
            if (!value.is_arr()) {
                err = "layout.topologies[].embeddings must be array";
                return false;
            }

            out.clear();
            for (const auto& item : value.as_arr()) {
                if (!item.is_obj()) {
                    err = "layout.topologies[].embeddings[] entries must be objects";
                    return false;
                }
                const auto& embedding_obj = item.as_obj();

                const auto* node = obj_get(embedding_obj, "node");
                if (!node) node = obj_get(embedding_obj, "index");
                if (!node) {
                    err = "layout.topologies[].embeddings[] requires node";
                    return false;
                }

                const auto* vector = obj_get(embedding_obj, "vector");
                if (!vector) vector = obj_get(embedding_obj, "values");
                if (!vector) vector = obj_get(embedding_obj, "embedding");
                if (!vector) {
                    err = "layout.topologies[].embeddings[] requires vector";
                    return false;
                }
                if (!vector->is_arr()) {
                    err = "layout.topologies[].embeddings[].vector must be array";
                    return false;
                }
                if (vector->as_arr().empty()) {
                    err = "layout.topologies[].embeddings[].vector must be non-empty";
                    return false;
                }

                SemanticEmbeddingVector embedding{};
                if (!parse_non_negative_size(
                        *node,
                        embedding.node,
                        "layout.topologies[].embeddings[].node",
                        err)) {
                    return false;
                }

                embedding.values.reserve(vector->as_arr().size());
                for (const auto& component : vector->as_arr()) {
                    if (!component.is_num()) {
                        err = "layout.topologies[].embeddings[].vector entries must be numbers";
                        return false;
                    }
                    const double value_f64 = component.number_as_f64();
                    if (!std::isfinite(value_f64)) {
                        err = "layout.topologies[].embeddings[].vector entries must be finite";
                        return false;
                    }
                    embedding.values.push_back(value_f64);
                }

                out.push_back(std::move(embedding));
            }
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

            if (const auto* node_count = obj_get(o, "node_count")) {
                if (!parse_non_negative_size(*node_count, spec.node_count, "layout.topologies[].node_count", err)) {
                    return false;
                }
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

            if (const auto* edges = obj_get(o, "edges")) {
                if (!parse_static_graph_edges(*edges, spec.static_edges, err)) {
                    return false;
                }
            }

            if (const auto* embeddings = obj_get(o, "embeddings")) {
                if (!parse_semantic_embeddings(*embeddings, spec.semantic_embeddings, err)) {
                    return false;
                }
            }

            if (const auto* metadata = obj_get(o, "metadata")) {
                if (!parse_topology_metadata(*metadata, spec.metadata, err)) {
                    return false;
                }
            }

            out = std::move(spec);
            return true;
        }

        const char* misplaced_wrapped_dataspec_key(const Object& o) {
            static constexpr const char* kRootDataspecKeys[] = {
                "input_profile",
                "time_col",
                "time_kind",
                "header_policy",
                "columns_by_index",
                "entity_col",
                "spatial_x_col",
                "spatial_y_col",
                "value_cols",
                "primary_value_col",
                "rename",
                "missing_tokens",
                "missing_default",
                "sort_by_time",
                "dedupe",
                "alignment_calendar",
                "alignment_non_temporal_multi_series",
            };
            for (const char* key : kRootDataspecKeys) {
                if (obj_get(o, key) != nullptr) return key;
            }
            return nullptr;
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
            if (const char* key = misplaced_wrapped_dataspec_key(o)) {
                if (std::string_view(key) == "alignment_calendar") {
                    err = "wrapped LayoutSpec root key `alignment_calendar` is not valid here; use `alignment.calendar` or put DataSpec fields under `ingest`";
                    return false;
                }
                if (std::string_view(key) == "alignment_non_temporal_multi_series") {
                    err = "wrapped LayoutSpec root key `alignment_non_temporal_multi_series` is not valid here; use `alignment.non_temporal_multi_series` or put DataSpec fields under `ingest`";
                    return false;
                }
                err = "wrapped LayoutSpec root key `" + std::string(key) +
                    "` is a DataSpec field; move it under `ingest` or use a raw DataSpec root without `alignment`/`topologies`";
                return false;
            }

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
