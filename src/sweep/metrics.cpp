#include "sweep/metrics.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include "ingest/run_manifest.h"

namespace sugar {

    namespace {
        std::string_view time_kind_name(TimeKind kind) {
            switch (kind) {
            case TimeKind::YyyyMmDd:
                return "YyyyMmDd";
            case TimeKind::Iso8601:
                return "Iso8601";
            case TimeKind::YearQuarter:
                return "YearQuarter";
            case TimeKind::YearMonth:
                return "YearMonth";
            case TimeKind::Index:
                return "Index";
            }
            return "Unknown";
        }

        std::string_view alignment_calendar_name(align::CalendarPolicy policy) {
            switch (policy) {
            case align::CalendarPolicy::Intersection:
                return "Intersection";
            case align::CalendarPolicy::Union:
                return "Union";
            }
            return "Unknown";
        }

        std::string_view alignment_missing_name(align::MissingPolicy policy) {
            switch (policy) {
            case align::MissingPolicy::NaN:
                return "NaN";
            }
            return "Unknown";
        }

        bool run_provenance_has_data(const RunProvenanceSummary& provenance) {
            return !provenance.primary_series_key.empty() ||
                !provenance.primary_selection_mode.empty() ||
                !provenance.time_kind.empty() ||
                !provenance.time_resolution.empty() ||
                provenance.series_count > 0 ||
                provenance.has_time_axis ||
                provenance.has_entity_axis ||
                provenance.has_spatial_axis ||
                provenance.input_warning_count > 0 ||
                provenance.csv_had_header ||
                provenance.csv_physical_line_count > 0 ||
                provenance.csv_blank_line_count > 0 ||
                provenance.csv_data_row_count > 0 ||
                provenance.dropped_row_count > 0 ||
                !provenance.input_warnings.empty() ||
                provenance.alignment_applied ||
                !provenance.topology_names.empty() ||
                provenance.topology_usage_count > 0;
        }

        bool metric_prefers_integral_display(const ResultMetricValue& metric) {
            if (metric.has_exact_u64_value) {
                return true;
            }
            if (metric.kind == ResultMetricKind::SignalCount) {
                return true;
            }
            return metric.name == "trades" ||
                metric.name == "long_trades" ||
                metric.name == "short_trades" ||
                metric.name == "rows_evaluated" ||
                metric.name == "warmup_rows" ||
                metric.name == "first_usable_signal_time_key";
        }

        std::string format_metric_value(const ResultMetricValue& metric) {
            if (!metric.is_finite) {
                return "NaN";
            }

            if (metric.has_exact_u64_value) {
                return std::to_string(metric.exact_u64_value);
            }

            std::ostringstream oss;
            if (metric_prefers_integral_display(metric)) {
                oss << std::fixed << std::setprecision(0) << metric.value;
                return oss.str();
            }

            oss << std::setprecision(12) << metric.value;
            return oss.str();
        }

        void append_metric(
            BacktestMetricCatalog& catalog,
            std::string name,
            double value,
            ResultMetricKind kind)
        {
            const bool is_finite = std::isfinite(value);
            if (!is_finite) {
                catalog.non_finite_metric_names.push_back(name);
            }
            catalog.values.push_back(ResultMetricValue{
                std::move(name),
                value,
                kind,
                is_finite,
                false,
                0
            });
        }

        void append_metric(
            BacktestMetricCatalog& catalog,
            std::string name,
            std::uint64_t value,
            ResultMetricKind kind)
        {
            catalog.values.push_back(ResultMetricValue{
                std::move(name),
                static_cast<double>(value),
                kind,
                true,
                true,
                value
            });
        }

