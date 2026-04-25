#include "contract_suite_internal.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

    namespace {
        struct AssertionHygienePattern {
            const char* reason;
            const char* regex_text;
        };

        struct AssertionHygieneFinding {
            std::string rel_path;
            std::size_t line{ 0 };
            std::string reason;
            std::string snippet;
        };

        bool has_suffix(const std::string& text, const char* suffix) {
            const std::size_t suffix_len = std::char_traits<char>::length(suffix);
            return text.size() >= suffix_len &&
                text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
        }

        std::filesystem::path contract_repo_root_from_source() {
            auto cursor = std::filesystem::path(__FILE__).parent_path();
            while (!cursor.empty()) {
                if (std::filesystem::exists(cursor / "CMakeLists.txt") &&
                    std::filesystem::exists(cursor / "src") &&
                    std::filesystem::exists(cursor / "include")) {
                    return cursor;
                }

                const auto parent = cursor.parent_path();
                if (parent == cursor) {
                    break;
                }
                cursor = parent;
            }

            return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        }

        std::vector<std::string> collect_assertion_hygiene_files(const std::filesystem::path& repo_root) {
            std::vector<std::string> files;

            const auto src_dir = repo_root / "src";
            if (std::filesystem::exists(src_dir)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
                    if (!entry.is_regular_file()) continue;
                    const std::string name = entry.path().filename().string();
                    if (name.rfind("contract_suite", 0) == 0 && has_suffix(name, ".cpp")) {
                        files.push_back(entry.path().lexically_relative(repo_root).generic_string());
                    }
                }
            }

            const auto app_dir = repo_root / "app";
            if (std::filesystem::exists(app_dir)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(app_dir)) {
                    if (!entry.is_regular_file()) continue;
                    const std::string name = entry.path().filename().string();
                    if (name == "app_tests.cpp" || has_suffix(name, "_tests.cpp")) {
                        files.push_back(entry.path().lexically_relative(repo_root).generic_string());
                    }
                }
            }

            std::sort(files.begin(), files.end());
            return files;
        }

        std::string strip_cpp_comments_and_literals(const std::string& body) {
            enum class State {
                Normal,
                LineComment,
                BlockComment,
                StringLiteral,
                CharLiteral,
            };

            std::string scrubbed = body;
            State state = State::Normal;
            bool escaped = false;

            for (std::size_t i = 0; i < scrubbed.size(); ++i) {
                const char ch = scrubbed[i];
                const char next = (i + 1 < scrubbed.size()) ? scrubbed[i + 1] : '\0';

                switch (state) {
                case State::Normal:
                    if (ch == '/' && next == '/') {
                        scrubbed[i] = ' ';
                        scrubbed[i + 1] = ' ';
                        ++i;
                        state = State::LineComment;
                    }
                    else if (ch == '/' && next == '*') {
                        scrubbed[i] = ' ';
                        scrubbed[i + 1] = ' ';
                        ++i;
                        state = State::BlockComment;
                    }
                    else if (ch == '"') {
                        scrubbed[i] = ' ';
                        state = State::StringLiteral;
                        escaped = false;
                    }
                    else if (ch == '\'') {
                        scrubbed[i] = ' ';
                        state = State::CharLiteral;
                        escaped = false;
                    }
                    break;
                case State::LineComment:
                    if (ch == '\n') {
                        state = State::Normal;
                    }
                    else {
                        scrubbed[i] = ' ';
                    }
                    break;
                case State::BlockComment:
                    if (ch == '\n') {
                        break;
                    }
                    scrubbed[i] = ' ';
                    if (ch == '*' && next == '/') {
                        scrubbed[i + 1] = ' ';
                        ++i;
                        state = State::Normal;
                    }
                    break;
                case State::StringLiteral:
                    if (ch == '\n') {
                        state = State::Normal;
                        escaped = false;
                        break;
                    }
                    scrubbed[i] = ' ';
                    if (escaped) {
                        escaped = false;
                    }
                    else if (ch == '\\') {
                        escaped = true;
                    }
                    else if (ch == '"') {
                        state = State::Normal;
                    }
                    break;
                case State::CharLiteral:
                    if (ch == '\n') {
                        state = State::Normal;
                        escaped = false;
                        break;
                    }
                    scrubbed[i] = ' ';
                    if (escaped) {
                        escaped = false;
                    }
                    else if (ch == '\\') {
                        escaped = true;
                    }
                    else if (ch == '\'') {
                        state = State::Normal;
                    }
                    break;
                }
            }

            return scrubbed;
        }

        std::size_t line_number_for_offset(const std::string& text, std::size_t offset) {
            return 1u + static_cast<std::size_t>(
                std::count(text.begin(), text.begin() + std::min(offset, text.size()), '\n'));
        }

        std::string collapse_whitespace(std::string text) {
            std::string out;
            out.reserve(text.size());

            bool pending_space = false;
            for (char ch : text) {
                if (std::isspace(static_cast<unsigned char>(ch))) {
                    pending_space = !out.empty();
                    continue;
                }
                if (pending_space) {
                    out.push_back(' ');
                    pending_space = false;
                }
                out.push_back(ch);
            }
            return out;
        }

        void collect_assertion_hygiene_findings(
            const std::string& rel_path,
            const std::string& scrubbed,
            std::vector<AssertionHygieneFinding>& out) {
            static const std::array<AssertionHygienePattern, 14> kPatterns = {{
                { "constant true assertion", R"(t\.(?:expect|check)\s*\(\s*true\s*,)" },
                { "negated false assertion", R"(t\.(?:expect|check)\s*\(\s*!\s*false\s*,)" },
                { "literal-true equality assertion", R"(t\.(?:expect|check)\s*\(\s*true\s*==\s*true\s*,)" },
                { "literal-false equality assertion", R"(t\.(?:expect|check)\s*\(\s*false\s*==\s*false\s*,)" },
                { "literal inequality assertion", R"(t\.(?:expect|check)\s*\(\s*(?:true\s*!=\s*false|false\s*!=\s*true)\s*,)" },
                { "self-equality assertion", R"(t\.(?:expect|check)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)\s*==\s*\1\s*,)" },
                { "self-inequality assertion", R"(t\.(?:expect|check)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)\s*!=\s*\1\s*,)" },
                { "duplicate-and assertion", R"(t\.(?:expect|check)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)\s*&&\s*\1\s*,)" },
                { "duplicate-or assertion", R"(t\.(?:expect|check)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)\s*\|\|\s*\1\s*,)" },
                { "constant true if-condition", R"(\bif\s*\(\s*true\s*\))" },
                { "negated false if-condition", R"(\bif\s*\(\s*!\s*false\s*\))" },
                { "literal-equality if-condition", R"(\bif\s*\(\s*(?:true\s*==\s*true|false\s*==\s*false)\s*\))" },
                { "literal-inequality if-condition", R"(\bif\s*\(\s*(?:true\s*!=\s*false|false\s*!=\s*true)\s*\))" },
                { "self-comparison if-condition", R"(\bif\s*\(\s*([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)\s*(?:==|!=|&&|\|\|)\s*\1\s*\))" },
            }};

            for (const auto& pattern : kPatterns) {
                const std::regex regex(pattern.regex_text, std::regex::ECMAScript | std::regex::optimize);
                for (std::sregex_iterator it(scrubbed.begin(), scrubbed.end(), regex), end; it != end; ++it) {
                    out.push_back(AssertionHygieneFinding{
                        rel_path,
                        line_number_for_offset(scrubbed, static_cast<std::size_t>(it->position())),
                        pattern.reason,
                        collapse_whitespace(it->str()),
                    });
                }
            }
        }

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
            TimeKey start_time = static_cast<TimeKey>(0),
            std::span<const double> spatial_x = {},
            std::span<const double> spatial_y = {}) {
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

        bool has_plan_reason(const PlanNodeCapability* node,
            PlanCapabilityReasonCode code,
            std::optional<AxisKind> axis = std::nullopt) {
            if (!node) return false;
            for (const auto& reason : node->reasons) {
                if (reason.code != code) continue;
                if (!axis.has_value()) return true;
                if (reason.has_axis && reason.axis == *axis) return true;
            }
            return false;
        }

        bool has_plan_reason_detail(const PlanNodeCapability* node,
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

        bool has_binding_reason(const PlanBindingCapability* binding,
            PlanCapabilityReasonCode code) {
            if (!binding) return false;
            for (const auto& reason : binding->reasons) {
                if (reason.code == code) return true;
            }
            return false;
        }

        bool any_plan_node_has_reason(const PlanCapabilityReport& report,
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

        bool expect_topology_resolution_failure(
            ContractAsserts& t,
            const char* action_label,
            std::string_view expected_substring,
            auto&& fn) {
            try {
                (void)fn();
                return t.expect(false, std::string(action_label) + " rejects explicitly");
            }
            catch (const std::exception& ex) {
                const std::string msg = ex.what();
                const bool contains = msg.find(expected_substring) != std::string::npos;
                return t.expect(contains, std::string(action_label) + " preserves explicit failure reason");
            }
        }

        PlanSpec make_spatial_radius_mean_plan(
            std::string name,
            double radius,
            double threshold,
            std::string topology_name,
            std::optional<std::string> subject_series = std::nullopt) {
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
    } // namespace

    bool test_plan_capability_contracts(ContractAsserts& t) {
        t.out << "[plan_capabilities] plan/operator capability diagnosis\n";

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "sma", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = false;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* close_node = find_plan_node(report, "c");
            const auto* sma_node = find_plan_node(report, "sma");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "spatial-only facts make temporal-local plan invalid");
            t.expect(report.incompatible_nodes == 1,
                "spatial-only facts count one incompatible temporal-local node");
            t.expect(close_node && close_node->status == PlanCapabilityStatus::Valid,
                "row-wise core.close remains valid on spatial-only facts");
            t.expect(sma_node && sma_node->status == PlanCapabilityStatus::Invalid,
                "ts.sma is invalid on spatial-only facts");
            t.expect(has_plan_reason(sma_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Time),
                "ts.sma failure cites missing time axis");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 4;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "temporal-only facts make spatial locality plan invalid");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Invalid,
                "spatial locality node is invalid when spatial topology is absent");
            t.expect(mean_node && mean_node->resolved_topology_name == builtin_spatial_topology_name(),
                "spatial locality node resolves built-in default topology name in capability report");
            t.expect(std::find(report.used_topology_names.begin(),
                    report.used_topology_names.end(),
                    std::string(builtin_spatial_topology_name())) != report.used_topology_names.end(),
                "plan capability report records used topology names");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Spatial),
                "spatial locality failure still cites missing spatial axis");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredTopologyFamily),
                "spatial locality failure cites missing required topology family");
            t.expect(has_plan_reason_detail(mean_node,
                    PlanCapabilityReasonCode::MissingRequiredTopologyFamily,
                    "provide spatial neighborhood/index schema"),
                "spatial locality topology-family failure carries providable spatial hint");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingNamedTopology),
                "spatial locality failure cites missing named topology when built-in spatial topology is unavailable");

            RunManifest manifest{};
            append_plan_capability_snapshot(report, manifest);
            const auto* usage = find_topology_usage(manifest, "mean", builtin_spatial_topology_name());
            t.expect(usage != nullptr,
                "plan capability snapshot records topology usage for unresolved built-in locality");
            if (usage) {
                t.expect(!usage->catalog_present,
                    "plan capability snapshot preserves missing-catalog topology usage state");
                t.expect(!usage->runtime_resolved,
                    "plan capability snapshot preserves unresolved topology state");
            }
            const auto* outcome = find_capability_outcome(manifest, "plan_node", "mean");
            t.expect(outcome != nullptr,
                "plan capability snapshot records plan-node outcome");
            if (outcome) {
                t.expect(outcome->status == "invalid",
                    "plan capability snapshot preserves plan-node invalid status");
            }
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "left", "core.close", {}, {} },
                NodeSpec{ "right", "core.close", {}, {} },
                NodeSpec{ "spread", "diff", { "left", "right" }, {} },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 10;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* spread_node = find_plan_node(report, "spread");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "single-series facts make cross-series plan invalid");
            t.expect(spread_node && spread_node->op_name == "ms.diff",
                "plan capability diagnosis normalizes runtime op aliases");
            t.expect(spread_node && spread_node->status == PlanCapabilityStatus::Invalid,
                "cross-series node is invalid on single-series facts");
            t.expect(has_plan_reason(spread_node, PlanCapabilityReasonCode::InsufficientSeriesCount),
                "cross-series node failure cites multiple-series requirement");
        }

        {
            const std::array<double, 3> close{ 10.0, 11.0, 12.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool runtime_ok = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(runtime_ok, "runtime-aware plan capability fixture builds single-series DataView");
            if (!runtime_ok) return true;

            ResolvedRuntimeInput runtime{};
            runtime.aligned_bundle = bundle;
            runtime.primary_ref = primary;
            runtime.primary_series_index = 0;
            runtime.run_manifest.preprocess.has_time_axis = true;
            runtime.run_manifest.preprocess.has_spatial_axis = false;
            runtime.run_manifest.preprocess.has_entity_axis = false;

            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "other_close", "core.ref", {}, { {"ref", std::string("other:close")} } },
                NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
            };
            spec.bool_nodes = {
                NodeSpec{ "entry", "cmp.gt", { "other_close", "zero" }, {} },
            };
            spec.bind.entry_long = "entry";
            spec.bind.exit_long = "missing_exit";

            const auto report = diagnose_plan_capabilities(spec, runtime);
            const auto* other_close = find_plan_node(report, "other_close");
            const auto* exit_binding = find_plan_binding(report, "exit_long");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "runtime-aware report is invalid for missing series binding");
            t.expect(report.incompatible_nodes == 1,
                "runtime-aware report counts invalid bound ref node");
            t.expect(report.incompatible_bindings == 1,
                "runtime-aware report counts invalid strategy binding target");
            t.expect(other_close && has_plan_reason(other_close, PlanCapabilityReasonCode::MissingRuntimeSeries),
                "core.ref failure cites missing runtime series");
            t.expect(exit_binding && has_binding_reason(exit_binding, PlanCapabilityReasonCode::InvalidBindingTarget),
                "binding failure cites invalid strategy binding target");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* dup = find_plan_node(report, "dup");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "missing or duplicate node ids make plan invalid");
            t.expect(any_plan_node_has_reason(report, PlanCapabilityReasonCode::MissingNodeId),
                "missing-id node is reported explicitly");
            t.expect(dup && has_plan_reason(dup, PlanCapabilityReasonCode::DuplicateNodeId),
                "duplicate node id is reported explicitly");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "st.radius_mean", { "c" }, {
                    {"last_n", static_cast<std::int64_t>(3)},
                    {"radius", 1.5}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 2;
            facts.row_count = 5;
            facts.has_time_axis = true;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Valid,
                "spatiotemporal facts make st.radius_mean plan valid");
            t.expect(report.incompatible_nodes == 0,
                "spatiotemporal facts produce no incompatible nodes");
            t.expect(std::find(report.used_axis_scopes.begin(),
                    report.used_axis_scopes.end(),
                    OpAxisScope::SpatioTemporal) != report.used_axis_scopes.end(),
                "spatiotemporal scope is recorded in used_axis_scopes");
            t.expect(std::find(report.used_topology_families.begin(),
                    report.used_topology_families.end(),
                    TopologyFamily::SpatioTemporalComposite) != report.used_topology_families.end(),
                "spatiotemporal topology family is recorded in used_topology_families");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Valid,
                "st.radius_mean node is valid on spatiotemporal facts");
            t.expect(mean_node && mean_node->required_topology_families ==
                std::vector<TopologyFamily>{ TopologyFamily::SpatioTemporalComposite },
                "st.radius_mean node preserves required topology family metadata");
            t.expect(mean_node && mean_node->required_topology_query_kinds ==
                std::vector<TopologyQueryKind>{ TopologyQueryKind::Intersection },
                "st.radius_mean node preserves required topology query metadata");
            t.expect(mean_node && mean_node->default_topology_name == builtin_spatiotemporal_topology_name(),
                "st.radius_mean node preserves default topology name");
            t.expect(mean_node && mean_node->topology_binding_param == "topology",
                "st.radius_mean node preserves topology binding param");
        }

        {
            OperatorRegistry registry{};
            {
                OpEntry close{};
                close.spec.name = "core.close";
                close.spec.op_type = OpType::Numeric;
                close.spec.output = ValueType::Num;
                registry.add(std::move(close));
            }
            {
                OpEntry semantic{};
                semantic.spec.name = "semantic.weighted_mean";
                semantic.spec.op_type = OpType::Numeric;
                semantic.spec.inputs = { ValueType::Num };
                semantic.spec.output = ValueType::Num;
                semantic.spec.axis_scope = OpAxisScope::RowWise;
                semantic.spec.required_topology_families = { TopologyFamily::SemanticGraph };
                semantic.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                semantic.spec.default_topology_name = "entity.semantic.graph";
                semantic.spec.topology_binding_param = "topology";
                registry.add(std::move(semantic));
            }

            const std::array<double, 4> close{ 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool runtime_ok = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(runtime_ok, "semantic-topology runtime fixture builds single-series DataView");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "semantic-topology runtime fixture preserves topology catalog sidecar");
            if (!runtime_ok || !bundle || !bundle->topology_catalog) return true;

            TopologySpec semantic_spec{};
            semantic_spec.name = "entity.semantic.graph";
            semantic_spec.family = TopologyFamily::SemanticGraph;
            semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
            semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            semantic_spec.artifact_ref = TopologyArtifactRef{
                "entity.semantic.graph.v1",
                "artifacts/entity.semantic.graph.parquet",
                "parquet",
                "sha256:entity-semantic-v1",
                "python_pandas"
            };
            bundle->topology_catalog->add(semantic_spec);

            ResolvedRuntimeInput runtime{};
            runtime.aligned_bundle = bundle;
            runtime.topology_catalog = bundle->topology_catalog;
            runtime.primary_ref = primary;
            runtime.primary_series_index = 0;
            runtime.run_manifest.preprocess.has_time_axis = true;
            runtime.run_manifest.preprocess.has_spatial_axis = false;
            runtime.run_manifest.preprocess.has_entity_axis = false;
            runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
            };

            const auto report = diagnose_plan_capabilities(spec, registry, runtime);
            const auto* semantic_node = find_plan_node(report, "semantic_mean");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "runtime-aware plan capability rejects declared semantic topology without runtime adapters");
            t.expect(semantic_node && semantic_node->resolved_topology_name == "entity.semantic.graph",
                "runtime-aware semantic topology report preserves resolved named topology");
            t.expect(has_plan_reason(semantic_node, PlanCapabilityReasonCode::TopologyResolutionFailed),
                "runtime-aware semantic topology report cites runtime topology-resolution failure");
            t.expect(has_plan_reason_detail(semantic_node,
                    PlanCapabilityReasonCode::TopologyResolutionFailed,
                    "metadata-only"),
                "runtime-aware semantic topology failure preserves metadata-only bearing detail");

            append_plan_capability_snapshot(report, runtime.run_manifest);
            const auto* semantic_usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
            t.expect(semantic_usage != nullptr,
                "runtime-aware plan capability snapshot records semantic topology usage");
            if (semantic_usage) {
                t.expect(semantic_usage->catalog_present,
                    "runtime-aware plan capability snapshot preserves semantic topology catalog presence");
                t.expect(!semantic_usage->runtime_resolved,
                    "runtime-aware plan capability snapshot preserves failed semantic topology resolution");
                t.expect(semantic_usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                    "runtime-aware plan capability snapshot preserves semantic topology artifact stable id");
            }
        }

        {
            const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
            const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
            const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
            const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
            const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool runtime_ok = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(runtime_ok, "runtime-aware locality-subject fixture builds two-series spatio-temporal DataView");
            if (!runtime_ok || !bundle) return true;

            ResolvedRuntimeInput runtime{};
            runtime.aligned_bundle = bundle;
            runtime.topology_catalog = bundle->topology_catalog;
            runtime.primary_ref = primary;
            runtime.primary_series_index = bundle->find_series_index(primary.series);
            runtime.series_key_to_entity = {
                { "A", "entity.A" },
                { "B", "entity.B" }
            };
            runtime.run_manifest.preprocess.has_time_axis = true;
            runtime.run_manifest.preprocess.has_spatial_axis = true;
            runtime.run_manifest.preprocess.has_entity_axis = true;
            runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    { "radius", 0.2 },
                    { "subject_series", std::string("missing.series") }
                } },
            };

            const auto registry = make_default_registry();
            const auto report = diagnose_plan_capabilities(spec, registry, runtime);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "runtime-aware plan capability rejects unresolved locality subject bindings");
            t.expect(mean_node && has_plan_reason(mean_node, PlanCapabilityReasonCode::LocalitySubjectResolutionFailed),
                "runtime-aware locality-subject failure is reported explicitly");
            t.expect(has_plan_reason_detail(mean_node,
                    PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                    "series not found"),
                "runtime-aware locality-subject failure preserves missing-series detail");

            append_plan_capability_snapshot(report, runtime.run_manifest);
            const auto* invalid_usage = find_topology_usage(
                runtime.run_manifest,
                "mean",
                builtin_spatial_topology_name());
            t.expect(invalid_usage != nullptr,
                "runtime-aware locality-subject snapshot records topology usage");
            if (invalid_usage) {
                t.expect(invalid_usage->locality_subject_kind == "series_key",
                    "runtime-aware locality-subject snapshot preserves bound subject kind");
                t.expect(invalid_usage->locality_subject_value == "missing.series",
                    "runtime-aware locality-subject snapshot preserves unresolved subject value");
                t.expect(invalid_usage->locality_subject_explicit,
                    "runtime-aware locality-subject snapshot preserves explicit binding flag");
                t.expect(invalid_usage->resolved_series_key.empty(),
                    "runtime-aware locality-subject snapshot keeps resolved series empty on failure");
                t.expect(invalid_usage->resolved_entity_key.empty(),
                    "runtime-aware locality-subject snapshot keeps resolved entity empty on failure");
            }

            PlanSpec resolved_spec{};
            resolved_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    { "radius", 0.2 },
                    { "subject_series", std::string("B") }
                } },
            };

            const auto resolved_report = diagnose_plan_capabilities(resolved_spec, registry, runtime);
            const auto* resolved_node = find_plan_node(resolved_report, "mean");
            t.expect(resolved_report.status == PlanCapabilityStatus::Valid,
                "runtime-aware plan capability accepts explicit resolved locality subject bindings");
            t.expect(resolved_node && resolved_node->has_runtime_locality_subject,
                "runtime-aware locality-subject report preserves resolved runtime subject");
            if (resolved_node && resolved_node->has_runtime_locality_subject) {
                t.expect(resolved_node->runtime_locality_subject.series_key == "B",
                    "runtime-aware locality-subject report preserves resolved series key");
                t.expect(resolved_node->runtime_locality_subject.entity_key == "entity.B",
                    "runtime-aware locality-subject report preserves resolved entity key");
            }

            append_plan_capability_snapshot(resolved_report, runtime.run_manifest);
            const auto* resolved_usage = find_topology_usage(
                runtime.run_manifest,
                "mean",
                builtin_spatial_topology_name());
            t.expect(resolved_usage != nullptr,
                "runtime-aware resolved locality-subject snapshot records topology usage");
            if (resolved_usage) {
                t.expect(resolved_usage->locality_subject_kind == "series_key",
                    "runtime-aware resolved locality-subject snapshot preserves subject kind");
                t.expect(resolved_usage->locality_subject_value == "B",
                    "runtime-aware resolved locality-subject snapshot preserves subject value");
                t.expect(resolved_usage->locality_subject_explicit,
                    "runtime-aware resolved locality-subject snapshot preserves explicit binding");
                t.expect(resolved_usage->resolved_series_key == "B",
                    "runtime-aware resolved locality-subject snapshot preserves resolved series key");
                t.expect(resolved_usage->resolved_entity_key == "entity.B",
                    "runtime-aware resolved locality-subject snapshot preserves resolved entity key");
                t.expect(resolved_usage->runtime_resolved,
                    "runtime-aware resolved locality-subject snapshot preserves successful runtime resolution");
            }
        }

        return true;
    }

    bool test_plan_capability_fact_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_facts] fact-only plan/operator capability diagnosis\n";

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "sma", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = false;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* close_node = find_plan_node(report, "c");
            const auto* sma_node = find_plan_node(report, "sma");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "spatial-only facts make temporal-local plan invalid");
            t.expect(report.incompatible_nodes == 1,
                "spatial-only facts count one incompatible temporal-local node");
            t.expect(close_node && close_node->status == PlanCapabilityStatus::Valid,
                "row-wise core.close remains valid on spatial-only facts");
            t.expect(sma_node && sma_node->status == PlanCapabilityStatus::Invalid,
                "ts.sma is invalid on spatial-only facts");
            t.expect(has_plan_reason(sma_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Time),
                "ts.sma failure cites missing time axis");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 4;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "temporal-only facts make spatial locality plan invalid");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Invalid,
                "spatial locality node is invalid when spatial topology is absent");
            t.expect(mean_node && mean_node->resolved_topology_name == builtin_spatial_topology_name(),
                "spatial locality node resolves built-in default topology name in capability report");
            t.expect(std::find(report.used_topology_names.begin(),
                    report.used_topology_names.end(),
                    std::string(builtin_spatial_topology_name())) != report.used_topology_names.end(),
                "plan capability report records used topology names");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Spatial),
                "spatial locality failure still cites missing spatial axis");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredTopologyFamily),
                "spatial locality failure cites missing required topology family");
            t.expect(has_plan_reason_detail(mean_node,
                    PlanCapabilityReasonCode::MissingRequiredTopologyFamily,
                    "provide spatial neighborhood/index schema"),
                "spatial locality topology-family failure carries providable spatial hint");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingNamedTopology),
                "spatial locality failure cites missing named topology when built-in spatial topology is unavailable");

            RunManifest manifest{};
            append_plan_capability_snapshot(report, manifest);
            const auto* usage = find_topology_usage(manifest, "mean", builtin_spatial_topology_name());
            t.expect(usage != nullptr,
                "plan capability snapshot records topology usage for unresolved built-in locality");
            if (usage) {
                t.expect(!usage->catalog_present,
                    "plan capability snapshot preserves missing-catalog topology usage state");
                t.expect(!usage->runtime_resolved,
                    "plan capability snapshot preserves unresolved topology state");
            }
            const auto* outcome = find_capability_outcome(manifest, "plan_node", "mean");
            t.expect(outcome != nullptr,
                "plan capability snapshot records plan-node outcome");
            if (outcome) {
                t.expect(outcome->status == "invalid",
                    "plan capability snapshot preserves plan-node invalid status");
            }
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "left", "core.close", {}, {} },
                NodeSpec{ "right", "core.close", {}, {} },
                NodeSpec{ "spread", "diff", { "left", "right" }, {} },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 10;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* spread_node = find_plan_node(report, "spread");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "single-series facts make cross-series plan invalid");
            t.expect(spread_node && spread_node->op_name == "ms.diff",
                "plan capability diagnosis normalizes runtime op aliases");
            t.expect(spread_node && spread_node->status == PlanCapabilityStatus::Invalid,
                "cross-series node is invalid on single-series facts");
            t.expect(has_plan_reason(spread_node, PlanCapabilityReasonCode::InsufficientSeriesCount),
                "cross-series node failure cites multiple-series requirement");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* dup = find_plan_node(report, "dup");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "missing or duplicate node ids make plan invalid");
            t.expect(any_plan_node_has_reason(report, PlanCapabilityReasonCode::MissingNodeId),
                "missing-id node is reported explicitly");
            t.expect(dup && has_plan_reason(dup, PlanCapabilityReasonCode::DuplicateNodeId),
                "duplicate node id is reported explicitly");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "st.radius_mean", { "c" }, {
                    {"last_n", static_cast<std::int64_t>(3)},
                    {"radius", 1.5}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 2;
            facts.row_count = 5;
            facts.has_time_axis = true;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Valid,
                "spatiotemporal facts make st.radius_mean plan valid");
            t.expect(report.incompatible_nodes == 0,
                "spatiotemporal facts produce no incompatible nodes");
            t.expect(std::find(report.used_axis_scopes.begin(),
                    report.used_axis_scopes.end(),
                    OpAxisScope::SpatioTemporal) != report.used_axis_scopes.end(),
                "spatiotemporal scope is recorded in used_axis_scopes");
            t.expect(std::find(report.used_topology_families.begin(),
                    report.used_topology_families.end(),
                    TopologyFamily::SpatioTemporalComposite) != report.used_topology_families.end(),
                "spatiotemporal topology family is recorded in used_topology_families");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Valid,
                "st.radius_mean node is valid on spatiotemporal facts");
            t.expect(mean_node && mean_node->required_topology_families ==
                std::vector<TopologyFamily>{ TopologyFamily::SpatioTemporalComposite },
                "st.radius_mean node preserves required topology family metadata");
            t.expect(mean_node && mean_node->required_topology_query_kinds ==
                std::vector<TopologyQueryKind>{ TopologyQueryKind::Intersection },
                "st.radius_mean node preserves required topology query metadata");
            t.expect(mean_node && mean_node->default_topology_name == builtin_spatiotemporal_topology_name(),
                "st.radius_mean node preserves default topology name");
            t.expect(mean_node && mean_node->topology_binding_param == "topology",
                "st.radius_mean node preserves topology binding param");
        }

        return true;
    }

    bool test_plan_capability_runtime_binding_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_binding] runtime-aware plan binding diagnosis\n";

        const std::array<double, 3> close{ 10.0, 11.0, 12.0 };
        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_single_series_dataview_from_close(
            close,
            bundle,
            primary,
            static_cast<TimeKey>(20240101));
        t.expect(runtime_ok, "runtime-aware plan capability fixture builds single-series DataView");
        if (!runtime_ok) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.primary_ref = primary;
        runtime.primary_series_index = 0;
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = false;
        runtime.run_manifest.preprocess.has_entity_axis = false;

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "other_close", "core.ref", {}, { {"ref", std::string("other:close")} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
        };
        spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "other_close", "zero" }, {} },
        };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "missing_exit";

        const auto report = diagnose_plan_capabilities(spec, runtime);
        const auto* other_close = find_plan_node(report, "other_close");
        const auto* exit_binding = find_plan_binding(report, "exit_long");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware report is invalid for missing series binding");
        t.expect(report.incompatible_nodes == 1,
            "runtime-aware report counts invalid bound ref node");
        t.expect(report.incompatible_bindings == 1,
            "runtime-aware report counts invalid strategy binding target");
        t.expect(other_close && has_plan_reason(other_close, PlanCapabilityReasonCode::MissingRuntimeSeries),
            "core.ref failure cites missing runtime series");
        t.expect(exit_binding && has_binding_reason(exit_binding, PlanCapabilityReasonCode::InvalidBindingTarget),
            "binding failure cites invalid strategy binding target");

        return true;
    }

    bool test_plan_capability_runtime_topology_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_topology] runtime-aware plan topology diagnosis\n";

        OperatorRegistry registry{};
        {
            OpEntry close{};
            close.spec.name = "core.close";
            close.spec.op_type = OpType::Numeric;
            close.spec.output = ValueType::Num;
            registry.add(std::move(close));
        }
        {
            OpEntry semantic{};
            semantic.spec.name = "semantic.weighted_mean";
            semantic.spec.op_type = OpType::Numeric;
            semantic.spec.inputs = { ValueType::Num };
            semantic.spec.output = ValueType::Num;
            semantic.spec.axis_scope = OpAxisScope::RowWise;
            semantic.spec.required_topology_families = { TopologyFamily::SemanticGraph };
            semantic.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            semantic.spec.default_topology_name = "entity.semantic.graph";
            semantic.spec.topology_binding_param = "topology";
            registry.add(std::move(semantic));
        }

        const std::array<double, 4> close{ 10.0, 11.0, 12.0, 13.0 };
        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_single_series_dataview_from_close(
            close,
            bundle,
            primary,
            static_cast<TimeKey>(20240101));
        t.expect(runtime_ok, "semantic-topology runtime fixture builds single-series DataView");
        t.expect(bundle && bundle->topology_catalog != nullptr,
            "semantic-topology runtime fixture preserves topology catalog sidecar");
        if (!runtime_ok || !bundle || !bundle->topology_catalog) return true;

        TopologySpec semantic_spec{};
        semantic_spec.name = "entity.semantic.graph";
        semantic_spec.family = TopologyFamily::SemanticGraph;
        semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
        semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
        semantic_spec.artifact_ref = TopologyArtifactRef{
            "entity.semantic.graph.v1",
            "artifacts/entity.semantic.graph.parquet",
            "parquet",
            "sha256:entity-semantic-v1",
            "python_pandas"
        };
        bundle->topology_catalog->add(semantic_spec);

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.primary_ref = primary;
        runtime.primary_series_index = 0;
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = false;
        runtime.run_manifest.preprocess.has_entity_axis = false;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
        };

        const auto report = diagnose_plan_capabilities(spec, registry, runtime);
        const auto* semantic_node = find_plan_node(report, "semantic_mean");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects declared semantic topology without runtime adapters");
        t.expect(semantic_node && semantic_node->resolved_topology_name == "entity.semantic.graph",
            "runtime-aware semantic topology report preserves resolved named topology");
        t.expect(has_plan_reason(semantic_node, PlanCapabilityReasonCode::TopologyResolutionFailed),
            "runtime-aware semantic topology report cites runtime topology-resolution failure");
        t.expect(has_plan_reason_detail(semantic_node,
                PlanCapabilityReasonCode::TopologyResolutionFailed,
                "metadata-only"),
            "runtime-aware semantic topology failure preserves metadata-only bearing detail");

        append_plan_capability_snapshot(report, runtime.run_manifest);
        const auto* semantic_usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
        t.expect(semantic_usage != nullptr,
            "runtime-aware plan capability snapshot records semantic topology usage");
        if (semantic_usage) {
            t.expect(semantic_usage->catalog_present,
                "runtime-aware plan capability snapshot preserves semantic topology catalog presence");
            t.expect(!semantic_usage->runtime_resolved,
                "runtime-aware plan capability snapshot preserves failed semantic topology resolution");
            t.expect(semantic_usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                "runtime-aware plan capability snapshot preserves semantic topology artifact stable id");
        }

        return true;
    }

    bool test_plan_capability_runtime_locality_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_locality] runtime-aware locality-subject diagnosis\n";

        const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
        const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "runtime-aware locality-subject fixture builds two-series spatio-temporal DataView");
        if (!runtime_ok || !bundle) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.primary_ref = primary;
        runtime.primary_series_index = bundle->find_series_index(primary.series);
        runtime.series_key_to_entity = {
            { "A", "entity.A" },
            { "B", "entity.B" }
        };
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = true;
        runtime.run_manifest.preprocess.has_entity_axis = true;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                { "radius", 0.2 },
                { "subject_series", std::string("missing.series") }
            } },
        };

        const auto registry = make_default_registry();
        const auto report = diagnose_plan_capabilities(spec, registry, runtime);
        const auto* mean_node = find_plan_node(report, "mean");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects unresolved locality subject bindings");
        t.expect(mean_node && has_plan_reason(mean_node, PlanCapabilityReasonCode::LocalitySubjectResolutionFailed),
            "runtime-aware locality-subject failure is reported explicitly");
        t.expect(has_plan_reason_detail(mean_node,
                PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                "series not found"),
            "runtime-aware locality-subject failure preserves missing-series detail");

        append_plan_capability_snapshot(report, runtime.run_manifest);
        const auto* invalid_usage = find_topology_usage(
            runtime.run_manifest,
            "mean",
            builtin_spatial_topology_name());
        t.expect(invalid_usage != nullptr,
            "runtime-aware locality-subject snapshot records topology usage");
        if (invalid_usage) {
            t.expect(invalid_usage->locality_subject_kind == "series_key",
                "runtime-aware locality-subject snapshot preserves bound subject kind");
            t.expect(invalid_usage->locality_subject_value == "missing.series",
                "runtime-aware locality-subject snapshot preserves unresolved subject value");
            t.expect(invalid_usage->locality_subject_explicit,
                "runtime-aware locality-subject snapshot preserves explicit binding flag");
            t.expect(invalid_usage->resolved_series_key.empty(),
                "runtime-aware locality-subject snapshot keeps resolved series empty on failure");
            t.expect(invalid_usage->resolved_entity_key.empty(),
                "runtime-aware locality-subject snapshot keeps resolved entity empty on failure");
        }

        PlanSpec resolved_spec{};
        resolved_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                { "radius", 0.2 },
                { "subject_series", std::string("B") }
            } },
        };

        const auto resolved_report = diagnose_plan_capabilities(resolved_spec, registry, runtime);
        const auto* resolved_node = find_plan_node(resolved_report, "mean");
        t.expect(resolved_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts explicit resolved locality subject bindings");
        t.expect(resolved_node && resolved_node->has_runtime_locality_subject,
            "runtime-aware locality-subject report preserves resolved runtime subject");
        if (resolved_node && resolved_node->has_runtime_locality_subject) {
            t.expect(resolved_node->runtime_locality_subject.series_key == "B",
                "runtime-aware locality-subject report preserves resolved series key");
            t.expect(resolved_node->runtime_locality_subject.entity_key == "entity.B",
                "runtime-aware locality-subject report preserves resolved entity key");
        }

        append_plan_capability_snapshot(resolved_report, runtime.run_manifest);
        const auto* resolved_usage = find_topology_usage(
            runtime.run_manifest,
            "mean",
            builtin_spatial_topology_name());
        t.expect(resolved_usage != nullptr,
            "runtime-aware resolved locality-subject snapshot records topology usage");
        if (resolved_usage) {
            t.expect(resolved_usage->locality_subject_kind == "series_key",
                "runtime-aware resolved locality-subject snapshot preserves subject kind");
            t.expect(resolved_usage->locality_subject_value == "B",
                "runtime-aware resolved locality-subject snapshot preserves subject value");
            t.expect(resolved_usage->locality_subject_explicit,
                "runtime-aware resolved locality-subject snapshot preserves explicit binding");
            t.expect(resolved_usage->resolved_series_key == "B",
                "runtime-aware resolved locality-subject snapshot preserves resolved series key");
            t.expect(resolved_usage->resolved_entity_key == "entity.B",
                "runtime-aware resolved locality-subject snapshot preserves resolved entity key");
            t.expect(resolved_usage->runtime_resolved,
                "runtime-aware resolved locality-subject snapshot preserves successful runtime resolution");
        }

        return true;
    }

    bool test_multi_entity_ref_binding_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_ref_binding_overload_rejection] multi-entity runtime binding overload rejection\n";

        const std::vector<double> primary_close = { 1.0, 2.0, 3.0 };
        const std::vector<double> secondary_close = { 4.0, 5.0, 6.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runtime-binding fixture builds two-series spatiotemporal DataView");
        if (!runtime_ok || !bundle) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.series_key_to_entity = {
            { "A", "entity.A" },
            { "B", "entity.B" }
        };
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = true;
        runtime.run_manifest.preprocess.has_entity_axis = true;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec ref_spec{};
        ref_spec.numeric_nodes = {
            NodeSpec{ "close_leaf", "core.ref", {}, { {"ref", std::string("close")} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
        };
        ref_spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "close_leaf", "zero" }, {} },
        };
        ref_spec.bind.entry_long = "entry";

        const auto ambiguous_ref_report = diagnose_plan_capabilities(ref_spec, runtime);
        const auto* close_leaf = find_plan_node(ambiguous_ref_report, "close_leaf");
        t.expect(ambiguous_ref_report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects unqualified refs on multi-series runtime without explicit primary");
        t.expect(close_leaf && has_plan_reason(close_leaf, PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries),
            "unqualified multi-series ref failure cites ambiguous runtime default series");
        t.expect(has_plan_reason_detail(close_leaf,
                PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries,
                "explicit primary/default series"),
            "unqualified multi-series ref failure preserves explicit target requirement detail");

        const auto registry = make_default_registry();
        const auto ambiguous_locality_spec = make_spatial_radius_mean_plan(
            "ambiguous_locality",
            0.2,
            0.0,
            std::string(builtin_spatial_topology_name()));
        const auto ambiguous_locality_report =
            diagnose_plan_capabilities(ambiguous_locality_spec, registry, runtime);
        const auto* mean_node = find_plan_node(ambiguous_locality_report, "mean");
        t.expect(ambiguous_locality_report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects implicit primary locality binding on multi-series runtime without explicit target");
        t.expect(mean_node && has_plan_reason(mean_node, PlanCapabilityReasonCode::LocalitySubjectResolutionFailed),
            "implicit multi-series locality binding failure cites locality-subject resolution");
        t.expect(has_plan_reason_detail(mean_node,
                PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                "explicit compatibility primary series"),
            "implicit multi-series locality binding failure preserves explicit target requirement detail");

        runtime.primary_ref = primary;
        runtime.primary_series_index = bundle->find_series_index(primary.series);

        const auto resolved_ref_report = diagnose_plan_capabilities(ref_spec, runtime);
        const auto* resolved_close_leaf = find_plan_node(resolved_ref_report, "close_leaf");
        t.expect(resolved_ref_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts unqualified refs once explicit primary series is present");
        t.expect(resolved_close_leaf && resolved_close_leaf->status == PlanCapabilityStatus::Valid,
            "runtime-aware unqualified ref node becomes valid once explicit primary series is present");

        const auto resolved_locality_report =
            diagnose_plan_capabilities(ambiguous_locality_spec, registry, runtime);
        const auto* resolved_mean_node = find_plan_node(resolved_locality_report, "mean");
        t.expect(resolved_locality_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts implicit primary locality binding once explicit primary series is present");
        t.expect(resolved_mean_node && resolved_mean_node->has_runtime_locality_subject,
            "runtime-aware locality report preserves resolved primary locality subject once target is explicit");
        if (resolved_mean_node && resolved_mean_node->has_runtime_locality_subject) {
            t.expect(resolved_mean_node->runtime_locality_subject.series_key == "A",
                "runtime-aware locality resolution binds implicit primary subject to explicit primary series");
            t.expect(resolved_mean_node->runtime_locality_subject.entity_key == "entity.A",
                "runtime-aware locality resolution preserves explicit primary entity provenance");
        }

        return true;
    }

    bool test_multi_entity_runner_design_load_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_runner_design_loads] neutral multi-entity runner design loads\n";

        const std::vector<double> primary_close = { 99.0, 110.0, 90.0, 120.0, 80.0, 130.0 };
        const std::vector<double> secondary_close = { 99.0, 101.0, 102.0, 103.0, 104.0, 105.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2, 10.3, 10.4, 10.5 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runner fixture builds two-series runtime");
        if (!runtime_ok || !bundle) return true;

        const DataView dv(*bundle);
        const PlanSpec spec = make_multi_entity_runner_contract_plan();
        PlanSweepRuntimeOptions opt{};

        {
            PanelRankingSpec ranking{};
            ranking.metric = PanelRankingMetric::Pnl;
            ranking.dd_weight = 9.0;

            const auto result = run_ranked_multi_entity_custom_planspec_over_dataview(
                dv,
                spec,
                opt,
                primary.col,
                2,
                ranking);

            t.expect(result.ranking.metric == PanelRankingMetric::Pnl,
                "multi-entity runner preserves explicit pnl ranking metric");
            t.expect(result.topk.size() == 2, "multi-entity runner keeps requested top-k when both entities succeed");
            t.expect(result.failures.empty(), "multi-entity runner design-load fixture has no failures");
            const PanelReport report = build_panel_report(result);
            t.expect(report.resolved_objective.primary.name == "pnl",
                "panel report resolves pnl ranking objective");
            t.expect(report.top_rows.size() == result.topk.size(),
                "panel report preserves ranked row count");
            if (result.topk.size() == 2) {
                t.expect(result.topk[0].entity_key == "entity.A" || result.topk[1].entity_key == "entity.A",
                    "multi-entity runner preserves entity identity distinct from series keys");
                const bool pnl_metric_ok =
                    result.topk[0].ranking_metric == PanelRankingMetric::Pnl &&
                    result.topk[1].ranking_metric == PanelRankingMetric::Pnl;
                const bool pnl_score_ok =
                    nearly_equal(result.topk[0].ranking_score, result.topk[0].metrics.pnl) &&
                    nearly_equal(result.topk[1].ranking_score, result.topk[1].metrics.pnl);
                t.expect(pnl_metric_ok, "multi-entity runner records pnl metric on each ranked row");
                t.expect(pnl_score_ok, "multi-entity runner scores pnl ranking directly from pnl");
                t.expect(report.top_rows[0].run.objective.rankability == RunRankabilityState::Rankable &&
                        report.top_rows[1].run.objective.rankability == RunRankabilityState::Rankable,
                    "panel report classifies pnl-ranked rows as rankable");
            }
        }

        {
            PanelRankingSpec ranking{};
            ranking.metric = PanelRankingMetric::PnlDd;
            ranking.dd_weight = 0.5;

            const auto result = run_ranked_multi_entity_custom_planspec_over_dataview(
                dv,
                spec,
                opt,
                primary.col,
                2,
                ranking);

            t.expect(result.ranking.metric == PanelRankingMetric::PnlDd,
                "multi-entity runner preserves explicit pnl_dd ranking metric");
            t.expect(nearly_equal(result.ranking.dd_weight, 0.5),
                "multi-entity runner preserves explicit pnl_dd drawdown weight");
            const PanelReport report = build_panel_report(result);
            t.expect(report.resolved_objective.penalties.size() == 1,
                "panel report preserves pnl_dd penalty structure");
            if (result.topk.size() == 2) {
                const bool pnl_dd_metric_ok =
                    result.topk[0].ranking_metric == PanelRankingMetric::PnlDd &&
                    result.topk[1].ranking_metric == PanelRankingMetric::PnlDd;
                const bool pnl_dd_score_ok =
                    nearly_equal(
                        result.topk[0].ranking_score,
                        result.topk[0].metrics.pnl - 0.5 * result.topk[0].metrics.max_drawdown) &&
                    nearly_equal(
                        result.topk[1].ranking_score,
                        result.topk[1].metrics.pnl - 0.5 * result.topk[1].metrics.max_drawdown);
                t.expect(pnl_dd_metric_ok, "multi-entity runner records pnl_dd metric on each ranked row");
                t.expect(pnl_dd_score_ok, "multi-entity runner scores pnl_dd ranking from explicit drawdown-weighted formula");
                t.expect(nearly_equal(report.top_rows[0].run.objective.score, result.topk[0].ranking_score) &&
                        nearly_equal(report.top_rows[1].run.objective.score, result.topk[1].ranking_score),
                    "panel report preserves per-row resolved objective scores");
            }
        }

        return true;
    }

    bool test_multi_entity_runner_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_runner_overload_rejection] neutral multi-entity runner overload rejection\n";

        const std::vector<double> primary_close = { 99.0, 110.0, 90.0, 120.0 };
        const std::vector<double> secondary_close = { 99.0, 101.0, 102.0, 103.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2, 0.3 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2, 10.3 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runner overload fixture builds two-series runtime");
        if (!runtime_ok || !bundle) return true;

        const DataView dv(*bundle);
        const PlanSpec bad_spec = make_spatial_radius_mean_plan(
            "multi_entity_runner_bad_subject",
            0.2,
            0.0,
            std::string(builtin_spatial_topology_name()),
            "missing.series");
        PanelRankingSpec ranking{};
        ranking.metric = PanelRankingMetric::PnlDd;
        ranking.dd_weight = 0.25;

        const auto invalid_col_result = run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            bad_spec,
            PlanSweepRuntimeOptions{},
            ColumnId{ 9999u },
            2,
            ranking);
        t.expect(invalid_col_result.topk.empty(),
            "multi-entity runner keeps top-k empty when primary column is missing for every series");
        t.expect(invalid_col_result.failures.size() == 2,
            "multi-entity runner records a failure per series when primary column is missing");
        const PanelReport invalid_col_report = build_panel_report(invalid_col_result);
        t.expect(invalid_col_report.failures.size() == invalid_col_result.failures.size(),
            "panel report preserves missing-column failure count");
        if (invalid_col_result.failures.size() == 2) {
            t.expect(invalid_col_result.failures[0].series_key == "A" ||
                    invalid_col_result.failures[1].series_key == "A",
                "multi-entity runner failure records preserve failing series keys");
            t.expect(invalid_col_result.failures[0].entity_key == "entity.A" ||
                    invalid_col_result.failures[1].entity_key == "entity.A",
                "multi-entity runner failure records preserve failing entity keys");
            t.expect(invalid_col_report.failures[0].kind == PanelFailureKind::MissingPrimaryColumn &&
                    invalid_col_report.failures[1].kind == PanelFailureKind::MissingPrimaryColumn,
                "panel report classifies missing-primary-column failures explicitly");
        }

        const auto bad_subject_result = run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            bad_spec,
            PlanSweepRuntimeOptions{},
            primary.col,
            2,
            ranking);
        t.expect(bad_subject_result.topk.empty(),
            "multi-entity runner keeps top-k empty when custom execution fails for every series");
        t.expect(bad_subject_result.failures.size() == 2,
            "multi-entity runner records execution failure per series");
        const PanelReport bad_subject_report = build_panel_report(bad_subject_result);
        if (bad_subject_result.failures.size() == 2) {
            const bool reason_ok =
                bad_subject_result.failures[0].reason.find("missing.series") != std::string::npos &&
                bad_subject_result.failures[1].reason.find("missing.series") != std::string::npos;
            t.expect(reason_ok,
                "multi-entity runner preserves per-series execution failure reason text");
            t.expect(bad_subject_report.failures[0].kind == PanelFailureKind::ExecutionFailed &&
                    bad_subject_report.failures[1].kind == PanelFailureKind::ExecutionFailed,
                "panel report classifies execution failures explicitly");
        }

        return true;
    }

    bool test_planspec_compile_determinism(ContractAsserts& t) {
        t.out << "[compile_determinism] PlanSpec compile determinism (graph + param binding)\n";

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_compile_det";
        spec.trade_mode = TradeMode::LongOnly;
        spec.params["n_fast"] = static_cast<std::int64_t>(3);
        spec.params["thr"] = 101.0;

        NodeSpec n_sma{};
        n_sma.id = "sma";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = ParamRef{ "n_fast" };

        NodeSpec n_c{};
        n_c.id = "c";
        n_c.op = "core.close";

        NodeSpec n_th{};
        n_th.id = "th";
        n_th.op = "core.const";
        n_th.params["value"] = ParamRef{ "thr" };

        spec.numeric_nodes = { n_sma, n_c, n_th };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "sma", "th" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "sma", "th" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSpec perm = spec;
        perm.numeric_nodes = { n_th, n_sma, n_c };
        perm.bool_nodes = { b_exit, b_entry };
        perm.params.clear();
        perm.params["thr"] = 101.0;
        perm.params["n_fast"] = static_cast<std::int64_t>(3);

        const auto registry = make_default_registry();
        CompileEnv env{};
        CompileResult c1{};
        CompileResult c2{};
        CompileResult c3{};
        std::string err1;
        std::string err2;
        std::string err3;
        const bool ok1 = compile_plan(spec, registry, env, c1, err1);
        const bool ok2 = compile_plan(spec, registry, env, c2, err2);
        const bool ok3 = compile_plan(perm, registry, env, c3, err3);

        t.expect(ok1, "compile pass #1 succeeds");
        t.expect(ok2, "compile pass #2 succeeds");
        t.expect(ok3, "compile pass #3 succeeds with reordered declarations");
        if (!ok1 || !ok2 || !ok3) {
            if (!ok1) t.out << "    compile#1 error: " << err1 << "\n";
            if (!ok2) t.out << "    compile#2 error: " << err2 << "\n";
            if (!ok3) t.out << "    compile#3 error: " << err3 << "\n";
            return true;
        }

        const auto gsig1 = canonical_graph_signature(spec);
        const auto gsig2 = canonical_graph_signature(perm);
        t.expect(gsig1 == gsig2, "canonical graph signature is stable across declaration order");

        std::string rsig1;
        std::string rsig2;
        std::string rerr;
        const bool rok1 = build_resolved_binding_signature(spec, registry, rsig1, rerr);
        const bool rok2 = build_resolved_binding_signature(perm, registry, rsig2, rerr);
        t.expect(rok1, "resolved binding signature build succeeds for base PlanSpec");
        t.expect(rok2, "resolved binding signature build succeeds for reordered PlanSpec");
        if (rok1 && rok2) {
            t.expect(rsig1 == rsig2, "resolved parameter bindings are stable across declaration order");
        }

        t.expect(c1.plan.name == c2.plan.name, "compiled plan name is stable");
        t.expect(c1.plan.mode == c2.plan.mode, "compiled trade mode is stable");
        t.expect(c1.exec.eval_mode == c2.exec.eval_mode, "compiled exec settings are stable");
        t.expect(c1.plan.first_valid_index == c2.plan.first_valid_index, "compiled warmup boundary is stable");
        t.expect(c1.plan.first_valid_index == 2, "param-ref binding resolves n_fast=3 into warmup=2");
        t.expect(c3.plan.first_valid_index == c1.plan.first_valid_index, "reordered PlanSpec compile keeps warmup boundary");

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "compile determinism test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        const StrategyExecutor ex{};
        const auto r1 = ex.run(dv, primary, c1.plan, c1.exec);
        const auto r2 = ex.run(dv, primary, c2.plan, c2.exec);
        const auto r3 = ex.run(dv, primary, c3.plan, c3.exec);
        t.expect(backtest_equal(r1, r2), "compiled plans execute identically on fixed input");
        t.expect(backtest_equal(r1, r3), "reordered PlanSpec executes identically on fixed input");
        return true;
    }

    std::vector<double> seeded_close_series(std::uint32_t seed, std::size_t n) {
        std::vector<double> out;
        out.reserve(n);
        std::uint32_t state = seed;
        for (std::size_t i = 0; i < n; ++i) {
            state = state * 1664525u + 1013904223u;
            out.push_back(100.0 + static_cast<double>(state % 1000u) / 100.0);
        }
        return out;
    }

    bool test_executor_determinism(ContractAsserts& t) {
        t.out << "[executor_determinism] executor determinism (fixed seed -> stable hash/result)\n";

        constexpr std::uint32_t kSeed = 424242u;
        const auto close = seeded_close_series(kSeed, 6);

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "executor determinism test creates DataView input");
        if (!okdv || !bundle) return true;

        StrategyPlan plan{};
        plan.name = "executor_determinism_contract";
        plan.mode = TradeMode::LongOnly;
        auto storage = std::make_shared<StrategyPlanStorage>();
        auto env = make_compile_env_storage();
        env->bool_leaf_values["entry_long_fixed"] = BoolBuffer(close.size(), 0u);
        env->bool_leaf_values["exit_long_fixed"] = BoolBuffer(close.size(), 0u);
        env->bool_leaf_values["allow_all"] = BoolBuffer(close.size(), 1u);
        if (close.size() > 1) env->bool_leaf_values["entry_long_fixed"][1] = 1u;
        if (close.size() > 3) env->bool_leaf_values["exit_long_fixed"][3] = 1u;

        storage->env = env;
        storage->program.bool_slot_count = 3;
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("entry_long_fixed")} },
            0
            });
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("exit_long_fixed")} },
            1
            });
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("allow_all")} },
            2
            });
        storage->program.entry_long_slot = 0;
        storage->program.exit_long_slot = 1;
        storage->program.allow_trade_slot = 2;
        plan.storage = std::move(storage);

        ExecSettings exec{};
        exec.eval_mode = BarEvalMode::SameBar;
        exec.fill_timing = FillTiming::SameBarClose;
        exec.close_out_at_end = true;
        exec.exits_first = true;
        exec.allow_same_bar_reversal = false;
        exec.costs = ExecCosts{};

        const DataView dv(*bundle);
        const StrategyExecutor ex{};
        const auto r1 = ex.run(dv, primary, plan, exec);
        const auto r2 = ex.run(dv, primary, plan, exec);
        t.expect(backtest_equal(r1, r2), "repeated executor runs are identical");

        constexpr std::uint64_t kExpectedHash = 0xb05a681b1b9abb69ULL;
        constexpr double kExpectedPnl = 3.9836766420520737;
        t.expect(r1.trades == 1, "executor fixed-signal scenario closes exactly one trade");
        t.expect(r1.long_trades == 1 && r1.short_trades == 0, "executor fixed-signal scenario trade sides are stable");
        t.expect(r1.trade_hash == kExpectedHash, "executor trade hash matches locked contract");
        t.expect(nearly_equal(r1.pnl, kExpectedPnl), "executor pnl matches locked contract");
        t.expect(r1.has_best_start_date, "executor best_start_date presence flag is stable");
        t.expect(r1.best_start_date == static_cast<TimeKey>(20240102), "executor best_start_date is stable");

        return true;
    }

    bool test_locality_design_load_contracts(ContractAsserts& t) {
        t.out << "[locality_design_loads] current locality execution design loads\n";

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatial locality design-load fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const PlanSpec spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_spatial",
                    0.2,
                    16.0,
                    std::string(builtin_spatial_topology_name()));

                const DataView dv(*bundle);
                ProgramSignalOutputs signals{};
                std::string err;
                const bool eval_ok = compile_and_eval_plan_signals(spec, dv, primary, signals, err);
                t.expect(eval_ok, "spatial.radius_mean design-load plan compiles and evaluates");
                t.expect(err.empty(), "spatial.radius_mean design-load evaluation has no error");
                t.expect(signals.entry_long.size() == close.size(),
                    "spatial.radius_mean design-load evaluation produces row-domain-sized entry buffer");
                if (signals.entry_long.size() == close.size()) {
                    t.expect(signals.entry_long[0] == kBoolFalse,
                        "spatial.radius_mean design load keeps first row below threshold when local mean is 15");
                    t.expect(signals.entry_long[1] == kBoolFalse,
                        "spatial.radius_mean design load keeps second row below threshold when local mean is 15");
                    t.expect(signals.entry_long[2] == kBoolTrue,
                        "spatial.radius_mean design load raises final row when isolated local mean exceeds threshold");
                }
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0, 10.1 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatiotemporal locality design-load fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                const PlanSpec spec = make_spatiotemporal_radius_mean_plan(
                    "contract_locality_design_spatiotemporal",
                    2,
                    0.2,
                    25.0,
                    std::string(builtin_spatiotemporal_topology_name()));

                const DataView dv(*bundle);
                ProgramSignalOutputs signals{};
                std::string err;
                const bool eval_ok = compile_and_eval_plan_signals(spec, dv, primary, signals, err);
                t.expect(eval_ok, "st.radius_mean design-load plan compiles and evaluates");
                t.expect(err.empty(), "st.radius_mean design-load evaluation has no error");
                t.expect(signals.entry_long.size() == close.size(),
                    "st.radius_mean design-load evaluation produces row-domain-sized entry buffer");
                if (signals.entry_long.size() == close.size()) {
                    t.expect(signals.entry_long[0] == kBoolFalse,
                        "st.radius_mean design load keeps warmup row false");
                    t.expect(signals.entry_long[1] == kBoolFalse,
                        "st.radius_mean design load keeps second row below threshold when local mean is 15");
                    t.expect(signals.entry_long[2] == kBoolTrue,
                        "st.radius_mean design load raises third row when local mean exceeds threshold");
                    t.expect(signals.entry_long[3] == kBoolTrue,
                        "st.radius_mean design load raises final row when local mean exceeds threshold");
                }
            }
        }

        {
            const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
            const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
            const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
            const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
            const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(okdv && bundle != nullptr,
                "locality-subject design-load fixture creates multi-series DataView input");
            if (okdv && bundle) {
                const PlanSpec default_spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_default_subject",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()));
                const PlanSpec subject_spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_explicit_subject",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));

                const DataView dv(*bundle);
                ProgramSignalOutputs default_signals{};
                ProgramSignalOutputs subject_signals{};
                std::string err;
                const bool default_ok = compile_and_eval_plan_signals(default_spec, dv, primary, default_signals, err);
                t.expect(default_ok, "default locality-subject design-load plan compiles and evaluates");
                t.expect(err.empty(), "default locality-subject design-load evaluation has no error");
                err.clear();
                const bool subject_ok = compile_and_eval_plan_signals(subject_spec, dv, primary, subject_signals, err);
                t.expect(subject_ok, "explicit locality-subject design-load plan compiles and evaluates");
                t.expect(err.empty(), "explicit locality-subject design-load evaluation has no error");
                t.expect(default_signals.entry_long.size() == primary_close.size() &&
                        subject_signals.entry_long.size() == primary_close.size(),
                    "locality-subject design-load evaluation produces row-domain-sized buffers for both bindings");
                if (default_signals.entry_long.size() == primary_close.size() &&
                    subject_signals.entry_long.size() == primary_close.size()) {
                    t.expect(default_signals.entry_long[0] == kBoolTrue,
                        "default locality subject uses primary-series locality by default");
                    t.expect(subject_signals.entry_long[0] == kBoolFalse,
                        "explicit subject_series redirects locality to the alternate series");
                }
            }
        }

        return true;
    }

    bool test_locality_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[locality_overload_rejection] current locality overload rejection contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "locality overload fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                auto spec = make_spatial_radius_mean_plan(
                    "contract_locality_overload_missing_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects unavailable named topology for spatial.radius_mean");
                t.expect(log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "locality overload preserves explicit missing-topology reason");
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "locality bad-subject fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                auto spec = make_spatial_radius_mean_plan(
                    "contract_locality_overload_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects unresolved locality subject for spatial.radius_mean");
                const std::string msg = log.str();
                t.expect(msg.find("locality subject could not be resolved") != std::string::npos,
                    "locality overload preserves explicit locality-subject resolution failure");
                t.expect(msg.find("series not found") != std::string::npos,
                    "locality overload preserves missing-series detail");
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "locality topology-family overload fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                auto spec = make_spatiotemporal_radius_mean_plan(
                    "contract_locality_overload_topology_family",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects topology family mismatch for st.radius_mean");
                const std::string msg = log.str();
                t.expect(msg.find("has family 'temporal_window'") != std::string::npos,
                    "locality overload reports actual mismatched topology family");
                t.expect(msg.find("spatiotemporal_composite") != std::string::npos,
                    "locality overload reports required topology family");
            }
        }

        return true;
    }

    bool test_locality_metadata_preservation_contracts(ContractAsserts& t) {
        t.out << "[locality_metadata_preservation] current locality metadata preservation contracts\n";

        {
            OperatorRegistry registry{};
            {
                OpEntry close{};
                close.spec.name = "core.close";
                close.spec.op_type = OpType::Numeric;
                close.spec.output = ValueType::Num;
                registry.add(std::move(close));
            }
            {
                OpEntry semantic{};
                semantic.spec.name = "semantic.weighted_mean";
                semantic.spec.op_type = OpType::Numeric;
                semantic.spec.inputs = { ValueType::Num };
                semantic.spec.output = ValueType::Num;
                semantic.spec.axis_scope = OpAxisScope::RowWise;
                semantic.spec.required_topology_families = { TopologyFamily::SemanticGraph };
                semantic.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                semantic.spec.default_topology_name = "entity.semantic.graph";
                semantic.spec.topology_binding_param = "topology";
                registry.add(std::move(semantic));
            }

            const std::array<double, 4> close{ 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool runtime_ok = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(runtime_ok, "locality metadata semantic fixture builds single-series DataView");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "locality metadata semantic fixture preserves topology catalog sidecar");
            if (runtime_ok && bundle && bundle->topology_catalog) {
                TopologySpec semantic_spec{};
                semantic_spec.name = "entity.semantic.graph";
                semantic_spec.family = TopologyFamily::SemanticGraph;
                semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
                semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                semantic_spec.artifact_ref = TopologyArtifactRef{
                    "entity.semantic.graph.v1",
                    "artifacts/entity.semantic.graph.parquet",
                    "parquet",
                    "sha256:entity-semantic-v1",
                    "python_pandas"
                };
                bundle->topology_catalog->add(semantic_spec);

                ResolvedRuntimeInput runtime{};
                runtime.aligned_bundle = bundle;
                runtime.topology_catalog = bundle->topology_catalog;
                runtime.primary_ref = primary;
                runtime.primary_series_index = 0;
                runtime.run_manifest.preprocess.has_time_axis = true;
                runtime.run_manifest.preprocess.has_spatial_axis = false;
                runtime.run_manifest.preprocess.has_entity_axis = false;
                runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

                PlanSpec spec{};
                spec.numeric_nodes = {
                    NodeSpec{ "c", "core.close", {}, {} },
                    NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
                };

                const auto report = diagnose_plan_capabilities(spec, registry, runtime);
                append_plan_capability_snapshot(report, runtime.run_manifest);

                const auto* usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
                t.expect(usage != nullptr,
                    "locality metadata snapshot records semantic topology usage without runtime execution");
                if (usage) {
                    t.expect(usage->catalog_present,
                        "locality metadata snapshot preserves semantic topology catalog presence");
                    t.expect(!usage->runtime_resolved,
                        "locality metadata snapshot preserves unresolved runtime state for semantic topology");
                    t.expect(usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                        "locality metadata snapshot preserves semantic topology artifact stable id");
                }
            }
        }

        {
            const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
            const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
            const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
            const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
            const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool runtime_ok = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(runtime_ok, "locality metadata subject fixture builds two-series spatio-temporal DataView");
            if (runtime_ok && bundle) {
                ResolvedRuntimeInput runtime{};
                runtime.aligned_bundle = bundle;
                runtime.topology_catalog = bundle->topology_catalog;
                runtime.primary_ref = primary;
                runtime.primary_series_index = bundle->find_series_index(primary.series);
                runtime.series_key_to_entity = {
                    { "A", "entity.A" },
                    { "B", "entity.B" }
                };
                runtime.run_manifest.preprocess.has_time_axis = true;
                runtime.run_manifest.preprocess.has_spatial_axis = true;
                runtime.run_manifest.preprocess.has_entity_axis = true;
                runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

                const auto registry = make_default_registry();

                const PlanSpec unresolved_spec = make_spatial_radius_mean_plan(
                    "contract_locality_metadata_unresolved_subject",
                    0.2,
                    0.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));

                const auto unresolved_report = diagnose_plan_capabilities(unresolved_spec, registry, runtime);
                append_plan_capability_snapshot(unresolved_report, runtime.run_manifest);

                const auto* unresolved_usage = find_topology_usage(
                    runtime.run_manifest,
                    "mean",
                    builtin_spatial_topology_name());
                t.expect(unresolved_usage != nullptr,
                    "locality metadata snapshot records unresolved locality subject usage");
                if (unresolved_usage) {
                    t.expect(unresolved_usage->locality_subject_kind == "series_key",
                        "locality metadata snapshot preserves unresolved subject kind");
                    t.expect(unresolved_usage->locality_subject_value == "missing.series",
                        "locality metadata snapshot preserves unresolved subject value");
                    t.expect(unresolved_usage->locality_subject_explicit,
                        "locality metadata snapshot preserves unresolved explicit binding");
                    t.expect(unresolved_usage->resolved_series_key.empty(),
                        "locality metadata snapshot keeps resolved series empty on subject failure");
                    t.expect(unresolved_usage->resolved_entity_key.empty(),
                        "locality metadata snapshot keeps resolved entity empty on subject failure");
                }

                const PlanSpec resolved_spec = make_spatial_radius_mean_plan(
                    "contract_locality_metadata_resolved_subject",
                    0.2,
                    0.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));

                const auto resolved_report = diagnose_plan_capabilities(resolved_spec, registry, runtime);
                append_plan_capability_snapshot(resolved_report, runtime.run_manifest);

                const auto* resolved_usage = find_topology_usage(
                    runtime.run_manifest,
                    "mean",
                    builtin_spatial_topology_name());
                t.expect(resolved_usage != nullptr,
                    "locality metadata snapshot records resolved locality subject usage");
                if (resolved_usage) {
                    t.expect(resolved_usage->locality_subject_kind == "series_key",
                        "locality metadata snapshot preserves resolved subject kind");
                    t.expect(resolved_usage->locality_subject_value == "B",
                        "locality metadata snapshot preserves resolved subject value");
                    t.expect(resolved_usage->locality_subject_explicit,
                        "locality metadata snapshot preserves resolved explicit binding");
                    t.expect(resolved_usage->resolved_series_key == "B",
                        "locality metadata snapshot preserves resolved series key");
                    t.expect(resolved_usage->resolved_entity_key == "entity.B",
                        "locality metadata snapshot preserves resolved entity key");
                    t.expect(resolved_usage->runtime_resolved,
                        "locality metadata snapshot preserves successful runtime resolution");
                }
            }
        }

        return true;
    }

    bool test_topology_design_load_contracts(ContractAsserts& t) {
        t.out << "[topology_design_loads] current built-in topology resolution design loads\n";

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "temporal topology design-load fixture creates temporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
                    "temporal topology design-load catalog includes temporal built-in");
                t.expect(dv.has_topology(builtin_temporal_topology_name()),
                    "temporal topology design-load reports built-in named topology");
                t.expect(dv.has_topology(TopologyFamily::TemporalWindow),
                    "temporal topology design-load reports temporal topology family");
                auto named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                t.expect(named != nullptr,
                    "temporal topology design-load resolves temporal built-in by name");
                if (named) {
                    t.expect(named->topology_name() == builtin_temporal_topology_name(),
                        "temporal topology design-load named resolution returns temporal query object");
                }

            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0, 50.0 };
            const std::vector<double> x = { 0.0, 1.0, 2.0, 8.0, 9.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatial topology design-load fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
                    "spatial topology design-load catalog includes spatial built-in");
                t.expect(dv.has_topology(builtin_spatial_topology_name()),
                    "spatial topology design-load reports built-in named topology");
                t.expect(dv.has_topology(TopologyFamily::SpatialMetric2D),
                    "spatial topology design-load reports spatial topology family");
                auto named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                t.expect(named != nullptr,
                    "spatial topology design-load resolves spatial built-in by name");
                if (named) {
                    t.expect(named->topology_name() == builtin_spatial_topology_name(),
                        "spatial topology design-load named resolution returns spatial query object");
                }

            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatiotemporal topology design-load fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
                    "spatiotemporal topology design-load catalog includes temporal built-in");
                t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
                    "spatiotemporal topology design-load catalog includes spatial built-in");
                t.expect(dv.topology_catalog()->has(builtin_spatiotemporal_topology_name()),
                    "spatiotemporal topology design-load catalog includes composite built-in");
                t.expect(dv.has_topology(TopologyFamily::TemporalWindow) &&
                    dv.has_topology(TopologyFamily::SpatialMetric2D) &&
                    dv.has_topology(TopologyFamily::SpatioTemporalComposite),
                    "spatiotemporal topology design-load reports all built-in topology families");

                auto temporal_named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                auto spatial_named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                auto composite_named = dv.resolve_topology(primary.series, builtin_spatiotemporal_topology_name());
                t.expect(temporal_named != nullptr,
                    "spatiotemporal topology design-load resolves temporal built-in by name");
                t.expect(spatial_named != nullptr,
                    "spatiotemporal topology design-load resolves spatial built-in by name");
                t.expect(composite_named != nullptr,
                    "spatiotemporal topology design-load resolves composite built-in by name");
                if (composite_named) {
                    t.expect(composite_named->topology_name() == builtin_spatiotemporal_topology_name(),
                        "spatiotemporal topology design-load named resolution returns composite query object");
                }

            }
        }

        return true;
    }

    bool test_topology_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[topology_overload_rejection] current built-in topology overload rejection contracts\n";

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "temporal topology overload fixture creates temporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                expect_topology_resolution_failure(
                    t,
                    "temporal topology overload missing-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, "missing.topology");
                    });
                expect_topology_resolution_failure(
                    t,
                    "temporal topology overload spatial-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                    });
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0, 10.1 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatial topology overload fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                RuntimeLocalitySubject observation_subject{};
                observation_subject.kind = LocalitySubjectKind::ObservationDomain;
                expect_topology_resolution_failure(
                    t,
                    "spatial topology overload temporal-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                    });
                expect_topology_resolution_failure(
                    t,
                    "spatial topology overload missing-series-local-subject case",
                    "requires a resolved series-local subject",
                    [&]() {
                        return dv.resolve_topology(observation_subject, builtin_spatial_topology_name());
                    });
            }
        }

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "semantic topology overload fixture creates temporal DataView input");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "semantic topology overload fixture preserves topology catalog sidecar");
            if (okdv && bundle && bundle->topology_catalog) {
                TopologySpec semantic_spec{};
                semantic_spec.name = "entity.semantic.graph";
                semantic_spec.origin = TopologyOrigin::LayoutDeclared;
                semantic_spec.bearing = TopologyBearing::MetadataOnly;
                semantic_spec.family = TopologyFamily::SemanticGraph;
                semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
                semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                bundle->topology_catalog->add(semantic_spec);

                const DataView dv(*bundle);
                expect_topology_resolution_failure(
                    t,
                    "semantic topology overload unimplemented-family case",
                    "metadata-only",
                    [&]() {
                        return dv.resolve_topology(primary.series, "entity.semantic.graph");
                    });
            }
        }

        return true;
    }

    bool test_topology_metadata_preservation_contracts(ContractAsserts& t) {
        t.out << "[topology_metadata_preservation] current topology metadata preservation contracts\n";

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 4;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            RunManifest manifest{};
            append_plan_capability_snapshot(report, manifest);

            const auto* usage = find_topology_usage(manifest, "mean", builtin_spatial_topology_name());
            t.expect(usage != nullptr,
                "topology metadata snapshot records missing built-in topology usage");
            if (usage) {
                t.expect(!usage->catalog_present,
                    "topology metadata snapshot preserves missing built-in catalog state");
                t.expect(!usage->runtime_resolved,
                    "topology metadata snapshot preserves unresolved built-in runtime state");
                t.expect(usage->required_topology_families ==
                        std::vector<std::string>{ std::string(topology_family_name(TopologyFamily::SpatialMetric2D)) },
                    "topology metadata snapshot preserves required built-in topology family");
            }
        }

        {
            OperatorRegistry registry{};
            {
                OpEntry close{};
                close.spec.name = "core.close";
                close.spec.op_type = OpType::Numeric;
                close.spec.output = ValueType::Num;
                registry.add(std::move(close));
            }
            {
                OpEntry semantic{};
                semantic.spec.name = "semantic.weighted_mean";
                semantic.spec.op_type = OpType::Numeric;
                semantic.spec.inputs = { ValueType::Num };
                semantic.spec.output = ValueType::Num;
                semantic.spec.axis_scope = OpAxisScope::RowWise;
                semantic.spec.required_topology_families = { TopologyFamily::SemanticGraph };
                semantic.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                semantic.spec.default_topology_name = "entity.semantic.graph";
                semantic.spec.topology_binding_param = "topology";
                registry.add(std::move(semantic));
            }

            const std::array<double, 4> close{ 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool runtime_ok = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(runtime_ok, "topology metadata semantic fixture builds single-series DataView");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "topology metadata semantic fixture preserves topology catalog sidecar");
            if (runtime_ok && bundle && bundle->topology_catalog) {
                TopologySpec semantic_spec{};
                semantic_spec.name = "entity.semantic.graph";
                semantic_spec.origin = TopologyOrigin::LayoutDeclared;
                semantic_spec.bearing = TopologyBearing::MetadataOnly;
                semantic_spec.family = TopologyFamily::SemanticGraph;
                semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
                semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                semantic_spec.artifact_ref = TopologyArtifactRef{
                    "entity.semantic.graph.v1",
                    "artifacts/entity.semantic.graph.parquet",
                    "parquet",
                    "sha256:entity-semantic-v1",
                    "python_pandas"
                };
                bundle->topology_catalog->add(semantic_spec);

                ResolvedRuntimeInput runtime{};
                runtime.aligned_bundle = bundle;
                runtime.topology_catalog = bundle->topology_catalog;
                runtime.primary_ref = primary;
                runtime.primary_series_index = 0;
                runtime.run_manifest.preprocess.has_time_axis = true;
                runtime.run_manifest.preprocess.has_spatial_axis = false;
                runtime.run_manifest.preprocess.has_entity_axis = false;
                runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

                PlanSpec semantic_plan{};
                semantic_plan.numeric_nodes = {
                    NodeSpec{ "c", "core.close", {}, {} },
                    NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
                };

                const auto report = diagnose_plan_capabilities(semantic_plan, registry, runtime);
                append_plan_capability_snapshot(report, runtime.run_manifest);

                const auto* usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
                t.expect(usage != nullptr,
                    "topology metadata snapshot records semantic topology usage");
                if (usage) {
                    t.expect(usage->catalog_present,
                        "topology metadata snapshot preserves semantic topology catalog presence");
                    t.expect(!usage->runtime_resolved,
                        "topology metadata snapshot preserves unresolved semantic runtime state");
                    t.expect(usage->topology_origin == "layout_declared",
                        "topology metadata snapshot preserves semantic topology origin classification");
                    t.expect(usage->topology_bearing == "metadata_only",
                        "topology metadata snapshot preserves semantic topology bearing classification");
                    t.expect(usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                        "topology metadata snapshot preserves semantic topology artifact stable id");
                    t.expect(usage->required_topology_query_kinds ==
                            std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::WeightedNeighborSet)) },
                        "topology metadata snapshot preserves semantic topology query-kind metadata");
                }
            }
        }

        return true;
    }

    bool test_axis_domain_runtime_contracts(ContractAsserts& t) {
        t.out << "[axis_domains] temporal/spatial/spatiotemporal runtime domain contracts\n";

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        const std::vector<double> spatial_x = {
            0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0
        };
        const std::vector<double> spatial_y(close.size(), 0.0);

        PlanSpec rowwise_spec{};
        rowwise_spec.version = 1;
        rowwise_spec.name = "contract_axis_domain_rowwise";
        rowwise_spec.trade_mode = TradeMode::LongOnly;

        NodeSpec rw_close{};
        rw_close.id = "c";
        rw_close.op = "core.close";

        NodeSpec rw_thr{};
        rw_thr.id = "thr";
        rw_thr.op = "core.const";
        rw_thr.params["value"] = 103.0;

        rowwise_spec.numeric_nodes = { rw_close, rw_thr };

        NodeSpec rw_entry{};
        rw_entry.id = "entry";
        rw_entry.op = "cmp.gt";
        rw_entry.args = { "c", "thr" };

        NodeSpec rw_exit{};
        rw_exit.id = "exit";
        rw_exit.op = "cmp.lt";
        rw_exit.args = { "c", "thr" };

        rowwise_spec.bool_nodes = { rw_entry, rw_exit };
        rowwise_spec.bind.entry_long = "entry";
        rowwise_spec.bind.exit_long = "exit";

        PlanSpec temporal_local_spec = rowwise_spec;
        temporal_local_spec.name = "contract_axis_domain_temporal_local";

        NodeSpec tl_sma{};
        tl_sma.id = "sma";
        tl_sma.op = "ts.sma";
        tl_sma.args = { "c" };
        tl_sma.params["n"] = static_cast<std::int64_t>(3);

        temporal_local_spec.numeric_nodes = { tl_sma, rw_close, rw_thr };

        NodeSpec tl_entry{};
        tl_entry.id = "entry";
        tl_entry.op = "cmp.gt";
        tl_entry.args = { "sma", "thr" };

        NodeSpec tl_exit{};
        tl_exit.id = "exit";
        tl_exit.op = "cmp.lt";
        tl_exit.args = { "sma", "thr" };

        temporal_local_spec.bool_nodes = { tl_entry, tl_exit };
        temporal_local_spec.bind.entry_long = "entry";
        temporal_local_spec.bind.exit_long = "exit";

        PlanSpec mixed_axis_spec = rowwise_spec;
        mixed_axis_spec.name = "contract_axis_domain_mixed_axes";

        NodeSpec mx_spatial{};
        mx_spatial.id = "sp";
        mx_spatial.op = "spatial.identity";
        mx_spatial.args = { "c" };

        mixed_axis_spec.numeric_nodes = { rw_close, mx_spatial, rw_thr };

        NodeSpec mx_entry{};
        mx_entry.id = "entry";
        mx_entry.op = "cmp.gt";
        mx_entry.args = { "sp", "thr" };

        NodeSpec mx_exit{};
        mx_exit.id = "exit";
        mx_exit.op = "cmp.lt";
        mx_exit.args = { "sp", "thr" };

        mixed_axis_spec.bool_nodes = { mx_entry, mx_exit };
        mixed_axis_spec.bind.entry_long = "entry";
        mixed_axis_spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "temporal-only fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "temporal-only DataView row_count follows observation domain");
                t.expect(!dv.empty(), "temporal-only DataView empty() is false");
                t.expect(dv.index_schema() != nullptr, "temporal-only DataView exposes index_schema");
                t.expect(bundle->row_count() == close.size(), "temporal-only AlignedBundle row_count follows observation domain");
                t.expect(bundle->time_axis().size() == close.size(), "temporal-only AlignedBundle exposes time_axis values");
                t.expect(dv.has_axis(AxisKind::Time), "temporal-only DataView has time axis");
                t.expect(!dv.has_axis(AxisKind::Spatial), "temporal-only DataView excludes spatial axis");
                t.expect(dv.axis(AxisKind::Time) != nullptr, "temporal-only DataView resolves time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) == nullptr, "temporal-only DataView has no spatial axis definition");
                t.expect(dv.time().size() == close.size(), "temporal-only DataView compatibility time() follows semantic time axis");
                t.expect(dv.time_axis().size() == close.size(), "temporal-only DataView exposes semantic time_axis values");

                std::ostringstream rowwise_log;
                BacktestResult rowwise_out{};
                const bool rowwise_ok = run_custom_plan_spec(dv, primary, rowwise_spec, opt, rowwise_log, &rowwise_out);
                t.expect(rowwise_ok, "row-wise contract plan executes on temporal-only DataView");
            }
        }

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close, make_spatial_only_index_schema(), bundle, primary, static_cast<TimeKey>(0), spatial_x, spatial_y);
            t.expect(okdv && bundle != nullptr, "spatial-only fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "spatial-only DataView row_count follows observation domain");
                t.expect(bundle->row_count() == close.size(), "spatial-only AlignedBundle row_count follows observation domain");
                t.expect(dv.index_schema() != nullptr, "spatial-only DataView exposes index_schema");
                t.expect(!dv.has_axis(AxisKind::Time), "spatial-only DataView excludes time axis");
                t.expect(dv.has_axis(AxisKind::Spatial), "spatial-only DataView has spatial axis");
                t.expect(dv.axis(AxisKind::Time) == nullptr, "spatial-only DataView has no time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) != nullptr, "spatial-only DataView resolves spatial axis definition");
                t.expect(dv.time().empty(), "spatial-only DataView compatibility time() is empty");
                t.expect(bundle->time_axis().empty(), "spatial-only AlignedBundle time_axis is empty");
                t.expect(dv.time_axis().empty(), "spatial-only DataView semantic time_axis is empty");

                std::ostringstream temporal_log;
                BacktestResult temporal_out{};
                const bool temporal_ok = run_custom_plan_spec(dv, primary, temporal_local_spec, opt, temporal_log, &temporal_out);
                t.expect(!temporal_ok, "temporal-local op contract rejects spatial-only DataView");
                t.expect(temporal_log.str().find("required axis 'time'") != std::string::npos,
                    "temporal-local rejection reports missing time axis explicitly");

                std::ostringstream rowwise_log;
                BacktestResult rowwise_out{};
                const bool rowwise_ok = run_custom_plan_spec(dv, primary, rowwise_spec, opt, rowwise_log, &rowwise_out);
                t.expect(rowwise_ok,
                    "row-wise contract plan executes on spatial-only DataView after operator-axis cleanup");
                t.expect(rowwise_log.str().find("required axis 'time'") == std::string::npos,
                    "row-wise spatial-only execution no longer reports a missing time axis");
                t.expect(!rowwise_out.has_best_start_date,
                    "spatial-only row-wise execution omits best_start_date without time axis");
            }
        }

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close, make_time_spatial_index_schema(), bundle, primary, static_cast<TimeKey>(20240101), spatial_x, spatial_y);
            t.expect(okdv && bundle != nullptr, "spatio-temporal fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "spatio-temporal DataView row_count follows observation domain");
                t.expect(bundle->row_count() == close.size(), "spatio-temporal AlignedBundle row_count follows observation domain");
                t.expect(dv.index_schema() != nullptr, "spatio-temporal DataView exposes index_schema");
                t.expect(dv.has_axis(AxisKind::Time), "spatio-temporal DataView has time axis");
                t.expect(dv.has_axis(AxisKind::Spatial), "spatio-temporal DataView has spatial axis");
                t.expect(dv.axis(AxisKind::Time) != nullptr, "spatio-temporal DataView resolves time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) != nullptr, "spatio-temporal DataView resolves spatial axis definition");
                t.expect(bundle->time_axis().size() == close.size(), "spatio-temporal AlignedBundle exposes time_axis values");
                t.expect(dv.time().size() == close.size(), "spatio-temporal DataView compatibility time() follows semantic time axis");
                t.expect(dv.time_axis().size() == close.size(), "spatio-temporal DataView exposes semantic time_axis values");

                std::ostringstream mixed_log;
                BacktestResult mixed_out{};
                const bool mixed_ok = run_custom_plan_spec(dv, primary, mixed_axis_spec, opt, mixed_log, &mixed_out);
                t.expect(mixed_ok, "mixed-axis contract plan executes when DataView has both time and spatial axes");
            }
        }

        return true;
    }

    bool test_compile_bridge_design_load_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_design_loads] current compile-bridge runtime design loads\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge design-load spatial fixture creates spatial-only DataView input");
            if (spatial_dv_ok && spatial_bundle) {
                auto spatial_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_spatial_runtime",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()));
                add_standard_exit_binding(spatial_spec);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_log;
                BacktestResult spatial_run{};
                const bool spatial_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_spec, opt, spatial_log, &spatial_run);
                t.expect(spatial_ok,
                    "compile-bridge design-load executes spatial.radius_mean plan on spatial-only DataView");
                if (!spatial_ok) {
                    t.out << "    compile-bridge design-load spatial runtime error: " << spatial_log.str() << "\n";
                }
            }
        }

        {
            const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
            const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
            const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
            const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
            const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> multi_bundle;
            ColumnRef multi_primary{};
            const bool multi_dv_ok = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                multi_bundle,
                multi_primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(multi_dv_ok && multi_bundle != nullptr,
                "compile-bridge design-load locality-subject fixture creates aligned DataView input");
            if (multi_dv_ok && multi_bundle) {
                auto default_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_subject_default",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()));
                add_standard_exit_binding(default_spec);
                auto subject_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_subject_secondary",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));
                add_standard_exit_binding(subject_spec);

                const DataView multi_dv(*multi_bundle);
                std::ostringstream default_log;
                std::ostringstream subject_log;
                BacktestResult default_run{};
                BacktestResult subject_run{};
                const bool default_ok = run_custom_plan_spec(
                    multi_dv, multi_primary, default_spec, opt, default_log, &default_run);
                const bool subject_ok = run_custom_plan_spec(
                    multi_dv, multi_primary, subject_spec, opt, subject_log, &subject_run);
                t.expect(default_ok,
                    "compile-bridge design-load default locality-subject plan executes through bridge");
                t.expect(subject_ok,
                    "compile-bridge design-load explicit locality-subject plan executes through bridge");
                if (!default_ok) {
                    t.out << "    compile-bridge design-load default-subject runtime error: " << default_log.str() << "\n";
                }
                if (!subject_ok) {
                    t.out << "    compile-bridge design-load explicit-subject runtime error: " << subject_log.str() << "\n";
                }
                if (default_ok && subject_ok) {
                    t.expect(default_run.trades != subject_run.trades ||
                            default_run.trade_hash != subject_run.trade_hash ||
                            !nearly_equal(default_run.pnl, subject_run.pnl),
                        "compile-bridge design-load explicit subject_series changes bridged execution outcome");
                }
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge design-load spatiotemporal fixture creates DataView input");
            if (st_dv_ok && st_bundle) {
                auto st_spec = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_design_st_runtime",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_spatiotemporal_topology_name()));
                add_standard_exit_binding(st_spec);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_log;
                BacktestResult st_run{};
                const bool st_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_spec, opt, st_log, &st_run);
                t.expect(st_ok,
                    "compile-bridge design-load executes st.radius_mean plan on spatio-temporal DataView");
                if (!st_ok) {
                    t.out << "    compile-bridge design-load spatio-temporal runtime error: " << st_log.str() << "\n";
                }
            }
        }

        return true;
    }

    bool test_compile_bridge_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_overload_rejection] current compile-bridge runtime overload rejection contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge overload spatial fixture creates spatial-only DataView input");
            if (spatial_dv_ok && spatial_bundle) {
                auto spatial_bad_topology = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_overload_spatial_bad_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spatial_bad_topology);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_bad_log;
                BacktestResult spatial_bad_run{};
                const bool spatial_bad_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_topology, opt, spatial_bad_log, &spatial_bad_run);
                t.expect(!spatial_bad_ok,
                    "compile-bridge overload rejects unavailable named topology for spatial.radius_mean");
                t.expect(spatial_bad_log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "compile-bridge overload preserves missing named topology reason");

                auto spatial_bad_subject = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_overload_spatial_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spatial_bad_subject);

                std::ostringstream spatial_bad_subject_log;
                BacktestResult spatial_bad_subject_run{};
                const bool spatial_bad_subject_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_subject, opt, spatial_bad_subject_log, &spatial_bad_subject_run);
                t.expect(!spatial_bad_subject_ok,
                    "compile-bridge overload rejects unresolved locality subject for spatial.radius_mean");
                const std::string spatial_bad_subject_msg = spatial_bad_subject_log.str();
                t.expect(spatial_bad_subject_msg.find("locality subject could not be resolved") != std::string::npos,
                    "compile-bridge overload preserves locality-subject resolution failure");
                t.expect(spatial_bad_subject_msg.find("series not found") != std::string::npos,
                    "compile-bridge overload preserves missing-series detail");
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge overload spatiotemporal fixture creates DataView input");
            if (st_dv_ok && st_bundle) {
                auto st_bad_topology = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_overload_st_bad_topology",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(st_bad_topology);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_bad_log;
                BacktestResult st_bad_run{};
                const bool st_bad_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_bad_topology, opt, st_bad_log, &st_bad_run);
                t.expect(!st_bad_ok,
                    "compile-bridge overload rejects topology family mismatch for st.radius_mean");
                const std::string st_bad_msg = st_bad_log.str();
                t.expect(st_bad_msg.find("has family 'temporal_window'") != std::string::npos,
                    "compile-bridge overload preserves actual mismatched topology family");
                t.expect(st_bad_msg.find("spatiotemporal_composite") != std::string::npos,
                    "compile-bridge overload preserves required topology family");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "compile-bridge overload unsupported-op fixture creates DataView input");
            if (okdv && bundle) {
                PlanSpec bad{};
                bad.version = 1;
                bad.name = "contract_compile_bridge_overload_bad_op";
                bad.trade_mode = TradeMode::LongOnly;

                NodeSpec n_close{};
                n_close.id = "c";
                n_close.op = "core.close";

                NodeSpec n_mid{};
                n_mid.id = "mid";
                n_mid.op = "bad.unsupported_op";
                n_mid.args = { "c" };

                NodeSpec n_thr{};
                n_thr.id = "thr";
                n_thr.op = "core.const";
                n_thr.params["value"] = 100.0;

                bad.numeric_nodes = { n_close, n_mid, n_thr };

                NodeSpec bad_entry{};
                bad_entry.id = "entry_bad";
                bad_entry.op = "cmp.gt";
                bad_entry.args = { "c", "thr" };

                NodeSpec bad_exit{};
                bad_exit.id = "exit_bad";
                bad_exit.op = "cmp.lt";
                bad_exit.args = { "c", "thr" };

                bad.bool_nodes = { bad_entry, bad_exit };
                bad.bind.entry_long = "entry_bad";
                bad.bind.exit_long = "exit_bad";

                const DataView dv(*bundle);
                std::ostringstream bad_log;
                BacktestResult bad_r{};
                const bool bad_ok = run_custom_plan_spec(dv, primary, bad, opt, bad_log, &bad_r);
                t.expect(!bad_ok,
                    "compile-bridge overload hard-fails when compile bridge cannot compile node op");
                const std::string bad_msg = bad_log.str();
                const bool explicit_fail_msg =
                    (bad_msg.find("PlanSpec compile failed:") != std::string::npos) ||
                    (bad_msg.find("PlanSpec runtime validation failed:") != std::string::npos);
                t.expect(explicit_fail_msg,
                    "compile-bridge overload keeps compile/validation failure explicit");
                t.expect(bad_msg.find("Unsupported numeric op in multi-series mode") == std::string::npos,
                    "compile-bridge overload does not regress to legacy fallback evaluator errors");
            }
        }

        return true;
    }
