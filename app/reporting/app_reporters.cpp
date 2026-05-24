#include "reporting/app_reporters.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "sweep/plan_spec.h"

namespace {

    const char* to_string(sugar::InputProfile f) {
        switch (f) {
        case sugar::InputProfile::Auto: return "auto";
        case sugar::InputProfile::LegacyOhlcvDate: return "legacy_ohlcv_date";
        case sugar::InputProfile::IsoOhlcvTime: return "iso_ohlcv_time";
        default: return "unknown";
        }
    }

    const char* to_string(sugar::TimeKind k) {
        switch (k) {
        case sugar::TimeKind::YyyyMmDd: return "YyyyMmDd";
        case sugar::TimeKind::Iso8601: return "Iso8601";
        case sugar::TimeKind::YearQuarter: return "YearQuarter";
        case sugar::TimeKind::YearMonth: return "YearMonth";
        case sugar::TimeKind::Index: return "Index";
        default: return "Unknown";
        }
    }

    const char* friendly_time_kind(sugar::TimeKind k) {
        switch (k) {
        case sugar::TimeKind::YyyyMmDd: return "YYYY-MM-DD";
        case sugar::TimeKind::Iso8601: return "ISO-8601";
        case sugar::TimeKind::YearQuarter: return "YYYY-Q#";
        case sugar::TimeKind::YearMonth: return "YYYY-MM";
        case sugar::TimeKind::Index: return "row index";
        default: return "unknown";
        }
    }

    const char* to_string(sugar::HeaderPolicy p) {
        switch (p) {
        case sugar::HeaderPolicy::RequiredHeaders: return "RequiredHeaders";
        case sugar::HeaderPolicy::OptionalHeaders: return "OptionalHeaders";
        case sugar::HeaderPolicy::HeaderlessPositional: return "HeaderlessPositional";
        default: return "Unknown";
        }
    }

    const char* to_string(sugar::MissingKind k) {
        switch (k) {
        case sugar::MissingKind::DropRow: return "DropRow";
        case sugar::MissingKind::NaN: return "NaN";
        case sugar::MissingKind::Ffill: return "Ffill";
        case sugar::MissingKind::Const: return "Const";
        default: return "Unknown";
        }
    }

    const char* to_string(sugar::DataSpec::DedupePolicy p) {
        switch (p) {
        case sugar::DataSpec::DedupePolicy::Error: return "Error";
        case sugar::DataSpec::DedupePolicy::First: return "First";
        case sugar::DataSpec::DedupePolicy::Last: return "Last";
        default: return "Unknown";
        }
    }

    const char* to_string(sugar::align::CalendarPolicy p) {
        switch (p) {
        case sugar::align::CalendarPolicy::Intersection: return "Intersection";
        case sugar::align::CalendarPolicy::Union: return "Union";
        default: return "Unknown";
        }
    }

    const char* to_string(sugar::align::MissingPolicy p) {
        switch (p) {
        case sugar::align::MissingPolicy::NaN: return "NaN";
        default: return "Unknown";
        }
    }

    std::optional<std::pair<sugar::TimeKey, sugar::TimeKey>> time_minmax(const sugar::NumericFrame& f) {
        if (!f.has_time_axis()) return std::nullopt;
        const auto t = f.time();
        if (t.empty()) return std::nullopt;
        const auto [mn, mx] = std::minmax_element(t.begin(), t.end());
        return std::make_pair(*mn, *mx);
    }

    void print_missing_stats(const sugar::NumericFrame& f,
        std::ostream& out,
        std::string_view prefix = {})
    {
        const auto& reg = f.schema().columns();
        for (std::size_t i = 0; i < reg.size(); ++i) {
            const sugar::ColumnId id{ static_cast<std::uint32_t>(i) };
            if (!f.has_col(id)) continue;
            const auto col = f.col(id);
            std::size_t miss = 0;
            for (double v : col) {
                if (!std::isfinite(v)) ++miss;
            }
            out << prefix
                << "col[" << i << "]=" << reg.name_by_id(id)
                << " missing=" << miss << "/" << col.size() << "\n";
        }
    }