        void append_metrics_from_backtest(
            const BacktestResult& result,
            BacktestMetricCatalog& catalog)
        {
            append_metric(catalog, "pnl", result.pnl, ResultMetricKind::CoreMetric);
            append_metric(catalog, "trades", static_cast<std::uint64_t>(result.trades), ResultMetricKind::CoreMetric);
            append_metric(catalog, "max_drawdown", result.max_drawdown, ResultMetricKind::CoreMetric);
            append_metric(catalog, "long_trades", static_cast<std::uint64_t>(result.long_trades), ResultMetricKind::CoreMetric);
            append_metric(catalog, "short_trades", static_cast<std::uint64_t>(result.short_trades), ResultMetricKind::CoreMetric);
            append_metric(catalog, "pnl_long", result.pnl_long, ResultMetricKind::CoreMetric);
            append_metric(catalog, "pnl_short", result.pnl_short, ResultMetricKind::CoreMetric);
            append_metric(catalog, "trade_hash", result.trade_hash, ResultMetricKind::CoreMetric);
            append_metric(catalog, "rows_evaluated", static_cast<std::uint64_t>(result.rows_evaluated), ResultMetricKind::CoreMetric);
            append_metric(catalog, "warmup_rows", static_cast<std::uint64_t>(result.warmup_rows), ResultMetricKind::CoreMetric);
            if (result.has_best_start_date) {
                append_metric(
                    catalog,
                    "first_usable_signal_time_key",
                    static_cast<std::uint64_t>(result.best_start_date),
                    ResultMetricKind::CoreMetric);
            }

            append_metric(catalog, "allow_trade_signals", static_cast<std::uint64_t>(result.signal_counts.allow_trade), ResultMetricKind::SignalCount);
            append_metric(catalog, "entry_long_signals", static_cast<std::uint64_t>(result.signal_counts.entry_long), ResultMetricKind::SignalCount);
            append_metric(catalog, "exit_long_signals", static_cast<std::uint64_t>(result.signal_counts.exit_long), ResultMetricKind::SignalCount);
            append_metric(catalog, "entry_short_signals", static_cast<std::uint64_t>(result.signal_counts.entry_short), ResultMetricKind::SignalCount);
            append_metric(catalog, "exit_short_signals", static_cast<std::uint64_t>(result.signal_counts.exit_short), ResultMetricKind::SignalCount);

            for (const auto& value : result.reported_values) {
                append_metric(catalog, value.name, value.value, ResultMetricKind::ReportedValue);
            }
        }