bool test_operator_axis_contracts(ContractAsserts& t) {
        t.out << "[operator_axes] operator axis-contract classification\n";

        const auto reg = make_default_registry();

        auto expect_op = [&](const char* name,
                             OpAxisScope scope,
                             std::vector<AxisKind> required,
                             const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered");
            if (!op) return;
            t.expect(op->spec.axis_scope == scope, std::string(label) + " has expected axis_scope");
            t.expect(op->spec.required_axes == required, std::string(label) + " has expected required_axes");
        };

        auto expect_topology = [&](const char* name,
                                   std::vector<TopologyFamily> families,
                                   std::vector<TopologyQueryKind> queries,
                                   std::string default_topology,
                                   std::string binding_param,
                                   const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for topology hooks");
            if (!op) return;
            t.expect(op->spec.required_topology_families == families,
                std::string(label) + " has expected required_topology_families");
            t.expect(op->spec.required_topology_query_kinds == queries,
                std::string(label) + " has expected required_topology_query_kinds");
            t.expect(op->spec.default_topology_name == default_topology,
                std::string(label) + " has expected default_topology_name");
            t.expect(op->spec.topology_binding_param == binding_param,
                std::string(label) + " has expected topology_binding_param");
        };
        auto expect_locality_subject = [&](const char* name,
                                           LocalitySubjectKind default_kind,
                                           std::string default_value,
                                           LocalitySubjectKind bound_kind,
                                           std::string binding_param,
                                           const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for locality-subject hooks");
            if (!op) return;
            t.expect(op->spec.default_locality_subject_kind == default_kind,
                std::string(label) + " has expected default_locality_subject_kind");
            t.expect(op->spec.default_locality_subject_value == default_value,
                std::string(label) + " has expected default_locality_subject_value");
            t.expect(op->spec.bound_locality_subject_kind == bound_kind,
                std::string(label) + " has expected bound_locality_subject_kind");
            t.expect(op->spec.locality_subject_binding_param == binding_param,
                std::string(label) + " has expected locality_subject_binding_param");
        };

        expect_op("core.close", OpAxisScope::RowWise, {}, "core.close");
        expect_op("core.const", OpAxisScope::RowWise, {}, "core.const");
        expect_op("core.ref", OpAxisScope::RowWise, {}, "core.ref");
        expect_op("core.select", OpAxisScope::RowWise, {}, "core.select");
        expect_op("core.bool_const", OpAxisScope::RowWise, {}, "core.bool_const");
        expect_op("logic.and", OpAxisScope::RowWise, {}, "logic.and");
        expect_op("logic.not", OpAxisScope::RowWise, {}, "logic.not");
        expect_op("cmp.gt", OpAxisScope::RowWise, {}, "cmp.gt");
        expect_op("cmp.cross_up", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_up");
        expect_op("cmp.cross_down", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_down");
        expect_op("ts.sma", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.sma");
        expect_op("ts.percentile", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.percentile");
        expect_op("ts.macd", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.macd");
        expect_op("ts.bollinger_width", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.bollinger_width");
        expect_op("ts.volatility_ratio", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.volatility_ratio");
        expect_op("ts.neg", OpAxisScope::RowWise, {}, "ts.neg");
        expect_op("ts.add", OpAxisScope::RowWise, {}, "ts.add");
        expect_op("ts.clamp", OpAxisScope::RowWise, {}, "ts.clamp");
        expect_op("ts.ratio_of_ma", OpAxisScope::RowWise, {}, "ts.ratio_of_ma");
        expect_op("ts.true_range", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.true_range");
        expect_op("ms.diff", OpAxisScope::CrossSeries, {}, "ms.diff");
        expect_op("ms.ratio", OpAxisScope::CrossSeries, {}, "ms.ratio");
        expect_op("ms.zscore", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.zscore");
        expect_op("ms.correlation", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.correlation");
        expect_op("spatial.identity", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.identity");
        expect_op("spatial.radius_mean", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.radius_mean");
        expect_op("st.radius_mean", OpAxisScope::SpatioTemporal, {AxisKind::Time, AxisKind::Spatial}, "st.radius_mean");
        expect_topology("core.close", {}, {}, std::string{}, std::string{}, "core.close");
        expect_topology("spatial.identity", {}, {}, std::string{}, std::string{}, "spatial.identity");
        expect_topology("spatial.radius_mean",
            { TopologyFamily::SpatialMetric2D },
            { TopologyQueryKind::MetricRadius },
            std::string(builtin_spatial_topology_name()),
            std::string("topology"),
            "spatial.radius_mean");
        expect_topology("st.radius_mean",
            { TopologyFamily::SpatioTemporalComposite },
            { TopologyQueryKind::Intersection },
            std::string(builtin_spatiotemporal_topology_name()),
            std::string("topology"),
            "st.radius_mean");
        expect_locality_subject("core.close",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string{},
            "core.close");
        expect_locality_subject("spatial.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "spatial.radius_mean");
        expect_locality_subject("st.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "st.radius_mean");

        {
            OpEntry subject_op{};
            subject_op.spec.name = "contract.locality_subject";
            subject_op.spec.default_locality_subject_kind = LocalitySubjectKind::PrimarySeries;
            subject_op.spec.bound_locality_subject_kind = LocalitySubjectKind::SeriesKey;
            subject_op.spec.locality_subject_binding_param = "subject_series";

            ResolvedLocalitySubject subject{};
            std::string err;
            ParamMap node_params{};
            ParamMap plan_params{};
            const bool default_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(default_ok, "locality-subject resolver accepts missing explicit binding");
            t.expect(subject.kind == LocalitySubjectKind::PrimarySeries &&
                subject.value.empty() &&
                !subject.explicit_binding,
                "locality-subject resolver preserves primary-series default");

            node_params["subject_series"] = std::string("BBB");
            const bool literal_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(literal_ok, "locality-subject resolver accepts literal subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "BBB" &&
                subject.explicit_binding,
                "locality-subject resolver upgrades to explicit series-key binding");

            node_params["subject_series"] = ParamRef{ "subject_param" };
            plan_params["subject_param"] = std::string("CCC");
            const bool ref_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(ref_ok, "locality-subject resolver accepts param-ref subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "CCC" &&
                subject.explicit_binding,
                "locality-subject resolver resolves param-ref subject binding");

            ResolvedParamMap resolved_params{};
            resolved_params.emplace("subject_series", std::string("DDD"));
            const bool resolved_ok = resolve_bound_locality_subject(subject_op, resolved_params, subject, err);
            t.expect(resolved_ok, "locality-subject resolver accepts resolved-param subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "DDD" &&
                subject.explicit_binding,
                "locality-subject resolver preserves resolved-param explicit subject");
        }

        return true;
    }

    bool test_planspec_axis_validation(ContractAsserts& t) {
        t.out << "[axis_validation] PlanSpec axis validation against DataView index_schema\n";

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "axis validation test creates DataView input");
        if (!okdv || !bundle) return true;

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_axis_validation";
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec n_close{};
        n_close.id = "c";
        n_close.op = "core.close";

        NodeSpec n_zero{};
        n_zero.id = "z";
        n_zero.op = "core.const";
        n_zero.params["value"] = 0.0;

        spec.numeric_nodes = { n_close, n_zero };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "c", "z" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "c", "z" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};
        BacktestResult out_ok{};
        std::ostringstream ok_log;
        const DataView dv_ok(*bundle);
        const bool ran_ok = run_custom_plan_spec(dv_ok, primary, spec, opt, ok_log, &out_ok);
        t.expect(ran_ok, "axis validation passes for row-wise plan when DataView has index_schema");

        auto broken = std::make_shared<align::AlignedBundle>(*bundle);
        broken->index_schema.reset();
        const DataView dv_bad(*broken);
        std::ostringstream bad_log;
        BacktestResult out_bad{};
        const bool ran_bad = run_custom_plan_spec(dv_bad, primary, spec, opt, bad_log, &out_bad);
        t.expect(ran_bad, "axis validation allows row-wise plan when DataView index_schema is missing");

        PlanSpec temporal_spec = spec;
        temporal_spec.name = "contract_axis_validation_temporal";
        NodeSpec n_sma{};
        n_sma.id = "s";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = static_cast<std::int64_t>(3);
        temporal_spec.numeric_nodes = { n_sma, n_close, n_zero };
        NodeSpec tb_entry{};
        tb_entry.id = "t_entry";
        tb_entry.op = "cmp.gt";
        tb_entry.args = { "s", "z" };
        NodeSpec tb_exit{};
        tb_exit.id = "t_exit";
        tb_exit.op = "cmp.lt";
        tb_exit.args = { "s", "z" };
        temporal_spec.bool_nodes = { tb_entry, tb_exit };
        temporal_spec.bind.entry_long = "t_entry";
        temporal_spec.bind.exit_long = "t_exit";

        std::ostringstream temporal_ok_log;
        BacktestResult temporal_ok_out{};
        const bool temporal_ran_ok = run_custom_plan_spec(dv_ok, primary, temporal_spec, opt, temporal_ok_log, &temporal_ok_out);
        t.expect(temporal_ran_ok, "axis validation passes for temporal-local plan when DataView has time axis");

        std::ostringstream temporal_bad_log;
        BacktestResult temporal_bad_out{};
        const bool temporal_ran_bad = run_custom_plan_spec(dv_bad, primary, temporal_spec, opt, temporal_bad_log, &temporal_bad_out);
        t.expect(!temporal_ran_bad, "axis validation fails for temporal-local plan when DataView index_schema is missing");
        const std::string temporal_msg = temporal_bad_log.str();
        t.expect(temporal_msg.find("required axis 'time'") != std::string::npos,
            "temporal-local validation failure reports missing time axis");

        PlanSpec spatial_spec = spec;
        spatial_spec.name = "contract_axis_validation_spatial";
        NodeSpec n_spatial{};
        n_spatial.id = "s";
        n_spatial.op = "spatial.identity";
        n_spatial.args = { "c" };
        spatial_spec.numeric_nodes.push_back(n_spatial);
        spatial_spec.bool_nodes.clear();

        NodeSpec sb_entry{};
        sb_entry.id = "s_entry";
        sb_entry.op = "cmp.gt";
        sb_entry.args = { "s", "z" };
        NodeSpec sb_exit{};
        sb_exit.id = "s_exit";
        sb_exit.op = "cmp.lt";
        sb_exit.args = { "s", "z" };
        spatial_spec.bool_nodes = { sb_entry, sb_exit };
        spatial_spec.bind.entry_long = "s_entry";
        spatial_spec.bind.exit_long = "s_exit";

        std::ostringstream spatial_log;
        BacktestResult spatial_out{};
        const bool spatial_ok = run_custom_plan_spec(dv_ok, primary, spatial_spec, opt, spatial_log, &spatial_out);
        t.expect(!spatial_ok, "spatial op is rejected on non-spatial DataView");
        const std::string spatial_msg = spatial_log.str();
        t.expect(spatial_msg.find("required axis 'spatial'") != std::string::npos,
            "spatial-axis validation failure is explicit");

        PlanSpec st_spec = spec;
        st_spec.name = "contract_axis_validation_spatiotemporal";
        NodeSpec n_st{};
        n_st.id = "st";
        n_st.op = "st.radius_mean";
        n_st.args = { "c" };
        n_st.params["last_n"] = static_cast<std::int64_t>(2);
        n_st.params["radius"] = 1.0;
        st_spec.numeric_nodes.push_back(n_st);
        st_spec.bool_nodes.clear();

        NodeSpec stb_entry{};
        stb_entry.id = "st_entry";
        stb_entry.op = "cmp.gt";
        stb_entry.args = { "st", "z" };
        NodeSpec stb_exit{};
        stb_exit.id = "st_exit";
        stb_exit.op = "cmp.lt";
        stb_exit.args = { "st", "z" };
        st_spec.bool_nodes = { stb_entry, stb_exit };
        st_spec.bind.entry_long = "st_entry";
        st_spec.bind.exit_long = "st_exit";

        std::ostringstream st_log;
        BacktestResult st_out{};
        const bool st_ok = run_custom_plan_spec(dv_ok, primary, st_spec, opt, st_log, &st_out);
        t.expect(!st_ok, "spatio-temporal op is rejected on non-spatial DataView");
        const std::string st_msg = st_log.str();
        t.expect(st_msg.find("required axis 'spatial'") != std::string::npos,
            "spatio-temporal validation failure reports missing spatial axis explicitly");

        return true;
    }

    bool test_axis_query_temporal_contracts(ContractAsserts& t) {
        t.out << "[axis_query_temporal] temporal axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary, static_cast<TimeKey>(20240101));
        t.expect(okdv && bundle != nullptr, "temporal-only query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(dv.axes().size() == 1, "temporal-only DataView exposes one axis definition");
        t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
            "temporal-only DataView topology catalog includes built-in temporal topology");
        t.expect(dv.has_topology(builtin_temporal_topology_name()),
            "temporal-only DataView reports built-in temporal topology by name");
        t.expect(dv.has_topology(TopologyFamily::TemporalWindow),
            "temporal-only DataView reports temporal topology family");
        t.expect(dv.topology(builtin_temporal_topology_name()) != nullptr,
            "temporal-only DataView resolves temporal topology spec");

        auto named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
        t.expect(named != nullptr, "temporal-only DataView resolves built-in temporal topology by name");
        if (named) {
            t.expect(named->topology_name() == builtin_temporal_topology_name(),
                "temporal-only named topology resolution returns temporal query object");
        }

        ResolvedLocalitySubject primary_subject{};
        primary_subject.kind = LocalitySubjectKind::PrimarySeries;
        const auto resolved_primary_subject = dv.resolve_locality_subject(primary_subject, primary.series);
        t.expect(resolved_primary_subject.series_key == primary.series,
            "temporal-only DataView resolves primary-series locality subject to primary series key");
        t.expect(resolved_primary_subject.entity_key == primary.series,
            "temporal-only DataView resolves primary-series locality subject to identity entity key");

        auto subject_named = dv.resolve_topology(primary_subject, primary.series, builtin_temporal_topology_name());
        t.expect(subject_named != nullptr, "temporal-only DataView resolves topology from locality-subject contract");

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "temporal-only DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_temporal_topology_name(),
                "temporal-only neighborhood query exposes built-in topology name");
            t.expect(nq->has_axis(AxisKind::Time), "temporal-only query advertises time axis");
            t.expect(!nq->has_axis(AxisKind::Spatial), "temporal-only query excludes spatial axis");

            const auto win = nq->query_time(3, TimeNeighborhoodRequest::last_n_samples(3));
            t.expect(win.begin == 1 && win.end == 4 && win.anchor == 3,
                "temporal-only time neighborhood returns expected contiguous row window");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    3,
                    TimeNeighborhoodRequest::last_n_samples(3)));
            t.expect(canonical.topology_name == builtin_temporal_topology_name(),
                "temporal-only canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 3,
                "temporal-only canonical neighborhood result preserves anchor row");
            t.expect(canonical.contiguous && canonical.ordered,
                "temporal-only canonical neighborhood result preserves contiguous ordered fast path");
            t.expect(canonical.begin == 1 && canonical.end == 4,
                "temporal-only canonical neighborhood result preserves ordered-window bounds");
            t.expect(canonical.materialize_indices() == std::vector<std::size_t>({ 1, 2, 3 }),
                "temporal-only canonical neighborhood result materializes contiguous indices on demand");
            t.expect(neighborhood_result_to_slice(canonical).begin == win.begin &&
                neighborhood_result_to_slice(canonical).end == win.end,
                "temporal-only compatibility adapter round-trips canonical result back to slice");
        }

        if (subject_named) {
            const auto canonical = subject_named->query(
                make_neighborhood_request(
                    subject_named->topology_name(),
                    3,
                    TimeNeighborhoodRequest::last_n_samples(2)));
            t.expect(canonical.anchor.series == primary.series,
                "temporal-only subject-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == primary.series,
                "temporal-only subject-bound query preserves resolved entity on canonical anchor");
        }

        ResolvedLocalitySubject observation_domain_subject{};
        observation_domain_subject.kind = LocalitySubjectKind::ObservationDomain;
        auto observation_domain_query = dv.resolve_topology(
            observation_domain_subject,
            primary.series,
            builtin_temporal_topology_name());
        t.expect(observation_domain_query != nullptr,
            "temporal-only DataView can resolve temporal topology for observation-domain subject");

        return true;
    }

    bool test_axis_query_spatial_contracts(ContractAsserts& t) {
        t.out << "[axis_query_spatial] spatial axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };
        const std::vector<double> x = { 0.0, 1.0, 4.0, 10.0, 11.0 };
        const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_with_index_schema(
            close,
            make_spatial_only_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(0),
            x,
            y);
        t.expect(okdv && bundle != nullptr, "spatial-only query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        bundle->entity_keys = { "station.alpha" };
        const DataView dv(*bundle);
        const auto spatial = dv.spatial_axis_2d(primary.series);

        t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
            "spatial-only DataView topology catalog includes built-in spatial topology");
        t.expect(dv.has_topology(builtin_spatial_topology_name()),
            "spatial-only DataView reports built-in spatial topology by name");
        t.expect(dv.has_topology(TopologyFamily::SpatialMetric2D),
            "spatial-only DataView reports spatial topology family");

        auto named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
        t.expect(named != nullptr, "spatial-only DataView resolves built-in spatial topology by name");
        if (named) {
            t.expect(named->topology_name() == builtin_spatial_topology_name(),
                "spatial-only named topology resolution returns spatial query object");
        }

        ResolvedLocalitySubject entity_subject{};
        entity_subject.kind = LocalitySubjectKind::EntityKey;
        entity_subject.value = "station.alpha";
        entity_subject.explicit_binding = true;
        const auto resolved_entity_subject = dv.resolve_locality_subject(entity_subject);
        t.expect(resolved_entity_subject.series_key == primary.series,
            "spatial-only DataView resolves entity-key locality subject to backing series key");
        t.expect(resolved_entity_subject.entity_key == "station.alpha",
            "spatial-only DataView preserves entity-key locality subject value");

        auto entity_named = dv.resolve_topology(entity_subject, {}, builtin_spatial_topology_name());
        t.expect(entity_named != nullptr, "spatial-only DataView resolves topology from entity-key subject");

        t.expect(spatial.has_value() && spatial->valid(), "spatial-only DataView exposes spatial axis payloads");
        if (spatial.has_value() && spatial->valid()) {
            const bool names_ok = spatial->x_name == "x" && spatial->y_name == "y";
            if (!names_ok) {
                t.out << "    spatial payload names: x='" << spatial->x_name << "' y='" << spatial->y_name << "'\n";
            }
            t.expect(names_ok,
                "spatial-only DataView preserves spatial axis column names");
            t.expect(spatial->x.size() == close.size() && spatial->y.size() == close.size(),
                "spatial-only DataView spatial payload sizes match row domain");
        }

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "spatial-only DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_spatial_topology_name(),
                "spatial-only neighborhood query exposes built-in topology name");
            t.expect(!nq->has_axis(AxisKind::Time), "spatial-only query excludes time axis");
            t.expect(nq->has_axis(AxisKind::Spatial), "spatial-only query advertises spatial axis");

            const auto near = nq->query_spatial(0, SpatialNeighborhoodRequest::radius_euclidean(1.1));
            t.expect(near.indices == std::vector<std::size_t>({ 0, 1 }),
                "spatial-only radius neighborhood returns expected rows");

            const auto knear = nq->query_spatial(4, SpatialNeighborhoodRequest::nearest_k(2));
            t.expect(knear.indices == std::vector<std::size_t>({ 3, 4 }),
                "spatial-only k-nearest neighborhood returns expected rows");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    4,
                    SpatialNeighborhoodRequest::nearest_k(2)));
            t.expect(canonical.topology_name == builtin_spatial_topology_name(),
                "spatial-only canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 4,
                "spatial-only canonical neighborhood result preserves anchor row");
            t.expect(!canonical.contiguous && canonical.ordered,
                "spatial-only canonical neighborhood result stays selection-based but ordered");
            t.expect(canonical.indices == std::vector<std::size_t>({ 3, 4 }),
                "spatial-only canonical neighborhood result preserves selected indices");
            t.expect(canonical.distances.size() == canonical.indices.size(),
                "spatial-only canonical neighborhood result can preserve optional metric distances");
            t.expect(neighborhood_result_to_selection(canonical).indices == knear.indices,
                "spatial-only compatibility adapter round-trips canonical result back to selection");
        }

        if (entity_named) {
            const auto canonical = entity_named->query(
                make_neighborhood_request(
                    entity_named->topology_name(),
                    4,
                    SpatialNeighborhoodRequest::nearest_k(2)));
            t.expect(canonical.anchor.series == primary.series,
                "spatial-only entity-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == "station.alpha",
                "spatial-only entity-bound query preserves resolved entity on canonical anchor");
        }

        return true;
    }

    bool test_axis_query_spatiotemporal_contracts(ContractAsserts& t) {
        t.out << "[axis_query_spatiotemporal] spatiotemporal axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };
        const std::vector<double> x = { 0.0, 1.0, 4.0, 10.0, 11.0 };
        const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_with_index_schema(
            close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            x,
            y);
        t.expect(okdv && bundle != nullptr, "spatio-temporal query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
            "spatio-temporal DataView topology catalog includes temporal built-in");
        t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
            "spatio-temporal DataView topology catalog includes spatial built-in");
        t.expect(dv.topology_catalog()->has(builtin_spatiotemporal_topology_name()),
            "spatio-temporal DataView topology catalog includes composite built-in");
        t.expect(dv.has_topology(builtin_temporal_topology_name()) &&
            dv.has_topology(builtin_spatial_topology_name()) &&
            dv.has_topology(builtin_spatiotemporal_topology_name()),
            "spatio-temporal DataView reports all built-in named topologies");
        t.expect(dv.has_topology(TopologyFamily::TemporalWindow) &&
            dv.has_topology(TopologyFamily::SpatialMetric2D) &&
            dv.has_topology(TopologyFamily::SpatioTemporalComposite),
            "spatio-temporal DataView reports all built-in topology families");

        auto temporal_named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
        t.expect(temporal_named != nullptr, "spatio-temporal DataView resolves temporal built-in by name");
        if (temporal_named) {
            t.expect(temporal_named->topology_name() == builtin_temporal_topology_name(),
                "spatio-temporal DataView temporal named resolution returns time query object");
        }

        auto spatial_named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
        t.expect(spatial_named != nullptr, "spatio-temporal DataView resolves spatial built-in by name");
        if (spatial_named) {
            t.expect(spatial_named->topology_name() == builtin_spatial_topology_name(),
                "spatio-temporal DataView spatial named resolution returns spatial query object");
        }

        auto composite_named = dv.resolve_topology(primary.series, builtin_spatiotemporal_topology_name());
        t.expect(composite_named != nullptr, "spatio-temporal DataView resolves composite built-in by name");
        if (composite_named) {
            t.expect(composite_named->topology_name() == builtin_spatiotemporal_topology_name(),
                "spatio-temporal DataView composite named resolution returns composite query object");
        }

        ResolvedLocalitySubject primary_subject{};
        primary_subject.kind = LocalitySubjectKind::PrimarySeries;
        auto subject_named = dv.resolve_topology(
            primary_subject,
            primary.series,
            builtin_spatiotemporal_topology_name());
        t.expect(subject_named != nullptr,
            "spatio-temporal DataView resolves composite topology from primary-series subject");

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "spatio-temporal DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_spatiotemporal_topology_name(),
                "spatio-temporal neighborhood query exposes built-in topology name");
            t.expect(nq->has_axis(AxisKind::Time), "spatio-temporal query advertises time axis");
            t.expect(nq->has_axis(AxisKind::Spatial), "spatio-temporal query advertises spatial axis");

            const auto both = nq->query_spatiotemporal(
                2,
                SpatioTemporalNeighborhoodRequest::intersection(
                    TimeNeighborhoodRequest::last_n_samples(3),
                    SpatialNeighborhoodRequest::radius_euclidean(3.1)));
            t.expect(both.indices == std::vector<std::size_t>({ 1, 2 }),
                "spatio-temporal neighborhood intersects time and spatial locality");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    2,
                    SpatioTemporalNeighborhoodRequest::intersection(
                        TimeNeighborhoodRequest::last_n_samples(3),
                        SpatialNeighborhoodRequest::radius_euclidean(3.1))));
            t.expect(canonical.topology_name == builtin_spatiotemporal_topology_name(),
                "spatio-temporal canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 2,
                "spatio-temporal canonical neighborhood result preserves anchor row");
            t.expect(canonical.indices == std::vector<std::size_t>({ 1, 2 }),
                "spatio-temporal canonical neighborhood result preserves intersection indices");
            t.expect(neighborhood_result_to_selection(canonical).indices == both.indices,
                "spatio-temporal compatibility adapter round-trips canonical result back to selection");
        }

        if (subject_named) {
            const auto canonical = subject_named->query(
                make_neighborhood_request(
                    subject_named->topology_name(),
                    2,
                    SpatioTemporalNeighborhoodRequest::intersection(
                        TimeNeighborhoodRequest::last_n_samples(2),
                        SpatialNeighborhoodRequest::radius_euclidean(3.1))));
            t.expect(canonical.anchor.series == primary.series,
                "spatio-temporal subject-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == primary.series,
                "spatio-temporal subject-bound query preserves resolved entity on canonical anchor");
        }

        return true;
    }
