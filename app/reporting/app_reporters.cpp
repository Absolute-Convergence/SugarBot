#include "reporting/app_reporters.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

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

            const std::string locality = format_locality_subject(node);
            if (!locality.empty()) {
                out << ", locality=" << locality;
            }
            out << "\n";
        }
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

    void print_panel_result(const sugar::PanelResult& r, std::ostream& out) {
        out << "Panel result\n";
        out << "  entities_total=" << r.entities_total
            << " ok=" << r.entities_ok
            << " failed=" << r.entities_failed << "\n";
        out << "  ranking_metric=" << sugar::panel_ranking_metric_name(r.ranking.metric);
        if (r.ranking.metric == sugar::PanelRankingMetric::PnlDd) {
            out << " dd_weight=" << r.ranking.dd_weight;
        }
        out << "\n";

        for (std::size_t i = 0; i < r.topk.size(); ++i) {
            const auto& row = r.topk[i];
            out << "  [" << i << "] series=" << row.series_key;
            if (row.entity_key != row.series_key) {
                out << " entity=" << row.entity_key;
            }
            out << " metric=" << sugar::panel_ranking_metric_name(row.ranking_metric);
            out << " ranking_score=" << row.ranking_score
                << " pnl=" << row.metrics.pnl
                << " trades=" << row.metrics.trades
                << "\n";
        }

        if (!r.failures.empty()) {
            out << "  failures:\n";
            for (const auto& f : r.failures) {
                out << "    series=" << f.series_key;
                if (f.entity_key != f.series_key) {
                    out << " entity=" << f.entity_key;
                }
                out << ": " << f.reason << "\n";
            }
        }
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
        print_preprocess_diagnostics(runtime.run_manifest.preprocess, out);
        print_alignment_provenance(runtime, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(runtime)),
            out);
    }

    void print_observation_capability_report(
        const sugar::ResolvedObservationWorld& world,
        std::ostream& out) {
        out << "Observation world provenance\n";
        print_preprocess_diagnostics(world.run_manifest.preprocess, out);
        print_alignment_provenance(world, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(world)),
            out);
    }

    void print_plan_capability_report(
        const sugar::PlanCapabilityReport& report,
        std::ostream& out) {
        out << "Plan compatibility\n";
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

    void print_plan_capability_report(
        const sugar::PlanSpec& spec,
        const sugar::ResolvedRuntimeInput& runtime,
        std::ostream& out) {
        print_plan_capability_report(
            sugar::diagnose_plan_capabilities(spec, runtime),
            out);
    }

} // namespace sugar::app