        bool try_lookup_backtest_metric_value(
            const BacktestResult& result,
            std::string_view name,
            ResultMetricValue& out)
        {
            auto assign = [&](std::string metric_name, double value, ResultMetricKind kind) {
                out = ResultMetricValue{
                    std::move(metric_name),
                    value,
                    kind,
                    std::isfinite(value),
                    false,
                    0
                };
                return true;
            };
            auto assign_u64 = [&](std::string metric_name, std::uint64_t value, ResultMetricKind kind) {
                out = ResultMetricValue{
                    std::move(metric_name),
                    static_cast<double>(value),
                    kind,
                    true,
                    true,
                    value
                };
                return true;
            };

            if (name == "pnl") return assign("pnl", result.pnl, ResultMetricKind::CoreMetric);
            if (name == "trades") return assign_u64("trades", static_cast<std::uint64_t>(result.trades), ResultMetricKind::CoreMetric);
            if (name == "max_drawdown") return assign("max_drawdown", result.max_drawdown, ResultMetricKind::CoreMetric);
            if (name == "long_trades") return assign_u64("long_trades", static_cast<std::uint64_t>(result.long_trades), ResultMetricKind::CoreMetric);
            if (name == "short_trades") return assign_u64("short_trades", static_cast<std::uint64_t>(result.short_trades), ResultMetricKind::CoreMetric);
            if (name == "pnl_long") return assign("pnl_long", result.pnl_long, ResultMetricKind::CoreMetric);
            if (name == "pnl_short") return assign("pnl_short", result.pnl_short, ResultMetricKind::CoreMetric);
            if (name == "trade_hash") return assign_u64("trade_hash", result.trade_hash, ResultMetricKind::CoreMetric);
            if (name == "rows_evaluated") return assign_u64("rows_evaluated", static_cast<std::uint64_t>(result.rows_evaluated), ResultMetricKind::CoreMetric);
            if (name == "warmup_rows") return assign_u64("warmup_rows", static_cast<std::uint64_t>(result.warmup_rows), ResultMetricKind::CoreMetric);
            if (name == "first_usable_signal_time_key" && result.has_best_start_date) {
                return assign_u64(
                    "first_usable_signal_time_key",
                    static_cast<std::uint64_t>(result.best_start_date),
                    ResultMetricKind::CoreMetric);
            }

            if (name == "allow_trade_signals") return assign_u64("allow_trade_signals", static_cast<std::uint64_t>(result.signal_counts.allow_trade), ResultMetricKind::SignalCount);
            if (name == "entry_long_signals") return assign_u64("entry_long_signals", static_cast<std::uint64_t>(result.signal_counts.entry_long), ResultMetricKind::SignalCount);
            if (name == "exit_long_signals") return assign_u64("exit_long_signals", static_cast<std::uint64_t>(result.signal_counts.exit_long), ResultMetricKind::SignalCount);
            if (name == "entry_short_signals") return assign_u64("entry_short_signals", static_cast<std::uint64_t>(result.signal_counts.entry_short), ResultMetricKind::SignalCount);
            if (name == "exit_short_signals") return assign_u64("exit_short_signals", static_cast<std::uint64_t>(result.signal_counts.exit_short), ResultMetricKind::SignalCount);

            const auto it = std::find_if(
                result.reported_values.begin(),
                result.reported_values.end(),
                [name](const NamedReportedValue& value) {
                    return value.name == name;
                });
            if (it == result.reported_values.end()) {
                return false;
            }

            return assign(it->name, it->value, ResultMetricKind::ReportedValue);
        }

        RunObjectiveSummary build_run_objective_summary(
            const BacktestResult& result,
            const RankingObjectiveSpec& objective)
        {
            RunObjectiveSummary summary{};
            summary.requested = true;
            summary.objective = objective;
            summary.score = -std::numeric_limits<double>::infinity();

            double primary_value = std::numeric_limits<double>::quiet_NaN();
            if (!try_lookup_reported_value(result, objective.primary, primary_value)) {
                summary.rankability = RunRankabilityState::MissingPrimaryValue;
                return summary;
            }
            if (!std::isfinite(primary_value)) {
                summary.rankability = RunRankabilityState::NonFinitePrimaryValue;
                return summary;
            }

            for (const auto& penalty : objective.penalties) {
                double penalty_value = std::numeric_limits<double>::quiet_NaN();
                if (!try_lookup_reported_value(result, penalty.value, penalty_value)) {
                    summary.rankability = RunRankabilityState::MissingPenaltyValue;
                    return summary;
                }
                if (!std::isfinite(penalty_value)) {
                    summary.rankability = RunRankabilityState::NonFinitePenaltyValue;
                    return summary;
                }
            }

            summary.rankability = RunRankabilityState::Rankable;
            summary.score = evaluate_ranking_objective(objective, result);
            return summary;
        }

        const ResultMetricValue* find_metric_value(
            const BacktestMetricCatalog& catalog,
            std::string_view name)
        {
            const auto it = std::find_if(
                catalog.values.begin(),
                catalog.values.end(),
                [name](const ResultMetricValue& value) {
                    return value.name == name;
                });
            if (it == catalog.values.end()) {
                return nullptr;
            }
            return &(*it);
        }
    } // namespace

    std::string_view objective_value_source_name(ObjectiveValueSource source) {
        switch (source) {
        case ObjectiveValueSource::ReportedMetric:
            return "reported_metric";
        case ObjectiveValueSource::NumericNodeOutput:
            return "numeric_node_output";
        }
        return "unknown";
    }

