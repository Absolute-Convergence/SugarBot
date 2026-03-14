#include "app_runtime.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

#include "raw_table.h"
#include "run_pipeline.h"
#include "strategy_menu.h"
#include "utils.h"

namespace fs = std::filesystem;

namespace {

    std::string trimmed_copy(std::string s) {
        sugar::trim_in_place(s);
        return s;
    }

    std::string normalized_header_name(std::string_view name) {
        std::string out(name);
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        sugar::trim_in_place(out);
        return out;
    }

    bool has_col_lower(const std::vector<std::string>& cols, const char* name) {
        const std::string target = normalized_header_name(name);
        for (const auto& col : cols) {
            if (normalized_header_name(col) == target) return true;
        }
        return false;
    }

    bool find_col_index_lower(const std::vector<std::string>& cols, const char* name, std::size_t& out_idx) {
        const std::string target = normalized_header_name(name);
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (normalized_header_name(cols[i]) == target) {
                out_idx = i;
                return true;
            }
        }
        return false;
    }

    bool sample_column_value(const sugar::RawTable& tbl, std::size_t idx, std::string& out) {
        for (const auto& row : tbl.rows) {
            if (idx >= row.size()) continue;
            out = trimmed_copy(row[idx]);
            if (!out.empty()) return true;
        }
        return false;
    }

    bool sample_looks_iso8601_datetime(std::string_view tok) {
        int yyyymmdd = 0;
        int seconds_since_mid = 0;
        int tz_offset_minutes = 0;
        return sugar::parse_iso8601_datetime(tok, yyyymmdd, seconds_since_mid, tz_offset_minutes);
    }

    bool sample_looks_yyyymmdd_date(std::string_view tok) {
        return sugar::parse_yyyymmdd(tok).has_value();
    }

    bool detect_runtime_format_from_csv(const std::string& csv_path,
        sugar::InputProfile& out,
        std::string& reason,
        std::string& err)
    {
        bool had_header = false;
        std::string load_err;
        const sugar::RawTable tbl = sugar::load_csv_table(
            csv_path,
            sugar::CsvHeaderPolicy::AutoDetect,
            had_header,
            load_err);
        if (!load_err.empty()) {
            err = "format auto-detect failed: " + load_err;
            return false;
        }

        if (!had_header || tbl.headers.empty()) {
            out = sugar::InputProfile::LegacyOhlcvDate;
            reason = "no detected header row";
            return true;
        }

        const bool has_open = has_col_lower(tbl.headers, "open");
        const bool has_high = has_col_lower(tbl.headers, "high");
        const bool has_low = has_col_lower(tbl.headers, "low");
        const bool has_close = has_col_lower(tbl.headers, "close");
        const bool has_ohlc = has_open && has_high && has_low && has_close;
        const bool has_time = has_col_lower(tbl.headers, "time");
        const bool has_date = has_col_lower(tbl.headers, "date");
        std::size_t time_idx = 0;
        const bool have_time_idx = find_col_index_lower(tbl.headers, "time", time_idx);

        auto classify_time_sample = [&](const char* header_reason,
                                        const char* iso_reason,
                                        const char* date_reason) -> bool {
            std::string sample;
            if (have_time_idx && sample_column_value(tbl, time_idx, sample)) {
                if (sample_looks_iso8601_datetime(sample)) {
                    out = sugar::InputProfile::IsoOhlcvTime;
                    reason = iso_reason;
                    return true;
                }
                if (sample_looks_yyyymmdd_date(sample)) {
                    out = sugar::InputProfile::LegacyOhlcvDate;
                    reason = date_reason;
                    return true;
                }
            }

            out = sugar::InputProfile::IsoOhlcvTime;
            reason = header_reason;
            return true;
        };

        if (has_ohlc && has_time) {
            return classify_time_sample(
                "header contains time+OHLC columns",
                "header contains time+OHLC columns with ISO timestamp sample values",
                "header contains time+OHLC columns with date-like sample values");
        }
        if (has_ohlc && has_date) {
            out = sugar::InputProfile::LegacyOhlcvDate;
            reason = "header contains date+OHLC columns";
            return true;
        }
        if (has_time) {
            return classify_time_sample(
                "header contains time column",
                "header contains time column with ISO timestamp sample values",
                "header contains time column with date-like sample values");
        }
        if (has_date) {
            out = sugar::InputProfile::LegacyOhlcvDate;
            reason = "header contains date column";
            return true;
        }

        out = sugar::InputProfile::LegacyOhlcvDate;
        reason = "fallback default (no time/date header match)";
        return true;
    }

    const char* to_string(sugar::InputProfile f) {
        switch (f) {
        case sugar::InputProfile::Auto: return "auto";
        case sugar::InputProfile::LegacyOhlcvDate: return "legacy_ohlcv_date";
        case sugar::InputProfile::IsoOhlcvTime: return "iso_ohlcv_time";
        default: return "unknown";
        }
    }

    bool make_runtime_fallback_data_spec(sugar::InputProfile profile, sugar::DataSpec& out, std::string& err) {
        if (!sugar::make_data_spec_profile_defaults(profile, out, err)) {
            return false;
        }

        out.input_profile = profile;
        out.header_policy = sugar::HeaderPolicy::OptionalHeaders;
        out.dedupe = sugar::DataSpec::DedupePolicy::Last;
        return true;
    }

} // namespace

