#include "data_view.h"

#include <cctype>
#include <stdexcept>

namespace sugar {

    namespace {
        std::shared_ptr<const TopologyCatalog> empty_topology_catalog() {
            static const auto kEmpty = std::make_shared<TopologyCatalog>();
            return kEmpty;
        }

        std::string trim_copy(std::string_view value) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            std::size_t begin = 0;
            while (begin < value.size() && is_space(static_cast<unsigned char>(value[begin]))) {
                ++begin;
            }
            std::size_t end = value.size();
            while (end > begin && is_space(static_cast<unsigned char>(value[end - 1]))) {
                --end;
            }
            return std::string(value.substr(begin, end - begin));
        }

        void ensure_series_available(
            const align::AlignedBundle& bundle,
            std::string_view series,
            std::string_view caller) {
            const std::size_t si = bundle.find_series_index(series);
            if (si == static_cast<std::size_t>(-1) || si >= bundle.frames.size()) {
                throw std::runtime_error(std::string(caller) + ": series not found: " + std::string(series));
            }
        }

        const TopologySpec& builtin_temporal_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_temporal_topology_spec();
            return kSpec;
        }

        const TopologySpec& builtin_spatial_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_spatial_topology_spec();
            return kSpec;
        }

        const TopologySpec& builtin_spatiotemporal_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_spatiotemporal_topology_spec();
            return kSpec;
        }

        const TopologySpec* resolve_builtin_topology_spec(
            const align::AlignedBundle& bundle,
            std::string_view topology_name) {
            if (topology_name == builtin_temporal_topology_name()) {
                if (!bundle.has_axis(AxisKind::Time)) return nullptr;
                if (bundle.topology_catalog) {
                    if (const auto* spec = bundle.topology_catalog->find(topology_name)) return spec;
                }
                return &builtin_temporal_topology_spec_ref();
            }
            if (topology_name == builtin_spatial_topology_name()) {
                if (!bundle.has_axis(AxisKind::Spatial)) return nullptr;
                if (bundle.topology_catalog) {
                    if (const auto* spec = bundle.topology_catalog->find(topology_name)) return spec;
                }
                return &builtin_spatial_topology_spec_ref();
            }
            if (topology_name == builtin_spatiotemporal_topology_name()) {
                if (!bundle.has_axis(AxisKind::Time) || !bundle.has_axis(AxisKind::Spatial)) return nullptr;
                if (bundle.topology_catalog) {
                    if (const auto* spec = bundle.topology_catalog->find(topology_name)) return spec;
                }
                return &builtin_spatiotemporal_topology_spec_ref();
            }
            return nullptr;
        }

        std::string default_runtime_topology_name(const align::AlignedBundle& bundle) {
            if (bundle.has_axis(AxisKind::Time) && bundle.has_axis(AxisKind::Spatial)) {
                return std::string(builtin_spatiotemporal_topology_name());
            }
            if (bundle.has_axis(AxisKind::Spatial)) {
                return std::string(builtin_spatial_topology_name());
            }
            if (bundle.has_axis(AxisKind::Time)) {
                return std::string(builtin_temporal_topology_name());
            }
            return {};
        }

        NeighborhoodAnchor make_subject_anchor(const RuntimeLocalitySubject& subject) {
            NeighborhoodAnchor anchor{};
            anchor.series = subject.series_key;
            anchor.entity = subject.entity_key;
            return anchor;
        }
    } // namespace

    std::span<const TimeKey> DataView::time() const {
        return time_axis();
    }

    std::span<const TimeKey> DataView::time_axis() const {
        if (!bundle_) return {};
        return bundle_->time_axis();
    }

    std::size_t DataView::size() const {
        return row_count();
    }

    std::size_t DataView::row_count() const {
        if (!bundle_) return 0;
        return bundle_->row_count();
    }

    bool DataView::empty() const {
        return row_count() == 0;
    }

    std::span<const AxisDef> DataView::axes() const {
        if (!bundle_) return {};
        return bundle_->axes();
    }

    bool DataView::has_axis(AxisKind kind) const {
        return bundle_ && bundle_->has_axis(kind);
    }

    bool DataView::has_axis(std::string_view name) const {
        return bundle_ && bundle_->has_axis(name);
    }

    const AxisDef* DataView::axis(AxisKind kind) const {
        if (!bundle_) return nullptr;
        return bundle_->axis(kind);
    }

    const AxisDef* DataView::axis(std::string_view name) const {
        if (!bundle_) return nullptr;
        return bundle_->axis(name);
    }

    std::shared_ptr<const TopologyCatalog> DataView::topology_catalog() const {
        if (!bundle_ || !bundle_->topology_catalog) return empty_topology_catalog();
        return bundle_->topology_catalog;
    }

    const TopologySpec* DataView::topology(std::string_view name) const {
        if (!bundle_) return nullptr;
        if (const auto* builtin = resolve_builtin_topology_spec(*bundle_, name)) {
            return builtin;
        }
        if (bundle_->topology_catalog) {
            return bundle_->topology_catalog->find(name);
        }
        return nullptr;
    }

    bool DataView::has_topology(std::string_view name) const {
        return topology(name) != nullptr;
    }

    bool DataView::has_topology(TopologyFamily family) const {
        if (!bundle_) return false;
        switch (family) {
        case TopologyFamily::TemporalWindow:
            if (bundle_->has_axis(AxisKind::Time)) return true;
            break;
        case TopologyFamily::SpatialMetric2D:
            if (bundle_->has_axis(AxisKind::Spatial)) return true;
            break;
        case TopologyFamily::SpatioTemporalComposite:
            if (bundle_->has_axis(AxisKind::Time) && bundle_->has_axis(AxisKind::Spatial)) return true;
            break;
        default:
            break;
        }
        return bundle_->topology_catalog && bundle_->topology_catalog->has_family(family);
    }

    std::optional<SpatialAxisValues2D> DataView::spatial_axis_2d(std::string_view series) const {
        if (!bundle_ || !bundle_->index_schema) return std::nullopt;
        const auto spatial = bundle_->index_schema->spatial_axis_2d();
        if (!spatial.has_value()) return std::nullopt;

        const std::size_t si = bundle_->find_series_index(series);
        if (si == static_cast<std::size_t>(-1) || si >= bundle_->frames.size()) return std::nullopt;

        ColumnId x_id{};
        ColumnId y_id{};
        if (!bundle_->schema || !bundle_->schema->columns().find(spatial->x_col, x_id) ||
            !bundle_->schema->columns().find(spatial->y_col, y_id)) {
            return std::nullopt;
        }

        const auto& frame = bundle_->frames[si];
        if (!frame.has_col(x_id) || !frame.has_col(y_id)) return std::nullopt;

        return SpatialAxisValues2D{
            spatial->x_col,
            spatial->y_col,
            frame.col(x_id),
            frame.col(y_id)
        };
    }

    RuntimeLocalitySubject DataView::resolve_locality_subject(
        const ResolvedLocalitySubject& subject,
        std::string_view compatibility_primary_series) const {
        if (!bundle_) {
            throw std::runtime_error("DataView::resolve_locality_subject() called with null bundle.");
        }

        RuntimeLocalitySubject out{};
        out.kind = subject.kind;
        out.value = trim_copy(subject.value);
        out.explicit_binding = subject.explicit_binding;

        auto bind_series = [&](std::string_view series_key) {
            ensure_series_available(*bundle_, series_key, "DataView::resolve_locality_subject()");
            out.series_key = std::string(series_key);
            const std::size_t index = bundle_->find_series_index(series_key);
            out.entity_key = std::string(bundle_->entity_key_for_series_index(index));
            if (out.entity_key.empty()) {
                out.entity_key = out.series_key;
            }
        };

        switch (subject.kind) {
        case LocalitySubjectKind::PrimarySeries:
            if (!compatibility_primary_series.empty()) {
                bind_series(compatibility_primary_series);
                return out;
            }
            if (bundle_->series_keys.size() == 1) {
                bind_series(bundle_->series_keys.front());
                return out;
            }
            throw std::runtime_error(
                "DataView::resolve_locality_subject(): primary_series requires an explicit compatibility primary series when runtime has multiple series.");

        case LocalitySubjectKind::SeriesKey:
            if (out.value.empty()) {
                throw std::runtime_error(
                    "DataView::resolve_locality_subject(): series_key subject requires a non-empty bound value.");
            }
            bind_series(out.value);
            return out;

        case LocalitySubjectKind::PrimaryEntity:
            if (!compatibility_primary_series.empty()) {
                bind_series(compatibility_primary_series);
                return out;
            }
            if (bundle_->series_keys.size() == 1) {
                bind_series(bundle_->series_keys.front());
                return out;
            }
            throw std::runtime_error(
                "DataView::resolve_locality_subject(): primary_entity requires an explicit compatibility primary series when runtime has multiple series.");

        case LocalitySubjectKind::EntityKey: {
            if (out.value.empty()) {
                throw std::runtime_error(
                    "DataView::resolve_locality_subject(): entity_key subject requires a non-empty bound value.");
            }

            std::size_t match_index = static_cast<std::size_t>(-1);
            std::size_t match_count = 0;
            for (std::size_t i = 0; i < bundle_->series_keys.size(); ++i) {
                if (bundle_->entity_key_for_series_index(i) != out.value) continue;
                if (match_index == static_cast<std::size_t>(-1)) {
                    match_index = i;
                }
                ++match_count;
            }
            if (match_index == static_cast<std::size_t>(-1)) {
                throw std::runtime_error(
                    "DataView::resolve_locality_subject(): entity_key '" + out.value + "' is not available on this runtime.");
            }
            if (match_count > 1) {
                throw std::runtime_error(
                    "DataView::resolve_locality_subject(): entity_key '" + out.value +
                    "' maps to multiple series and cannot yet resolve to one series-local topology subject.");
            }
            bind_series(bundle_->series_keys[match_index]);
            out.entity_key = out.value;
            return out;
        }

        case LocalitySubjectKind::ObservationDomain:
            return out;
        }

        throw std::runtime_error("DataView::resolve_locality_subject(): unknown subject kind.");
    }

    std::unique_ptr<NeighborhoodQuery> DataView::resolve_topology(
        const RuntimeLocalitySubject& subject,
        std::string_view topology_name) const {
        if (!bundle_) {
            throw std::runtime_error("DataView::resolve_topology() called with null bundle.");
        }

        std::string chosen_name = topology_name.empty()
            ? default_runtime_topology_name(*bundle_)
            : std::string(topology_name);
        if (chosen_name.empty()) {
            throw std::runtime_error("DataView::resolve_topology(): runtime exposes no resolvable topology.");
        }

        const TopologySpec* spec = topology(chosen_name);
        if (!spec) {
            throw std::runtime_error(
                "DataView::resolve_topology(): topology '" + chosen_name + "' is not available on this runtime.");
        }

        if (chosen_name == builtin_temporal_topology_name()) {
            const auto time = time_axis();
            if (time.empty()) {
                throw std::runtime_error(
                    "DataView::resolve_topology(): temporal topology requires a semantic time axis.");
            }
            return bind_neighborhood_query_subject(
                std::make_unique<TimeNeighborhoodQuery>(time),
                make_subject_anchor(subject));
        }

        if (spec->domain_scope == TopologyDomainScope::SeriesLocal && !subject.has_series()) {
            throw std::runtime_error(
                "DataView::resolve_topology(): topology '" + chosen_name +
                "' requires a resolved series-local subject.");
        }
        if (spec->domain_scope == TopologyDomainScope::EntityPanel && !subject.has_entity()) {
            throw std::runtime_error(
                "DataView::resolve_topology(): topology '" + chosen_name +
                "' requires a resolved entity-local subject.");
        }

        const auto spatial = subject.has_series()
            ? spatial_axis_2d(subject.series_key)
            : std::optional<SpatialAxisValues2D>{};
        if (chosen_name == builtin_spatial_topology_name()) {
            if (!spatial.has_value()) {
                throw std::runtime_error(
                    "DataView::resolve_topology(): spatial topology requires spatial axis payloads on the resolved series-local subject.");
            }
            return bind_neighborhood_query_subject(
                std::make_unique<SpatialNeighborhoodQuery2D>(spatial->x, spatial->y),
                make_subject_anchor(subject));
        }

        if (chosen_name == builtin_spatiotemporal_topology_name()) {
            const auto time = time_axis();
            if (time.empty() || !spatial.has_value()) {
                throw std::runtime_error(
                    "DataView::resolve_topology(): spatio-temporal topology requires both time and spatial payloads on the resolved series-local subject.");
            }
            return bind_neighborhood_query_subject(
                std::make_unique<SpatioTemporalNeighborhoodQuery2D>(time, spatial->x, spatial->y),
                make_subject_anchor(subject));
        }

        throw std::runtime_error(
            "DataView::resolve_topology(): topology '" + chosen_name +
            "' is present but runtime query adapters for family '" +
            std::string(topology_family_name(spec->family)) + "' are not implemented yet.");
    }

    std::unique_ptr<NeighborhoodQuery> DataView::resolve_topology(
        const ResolvedLocalitySubject& subject,
        std::string_view compatibility_primary_series,
        std::string_view topology_name) const {
        return resolve_topology(
            resolve_locality_subject(subject, compatibility_primary_series),
            topology_name);
    }

    std::unique_ptr<NeighborhoodQuery> DataView::resolve_topology(
        std::string_view series,
        std::string_view topology_name) const {
        ResolvedLocalitySubject subject{};
        subject.kind = LocalitySubjectKind::SeriesKey;
        subject.value = std::string(series);
        subject.explicit_binding = true;
        return resolve_topology(subject, {}, topology_name);
    }

    std::unique_ptr<NeighborhoodQuery> DataView::make_neighborhood_query(
        const ResolvedLocalitySubject& subject,
        std::string_view compatibility_primary_series) const {
        return resolve_topology(subject, compatibility_primary_series);
    }

    std::unique_ptr<NeighborhoodQuery> DataView::make_neighborhood_query(std::string_view series) const {
        return resolve_topology(series);
    }

    const align::AlignedBundle& DataView::bundle() const {
        if (!bundle_) throw std::runtime_error("DataView::bundle() called with null bundle.");
        return *bundle_;
    }

    const std::shared_ptr<IndexSchema>& DataView::index_schema() const {
        static const std::shared_ptr<IndexSchema> kNull{};
        if (!bundle_) return kNull;
        return bundle_->index_schema;
    }

    bool DataView::has_col(const ColumnRef& ref) const {
        if (!bundle_) return false;
        const std::size_t si = bundle_->find_series_index(ref.series);
        if (si == static_cast<std::size_t>(-1) || si >= bundle_->frames.size()) return false;
        return bundle_->frames[si].has_col(ref.col);
    }

    std::span<const double> DataView::col(const ColumnRef& ref) const {
        if (!bundle_) return {};
        const std::size_t si = bundle_->find_series_index(ref.series);
        if (si == static_cast<std::size_t>(-1) || si >= bundle_->frames.size()) {
            throw std::out_of_range("DataView::col series not found: " + ref.series);
        }
        if (!bundle_->frames[si].has_col(ref.col)) {
            throw std::out_of_range("DataView::col column missing for series: " + ref.series);
        }
        return bundle_->frames[si].col(ref.col);
    }

} // namespace sugar
