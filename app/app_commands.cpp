#include "app_commands.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_reporters.h"
#include "app_runtime.h"

#include "contract_suite.h"
#include "op_registry.h"
#include "panel_runner.h"
#include "strategy_menu.h"
#include "utils.h"

namespace {

    std::optional<std::pair<sugar::TimeKey, sugar::TimeKey>> time_minmax(const sugar::NumericFrame& f) {
        if (!f.has_time_axis()) return std::nullopt;
        const auto t = f.time();
        if (t.empty()) return std::nullopt;
        const auto [mn, mx] = std::minmax_element(t.begin(), t.end());
        return std::make_pair(*mn, *mx);
    }

} // namespace

namespace sugar::app {

    int run_dump_ops_command(std::ostream& out) {
        const auto reg = sugar::make_default_registry();
        out << reg.describe_text(true);
        return 0;
    }

    int run_contract_suite_command(const AppCommand& cmd, std::ostream& out, std::ostream&) {
        const bool ok = sugar::run_contract_suite(out, cmd.contract_suite);
        return ok ? 0 : 1;
    }

    int run_prep_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::DataSpec ds;
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.prep_dump.spec_path, ds, load_err)) {
            err << "Error: failed to load DataSpec: " << load_err << "\n";
            return 1;
        }

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.prep_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_data_spec_summary(ds, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);
        print_column_registry(*fs.schema, out);

        if (!report.produced_frameset) {
            out << "Preprocess output: NumericFrame\n";
            print_frame_summary(fs.by_entity.begin()->second, out, "  ");
            return 0;
        }

        out << "Preprocess output: FrameSet\n";
        out << "  entity_count: " << fs.by_entity.size() << "\n";

        std::vector<std::pair<std::string, std::size_t>> entities;
        entities.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            entities.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(entities.begin(), entities.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t topn = std::min<std::size_t>(10, entities.size());
        out << "  top entities by rows (up to 10):\n";
        for (std::size_t i = 0; i < topn; ++i) {
            const auto& [ekey, rc] = entities[i];
            auto it = fs.by_entity.find(ekey);
            if (it == fs.by_entity.end()) continue;
            out << "    " << ekey << ": rows=" << rc;
            if (rc > 0) {
                if (const auto mm = time_minmax(it->second)) {
                    const auto [mn, mx] = *mm;
                    out << " time=" << mn << ".." << mx;
                }
                else {
                    out << " row_domain=0.." << (rc - 1);
                }
            }
            out << "\n";
        }

        for (const auto& [ekey, frame] : fs.by_entity) {
            out << "  entity: " << ekey << "\n";
            print_frame_summary(frame, out, "    ");
        }

        return 0;
    }

    int run_align_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::DataSpec ds;
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.align_dump.spec_path, ds, load_err)) {
            err << "Error: failed to load DataSpec: " << load_err << "\n";
            return 1;
        }
        ds.entity_col = cmd.align_dump.series_key_col;

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.align_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }
        if (!report.produced_frameset) {
            err << "Error: --align-dump requires multiple series; preprocess did not produce FrameSet\n";
            return 1;
        }
        if (fs.by_entity.size() < 2) {
            err << "Error: need at least 2 series for alignment dump\n";
            return 1;
        }

        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);

        std::vector<std::pair<std::string, std::size_t>> ranked;
        ranked.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            ranked.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t nsel = std::min<std::size_t>(5, ranked.size());
        std::vector<std::pair<std::string, sugar::NumericFrame>> inputs;
        inputs.reserve(nsel);
        for (std::size_t i = 0; i < nsel; ++i) {
            const auto& key = ranked[i].first;
            auto it = fs.by_entity.find(key);
            if (it != fs.by_entity.end()) {
                inputs.emplace_back(it->first, it->second);
            }
        }
        if (inputs.size() < 2) {
            err << "Error: failed to select at least 2 series for alignment dump\n";
            return 1;
        }

        sugar::align::AlignConfig cfg{};
        cfg.calendar = cmd.align_dump.calendar;
        cfg.missing = sugar::align::MissingPolicy::NaN;
        sugar::FrameSet selected{};
        selected.schema = fs.schema;
        selected.index_schema = fs.index_schema;
        for (auto& kv : inputs) {
            selected.by_entity.emplace(kv.first, std::move(kv.second));
        }

        std::shared_ptr<sugar::align::AlignedBundle> bundle{};
        std::string align_err;
        if (!sugar::app::align_frameset(selected, cfg, bundle, align_err)) {
            err << "Error: " << align_err << "\n";
            return 1;
        }

        print_align_dump(*bundle, out);
        return 0;
    }

    int run_panel_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        const sugar::StrategySource source = sugar::prompt_strategy_source(in, out);
        if (source == sugar::StrategySource::CustomPlanSpec) {
            sugar::CustomPlanSpecRunOptions custom_opt{};
            sugar::prompt_custom_user_planspec(in, out, custom_opt);
            if (custom_opt.plan_spec_path.empty()) {
                out << "[Info] Custom strategy cancelled (no PlanSpec JSON provided).\n";
                return 0;
            }

            sugar::PlanSpec spec{};
            sugar::PlanSweepRuntimeOptions rt{};
            bool has_embedded_observations = false;
            std::string spec_err;
            if (!load_custom_planspec_bundle(custom_opt, spec, rt, has_embedded_observations, spec_err)) {
                err << "Error: failed to load PlanSpec runtime: " << spec_err << "\n";
                return 1;
            }

            bool use_embedded_observations = false;
            if (has_embedded_observations) {
                out << "[Info] PlanSpec embeds legacy observation inputs. --run-panel custom execution can use either those "
                    << "embedded observations or the CLI DataSpec/CSV input.\n";
                use_embedded_observations = sugar::prompt_yes_no(
                    in,
                    out,
                    "Use embedded legacy observations instead of --run-panel DataSpec/CSV input?",
                    true);
            }

            if (use_embedded_observations) {
                AlignedRuntimeInput runtime{};
                std::string runtime_err;
                if (!build_embedded_observation_runtime(spec, runtime, runtime_err)) {
                    err << "Error: " << runtime_err << "\n";
                    return 1;
                }
                if (!cmd.run_panel.primary_col.empty()) {
                    out << "[Info] --primary is ignored when using embedded legacy observations.\n";
                }

                out << "[Info] custom panel execution using embedded legacy observations.\n";
                print_observation_capability_report(runtime, out);
                const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
                sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
                print_plan_capability_report(plan_report, out);
                const sugar::DataView panel_dv(*runtime.aligned_bundle);
                const auto pres = sugar::run_panel_custom_planspec_over_dataview(
                    panel_dv,
                    spec,
                    rt,
                    runtime.primary_ref.col,
                    cmd.run_panel.topk);
                print_panel_result(pres, out);
                return 0;
            }

            AlignedRuntimeInput runtime{};
            std::string runtime_err;
            if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
                err << "Error: " << runtime_err << "\n";
                return 1;
            }

            out << "[Info] custom panel execution using --run-panel DataSpec/CSV observations.\n";
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
            print_plan_capability_report(plan_report, out);
            const sugar::DataView panel_dv(*runtime.aligned_bundle);
            const auto pres = sugar::run_panel_custom_planspec_over_dataview(
                panel_dv,
                spec,
                rt,
                runtime.primary_ref.col,
                cmd.run_panel.topk);
            print_panel_result(pres, out);
            return 0;
        }

        const sugar::StrategyTemplate templ = sugar::prompt_strategy_template(in, out);
        const sugar::TradeMode mode = sugar::prompt_trade_mode(in, out, sugar::TradeMode::LongShort);

        sugar::PlanSpec spec{};
        std::string map_err;
        if (!sugar::load_template_planspec(templ, spec, map_err)) {
            err << "Error: template PlanSpec mapping failed: " << map_err << "\n";
            return 1;
        }

        AlignedRuntimeInput runtime{};
        std::string runtime_err;
        if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_observation_capability_report(runtime, out);
        const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
        sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
        print_plan_capability_report(plan_report, out);
        const sugar::DataView panel_dv(*runtime.aligned_bundle);
        sugar::PlanSweepRuntimeOptions rt{};
        rt.override_mode = true;
        rt.custom_mode = mode;
        const auto pres = sugar::run_panel_custom_planspec_over_dataview(
            panel_dv, spec, rt, runtime.primary_ref.col, cmd.run_panel.topk);
        print_panel_result(pres, out);
        return 0;
    }

} // namespace sugar::app