    void print_reason_list(const std::vector<sugar::ObservationCapabilityReasonCode>& reasons,
        std::ostream& out) {
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            if (i) out << "; ";
            out << sugar::observation_capability_reason_text(reasons[i]);
        }
    }

    std::string escape_json(std::string_view text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char ch : text) {
            switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += ch;
                break;
            }
        }
        return out;
    }

    void append_json_string(std::ostream& out, std::string_view value) {
        out << '"' << escape_json(value) << '"';
    }

    void append_json_string_array(
        std::ostream& out,
        const std::vector<std::string>& values) {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) out << ",";
            append_json_string(out, values[i]);
        }
        out << "]";
    }

    std::string runtime_source_kind_name(sugar::RuntimeObservationSourceKind kind) {
        switch (kind) {
        case sugar::RuntimeObservationSourceKind::Unknown:
            return "unknown";
        case sugar::RuntimeObservationSourceKind::AppSuppliedObservations:
            return "app_supplied_observations";
        case sugar::RuntimeObservationSourceKind::LegacyEmbeddedPlanCompat:
            return "legacy_embedded_plan_compat";
        }
        return "unknown";
    }

    int rounded_missing_percent(const sugar::ValueColumnQuality& stats);

    void append_json_value_column_quality(
        std::ostream& out,
        const std::vector<sugar::ValueColumnQuality>& quality) {
        out << "[";
        for (std::size_t i = 0; i < quality.size(); ++i) {
            if (i) out << ",";
            const auto& stats = quality[i];
            out << "{";
            out << "\"name\":";
            append_json_string(out, stats.name);
            out << ",\"total_values\":" << stats.total_values;
            out << ",\"finite_values\":" << stats.finite_values;
            out << ",\"missing_values\":" << stats.missing_values;
            out << ",\"missing_percent\":" << rounded_missing_percent(stats);
            out << ",\"has_nonzero_finite\":"
                << (stats.has_nonzero_finite ? "true" : "false");
            out << "}";
        }
        out << "]";
    }

    void append_json_preprocess_aggregates(
        std::ostream& out,
        const std::vector<sugar::PreprocessIssueAggregate>& aggregates) {
        std::vector<sugar::PreprocessIssueAggregate> sorted = aggregates;
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                if (a.count != b.count) return a.count > b.count;
                if (a.code != b.code) return a.code < b.code;
                if (a.column != b.column) return a.column < b.column;
                if (a.category != b.category) return a.category < b.category;
                if (a.entity != b.entity) return a.entity < b.entity;
                return a.key < b.key;
            });

        out << "[";
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i) out << ",";
            const auto& aggregate = sorted[i];
            out << "{";
            out << "\"code\":";
            append_json_string(out, aggregate.code);
            out << ",\"column\":";
            append_json_string(out, aggregate.column);
            out << ",\"category\":";
            append_json_string(out, aggregate.category);
            out << ",\"entity\":";
            append_json_string(out, aggregate.entity);
            out << ",\"key\":";
            append_json_string(out, aggregate.key);
            out << ",\"count\":" << aggregate.count;
            out << ",\"samples\":[";
            for (std::size_t s = 0; s < aggregate.samples.size(); ++s) {
                if (s) out << ",";
                const auto& sample = aggregate.samples[s];
                out << "{";
                out << "\"csv_data_row\":" << sample.csv_data_row;
                out << ",\"column\":";
                append_json_string(out, sample.column);
                out << ",\"token\":";
                append_json_string(out, sample.token);
                out << ",\"category\":";
                append_json_string(out, sample.category);
                out << ",\"entity\":";
                append_json_string(out, sample.entity);
                out << ",\"key\":";
                append_json_string(out, sample.key);
                out << "}";
            }
            out << "]}";
        }
        out << "]";
    }

    void append_json_data_spec_summary(
        std::ostream& out,
        const sugar::DataSpec& ds) {
        out << "{";
        out << "\"input_profile\":";
        append_json_string(out, to_string(ds.input_profile));
        out << ",\"time_col\":";
        append_json_string(out, sugar::data_spec_has_time_axis(ds) ? ds.time_col : std::string{});
        out << ",\"time_kind\":";
        append_json_string(out, sugar::data_spec_has_time_axis(ds) ? to_string(ds.time_kind) : "none");
        out << ",\"time_kind_friendly\":";
        append_json_string(out, sugar::data_spec_has_time_axis(ds) ? friendly_time_kind(ds.time_kind) : "none");
        out << ",\"primary_value_col\":";
        append_json_string(out, ds.primary_value_col);
        out << ",\"entity_col\":";
        append_json_string(out, ds.entity_col.value_or(""));
        out << ",\"spatial_x_col\":";
        append_json_string(out, ds.spatial_x_col.value_or(""));
        out << ",\"spatial_y_col\":";
        append_json_string(out, ds.spatial_y_col.value_or(""));
        out << ",\"header_policy\":";
        append_json_string(out, to_string(ds.header_policy));
        out << ",\"dedupe\":";
        append_json_string(out, to_string(ds.dedupe));
        out << ",\"value_cols\":";
        append_json_string_array(out, ds.value_cols);
        out << ",\"value_cols_inferred_from_primary_value_col\":"
            << (ds.value_cols_inferred_from_primary_value_col ? "true" : "false");
        out << ",\"primary_value_col_inferred_from_single_value_col\":"
            << (ds.primary_value_col_inferred_from_single_value_col ? "true" : "false");
        out << "}";
    }

    void append_json_input_diagnostics(
        std::ostream& out,
        const sugar::PreprocessReport& report) {
        out << "{";
        out << "\"header_present\":" << (report.csv_had_header ? "true" : "false");
        out << ",\"csv_physical_lines\":" << report.csv_physical_line_count;
        out << ",\"csv_rows\":" << report.csv_data_row_count;
        out << ",\"blank_rows_skipped\":" << report.csv_blank_line_count;
        out << ",\"rows_dropped\":" << report.dropped_row_count;
        out << ",\"parsed_columns\":";
        append_json_string_array(out, report.parsed_columns);
        out << ",\"warnings\":";
        append_json_string_array(out, report.input_warnings);
        out << ",\"value_column_quality\":";
        append_json_value_column_quality(out, report.value_column_quality);
        out << "}";
    }

    void append_json_observation_domain(
        std::ostream& out,
        const sugar::ObservationWorldFacts& facts) {
        out << "{";
        out << "\"row_domain_size\":" << facts.row_count;
        out << ",\"series_count\":" << facts.series_count;
        out << ",\"produced_frameset\":"
            << (facts.produced_frameset ? "true" : "false");
        out << ",\"has_time_axis\":"
            << (facts.has_time_axis ? "true" : "false");
        out << ",\"has_spatial_axis\":"
            << (facts.has_spatial_axis ? "true" : "false");
        out << ",\"has_entity_axis\":"
            << (facts.has_entity_axis ? "true" : "false");
        out << ",\"has_runtime_bundle\":"
            << (facts.has_runtime_bundle ? "true" : "false");
        out << "}";
    }

    void append_json_observation_capabilities(
        std::ostream& out,
        const sugar::ObservationCapabilityReport& report) {
        out << "[";
        for (std::size_t i = 0; i < report.surfaces.size(); ++i) {
            if (i) out << ",";
            const auto& surface = report.surfaces[i];
            out << "{";
            out << "\"surface\":";
            append_json_string(out, sugar::observation_surface_name(surface.surface));
            out << ",\"status\":";
            append_json_string(out, sugar::observation_capability_status_name(surface.status));
            out << ",\"reasons\":[";
            for (std::size_t r = 0; r < surface.reasons.size(); ++r) {
                if (r) out << ",";
                out << "{";
                out << "\"code\":";
                append_json_string(
                    out,
                    sugar::observation_capability_reason_code_name(surface.reasons[r]));
                out << ",\"text\":";
                append_json_string(
                    out,
                    sugar::observation_capability_reason_text(surface.reasons[r]));
                out << "}";
            }
            out << "]}";
        }
        out << "]";
    }

    void append_json_topologies(
        std::ostream& out,
        const std::vector<sugar::TopologySpec>& topologies) {
        out << "[";
        for (std::size_t i = 0; i < topologies.size(); ++i) {
            if (i) out << ",";
            const auto& spec = topologies[i];
            out << "{";
            out << "\"name\":";
            append_json_string(out, spec.name);
            out << ",\"family\":";
            append_json_string(out, sugar::topology_family_name(spec.family));
            out << ",\"origin\":";
            append_json_string(out, sugar::topology_origin_name(spec.origin));
            out << ",\"bearing\":";
            append_json_string(out, sugar::topology_bearing_name(spec.bearing));
            out << ",\"domain_scope\":";
            append_json_string(out, sugar::topology_domain_scope_name(spec.domain_scope));
            out << ",\"node_count\":" << spec.node_count;
            out << ",\"edge_count\":" << spec.static_edges.size();
            out << ",\"embedding_count\":" << spec.semantic_embeddings.size();
            out << ",\"directed\":" << (spec.directed ? "true" : "false");
            out << ",\"weighted\":" << (spec.weighted ? "true" : "false");
            out << ",\"query_kinds\":[";
            for (std::size_t q = 0; q < spec.query_kinds.size(); ++q) {
                if (q) out << ",";
                append_json_string(out, sugar::topology_query_kind_name(spec.query_kinds[q]));
            }
            out << "]";
            if (spec.artifact_ref.has_value()) {
                out << ",\"artifact\":{";
                out << "\"stable_id\":";
                append_json_string(out, spec.artifact_ref->stable_id);
                out << ",\"locator\":";
                append_json_string(out, spec.artifact_ref->locator);
                out << ",\"format\":";
                append_json_string(out, spec.artifact_ref->format);
                out << ",\"digest\":";
                append_json_string(out, spec.artifact_ref->digest);
                out << ",\"producer\":";
                append_json_string(out, spec.artifact_ref->producer);
                out << "}";
            }
            out << "}";
        }
        out << "]";
    }

    void append_json_series_retention(
        std::ostream& out,
        const sugar::ResolvedObservationWorld& world) {
        out << "[";
        std::vector<std::string> series_keys;
        if (world.aligned_bundle) {
            series_keys = world.aligned_bundle->series_keys;
        }
        else if (world.frameset) {
            for (const auto& [key, _] : world.frameset->by_entity) {
                series_keys.push_back(key);
            }
            std::sort(series_keys.begin(), series_keys.end());
        }

        for (std::size_t i = 0; i < series_keys.size(); ++i) {
            if (i) out << ",";
            const auto& key = series_keys[i];
            std::size_t raw_rows = 0;
            std::size_t aligned_rows = 0;
            if (world.frameset) {
                auto it = world.frameset->by_entity.find(key);
                if (it != world.frameset->by_entity.end()) {
                    raw_rows = it->second.row_count();
                }
            }
            if (world.aligned_bundle) {
                const std::size_t index = world.aligned_bundle->find_series_index(key);
                if (index != static_cast<std::size_t>(-1) &&
                    index < world.aligned_bundle->frames.size()) {
                    aligned_rows = world.aligned_bundle->frames[index].row_count();
                }
            }
            auto entity_it = world.series_key_to_entity.find(key);
            out << "{";
            out << "\"series_key\":";
            append_json_string(out, key);
            out << ",\"entity_key\":";
            append_json_string(
                out,
                entity_it == world.series_key_to_entity.end()
                    ? key
                    : entity_it->second);
            out << ",\"raw_rows\":" << raw_rows;
            out << ",\"aligned_rows\":" << aligned_rows;
            out << "}";
        }
        out << "]";
    }

    std::string query_kind_list(const std::vector<sugar::TopologyQueryKind>& kinds) {
        if (kinds.empty()) return "<none>";
        std::string out;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if (i) out += ", ";
            out += sugar::topology_query_kind_name(kinds[i]);
        }
        return out;
    }

    const sugar::TopologySpec* find_topology_by_name(
        const std::vector<sugar::TopologySpec>& topologies,
        const std::string& name) {
        for (const auto& spec : topologies) {
            if (spec.name == name) return &spec;
        }
        return nullptr;
    }

    bool topology_has_weight_payload(const sugar::TopologySpec& spec) {
        if (spec.weighted) return true;
        for (const auto& edge : spec.static_edges) {
            if (edge.weight != 1.0) return true;
        }
        return false;
    }

    bool topology_has_time_valid_edges(const sugar::TopologySpec& spec) {
        return std::any_of(spec.static_edges.begin(),
            spec.static_edges.end(),
            [](const sugar::StaticGraphEdge& edge) {
                return edge.valid_from.has_value() || edge.valid_to.has_value();
            });
    }

    bool topology_supports_kind(
        const sugar::TopologySpec& spec,
        sugar::TopologyQueryKind kind) {
        return spec.supports_query_kind(kind);
    }

    void print_column_names(const sugar::FrameSchema& schema, std::ostream& out) {
        out << "Resolved schema columns\n";
        out << "  columns: ";
        const auto& reg = schema.columns();
        if (reg.size() == 0) {
            out << "<none>\n";
            return;
        }
        for (std::size_t i = 0; i < reg.size(); ++i) {
            if (i) out << ", ";
            out << reg.name_by_id(sugar::ColumnId{ static_cast<std::uint32_t>(i) });
        }
        out << "\n";
    }

    void print_layout_parse_confirmation(
        const sugar::DataSpec& ds,
        const sugar::PreprocessReport& report,
        std::ostream& out) {
        out << "Parsed CSV/layout confirmation\n";
        out << "  parsed_rows: " << report.csv_data_row_count << "\n";
        if (sugar::data_spec_has_time_axis(ds)) {
            out << "  time_column: " << ds.time_col
                << " (" << friendly_time_kind(ds.time_kind)
                << "; internal=" << to_string(ds.time_kind) << ")\n";
        }
        else {
            out << "  time_column: <none>\n";
        }
        out << "  value_columns: ";
        for (std::size_t i = 0; i < ds.value_cols.size(); ++i) {
            if (i) out << ", ";
            out << ds.value_cols[i];
        }
        if (ds.value_cols.empty()) {
            out << "<none>";
        }
        out << "\n";
        out << "  primary_value_column: " << ds.primary_value_col;
        if (ds.value_cols.size() == 1 && ds.primary_value_col == ds.value_cols.front()) {
            out << " (single value column)";
        }
        out << "\n";
        if (ds.value_cols_inferred_from_primary_value_col) {
            out << "  layout_warning: SINGLE VALUE COLUMN ASSUMED; `value_cols` was omitted and inferred from `primary_value_col`.\n";
        }
        if (ds.primary_value_col_inferred_from_single_value_col) {
            out << "  layout_note: `primary_value_col` was omitted and inferred from the single `value_cols` entry.\n";
        }
    }

    int rounded_missing_percent(const sugar::ValueColumnQuality& stats) {
        if (stats.total_values == 0) return 0;
        return static_cast<int>(
            std::round(
                (100.0 * static_cast<double>(stats.missing_values)) /
                static_cast<double>(stats.total_values)));
    }

    void print_value_column_quality(
        const std::vector<sugar::ValueColumnQuality>& quality,
        std::ostream& out) {
        if (quality.empty()) {
            return;
        }
        out << "  value_column_quality:\n";
        for (const auto& stats : quality) {
            out << "    - " << stats.name
                << ": missing=" << stats.missing_values << "/"
                << stats.total_values << " ("
                << rounded_missing_percent(stats) << "%)"
                << ", finite=" << stats.finite_values << "/"
                << stats.total_values << "\n";
        }
    }

    void print_resolved_observation_domain(
        const sugar::ObservationWorldFacts& facts,
        const std::shared_ptr<sugar::FrameSchema>& schema,
        std::ostream& out) {
        out << "Resolved observation domain\n";
        out << "  row_domain_size: " << facts.row_count << "\n";
        out << "  series_count: " << facts.series_count << "\n";
        out << "  has_time_axis: " << (facts.has_time_axis ? "true" : "false") << "\n";
        out << "  has_spatial_axis: " << (facts.has_spatial_axis ? "true" : "false") << "\n";
        out << "  has_entity_axis: " << (facts.has_entity_axis ? "true" : "false") << "\n";
        if (schema) {
            print_column_names(*schema, out);
        }
    }

    std::size_t expected_topology_domain_size(
        const sugar::TopologySpec& spec,
        const sugar::ObservationWorldFacts& facts) {
        if (spec.node_count > 0) return spec.node_count;
        if (spec.domain_scope == sugar::TopologyDomainScope::EntityPanel) {
            return facts.series_count;
        }
        return facts.row_count;
    }

    void print_semantic_embedding_layout_warnings(
        const sugar::TopologySpec& spec,
        std::ostream& out) {
        if (spec.family != sugar::TopologyFamily::SemanticGraph ||
            spec.semantic_embeddings.empty()) {
            return;
        }

        const std::size_t expected_dim =
            spec.semantic_embeddings.front().values.size();
        for (const auto& embedding : spec.semantic_embeddings) {
            if (embedding.values.size() != expected_dim) {
                out << "    warning: semantic embedding vector dimensions differ; node="
                    << embedding.node << " has dimension "
                    << embedding.values.size() << ", expected "
                    << expected_dim
                    << ". Normalize embeddings before using topology operators.\n";
                break;
            }
        }

        if (spec.node_count == 0) {
            return;
        }
        for (const auto& embedding : spec.semantic_embeddings) {
            if (embedding.node >= spec.node_count) {
                out << "    warning: semantic embedding node=" << embedding.node
                    << " is outside node_count=" << spec.node_count
                    << ". Keep embedding node ids inside the topology node domain.\n";
                break;
            }
        }
    }

    void print_topology_layout_diagnostics(
        const sugar::ObservationWorldFacts& facts,
        std::ostream& out) {
        bool wrote_header = false;
        bool wrote_row_domain_note = false;
        for (const auto& spec : facts.available_topologies) {
            if (spec.origin != sugar::TopologyOrigin::LayoutDeclared) continue;
            if (!wrote_header) {
                out << "Topology layout diagnostics\n";
                out << "  row_domain_size: " << facts.row_count << "\n";
                out << "  series_count: " << facts.series_count << "\n";
                wrote_header = true;
            }

            out << "  topology " << spec.name
                << ": scope=" << sugar::topology_domain_scope_name(spec.domain_scope)
                << ", node_count=" << (spec.node_count == 0 ? std::string("<default>") : std::to_string(spec.node_count))
                << ", resolved_node_domain=" << expected_topology_domain_size(spec, facts)
                << ", declared_queries=" << query_kind_list(spec.query_kinds)
                << "\n";

            if (spec.domain_scope == sugar::TopologyDomainScope::RowDomain &&
                !wrote_row_domain_note) {
                out << "    note: domain_scope=row_domain means topology node ids index the aligned row domain, not raw CSV physical line numbers.\n";
                wrote_row_domain_note = true;
            }

            if (spec.node_count > 0 &&
                spec.domain_scope != sugar::TopologyDomainScope::EntityPanel &&
                spec.node_count != facts.row_count) {
                out << "    warning: explicit node_count=" << spec.node_count
                    << " differs from aligned row_domain_size=" << facts.row_count
                    << "; omit node_count to default topology nodes to the aligned row domain.\n";
            }

            if (topology_has_weight_payload(spec) &&
                !topology_supports_kind(spec, sugar::TopologyQueryKind::WeightedNeighborSet)) {
                out << "    warning: weighted edge payload is present but query_kinds does not declare weighted_neighbor_set; topology.weighted_sum requires that query kind.\n";
            }

            if (topology_has_time_valid_edges(spec) && !facts.has_time_axis) {
                out << "    warning: time-valid edges are declared with valid_from/valid_to, but the layout has no time axis";
                if (facts.has_spatial_axis) {
                    out << "; spatial axes do not provide graph time";
                }
                out << ". Add `time_col`/`time_kind` or remove the time-valid edge fields.\n";
            }

            print_semantic_embedding_layout_warnings(spec, out);
        }
    }

    void print_topology_list(const std::vector<sugar::TopologySpec>& topologies,
        std::ostream& out) {
        out << "  topologies: ";
        if (topologies.empty()) {
            out << "<none>\n";
            return;
        }
        out << "\n";
        for (const auto& spec : topologies) {
            out << "    " << spec.name
                << " [" << sugar::topology_family_name(spec.family)
                << ", origin=" << sugar::topology_origin_name(spec.origin)
                << ", bearing=" << sugar::topology_bearing_name(spec.bearing)
                << ", scope=" << sugar::topology_domain_scope_name(spec.domain_scope)
                << ", node_count="
                << (spec.node_count == 0 ? std::string("<default>") : std::to_string(spec.node_count))
                << ", edges=" << spec.static_edges.size()
                << ", embeddings=" << spec.semantic_embeddings.size()
                << ", queries=";
            if (spec.query_kinds.empty()) {
                out << "<none>";
            }
            else {
                for (std::size_t i = 0; i < spec.query_kinds.size(); ++i) {
                    if (i) out << ", ";
                    out << sugar::topology_query_kind_name(spec.query_kinds[i]);
                }
            }
            if (spec.artifact_ref.has_value() && !spec.artifact_ref->empty()) {
                out << ", artifact=";
                const auto artifact_id = sugar::topology_artifact_identity(*spec.artifact_ref);
                if (!artifact_id.empty()) {
                    out << artifact_id;
                }
                else {
                    out << "<inline>";
                }
                if (!spec.artifact_ref->producer.empty()) {
                    out << ", producer=" << spec.artifact_ref->producer;
                }
            }
            out << "]\n";
        }
    }

    bool node_mentions_topology_usage(const sugar::PlanNodeCapability& node) {
        return !node.resolved_topology_name.empty() ||
            !node.default_topology_name.empty() ||
            !node.required_topology_families.empty() ||
            !node.required_topology_query_kinds.empty() ||
            node.has_locality_subject ||
            node.has_runtime_locality_subject;
    }

    std::string format_locality_subject(const sugar::PlanNodeCapability& node) {
        if (!node.has_locality_subject && !node.has_runtime_locality_subject) {
            return {};
        }

        std::ostringstream oss;
        if (node.has_runtime_locality_subject) {
            oss << sugar::locality_subject_kind_name(node.runtime_locality_subject.kind);
            if (!node.runtime_locality_subject.value.empty()) {
                oss << "(" << node.runtime_locality_subject.value << ")";
            }
            if (node.runtime_locality_subject.has_series() || node.runtime_locality_subject.has_entity()) {
                oss << " -> ";
                bool first = true;
                if (node.runtime_locality_subject.has_series()) {
                    oss << "series=" << node.runtime_locality_subject.series_key;
                    first = false;
                }
                if (node.runtime_locality_subject.has_entity()) {
                    if (!first) oss << ", ";
                    oss << "entity=" << node.runtime_locality_subject.entity_key;
                }
            }
            return oss.str();
        }

        oss << sugar::locality_subject_kind_name(node.locality_subject.kind);
        if (!node.locality_subject.value.empty()) {
            oss << "(" << node.locality_subject.value << ")";
        }
        return oss.str();
    }

    void print_topology_usage_summary(const sugar::PlanCapabilityReport& report,
        std::ostream& out) {
        bool wrote_header = false;
        for (const auto& node : report.nodes) {
            if (!node_mentions_topology_usage(node)) continue;
            if (!wrote_header) {
                out << "  topology_usage:\n";
                wrote_header = true;
            }

            out << "    " << (node.node_id.empty() ? std::string("<missing>") : node.node_id)
                << " (" << node.op_name << "): ";

            const std::string topology_name =
                !node.resolved_topology_name.empty()
                ? node.resolved_topology_name
                : node.default_topology_name;
            out << "topology=" << (topology_name.empty() ? std::string("<none>") : topology_name);
            if (!node.required_topology_query_kinds.empty()) {
                out << ", required_query_kinds="
                    << query_kind_list(node.required_topology_query_kinds);
            }
            if (!topology_name.empty()) {
                if (const auto* topology = find_topology_by_name(
                        report.facts.available_topologies,
                        topology_name)) {
                    out << ", declared_query_kinds="
                        << query_kind_list(topology->query_kinds);
                    std::vector<sugar::TopologyQueryKind> missing_queries;
                    for (const auto kind : node.required_topology_query_kinds) {
                        if (!topology->supports_query_kind(kind)) {
                            missing_queries.push_back(kind);
                        }
                    }
                    if (!missing_queries.empty()) {
                        out << ", missing_query_kinds="
                            << query_kind_list(missing_queries);
                    }
                    if (topology_has_weight_payload(*topology) &&
                        !topology->supports_query_kind(sugar::TopologyQueryKind::WeightedNeighborSet)) {
                        out << ", hint=add weighted_neighbor_set to layout.topologies[].query_kinds for topology.weighted_sum";
                    }
                }
            }

            const std::string locality = format_locality_subject(node);
            if (!locality.empty()) {
                out << ", locality=" << locality;
            }
            out << "\n";
        }
    }

    std::optional<std::string> string_param(
        const sugar::NodeSpec& node,
        const std::string& key) {
        const auto it = node.params.find(key);
        if (it == node.params.end()) {
            return std::nullopt;
        }
        if (const auto* value = std::get_if<std::string>(&it->second)) {
            return *value;
        }
        return std::nullopt;
    }

    std::string column_name_from_ref_token(std::string token) {
        const auto colon = token.find(':');
        if (colon != std::string::npos && colon + 1 < token.size()) {
            return token.substr(colon + 1);
        }
        return token;
    }

    const sugar::ValueColumnQuality* find_value_column_quality(
        const sugar::PreprocessReport& report,
        const std::string& column_name) {
        for (const auto& stats : report.value_column_quality) {
            if (stats.name == column_name) {
                return &stats;
            }
        }
        return nullptr;
    }

    bool value_column_quality_is_noteworthy(
        const sugar::ValueColumnQuality& stats) {
        if (stats.total_values == 0) {
            return false;
        }
        return stats.finite_values == 0 ||
            stats.missing_values * 2 > stats.total_values;
    }

    std::optional<std::string> direct_column_reference_for_node(
        const sugar::NodeSpec& node,
        const sugar::PreprocessReport& report) {
        if (node.op == "core.ref" || node.op == "core.col" || node.op == "ms.col") {
            if (const auto ref = string_param(node, "ref")) {
                return column_name_from_ref_token(*ref);
            }
            return std::nullopt;
        }
        if (node.op == "core.precomputed_num") {
            if (const auto name = string_param(node, "name")) {
                return column_name_from_ref_token(*name);
            }
            return std::nullopt;
        }
        if (node.op == "core.close") {
            if (const auto primary = string_param(node, "primary_value_col")) {
                return *primary;
            }
            return report.data_spec.primary_value_col;
        }
        return std::nullopt;
    }

    template <class T>
    void print_alignment_provenance(const T& manifest_owner, std::ostream& out) {
        if (!manifest_owner.run_manifest.preprocess.alignment_applied) {
            return;
        }
        out << "  alignment_calendar: " << to_string(manifest_owner.run_manifest.preprocess.alignment_calendar) << "\n";
        out << "  alignment_missing: " << to_string(manifest_owner.run_manifest.preprocess.alignment_missing) << "\n";
        out << "  alignment_non_temporal_multi_series: "
            << sugar::align::non_temporal_multi_series_policy_name(
                manifest_owner.run_manifest.preprocess.alignment_non_temporal_multi_series)
            << "\n";
    }

} // namespace

