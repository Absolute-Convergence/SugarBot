#include "contract_suite_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string>
#include <utility>
#include <vector>

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

        struct SourceSizePolicy {
            std::string family;
            std::size_t warning_lines{ 0 };
            std::size_t failure_lines{ 0 };
        };

        bool has_suffix(const std::string& text, const char* suffix) {
            const std::size_t suffix_len = std::char_traits<char>::length(suffix);
            return text.size() >= suffix_len &&
                text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
        }

        bool has_prefix(const std::string& text, const char* prefix) {
            const std::size_t prefix_len = std::char_traits<char>::length(prefix);
            return text.size() >= prefix_len &&
                text.compare(0, prefix_len, prefix) == 0;
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

        std::string lowercase_ascii(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return text;
        }

        bool is_source_size_sentinel_file(const std::filesystem::path& path) {
            const std::string ext = lowercase_ascii(path.extension().string());
            return ext == ".h" ||
                ext == ".hpp" ||
                ext == ".hh" ||
                ext == ".inl" ||
                ext == ".cpp" ||
                ext == ".cc" ||
                ext == ".cxx";
        }

        std::vector<std::string> collect_source_size_sentinel_files(const std::filesystem::path& repo_root) {
            std::vector<std::string> files;
            const std::array<const char*, 3> roots = {{ "include", "src", "app" }};

            for (const char* root_name : roots) {
                const auto root = repo_root / root_name;
                if (!std::filesystem::exists(root)) {
                    continue;
                }

                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         root,
                         std::filesystem::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file()) {
                        continue;
                    }
                    if (!is_source_size_sentinel_file(entry.path())) {
                        continue;
                    }
                    files.push_back(entry.path().lexically_relative(repo_root).generic_string());
                }
            }

            std::sort(files.begin(), files.end());
            return files;
        }

        SourceSizePolicy source_size_policy_for(const std::string& rel_path) {
            if (has_prefix(rel_path, "include/")) {
                return SourceSizePolicy{ "public header", 1200, 4000 };
            }
            if (has_prefix(rel_path, "src/contracts/") ||
                has_prefix(rel_path, "app/tests/")) {
                return SourceSizePolicy{ "test/contract source", 1800, 8000 };
            }
            return SourceSizePolicy{ "implementation source", 1800, 8000 };
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

        std::size_t source_line_count(const std::string& text) {
            if (text.empty()) {
                return 0;
            }

            const auto newlines = static_cast<std::size_t>(
                std::count(text.begin(), text.end(), '\n'));
            return newlines + ((text.back() == '\n') ? 0U : 1U);
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
    } // namespace

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

    bool test_contract_source_hygiene_contracts(ContractAsserts& t) {
        t.out << "[contract_source_hygiene] Dynamic source-size hygiene sentinels\n";

        const std::filesystem::path repo_root = contract_repo_root_from_source();
        const std::vector<std::string> files = collect_source_size_sentinel_files(repo_root);

        bool all_ok = t.expect(!files.empty(), "source-size sentinels discover source/header files dynamically");
        std::size_t scanned = 0;
        for (const auto& rel : files) {
            std::string body;
            const bool loaded = read_text_file(repo_root / rel, body);
            if (!loaded) {
                if (!t.expect(false, "source-size sentinel can read " + rel)) {
                    all_ok = false;
                }
                continue;
            }

            ++scanned;
            const SourceSizePolicy policy = source_size_policy_for(rel);
            const std::size_t lines = source_line_count(body);
            if (lines > policy.warning_lines) {
                t.warn(policy.family + " size warning: " + rel +
                    " has " + std::to_string(lines) +
                    " lines; warning sentinel is " + std::to_string(policy.warning_lines) +
                    ", failure sentinel is " + std::to_string(policy.failure_lines));
            }

            const bool below_failure_sentinel = lines <= policy.failure_lines;
            if (!below_failure_sentinel &&
                !t.expect(false, "source-size monolith sentinel is not exceeded: " + rel)) {
                all_ok = false;
                t.out << "    " << rel << " has " << lines
                    << " lines; failure sentinel is " << policy.failure_lines << "\n";
            }
        }
        t.out << "    scanned " << scanned << " source/header files for source-size sentinels\n";

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
                if (!t.expect(body.find("compile_plan_template(") == std::string::npos,
                    "plan_sweep orchestration no longer owns template compilation")) {
                    all_ok = false;
                }
                if (!t.expect(body.find("instantiate_compiled_plan(") == std::string::npos,
                    "plan_sweep orchestration no longer owns per-trial instantiation")) {
                    all_ok = false;
                }
            }
        }

        {
            const auto p = repo_root / "src/sweep/plan_sweep_runtime_adapter.cpp";
            std::string body;
            const bool loaded = read_text_file(p, body);
            if (!t.expect(loaded, "runtime gate can read src/sweep/plan_sweep_runtime_adapter.cpp")) {
                all_ok = false;
            }
            else {
                body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
                if (!t.expect(body.find("compile_plan(") == std::string::npos,
                    "plan_sweep runtime adapter excludes per-run compile_plan calls")) {
                    all_ok = false;
                }
                if (!t.expect(body.find("compile_plan_template(") != std::string::npos,
                    "plan_sweep runtime adapter owns compile-once template compilation")) {
                    all_ok = false;
                }
                if (!t.expect(body.find("instantiate_compiled_plan(") != std::string::npos,
                    "plan_sweep runtime adapter instantiates compiled plans per trial override")) {
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
