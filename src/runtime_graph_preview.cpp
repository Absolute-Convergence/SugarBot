#include "runtime_graph_preview.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "utils.h"

namespace sugar {

    namespace {

        const char* time_kind_name(TimeKind kind) {
            switch (kind) {
            case TimeKind::YyyyMmDd: return "YyyyMmDd";
            case TimeKind::Iso8601: return "Iso8601";
            case TimeKind::YearQuarter: return "YearQuarter";
            case TimeKind::YearMonth: return "YearMonth";
            case TimeKind::Index: return "Index";
            default: return "Unknown";
            }
        }

        std::string format_yyyymmdd_dashed(TimeKey tk) {
            char buf[9];
            format_yyyymmdd(tk, buf);
            std::string out(buf, 8);
            if (out.size() != 8) return std::to_string(tk);
            out.insert(4, 1, '-');
            out.insert(7, 1, '-');
            return out;
        }

        std::string format_iso8601_utc(TimeKey tk) {
            const std::time_t tt = static_cast<std::time_t>(tk);
            std::tm tm{};
#ifdef _WIN32
            if (gmtime_s(&tm, &tt) != 0) return std::to_string(tk);
#else
            if (gmtime_r(&tt, &tm) == nullptr) return std::to_string(tk);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return oss.str();
        }

        std::string format_time_key_for_preview(TimeKey tk, TimeKind kind) {
            switch (kind) {
            case TimeKind::YyyyMmDd:
                return format_yyyymmdd_dashed(tk);
            case TimeKind::Iso8601:
                return format_iso8601_utc(tk);
            case TimeKind::YearQuarter: {
                const auto year = static_cast<long long>(tk / 10);
                const auto q = static_cast<long long>(tk % 10);
                return std::to_string(year) + "-Q" + std::to_string(q);
            }
            case TimeKind::YearMonth: {
                const auto year = static_cast<long long>(tk / 100);
                const auto month = static_cast<long long>(tk % 100);
                std::ostringstream oss;
                oss << year << "-" << std::setw(2) << std::setfill('0') << month;
                return oss.str();
            }
            case TimeKind::Index:
            default:
                return std::to_string(tk);
            }
        }

        std::string primary_column_name(const DataView& dv, const ColumnRef& primary) {
            const auto& bundle = dv.bundle();
            if (!bundle.schema) return "col#" + std::to_string(primary.col.v);
            const auto& reg = bundle.schema->columns();
            if (primary.col.v >= reg.size()) return "col#" + std::to_string(primary.col.v);
            return reg.name_by_id(primary.col);
        }

        std::vector<double> bin_values(std::span<const double> values, std::size_t bins) {
            std::vector<double> out;
            if (values.empty() || bins == 0) return out;

            bins = std::min(bins, values.size());
            out.reserve(bins);
            for (std::size_t i = 0; i < bins; ++i) {
                const std::size_t begin = (i * values.size()) / bins;
                std::size_t end = ((i + 1) * values.size()) / bins;
                if (end <= begin) end = std::min(values.size(), begin + 1);

                double acc = 0.0;
                std::size_t count = 0;
                for (std::size_t j = begin; j < end; ++j) {
                    if (!std::isfinite(values[j])) continue;
                    acc += values[j];
                    ++count;
                }
                out.push_back(count ? (acc / static_cast<double>(count))
                                    : std::numeric_limits<double>::quiet_NaN());
            }
            return out;
        }

        std::string make_ascii_graph(std::span<const double> values, std::size_t width) {
            static constexpr char kLevels[] = ".:-=+*#%@";

            const std::vector<double> bins = bin_values(values, width);
            if (bins.empty()) return {};

            double min_v = std::numeric_limits<double>::infinity();
            double max_v = -std::numeric_limits<double>::infinity();
            for (double v : bins) {
                if (!std::isfinite(v)) continue;
                min_v = std::min(min_v, v);
                max_v = std::max(max_v, v);
            }

            if (!std::isfinite(min_v) || !std::isfinite(max_v)) {
                return std::string(bins.size(), ' ');
            }

            std::string out;
            out.reserve(bins.size());
            const std::size_t max_level = sizeof(kLevels) - 2;
            const bool flat = std::fabs(max_v - min_v) < 1e-12;
            for (double v : bins) {
                if (!std::isfinite(v)) {
                    out.push_back(' ');
                    continue;
                }
                if (flat) {
                    out.push_back('=');
                    continue;
                }
                const double scaled = (v - min_v) / (max_v - min_v);
                const auto idx = static_cast<std::size_t>(std::clamp(
                    std::llround(scaled * static_cast<double>(max_level)),
                    0LL,
                    static_cast<long long>(max_level)));
                out.push_back(kLevels[idx]);
            }
            return out;
        }

        std::vector<std::size_t> sample_indices(std::size_t n) {
            std::vector<std::size_t> out;
            if (n == 0) return out;

            auto add = [&](std::size_t idx) {
                if (idx >= n) idx = n - 1;
                if (std::find(out.begin(), out.end(), idx) == out.end()) {
                    out.push_back(idx);
                }
            };

            add(0);
            add(n / 3);
            add((2 * n) / 3);
            add(n - 1);
            return out;
        }

