#include "app_support/feature_dump.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include "engine/plan_compiler.h"
#include "engine/program_evaluator_internal.h"
#include "engine/plan_compiler_internal.h"
#include "ops/op_registry.h"
#include "ingest/data_view.h"

namespace sugar::app {

    namespace {

        std::string format_numeric_cell(double value) {
            if (!std::isfinite(value)) {
                return "nan";
            }
            std::ostringstream oss;
            oss << std::setprecision(17) << value;
            return oss.str();
        }

    } // namespace

    bool dump_feature_table(
        const sugar::ResolvedRuntimeInput& runtime,
        const sugar::PlanSpec& spec,
        std::ostream& out,
        std::string& err) {
        if (!runtime.aligned_bundle) {
            err = "feature-dump requires an aligned runtime bundle";
            return false;
        }
        if (runtime.primary_series_index == static_cast<std::size_t>(-1) ||
            runtime.primary_series_index >= runtime.aligned_bundle->frames.size()) {
            err = "feature-dump primary series index is invalid";
            return false;
        }

        const auto registry = sugar::make_default_registry();
        sugar::CompiledPlanTemplate plan_template{};
        std::vector<sugar::NamedNumericOutput> numeric_outputs;
        std::string compile_err;
        if (!sugar::compile_feature_dump_template(
                spec,
                registry,
                sugar::CompileEnv{},
                plan_template,
                numeric_outputs,
                compile_err)) {
            err = "feature-dump compile failed: " + compile_err;
            return false;
        }

        sugar::CompileResult compiled{};
        std::string instantiate_err;
        if (!sugar::instantiate_compiled_plan(plan_template, nullptr, compiled, instantiate_err)) {
            err = "feature-dump instantiate failed: " + instantiate_err;
            return false;
        }

        const sugar::DataView dv(*runtime.aligned_bundle);
        sugar::ProgramEvaluationBuffers buffers{};
        std::string eval_err;
        if (!sugar::evaluate_strategy_plan_program_buffers(
                dv,
                runtime.primary_ref,
                compiled.plan,
                buffers,
                eval_err)) {
            err = "feature-dump evaluation failed: " + eval_err;
            return false;
        }

        const auto& frame = runtime.aligned_bundle->frames[runtime.primary_series_index];
        const bool has_time_axis = frame.has_time_axis();
        const auto time_axis = frame.time();
        const std::string entity_key =
            runtime.primary_entity_key.empty() ? runtime.primary_ref.series : runtime.primary_entity_key;

        out << "series_key,entity_key,row_index";
        if (has_time_axis) {
            out << ",time_key";
        }
        for (const auto& feature : numeric_outputs) {
            out << "," << feature.node_id;
        }
        out << "\n";

        for (std::size_t row = 0; row < frame.row_count(); ++row) {
            out << runtime.primary_ref.series
                << "," << entity_key
                << "," << row;
            if (has_time_axis) {
                out << "," << time_axis[row];
            }
            for (const auto& feature : numeric_outputs) {
                if (feature.output_slot >= buffers.numeric_slots.size() ||
                    row >= buffers.numeric_slots[feature.output_slot].size()) {
                    err = "feature-dump numeric output slot is out of range";
                    return false;
                }
                out << "," << format_numeric_cell(buffers.numeric_slots[feature.output_slot][row]);
            }
            out << "\n";
        }

        return true;
    }

} // namespace sugar::app
