#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sugar {

    enum class LocalitySubjectKind : std::uint8_t {
        PrimarySeries = 0,
        SeriesKey = 1,
        PrimaryEntity = 2,
        EntityKey = 3,
        ObservationDomain = 4,
    };

    inline std::string_view locality_subject_kind_name(LocalitySubjectKind kind) {
        switch (kind) {
        case LocalitySubjectKind::PrimarySeries: return "primary_series";
        case LocalitySubjectKind::SeriesKey: return "series_key";
        case LocalitySubjectKind::PrimaryEntity: return "primary_entity";
        case LocalitySubjectKind::EntityKey: return "entity_key";
        case LocalitySubjectKind::ObservationDomain: return "observation_domain";
        }
        return "unknown";
    }

    struct ResolvedLocalitySubject {
        LocalitySubjectKind kind{ LocalitySubjectKind::PrimarySeries };
        std::string value{};
        bool explicit_binding{ false };
    };

    struct RuntimeLocalitySubject {
        LocalitySubjectKind kind{ LocalitySubjectKind::PrimarySeries };
        std::string value{};
        bool explicit_binding{ false };
        std::string series_key{};
        std::string entity_key{};

        bool has_series() const { return !series_key.empty(); }
        bool has_entity() const { return !entity_key.empty(); }
        bool is_observation_domain() const {
            return kind == LocalitySubjectKind::ObservationDomain;
        }
    };

} // namespace sugar
