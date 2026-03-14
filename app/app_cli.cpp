#include "app_cli.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

#include "raw_table.h"
#include "utils.h"

namespace {

    using sugar::InputProfile;

    bool is_flag_with_value(std::string_view flag) {
        return
            flag == "--data-spec" ||
            flag == "--format" ||
            flag == "--prep-dump" ||
            flag == "--align-dump" ||
            flag == "--series-key-col" ||
            flag == "--calendar" ||
            flag == "--run-panel" ||
            flag == "--primary" ||
            flag == "--topk" ||
            flag == "--contract-section";
    }

    std::string first_non_option_arg(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string_view a(argv[i]);
            if (a.starts_with("--")) {
                if (is_flag_with_value(a) && i + 1 < argc) {
                    ++i;
                }
                continue;
            }
            return std::string(a);
        }
        return {};
    }

    bool has_flag(int argc, char** argv, std::string_view flag) {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == flag) return true;
        }
        return false;
    }

    int find_flag_index(int argc, char** argv, std::string_view flag) {
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == flag) return i;
        }
        return -1;
    }

    bool read_flag_value(int argc, char** argv, std::string_view flag, std::string& out) {
        const int i = find_flag_index(argc, argv, flag);
        if (i < 0 || i + 1 >= argc) return false;
        out = std::string(argv[i + 1]);
        return true;
    }

    std::string lower_copy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool parse_runtime_format(std::string_view s, InputProfile& out) {
        const std::string x = lower_copy(std::string(s));
        if (x == "auto") {
            out = InputProfile::Auto;
            return true;
        }
        if (x == "legacy_ohlcv_date" || x == "legacy" || x == "date_ohlcv") {
            out = InputProfile::LegacyOhlcvDate;
            return true;
        }
        if (x == "iso_ohlcv_time" || x == "iso" || x == "time_ohlcv") {
            out = InputProfile::IsoOhlcvTime;
            return true;
        }
        return false;
    }

    bool parse_calendar_policy(std::string_view s, sugar::align::CalendarPolicy& out) {
        const std::string x = lower_copy(std::string(s));
        if (x == "union") {
            out = sugar::align::CalendarPolicy::Union;
            return true;
        }
        if (x == "intersection") {
            out = sugar::align::CalendarPolicy::Intersection;
            return true;
        }
        return false;
    }

} // namespace

namespace sugar::app {

    bool parse_app_command(int argc, char** argv, AppCommand& out, std::string& err) {
        out = AppCommand{};

        if (has_flag(argc, argv, "--dump-ops") ||
            has_flag(argc, argv, "--dump_ops") ||
            has_flag(argc, argv, "--dumpops"))
        {
            out.kind = CommandKind::DumpOps;
            return true;
        }

        if (has_flag(argc, argv, "--run-contract-suite")) {
            out.kind = CommandKind::RunContractSuite;
            for (int i = 1; i < argc; ++i) {
                if (std::string_view(argv[i]) != "--contract-section") continue;
                if (i + 1 >= argc) {
                    err = "--contract-section requires <key>";
                    return false;
                }
                out.contract_suite.sections.emplace_back(argv[++i]);
            }
            return true;
        }

        const int prep_i = find_flag_index(argc, argv, "--prep-dump");
        if (prep_i >= 0) {
            if (prep_i + 2 >= argc) {
                err = "--prep-dump requires <data_spec.json> <csv_path>";
                return false;
            }
            out.kind = CommandKind::PrepDump;
            out.prep_dump.spec_path = sugar::normalize_path_for_runtime(std::string(argv[prep_i + 1]));
            out.prep_dump.csv_path = sugar::normalize_path_for_runtime(std::string(argv[prep_i + 2]));
            return true;
        }

        const int align_i = find_flag_index(argc, argv, "--align-dump");
        if (align_i >= 0) {
            if (align_i + 2 >= argc) {
                err = "--align-dump requires <data_spec.json> <csv_path>";
                return false;
            }

            out.kind = CommandKind::AlignDump;
            out.align_dump.spec_path = sugar::normalize_path_for_runtime(std::string(argv[align_i + 1]));
            out.align_dump.csv_path = sugar::normalize_path_for_runtime(std::string(argv[align_i + 2]));
            if (!read_flag_value(argc, argv, "--series-key-col", out.align_dump.series_key_col) ||
                out.align_dump.series_key_col.empty())
            {
                err = "--series-key-col <col> is required for --align-dump";
                return false;
            }

            std::string calendar_s;
            if (read_flag_value(argc, argv, "--calendar", calendar_s) &&
                !parse_calendar_policy(calendar_s, out.align_dump.calendar))
            {
                err = "--calendar must be union or intersection";
                return false;
            }
            return true;
        }

        const int panel_i = find_flag_index(argc, argv, "--run-panel");
        if (panel_i >= 0) {
            if (panel_i + 2 >= argc) {
                err = "--run-panel requires <data_spec.json> <csv_path>";
                return false;
            }

            out.kind = CommandKind::RunPanel;
            out.run_panel.spec_path = sugar::normalize_path_for_runtime(std::string(argv[panel_i + 1]));
            out.run_panel.csv_path = sugar::normalize_path_for_runtime(std::string(argv[panel_i + 2]));
            (void)read_flag_value(argc, argv, "--primary", out.run_panel.primary_col);

            std::string topk_s;
            if (read_flag_value(argc, argv, "--topk", topk_s)) {
                try {
                    const int v = std::stoi(topk_s);
                    if (v <= 0) {
                        err = "--topk must be a positive integer";
                        return false;
                    }
                    out.run_panel.topk = static_cast<std::size_t>(v);
                }
                catch (...) {
                    err = "--topk must be a positive integer";
                    return false;
                }
            }
            return true;
        }

        out.kind = CommandKind::Interactive;
        const int data_spec_i = find_flag_index(argc, argv, "--data-spec");
        out.interactive.has_runtime_data_spec = (data_spec_i >= 0);
        if (out.interactive.has_runtime_data_spec) {
            if (data_spec_i + 1 >= argc) {
                err = "--data-spec requires <data_spec.json>";
                return false;
            }
            out.interactive.runtime_data_spec_path =
                sugar::normalize_path_for_runtime(std::string(argv[data_spec_i + 1]));
        }

        std::string fmt_arg;
        if (read_flag_value(argc, argv, "--format", fmt_arg) &&
            !parse_runtime_format(fmt_arg, out.interactive.requested_format))
        {
            err = "--format must be one of auto | legacy_ohlcv_date | iso_ohlcv_time";
            return false;
        }

        if (out.interactive.has_runtime_data_spec &&
            data_spec_i + 2 < argc &&
            !std::string_view(argv[data_spec_i + 2]).starts_with("--"))
        {
            out.interactive.csv_path = std::string(argv[data_spec_i + 2]);
        }
        else {
            out.interactive.csv_path = first_non_option_arg(argc, argv);
            if (out.interactive.has_runtime_data_spec &&
                out.interactive.csv_path == out.interactive.runtime_data_spec_path)
            {
                out.interactive.csv_path.clear();
            }
        }

        if (!out.interactive.csv_path.empty()) {
            out.interactive.csv_path = sugar::normalize_path_for_runtime(out.interactive.csv_path);
        }

        return true;
    }

} // namespace sugar::app