namespace sugar::app {

    void print_preprocess_diagnostics(
        const sugar::PreprocessReport& report,
        std::ostream& out) {
        out << "Input diagnostics\n";
        out << "  header_present: " << (report.csv_had_header ? "true" : "false") << "\n";
        out << "  csv_physical_lines: " << report.csv_physical_line_count << "\n";
        out << "  csv_rows: " << report.csv_data_row_count << "\n";
        out << "  blank_rows_skipped: " << report.csv_blank_line_count << "\n";
        out << "  rows_dropped: " << report.dropped_row_count << "\n";
        out << "  parsed_columns: ";
        if (report.parsed_columns.empty()) {
            out << "<none>";
        }
        else {
            for (std::size_t i = 0; i < report.parsed_columns.size(); ++i) {
                if (i) out << ", ";
                out << report.parsed_columns[i];
            }
        }
        out << "\n";
        print_value_column_quality(report.value_column_quality, out);
        if (report.input_warnings.empty()) {
            out << "  warnings: <none>\n";
            return;
        }

        out << "  warnings:\n";
        for (const auto& warning : report.input_warnings) {
            out << "    - " << warning << "\n";
        }
    }

    void print_data_spec_summary(const sugar::DataSpec& ds, std::ostream& out) {
        out << "DataSpec summary\n";
        out << "  input_profile: " << to_string(ds.input_profile) << "\n";
        if (sugar::data_spec_has_time_axis(ds)) {
            out << "  time_col: " << ds.time_col << "\n";
            out << "  time_kind: " << to_string(ds.time_kind) << "\n";
        }
        else {
            out << "  time_col: <none>\n";
            out << "  time_kind: <none>\n";
        }
        out << "  primary_value_col: " << ds.primary_value_col << "\n";
        out << "  entity_col: "
            << (ds.entity_col ? *ds.entity_col : std::string("<none>"))
            << "\n";
        out << "  spatial_axis: ";
        if (sugar::data_spec_has_spatial_axis(ds)) {
            out << *ds.spatial_x_col << "," << *ds.spatial_y_col << "\n";
        }
        else {
            out << "<none>\n";
        }
        out << "  header_policy: " << to_string(ds.header_policy) << "\n";
        out << "  dedupe: " << to_string(ds.dedupe) << "\n";
        out << "  missing_default: " << to_string(ds.missing_default.kind);
        if (ds.missing_default.kind == sugar::MissingKind::Const) {
            out << "(" << ds.missing_default.const_value << ")";
        }
        out << "\n";

        out << "  value_cols: ";
        for (std::size_t i = 0; i < ds.value_cols.size(); ++i) {
            if (i) out << ", ";
            out << ds.value_cols[i];
        }
        out << "\n";
        if (ds.value_cols_inferred_from_primary_value_col) {
            out << "  layout_warning: SINGLE VALUE COLUMN ASSUMED; `value_cols` was omitted and inferred from `primary_value_col`.\n";
        }
        if (ds.primary_value_col_inferred_from_single_value_col) {
            out << "  layout_note: `primary_value_col` was omitted and inferred from the single `value_cols` entry.\n";
        }

        out << "  rename: ";
        if (ds.rename.empty()) {
            out << "<none>\n";
        }
        else {
            bool first = true;
            for (const auto& kv : ds.rename) {
                if (!first) out << ", ";
                first = false;
                out << kv.first << "->" << kv.second;
            }
            out << "\n";
        }

        out << "  missing_tokens: ";
        for (std::size_t i = 0; i < ds.missing_tokens.size(); ++i) {
            if (i) out << ", ";
            out << ds.missing_tokens[i];
        }
        out << "\n";

        out << "  columns_by_index: ";
        if (ds.columns_by_index.empty()) {
            out << "<none>\n";
        }
        else {
            std::vector<std::pair<std::size_t, std::string>> by_col;
            by_col.reserve(ds.columns_by_index.size());
            for (const auto& kv : ds.columns_by_index) {
                by_col.emplace_back(kv.second, kv.first);
            }
            std::sort(by_col.begin(), by_col.end());
            bool first = true;
            for (const auto& kv : by_col) {
                if (!first) out << ", ";
                first = false;
                out << kv.first << "->" << kv.second;
            }
            out << "\n";
        }
    }