namespace sugar::app {

    std::string default_csv_path_string() {
        fs::path rel = fs::path("data") / "BTCUSD_420.csv";
        if (fs::exists(rel)) return rel.string();

#ifdef __linux__
        return fs::path("/mnt/c/Dev/sugar_Bot/data/BTCUSD_420.csv").string();
#else
        return fs::path("C:/Dev/sugar_Bot/data/BTCUSD_420.csv").string();
#endif
    }

    bool preprocess_to_frameset(const DataSpec& spec,
        const std::string& csv_path,
        FrameSet& out,
        PreprocessReport& out_report,
        std::string& err,
        const std::optional<align::AlignConfig>& align_cfg,
        std::shared_ptr<FrameSchema> schema)
    {
        return sugar::preprocess_to_frameset(spec, csv_path, out, out_report, err, align_cfg, std::move(schema));
    }

    bool align_frameset(const FrameSet& fs,
        const align::AlignConfig& cfg,
        std::shared_ptr<align::AlignedBundle>& out,
        std::string& err)
    {
        return sugar::align_frameset(fs, cfg, out, err);
    }

    std::string pick_primary_entity_key(const FrameSet& fs) {
        return sugar::pick_primary_entity_key(fs);
    }

    bool build_aligned_runtime_input(const DataSpec& spec,
        const std::string& csv_path,
        const std::string& primary_col_name,
        const align::AlignConfig& cfg,
        AlignedRuntimeInput& out,
        std::string& err)
    {
        return sugar::build_resolved_runtime_input(spec, csv_path, primary_col_name, cfg, out, err);
    }

    bool build_interactive_runtime_session(const InteractiveCommand& cmd,
        const std::string& csv_path,
        RuntimeSession& out,
        std::string& err)
    {
        out = RuntimeSession{};
        const std::string normalized_csv_path = sugar::normalize_path_for_runtime(csv_path);

        if (normalized_csv_path.empty()) {
            err = "csv path is empty";
            return false;
        }

        sugar::DataSpec ingest_spec{};
        if (cmd.has_runtime_data_spec) {
            if (!load_data_spec_file(cmd.runtime_data_spec_path, ingest_spec, err)) {
                err = "failed to load DataSpec: " + err;
                return false;
            }
            if (cmd.requested_format != InputProfile::Auto) {
                out.notices.emplace_back("[Info] --format ignored because --data-spec is explicit.");
            }
        }
        else {
            InputProfile resolved = cmd.requested_format;
            std::string detect_reason;
            if (resolved == InputProfile::Auto) {
                std::string detect_err;
                if (!detect_runtime_format_from_csv(normalized_csv_path, resolved, detect_reason, detect_err)) {
                    err = detect_err;
                    return false;
                }
            }

            switch (resolved) {
            case InputProfile::LegacyOhlcvDate:
                break;
            case InputProfile::IsoOhlcvTime:
                break;
            case InputProfile::Auto:
            default:
                resolved = InputProfile::LegacyOhlcvDate;
                break;
            }
            if (!make_runtime_fallback_data_spec(resolved, ingest_spec, err)) {
                err = "failed to build runtime format preset: " + err;
                return false;
            }

            std::ostringstream msg;
            msg << "[Info] runtime format preset: " << to_string(resolved);
            if (cmd.requested_format == InputProfile::Auto && !detect_reason.empty()) {
                msg << " (auto-detected: " << detect_reason << ")";
            }
            out.notices.push_back(msg.str());
        }

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;

        if (!build_aligned_runtime_input(
            ingest_spec,
            normalized_csv_path,
            ingest_spec.primary_value_col,
            cfg,
            out.runtime,
            err))
        {
            return false;
        }

        return true;
    }

    bool build_run_panel_runtime_input(const RunPanelCommand& cmd,
        AlignedRuntimeInput& out,
        std::string& err)
    {
        sugar::DataSpec ds{};
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.spec_path, ds, load_err)) {
            err = "failed to load DataSpec: " + load_err;
            return false;
        }

        const std::string primary_name = cmd.primary_col.empty() ?
            ds.primary_value_col :
            cmd.primary_col;

        sugar::align::AlignConfig cfg{};
        cfg.calendar = sugar::align::CalendarPolicy::Intersection;
        cfg.missing = sugar::align::MissingPolicy::NaN;
        return build_aligned_runtime_input(ds, cmd.csv_path, primary_name, cfg, out, err);
    }

    bool build_embedded_observation_runtime(const PlanSpec& spec,
        AlignedRuntimeInput& out,
        std::string& err)
    {
        return sugar::build_embedded_plan_observation_compat_runtime(spec, out, err);
    }

    bool load_custom_planspec_bundle(const sugar::CustomPlanSpecRunOptions& opt,
        PlanSpec& out_spec,
        sugar::PlanSweepRuntimeOptions& out_rt,
        bool& out_has_embedded_observations,
        std::string& err)
    {
        out_spec = PlanSpec{};
        out_rt = sugar::PlanSweepRuntimeOptions{};
        out_has_embedded_observations = false;

        if (!sugar::load_custom_planspec_runtime(opt, out_spec, out_rt, err)) {
            return false;
        }
        if (!sugar::plan_spec_has_embedded_observation_inputs(out_spec, out_has_embedded_observations, err)) {
            return false;
        }
        return true;
    }

} // namespace sugar::app