bool test_compile_bridge_registry_surface_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_registry_surface] compile-bridge registry surface contracts\n";

        const auto reg = make_default_registry();
        const std::vector<std::string> ts_phase1_ops = {
            "core.select",
            "ms.correlation",
            "ts.abs",
            "ts.add",
            "ts.atr",
            "ts.atr_wilder",
            "ts.bollinger_width",
            "ts.clamp",
            "ts.clamp_const",
            "ts.delta",
            "ts.donchian_lower",
            "ts.donchian_mid",
            "ts.donchian_upper",
            "ts.donchian_width",
            "ts.div",
            "ts.ema",
            "ts.exp",
            "ts.log",
            "ts.macd",
            "ts.max",
            "ts.min",
            "ts.mul",
            "ts.neg",
            "ts.normalize_minmax",
            "ts.normalize_minmax_global",
            "ts.percentile",
            "ts.pow_const",
            "ts.ratio_of_ma",
            "ts.roc",
            "ts.roll_max",
            "ts.roll_min",
            "ts.rsi",
            "ts.shift",
            "ts.sma",
            "ts.stddev",
            "ts.stddev_ema",
            "ts.sqrt",
            "ts.sub",
            "ts.true_range",
            "ts.true_range_ohlc",
            "ts.volatility_ratio",
            "ts.zscore_ema",
            "ts.zscore_sma",
            "spatial.radius_mean",
            "st.radius_mean",
        };
        for (const auto& op : ts_phase1_ops) {
            t.expect(reg.has(op), "registry exposes " + op);
        }

        return true;
    }

    bool test_compile_bridge_temporal_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_temporal_runtime] compile-bridge temporal runtime contracts\n";

        std::vector<double> close;
        close.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "compile-bridge test creates DataView input");
        if (!okdv || !bundle) return true;

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_compile_bridge_runtime";
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec n_close{};
        n_close.id = "c";
        n_close.op = "core.close";

        NodeSpec n_sma{};
        n_sma.id = "sma";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = static_cast<std::int64_t>(3);

        NodeSpec n_thr{};
        n_thr.id = "thr";
        n_thr.op = "core.const";
        n_thr.params["value"] = 100.0;

        spec.numeric_nodes = { n_close, n_sma, n_thr };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "sma", "thr" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "sma", "thr" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};
        const DataView dv(*bundle);
        std::ostringstream run_log;
        BacktestResult r{};
        const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &r);
        t.expect(ok, "custom PlanSpec runtime executes ts.sma plan via compile bridge");
        if (!ok) {
            t.out << "    compile-bridge runtime error: " << run_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_spatial_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_spatial_runtime] compile-bridge spatial runtime contracts\n";

        const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
        const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
        const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> spatial_bundle;
        ColumnRef spatial_primary{};
        const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
            spatial_close,
            make_spatial_only_index_schema(),
            spatial_bundle,
            spatial_primary,
            static_cast<TimeKey>(0),
            spatial_x,
            spatial_y);
        t.expect(spatial_dv_ok && spatial_bundle != nullptr,
            "compile-bridge spatial-op test creates spatial-only DataView input");
        if (!spatial_dv_ok || !spatial_bundle) return true;

        PlanSpec spatial_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_spatial_runtime",
            1.1,
            14.0,
            std::string(builtin_spatial_topology_name()));
        add_standard_exit_binding(spatial_spec);

        PlanSweepRuntimeOptions opt{};
        const DataView spatial_dv(*spatial_bundle);
        std::ostringstream spatial_log;
        BacktestResult spatial_run{};
        const bool spatial_ok = run_custom_plan_spec(
            spatial_dv, spatial_primary, spatial_spec, opt, spatial_log, &spatial_run);
        t.expect(spatial_ok,
            "custom PlanSpec runtime executes spatial.radius_mean plan on spatial-only DataView");
        if (!spatial_ok) {
            t.out << "    compile-bridge spatial runtime error: " << spatial_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_subject_routing_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_subject_routing] compile-bridge locality subject-routing contracts\n";

        const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
        const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> multi_bundle;
        ColumnRef multi_primary{};
        const bool multi_dv_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            multi_bundle,
            multi_primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(multi_dv_ok && multi_bundle != nullptr,
            "compile-bridge multi-series locality-subject test creates aligned DataView input");
        if (!multi_dv_ok || !multi_bundle) return true;

        PlanSpec default_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_subject_default",
            0.2,
            50.0,
            std::string(builtin_spatial_topology_name()));
        add_standard_exit_binding(default_spec);

        PlanSpec subject_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_subject_secondary",
            0.2,
            50.0,
            std::string(builtin_spatial_topology_name()),
            std::string("B"));
        add_standard_exit_binding(subject_spec);

        const auto registry = make_default_registry();
        CompileEnv env{};
        CompileResult default_compiled{};
        CompileResult subject_compiled{};
        std::string compile_err;
        t.expect(compile_plan(default_spec, registry, env, default_compiled, compile_err),
            "compile-bridge default locality-subject plan compiles");
        t.expect(compile_plan(subject_spec, registry, env, subject_compiled, compile_err),
            "compile-bridge explicit locality-subject plan compiles");

        const DataView multi_dv(*multi_bundle);
        ProgramSignalOutputs default_signals{};
        ProgramSignalOutputs subject_signals{};
        std::string eval_err;
        t.expect(evaluate_strategy_plan_program(
                multi_dv,
                multi_primary,
                default_compiled.plan,
                default_signals,
                eval_err),
            "compile-bridge default locality-subject plan evaluates");
        t.expect(evaluate_strategy_plan_program(
                multi_dv,
                multi_primary,
                subject_compiled.plan,
                subject_signals,
                eval_err),
            "compile-bridge explicit locality-subject plan evaluates");
        t.expect(!default_signals.entry_long.empty() && !subject_signals.entry_long.empty(),
            "compile-bridge locality-subject evaluation produces entry buffers");
        if (!default_signals.entry_long.empty() && !subject_signals.entry_long.empty()) {
            t.expect(default_signals.entry_long[0] == kBoolTrue,
                "default locality subject keeps first-row entry true on primary-series topology");
            t.expect(subject_signals.entry_long[0] == kBoolFalse,
                "explicit subject_series redirects topology lookup and changes first-row entry result");
        }

        return true;
    }

    bool test_compile_bridge_spatiotemporal_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_spatiotemporal_runtime] compile-bridge spatiotemporal runtime contracts\n";

        const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
        const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
        const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> st_bundle;
        ColumnRef st_primary{};
        const bool st_dv_ok = make_single_series_dataview_with_index_schema(
            st_close,
            make_time_spatial_index_schema(),
            st_bundle,
            st_primary,
            static_cast<TimeKey>(20240101),
            st_x,
            st_y);
        t.expect(st_dv_ok && st_bundle != nullptr,
            "compile-bridge spatio-temporal-op test creates spatio-temporal DataView input");
        if (!st_dv_ok || !st_bundle) return true;

        PlanSpec st_spec = make_spatiotemporal_radius_mean_plan(
            "contract_compile_bridge_st_runtime",
            2,
            0.5,
            20.0,
            std::string(builtin_spatiotemporal_topology_name()));
        st_spec.params["last_n"] = static_cast<std::int64_t>(2);
        st_spec.params["topology_name"] = std::string(builtin_spatiotemporal_topology_name());
        st_spec.numeric_nodes[1].params["last_n"] = ParamRef{ "last_n" };
        st_spec.numeric_nodes[1].params["topology"] = ParamRef{ "topology_name" };
        add_standard_exit_binding(st_spec);

        PlanSweepRuntimeOptions opt{};
        const DataView st_dv(*st_bundle);
        std::ostringstream st_log;
        BacktestResult st_run{};
        const bool st_ok = run_custom_plan_spec(
            st_dv, st_primary, st_spec, opt, st_log, &st_run);
        t.expect(st_ok,
            "custom PlanSpec runtime executes st.radius_mean plan on spatio-temporal DataView");
        if (!st_ok) {
            t.out << "    compile-bridge spatio-temporal runtime error: " << st_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_sweep_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_sweep_runtime] compile-bridge sweep runtime contracts\n";

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge sweep test creates spatio-temporal DataView input");
            if (st_dv_ok && st_bundle) {
                PlanSpec st_sweep = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_st_sweep_runtime",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_spatiotemporal_topology_name()));
                add_standard_exit_binding(st_sweep);

                PlanSweepSpec sweep_cfg{};
                sweep_cfg.enabled = true;
                sweep_cfg.engine = PlanSweepEngineKind::Exhaustive;
                sweep_cfg.top_k = 2;
                sweep_cfg.max_evals = 2;
                SweepParamRange last_n_range{};
                last_n_range.values = { 2.0, 3.0 };
                last_n_range.is_int = true;
                sweep_cfg.params["last_n"] = last_n_range;
                st_sweep.sweep = sweep_cfg;

                PlanSweepRuntimeOptions opt{};
                const DataView st_dv(*st_bundle);
                std::ostringstream sweep_log;
                BacktestResult sweep_best{};
                const bool sweep_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_sweep, opt, sweep_log, &sweep_best);
                t.expect(sweep_ok,
                    "custom runtime executes exhaustive sweep for st.radius_mean through compiled-program template path");
                const std::string sweep_msg = sweep_log.str();
                t.expect(sweep_msg.find("Compile failures: 0") != std::string::npos,
                    "st.radius_mean sweep reports zero compile failures for valid param overrides");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "compile-bridge sweep test creates temporal DataView input");
            if (!okdv || !bundle) return true;

            PlanSpec sweep_spec{};
            sweep_spec.version = 1;
            sweep_spec.name = "contract_compile_bridge_sweep_runtime";
            sweep_spec.trade_mode = TradeMode::LongOnly;
            sweep_spec.params["n_fast"] = static_cast<std::int64_t>(2);

            NodeSpec sw_close{};
            sw_close.id = "c";
            sw_close.op = "core.close";

            NodeSpec sw_sma{};
            sw_sma.id = "sma";
            sw_sma.op = "ts.sma";
            sw_sma.args = { "c" };
            sw_sma.params["n"] = ParamRef{ "n_fast" };

            NodeSpec sw_thr{};
            sw_thr.id = "thr";
            sw_thr.op = "core.const";
            sw_thr.params["value"] = 102.5;

            sweep_spec.numeric_nodes = { sw_close, sw_sma, sw_thr };

            NodeSpec sw_entry{};
            sw_entry.id = "entry";
            sw_entry.op = "cmp.gt";
            sw_entry.args = { "sma", "thr" };

            NodeSpec sw_exit{};
            sw_exit.id = "exit";
            sw_exit.op = "cmp.lt";
            sw_exit.args = { "sma", "thr" };

            sweep_spec.bool_nodes = { sw_entry, sw_exit };
            sweep_spec.bind.entry_long = "entry";
            sweep_spec.bind.exit_long = "exit";

            PlanSweepSpec sweep_cfg{};
            sweep_cfg.enabled = true;
            sweep_cfg.engine = PlanSweepEngineKind::Exhaustive;
            sweep_cfg.top_k = 3;
            sweep_cfg.max_evals = 3;
            SweepParamRange n_fast_range{};
            n_fast_range.values = { 2.0, 3.0, 4.0 };
            n_fast_range.is_int = true;
            sweep_cfg.params["n_fast"] = n_fast_range;
            sweep_spec.sweep = sweep_cfg;

            PlanSweepRuntimeOptions opt{};
            const DataView dv(*bundle);
            BacktestResult manual_best{};
            double manual_best_score = -std::numeric_limits<double>::infinity();
            for (const std::int64_t n_fast : { static_cast<std::int64_t>(2), static_cast<std::int64_t>(3), static_cast<std::int64_t>(4) }) {
                PlanSpec single = sweep_spec;
                single.sweep.reset();
                single.params["n_fast"] = n_fast;

                std::ostringstream single_log;
                BacktestResult single_result{};
                const bool single_ok = run_custom_plan_spec(dv, primary, single, opt, single_log, &single_result);
                t.expect(single_ok, "manual sweep candidate compiles/runs for n_fast=" + std::to_string(n_fast));
                if (!single_ok) {
                    t.out << "    manual sweep candidate error (" << n_fast << "): " << single_log.str() << "\n";
                    continue;
                }
                if (single_result.pnl > manual_best_score) {
                    manual_best_score = single_result.pnl;
                    manual_best = single_result;
                }
            }

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            const bool sweep_ok = run_custom_plan_spec(dv, primary, sweep_spec, opt, sweep_log, &sweep_best);
            t.expect(sweep_ok, "custom runtime executes exhaustive sweep through compiled-program template path");
            const std::string sweep_msg = sweep_log.str();
            t.expect(sweep_msg.find("Compile failures: 0") != std::string::npos,
                "sweep reports zero compile failures for valid param overrides");
            if (sweep_ok && std::isfinite(manual_best_score)) {
                t.expect(nearly_equal(sweep_best.pnl, manual_best_score),
                    "sweep best objective matches manual best candidate score");
            }
        }

        return true;
    }

    bool test_compile_bridge_failure_path_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_failure_path] compile-bridge explicit failure-path contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge failure-path spatial fixture builds");
            if (spatial_dv_ok && spatial_bundle) {
                PlanSpec spatial_bad_topology = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_spatial_runtime_bad_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spatial_bad_topology);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_bad_log;
                BacktestResult spatial_bad_run{};
                const bool spatial_bad_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_topology, opt, spatial_bad_log, &spatial_bad_run);
                t.expect(!spatial_bad_ok,
                    "custom PlanSpec runtime rejects unavailable named topology for spatial.radius_mean");
                t.expect(spatial_bad_log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "spatial runtime validation reports missing named topology explicitly");

                PlanSpec spatial_bad_subject = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_spatial_runtime_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spatial_bad_subject);

                std::ostringstream spatial_bad_subject_log;
                BacktestResult spatial_bad_subject_run{};
                const bool spatial_bad_subject_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_subject, opt, spatial_bad_subject_log, &spatial_bad_subject_run);
                t.expect(!spatial_bad_subject_ok,
                    "custom PlanSpec runtime rejects unresolved locality subject for spatial.radius_mean");
                const std::string spatial_bad_subject_msg = spatial_bad_subject_log.str();
                t.expect(spatial_bad_subject_msg.find("locality subject could not be resolved") != std::string::npos,
                    "spatial runtime validation reports locality-subject resolution failure explicitly");
                t.expect(spatial_bad_subject_msg.find("series not found") != std::string::npos,
                    "spatial runtime validation preserves missing-series detail for locality subject");
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge failure-path spatiotemporal fixture builds");
            if (st_dv_ok && st_bundle) {
                PlanSpec st_bad_topology = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_st_runtime_bad_topology",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(st_bad_topology);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_bad_log;
                BacktestResult st_bad_run{};
                const bool st_bad_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_bad_topology, opt, st_bad_log, &st_bad_run);
                t.expect(!st_bad_ok,
                    "custom PlanSpec runtime rejects topology family mismatches for st.radius_mean");
                const std::string st_bad_msg = st_bad_log.str();
                t.expect(st_bad_msg.find("has family 'temporal_window'") != std::string::npos,
                    "spatio-temporal runtime validation reports actual mismatched topology family");
                t.expect(st_bad_msg.find("spatiotemporal_composite") != std::string::npos,
                    "spatio-temporal runtime validation reports required topology family");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "compile-bridge failure-path temporal fixture builds");
            if (!okdv || !bundle) return true;

            PlanSpec bad{};
            bad.version = 1;
            bad.name = "contract_compile_bridge_no_fallback";
            bad.trade_mode = TradeMode::LongOnly;

            NodeSpec n_close{};
            n_close.id = "c";
            n_close.op = "core.close";

            NodeSpec n_bad{};
            n_bad.id = "sma";
            n_bad.op = "bad.unsupported_op";
            n_bad.args = { "c" };

            NodeSpec n_thr{};
            n_thr.id = "thr";
            n_thr.op = "core.const";
            n_thr.params["value"] = 100.0;

            bad.numeric_nodes = { n_close, n_bad, n_thr };

            NodeSpec bad_entry{};
            bad_entry.id = "entry_bad";
            bad_entry.op = "cmp.gt";
            bad_entry.args = { "c", "thr" };

            NodeSpec bad_exit{};
            bad_exit.id = "exit_bad";
            bad_exit.op = "cmp.lt";
            bad_exit.args = { "c", "thr" };

            bad.bool_nodes = { bad_entry, bad_exit };
            bad.bind.entry_long = "entry_bad";
            bad.bind.exit_long = "exit_bad";

            const DataView dv(*bundle);
            std::ostringstream bad_log;
            BacktestResult bad_r{};
            const bool bad_ok = run_custom_plan_spec(dv, primary, bad, opt, bad_log, &bad_r);
            t.expect(!bad_ok, "custom runtime hard-fails when compile bridge cannot compile node op");
            const std::string bad_msg = bad_log.str();
            const bool explicit_fail_msg =
                (bad_msg.find("PlanSpec compile failed:") != std::string::npos) ||
                (bad_msg.find("PlanSpec runtime validation failed:") != std::string::npos);
            t.expect(explicit_fail_msg,
                "compile/validation failure path is explicit in custom runtime output");
            t.expect(bad_msg.find("Unsupported numeric op in multi-series mode") == std::string::npos,
                "legacy evaluator error signature is absent (fallback path unreachable)");
        }

        return true;
    }

