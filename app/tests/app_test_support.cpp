#include "tests/app_test_support.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

namespace sugar::app::tests {

    namespace {

        namespace fs = std::filesystem;

        fs::path find_repo_root_from(fs::path cur) {
            while (!cur.empty()) {
                if (fs::exists(cur / "CMakeLists.txt")) return cur;
                const auto parent = cur.parent_path();
                if (parent == cur) break;
                cur = parent;
            }
            return {};
        }

    } // namespace

    void TestContext::check(bool cond, std::string_view label) {
        if (cond) {
            ++passed;
            out << "  [PASS] " << label << "\n";
        }
        else {
            ++failed;
            out << "  [FAIL] " << label << "\n";
        }
    }

    fs::path find_repo_root() {
#ifdef SUGARBOT_SOURCE_DIR
        const fs::path configured_root = fs::path(SUGARBOT_SOURCE_DIR);
        if (!configured_root.empty() && fs::exists(configured_root / "CMakeLists.txt")) {
            return configured_root;
        }
#endif

        if (const fs::path cwd_root = find_repo_root_from(fs::current_path()); !cwd_root.empty()) {
            return cwd_root;
        }

        return find_repo_root_from(fs::path(__FILE__).parent_path());
    }

    fs::path app_fixture_dir(std::string& err) {
        std::error_code ec;
        const fs::path tmp_root = fs::temp_directory_path(ec);
        if (ec) {
            err = "failed to resolve app fixture temp directory: " + ec.message();
            return {};
        }

        const fs::path fixture_dir = tmp_root / "sugar_app_tests";
        fs::create_directories(fixture_dir, ec);
        if (ec) {
            err = "failed to create app fixture directory: " + ec.message();
            return {};
        }
        return fixture_dir;
    }

    bool create_time_header_daily_fixture(std::string& out_csv_path, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path csv_path = fixture_dir / "app_tests_tmp_time_header_daily.csv";
        std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
        if (!csv_out) {
            err = "failed to open daily fixture CSV path for write";
            return false;
        }

        csv_out <<
            "time,open,high,low,close,Volume\n"
            "2024-01-01,100,101,99,100.5,1000\n"
            "2024-01-02,101,103,100,102.0,1100\n";

        out_csv_path = csv_path.string();
        return true;
    }

    bool create_spatial_only_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path spec_path = fixture_dir / "app_tests_tmp_spatial_only.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_spatial_only.csv";