    void print_column_registry(const sugar::FrameSchema& schema, std::ostream& out) {
        out << "Schema columns\n";
        const auto& reg = schema.columns();
        for (std::size_t i = 0; i < reg.size(); ++i) {
            out << "  [" << i << "] " << reg.name_by_id(sugar::ColumnId{ static_cast<std::uint32_t>(i) }) << "\n";
        }
    }

    void print_frame_summary(const sugar::NumericFrame& f, std::ostream& out, std::string_view prefix) {
        out << prefix << "rows: " << f.row_count() << "\n";
        if (f.row_count() > 0) {
            if (const auto mm = time_minmax(f)) {
                const auto [mn, mx] = *mm;
                out << prefix << "time: " << mn << ".." << mx << "\n";
            }
            else {
                out << prefix << "row_domain: 0.." << (f.row_count() - 1) << "\n";
            }
        }
        print_missing_stats(f, out, std::string(prefix));
    }

    void print_panel_result(
        const sugar::PanelResult& r,
        std::ostream& out,
        const sugar::RunProvenanceSummary* provenance) {
        print_panel_report_summary(build_panel_report(r, provenance), out);
    }

    void print_align_dump(const sugar::align::AlignedBundle& b, std::ostream& out) {
        out << "Alignment dump\n";
        out << "  rows=" << b.row_count() << "\n";
        out << "  series_count=" << b.series_keys.size() << "\n";
        const auto time = b.time_axis();
        if (!time.empty()) {
            out << "  time=" << time.front() << ".." << time.back() << "\n";
        }
        else if (b.row_count() > 0) {
            out << "  row_domain=0.." << (b.row_count() - 1) << "\n";
        }

        for (std::size_t i = 0; i < b.series_keys.size() && i < b.frames.size(); ++i) {
            out << "  series[" << i << "]=" << b.series_keys[i] << "\n";
            const auto& f = b.frames[i];
            const auto& reg = b.schema->columns();
            const std::size_t ncols = reg.size();
            for (std::size_t ci = 0; ci < ncols; ++ci) {
                const sugar::ColumnId id{ static_cast<std::uint32_t>(ci) };
                if (!f.has_col(id)) continue;
                const auto col = f.col(id);
                std::size_t miss = 0;
                for (double v : col) {
                    if (!std::isfinite(v)) ++miss;
                }
                out << "    col=" << reg.name_by_id(id)
                    << " missing=" << miss << "/" << col.size()
                    << "\n";
            }
        }
    }