bool test_rowwise_runtime_branching_contracts(ContractAsserts& t) {
        t.out << "[rowwise_runtime_branching] row-wise numeric branching contracts\n";

        const std::vector<double> close = {
            100.0, 104.0, 101.0, 105.0, 99.0, 106.0, 98.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "row-wise branching test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_plan = [&](PlanSpec spec, BacktestResult& out, std::string& log_out) -> bool {
            std::ostringstream run_log;
            const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
            log_out = run_log.str();
            return ok;
        };

        PlanSpec select_spec{};
        select_spec.version = 1;
        select_spec.name = "contract_rowwise_core_select";
        select_spec.trade_mode = TradeMode::LongOnly;
        select_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "thr", "core.const", {}, { {"value", 103.0} } },
            NodeSpec{ "pos", "core.const", {}, { {"value", 1.0} } },
            NodeSpec{ "neg", "core.const", {}, { {"value", -1.0} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
            NodeSpec{ "x", "core.select", { "cond", "pos", "neg" }, {} },
        };
        select_spec.bool_nodes = {
            NodeSpec{ "cond", "cmp.gt", { "c", "thr" }, {} },
            NodeSpec{ "entry", "cmp.gt", { "x", "zero" }, {} },
            NodeSpec{ "exit", "cmp.lt", { "x", "zero" }, {} },
        };
        select_spec.bind.entry_long = "entry";
        select_spec.bind.exit_long = "exit";

        PlanSpec direct_spec{};
        direct_spec.version = 1;
        direct_spec.name = "contract_rowwise_direct_compare";
        direct_spec.trade_mode = TradeMode::LongOnly;
        direct_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "thr", "core.const", {}, { {"value", 103.0} } },
        };
        direct_spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "c", "thr" }, {} },
            NodeSpec{ "exit", "cmp.lt", { "c", "thr" }, {} },
        };
        direct_spec.bind.entry_long = "entry";
        direct_spec.bind.exit_long = "exit";

        BacktestResult select_out{};
        BacktestResult direct_out{};
        std::string select_log;
        std::string direct_log;
        const bool select_ok = run_plan(select_spec, select_out, select_log);
        const bool direct_ok = run_plan(direct_spec, direct_out, direct_log);
        t.expect(select_ok, "core.select runtime contract executes");
        t.expect(direct_ok, "direct compare runtime contract executes");
        if (!select_ok) t.out << "    core.select error: " << select_log << "\n";
        if (!direct_ok) t.out << "    direct compare error: " << direct_log << "\n";
        if (select_ok && direct_ok) {
            t.expect(select_out.trades > 0, "core.select runtime contract produces trades");
            t.expect(backtest_equal(select_out, direct_out),
                "core.select remains runtime-equivalent to direct bool threshold routing");
        }

        return true;
    }

