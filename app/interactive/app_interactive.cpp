#include "interactive/app_interactive.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "commands/app_commands.h"
#include "ingest/utils.h"

namespace {

    namespace fs = std::filesystem;

    enum class InteractiveWorkflow {
        Canceled = 0,
        FirstRunTutorial = 1,
        InspectTutorial = 2,
        DirectArtifactRun = 3,
        SweepEngineTutorial = 4,
        DynamicTopologyTutorial = 5,
        AdvancedReportsTutorial = 6,
    };

    struct TutorialArtifacts {
        std::string layout_path{};
        std::string logic_path{};
        std::string csv_path{};
    };

    fs::path find_source_root_from(fs::path current) {
        while (!current.empty()) {
            if (fs::exists(current / "CMakeLists.txt")) {
                return current;
            }
            const fs::path parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
        return {};
    }

    fs::path source_root() {
#ifdef SUGARBOT_SOURCE_DIR
        const fs::path configured = fs::path(SUGARBOT_SOURCE_DIR);
        if (!configured.empty() && fs::exists(configured / "CMakeLists.txt")) {
            return configured;
        }
#endif
        if (const fs::path cwd_root = find_source_root_from(fs::current_path()); !cwd_root.empty()) {
            return cwd_root;
        }
        return find_source_root_from(fs::path(__FILE__).parent_path());
    }

    std::string tutorial_file(std::string_view relative_path) {
        const fs::path root = source_root();
        const fs::path path = root.empty()
            ? fs::path(std::string(relative_path))
            : root / fs::path(std::string(relative_path));
        return sugar::normalize_path_for_runtime(path.lexically_normal().generic_string());
    }

    TutorialArtifacts quickstart_artifacts() {
        return {
            tutorial_file("graphs/demos/quickstart_ohlcv.layout.json"),
            tutorial_file("graphs/demos/quickstart_ohlcv.planspec.json"),
            tutorial_file("data/sample.csv"),
        };
    }

    TutorialArtifacts quickstart_sweep_artifacts(std::string_view planspec_file) {
        return {
            tutorial_file("graphs/demos/quickstart_ohlcv.layout.json"),
            tutorial_file("graphs/demos/sweep_engines/" + std::string(planspec_file)),
            tutorial_file("data/sample.csv"),
        };
    }

    TutorialArtifacts row_dynamic_artifacts() {
        return {
            tutorial_file("graphs/demos/quickstart_ohlcv.layout.json"),
            tutorial_file("graphs/demos/dynamic_execution/g10_row_varying_declared_dynamic.planspec.json"),
            tutorial_file("data/sample.csv"),
        };
    }

    TutorialArtifacts topology_dynamic_artifacts() {
        return {
            tutorial_file("graphs/demos/topology_readiness/f8_relational_time_graph.layout.json"),
            tutorial_file("graphs/demos/dynamic_execution/g10_topology_declared_dynamic.planspec.json"),
            tutorial_file("data/topology_readiness/f8_relational_time_graph.csv"),
        };
    }

    TutorialArtifacts advanced_report_artifacts() {
        return quickstart_sweep_artifacts("i_advanced_report_projection_sweep.planspec.json");
    }

    bool is_existing_file_path(const std::string& path) {
        std::error_code ec;
        const fs::path p(path);
        if (!fs::exists(p, ec) || ec) {
            return false;
        }
        ec.clear();
        return fs::is_regular_file(p, ec) && !ec;
    }

    bool tutorial_artifacts_exist(
        const TutorialArtifacts& artifacts,
        std::ostream& err) {
        bool ok = true;
        auto check = [&](std::string_view label, const std::string& path) {
            if (!is_existing_file_path(path)) {
                err << "Error: tutorial " << label << " file is missing: " << path << "\n";
                ok = false;
            }
        };
        check("layout", artifacts.layout_path);
        check("PlanSpec", artifacts.logic_path);
        check("CSV", artifacts.csv_path);
        return ok;
    }

    void print_missing_file_prompt(std::ostream& out, const std::string& path) {
        out << "[Info] File not found: " << path
            << ". Enter an existing file path or q to cancel.\n";
    }

    bool parse_menu_choice(const std::string& line, int min_choice, int max_choice, int& out_choice)
    {
        std::istringstream iss(line);
        int choice = 0;
        if (!(iss >> choice)) {
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            return false;
        }
        if (choice < min_choice || choice > max_choice) {
            return false;
        }
        out_choice = choice;
        return true;
    }

    InteractiveWorkflow prompt_interactive_workflow(std::istream& in, std::ostream& out) {
        for (;;) {
            out << "\nChoose SugarBot tutorial:\n";
            out << "  new-user path: 1) first real run, 2) inspect before running, 3) run your own artifacts\n";
            out << "  next chapters: 4) sweep engines, 5) dynamic execution + topology, 6) advanced reports\n";
            out << "  1) Start here: layout.json + PlanSpec + CSV -> real run report\n";
            out << "  2) Inspect before running: combined plan report, no execution\n";
            out << "  3) Direct artifact run: choose your own layout.json + PlanSpec + CSV\n";
            out << "  4) Sweep engine chapters: exhaustive, Monte Carlo, coarse/fine, Bayesian posterior\n";
            out << "  5) Dynamic execution + topology chapters over checked-in demos\n";
            out << "  6) Advanced report projections: table, objective, diversity, duplicates, geometry, Pareto, trace, JSON\n";
            out << "Selection [default: 1, q to quit]: ";

            std::string line;
            std::getline(in, line);
            if (sugar::is_cancel_input(line)) {
                return InteractiveWorkflow::Canceled;
            }
            if (line.empty()) {
                return InteractiveWorkflow::FirstRunTutorial;
            }

            int choice = 0;
            if (parse_menu_choice(line, 1, 6, choice)) {
                return static_cast<InteractiveWorkflow>(choice);
            }

            out << "[Info] Invalid selection. Enter 1-6 or q to cancel.\n";
        }
    }

    void print_equivalent_cli(std::ostream& out, const std::string& command) {
        out << "[Info] equivalent CLI: sugarbot " << command << "\n";
    }

    void print_tutorial_header(std::ostream& out, std::string_view title) {
        out << "\nSugarBot tutorial: " << title << "\n";
    }

    bool prompt_after_tutorial(std::istream& in, std::ostream& out) {
        out << "\nPress Enter to return to the tutorial menu (q to quit): ";
        std::string line;
        if (!std::getline(in, line)) {
            return false;
        }
        return !sugar::is_cancel_input(line);
    }

    void print_quickstart_files(std::ostream& out, const TutorialArtifacts& artifacts) {
        out << "Files used:\n";
        out << "  layout: " << artifacts.layout_path << "\n";
        out << "  PlanSpec: " << artifacts.logic_path << "\n";
        out << "  CSV: " << artifacts.csv_path << "\n";
    }

    void print_three_artifact_schema_spotlight(std::ostream& out) {
        out << "Schema spotlight:\n";
        out << "  layout.ingest maps CSV columns into SugarBot's observation world.\n";
        out << "  PlanSpec numeric_nodes and bool_nodes define the strategy graph.\n";
        out << "  bindings.entry_long and bindings.exit_long connect graph outputs to trading intent.\n";
    }

    void print_report_schema_spotlight(std::ostream& out) {
        out << "Schema spotlight:\n";
        out << "  --obs-report is the narrow data-only check for parsed rows, columns, entities, and topology readiness.\n";
        out << "  --plan-report includes observation checks, then binds PlanSpec refs before execution.\n";
        out << "  This chapter runs only --plan-report so the observation report is not repeated.\n";
    }

    void print_sweep_schema_spotlight(
        std::ostream& out,
        std::string_view engine,
        std::string_view knobs) {
        out << "Schema spotlight:\n";
        out << "  sweep.enabled=true turns one PlanSpec into a ranked candidate search.\n";
        out << "  sweep.engine=\"" << engine << "\" selects the candidate scheduling policy.\n";
        out << "  sweep.params defines the finite parameter grid the engine can choose from.\n";
        out << "  " << knobs << "\n";
    }

    void print_dynamic_schema_spotlight(std::ostream& out) {
        out << "Schema spotlight:\n";
        out << "  execution.mode=\"declared_dynamic\" enables bounded row-local routing.\n";
        out << "  execution.subgraphs declares the node sets that can be activated.\n";
        out << "  execution.routes maps condition nodes to active subgraphs and fallbacks.\n";
        out << "  execution.outputs maps selected dynamic outputs back to strategy bindings.\n";
    }

    void print_topology_schema_spotlight(std::ostream& out) {
        out << "Schema spotlight:\n";
        out << "  layout.topologies declares query-bearing graph assets beside ingest metadata.\n";
        out << "  topology.* PlanSpec nodes consume those assets through explicit topology params.\n";
        out << "  Dynamic route conditions can use topology-derived node outputs like any other bool.\n";
        out << "  --report topology focuses on declared topology and operator-usage provenance.\n";
    }

    void print_advanced_report_schema_spotlight(std::ostream& out) {
        out << "Schema spotlight:\n";
        out << "  --report table is the ranked candidate spreadsheet view.\n";
        out << "  --report objective explains why candidate scores are rankable or not.\n";
        out << "  --report diversity, duplicates, geometry, pareto, and trace are focused diagnostics.\n";
        out << "  --report json is the scriptable schema; human projections are meant for reading.\n";
    }

    std::string run_command_text(
        const TutorialArtifacts& artifacts,
        sugar::EvaluationReportProjection projection,
        bool explicit_projection) {
        std::string command =
            "run " + artifacts.layout_path + " " + artifacts.logic_path + " " + artifacts.csv_path;
        if (explicit_projection) {
            command += " --report ";
            command += sugar::evaluation_report_projection_name(projection);
        }
        return command;
    }

    int run_tutorial_artifacts(
        const TutorialArtifacts& artifacts,
        sugar::EvaluationReportProjection projection,
        bool explicit_projection,
        std::ostream& out,
        std::ostream& err) {
        if (!tutorial_artifacts_exist(artifacts, err)) {
            return 1;
        }

        print_quickstart_files(out, artifacts);
        print_equivalent_cli(out, run_command_text(artifacts, projection, explicit_projection));
        out << "[Info] chapter run uses the existing noninteractive pipeline.\n";

        sugar::app::AppCommand run_cmd{};
        run_cmd.kind = sugar::app::CommandKind::Run;
        run_cmd.run.layout_path = artifacts.layout_path;
        run_cmd.run.logic_path = artifacts.logic_path;
        run_cmd.run.csv_path = artifacts.csv_path;
        run_cmd.run.report_projection = projection;
        return sugar::app::run_unified_cli_command(run_cmd, out, err);
    }

    bool prompt_path(
        std::istream& in,
        std::ostream& out,
        const char* label,
        const std::string& default_value,
        std::string& path)
    {
        for (;;) {
            out << label;
            if (!default_value.empty()) {
                out << " [default: " << default_value << "]";
            }
            out << " (q to cancel): ";

            std::string line;
            if (!std::getline(in, line)) {
                return false;
            }
            if (sugar::is_cancel_input(line)) {
                return false;
            }

            std::string candidate;
            if (line.empty()) {
                if (default_value.empty()) {
                    out << "[Info] Path is required. Enter a path or q to cancel.\n";
                    continue;
                }
                candidate = sugar::normalize_path_for_runtime(default_value);
            }
            else {
                candidate = sugar::normalize_path_for_runtime(std::move(line));
            }

            if (!is_existing_file_path(candidate)) {
                print_missing_file_prompt(out, candidate);
                continue;
            }

            path = std::move(candidate);
            return true;
        }
    }

    int run_first_run_tutorial(std::ostream& out, std::ostream& err) {
        const TutorialArtifacts artifacts = quickstart_artifacts();
        if (!tutorial_artifacts_exist(artifacts, err)) {
            return 1;
        }

        print_tutorial_header(out, "first real run");
        out << "Learning goal: understand the three artifacts SugarBot actually runs.\n";
        print_quickstart_files(out, artifacts);
        print_three_artifact_schema_spotlight(out);
        print_equivalent_cli(
            out,
            "run " + artifacts.layout_path + " " + artifacts.logic_path + " " + artifacts.csv_path);
        out << "[Info] tutorial run uses the existing noninteractive pipeline.\n";

        sugar::app::AppCommand run_cmd{};
        run_cmd.kind = sugar::app::CommandKind::Run;
        run_cmd.run.layout_path = artifacts.layout_path;
        run_cmd.run.logic_path = artifacts.logic_path;
        run_cmd.run.csv_path = artifacts.csv_path;

        const int rc = sugar::app::run_unified_cli_command(run_cmd, out, err);
        out << "Tutorial debrief:\n";
        out << "  The run report above came from the same path as `sugarbot run`.\n";
        out << "  Read Plan compatibility first, then execution metrics such as PnL, trades, and trade_hash.\n";
        return rc;
    }

    int run_inspect_tutorial(std::ostream& out, std::ostream& err) {
        const TutorialArtifacts artifacts = quickstart_artifacts();
        if (!tutorial_artifacts_exist(artifacts, err)) {
            return 1;
        }

        print_tutorial_header(out, "inspect before running");
        out << "Learning goal: diagnose data and logic before asking SugarBot for a result.\n";
        print_quickstart_files(out, artifacts);
        print_report_schema_spotlight(out);

        sugar::app::AppCommand plan_cmd{};
        plan_cmd.kind = sugar::app::CommandKind::PlanReport;
        plan_cmd.plan_report.layout_path = artifacts.layout_path;
        plan_cmd.plan_report.logic_path = artifacts.logic_path;
        plan_cmd.plan_report.csv_path = artifacts.csv_path;
        print_equivalent_cli(
            out,
            "--plan-report " + artifacts.layout_path + " " +
                artifacts.logic_path + " " + artifacts.csv_path);
        const int plan_rc = sugar::app::run_plan_report_command(plan_cmd, out, err);

        out << "Tutorial debrief:\n";
        out << "  The combined plan report above is the usual inspect-before-run stop.\n";
        out << "  Use standalone --obs-report only when data parsing or layout authoring is the question.\n";
        return plan_rc;
    }

    int run_direct_artifact_command(
        const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand run_cmd{};
        run_cmd.kind = sugar::app::CommandKind::Run;

        print_tutorial_header(out, "direct artifact run");
        out << "Learning goal: use the tutorial shell as a thin wrapper over the real CLI path.\n";
        if (!prompt_path(in, out, "Layout JSON path", interactive.runtime_data_spec_path, run_cmd.run.layout_path) ||
            !prompt_path(in, out, "Logic/PlanSpec JSON path", {}, run_cmd.run.logic_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, run_cmd.run.csv_path))
        {
            out << "[Info] Interactive file run cancelled.\n";
            return 0;
        }

        out << "[Info] direct artifact run using existing noninteractive pipeline.\n";
        print_equivalent_cli(out, "run " + run_cmd.run.layout_path + " " + run_cmd.run.logic_path + " " + run_cmd.run.csv_path);
        const int rc = sugar::app::run_unified_cli_command(run_cmd, out, err);
        if (rc == 0) {
            out << "Tutorial debrief:\n";
            out << "  Your selected files just ran through the same path as `sugarbot run`.\n";
            out << "  Keep the equivalent CLI command above as the reproducible form of this run.\n";
        }
        return rc;
    }

    struct SweepEngineChapter {
        std::string_view title{};
        std::string_view engine{};
        std::string_view planspec_file{};
        std::string_view knobs{};
        sugar::EvaluationReportProjection projection{ sugar::EvaluationReportProjection::Compact };
    };

    int run_sweep_engine_tutorial(std::ostream& out, std::ostream& err) {
        const std::vector<SweepEngineChapter> chapters = {
            {
                "exhaustive grid",
                "exhaustive",
                "i_exhaustive_threshold_sweep.planspec.json",
                "Exhaustive uses the whole bounded grid; no engine_config object is required.",
                sugar::EvaluationReportProjection::Compact,
            },
            {
                "Monte Carlo seeded sample",
                "monte_carlo",
                "i_monte_carlo_threshold_sweep.planspec.json",
                "sweep.engine_config.monte_carlo carries draws, seed, dedupe, and retained top_k.",
                sugar::EvaluationReportProjection::Compact,
            },
            {
                "coarse/fine neighborhood search",
                "coarse_fine",
                "i_coarse_fine_threshold_sweep.planspec.json",
                "sweep.engine_config.coarse_fine carries stage breadth, coarse stride, hops, and candidate dedupe.",
                sugar::EvaluationReportProjection::Compact,
            },
            {
                "Bayesian posterior policy",
                "bayesian_posterior",
                "i_bayesian_posterior_threshold_sweep.planspec.json",
                "sweep.engine_config.bayesian_posterior carries surrogate, acquisition, design size, iterations, seed, and normalization.",
                sugar::EvaluationReportProjection::PosteriorDiagnostics,
            },
        };

        print_tutorial_header(out, "sweep engine chapters");
        out << "Learning goal: compare sweep.engine choices while keeping layout and CSV fixed.\n";
        out << "Shared lesson: engine choice changes candidate scheduling, not the PlanSpec graph semantics.\n";

        for (const auto& chapter : chapters) {
            out << "\nChapter: " << chapter.title << "\n";
            print_sweep_schema_spotlight(out, chapter.engine, chapter.knobs);
            const TutorialArtifacts artifacts = quickstart_sweep_artifacts(chapter.planspec_file);
            const int rc = run_tutorial_artifacts(
                artifacts,
                chapter.projection,
                true,
                out,
                err);
            if (rc != 0) {
                return rc;
            }
        }

        out << "Tutorial debrief:\n";
        out << "  sweep.params is the candidate space shared by all four chapters.\n";
        out << "  sweep.engine_config is typed per engine and only appears when that engine needs knobs.\n";
        out << "  Compact reports summarize candidate outcomes; posterior reports expose Bayesian training and proposal diagnostics.\n";
        return 0;
    }

    int run_dynamic_topology_tutorial(std::ostream& out, std::ostream& err) {
        print_tutorial_header(out, "dynamic execution and topology");
        out << "Learning goal: see declared dynamic routes and topology-backed route inputs through real reports.\n";

        out << "\nChapter: row-varying declared dynamic routing\n";
        print_dynamic_schema_spotlight(out);
        int rc = run_tutorial_artifacts(
            row_dynamic_artifacts(),
            sugar::EvaluationReportProjection::Full,
            false,
            out,
            err);
        if (rc != 0) {
            return rc;
        }

        out << "\nChapter: topology-informed dynamic routing\n";
        print_topology_schema_spotlight(out);
        rc = run_tutorial_artifacts(
            topology_dynamic_artifacts(),
            sugar::EvaluationReportProjection::Full,
            false,
            out,
            err);
        if (rc != 0) {
            return rc;
        }

        out << "\nChapter: focused topology projection\n";
        out << "Schema spotlight:\n";
        out << "  The same artifacts can be re-run with --report topology for focused topology diagnostics.\n";
        out << "  Use this when route behavior depends on query-bearing topology assets.\n";
        rc = run_tutorial_artifacts(
            topology_dynamic_artifacts(),
            sugar::EvaluationReportProjection::TopologyDiagnostics,
            true,
            out,
            err);
        if (rc != 0) {
            return rc;
        }

        out << "Tutorial debrief:\n";
        out << "  Declared dynamic execution is bounded PlanSpec routing metadata, not a second executor.\n";
        out << "  Topology stays in LayoutSpec/runtime provenance while topology.* nodes provide route inputs.\n";
        return 0;
    }

    struct AdvancedReportChapter {
        std::string_view title{};
        sugar::EvaluationReportProjection projection{ sugar::EvaluationReportProjection::Compact };
        std::string_view lesson{};
    };

    int run_advanced_reports_tutorial(std::ostream& out, std::ostream& err) {
        const std::vector<AdvancedReportChapter> chapters = {
            {
                "candidate table",
                sugar::EvaluationReportProjection::CandidateTable,
                "Use table when you need stable rows for ranked candidates.",
            },
            {
                "objective breakdown",
                sugar::EvaluationReportProjection::ObjectiveBreakdown,
                "Use objective when score construction or rankability is the question.",
            },
            {
                "result diversity",
                sugar::EvaluationReportProjection::DiversityDiagnostics,
                "Use diversity when repeated trade hashes or basin filtering affect Top-K.",
            },
            {
                "duplicate trade paths",
                sugar::EvaluationReportProjection::DuplicateTradePaths,
                "Use duplicates to inspect repeated trade_hash groups directly.",
            },
            {
                "search geometry",
                sugar::EvaluationReportProjection::SearchGeometry,
                "Use geometry to inspect basins, winner neighborhoods, and retained-pool shape.",
            },
            {
                "Pareto front",
                sugar::EvaluationReportProjection::ParetoFront,
                "Use pareto when multiple objective directions are active.",
            },
            {
                "replay trace",
                sugar::EvaluationReportProjection::ReplayTrace,
                "Use trace to inspect proposal/observation order and exact replay metadata.",
            },
            {
                "scriptable JSON",
                sugar::EvaluationReportProjection::Json,
                "Use json for automation that needs the full structured report object.",
            },
        };

        print_tutorial_header(out, "advanced report projections");
        out << "Learning goal: choose the narrowest report projection for the question being asked.\n";
        out << "Shared artifact: one small sweep with result diversity, Pareto, geometry, and replay trace metadata.\n";
        print_advanced_report_schema_spotlight(out);

        const TutorialArtifacts artifacts = advanced_report_artifacts();
        for (const auto& chapter : chapters) {
            out << "\nChapter: " << chapter.title << "\n";
            out << "Projection lesson: " << chapter.lesson << "\n";
            const int rc = run_tutorial_artifacts(
                artifacts,
                chapter.projection,
                true,
                out,
                err);
            if (rc != 0) {
                return rc;
            }
        }

        out << "Tutorial debrief:\n";
        out << "  Advanced projections are report-layer views over the same evaluated sweep.\n";
        out << "  Prefer the narrow human projection for inspection and JSON for automation.\n";
        out << "  Milestone I is complete when these tutorial paths stay real CLI-backed runs.\n";
        return 0;
    }

} // namespace

namespace sugar::app {

    int run_interactive_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        for (;;) {
            const InteractiveWorkflow workflow = prompt_interactive_workflow(in, out);
            int rc = 0;
            switch (workflow) {
            case InteractiveWorkflow::FirstRunTutorial:
                rc = run_first_run_tutorial(out, err);
                break;
            case InteractiveWorkflow::InspectTutorial:
                rc = run_inspect_tutorial(out, err);
                break;
            case InteractiveWorkflow::DirectArtifactRun:
                rc = run_direct_artifact_command(cmd.interactive, in, out, err);
                break;
            case InteractiveWorkflow::SweepEngineTutorial:
                rc = run_sweep_engine_tutorial(out, err);
                break;
            case InteractiveWorkflow::DynamicTopologyTutorial:
                rc = run_dynamic_topology_tutorial(out, err);
                break;
            case InteractiveWorkflow::AdvancedReportsTutorial:
                rc = run_advanced_reports_tutorial(out, err);
                break;
            case InteractiveWorkflow::Canceled:
                out << "[Info] Interactive command cancelled.\n";
                return 0;
            }

            const bool continue_tutorial = prompt_after_tutorial(in, out);
            if (rc != 0 || !continue_tutorial) {
                return rc;
            }
        }
    }

} // namespace sugar::app