    void print_observation_capability_report(
        const sugar::ObservationCapabilityReport& report,
        std::ostream& out) {
        out << "Observation capability\n";
        for (const auto& surface : report.surfaces) {
            out << "  " << sugar::observation_surface_name(surface.surface)
                << ": " << sugar::observation_capability_status_name(surface.status);
            if (!surface.reasons.empty()) {
                out << " - ";
                print_reason_list(surface.reasons, out);
            }
            out << "\n";
        }
        print_topology_list(report.facts.available_topologies, out);
    }

    void print_observation_capability_report(
        const sugar::ResolvedRuntimeInput& runtime,
        std::ostream& out) {
        out << "Runtime provenance\n";
        out << "  primary_series: "
            << (runtime.run_manifest.primary_series_key.empty()
                ? std::string("<none>")
                : runtime.run_manifest.primary_series_key)
            << "\n";
        out << "  primary_selection_mode: "
            << sugar::runtime_primary_selection_mode_name(runtime.run_manifest.primary_selection_mode)
            << "\n";
        if (runtime.ingest_spec) {
            print_layout_parse_confirmation(
                *runtime.ingest_spec,
                runtime.run_manifest.preprocess,
                out);
        }
        print_preprocess_diagnostics(runtime.run_manifest.preprocess, out);
        print_alignment_provenance(runtime, out);
        const auto facts = sugar::make_observation_world_facts(runtime);
        print_resolved_observation_domain(
            facts,
            runtime.aligned_bundle ? runtime.aligned_bundle->schema :
                (runtime.frameset ? runtime.frameset->schema : std::shared_ptr<sugar::FrameSchema>{}),
            out);
        print_topology_layout_diagnostics(facts, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(facts),
            out);
    }

