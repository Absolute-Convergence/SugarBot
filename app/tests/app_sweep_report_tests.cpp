#include "tests/app_test_support.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include "ops/op_registry.h"
#include "reporting/app_reporters.h"
#include "sweep/plan_spec.h"
#include "sweep/plan_sweep.h"

namespace sugar::app::tests {

    namespace {

        bool test_plan_sweep_report_preserves_fractional_param_ranges(TestContext& t) {
            t.out << "[app_reporters] fractional sweep range display\n";

            sugar::PlanSweepReport report{};
            report.requested_sweep.enabled = true;
            report.requested_sweep.engine = sugar::PlanSweepEngineKind::MonteCarlo;
            report.requested_sweep.objective = "pnl";
            report.requested_sweep.top_k = 1;
            report.requested_sweep.max_evals = 3;
            report.resolved_objective.primary.name = "pnl";
            report.parameter_count = 1;
            report.total_combinations = 3;
            report.candidate_space_size = 3;
            report.evaluations_run = 3;

            sugar::SweepParameterRangeSummary threshold{};
            threshold.name = "threshold";
            threshold.requested_range.values = { 0.9, 1.1, 1.3 };
            threshold.requested_range.is_int = false;
            threshold.expanded_count = 3;
            report.parameter_ranges.push_back(std::move(threshold));

            std::ostringstream out;
            sugar::print_plan_sweep_report(report, out);
            const std::string text = out.str();

            t.check(text.find("Engine: monte_carlo") != std::string::npos,
                "sweep report prints the selected engine");
            t.check(text.find("threshold: [0.9, 1.1, 1.3]") != std::string::npos,
                "sweep report preserves fractional requested values");
            t.check(text.find("threshold: [1, 1, 1]") == std::string::npos,
                "sweep report no longer rounds fractional requested values to integers");

            const std::string plan_json =
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"fractional_sweep_parse\",\n"
                "  \"numeric_nodes\": [],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"sweep\": {\n"
                "    \"enabled\": true,\n"
                "    \"params\": {\n"
                "      \"threshold\": { \"values\": [0.9, 1.1, 1.3] },\n"
                "      \"window\": { \"values\": [8, 16, 24] },\n"
                "      \"forced_int\": { \"values\": [0.9, 1.1], \"is_int\": true }\n"
                "    }\n"
                "  }\n"
                "}\n";
            sugar::PlanSpec spec{};
            std::string err;
            const bool parsed = sugar::parse_plan_spec_json(plan_json, spec, err);
            t.check(parsed, "PlanSpec parser accepts fractional sweep fixture");
            t.check(parsed && spec.sweep.has_value() &&
                    !spec.sweep->params.at("threshold").is_int,
                "PlanSpec parser infers float sweep params from decimal values");
            t.check(parsed && spec.sweep.has_value() &&
                    spec.sweep->params.at("window").is_int,
                "PlanSpec parser keeps integer sweep params integer by default");
            t.check(parsed && spec.sweep.has_value() &&
                    spec.sweep->params.at("forced_int").is_int,
                "PlanSpec parser honors explicit integer sweep params");
            return t.failed == 0;
        }