    std::string_view objective_direction_name(ObjectiveDirection direction) {
        switch (direction) {
        case ObjectiveDirection::Maximize:
            return "maximize";
        case ObjectiveDirection::Minimize:
            return "minimize";
        }
        return "unknown";
    }

    std::string_view numeric_node_summary_mode_name(NumericNodeSummaryMode mode) {
        switch (mode) {
        case NumericNodeSummaryMode::LastFiniteValue:
            return "last_finite";
        }
        return "unknown";
    }

    std::string_view result_metric_kind_name(ResultMetricKind kind) {
        switch (kind) {
        case ResultMetricKind::CoreMetric:
            return "core_metric";
        case ResultMetricKind::SignalCount:
            return "signal_count";
        case ResultMetricKind::ReportedValue:
            return "reported_value";
        }
        return "unknown";
    }

    std::string_view run_outcome_state_name(RunOutcomeState state) {
        switch (state) {
        case RunOutcomeState::Completed:
            return "completed";
        case RunOutcomeState::CompletedWithNonFiniteMetrics:
            return "completed_with_non_finite_metrics";
        case RunOutcomeState::Failed:
            return "failed";
        case RunOutcomeState::Canceled:
            return "canceled";
        }
        return "unknown";
    }

    std::string_view run_rankability_state_name(RunRankabilityState state) {
        switch (state) {
        case RunRankabilityState::NotEvaluated:
            return "not_evaluated";
        case RunRankabilityState::Rankable:
            return "rankable";
        case RunRankabilityState::MissingPrimaryValue:
            return "missing_primary_value";
        case RunRankabilityState::NonFinitePrimaryValue:
            return "non_finite_primary_value";
        case RunRankabilityState::MissingPenaltyValue:
            return "missing_penalty_value";
        case RunRankabilityState::NonFinitePenaltyValue:
            return "non_finite_penalty_value";
        }
        return "unknown";
    }

    RunProvenanceSummary build_run_provenance_summary(const RunManifest& manifest) {
        RunProvenanceSummary summary{};
        summary.primary_series_key = manifest.primary_series_key;
        summary.primary_selection_mode = std::string(
            runtime_primary_selection_mode_name(manifest.primary_selection_mode));
        summary.has_time_axis = manifest.preprocess.has_time_axis;
        summary.has_entity_axis = manifest.preprocess.has_entity_axis;
        summary.has_spatial_axis = manifest.preprocess.has_spatial_axis;
        if (manifest.preprocess.has_time_axis) {
            summary.time_kind = std::string(time_kind_name(manifest.preprocess.time_kind));
            summary.time_resolution = manifest.preprocess.time_resolution;
        }
        summary.series_count = manifest.preprocess.series_keys.size();
        summary.input_warning_count = manifest.preprocess.input_warnings.size();
        summary.csv_had_header = manifest.preprocess.csv_had_header;
        summary.csv_physical_line_count = manifest.preprocess.csv_physical_line_count;
        summary.csv_blank_line_count = manifest.preprocess.csv_blank_line_count;
        summary.csv_data_row_count = manifest.preprocess.csv_data_row_count;
        summary.dropped_row_count = manifest.preprocess.dropped_row_count;
        summary.input_warnings = manifest.preprocess.input_warnings;
        summary.alignment_applied = manifest.preprocess.alignment_applied;
        if (summary.alignment_applied) {
            summary.alignment_calendar = std::string(
                alignment_calendar_name(manifest.preprocess.alignment_calendar));
            summary.alignment_missing = std::string(
                alignment_missing_name(manifest.preprocess.alignment_missing));
            summary.alignment_non_temporal_multi_series = std::string(
                align::non_temporal_multi_series_policy_name(
                    manifest.preprocess.alignment_non_temporal_multi_series));
        }
        summary.topology_usage_count = manifest.topology_usages.size();
        summary.topology_names.reserve(manifest.topologies.size());
        for (const auto& topology : manifest.topologies) {
            if (!topology.name.empty()) {
                summary.topology_names.push_back(topology.name);
            }
        }
        return summary;
    }