    void print_observation_capability_report(
        const sugar::ResolvedObservationWorld& world,
        std::ostream& out) {
        out << "Observation world provenance\n";
        if (world.ingest_spec) {
            print_layout_parse_confirmation(
                *world.ingest_spec,
                world.run_manifest.preprocess,
                out);
        }
        print_preprocess_diagnostics(world.run_manifest.preprocess, out);
        print_alignment_provenance(world, out);
        const auto facts = sugar::make_observation_world_facts(world);
        print_resolved_observation_domain(
            facts,
            world.aligned_bundle ? world.aligned_bundle->schema :
                (world.frameset ? world.frameset->schema : std::shared_ptr<sugar::FrameSchema>{}),
            out);
        print_topology_layout_diagnostics(facts, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(facts),
            out);
    }

    void print_observation_report_json(
        const sugar::ResolvedObservationWorld& world,
        std::ostream& out) {
        const auto facts = sugar::make_observation_world_facts(world);
        const auto capability_report = sugar::diagnose_observation_capabilities(facts);

        out << "{";
        out << "\"schema_version\":\"sugarbot.observation_report.v1\"";
        out << ",\"kind\":\"observation_report\"";
        out << ",\"ok\":true";
        out << ",\"source_kind\":";
        append_json_string(out, runtime_source_kind_name(world.source_kind));
        out << ",\"primary_input_path\":";
        append_json_string(out, world.primary_input_path);
        if (world.ingest_spec.has_value()) {
            out << ",\"layout\":";
            append_json_data_spec_summary(out, *world.ingest_spec);
        }
        out << ",\"input\":";
        append_json_input_diagnostics(out, world.run_manifest.preprocess);
        out << ",\"domain\":";
        append_json_observation_domain(out, facts);
        out << ",\"series_retention\":";
        append_json_series_retention(out, world);
        out << ",\"capabilities\":";
        append_json_observation_capabilities(out, capability_report);
        out << ",\"topologies\":";
        append_json_topologies(out, capability_report.facts.available_topologies);
        out << "}\n";
    }