        bool test_plan_sweep_report_derives_qol_warnings(TestContext& t) {
            t.out << "[app_reporters] sweep QoL warning derivation\n";

            sugar::PlanSweepReport report{};
            report.requested_sweep.enabled = true;
            report.requested_sweep.engine = sugar::PlanSweepEngineKind::CoarseFine;
            report.requested_sweep.objective = "pnl";
            report.requested_sweep.top_k = 2;
            report.requested_sweep.max_evals = 10;
            report.resolved_objective.primary.source = sugar::ObjectiveValueSource::ReportedMetric;
            report.resolved_objective.primary.name = "pnl";
            report.resolved_objective.direction = sugar::ObjectiveDirection::Maximize;
            report.parameter_count = 3;
            report.total_combinations = 10;
            report.candidate_space_size = 10;
            report.evaluations_run = 10;

            sugar::SweepParameterRangeSummary breakout{};
            breakout.name = "breakout_n";
            breakout.requested_range.min = 12;
            breakout.requested_range.max = 30;
            breakout.requested_range.step = 6;
            breakout.requested_range.is_int = true;
            breakout.expanded_count = 4;
            report.parameter_ranges.push_back(std::move(breakout));

            sugar::SweepParameterRangeSummary fast{};
            fast.name = "n_fast";
            fast.requested_range.min = 10;
            fast.requested_range.max = 30;
            fast.requested_range.step = 1;
            fast.requested_range.is_int = true;
            fast.expanded_count = 21;
            report.parameter_ranges.push_back(std::move(fast));

            sugar::SweepParameterRangeSummary slow{};
            slow.name = "n_slow";
            slow.requested_range.min = 20;
            slow.requested_range.max = 40;
            slow.requested_range.step = 1;
            slow.requested_range.is_int = true;
            slow.expanded_count = 21;
            report.parameter_ranges.push_back(std::move(slow));

            for (std::size_t i = 0; i < 2; ++i) {
                sugar::SweepRankedCandidate candidate{};
                candidate.candidate_index = i;
                candidate.score = 10.0 - static_cast<double>(i);
                candidate.result.pnl = candidate.score;
                candidate.parameter_values = {
                    sugar::SweepParameterSetting{ "breakout_n", static_cast<std::int64_t>(12) },
                    sugar::SweepParameterSetting{ "n_fast", static_cast<std::int64_t>(27) },
                    sugar::SweepParameterSetting{ "n_slow", static_cast<std::int64_t>(20) },
                };
                report.top_candidates.push_back(std::move(candidate));
            }

            report.warnings = sugar::derive_plan_sweep_report_warnings(report);

            std::ostringstream out;
            sugar::print_plan_sweep_report(report, out);
            const std::string text = out.str();

            t.check(report.warnings.size() >= 2,
                "sweep report derives boundary and semantic warning text");
            t.check(text.find("sweep boundary concentration") != std::string::npos &&
                    text.find("breakout_n=12") != std::string::npos,
                "sweep report annotates returned candidates concentrated at a sweep boundary");
            t.check(text.find("semantic fast/slow warning") != std::string::npos &&
                    text.find("n_fast > n_slow") != std::string::npos,
                "sweep report annotates likely fast/slow parameter inversions");
            return t.failed == 0;
        }

        bool test_param_string_literal_error_hints_ref_syntax(TestContext& t) {
            t.out << "[op_registry] param string literal ref-syntax hint\n";

            const auto registry = sugar::make_default_registry();
            const auto* op = registry.get("ts.sma");
            t.check(op != nullptr, "ts.sma registry entry is available");
            if (op == nullptr) return false;

            sugar::ParamMap node_params{};
            node_params["n"] = std::string("n_fast");
            sugar::ParamMap plan_params{};
            plan_params["n_fast"] = static_cast<std::int64_t>(5);
            sugar::ResolvedParamMap resolved{};
            std::string err;
            const bool ok = sugar::resolve_and_validate_params(
                *op,
                node_params,
                plan_params,
                nullptr,
                resolved,
                err);

            t.check(!ok, "string literal numeric param is rejected");
            t.check(err.find("Use {\"ref\":\"n_fast\"}") != std::string::npos,
                "wrong-type error points at missing param-ref syntax");
            return t.failed == 0;
        }

    } // namespace

    void register_sweep_report_tests(AppTestRegistry& tests) {
        tests.push_back(test_plan_sweep_report_preserves_fractional_param_ranges);
        tests.push_back(test_plan_sweep_report_derives_qol_warnings);
        tests.push_back(test_param_string_literal_error_hints_ref_syntax);
    }

} // namespace sugar::app::tests