bool test_ts_alias_runtime_parity_contracts(ContractAsserts& t) {
        t.out << "[ts_alias_runtime_parity] TS runtime alias parity smoke contracts\n";

        std::vector<double> close;
        close.reserve(32);
        for (std::size_t i = 0; i < 32; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "ts alias parity test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_case = [&](std::string_view op_name,
            const std::vector<std::string>& args,
            const ParamMap& params,
            double threshold,
            BacktestResult& out,
            std::string& log_out) -> bool {
                PlanSpec spec{};
                spec.version = 1;
                spec.name = "contract_ts_alias_" + std::string(op_name);
                spec.trade_mode = TradeMode::LongOnly;

                NodeSpec n_close{};
                n_close.id = "c";
                n_close.op = "core.close";

                NodeSpec n_zero{};
                n_zero.id = "z";
                n_zero.op = "core.const";
                n_zero.params["value"] = 0.0;

                NodeSpec n_one{};
                n_one.id = "one";
                n_one.op = "core.const";
                n_one.params["value"] = 1.0;

                NodeSpec n_two{};
                n_two.id = "two";
                n_two.op = "core.const";
                n_two.params["value"] = 2.0;

                NodeSpec n_thr{};
                n_thr.id = "thr";
                n_thr.op = "core.const";
                n_thr.params["value"] = threshold;

                NodeSpec n_out{};
                n_out.id = "x";
                n_out.op = std::string(op_name);
                n_out.args = args;
                n_out.params = params;

                spec.numeric_nodes = { n_close, n_zero, n_one, n_two, n_thr, n_out };

                NodeSpec b_entry{};
                b_entry.id = "entry";
                b_entry.op = "cmp.gt";
                b_entry.args = { "x", "thr" };

                NodeSpec b_exit{};
                b_exit.id = "exit";
                b_exit.op = "cmp.lt";
                b_exit.args = { "x", "thr" };

                spec.bool_nodes = { b_entry, b_exit };
                spec.bind.entry_long = "entry";
                spec.bind.exit_long = "exit";

                std::ostringstream run_log;
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
                log_out = run_log.str();
                return ok;
            };

        struct AliasCase {
            const char* alias;
            const char* canonical;
            std::vector<std::string> args;
            ParamMap params;
            double threshold;
        };

        const std::vector<AliasCase> cases = {
            { "ts.stdev", "ts.stddev", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.stdev_ema", "ts.stddev_ema", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.atr_w", "ts.atr_wilder", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.dc_upper", "ts.donchian_upper", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 110.5 },
            { "ts.dc_lower", "ts.donchian_lower", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 108.5 },
            { "ts.dc_mid", "ts.donchian_mid", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 109.5 },
            { "ts.dc_width", "ts.donchian_width", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 1.5 },
            { "ts.trange", "ts.true_range", { "c" }, ParamMap{}, 0.5 },
            { "ts.trange_ohlc", "ts.true_range_ohlc", { "c", "c", "c" }, ParamMap{}, 0.5 },
        };

        for (const auto& test_case : cases) {
            BacktestResult alias_result{};
            BacktestResult canonical_result{};
            std::string alias_log;
            std::string canonical_log;

            const bool alias_ok = run_case(
                test_case.alias,
                test_case.args,
                test_case.params,
                test_case.threshold,
                alias_result,
                alias_log);
            const bool canonical_ok = run_case(
                test_case.canonical,
                test_case.args,
                test_case.params,
                test_case.threshold,
                canonical_result,
                canonical_log);

            t.expect(alias_ok, std::string("alias runtime smoke executes '") + test_case.alias + "'");
            t.expect(canonical_ok, std::string("canonical runtime smoke executes '") + test_case.canonical + "'");
            if (!alias_ok) {
                t.out << "    alias runtime error (" << test_case.alias << "): " << alias_log << "\n";
            }
            if (!canonical_ok) {
                t.out << "    canonical runtime error (" << test_case.canonical << "): " << canonical_log << "\n";
            }
            if (alias_ok && canonical_ok) {
                t.expect(alias_result.trades > 0 || canonical_result.trades > 0,
                    std::string("alias parity case is signal-sensitive for '") + test_case.alias + "'");
                t.expect(backtest_equal(alias_result, canonical_result),
                    std::string("alias runtime parity holds for '") + test_case.alias +
                    "' -> '" + test_case.canonical + "'");
            }
        }

        return true;
    }

    bool test_ts_runtime_equivalence_smoke_contracts(ContractAsserts& t) {
        t.out << "[ts_runtime_equivalence_smoke] TS runtime representative equivalence smoke contracts\n";

        std::vector<double> close;
        close.reserve(32);
        for (std::size_t i = 0; i < 32; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "ts runtime equivalence smoke creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_plan = [&](PlanSpec spec, BacktestResult& out, std::string& log_out) -> bool {
            std::ostringstream run_log;
            const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
            log_out = run_log.str();
            return ok;
        };

        auto add_threshold_bindings = [](PlanSpec& spec, std::string threshold_node = "thr") {
            NodeSpec b_entry{};
            b_entry.id = "entry";
            b_entry.op = "cmp.gt";
            b_entry.args = { "x", std::move(threshold_node) };

            NodeSpec b_exit{};
            b_exit.id = "exit";
            b_exit.op = "cmp.lt";
            b_exit.args = { "x", "thr" };

            spec.bool_nodes = { b_entry, b_exit };
            spec.bind.entry_long = "entry";
            spec.bind.exit_long = "exit";
        };

        {
            PlanSpec neg_spec{};
            neg_spec.version = 1;
            neg_spec.name = "contract_ts_equiv_neg";
            neg_spec.trade_mode = TradeMode::LongOnly;
            neg_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "minus_one", "core.const", {}, { {"value", -1.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "x", "ts.neg", { "c" }, {} },
            };
            add_threshold_bindings(neg_spec);

            PlanSpec mul_spec{};
            mul_spec.version = 1;
            mul_spec.name = "contract_ts_equiv_mul";
            mul_spec.trade_mode = TradeMode::LongOnly;
            mul_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "minus_one", "core.const", {}, { {"value", -1.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "x", "ts.mul", { "c", "minus_one" }, {} },
            };
            add_threshold_bindings(mul_spec);

            BacktestResult neg_out{};
            BacktestResult mul_out{};
            std::string neg_log;
            std::string mul_log;
            const bool neg_ok = run_plan(neg_spec, neg_out, neg_log);
            const bool mul_ok = run_plan(mul_spec, mul_out, mul_log);
            t.expect(neg_ok, "ts.neg representative runtime equivalence case executes");
            t.expect(mul_ok, "ts.mul representative runtime equivalence case executes");
            if (!neg_ok) t.out << "    ts.neg equivalence error: " << neg_log << "\n";
            if (!mul_ok) t.out << "    ts.mul equivalence error: " << mul_log << "\n";
            if (neg_ok && mul_ok) {
                t.expect(neg_out.trades > 0, "ts.neg representative equivalence case produces trades");
                t.expect(backtest_equal(neg_out, mul_out),
                    "ts.neg remains runtime-equivalent to multiplying by -1");
            }
        }

        {
            PlanSpec clamp_const_spec{};
            clamp_const_spec.version = 1;
            clamp_const_spec.name = "contract_ts_equiv_clamp_const";
            clamp_const_spec.trade_mode = TradeMode::LongOnly;
            clamp_const_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "lo", "core.const", {}, { {"value", -115.0} } },
                NodeSpec{ "hi", "core.const", {}, { {"value", -105.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "neg", "ts.neg", { "c" }, {} },
                NodeSpec{ "x", "ts.clamp_const", { "neg" }, { {"lo", -115.0}, {"hi", -105.0} } },
            };
            add_threshold_bindings(clamp_const_spec);

            PlanSpec clamp_spec{};
            clamp_spec.version = 1;
            clamp_spec.name = "contract_ts_equiv_clamp";
            clamp_spec.trade_mode = TradeMode::LongOnly;
            clamp_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "lo", "core.const", {}, { {"value", -115.0} } },
                NodeSpec{ "hi", "core.const", {}, { {"value", -105.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "neg", "ts.neg", { "c" }, {} },
                NodeSpec{ "x", "ts.clamp", { "neg", "lo", "hi" }, {} },
            };
            add_threshold_bindings(clamp_spec);

            BacktestResult clamp_const_out{};
            BacktestResult clamp_out{};
            std::string clamp_const_log;
            std::string clamp_log;
            const bool clamp_const_ok = run_plan(clamp_const_spec, clamp_const_out, clamp_const_log);
            const bool clamp_ok = run_plan(clamp_spec, clamp_out, clamp_log);
            t.expect(clamp_const_ok, "ts.clamp_const representative runtime equivalence case executes");
            t.expect(clamp_ok, "ts.clamp representative runtime equivalence case executes");
            if (!clamp_const_ok) t.out << "    ts.clamp_const equivalence error: " << clamp_const_log << "\n";
            if (!clamp_ok) t.out << "    ts.clamp equivalence error: " << clamp_log << "\n";
            if (clamp_const_ok && clamp_ok) {
                t.expect(clamp_const_out.trades > 0, "ts.clamp representative equivalence case produces trades");
                t.expect(backtest_equal(clamp_const_out, clamp_out),
                    "ts.clamp_const remains runtime-equivalent to ts.clamp with constant bounds");
            }
        }

        {
            PlanSpec tr_ohlc_spec{};
            tr_ohlc_spec.version = 1;
            tr_ohlc_spec.name = "contract_ts_equiv_tr_ohlc";
            tr_ohlc_spec.trade_mode = TradeMode::LongOnly;
            tr_ohlc_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.5} } },
                NodeSpec{ "x", "ts.true_range_ohlc", { "c", "c", "c" }, {} },
            };
            add_threshold_bindings(tr_ohlc_spec);

            PlanSpec abs_delta_spec{};
            abs_delta_spec.version = 1;
            abs_delta_spec.name = "contract_ts_equiv_abs_delta";
            abs_delta_spec.trade_mode = TradeMode::LongOnly;
            abs_delta_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.5} } },
                NodeSpec{ "delta", "ts.delta", { "c" }, {} },
                NodeSpec{ "x", "ts.abs", { "delta" }, {} },
            };
            add_threshold_bindings(abs_delta_spec);

            BacktestResult tr_ohlc_out{};
            BacktestResult abs_delta_out{};
            std::string tr_ohlc_log;
            std::string abs_delta_log;
            const bool tr_ohlc_ok = run_plan(tr_ohlc_spec, tr_ohlc_out, tr_ohlc_log);
            const bool abs_delta_ok = run_plan(abs_delta_spec, abs_delta_out, abs_delta_log);
            t.expect(tr_ohlc_ok, "ts.true_range_ohlc representative runtime equivalence case executes");
            t.expect(abs_delta_ok, "ts.abs(ts.delta) representative runtime equivalence case executes");
            if (!tr_ohlc_ok) t.out << "    ts.true_range_ohlc equivalence error: " << tr_ohlc_log << "\n";
            if (!abs_delta_ok) t.out << "    ts.abs(ts.delta) equivalence error: " << abs_delta_log << "\n";
            if (tr_ohlc_ok && abs_delta_ok) {
                t.expect(tr_ohlc_out.trades > 0, "ts.true_range_ohlc representative equivalence case produces trades");
                t.expect(backtest_equal(tr_ohlc_out, abs_delta_out),
                    "ts.true_range_ohlc remains runtime-equivalent to abs(delta) on identical OHLC inputs");
            }
        }

        {
            PlanSpec mid_spec{};
            mid_spec.version = 1;
            mid_spec.name = "contract_ts_equiv_dc_mid";
            mid_spec.trade_mode = TradeMode::LongOnly;
            mid_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 109.5} } },
                NodeSpec{ "x", "ts.donchian_mid", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
            };
            add_threshold_bindings(mid_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_dc_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "two", "core.const", {}, { {"value", 2.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", 109.5} } },
                NodeSpec{ "upper", "ts.donchian_upper", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "lower", "ts.donchian_lower", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "sum", "ts.add", { "upper", "lower" }, {} },
                NodeSpec{ "x", "ts.div", { "sum", "two" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult mid_out{};
            BacktestResult formula_out{};
            std::string mid_log;
            std::string formula_log;
            const bool mid_ok = run_plan(mid_spec, mid_out, mid_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(mid_ok, "ts.donchian_mid representative runtime equivalence case executes");
            t.expect(formula_ok, "donchian formula representative runtime equivalence case executes");
            if (!mid_ok) t.out << "    ts.donchian_mid equivalence error: " << mid_log << "\n";
            if (!formula_ok) t.out << "    donchian formula equivalence error: " << formula_log << "\n";
            if (mid_ok && formula_ok) {
                t.expect(mid_out.trades > 0, "ts.donchian_mid representative equivalence case produces trades");
                t.expect(backtest_equal(mid_out, formula_out),
                    "ts.donchian_mid remains runtime-equivalent to its upper/lower midpoint formula");
            }
        }

        {
            PlanSpec percentile_spec{};
            percentile_spec.version = 1;
            percentile_spec.name = "contract_ts_equiv_percentile";
            percentile_spec.trade_mode = TradeMode::LongOnly;
            percentile_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 111.5} } },
                NodeSpec{ "x", "ts.percentile", { "c" }, { {"n", static_cast<std::int64_t>(4)}, {"q", 100.0} } },
            };
            add_threshold_bindings(percentile_spec);

            PlanSpec roll_max_spec{};
            roll_max_spec.version = 1;
            roll_max_spec.name = "contract_ts_equiv_percentile_rollmax";
            roll_max_spec.trade_mode = TradeMode::LongOnly;
            roll_max_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 111.5} } },
                NodeSpec{ "x", "ts.roll_max", { "c" }, { {"n", static_cast<std::int64_t>(4)} } },
            };
            add_threshold_bindings(roll_max_spec);

            BacktestResult percentile_out{};
            BacktestResult roll_max_out{};
            std::string percentile_log;
            std::string roll_max_log;
            const bool percentile_ok = run_plan(percentile_spec, percentile_out, percentile_log);
            const bool roll_max_ok = run_plan(roll_max_spec, roll_max_out, roll_max_log);
            t.expect(percentile_ok, "ts.percentile representative runtime equivalence case executes");
            t.expect(roll_max_ok, "ts.roll_max percentile equivalence case executes");
            if (!percentile_ok) t.out << "    ts.percentile equivalence error: " << percentile_log << "\n";
            if (!roll_max_ok) t.out << "    ts.roll_max equivalence error: " << roll_max_log << "\n";
            if (percentile_ok && roll_max_ok) {
                t.expect(percentile_out.trades > 0, "ts.percentile representative equivalence case produces trades");
                t.expect(backtest_equal(percentile_out, roll_max_out),
                    "ts.percentile at q=100 remains runtime-equivalent to ts.roll_max");
            }
        }

        {
            PlanSpec macd_spec{};
            macd_spec.version = 1;
            macd_spec.name = "contract_ts_equiv_macd";
            macd_spec.trade_mode = TradeMode::LongOnly;
            macd_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", -0.0001} } },
                NodeSpec{ "x", "ts.macd", { "c" }, {
                    {"fast", static_cast<std::int64_t>(3)},
                    {"slow", static_cast<std::int64_t>(6)},
                    {"signal", static_cast<std::int64_t>(3)}
                } },
            };
            add_threshold_bindings(macd_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_macd_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", -0.0001} } },
                NodeSpec{ "fast", "ts.ema", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "slow", "ts.ema", { "c" }, { {"n", static_cast<std::int64_t>(6)} } },
                NodeSpec{ "line", "ts.sub", { "fast", "slow" }, {} },
                NodeSpec{ "signal", "ts.ema", { "line" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "x", "ts.sub", { "line", "signal" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult macd_out{};
            BacktestResult formula_out{};
            std::string macd_log;
            std::string formula_log;
            const bool macd_ok = run_plan(macd_spec, macd_out, macd_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(macd_ok, "ts.macd representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.macd formula equivalence case executes");
            if (!macd_ok) t.out << "    ts.macd equivalence error: " << macd_log << "\n";
            if (!formula_ok) t.out << "    ts.macd formula equivalence error: " << formula_log << "\n";
            if (macd_ok && formula_ok) {
                t.expect(macd_out.trades > 0, "ts.macd representative equivalence case produces trades");
                t.expect(backtest_equal(macd_out, formula_out),
                    "ts.macd remains runtime-equivalent to its EMA-difference histogram formula");
            }
        }

        {
            PlanSpec width_spec{};
            width_spec.version = 1;
            width_spec.name = "contract_ts_equiv_bollinger_width";
            width_spec.trade_mode = TradeMode::LongOnly;
            width_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.045} } },
                NodeSpec{ "x", "ts.bollinger_width", { "c" }, {
                    {"n", static_cast<std::int64_t>(5)},
                    {"k", 2.0}
                } },
            };
            add_threshold_bindings(width_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_bollinger_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.045} } },
                NodeSpec{ "mid", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(5)} } },
                NodeSpec{ "mid_abs", "ts.abs", { "mid" }, {} },
                NodeSpec{ "sd", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(5)} } },
                NodeSpec{ "scale", "core.const", {}, { {"value", 4.0} } },
                NodeSpec{ "num", "ts.mul", { "sd", "scale" }, {} },
                NodeSpec{ "x", "ts.div", { "num", "mid_abs" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult width_out{};
            BacktestResult formula_out{};
            std::string width_log;
            std::string formula_log;
            const bool width_ok = run_plan(width_spec, width_out, width_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(width_ok, "ts.bollinger_width representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.bollinger_width formula equivalence case executes");
            if (!width_ok) t.out << "    ts.bollinger_width equivalence error: " << width_log << "\n";
            if (!formula_ok) t.out << "    ts.bollinger_width formula equivalence error: " << formula_log << "\n";
            if (width_ok && formula_ok) {
                t.expect(width_out.trades > 0, "ts.bollinger_width representative equivalence case produces trades");
                t.expect(backtest_equal(width_out, formula_out),
                    "ts.bollinger_width remains runtime-equivalent to its normalized band-width formula");
            }
        }

        {
            PlanSpec ratio_spec{};
            ratio_spec.version = 1;
            ratio_spec.name = "contract_ts_equiv_volatility_ratio";
            ratio_spec.trade_mode = TradeMode::LongOnly;
            ratio_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.45} } },
                NodeSpec{ "x", "ts.volatility_ratio", { "c" }, {
                    {"fast_n", static_cast<std::int64_t>(3)},
                    {"slow_n", static_cast<std::int64_t>(6)}
                } },
            };
            add_threshold_bindings(ratio_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_volatility_ratio_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.45} } },
                NodeSpec{ "fast", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "slow", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(6)} } },
                NodeSpec{ "x", "ts.div", { "fast", "slow" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult ratio_out{};
            BacktestResult formula_out{};
            std::string ratio_log;
            std::string formula_log;
            const bool ratio_ok = run_plan(ratio_spec, ratio_out, ratio_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(ratio_ok, "ts.volatility_ratio representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.volatility_ratio formula equivalence case executes");
            if (!ratio_ok) t.out << "    ts.volatility_ratio equivalence error: " << ratio_log << "\n";
            if (!formula_ok) t.out << "    ts.volatility_ratio formula equivalence error: " << formula_log << "\n";
            if (ratio_ok && formula_ok) {
                t.expect(ratio_out.trades > 0, "ts.volatility_ratio representative equivalence case produces trades");
                t.expect(backtest_equal(ratio_out, formula_out),
                    "ts.volatility_ratio remains runtime-equivalent to fast/slow stddev ratio");
            }
        }

        {
            PlanSpec corr_spec{};
            corr_spec.version = 1;
            corr_spec.name = "contract_ms_correlation_smoke";
            corr_spec.trade_mode = TradeMode::LongOnly;
            corr_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.9} } },
                NodeSpec{ "x", "ms.correlation", { "c", "c" }, { {"n", static_cast<std::int64_t>(4)} } },
            };
            add_threshold_bindings(corr_spec);

            BacktestResult corr_out{};
            std::string corr_log;
            const bool corr_ok = run_plan(corr_spec, corr_out, corr_log);
            t.expect(corr_ok, "ms.correlation runtime smoke executes");
            if (!corr_ok) {
                t.out << "    ms.correlation smoke error: " << corr_log << "\n";
            }
            else {
                t.expect(corr_out.trades > 0, "ms.correlation smoke produces trades on perfectly correlated windows");
            }
        }

        return true;
    }

    bool test_assertion_hygiene_contracts(ContractAsserts& t) {
        t.out << "[assertion_hygiene] Test assertion-hygiene source scan\n";

        const std::filesystem::path repo_root = contract_repo_root_from_source();
        const std::vector<std::string> files = collect_assertion_hygiene_files(repo_root);

        bool all_ok = t.expect(!files.empty(), "assertion hygiene discovered test source files");
        for (const auto& rel : files) {
            std::string body;
            const bool loaded = read_text_file(repo_root / rel, body);
            const std::string load_label = std::string("assertion hygiene can read ") + rel;
            if (!t.expect(loaded, load_label)) {
                all_ok = false;
                continue;
            }

            std::vector<AssertionHygieneFinding> findings;
            collect_assertion_hygiene_findings(rel, strip_cpp_comments_and_literals(body), findings);
            std::sort(findings.begin(), findings.end(),
                [](const AssertionHygieneFinding& a, const AssertionHygieneFinding& b) {
                    if (a.rel_path != b.rel_path) return a.rel_path < b.rel_path;
                    if (a.line != b.line) return a.line < b.line;
                    return a.reason < b.reason;
                });

            const bool clean = findings.empty();
            const std::string clean_label =
                std::string("assertion hygiene excludes trivial always-true conditions: ") + rel;
            if (!t.expect(clean, clean_label)) {
                all_ok = false;
                for (const auto& finding : findings) {
                    t.out << "    " << finding.rel_path << ":" << finding.line
                        << " [" << finding.reason << "] " << finding.snippet << "\n";
                }
            }
        }

        return all_ok;
    }

    bool test_runtime_no_legacy_composite_include(ContractAsserts& t) {
        t.out << "[runtime_gates] Runtime no-legacy wrapper include gate\n";

        const std::filesystem::path repo_root = contract_repo_root_from_source();
        const std::vector<std::string> runtime_files = {
            "src/sweep/plan_sweep.cpp",
            "src/ops/op_module_core.cpp",
            "src/ops/op_module_ts.cpp",
            "src/ops/op_module_ms.cpp",
            "src/ops/op_module_logic.cpp",
            "src/ops/op_module_spatial.cpp",
            "src/app_support/strategy_menu.cpp",
            "src/sweep/panel_runner.cpp",
            "src/engine/strategy_executor.cpp",
            "app/entrypoints/main.cpp",
        };

        bool all_ok = true;
        for (const auto& rel : runtime_files) {
            const auto p = repo_root / rel;
            std::string body;
            const bool loaded = read_text_file(p, body);
            const std::string lbl_load = std::string("runtime gate can read ") + rel;
            if (!t.expect(loaded, lbl_load)) {
                all_ok = false;
                continue;
            }

            const std::vector<std::string> forbidden_headers = {
                "#include \"indicators_composite_numeric.h\"",
                "#include \"indicators_composite_bool.h\"",
                "#include \"indicators_donchian.h\"",
                "#include \"indicators_atr.h\"",
                "#include \"indicators_rsi.h\"",
                "#include \"strategy_donchian_breakout.h\"",
                "#include \"strategy_gated_diff_cross.h\"",
                "#include \"strategy_zscore_mean_reversion.h\"",
                "#include \"strategy_swing_breakout_validation.h\"",
            };
            for (const auto& inc : forbidden_headers) {
                const bool has_forbidden = body.find(inc) != std::string::npos;
                const std::string lbl_no_legacy = std::string("runtime file has no legacy include (")
                    + inc + "): " + rel;
                if (!t.expect(!has_forbidden, lbl_no_legacy)) {
                    all_ok = false;
                }
            }
        }

        {
            const auto cmake = repo_root / "CMakeLists.txt";
            std::string body;
            const bool loaded = read_text_file(cmake, body);
            if (!t.expect(loaded, "runtime gate can read CMakeLists.txt")) {
                all_ok = false;
            }
            else {
                const std::vector<std::string> forbidden_sources = {
                    "src/strategy_donchian_breakout.cpp",
                    "src/strategy_gated_diff_cross.cpp",
                    "src/strategy_zscore_mean_reversion.cpp",
                    "src/strategy_swing_breakout_validation.cpp",
                    "src/indicator_graph.cpp",
                    "src/legacy_runtime_adapters.cpp",
                };
                for (const auto& src : forbidden_sources) {
                    const bool present = body.find(src) != std::string::npos;
                    const std::string lbl = std::string("CMakeLists excludes retired hardcoded strategy source: ") + src;
                    if (!t.expect(!present, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "include/ingest/numeric_frame.h",
                "include/ingest/data_view.h",
                "include/sweep/strategy_plan.h",
                "include/engine/plan_compiler.h",
                "include/ops/op_registry.h",
                "include/engine/strategy_executor.h",
                "include/engine/compiled_program.h",
                "include/engine/program_evaluator.h",
            };
            const std::vector<std::string> forbidden = {
                "SeriesView",
                "CandleSeries",
                "IndicatorPtr",
                "BoolSignalPtr",
                "#include \"series.h\"",
                "#include \"series_view.h\"",
                "#include \"indicator.h\"",
                "#include \"indicator_graph.h\"",
                "#include \"indicator_precomputed.h\"",
                "#include \"signals_precomputed.h\"",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("stable header excludes narrow runtime contract token (")
                        + pattern + "): " + rel;
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const auto compiled_header = repo_root / "include/engine/compiled_program.h";
            std::string body;
            const bool loaded = read_text_file(compiled_header, body);
            if (!t.expect(loaded, "runtime gate can read include/engine/compiled_program.h for machine contract")) {
                all_ok = false;
            }
            else {
                const std::vector<std::string> required = {
                    "ProgramOutputShape",
                    "ProgramKernelAccess",
                    "ProgramScheduling",
                    "CompiledProgram",
                    "ProgramInputRef",
                };
                for (const auto& pattern : required) {
                    const std::string lbl =
                        std::string("compiled_program.h freezes runtime-machine token: ") + pattern;
                    if (!t.expect(body.find(pattern) != std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const auto evaluator_header = repo_root / "include/engine/program_evaluator.h";
            std::string body;
            const bool loaded = read_text_file(evaluator_header, body);
            if (!t.expect(loaded, "runtime gate can read include/engine/program_evaluator.h for evaluator contract")) {
                all_ok = false;
            }
            else if (!t.expect(body.find("ProgramSignalOutputs") != std::string::npos,
                "program_evaluator.h freezes ProgramSignalOutputs contract")) {
                all_ok = false;
            }
        }

        {
            const std::vector<std::string> files = {
                "src/engine/plan_compiler.cpp",
                "src/engine/program_evaluator.cpp",
                "src/engine/strategy_executor.cpp",
                "src/sweep/plan_sweep.cpp",
            };
            const std::vector<std::string> forbidden = {
                "SeriesView",
                "CandleSeries",
                "IndicatorPtr",
                "BoolSignalPtr",
                "#include \"series.h\"",
                "#include \"series_view.h\"",
                "#include \"indicator.h\"",
                "#include \"indicator_graph.h\"",
                "#include \"indicator_precomputed.h\"",
                "#include \"signals_precomputed.h\"",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("core runtime source excludes narrow engine token (")
                        + pattern + "): " + rel;
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const std::vector<std::string> retired_files = {
                "include/indicator_precomputed.h",
                "include/signals_precomputed.h",
                "include/series_view.h",
                "include/indicator.h",
                "include/indicator_graph.h",
                "src/indicator_graph.cpp",
                "src/legacy_runtime_adapters.h",
                "src/legacy_runtime_adapters.cpp",
            };
            for (const auto& rel : retired_files) {
                if (!t.expect(!std::filesystem::exists(repo_root / rel),
                    "active tree excludes retired compatibility file: " + rel)) {
                    all_ok = false;
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "src/contracts/contract_suite_internal.h",
                "src/contracts/contract_suite_data.cpp",
            };
            const std::vector<std::string> forbidden = {
                "SeriesView",
                "IndicatorPtr",
                "BoolSignalPtr",
                "MapIndicator",
                "BinaryMapIndicator",
                "CompareSignal",
                "CrossSignal",
                "#include \"series_view.h\"",
                "#include \"indicator.h\"",
                "#include \"indicator_graph.h\"",
                "make_single_series_dataview_from_candles",
                "make_single_series_dataview_from_series_view",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("contract suite excludes retired compatibility token (")
                        + pattern + "): " + rel;
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const auto rel = std::string("include/engine/strategy_executor.h");
            const auto p = repo_root / rel;
            std::string body;
            const bool loaded = read_text_file(p, body);
            if (!t.expect(loaded, "runtime gate can read include/engine/strategy_executor.h for adapter contract")) {
                all_ok = false;
            }
            else {
                const std::vector<std::string> forbidden = {
                    "make_single_series_dataview_from_series_view",
                    "make_single_series_dataview_from_candles",
                };
                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("StrategyExecutor public header excludes legacy adapter helper: ")
                        + pattern;
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "include/sweep/plan_sweep.h",
                "src/sweep/plan_sweep.cpp",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                const bool has_candle_adapter =
                    body.find("run_custom_plan_spec(\n        const CandleSeries& series,") != std::string::npos;
                const std::string lbl_no_adapter =
                    std::string("PlanSpec API excludes CandleSeries adapter overload: ") + rel;
                if (!t.expect(!has_candle_adapter, lbl_no_adapter)) {
                    all_ok = false;
                }

                const bool has_spec_only_runner =
                    body.find("run_custom_plan_spec(\n        const PlanSpec& spec,") != std::string::npos;
                const std::string lbl_no_spec_only_runner =
                    std::string("PlanSpec API excludes spec-only custom runner overload: ") + rel;
                if (!t.expect(!has_spec_only_runner, lbl_no_spec_only_runner)) {
                    all_ok = false;
                }
            }
        }

        {
            const auto p = repo_root / "src/sweep/plan_sweep.cpp";
            std::string body;
            const bool loaded = read_text_file(p, body);
            if (!t.expect(loaded, "runtime gate can read src/sweep/plan_sweep.cpp for dead-evaluator check")) {
                all_ok = false;
            }
            else {
                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                const std::vector<std::pair<std::string, std::string>> forbidden = {
                    { "struct MultiEvalCtx", "plan_sweep excludes legacy MultiEvalCtx scaffold" },
                    { "bool eval_num_node(", "plan_sweep excludes legacy numeric evaluator" },
                    { "bool eval_bool_node(", "plan_sweep excludes legacy bool evaluator" },
                    { "run_multi_plan_once(", "plan_sweep excludes legacy wrapper execution path" },
                    { "Unsupported numeric op in multi-series mode", "plan_sweep excludes legacy numeric fallback errors" },
                    { "Unsupported bool op in multi-series mode", "plan_sweep excludes legacy bool fallback errors" },
                };
                for (const auto& [pattern, label] : forbidden) {
                    if (!t.expect(body.find(pattern) == std::string::npos, label)) {
                        all_ok = false;
                    }
                }

                if (!t.expect(body.find("compile_plan(") == std::string::npos,
                    "plan_sweep excludes per-run compile_plan calls")) {
                    all_ok = false;
                }
                if (!t.expect(body.find("compile_plan_template(") != std::string::npos,
                    "plan_sweep uses compile-once template compilation")) {
                    all_ok = false;
                }
                if (!t.expect(body.find("instantiate_compiled_plan(") != std::string::npos,
                    "plan_sweep instantiates compiled plans per trial override")) {
                    all_ok = false;
                }
            }
        }

        {
            const auto rel = std::string("src/ops/op_registry_internal.h");
            const auto p = repo_root / rel;
            std::string body;
            const bool loaded = read_text_file(p, body);
            if (!t.expect(loaded, "runtime gate can read src/op_registry_internal.h")) {
                all_ok = false;
            }
            else {
                const std::vector<std::string> forbidden = {
                    "OpEntryRuntime",
                    "mutable_runtime(",
                    "runtime_binding(",
                    "build_num",
                    "build_bool",
                    "IndicatorPtr",
                    "BoolSignalPtr",
                    "numeric_leaves",
                    "bool_leaves",
                };
                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("op_registry_internal excludes dead graph-builder token (")
                        + pattern + ")";
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "src/ops/op_module_core.cpp",
                "src/ops/op_module_ts.cpp",
                "src/ops/op_module_ms.cpp",
                "src/ops/op_module_logic.cpp",
                "src/ops/op_module_spatial.cpp",
            };
            const std::vector<std::string> forbidden = {
                "mutable_runtime(",
                "IndicatorPtr",
                "BoolSignalPtr",
                "MapIndicator",
                "BinaryMapIndicator",
                "CloseIndicator",
                "bconst(",
                "cross_up(",
                "cross_down(",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                for (const auto& pattern : forbidden) {
                    const std::string lbl =
                        std::string("op module excludes graph-era builder token (")
                        + pattern + "): " + rel;
                    if (!t.expect(body.find(pattern) == std::string::npos, lbl)) {
                        all_ok = false;
                    }
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "include/engine/strategy_executor.h",
                "src/engine/strategy_executor.cpp",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                const bool has_series_view_overload =
                    body.find("StrategyExecutor::run(const SeriesView& view,") != std::string::npos ||
                    body.find("BacktestResult run(const SeriesView& view,") != std::string::npos;
                const bool has_candle_series_overload =
                    body.find("StrategyExecutor::run(const CandleSeries& series,") != std::string::npos ||
                    body.find("BacktestResult run(const CandleSeries& series,") != std::string::npos;
                if (!t.expect(!has_series_view_overload,
                    "StrategyExecutor API excludes SeriesView run overload: " + rel)) {
                    all_ok = false;
                }
                if (!t.expect(!has_candle_series_overload,
                    "StrategyExecutor API excludes CandleSeries run overload: " + rel)) {
                    all_ok = false;
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "include/sweep/strategy_menu.h",
                "src/app_support/strategy_menu.cpp",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                const bool has_candle_template_overload =
                    body.find("run_strategy_template(const CandleSeries& series,") != std::string::npos ||
                    body.find("run_strategy_template(\n        const CandleSeries& series,") != std::string::npos;
                if (!t.expect(!has_candle_template_overload,
                    "strategy_menu API excludes CandleSeries run_strategy_template overload: " + rel)) {
                    all_ok = false;
                }
            }
        }

        {
            const std::vector<std::string> files = {
                "include/sweep/sweep.h",
                "src/sweep/sweep.cpp",
                "src/sweep/sweep_session.cpp",
            };
            for (const auto& rel : files) {
                const auto p = repo_root / rel;
                std::string body;
                const bool loaded = read_text_file(p, body);
                const std::string lbl_load = std::string("runtime gate can read ") + rel;
                if (!t.expect(loaded, lbl_load)) {
                    all_ok = false;
                    continue;
                }

                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                if (!t.expect(body.find("CandleSeries") == std::string::npos,
                    "sweep architecture excludes CandleSeries contracts: " + rel)) {
                    all_ok = false;
                }
            }
        }

        {
            const auto active_header = repo_root / "include/backtester.h";
            const auto active_src = repo_root / "src/backtester.cpp";
            if (!t.expect(!std::filesystem::exists(active_header),
                "active tree excludes include/backtester.h")) {
                all_ok = false;
            }
            if (!t.expect(!std::filesystem::exists(active_src),
                "active tree excludes src/backtester.cpp")) {
                all_ok = false;
            }
        }

        {
            const auto cmake = repo_root / "CMakeLists.txt";
            std::string body;
            const bool loaded = read_text_file(cmake, body);
            if (!t.expect(loaded, "runtime gate can read CMakeLists.txt for backtester retirement")) {
                all_ok = false;
            }
            else if (!t.expect(body.find("src/backtester.cpp") == std::string::npos,
                "CMakeLists excludes retired backtester source")) {
                all_ok = false;
            }
        }

        return all_ok;
    }

} // namespace sugar::contract_suite_internal