    void print_observation_report_error_json(
        std::string_view code,
        std::string_view message,
        const sugar::PreprocessReport* report,
        std::ostream& out) {
        out << "{";
        out << "\"schema_version\":\"sugarbot.observation_report.v1\"";
        out << ",\"kind\":\"observation_report\"";
        out << ",\"ok\":false";
        out << ",\"error\":{";
        out << "\"code\":";
        append_json_string(out, code.empty() ? std::string_view("unknown") : code);
        out << ",\"message\":";
        append_json_string(out, message);
        out << "}";
        if (report != nullptr) {
            out << ",\"layout\":";
            append_json_data_spec_summary(out, report->data_spec);
            out << ",\"input\":";
            append_json_input_diagnostics(out, *report);
            out << ",\"error_aggregates\":";
            append_json_preprocess_aggregates(
                out,
                report->preprocess_error_aggregates);
        }
        out << "}\n";
    }

    void print_plan_capability_report(
        const sugar::PlanCapabilityReport& report,
        std::ostream& out,
        std::string_view stage_label) {
        out << "Plan compatibility";
        if (!stage_label.empty()) {
            out << " (" << stage_label << ")";
        }
        out << "\n";
        out << "  status: " << sugar::plan_capability_status_name(report.status) << "\n";
        out << "  total_nodes: " << report.total_nodes << "\n";
        out << "  incompatible_nodes: " << report.incompatible_nodes << "\n";
        out << "  incompatible_bindings: " << report.incompatible_bindings << "\n";
        out << "  axis_scopes: ";
        if (report.used_axis_scopes.empty()) {
            out << "<none>";
        }
        else {
            for (std::size_t i = 0; i < report.used_axis_scopes.size(); ++i) {
                if (i) out << ", ";
                out << sugar::plan_axis_scope_name(report.used_axis_scopes[i]);
            }
        }
        out << "\n";
        out << "  topology_families: ";
        if (report.used_topology_families.empty()) {
            out << "<none>";
        }
        else {
            for (std::size_t i = 0; i < report.used_topology_families.size(); ++i) {
                if (i) out << ", ";
                out << sugar::topology_family_name(report.used_topology_families[i]);
            }
        }
        out << "\n";
        out << "  topology_names: ";
        if (report.used_topology_names.empty()) {
            out << "<none>";
        }
        else {
            for (std::size_t i = 0; i < report.used_topology_names.size(); ++i) {
                if (i) out << ", ";
                out << report.used_topology_names[i];
            }
        }
        out << "\n";
        print_topology_usage_summary(report, out);

        if (report.incompatible_nodes == 0 && report.incompatible_bindings == 0) {
            return;
        }

        if (report.incompatible_nodes > 0) {
            out << "  incompatible_nodes_detail:\n";
        }
        for (const auto& node : report.nodes) {
            if (node.status != sugar::PlanCapabilityStatus::Invalid) continue;
            out << "    " << sugar::plan_node_kind_name(node.node_kind)
                << " " << (node.node_id.empty() ? std::string("<missing>") : node.node_id)
                << " (" << node.op_name;
            if (node.original_op_name != node.op_name) {
                out << " from " << node.original_op_name;
            }
            out << ", scope=" << sugar::plan_axis_scope_name(node.axis_scope);
            if (!node.resolved_topology_name.empty()) {
                out << ", topology=" << node.resolved_topology_name;
            }
            const std::string locality = format_locality_subject(node);
            if (!locality.empty()) {
                out << ", locality=" << locality;
            }
            out << "): ";
            for (std::size_t i = 0; i < node.reasons.size(); ++i) {
                if (i) out << "; ";
                out << sugar::plan_capability_reason_text(node.reasons[i]);
            }
            out << "\n";
        }

        if (report.incompatible_bindings > 0) {
            out << "  incompatible_bindings_detail:\n";
            for (const auto& binding : report.bindings) {
                if (binding.status != sugar::PlanCapabilityStatus::Invalid) continue;
                out << "    " << binding.binding_name << " -> " << binding.target_id << ": ";
                for (std::size_t i = 0; i < binding.reasons.size(); ++i) {
                    if (i) out << "; ";
                    out << sugar::plan_capability_reason_text(binding.reasons[i]);
                }
                out << "\n";
            }
        }
    }

