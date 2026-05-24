#include "tests/app_test_support.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "ingest/layout_spec.h"

namespace sugar::app::tests {

    namespace {

        bool test_dump_ops_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-ops smoke\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::DumpOps;
            std::ostringstream out;
            const int rc = run_dump_ops_command(cmd, out);
            const std::string text = out.str();
            t.check(rc == 0, "dump-ops command returns success");
            t.check(text.find("ts.sma") != std::string::npos, "dump-ops output includes ts.sma");
            t.check(text.find("core.select") != std::string::npos, "dump-ops output includes core.select");
            t.check(text.find("ts.macd") != std::string::npos, "dump-ops output includes ts.macd");
            t.check(text.find("ts.bollinger_width") != std::string::npos, "dump-ops output includes ts.bollinger_width");
            t.check(text.find("ts.entropy") != std::string::npos, "dump-ops output includes ts.entropy");
            t.check(text.find("ts.autocorrelation") != std::string::npos, "dump-ops output includes ts.autocorrelation");
            t.check(text.find("ts.hurst") != std::string::npos, "dump-ops output includes ts.hurst");
            t.check(text.find("ts.regime_change") != std::string::npos, "dump-ops output includes ts.regime_change");
            t.check(text.find("ms.correlation") != std::string::npos, "dump-ops output includes ms.correlation");
            t.check(text.find("spatial.identity") != std::string::npos, "dump-ops output includes spatial.identity");
            t.check(text.find("spatial.radius_mean") != std::string::npos, "dump-ops output includes spatial.radius_mean");
            t.check(text.find("st.radius_mean") != std::string::npos, "dump-ops output includes st.radius_mean");
            t.check(text.find("topology.neighbor_mean") != std::string::npos, "dump-ops output includes topology.neighbor_mean");
            t.check(text.find("topology.weighted_sum") != std::string::npos, "dump-ops output includes topology.weighted_sum");
            t.check(text.find("topology.degree") != std::string::npos, "dump-ops output includes topology.degree");
            t.check(text.find("required_topology_families") != std::string::npos,
                "dump-ops output includes topology family metadata");
            t.check(text.find("topology_binding_param") != std::string::npos,
                "dump-ops output includes topology binding metadata");
            t.check(text.find("default_locality_subject") != std::string::npos,
                "dump-ops output includes locality-subject metadata");
            t.check(text.find("primary_series") != std::string::npos,
                "dump-ops output exposes primary-series locality subject default");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "dump-ops output includes built-in spatial topology default");
            t.check(text.find("builtin.spatiotemporal.composite") != std::string::npos,
                "dump-ops output includes built-in spatiotemporal topology default");
            t.check(text.find("`topology`") != std::string::npos,
                "dump-ops output exposes the locality topology binding param name");
            t.check(text.find("`subject_series`") != std::string::npos,
                "dump-ops output exposes the locality subject binding param name");
            t.check(text.find("caller-supplied `DataView` observations") != std::string::npos,
                "dump-ops contract text describes DataView-first custom runtime");
            t.check(text.find("Legacy embedded observation inputs (`series_inputs`) are optional compatibility metadata") != std::string::npos,
                "dump-ops contract text marks legacy embedded observation inputs as optional adapter metadata");
            t.check(text.find("not a primary app/runtime surface") != std::string::npos,
                "dump-ops contract text demotes legacy embedded observation inputs from primary runtime surface");
            t.check(text.find("requires `series_inputs`") == std::string::npos,
                "dump-ops contract text no longer claims series_inputs is universally required");
            t.check(text.find("`--dump-layout`") != std::string::npos,
                "dump-ops output points users to the layout contract surface");
            t.check(text.find("`--dump-plan`") != std::string::npos,
                "dump-ops output points users to the plan contract surface");
            t.check(text.find("`--align-dump <layout.json> <csv> --series-key-col <col>`") != std::string::npos,
                "dump-ops output points users to align-dump for aligned-series discovery");
            t.check(text.find("authoring_note: requires multiple aligned series/entity lanes") != std::string::npos,
                "dump-ops output includes per-op cross-series authoring notes");
            t.check(text.find("use `ts.sub` instead") != std::string::npos,
                "dump-ops output suggests ts.sub for same-series subtraction");
            return t.failed == 0;
        }

        bool test_dump_ops_module_catalog_example_sections(TestContext& t) {
            t.out << "[app_dispatch] dump-ops module/catalog/example sections\n";

            AppCommand modules_cmd{};
            modules_cmd.kind = CommandKind::DumpOps;
            modules_cmd.dump_ops.section = DumpOpsSection::Modules;
            std::ostringstream modules_out;
            const int modules_rc = run_dump_ops_command(modules_cmd, modules_out);
            const std::string modules_text = modules_out.str();
            t.check(modules_rc == 0, "dump-ops modules command returns success");
            t.check(modules_text.find("SugarBot Operator Modules And Rules") != std::string::npos,
                "dump-ops modules prints module header");
            t.check(modules_text.find("Node And Evaluation Rules") != std::string::npos,
                "dump-ops modules prints node evaluation rules");
            t.check(modules_text.find("core.ref") != std::string::npos,
                "dump-ops modules explains reference surfaces");
            t.check(modules_text.find("same-series arithmetic belongs here") != std::string::npos,
                "dump-ops modules explains ts vs ms arithmetic rule");
            t.check(modules_text.find("generic `topology.*` neighborhood operators") != std::string::npos,
                "dump-ops modules marks topology operators as runnable CLI-first locality ops");
            t.check(modules_text.find("query-bearing topology providers") != std::string::npos,
                "dump-ops modules explains topology operator provider requirements");
            t.check(modules_text.find("cmp.gte") != std::string::npos &&
                    modules_text.find("bool.and") != std::string::npos,
                "dump-ops modules documents comparison and boolean alias traps");
            t.check(modules_text.find("Declared dynamic execution is PlanSpec routing metadata") != std::string::npos &&
                    modules_text.find("execution.mode=\"declared_dynamic\"") != std::string::npos &&
                    modules_text.find("graphs/demos/dynamic_execution/") != std::string::npos,
                "dump-ops modules points dynamic execution authors to plan schema and demos");
            t.check(modules_text.find("**ts.sma**") == std::string::npos,
                "dump-ops modules avoids per-op catalog detail");

            AppCommand ops_cmd{};
            ops_cmd.kind = CommandKind::DumpOps;
            ops_cmd.dump_ops.section = DumpOpsSection::Operations;
            std::ostringstream ops_out;
            const int ops_rc = run_dump_ops_command(ops_cmd, ops_out);
            const std::string ops_text = ops_out.str();
            t.check(ops_rc == 0, "dump-ops ops command returns success");
            t.check(ops_text.find("SugarBot Operator Catalog") != std::string::npos,
                "dump-ops ops prints catalog header");
            t.check(ops_text.find("**ts.sma**") != std::string::npos,
                "dump-ops ops includes operator detail");
            t.check(ops_text.find("Accepted Operator Aliases") != std::string::npos &&
                    ops_text.find("`core.col` -> `core.ref`") != std::string::npos &&
                    ops_text.find("`ms.col` -> `core.ref`") != std::string::npos &&
                    ops_text.find("`bool.and` -> `logic.and`") != std::string::npos,
                "dump-ops ops lists accepted aliases including core.col");
            t.check(ops_text.find("Example 1:") == std::string::npos,
                "dump-ops ops avoids working examples");

            AppCommand examples_cmd{};
            examples_cmd.kind = CommandKind::DumpOps;
            examples_cmd.dump_ops.section = DumpOpsSection::Examples;
            std::ostringstream examples_out;
            const int examples_rc = run_dump_ops_command(examples_cmd, examples_out);
            const std::string examples_text = examples_out.str();
            t.check(examples_rc == 0, "dump-ops examples command returns success");
            t.check(examples_text.find("SugarBot Operator Working Examples") != std::string::npos,
                "dump-ops examples prints examples header");
            t.check(examples_text.find("Spatial Radius Mean") != std::string::npos,
                "dump-ops examples includes spatial example");
            t.check(examples_text.find("Named Layout Column (core.ref)") != std::string::npos &&
                    examples_text.find("\"op\": \"core.ref\"") != std::string::npos &&
                    examples_text.find("\"params\": { \"ref\": \"ROC\" }") != std::string::npos,
                "dump-ops examples shows core.ref column lookup syntax");
            t.check(examples_text.find("core.col") != std::string::npos &&
                    examples_text.find("same-series column accessor") != std::string::npos,
                "dump-ops examples explains the core.col alias");
            t.check(examples_text.find("Precomputed Numeric Leaf (advanced)") != std::string::npos &&
                    examples_text.find("\"op\": \"core.precomputed_num\"") != std::string::npos &&
                    examples_text.find("\"params\": { \"name\": \"MA\" }") != std::string::npos &&
                    examples_text.find("Prefer `core.ref`/`core.col`") != std::string::npos,
                "dump-ops examples explains precomputed numeric leaf usage");
            t.check(examples_text.find("Sweep Parameter Threshold") != std::string::npos &&
                    examples_text.find("There is no `core.param` operator") != std::string::npos &&
                    examples_text.find("\"op\": \"core.const\"") != std::string::npos &&
                    examples_text.find("\"value\": { \"ref\": \"threshold\" }") != std::string::npos &&
                    examples_text.find("\"threshold\": { \"min\": -5.0, \"max\": 5.0, \"step\": 1.0 }") != std::string::npos,
                "dump-ops examples documents sweep parameter thresholds through core.const refs");
            t.check(examples_text.find("Row-Domain Topology Degree Gate") != std::string::npos &&
                    examples_text.find("\"domain_scope\": \"row_domain\"") != std::string::npos &&
                    examples_text.find("\"query_kinds\": [\"neighbor_set\"]") != std::string::npos &&
                    examples_text.find("\"op\": \"topology.degree\"") != std::string::npos &&
                    examples_text.find("\"topology\": \"row.chain\"") != std::string::npos,
                "dump-ops examples includes a minimal topology PlanSpec path");
            t.check(examples_text.find("## Module:") == std::string::npos,
                "dump-ops examples avoids full op catalog");

            return t.failed == 0;
        }

        bool test_help_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] help smoke\n";
            std::ostringstream out;
            const int rc = run_help_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "help command returns success");
            t.check(text.find("SugarBot CLI") != std::string::npos,
                "help output includes CLI header");
            t.check(text.find("--obs-report") != std::string::npos,
                "help output includes obs-report surface");
            t.check(text.find("--plan-report") != std::string::npos,
                "help output includes plan-report surface");
            t.check(text.find("--feature-dump") != std::string::npos,
                "help output includes feature-dump surface");
            t.check(text.find("--onramp") != std::string::npos,
                "help output includes onramp surface");
            t.check(text.find("--help-diagnostics") != std::string::npos,
                "help output includes diagnostics help surface");
            t.check(text.find("--help-reports") != std::string::npos,
                "help output includes reports help surface");
            t.check(text.find("--scaffold") != std::string::npos,
                "help output includes scaffold surface");
            t.check(text.find("run <layout.json> <logic.json> <data.csv>") != std::string::npos,
                "help output includes unified run surface");
            t.check(text.find("sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/quickstart_ohlcv.planspec.json data/sample.csv") != std::string::npos,
                "help output includes the checked-in quickstart workflow");
            t.check(text.find("--dump-layout") != std::string::npos,
                "help output includes dump-layout surface");
            t.check(text.find("--dump-plan") != std::string::npos,
                "help output includes dump-plan surface");
            t.check(text.find("--dump-logic") != std::string::npos,
                "help output includes dump-plan alias");
            t.check(text.find("raw `DataSpec` object") == std::string::npos &&
                    text.find("`data_spec`") == std::string::npos,
                "help output delegates layout contract detail to dump-layout");
            t.check(text.find("--align-dump layout.json data.csv --series-key-col <col>") == std::string::npos,
                "help output delegates align-dump detail to diagnostics help");
            t.check(text.find("legacy_ohlcv_date") == std::string::npos,
                "help output keeps legacy format presets out of the short front door");
            t.check(text.find("CSV + layout + PlanSpec DAG model") != std::string::npos &&
                    text.find("human-readable output by default") != std::string::npos,
                "help output points first-time users to the onramp and human default output");
            t.check(text.find("Interactive mode keeps exploratory CSV compatibility presets separate from the canonical three-artifact `run` path") != std::string::npos,
                "help output scopes compatibility presets to interactive mode");
            t.check(text.find("Reports and diagnostics:") == std::string::npos &&
                    text.find("Plan contract:") == std::string::npos &&
                    text.find("Layout contract:") == std::string::npos,
                "help output stays a short front door instead of a full reference wall");
            t.check(text.find("`sugarbot --help-diagnostics`: obs/align/plan-report workflow") != std::string::npos &&
                    text.find("`sugarbot --help-reports`: final `run --report ...` projections") != std::string::npos &&
                    text.find("explicit layout starter from real CSV headers, not a legacy preset") != std::string::npos,
                "help output points to separated detailed help surfaces");
            t.check(text.find("Dynamic DAG execution:") != std::string::npos &&
                    text.find("execution.mode=\"declared_dynamic\"") != std::string::npos &&
                    text.find("graphs/demos/dynamic_execution/") != std::string::npos,
                "help output surfaces dynamic DAG execution from the front door");
            t.check(text.find("cmp.gte") != std::string::npos &&
                    text.find("cmp.lte") != std::string::npos &&
                    text.find("bool.and/or/not") != std::string::npos,
                "help output names common comparison and boolean aliases");
            t.check(text.find("run.scope=per_entity_ranked") != std::string::npos &&
                    text.find("requires `run.top_k`") != std::string::npos,
                "help output gives proactive ranked-run top_k guidance");
            return t.failed == 0;
        }

        bool test_help_diagnostics_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] diagnostics help smoke\n";
            std::ostringstream out;
            const int rc = run_help_diagnostics_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "diagnostics help command returns success");
            t.check(text.find("SugarBot Diagnostics Help") != std::string::npos,
                "diagnostics help output includes header");
            t.check(text.find("Recommended sequence:") != std::string::npos &&
                    text.find("`sugarbot --obs-report layout.json data.csv`") != std::string::npos &&
                    text.find("`sugarbot --align-dump layout.json data.csv --series-key-col <col>`") != std::string::npos &&
                    text.find("`sugarbot --plan-report layout.json logic.json data.csv`") != std::string::npos,
                "diagnostics help output explains the diagnostic sequence");
            t.check(text.find("parsed rows") != std::string::npos &&
                    text.find("time column/kind") != std::string::npos &&
                    text.find("value columns") != std::string::npos &&
                    text.find("ingest diagnostics") != std::string::npos &&
                    text.find("row-domain facts") != std::string::npos &&
                    text.find("declared-topology warnings") != std::string::npos,
                "diagnostics help output describes obs-report early validation");
            t.check(text.find("required/declared query kinds") != std::string::npos &&
                    text.find("capability mismatches") != std::string::npos,
                "diagnostics help output describes plan-report topology capability checks");
            t.check(text.find("declared dynamic execution mode/subgraph/route/output counts") != std::string::npos,
                "diagnostics help output describes dynamic execution validation in plan-report");
            t.check(text.find("Failure triage:") != std::string::npos &&
                    text.find("Final report shape questions: use `--help-reports`") != std::string::npos,
                "diagnostics help output includes failure triage and report cross-link");
            t.check(text.find("Current surface: `--help-diagnostics`") != std::string::npos &&
                    text.find("`sugarbot --onramp` explains how the three artifacts fit together") != std::string::npos,
                "diagnostics help output cross-links onramp and artifacts");
            return t.failed == 0;
        }

        bool test_help_reports_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] reports help smoke\n";
            std::ostringstream out;
            const int rc = run_help_reports_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "reports help command returns success");
            t.check(text.find("SugarBot Reports Help") != std::string::npos,
                "reports help output includes header");
            t.check(text.find("changes only the final execution projection") != std::string::npos &&
                    text.find("does not replace `--obs-report`, `--align-dump`, or `--plan-report`") != std::string::npos,
                "reports help output separates final reports from diagnostics");
            t.check(text.find("Human-oriented projections:") != std::string::npos &&
                    text.find("`full` or `default`") != std::string::npos &&
                    text.find("`topology`") != std::string::npos &&
                    text.find("`trace`") != std::string::npos,
                "reports help output lists human-oriented projections");
            t.check(text.find("Automation-oriented projections:") != std::string::npos &&
                    text.find("`compact`: compact summary for scripts or AI agents") != std::string::npos &&
                    text.find("`json`: a single final `sugarbot.evaluation_report.v1` object") != std::string::npos,
                "reports help output marks compact/json as automation-oriented");
            t.check(text.find("run.provenance.topologies") != std::string::npos &&
                    text.find("run.provenance.topology_usages") != std::string::npos &&
                    text.find("run.execution.dynamic_execution") != std::string::npos,
                "reports help output documents topology and dynamic execution JSON paths");
            t.check(text.find("includes dynamic execution route diagnostics") != std::string::npos,
                "reports help output says default/full reports include dynamic route diagnostics");
            t.check(text.find("sugarbot run layout.json logic.json data.csv --report topology") != std::string::npos &&
                    text.find("sugarbot run layout.json logic.json data.csv --report json") != std::string::npos,
                "reports help output includes focused report examples");
            t.check(text.find("This help page documents local CLI `sugarbot run` report output") != std::string::npos &&
                    text.find("promotion") == std::string::npos &&
                    text.find("stabilization") == std::string::npos,
                "reports help output preserves a user-facing CLI boundary");
            return t.failed == 0;
        }

        bool test_onramp_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] onramp smoke\n";
            std::ostringstream out;
            const int rc = run_onramp_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "onramp command returns success");
            t.check(text.find("SugarBot Onramp") != std::string::npos,
                "onramp output includes header");
            t.check(text.find("A SugarBot run combines three artifacts: a CSV, a layout that maps CSV columns into an observation world, and a PlanSpec DAG that computes numeric/bool nodes and binds bool nodes to logical decisions.") != std::string::npos,
                "onramp output states the canonical three-artifact language");
            t.check(text.find("Observation world:") != std::string::npos &&
                    text.find("ResolvedObservationWorld") != std::string::npos &&
                    text.find("DataView") != std::string::npos &&
                    text.find("RunManifest") != std::string::npos,
                "onramp output defines observation world in code-grounded terms");
            t.check(text.find("Logical decisions:") != std::string::npos &&
                    text.find("entry_long") != std::string::npos &&
                    text.find("allow_trade") != std::string::npos &&
                    text.find("StrategyExecutor") != std::string::npos,
                "onramp output defines logical decisions in code-grounded terms");
            t.check(text.find("`time_col` names the ordered axis column") != std::string::npos &&
                    text.find("`time_kind` tells SugarBot how to parse/order") != std::string::npos &&
                    text.find("`value_cols` names the numeric columns available to plans") != std::string::npos &&
                    text.find("with one value column SugarBot can infer it") != std::string::npos,
                "onramp output says the obvious layout field purposes out loud");
            t.check(text.find("Checked-in first success:") != std::string::npos &&
                    text.find("graphs/demos/quickstart_ohlcv.layout.json") != std::string::npos &&
                    text.find("graphs/demos/quickstart_ohlcv.planspec.json") != std::string::npos &&
                    text.find("data/sample.csv") != std::string::npos,
                "onramp output points to the checked-in quickstart workflow");
            t.check(text.find("predefined workflows with explicit layout and PlanSpec artifacts") != std::string::npos &&
                    text.find("fail through diagnostics instead of hidden coercion") != std::string::npos,
                "onramp output guards quick starts against hidden compatibility coercion");
            t.check(text.find("`sugarbot --scaffold data.csv` can draft a modern explicit layout") != std::string::npos &&
                    text.find("does not choose a strategy, run legacy candle code, or coerce arbitrary data") != std::string::npos &&
                    text.find("echoes parsed rows, the time column/kind, value columns") != std::string::npos,
                "onramp output introduces scaffold without bypassing the modern path");
            t.check(text.find("PlanSpec reminders:") != std::string::npos &&
                    text.find("run.scope=per_entity_ranked") != std::string::npos &&
                    text.find("requires `run.top_k`") != std::string::npos &&
                    text.find("cmp.gte") != std::string::npos &&
                    text.find("bool.and/or/not") != std::string::npos,
                "onramp output says common PlanSpec traps out loud");
            t.check(text.find("Power-user trail:") != std::string::npos &&
                    text.find("first working examples for named columns, precomputed numeric leaves, and row-domain `topology.*` plans") != std::string::npos &&
                    text.find("graphs/demos/topology_readiness/") != std::string::npos &&
                    text.find("graphs/demos/dynamic_execution/") != std::string::npos &&
                    text.find("constant-route equivalence, row-varying routing, and topology-degree routing") != std::string::npos &&
                    text.find("data/topology_readiness/") != std::string::npos &&
                    text.find("topology node ids are aligned row indices after ingest/sort") != std::string::npos,
                "onramp output bridges from new-user diagnostics to power-user topology and dynamic execution examples");
            t.check(text.find("`sugarbot run layout.json logic.json data.csv`") != std::string::npos &&
                    text.find("default human-readable report") != std::string::npos,
                "onramp output keeps the default run human-readable");
            t.check(text.find("`--report compact` and `--report json` are automation-oriented") != std::string::npos,
                "onramp output marks compact/json as automation-oriented");
            t.check(text.find("input_profile") != std::string::npos &&
                    text.find("compatibility conveniences") != std::string::npos &&
                    text.find("core model is still CSV + layout + PlanSpec DAG") != std::string::npos,
                "onramp output keeps input_profile compatibility scoped");
            t.check(text.find("Legacy format presets belong to the interactive compatibility workflow") != std::string::npos,
                "onramp output keeps legacy format presets out of the canonical run path");
            t.check(text.find("`sugarbot --dump-layout` documents the layout") != std::string::npos &&
                    text.find("`sugarbot --dump-plan` documents the PlanSpec DAG") != std::string::npos,
                "onramp output cross-links layout and plan references");
            return t.failed == 0;
        }

        bool test_dump_layout_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-layout smoke\n";
            std::ostringstream out;
            const int rc = run_dump_layout_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "dump-layout command returns success");
            t.check(text.find("SugarBot Layout Contract") != std::string::npos,
                "dump-layout output includes header");
            t.check(text.find("This surface covers the layout artifact") != std::string::npos &&
                    text.find("A SugarBot run combines three artifacts") != std::string::npos &&
                    text.find("Observation world:") != std::string::npos &&
                    text.find("Logical decisions:") != std::string::npos,
                "dump-layout output includes UX0 run-model terminology");
            t.check(text.find("Current surface: `--dump-layout`") != std::string::npos &&
                    text.find("`sugarbot --dump-plan` documents the PlanSpec DAG") != std::string::npos &&
                    text.find("Dynamic DAG demos live in `graphs/demos/dynamic_execution/`") != std::string::npos,
                "dump-layout output cross-links the other artifacts");
            t.check(text.find("Raw `DataSpec` object") != std::string::npos,
                "dump-layout output explains the raw dataspec root shape");
            t.check(text.find("Wrapped `LayoutSpec` object with `ingest`") != std::string::npos,
                "dump-layout output explains the wrapped layout shape");
            t.check(text.find("`data_spec` is not a supported wrapper key") != std::string::npos,
                "dump-layout output rejects the unsupported data_spec wrapper");
            t.check(text.find("`YyyyMmDd`") != std::string::npos,
                "dump-layout output includes valid time kinds");
            t.check(text.find("`YYYY-MM-DD` or internal `YyyyMmDd`") != std::string::npos &&
                    text.find("`primary_value_col` is required when `value_cols` has more than one column") != std::string::npos &&
                    text.find("In the single-value case, the primary value is obvious") != std::string::npos &&
                    text.find("SINGLE VALUE COLUMN ASSUMED") != std::string::npos,
                "dump-layout output documents readable time aliases and single-value layout inference");
            t.check(text.find("Why the fields exist") != std::string::npos &&
                    text.find("`time_kind` tells SugarBot how to parse and order that axis deterministically") != std::string::npos &&
                    text.find("`value_cols` declares the numeric observation columns") != std::string::npos,
                "dump-layout output explains why layout fields exist");
            t.check(text.find("`sugarbot --scaffold data.csv` reads a headered CSV") != std::string::npos &&
                    text.find("does not apply `input_profile` presets or hidden legacy coercion") != std::string::npos,
                "dump-layout output documents scaffold as modern explicit layout authoring");
            t.check(text.find("Dynamic DAG execution and layout") != std::string::npos &&
                    text.find("Dynamic DAG execution is authored in PlanSpec `execution`, not in the layout") != std::string::npos &&
                    text.find("route selectors may consume `topology.*` nodes") != std::string::npos &&
                    text.find("execution.mode=\"declared_dynamic\"") != std::string::npos,
                "dump-layout output cross-links dynamic DAG execution to PlanSpec and topology layout");
            t.check(text.find("EqualLengthRowDomain") != std::string::npos,
                "dump-layout output includes non-temporal alignment policy values");
            return t.failed == 0;
        }

        bool test_scaffold_layout_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] scaffold smoke\n";
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_time_header_daily_fixture(csv_path, fixture_err);
            t.check(fixture_ok, "scaffold fixture CSV is created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::ScaffoldLayout;
            cmd.scaffold_layout.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_scaffold_layout_command(cmd, out, err);
            const std::string json = out.str();
            const std::string notes = err.str();

            t.check(rc == 0, "scaffold command returns success");
            t.check(notes.find("No legacy preset or compatibility runtime was used") != std::string::npos,
                "scaffold command states the no-legacy boundary");
            t.check(notes.find("Scaffold assumptions:") != std::string::npos &&
                    notes.find("time_col: time") != std::string::npos &&
                    notes.find("time_kind: YYYY-MM-DD") != std::string::npos &&
                    notes.find("value_cols: open, high, low, close, Volume") != std::string::npos,
                "scaffold command echoes the assumptions it used");
            t.check(json.find("\"ingest\"") != std::string::npos &&
                    json.find("\"time_col\": \"time\"") != std::string::npos &&
                    json.find("\"time_kind\": \"YYYY-MM-DD\"") != std::string::npos &&
                    json.find("\"value_cols\": [\"open\", \"high\", \"low\", \"close\", \"Volume\"]") != std::string::npos &&
                    json.find("\"primary_value_col\": \"close\"") != std::string::npos,
                "scaffold emits explicit layout JSON from actual CSV headers");
            t.check(json.find("input_profile") == std::string::npos &&
                    json.find("legacy_ohlcv_date") == std::string::npos &&
                    json.find("\"header_policy\"") == std::string::npos,
                "scaffold output omits legacy shortcuts and default-implied fields");

            sugar::LayoutSpec layout{};
            std::string parse_err;
            const bool parse_ok = sugar::parse_layout_spec_json(json, layout, parse_err);
            t.check(parse_ok, "scaffold output parses as LayoutSpec");
            if (parse_ok) {
                t.check(layout.ingest.time_col == "time" &&
                        layout.ingest.time_kind == sugar::TimeKind::YyyyMmDd &&
                        layout.ingest.value_cols.size() == 5 &&
                        layout.ingest.primary_value_col == "close",
                    "scaffold output preserves parsed modern layout semantics");
            }
            return t.failed == 0;
        }

        bool test_dump_plan_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-plan smoke\n";
            std::ostringstream out;
            const int rc = run_dump_plan_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "dump-plan command returns success");
            t.check(text.find("SugarBot Plan Contract") != std::string::npos,
                "dump-plan output includes header");
            t.check(text.find("This surface covers the PlanSpec DAG artifact") != std::string::npos &&
                    text.find("A SugarBot run combines three artifacts") != std::string::npos &&
                    text.find("Observation world:") != std::string::npos &&
                    text.find("Logical decisions:") != std::string::npos,
                "dump-plan output includes UX0 run-model terminology");
            t.check(text.find("Current surface: `--dump-plan`") != std::string::npos &&
                    text.find("`sugarbot --dump-layout` documents the layout") != std::string::npos,
                "dump-plan output cross-links the other artifacts");
            t.check(text.find("raw `PlanSpec` root object") != std::string::npos,
                "dump-plan output explains raw planspec root shape");
            t.check(text.find("run.scope") != std::string::npos,
                "dump-plan output documents run dispatch");
            t.check(text.find("primary_series") != std::string::npos,
                "dump-plan output includes primary-series scope");
            t.check(text.find("per_entity_ranked") != std::string::npos,
                "dump-plan output includes ranked scope");
            t.check(text.find("run.objective") != std::string::npos,
                "dump-plan output documents canonical run objective shape");
            t.check(text.find("Declared dynamic execution (`execution`)") != std::string::npos &&
                    text.find("mode=\"declared_dynamic\"") != std::string::npos &&
                    text.find("subgraphs") != std::string::npos &&
                    text.find("routes") != std::string::npos &&
                    text.find("outputs") != std::string::npos &&
                    text.find("bounds.decision_sample_limit") != std::string::npos &&
                    text.find("graphs/demos/dynamic_execution/") != std::string::npos,
                "dump-plan output documents the declared dynamic execution surface and demos");
            t.check(text.find("cmp.gte") != std::string::npos &&
                    text.find("cmp.lte") != std::string::npos &&
                    text.find("bool.and") != std::string::npos &&
                    text.find("core.precomputed_num") != std::string::npos,
                "dump-plan output says the obvious operator alias and precomputed-column rules");
            t.check(text.find("`--align-dump`") != std::string::npos,
                "dump-plan output points users to align-dump before cross-series authoring");
            t.check(text.find("values become the aligned series/entity keys") != std::string::npos,
                "dump-plan output explains --series-key-col semantics");
            t.check(text.find("`ts.sub`") != std::string::npos,
                "dump-plan output includes same-series subtraction guidance");
            return t.failed == 0;
        }

    } // namespace

    void register_reference_command_tests(AppTestRegistry& tests) {
        tests.push_back(test_dump_ops_dispatch_smoke);
        tests.push_back(test_dump_ops_module_catalog_example_sections);
        tests.push_back(test_help_dispatch_smoke);
        tests.push_back(test_help_diagnostics_dispatch_smoke);
        tests.push_back(test_help_reports_dispatch_smoke);
        tests.push_back(test_onramp_dispatch_smoke);
        tests.push_back(test_dump_layout_dispatch_smoke);
        tests.push_back(test_scaffold_layout_dispatch_smoke);
        tests.push_back(test_dump_plan_dispatch_smoke);
    }

} // namespace sugar::app::tests