        std::string summarize_series_keys(const align::AlignedBundle& bundle, std::string_view primary_series) {
            if (bundle.series_keys.empty()) return "(none)";

            std::ostringstream oss;
            const std::size_t shown = std::min<std::size_t>(bundle.series_keys.size(), 5);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i) oss << ", ";
                if (bundle.series_keys[i] == primary_series) {
                    oss << "[" << bundle.series_keys[i] << "]";
                }
                else {
                    oss << bundle.series_keys[i];
                }
            }
            if (bundle.series_keys.size() > shown) {
                oss << ", ... +" << (bundle.series_keys.size() - shown) << " more";
            }
            return oss.str();
        }

        std::string format_row_anchor(std::size_t row_idx) {
            return "row#" + std::to_string(row_idx);
        }

        void print_observation_capability_report(
            const ObservationCapabilityReport& report,
            std::ostream& out) {
            out << "Observation capability\n";
            for (const auto& surface : report.surfaces) {
                out << "  " << observation_surface_name(surface.surface)
                    << ": " << observation_capability_status_name(surface.status);
                if (!surface.reasons.empty()) {
                    out << " - ";
                    for (std::size_t i = 0; i < surface.reasons.size(); ++i) {
                        if (i) out << "; ";
                        out << observation_capability_reason_text(surface.reasons[i]);
                    }
                }
                out << "\n";
            }
            out << "  topologies: ";
            if (report.facts.available_topologies.empty()) {
                out << "<none>\n";
                return;
            }
            out << "\n";
            for (const auto& spec : report.facts.available_topologies) {
                out << "    " << spec.name
                    << " [" << topology_family_name(spec.family)
                    << ", scope=" << topology_domain_scope_name(spec.domain_scope)
                    << ", queries=";
                if (spec.query_kinds.empty()) {
                    out << "<none>";
                }
                else {
                    for (std::size_t i = 0; i < spec.query_kinds.size(); ++i) {
                        if (i) out << ", ";
                        out << topology_query_kind_name(spec.query_kinds[i]);
                    }
                }
                if (spec.artifact_ref.has_value() && !spec.artifact_ref->empty()) {
                    out << ", artifact=";
                    const auto artifact_id = topology_artifact_identity(*spec.artifact_ref);
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

    } // namespace

    void print_runtime_graph_preview(
        const DataView& dv,
        const ColumnRef& primary,
        const RunManifest& manifest,
        std::string_view source_label,
        std::ostream& out,
        std::size_t graph_width,
        const ObservationCapabilityReport* capability_report) {
        const auto& bundle = dv.bundle();
        const auto time = dv.time();
        const bool has_time_axis = manifest.preprocess.has_time_axis && !time.empty();

        out << std::fixed << std::setprecision(4);
        out << "Runtime graph preview\n";
        out << "  source: " << source_label << "\n";
        out << "  primary: " << primary.series << ":" << primary_column_name(dv, primary) << "\n";
        out << "  series_count: " << bundle.series_keys.size() << "\n";
        out << "  series_keys: " << summarize_series_keys(bundle, primary.series) << "\n";
        out << "  time_kind: " << (has_time_axis ? time_kind_name(manifest.preprocess.time_kind) : "<none>") << "\n";
        out << "  time_resolution: " << (has_time_axis ? manifest.preprocess.time_resolution : "<none>") << "\n";
        if (capability_report != nullptr) {
            print_observation_capability_report(*capability_report, out);
        }

        if (!dv.has_col(primary)) {
            out << "  rows: " << dv.row_count() << "\n";
            out << "  graph: (primary column missing)\n\n";
            return;
        }

        const auto values = dv.col(primary);
        out << "  rows: " << values.size() << "\n";
        if (values.empty()) {
            out << "  graph: (empty)\n\n";
            return;
        }
        if (has_time_axis) {
            out << "  time_range: "
                << format_time_key_for_preview(time.front(), manifest.preprocess.time_kind)
                << " .. "
                << format_time_key_for_preview(time.back(), manifest.preprocess.time_kind)
                << "\n";
        }
        else {
            out << "  row_domain: 0 .. " << (values.size() - 1) << "\n";
        }

        std::size_t finite_count = 0;
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        std::size_t last_finite_idx = values.size();
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double v = values[i];
            if (!std::isfinite(v)) continue;
            ++finite_count;
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
            last_finite_idx = i;
        }

        out << "  finite_primary: " << finite_count << "/" << values.size() << "\n";
        if (finite_count == 0) {
            out << "  graph[" << std::min(graph_width, values.size()) << "]: |"
                << make_ascii_graph(values, graph_width) << "|\n";
            out << "  samples: (no finite primary values)\n\n";
            return;
        }

        out << "  min/max: " << min_v << " / " << max_v << "\n";
        out << "  latest: " << values[last_finite_idx] << " @ "
            << (has_time_axis
                ? format_time_key_for_preview(time[last_finite_idx], manifest.preprocess.time_kind)
                : format_row_anchor(last_finite_idx))
            << "\n";
        const std::string graph = make_ascii_graph(values, graph_width);
        out << "  graph[" << graph.size() << "]: |" << graph << "|\n";
        out << "  samples:\n";
        for (const std::size_t idx : sample_indices(values.size())) {
            out << "    [" << std::setw(5) << idx << "] "
                << (has_time_axis
                    ? format_time_key_for_preview(time[idx], manifest.preprocess.time_kind)
                    : format_row_anchor(idx))
                << " -> ";
            if (std::isfinite(values[idx])) {
                out << values[idx];
            }
            else {
                out << "NaN";
            }
            out << "\n";
        }
        out << "\n";
    }

} // namespace sugar