    void print_plan_data_quality_notes(
        const sugar::PlanSpec& spec,
        const sugar::PreprocessReport& report,
        std::ostream& out) {
        bool wrote_header = false;
        auto visit = [&](std::string_view node_kind, const sugar::NodeSpec& node) {
            const auto column = direct_column_reference_for_node(node, report);
            if (!column.has_value()) {
                return;
            }
            const auto* stats = find_value_column_quality(report, *column);
            if (stats == nullptr || !value_column_quality_is_noteworthy(*stats)) {
                return;
            }
            if (!wrote_header) {
                out << "Plan data-quality notes\n";
                wrote_header = true;
            }
            out << "  - " << node_kind << " node "
                << (node.id.empty() ? std::string("<missing>") : node.id)
                << " (" << node.op << ") reads value column '" << *column
                << "', which has " << rounded_missing_percent(*stats)
                << "% missing values after preprocessing ("
                << stats->missing_values << "/" << stats->total_values
                << " rows)\n";
        };

        for (const auto& node : spec.numeric_nodes) {
            visit("numeric", node);
        }
        for (const auto& node : spec.bool_nodes) {
            visit("bool", node);
        }
    }

    void print_plan_binding_notes(
        const sugar::PlanSpec& spec,
        std::ostream& out) {
        const bool has_long_entry = !spec.bind.entry_long.empty();
        const bool has_short_entry = !spec.bind.entry_short.empty();
        if (has_long_entry || has_short_entry) {
            return;
        }

        out << "Plan binding notes\n"
            << "  - no entry bindings are active: `bindings.entry_long` and `bindings.entry_short` are empty, so this PlanSpec can validate but cannot open positions. Add an entry binding before `run` if zero trades are not intentional.\n";
    }

    void print_plan_capability_report(
        const sugar::PlanSpec& spec,
        const sugar::ResolvedRuntimeInput& runtime,
        std::ostream& out) {
        print_plan_capability_report(
            sugar::diagnose_plan_capabilities(spec, runtime),
            out);
        print_plan_data_quality_notes(spec, runtime.run_manifest.preprocess, out);
        print_plan_binding_notes(spec, out);
    }

} // namespace sugar::app
