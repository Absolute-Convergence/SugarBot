#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace sugar::app::tests {

    struct TestContext {
        std::ostream& out;
        int passed{ 0 };
        int failed{ 0 };

        void check(bool cond, std::string_view label);
    };

    struct PanelFixturePaths {
        std::string spec_path;
        std::string csv_path;
    };

    struct RunFixturePaths {
        std::string layout_path;
        std::string logic_path;
        std::string csv_path;
    };

    using AppTestFn = bool (*)(TestContext&);
    using AppTestRegistry = std::vector<AppTestFn>;

    std::filesystem::path find_repo_root();
    std::filesystem::path app_fixture_dir(std::string& err);

    bool create_time_header_daily_fixture(std::string& out_csv_path, std::string& err);
    bool create_spatial_only_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err);
    bool create_spatial_panel_fixture(
        std::string& out_spec_path,
        std::string& out_csv_path,
        std::string& err,
        bool enable_equal_length_row_domain = false);
    bool create_spatiotemporal_panel_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err);
    bool read_text_file(const std::filesystem::path& path, std::string& out, std::string& err);
    bool create_panel_fixture(PanelFixturePaths& out, std::string& err);
    bool create_unified_single_series_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_feature_dump_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_drop_row_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_panel_ranked_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_primary_series_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_panel_missing_run_fixture(RunFixturePaths& out, std::string& err);
    bool create_unified_legacy_logic_fixture(RunFixturePaths& out, std::string& err);
    bool create_relative_planspec_fixture(std::string& out_path, std::string& err);
    bool create_custom_planspec_fixture(bool include_series_inputs, std::string& out_path, std::string& err);
    bool create_custom_missing_runtime_ref_planspec_fixture(std::string& out_path, std::string& err);

    void register_cli_tests(AppTestRegistry& tests);
    void register_runtime_tests(AppTestRegistry& tests);
    void register_command_tests(AppTestRegistry& tests);
    void register_interactive_tests(AppTestRegistry& tests);
    void register_run_tests(AppTestRegistry& tests);

    bool run_all(std::ostream& out);

} // namespace sugar::app::tests
