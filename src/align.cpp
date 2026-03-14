#include "align.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace sugar::align {

    namespace {
        bool schema_compatible(const FrameSchema& a, const FrameSchema& b) {
            const auto& ra = a.columns();
            const auto& rb = b.columns();
            if (ra.size() != rb.size()) return false;
            for (std::size_t i = 0; i < ra.size(); ++i) {
                ColumnId id{ static_cast<std::uint32_t>(i) };
                if (ra.name_by_id(id) != rb.name_by_id(id)) return false;
            }
            return true;
        }

        std::optional<std::pair<std::string, std::string>> spatial_signature(
            const std::shared_ptr<IndexSchema>& idx) {
            if (!idx) return std::nullopt;
            const auto s = idx->spatial_axis_2d();
            if (!s.has_value()) return std::nullopt;
            return std::make_pair(s->x_col, s->y_col);
        }

        bool spatial_axis_compatible(
            const std::vector<std::pair<std::string, const NumericFrame*>>& ordered,
            std::string& mismatch_series) {
            if (ordered.empty()) return true;
            const auto expected = spatial_signature(ordered.front().second->index_schema());
            for (std::size_t i = 1; i < ordered.size(); ++i) {
                const auto& kv = ordered[i];
                const auto got = spatial_signature(kv.second->index_schema());
                if (got.has_value() != expected.has_value()) {
                    mismatch_series = kv.first;
                    return false;
                }
                if (got.has_value() && *got != *expected) {
                    mismatch_series = kv.first;
                    return false;
                }
            }
            return true;
        }

        std::shared_ptr<IndexSchema> pick_output_index_schema(
            const std::vector<std::pair<std::string, const NumericFrame*>>& ordered) {
            for (const auto& kv : ordered) {
                if (kv.second->index_schema()) return kv.second->index_schema();
            }
            bool any_time_axis = false;
            for (const auto& kv : ordered) {
                if (kv.second->has_time_axis()) {
                    any_time_axis = true;
                    break;
                }
            }
            if (!any_time_axis) return {};
            return ordered.size() > 1 ? IndexSchema::make_time_entity() : IndexSchema::make_time_only();
        }

        std::vector<TimeKey> sorted_unique_times(std::span<const TimeKey> t) {
            std::vector<TimeKey> out(t.begin(), t.end());
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        void mount_runtime_topologies(AlignedBundle& bundle) {
            if (!bundle.topology_catalog) {
                bundle.topology_catalog = std::make_shared<TopologyCatalog>();
            }
            mount_builtin_runtime_topologies(
                *bundle.topology_catalog,
                bundle.has_axis(AxisKind::Time),
                bundle.has_axis(AxisKind::Spatial));
        }
    } // namespace

    std::size_t AlignedBundle::find_series_index(std::string_view key) const {
        for (std::size_t i = 0; i < series_keys.size(); ++i) {
            if (series_keys[i] == key) return i;
        }
        return static_cast<std::size_t>(-1);
    }

    std::size_t AlignedBundle::find_series_index_for_entity(std::string_view key) const {
        for (std::size_t i = 0; i < series_keys.size(); ++i) {
            if (entity_key_for_series_index(i) == key) return i;
        }
        return static_cast<std::size_t>(-1);
    }

    std::size_t AlignedBundle::row_count() const {
        if (!time.empty()) return time.size();
        if (!frames.empty()) return frames.front().row_count();
        return 0;
    }

    bool AlignedBundle::empty() const {
        return row_count() == 0;
    }

    bool AlignedBundle::has_index_schema() const {
        return static_cast<bool>(index_schema);
    }

    bool AlignedBundle::has_time_axis() const {
        if (index_schema) return index_schema->has_kind(AxisKind::Time);
        return !time.empty();
    }

    std::span<const AxisDef> AlignedBundle::axes() const {
        if (!index_schema) return {};
        return std::span<const AxisDef>(index_schema->axes().data(), index_schema->axes().size());
    }

    bool AlignedBundle::has_axis(AxisKind kind) const {
        return index_schema && index_schema->has_kind(kind);
    }

    bool AlignedBundle::has_axis(std::string_view name) const {
        return index_schema && index_schema->has_axis(name);
    }

    const AxisDef* AlignedBundle::axis(AxisKind kind) const {
        if (!index_schema) return nullptr;
        for (const auto& a : index_schema->axes()) {
            if (a.kind == kind) return &a;
        }
        return nullptr;
    }

    const AxisDef* AlignedBundle::axis(std::string_view name) const {
        if (!index_schema) return nullptr;
        for (const auto& a : index_schema->axes()) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }

    std::string_view AlignedBundle::entity_key_for_series_index(std::size_t index) const {
        if (index < entity_keys.size() && !entity_keys[index].empty()) {
            return entity_keys[index];
        }
        if (index < series_keys.size()) {
            return series_keys[index];
        }
        return {};
    }

    std::span<const TimeKey> AlignedBundle::time_axis() const {
        if (!has_axis(AxisKind::Time)) return {};
        return std::span<const TimeKey>(time.data(), time.size());
    }

    AlignedBundle align_frames(
        std::shared_ptr<FrameSchema> schema,
        const std::vector<std::pair<std::string, NumericFrame>>& inputs,
        AlignConfig cfg)
    {
        if (!schema) throw std::invalid_argument("align_frames: schema is null");
        if (inputs.empty()) throw std::invalid_argument("align_frames: inputs is empty");

        std::vector<std::pair<std::string, const NumericFrame*>> ordered;
        ordered.reserve(inputs.size());
        for (const auto& kv : inputs) {
            ordered.push_back({ kv.first, &kv.second });
        }
        std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });

        for (std::size_t i = 1; i < ordered.size(); ++i) {
            if (ordered[i - 1].first == ordered[i].first) {
                throw std::invalid_argument("align_frames: duplicate series key: " + ordered[i].first);
            }
        }

        for (const auto& kv : ordered) {
            const auto& f = *kv.second;
            if (!schema_compatible(*schema, f.schema())) {
                throw std::invalid_argument("align_frames: schema mismatch for series: " + kv.first);
            }
        }
        {
            std::string mismatch_series;
            if (!spatial_axis_compatible(ordered, mismatch_series)) {
                throw std::invalid_argument("align_frames: spatial axis mismatch for series: " + mismatch_series);
            }
        }

        bool any_time_axis = false;
        bool all_time_axis = true;
        for (const auto& kv : ordered) {
            const bool has_time_axis = kv.second->has_time_axis();
            any_time_axis = any_time_axis || has_time_axis;
            all_time_axis = all_time_axis && has_time_axis;
        }
        if (any_time_axis && !all_time_axis) {
            throw std::invalid_argument("align_frames: mixed temporal and non-temporal series are not supported");
        }

        std::vector<TimeKey> calendar;
        if (!any_time_axis) {
            if (ordered.size() > 1) {
                throw std::invalid_argument("align_frames: non-temporal multi-series alignment requires explicit policy");
            }
        }
        else if (cfg.calendar == CalendarPolicy::Union) {
            std::vector<TimeKey> all;
            for (const auto& kv : ordered) {
                const auto t = kv.second->time();
                all.insert(all.end(), t.begin(), t.end());
            }
            std::sort(all.begin(), all.end());
            all.erase(std::unique(all.begin(), all.end()), all.end());
            calendar = std::move(all);
        }
        else {
            std::unordered_map<TimeKey, std::size_t> counts;
            for (const auto& kv : ordered) {
                const auto uniq = sorted_unique_times(kv.second->time());
                for (TimeKey t : uniq) counts[t] += 1;
            }
            calendar.reserve(counts.size());
            for (const auto& kv : counts) {
                if (kv.second == ordered.size()) calendar.push_back(kv.first);
            }
            std::sort(calendar.begin(), calendar.end());
        }

        std::unordered_map<TimeKey, std::size_t> pos;
        pos.reserve(calendar.size() * 2 + 1);
        for (std::size_t i = 0; i < calendar.size(); ++i) pos.emplace(calendar[i], i);

        AlignedBundle out{};
        out.schema = std::move(schema);
        out.index_schema = pick_output_index_schema(ordered);
        out.topology_catalog = std::make_shared<TopologyCatalog>();
        out.time = calendar;
        out.series_keys.reserve(ordered.size());
        out.entity_keys.reserve(ordered.size());
        out.frames.reserve(ordered.size());
        mount_runtime_topologies(out);

        const std::size_t ncols = out.schema->columns().size();
        const double qnan = std::numeric_limits<double>::quiet_NaN();

        if (!any_time_axis) {
            const auto& kv = ordered.front();
            out.series_keys.push_back(kv.first);
            out.entity_keys.push_back(kv.first);

            const NumericFrame& src = *kv.second;
            NumericFrame dst(out.schema, src.row_count());
            dst.set_index_schema(out.index_schema);

            for (std::size_t ci = 0; ci < ncols; ++ci) {
                ColumnId id{ static_cast<std::uint32_t>(ci) };
                if (!src.has_col(id)) continue;

                auto out_col = dst.ensure_col(id, qnan);
                const auto in_col = src.col(id);
                std::copy(in_col.begin(), in_col.end(), out_col.begin());
            }

            out.frames.push_back(std::move(dst));
            return out;
        }

        for (const auto& kv : ordered) {
            out.series_keys.push_back(kv.first);
            out.entity_keys.push_back(kv.first);

            const NumericFrame& src = *kv.second;
            NumericFrame dst(out.schema, out.time);
            dst.set_index_schema(out.index_schema);

            const auto src_time = src.time();
            for (std::size_t ci = 0; ci < ncols; ++ci) {
                ColumnId id{ static_cast<std::uint32_t>(ci) };
                if (!src.has_col(id)) continue;

                auto out_col = dst.ensure_col(id, qnan);
                const auto in_col = src.col(id);

                for (std::size_t i = 0; i < src_time.size(); ++i) {
                    auto it = pos.find(src_time[i]);
                    if (it == pos.end()) continue;
                    out_col[it->second] = in_col[i];
                }
            }

            out.frames.push_back(std::move(dst));
        }

        return out;
    }

} // namespace sugar::align
