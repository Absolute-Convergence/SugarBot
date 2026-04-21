#include "contract_suite_runtime_shared.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/plan_compiler.h"
#include "ops/op_registry.h"
#include "strategy_plan_internal.h"

namespace sugar::contract_suite_internal {

    std::shared_ptr<IndexSchema> make_spatial_only_index_schema() {
        auto idx = std::make_shared<IndexSchema>();
        idx->add_spatial_axis_2d("x", "y");
        return idx;
    }

    std::shared_ptr<IndexSchema> make_time_spatial_index_schema() {
        auto idx = IndexSchema::make_time_only();
        idx->add_spatial_axis_2d("x", "y");
        return idx;
    }

    bool make_single_series_dataview_with_index_schema(
        std::span<const double> close,
        const std::shared_ptr<IndexSchema>& index_schema,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time,
        std::span<const double> spatial_x,
        std::span<const double> spatial_y) {
        FrameSchema schema;
        NumericFrame frame(schema, close.size());
        if (index_schema && index_schema->has_kind(AxisKind::Time)) {
            std::vector<TimeKey> time;
            time.reserve(close.size());
            for (std::size_t i = 0; i < close.size(); ++i) {
                time.push_back(start_time + static_cast<TimeKey>(i));
            }
            frame = NumericFrame(schema, std::move(time));
        }
        frame.set_index_schema(index_schema);
        const ColumnId close_id = schema.columns().get_or_add("close");
        auto out = frame.ensure_col(close_id);
        std::copy(close.begin(), close.end(), out.begin());
        if (index_schema && index_schema->spatial_axis_2d().has_value()) {
            const auto spatial = *index_schema->spatial_axis_2d();
            if (spatial_x.empty() || spatial_y.empty() || spatial_x.size() != close.size() || spatial_y.size() != close.size()) {
                return false;
            }
            const ColumnId x_id = schema.columns().get_or_add(spatial.x_col);
            const ColumnId y_id = schema.columns().get_or_add(spatial.y_col);
            auto x_out = frame.ensure_col(x_id);
            auto y_out = frame.ensure_col(y_id);
            std::copy(spatial_x.begin(), spatial_x.end(), x_out.begin());
            std::copy(spatial_y.begin(), spatial_y.end(), y_out.begin());
        }
        return make_single_series_dataview_from_frame(std::move(frame), bundle_out, primary_out);
    }

    bool make_two_series_dataview_with_index_schema(
        std::span<const double> primary_close,
        std::span<const double> secondary_close,
        const std::shared_ptr<IndexSchema>& index_schema,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time,
        std::span<const double> primary_x,
        std::span<const double> primary_y,
        std::span<const double> secondary_x,
        std::span<const double> secondary_y) {
        if (primary_close.size() != secondary_close.size() ||
            primary_close.size() != primary_x.size() ||
            primary_close.size() != primary_y.size() ||
            primary_close.size() != secondary_x.size() ||
            primary_close.size() != secondary_y.size()) {
            return false;
        }

        auto schema = std::make_shared<FrameSchema>();
        std::vector<TimeKey> time;
        time.reserve(primary_close.size());
        for (std::size_t i = 0; i < primary_close.size(); ++i) {
            time.push_back(start_time + static_cast<TimeKey>(i));
        }

        const ColumnId close_id = schema->columns().get_or_add("close");
        const auto spatial = index_schema->spatial_axis_2d();
        if (!spatial.has_value()) {
            return false;
        }
        const ColumnId x_id = schema->columns().get_or_add(spatial->x_col);
        const ColumnId y_id = schema->columns().get_or_add(spatial->y_col);

        auto make_frame = [&](std::span<const double> close,
                              std::span<const double> x,
                              std::span<const double> y) {
            NumericFrame frame(*schema, std::vector<TimeKey>(time.begin(), time.end()));
            frame.set_index_schema(index_schema);
            auto close_out = frame.ensure_col(close_id);
            auto x_out = frame.ensure_col(x_id);
            auto y_out = frame.ensure_col(y_id);
            std::copy(close.begin(), close.end(), close_out.begin());
            std::copy(x.begin(), x.end(), x_out.begin());
            std::copy(y.begin(), y.end(), y_out.begin());
            return frame;
        };

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.emplace_back("A", make_frame(primary_close, primary_x, primary_y));
        inputs.emplace_back("B", make_frame(secondary_close, secondary_x, secondary_y));

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        auto bundle = std::make_shared<align::AlignedBundle>(
            align::align_frames(schema, inputs, cfg));
        bundle->entity_keys = { "entity.A", "entity.B" };

        primary_out.series = "A";
        primary_out.col = close_id;
        bundle_out = std::move(bundle);
        return true;
    }

    bool compile_and_eval_plan_signals(
        const PlanSpec& spec,
        const DataView& dv,
        const ColumnRef& primary,
        ProgramSignalOutputs& out,
        std::string& err) {
        const auto registry = make_default_registry();
        CompileEnv env{};
        CompileResult compiled{};
        if (!compile_plan(spec, registry, env, compiled, err)) {
            return false;
        }
        return evaluate_strategy_plan_program(dv, primary, compiled.plan, out, err);
    }

    const PlanNodeCapability* find_plan_node(
        const PlanCapabilityReport& report,
        std::string_view node_id) {
        for (const auto& node : report.nodes) {
            if (node.node_id == node_id) return &node;
        }
        return nullptr;
    }