    RunReport build_run_report(
        const BacktestResult& result,
        const RankingObjectiveSpec* objective,
        const RunProvenanceSummary* provenance,
        RunOutcomeState outcome)
    {
        RunReport report{};
        report.execution = result;
        report.outcome = outcome;
        append_metrics_from_backtest(result, report.metric_catalog);
        if (report.outcome == RunOutcomeState::Completed &&
            !report.metric_catalog.non_finite_metric_names.empty()) {
            report.outcome = RunOutcomeState::CompletedWithNonFiniteMetrics;
        }
        if (objective != nullptr) {
            report.objective = build_run_objective_summary(result, *objective);
        }
        if (provenance != nullptr) {
            report.provenance = *provenance;
        }
        return report;
    }

    const ResultMetricValue* find_metric_value(
        const RunReport& report,
        std::string_view name)
    {
        return find_metric_value(report.metric_catalog, name);
    }

    bool try_lookup_reported_value(
        const BacktestResult& result,
        const ObjectiveValueSpec& value,
        double& out) {
        if (value.name.empty()) {
            return false;
        }

        ResultMetricValue metric{};
        if (!try_lookup_backtest_metric_value(result, value.name, metric)) {
            return false;
        }

        if (value.source == ObjectiveValueSource::NumericNodeOutput &&
            metric.kind != ResultMetricKind::ReportedValue) {
            return false;
        }

        out = metric.value;
        return true;
    }

    bool ranking_objective_prefers_left(
        const RankingObjectiveSpec& objective,
        const BacktestResult& left,
        const BacktestResult& right) {
        const double left_score = evaluate_ranking_objective(objective, left);
        const double right_score = evaluate_ranking_objective(objective, right);
        if (left_score != right_score) {
            return left_score > right_score;
        }

        for (const auto& tie_break : objective.tie_breaks) {
            double left_value = std::numeric_limits<double>::quiet_NaN();
            double right_value = std::numeric_limits<double>::quiet_NaN();
            const bool left_ok = try_lookup_reported_value(left, tie_break.value, left_value);
            const bool right_ok = try_lookup_reported_value(right, tie_break.value, right_value);

            const double normalized_left =
                (left_ok && std::isfinite(left_value))
                ? ((tie_break.direction == ObjectiveDirection::Minimize) ? -left_value : left_value)
                : -std::numeric_limits<double>::infinity();
            const double normalized_right =
                (right_ok && std::isfinite(right_value))
                ? ((tie_break.direction == ObjectiveDirection::Minimize) ? -right_value : right_value)
                : -std::numeric_limits<double>::infinity();

            if (normalized_left != normalized_right) {
                return normalized_left > normalized_right;
            }
        }

        return false;
    }

    double evaluate_ranking_objective(
        const RankingObjectiveSpec& objective,
        const BacktestResult& result) {
        double score = std::numeric_limits<double>::quiet_NaN();
        if (!try_lookup_reported_value(result, objective.primary, score) || !std::isfinite(score)) {
            return -std::numeric_limits<double>::infinity();
        }

        if (objective.direction == ObjectiveDirection::Minimize) {
            score = -score;
        }

        for (const auto& penalty : objective.penalties) {
            double penalty_value = std::numeric_limits<double>::quiet_NaN();
            if (!try_lookup_reported_value(result, penalty.value, penalty_value) || !std::isfinite(penalty_value)) {
                return -std::numeric_limits<double>::infinity();
            }
            score -= penalty.weight * penalty_value;
        }

        return score;
    }