        {
            std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
            if (!spec_out) {
                err = "failed to open spatial-only fixture DataSpec path for write";
                return false;
            }
            spec_out <<
                "{\n"
                "  \"header_policy\": \"RequiredHeaders\",\n"
                "  \"spatial_x_col\": \"x\",\n"
                "  \"spatial_y_col\": \"y\",\n"
                "  \"value_cols\": [\"close\"],\n"
                "  \"primary_value_col\": \"close\",\n"
                "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                "  \"sort_by_time\": true,\n"
                "  \"dedupe\": \"Error\"\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open spatial-only fixture CSV path for write";
                return false;
            }
            csv_out <<
                "x,y,close\n"
                "5,0,10\n"
                "1,0,20\n"
                "3,0,30\n";
        }

        out_spec_path = spec_path.string();
        out_csv_path = csv_path.string();
        return true;
    }

    bool create_spatial_panel_fixture(
        std::string& out_spec_path,
        std::string& out_csv_path,
        std::string& err,
        bool enable_equal_length_row_domain) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path spec_path = fixture_dir / "app_tests_tmp_spatial_panel.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_spatial_panel.csv";

        {
            std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
            if (!spec_out) {
                err = "failed to open spatial-panel fixture DataSpec path for write";
                return false;
            }
            spec_out <<
                "{\n"
                "  \"header_policy\": \"RequiredHeaders\",\n"
                "  \"entity_col\": \"region\",\n"
                "  \"spatial_x_col\": \"x\",\n"
                "  \"spatial_y_col\": \"y\",\n"
                "  \"value_cols\": [\"close\"],\n"
                "  \"primary_value_col\": \"close\",\n"
                "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                "  \"sort_by_time\": true,\n"
                "  \"dedupe\": \"Error\"";
            if (enable_equal_length_row_domain) {
                spec_out <<
                    ",\n"
                    "  \"alignment_non_temporal_multi_series\": \"EqualLengthRowDomain\"";
            }
            spec_out <<
                "\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open spatial-panel fixture CSV path for write";
                return false;
            }
            csv_out <<
                "region,x,y,close\n"
                "A,0,0,10\n"
                "A,1,0,11\n"
                "B,5,0,20\n"
                "B,6,0,21\n";
        }

        out_spec_path = spec_path.string();
        out_csv_path = csv_path.string();
        return true;
    }

    bool create_spatiotemporal_panel_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path spec_path = fixture_dir / "app_tests_tmp_spatiotemporal_panel.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_spatiotemporal_panel.csv";

        {
            std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
            if (!spec_out) {
                err = "failed to open spatiotemporal-panel fixture DataSpec path for write";
                return false;
            }
            spec_out <<
                "{\n"
                "  \"header_policy\": \"RequiredHeaders\",\n"
                "  \"time_col\": \"time\",\n"
                "  \"time_kind\": \"Iso8601\",\n"
                "  \"entity_col\": \"region\",\n"
                "  \"spatial_x_col\": \"x\",\n"
                "  \"spatial_y_col\": \"y\",\n"
                "  \"value_cols\": [\"close\"],\n"
                "  \"primary_value_col\": \"close\",\n"
                "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                "  \"sort_by_time\": true,\n"
                "  \"dedupe\": \"Error\"\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open spatiotemporal-panel fixture CSV path for write";
                return false;
            }
            csv_out <<
                "time,region,x,y,close\n"
                "2024-01-01T00:00:00Z,A,0.0,0.0,1.0\n"
                "2024-01-02T00:00:00Z,A,0.1,0.0,100.0\n"
                "2024-01-03T00:00:00Z,A,10.0,0.0,1.0\n"
                "2024-01-01T00:00:00Z,B,0.0,0.0,0.0\n"
                "2024-01-02T00:00:00Z,B,10.0,0.0,0.0\n"
                "2024-01-03T00:00:00Z,B,10.1,0.0,0.0\n";
        }

        out_spec_path = spec_path.string();
        out_csv_path = csv_path.string();
        return true;
    }

    bool read_text_file(const fs::path& path, std::string& out, std::string& err) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            err = "failed to open file: " + path.string();
            return false;
        }

        std::ostringstream oss;
        oss << in.rdbuf();
        out = oss.str();
        return true;
    }

    bool create_panel_fixture(PanelFixturePaths& out, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path spec_path = fixture_dir / "app_tests_tmp_iso_panel_with_entity.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_iso_panel.csv";

        {
            std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
            if (!spec_out) {
                err = "failed to open fixture DataSpec path for write";
                return false;
            }
            spec_out <<
                "{\n"
                "  \"time_col\": \"time\",\n"
                "  \"time_kind\": \"Iso8601\",\n"
                "  \"header_policy\": \"RequiredHeaders\",\n"
                "  \"entity_col\": \"symbol\",\n"
                "  \"value_cols\": [\"open\", \"high\", \"low\", \"close\", \"volume\"],\n"
                "  \"primary_value_col\": \"close\",\n"
                "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                "  \"sort_by_time\": true,\n"
                "  \"dedupe\": \"Error\"\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open fixture CSV path for write";
                return false;
            }
            csv_out <<
                "symbol,time,open,high,low,close,volume\n"
                "AAA,2024-01-01T00:00:00Z,100,101,99,100.5,1000\n"
                "BBB,2024-01-01T00:00:00Z,200,202,198,201.0,1500\n"
                "AAA,2024-01-02T00:00:00Z,101,103,100,102.0,1100\n"
                "BBB,2024-01-02T00:00:00Z,201,203,199,202.5,1600\n";
        }

        out.spec_path = spec_path.string();
        out.csv_path = csv_path.string();
        return true;
    }

    bool create_unified_single_series_fixture(RunFixturePaths& out, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path layout_path = fixture_dir / "app_tests_tmp_run_single_layout.json";
        const fs::path logic_path = fixture_dir / "app_tests_tmp_run_single_logic.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_run_single.csv";

        {
            std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
            if (!layout_out) {
                err = "failed to open unified single-series LayoutSpec path for write";
                return false;
            }
            layout_out <<
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "    \"missing_default\": { \"kind\": \"NaN\" },\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  }\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open unified single-series CSV path for write";
                return false;
            }
            csv_out <<
                "date,close\n"
                "20240101,10\n"
                "20240102,11\n";
        }

        {
            std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
            if (!logic_out) {
                err = "failed to open unified single-series logic path for write";
                return false;
            }
            logic_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_run_single\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
                "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"entry\",\n"
                "    \"exit_long\": \"exit\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";
        }

        out.layout_path = layout_path.string();
        out.logic_path = logic_path.string();
        out.csv_path = csv_path.string();
        return true;
    }

    bool create_unified_feature_dump_fixture(RunFixturePaths& out, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path layout_path = fixture_dir / "app_tests_tmp_feature_dump_layout.json";
        const fs::path logic_path = fixture_dir / "app_tests_tmp_feature_dump_logic.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_feature_dump.csv";

        {
            std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
            if (!layout_out) {
                err = "failed to open feature-dump LayoutSpec path for write";
                return false;
            }
            layout_out <<
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "    \"missing_default\": { \"kind\": \"NaN\" },\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  }\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open feature-dump CSV path for write";
                return false;
            }
            csv_out <<
                "date,close\n"
                "20240101,10\n"
                "20240102,12\n"
                "20240103,14\n";
        }

        {
            std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
            if (!logic_out) {
                err = "failed to open feature-dump logic path for write";
                return false;
            }
            logic_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_feature_dump\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"sma2\", \"op\": \"ts.sma\", \"args\": [\"c\"], \"params\": { \"n\": 2 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";
        }

        out.layout_path = layout_path.string();
        out.logic_path = logic_path.string();
        out.csv_path = csv_path.string();
        return true;
    }

    bool create_unified_drop_row_fixture(RunFixturePaths& out, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path layout_path = fixture_dir / "app_tests_tmp_drop_row_layout.json";
        const fs::path logic_path = fixture_dir / "app_tests_tmp_drop_row_logic.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_drop_row.csv";

        {
            std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
            if (!layout_out) {
                err = "failed to open drop-row LayoutSpec path for write";
                return false;
            }
            layout_out <<
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "    \"missing_default\": { \"kind\": \"DropRow\" },\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  }\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open drop-row CSV path for write";
                return false;
            }
            csv_out <<
                "date,close\n"
                "\n"
                "20240101,10\n"
                "20240102,\n"
                "20240103,12\n";
        }

        {
            std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
            if (!logic_out) {
                err = "failed to open drop-row logic path for write";
                return false;
            }
            logic_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_drop_row\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"entry\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";
        }

        out.layout_path = layout_path.string();
        out.logic_path = logic_path.string();
        out.csv_path = csv_path.string();
        return true;
    }

    bool create_unified_panel_ranked_fixture(RunFixturePaths& out, std::string& err) {
        const fs::path fixture_dir = app_fixture_dir(err);
        if (fixture_dir.empty()) {
            return false;
        }

        const fs::path layout_path = fixture_dir / "app_tests_tmp_run_panel_layout.json";
        const fs::path logic_path = fixture_dir / "app_tests_tmp_run_panel_logic.json";
        const fs::path csv_path = fixture_dir / "app_tests_tmp_run_panel.csv";

        {
            std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
            if (!layout_out) {
                err = "failed to open unified panel LayoutSpec path for write";
                return false;
            }
            layout_out <<
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"time\",\n"
                "    \"time_kind\": \"Iso8601\",\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"entity_col\": \"symbol\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "    \"missing_default\": { \"kind\": \"NaN\" },\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  },\n"
                "  \"topologies\": [\n"
                "    {\n"
                "      \"name\": \"semantic.regime.test\",\n"
                "      \"family\": \"semantic_graph\",\n"
                "      \"bearing\": \"metadata_only\",\n"
                "      \"metadata\": {\n"
                "        \"producer\": \"app_tests\"\n"
                "      }\n"
                "    }\n"
                "  ]\n"
                "}\n";
        }

        {
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open unified panel CSV path for write";
                return false;
            }
            csv_out <<
                "symbol,time,close\n"
                "AAA,2024-01-01T00:00:00Z,100.0\n"
                "BBB,2024-01-01T00:00:00Z,90.0\n"
                "AAA,2024-01-02T00:00:00Z,110.0\n"
                "BBB,2024-01-02T00:00:00Z,80.0\n";
        }

        {
            std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
            if (!logic_out) {
                err = "failed to open unified panel logic path for write";
                return false;
            }
            logic_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_run_ranked\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"run\": {\n"
                "    \"scope\": \"per_entity_ranked\",\n"
                "    \"top_k\": 2,\n"
                "    \"objective\": {\n"
                "      \"value\": { \"source\": \"reported_metric\", \"name\": \"pnl\" },\n"
                "      \"direction\": \"maximize\"\n"
                "    }\n"
                "  },\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
                "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"entry\",\n"
                "    \"exit_long\": \"exit\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";
        }

        out.layout_path = layout_path.string();
        out.logic_path = logic_path.string();
        out.csv_path = csv_path.string();
        return true;
    }

    bool create_unified_primary_series_fixture(RunFixturePaths& out, std::string& err) {
        if (!create_unified_panel_ranked_fixture(out, err)) {
            return false;
        }

        std::ofstream logic_out(out.logic_path, std::ios::binary | std::ios::trunc);
        if (!logic_out) {
            err = "failed to reopen unified primary-series logic path for write";
            return false;
        }
        logic_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_run_primary_series\",\n"
            "  \"trade_mode\": \"LongOnly\",\n"
            "  \"run\": {\n"
            "    \"scope\": \"primary_series\",\n"
            "    \"target_entity\": \"BBB\"\n"
            "  },\n"
            "  \"numeric_nodes\": [\n"
            "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
            "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
            "  ],\n"
            "  \"bool_nodes\": [\n"
            "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
            "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"entry\",\n"
            "    \"exit_long\": \"exit\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";
        return true;
    }

    bool create_unified_panel_missing_run_fixture(RunFixturePaths& out, std::string& err) {
        if (!create_unified_panel_ranked_fixture(out, err)) {
            return false;
        }

        std::ofstream logic_out(out.logic_path, std::ios::binary | std::ios::trunc);
        if (!logic_out) {
            err = "failed to reopen unified missing-run logic path for write";
            return false;
        }
        logic_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_run_missing_scope\",\n"
            "  \"trade_mode\": \"LongOnly\",\n"
            "  \"numeric_nodes\": [\n"
            "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
            "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
            "  ],\n"
            "  \"bool_nodes\": [\n"
            "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
            "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"entry\",\n"
            "    \"exit_long\": \"exit\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";
        return true;
    }

    bool create_unified_legacy_logic_fixture(RunFixturePaths& out, std::string& err) {
        if (!create_unified_panel_ranked_fixture(out, err)) {
            return false;
        }

        std::ofstream logic_out(out.logic_path, std::ios::binary | std::ios::trunc);
        if (!logic_out) {
            err = "failed to reopen unified legacy logic path for write";
            return false;
        }
        logic_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_run_legacy_logic\",\n"
            "  \"series_inputs\": [\n"
            "    {\n"
            "      \"key\": \"main\",\n"
            "      \"path\": \"app_tests_tmp_run_panel.csv\",\n"
            "      \"data_spec\": \"app_tests_tmp_run_panel_layout.json\",\n"
            "      \"entity\": \"AAA\"\n"
            "    }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"\",\n"
            "    \"exit_long\": \"\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";
        return true;
    }

    bool create_relative_planspec_fixture(std::string& out_path, std::string& err) {
        PanelFixturePaths fixture{};
        if (!create_panel_fixture(fixture, err)) {
            return false;
        }

        const fs::path plan_path = fs::path(fixture.spec_path).parent_path() / "app_tests_tmp_relative_plan.json";
        std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
        if (!plan_out) {
            err = "failed to open PlanSpec fixture path for write";
            return false;
        }

        plan_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_relative_embedded_observations\",\n"
            "  \"series_inputs\": [\n"
            "    {\n"
            "      \"key\": \"main\",\n"
            "      \"path\": \"app_tests_tmp_iso_panel.csv\",\n"
            "      \"data_spec\": \"app_tests_tmp_iso_panel_with_entity.json\",\n"
            "      \"entity\": \"AAA\"\n"
            "    }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"\",\n"
            "    \"exit_long\": \"\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";

        out_path = plan_path.string();
        return true;
    }

    bool create_custom_planspec_fixture(bool include_series_inputs, std::string& out_path, std::string& err) {
        PanelFixturePaths fixture{};
        if (!create_panel_fixture(fixture, err)) {
            return false;
        }

        const fs::path fixture_dir = fs::path(fixture.spec_path).parent_path();
        const fs::path plan_path = fixture_dir / (include_series_inputs ?
            "app_tests_tmp_custom_with_series_inputs.json" :
            "app_tests_tmp_custom_no_series_inputs.json");
        std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
        if (!plan_out) {
            err = "failed to open custom PlanSpec fixture path for write";
            return false;
        }

        plan_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_custom_positive_close\",\n";
        if (include_series_inputs) {
            plan_out <<
                "  \"series_inputs\": [\n"
                "    {\n"
                "      \"key\": \"main\",\n"
                "      \"path\": \"app_tests_tmp_iso_panel.csv\",\n"
                "      \"data_spec\": \"app_tests_tmp_iso_panel_with_entity.json\",\n"
                "      \"entity\": \"AAA\"\n"
                "    }\n"
                "  ],\n";
        }
        plan_out <<
            "  \"trade_mode\": \"LongOnly\",\n"
            "  \"numeric_nodes\": [\n"
            "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
            "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
            "  ],\n"
            "  \"bool_nodes\": [\n"
            "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
            "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"entry\",\n"
            "    \"exit_long\": \"exit\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";

        out_path = plan_path.string();
        return true;
    }

    bool create_custom_missing_runtime_ref_planspec_fixture(std::string& out_path, std::string& err) {
        PanelFixturePaths fixture{};
        if (!create_panel_fixture(fixture, err)) {
            return false;
        }

        const fs::path fixture_dir = fs::path(fixture.spec_path).parent_path();
        const fs::path plan_path = fixture_dir / "app_tests_tmp_custom_missing_runtime_ref.json";
        std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
        if (!plan_out) {
            err = "failed to open missing-runtime-ref PlanSpec fixture path for write";
            return false;
        }

        plan_out <<
            "{\n"
            "  \"version\": 1,\n"
            "  \"name\": \"app_tests_custom_missing_runtime_ref\",\n"
            "  \"trade_mode\": \"LongOnly\",\n"
            "  \"numeric_nodes\": [\n"
            "    { \"id\": \"other\", \"op\": \"core.ref\", \"params\": { \"ref\": \"ZZZ:close\" } },\n"
            "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
            "  ],\n"
            "  \"bool_nodes\": [\n"
            "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"other\", \"z\"] },\n"
            "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"other\", \"z\"] }\n"
            "  ],\n"
            "  \"bindings\": {\n"
            "    \"entry_long\": \"entry\",\n"
            "    \"exit_long\": \"exit\",\n"
            "    \"entry_short\": \"\",\n"
            "    \"exit_short\": \"\",\n"
            "    \"allow_trade\": \"\"\n"
            "  }\n"
            "}\n";

        out_path = plan_path.string();
        return true;
    }

} // namespace sugar::app::tests
