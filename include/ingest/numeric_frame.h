#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <span>

#include "ingest/time_key.h"
#include "ingest/index_schema.h"

namespace sugar {

    struct ColumnId {
        std::uint32_t v = 0;
    };

    class ColumnRegistry {
    public:
        ColumnId get_or_add(std::string_view name) {
            auto it = name_to_id_.find(std::string(name));
            if (it != name_to_id_.end()) return it->second;

            const ColumnId id{ static_cast<std::uint32_t>(id_to_name_.size()) };
            id_to_name_.emplace_back(name);
            name_to_id_.emplace(id_to_name_.back(), id);
            return id;
        }

        bool find(std::string_view name, ColumnId& out) const {
            auto it = name_to_id_.find(std::string(name));
            if (it == name_to_id_.end()) return false;
            out = it->second;
            return true;
        }

        std::size_t size() const { return id_to_name_.size(); }
        const std::string& name_by_id(ColumnId id) const {
            return id_to_name_.at(static_cast<std::size_t>(id.v));
        }

    private:
        std::unordered_map<std::string, ColumnId> name_to_id_;
        std::vector<std::string> id_to_name_;
    };

    class FrameSchema {
    public:
        ColumnRegistry& columns() { return columns_; }
        const ColumnRegistry& columns() const { return columns_; }

    private:
        ColumnRegistry columns_;
    };

    class NumericFrame {
    public:
        explicit NumericFrame(FrameSchema& schema)
            : schema_(&schema), row_count_(0) {
        }

        explicit NumericFrame(std::shared_ptr<FrameSchema> schema)
            : schema_owner_(std::move(schema)), schema_(schema_owner_.get()), row_count_(0) {
            assert(schema_ && "NumericFrame shared schema constructor requires non-null schema.");
        }

        NumericFrame(FrameSchema& schema, std::size_t row_count)
            : schema_(&schema), row_count_(row_count) {
        }

        NumericFrame(std::shared_ptr<FrameSchema> schema, std::size_t row_count)
            : schema_owner_(std::move(schema)), schema_(schema_owner_.get()), row_count_(row_count) {
            assert(schema_ && "NumericFrame shared schema row-domain constructor requires non-null schema.");
        }

        NumericFrame(FrameSchema& schema, std::vector<TimeKey> time)
            : schema_(&schema), row_count_(time.size()), time_(std::move(time)) {
        }

        NumericFrame(std::shared_ptr<FrameSchema> schema, std::vector<TimeKey> time)
            : schema_owner_(std::move(schema)), schema_(schema_owner_.get()), row_count_(time.size()), time_(std::move(time)) {
            assert(schema_ && "NumericFrame shared schema time constructor requires non-null schema.");
        }

        std::size_t row_count() const { return row_count_; }
        std::span<const TimeKey> time() const { return std::span<const TimeKey>(time_.data(), time_.size()); }
        FrameSchema& schema() {
            assert(schema_ && "NumericFrame::schema() called with null schema pointer.");
            return *schema_;
        }
        const FrameSchema& schema() const {
            assert(schema_ && "NumericFrame::schema() called with null schema pointer.");
            return *schema_;
        }
        void set_index_schema(std::shared_ptr<IndexSchema> s) { index_schema_ = std::move(s); }
        const std::shared_ptr<IndexSchema>& index_schema() const { return index_schema_; }
        bool has_index_schema() const { return static_cast<bool>(index_schema_); }
        bool has_axis(AxisKind kind) const {
            if (index_schema_) return index_schema_->has_kind(kind);
            return kind == AxisKind::Time && !time_.empty();
        }
        bool has_time_axis() const { return has_axis(AxisKind::Time); }
        bool has_schema_owner() const { return static_cast<bool>(schema_owner_); }

        bool has_col(ColumnId id) const {
            const std::size_t i = static_cast<std::size_t>(id.v);
            return i < present_.size() && present_[i] != 0u;
        }

        std::span<const double> col(ColumnId id) const {
            const std::size_t i = static_cast<std::size_t>(id.v);
            if (!has_col(id)) return {};
            return std::span<const double>(cols_[i].data(), cols_[i].size());
        }

        std::span<double> ensure_col(ColumnId id, double fill = std::numeric_limits<double>::quiet_NaN()) {
            const std::size_t i = static_cast<std::size_t>(id.v);
            if (i >= cols_.size()) {
                cols_.resize(i + 1);
                present_.resize(i + 1, 0u);
            }

            auto& c = cols_[i];
            if (c.size() != row_count()) {
                c.assign(row_count(), fill);
            }
            present_[i] = 1u;
            return std::span<double>(c.data(), c.size());
        }

    private:
        std::shared_ptr<FrameSchema> schema_owner_;
        FrameSchema* schema_ = nullptr;
        std::shared_ptr<IndexSchema> index_schema_;
        std::size_t row_count_ = 0;
        std::vector<TimeKey> time_;
        std::vector<std::vector<double>> cols_;
        std::vector<std::uint8_t> present_;
    };

    struct FrameSet {
        std::shared_ptr<FrameSchema> schema;
        std::shared_ptr<IndexSchema> index_schema;
        std::unordered_map<std::string, NumericFrame> by_entity;
    };

} // namespace sugar