    bool has_plan_reason(
        const PlanNodeCapability* node,
        PlanCapabilityReasonCode code,
        std::optional<AxisKind> axis) {
        if (!node) return false;
        for (const auto& reason : node->reasons) {
            if (reason.code != code) continue;
            if (!axis.has_value()) return true;
            if (reason.has_axis && reason.axis == *axis) return true;
        }
        return false;
    }

    bool has_plan_reason_detail(
        const PlanNodeCapability* node,
        PlanCapabilityReasonCode code,
        std::string_view detail_substring) {
        if (!node) return false;
        for (const auto& reason : node->reasons) {
            if (reason.code != code) continue;
            if (reason.detail.find(detail_substring) != std::string::npos) return true;
        }
        return false;
    }

    const PlanBindingCapability* find_plan_binding(
        const PlanCapabilityReport& report,
        std::string_view binding_name) {
        for (const auto& binding : report.bindings) {
            if (binding.binding_name == binding_name) return &binding;
        }
        return nullptr;
    }

    bool has_binding_reason(
        const PlanBindingCapability* binding,
        PlanCapabilityReasonCode code) {
        if (!binding) return false;
        for (const auto& reason : binding->reasons) {
            if (reason.code == code) return true;
        }
        return false;
    }

    bool any_plan_node_has_reason(
        const PlanCapabilityReport& report,
        PlanCapabilityReasonCode code) {
        for (const auto& node : report.nodes) {
            for (const auto& reason : node.reasons) {
                if (reason.code == code) return true;
            }
        }
        return false;
    }

    const TopologyUsageSnapshot* find_topology_usage(
        const RunManifest& manifest,
        std::string_view subject_id,
        std::string_view topology_name) {
        for (const auto& usage : manifest.topology_usages) {
            if (usage.subject_kind == "plan_node" &&
                usage.subject_id == subject_id &&
                usage.topology_name == topology_name) {
                return &usage;
            }
        }
        return nullptr;
    }

    PlanSpec make_spatial_radius_mean_plan(
        std::string name,
        double radius,
        double threshold,
        std::string topology_name,
        std::optional<std::string> subject_series) {
        PlanSpec spec{};
        spec.version = 1;
        spec.name = std::move(name);
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec c{};
        c.id = "c";
        c.op = "core.close";

        NodeSpec mean{};
        mean.id = "mean";
        mean.op = "spatial.radius_mean";
        mean.args = { "c" };
        mean.params["radius"] = radius;
        mean.params["topology"] = std::move(topology_name);
        if (subject_series.has_value()) {
            mean.params["subject_series"] = std::move(*subject_series);
        }

        NodeSpec thr{};
        thr.id = "thr";
        thr.op = "core.const";
        thr.params["value"] = threshold;

        spec.numeric_nodes = { c, mean, thr };

        NodeSpec entry{};
        entry.id = "entry";
        entry.op = "cmp.gt";
        entry.args = { "mean", "thr" };

        spec.bool_nodes = { entry };
        spec.bind.entry_long = "entry";
        return spec;
    }

    PlanSpec make_spatiotemporal_radius_mean_plan(
        std::string name,
        std::int64_t last_n,
        double radius,
        double threshold,
        std::string topology_name) {
        PlanSpec spec{};
        spec.version = 1;
        spec.name = std::move(name);
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec c{};
        c.id = "c";
        c.op = "core.close";

        NodeSpec mean{};
        mean.id = "mean";
        mean.op = "st.radius_mean";
        mean.args = { "c" };
        mean.params["last_n"] = last_n;
        mean.params["radius"] = radius;
        mean.params["topology"] = std::move(topology_name);

        NodeSpec thr{};
        thr.id = "thr";
        thr.op = "core.const";
        thr.params["value"] = threshold;

        spec.numeric_nodes = { c, mean, thr };

        NodeSpec entry{};
        entry.id = "entry";
        entry.op = "cmp.gt";
        entry.args = { "mean", "thr" };

        spec.bool_nodes = { entry };
        spec.bind.entry_long = "entry";
        return spec;
    }

    void add_standard_exit_binding(PlanSpec& spec) {
        NodeSpec exit{};
        exit.id = "exit";
        exit.op = "cmp.lt";
        exit.args = { "mean", "thr" };
        spec.bool_nodes.push_back(std::move(exit));
        spec.bind.exit_long = "exit";
    }

    PlanSpec make_multi_entity_runner_contract_plan() {
        PlanSpec spec{};
        spec.version = 1;
        spec.name = "multi_entity_runner_contract";
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec close{};
        close.id = "c";
        close.op = "core.close";

        NodeSpec entry_thr{};
        entry_thr.id = "entry_thr";
        entry_thr.op = "core.const";
        entry_thr.params["value"] = 100.0;

        NodeSpec exit_thr{};
        exit_thr.id = "exit_thr";
        exit_thr.op = "core.const";
        exit_thr.params["value"] = 95.0;

        spec.numeric_nodes = { close, entry_thr, exit_thr };

        NodeSpec entry{};
        entry.id = "entry";
        entry.op = "cmp.gt";
        entry.args = { "c", "entry_thr" };

        NodeSpec exit{};
        exit.id = "exit";
        exit.op = "cmp.lt";
        exit.args = { "c", "exit_thr" };

        spec.bool_nodes = { entry, exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";
        return spec;
    }

    const CapabilityOutcomeSnapshot* find_capability_outcome(
        const RunManifest& manifest,
        std::string_view domain,
        std::string_view subject) {
        for (const auto& outcome : manifest.capability_outcomes) {
            if (outcome.domain == domain && outcome.subject == subject) {
                return &outcome;
            }
        }
        return nullptr;
    }

} // namespace sugar::contract_suite_internal