    std::string describe_ranking_objective(const RankingObjectiveSpec& objective) {
        std::ostringstream out;
        out << objective_direction_name(objective.direction)
            << " "
            << objective_value_source_name(objective.primary.source)
            << ":"
            << objective.primary.name;

        if (objective.primary.source == ObjectiveValueSource::NumericNodeOutput) {
            out << " [" << numeric_node_summary_mode_name(objective.primary.summary_mode) << "]";
        }

        for (const auto& penalty : objective.penalties) {
            out << " - " << penalty.weight
                << " * "
                << objective_value_source_name(penalty.value.source)
                << ":" << penalty.value.name;
        }

        if (!objective.tie_breaks.empty()) {
            out << " | tie_breaks=";
            for (std::size_t i = 0; i < objective.tie_breaks.size(); ++i) {
                if (i != 0) out << ", ";
                out << objective_direction_name(objective.tie_breaks[i].direction)
                    << " "
                    << objective_value_source_name(objective.tie_breaks[i].value.source)
                    << ":" << objective.tie_breaks[i].value.name;
            }
        }

        return out.str();
    }

    void increment_run_outcome_counts(
        RunOutcomeCounts& counts,
        RunOutcomeState state) {
        switch (state) {
        case RunOutcomeState::Completed:
            ++counts.completed;
            return;
        case RunOutcomeState::CompletedWithNonFiniteMetrics:
            ++counts.completed_with_non_finite_metrics;
            return;
        case RunOutcomeState::Failed:
            ++counts.failed;
            return;
        case RunOutcomeState::Canceled:
            ++counts.canceled;
            return;
        }
    }

    void increment_run_rankability_counts(
        RunRankabilityCounts& counts,
        RunRankabilityState state) {
        switch (state) {
        case RunRankabilityState::NotEvaluated:
            ++counts.not_evaluated;
            return;
        case RunRankabilityState::Rankable:
            ++counts.rankable;
            return;
        case RunRankabilityState::MissingPrimaryValue:
            ++counts.missing_primary_value;
            return;
        case RunRankabilityState::NonFinitePrimaryValue:
            ++counts.non_finite_primary_value;
            return;
        case RunRankabilityState::MissingPenaltyValue:
            ++counts.missing_penalty_value;
            return;
        case RunRankabilityState::NonFinitePenaltyValue:
            ++counts.non_finite_penalty_value;
            return;
        }
    }

    void print_run_provenance_summary(
        const RunProvenanceSummary& provenance,
        std::ostream& os,
        std::string_view indent) {
        if (!run_provenance_has_data(provenance)) {
            return;
        }

        const bool panel_scope =
            provenance.primary_series_key.empty() &&
            provenance.has_entity_axis &&
            provenance.series_count > 0;

        os << indent << "Run provenance:\n"
            << indent << "  primary_series: "
            << (provenance.primary_series_key.empty()
                    ? (panel_scope ? "<panel scope>" : "<none>")
                    : provenance.primary_series_key)
            << "\n"
            << indent << "  primary_selection_mode: "
            << (provenance.primary_selection_mode.empty() ? "<unknown>" : provenance.primary_selection_mode) << "\n"
            << indent << "  series_count: " << provenance.series_count << "\n"
            << indent << "  input_warnings: " << provenance.input_warning_count << "\n"
            << indent << "  csv_had_header: " << (provenance.csv_had_header ? "true" : "false") << "\n"
            << indent << "  csv_physical_lines: " << provenance.csv_physical_line_count << "\n"
            << indent << "  csv_blank_lines: " << provenance.csv_blank_line_count << "\n"
            << indent << "  csv_data_rows: " << provenance.csv_data_row_count << "\n"
            << indent << "  dropped_rows: " << provenance.dropped_row_count << "\n"
            << indent << "  has_time_axis: " << (provenance.has_time_axis ? "true" : "false") << "\n"
            << indent << "  has_entity_axis: " << (provenance.has_entity_axis ? "true" : "false") << "\n"
            << indent << "  has_spatial_axis: " << (provenance.has_spatial_axis ? "true" : "false") << "\n";

        for (const auto& warning : provenance.input_warnings) {
            os << indent << "  input_warning: " << warning << "\n";
        }

        if (provenance.has_time_axis) {
            os << indent << "  time_kind: "
                << (provenance.time_kind.empty() ? "<unknown>" : provenance.time_kind) << "\n"
                << indent << "  time_resolution: "
                << (provenance.time_resolution.empty() ? "<unknown>" : provenance.time_resolution) << "\n";
        }

        if (provenance.alignment_applied) {
            os << indent << "  alignment_calendar: "
                << (provenance.alignment_calendar.empty() ? "<unknown>" : provenance.alignment_calendar) << "\n"
                << indent << "  alignment_missing: "
                << (provenance.alignment_missing.empty() ? "<unknown>" : provenance.alignment_missing) << "\n"
                << indent << "  alignment_non_temporal_multi_series: "
                << (provenance.alignment_non_temporal_multi_series.empty()
                    ? "<unknown>"
                    : provenance.alignment_non_temporal_multi_series) << "\n";
        }

        os << indent << "  topology_usage_count: " << provenance.topology_usage_count << "\n"
            << indent << "  topologies: ";
        if (provenance.topology_names.empty()) {
            os << "<none>\n";
            return;
        }
        for (std::size_t i = 0; i < provenance.topology_names.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << provenance.topology_names[i];
        }
        os << "\n";
    }

