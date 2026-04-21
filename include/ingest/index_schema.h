#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sugar {

    // Minimal axis contract for indexed frame-like data.
    // Current supported kinds are Time + Entity.
    // Space is a placeholder to keep the model extensible.
    enum class AxisKind : std::uint8_t {
        Time = 0,
        Entity = 1,
        Spatial = 2,
    };

    // Optional axis-level alignment intent metadata.
    // Existing runtime alignment remains time-calendar only; this contract is
    // additive scaffolding for future axis-aware alignment policies.
    enum class AxisAlignmentHint : std::uint8_t {
        None = 0,
        Calendar = 1,
        SpatialGrid = 2,
        Coordinates2D = 3,
    };

    struct SpatialAxis2D {
        std::string x_col;
        std::string y_col;
    };

    struct AxisDef {
        std::string name;
        AxisKind kind{ AxisKind::Time };
        bool ordered{ true };
        std::optional<SpatialAxis2D> spatial2d{};
        AxisAlignmentHint align_hint{ AxisAlignmentHint::None };
        bool required{ true };
        std::optional<std::string> units{};
        std::optional<std::string> resolution{};
    };

    class IndexSchema {
    public:
        IndexSchema() = default;

        static std::shared_ptr<IndexSchema> make_time_only() {
            auto s = std::make_shared<IndexSchema>();
            AxisDef t{};
            t.name = "time";
            t.kind = AxisKind::Time;
            t.ordered = true;
            t.align_hint = AxisAlignmentHint::Calendar;
            s->axes_.push_back(std::move(t));
            return s;
        }

        static std::shared_ptr<IndexSchema> make_time_entity() {
            auto s = std::make_shared<IndexSchema>();
            AxisDef t{};
            t.name = "time";
            t.kind = AxisKind::Time;
            t.ordered = true;
            t.align_hint = AxisAlignmentHint::Calendar;
            s->axes_.push_back(std::move(t));

            AxisDef e{};
            e.name = "entity";
            e.kind = AxisKind::Entity;
            e.ordered = true;
            s->axes_.push_back(std::move(e));
            return s;
        }

        const std::vector<AxisDef>& axes() const { return axes_; }

        bool has_axis(std::string_view name) const {
            for (const auto& a : axes_) {
                if (a.name == name) return true;
            }
            return false;
        }

        bool has_kind(AxisKind k) const {
            for (const auto& a : axes_) {
                if (a.kind == k) return true;
            }
            return false;
        }

        std::optional<SpatialAxis2D> spatial_axis_2d() const {
            for (const auto& a : axes_) {
                if (a.kind == AxisKind::Spatial && a.spatial2d.has_value()) {
                    return a.spatial2d;
                }
            }
            return std::nullopt;
        }

        void add_axis(AxisDef a) {
            axes_.push_back(std::move(a));
        }

        void add_spatial_axis_2d(std::string x_col, std::string y_col) {
            AxisDef a{};
            a.name = "spatial";
            a.kind = AxisKind::Spatial;
            a.ordered = false;
            a.align_hint = AxisAlignmentHint::Coordinates2D;
            a.spatial2d = SpatialAxis2D{ std::move(x_col), std::move(y_col) };
            axes_.push_back(std::move(a));
        }

    private:
        std::vector<AxisDef> axes_;
    };

} // namespace sugar