    void print_run_report_summary(
        const RunReport& report,
        std::ostream& os,
        std::string_view indent)
    {
        const BacktestResult& r = report.execution;
        os << indent << "PnL: " << r.pnl << "%\n"
            << indent << "Trades: " << r.trades << "\n"
            << indent << "Max DD: " << r.max_drawdown << "%\n"
            << indent << "Trade hash: " << r.trade_hash << "\n";

        if (r.has_best_start_date) {
            os << indent
                << "First usable signal time_key: "
                << r.best_start_date << "\n";
        }

        os << indent << "Long trades: " << r.long_trades
            << ", Long PnL: " << r.pnl_long << "%\n"
            << indent << "Short trades: " << r.short_trades
            << ", Short PnL: " << r.pnl_short << "%\n"
            << indent << "Rows evaluated: " << r.rows_evaluated
            << ", Warmup rows: " << r.warmup_rows << "\n"
            << indent << "Signal counts:"
            << " allow_trade=" << r.signal_counts.allow_trade
            << " entry_long=" << r.signal_counts.entry_long
            << " exit_long=" << r.signal_counts.exit_long
            << " entry_short=" << r.signal_counts.entry_short
            << " exit_short=" << r.signal_counts.exit_short << "\n";

        print_run_provenance_summary(report.provenance, os, indent);

        if (report.objective.requested) {
            os << indent << "Objective: "
                << describe_ranking_objective(report.objective.objective) << "\n"
                << indent << "Objective rankability: "
                << run_rankability_state_name(report.objective.rankability) << "\n";
            if (report.objective.rankability == RunRankabilityState::Rankable) {
                os << indent << "Objective score: " << report.objective.score << "\n";
            }
        }

        if (report.outcome != RunOutcomeState::Completed) {
            os << indent << "Run outcome: "
                << run_outcome_state_name(report.outcome) << "\n";
        }

        if (!report.metric_catalog.values.empty()) {
            os << indent << "Metric catalog:\n";
            for (const auto& metric : report.metric_catalog.values) {
                os << indent << "  " << metric.name
                    << " [" << result_metric_kind_name(metric.kind) << "]: "
                    << format_metric_value(metric) << "\n";
            }
        }

        if (!report.metric_catalog.non_finite_metric_names.empty()) {
            os << indent << "Non-finite metrics:\n";
            for (const auto& name : report.metric_catalog.non_finite_metric_names) {
                os << indent << "  " << name << "\n";
            }
        }
    }

    void print_backtest_summary(
        const BacktestResult& r,
        std::ostream& os,
        std::string_view indent)
    {
        print_run_report_summary(build_run_report(r), os, indent);
    }

} // namespace sugar
