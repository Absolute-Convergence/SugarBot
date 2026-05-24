#include "contract_suite_internal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "legacy/legacy_candle_frame_adapter.h"
#include "ingest/csv.h"
#include "ingest/json_min.h"
#include "ingest/observation_capability.h"
#include "ops/op_kernels_math.h"
#include "op_kernels_ms.h"
#include "op_kernels_spatial.h"
#include "op_kernels_ts.h"
#include "ingest/preprocess.h"
#include "ingest/run_manifest.h"
#include "ingest/run_pipeline.h"

namespace sugar::contract_suite_internal {

    bool test_preprocess_missing_policy(ContractAsserts& t) {
        t.out << "[missing_policy] preprocess missing-token policy\n";

        const auto dir = contract_tmp_case_dir("missing_policy");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "missing_policy.csv";
        const std::string body =
            "date,close\n"
            "20240101,1\n"
            "20240102,NA\n"
            "20240103,3\n";
        if (!write_text_file(csv, body)) {
            return t.expect(false, "failed to write missing_policy fixture");
        }

        struct OwnedPreprocessRun {
            PreprocessResult result;
            std::shared_ptr<FrameSchema> schema;
        };

        auto run_with = [&](MissingKind mk, double cv = 0.0) -> OwnedPreprocessRun {
            OwnedPreprocessRun out{};
            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.missing_tokens = { "NA" };
            spec.missing_default.kind = mk;
            spec.missing_default.const_value = cv;
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            out.schema = std::make_shared<FrameSchema>();
            out.result = preprocess_csv(spec, csv.string(), out.schema);
            return out;
        };

        {
            auto run = run_with(MissingKind::Const, 7.0);
            auto& r = run.result;
            t.expect(r.err.empty(), "const missing policy preprocess succeeds");
            t.expect(r.frame.has_value(), "const missing policy produces NumericFrame");
            if (r.frame) {
                ColumnId close_id{};
                const bool ok_id = r.frame->schema().columns().find("close", close_id);
                t.expect(ok_id, "const policy resolves close column");
                if (ok_id) {
                    const auto c = r.frame->col(close_id);
                    t.expect(c.size() == 3, "const policy keeps all rows");
                    if (c.size() == 3) {
                        t.expect(nearly_equal(c[0], 1.0), "const policy row0 close=1");
                        t.expect(nearly_equal(c[1], 7.0), "const policy missing token replaced with const");
                        t.expect(nearly_equal(c[2], 3.0), "const policy row2 close=3");
                    }
                }
            }
        }

        {
            auto run = run_with(MissingKind::Ffill);
            auto& r = run.result;
            t.expect(r.err.empty(), "ffill missing policy preprocess succeeds");
            t.expect(r.frame.has_value(), "ffill missing policy produces NumericFrame");
            if (r.frame) {
                ColumnId close_id{};
                const bool ok_id = r.frame->schema().columns().find("close", close_id);
                t.expect(ok_id, "ffill policy resolves close column");
                if (ok_id) {
                    const auto c = r.frame->col(close_id);
                    t.expect(c.size() == 3, "ffill policy keeps all rows");
                    if (c.size() == 3) {
                        t.expect(nearly_equal(c[0], 1.0), "ffill row0 close=1");
                        t.expect(nearly_equal(c[1], 1.0), "ffill replaces missing with last value");
                        t.expect(nearly_equal(c[2], 3.0), "ffill row2 close=3");
                    }
                }
            }
        }

        {
            auto run = run_with(MissingKind::DropRow);
            auto& r = run.result;
            t.expect(r.err.empty(), "drop-row missing policy preprocess succeeds");
            t.expect(r.frame.has_value(), "drop-row missing policy produces NumericFrame");
            if (r.frame) {
                ColumnId close_id{};
                const bool ok_id = r.frame->schema().columns().find("close", close_id);
                t.expect(ok_id, "drop-row policy resolves close column");
                if (ok_id) {
                    const auto c = r.frame->col(close_id);
                    const auto time = r.frame->time();
                    t.expect(c.size() == 2, "drop-row removes row with missing token");
                    t.expect(time.size() == 2, "drop-row time axis shrinks consistently");
                    if (c.size() == 2 && time.size() == 2) {
                        t.expect(nearly_equal(c[0], 1.0) && nearly_equal(c[1], 3.0), "drop-row keeps non-missing values");
                        t.expect(time[0] == static_cast<TimeKey>(20240101) &&
                            time[1] == static_cast<TimeKey>(20240103),
                            "drop-row preserves expected time keys");
                    }
                }
            }
        }

        {
            const auto csv2 = dir / "headerless_ok.csv";
            const std::string body2 =
                "20240101,10,100\n"
                "20240102,11,110\n";
            if (!write_text_file(csv2, body2)) {
                return t.expect(false, "failed to write headerless_ok fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::HeaderlessPositional;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close", "volume" };
            spec.primary_value_col = "close";
            spec.columns_by_index = {
                {"date", 0},
                {"close", 1},
                {"volume", 2},
            };
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            auto schema = std::make_shared<FrameSchema>();
            auto r = preprocess_csv(spec, csv2.string(), schema);
            t.expect(r.err.empty(), "headerless positional preprocess succeeds with complete mapping");
            t.expect(r.frame.has_value(), "headerless positional yields NumericFrame");
            if (r.frame) {
                ColumnId close_id{};
                const bool ok_id = r.frame->schema().columns().find("close", close_id);
                t.expect(ok_id, "headerless positional resolves close column");
                if (ok_id) {
                    const auto c = r.frame->col(close_id);
                    t.expect(c.size() == 2 && nearly_equal(c[0], 10.0) && nearly_equal(c[1], 11.0),
                        "headerless positional maps close by index deterministically");
                }
            }
        }

        {
            const auto csv3 = dir / "headerless_incomplete.csv";
            const std::string body3 = "20240101,99\n";
            if (!write_text_file(csv3, body3)) {
                return t.expect(false, "failed to write headerless_incomplete fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::HeaderlessPositional;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.columns_by_index = { {"date", 0} };
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            auto schema = std::make_shared<FrameSchema>();
            auto r = preprocess_csv(spec, csv3.string(), schema);
            t.expect(!r.err.empty(), "headerless positional fails when mapping is incomplete");
            t.expect(r.err.find("incomplete columns_by_index mapping") != std::string::npos,
                "headerless positional failure message is explicit");
        }

        return true;
    }

    bool test_csv_row_skip_diagnostics_contracts(ContractAsserts& t) {
        t.out << "[csv_row_skip_diagnostics] legacy/raw-table CSV row-skip diagnostics\n";

        const auto dir = contract_tmp_case_dir("csv_row_skip_diagnostics");
        std::filesystem::create_directories(dir);

        {
            const auto csv = dir / "legacy_diagnostics.csv";
            const std::string body =
                "date,open,high,low,close,volume\n"
                "\n"
                "20240101,1,2,0,1.5,10\n"
                "20240102,1,2\n"
                "bad-date,1,2,0,1.5,10\n"
                "20240104,1,2,0,bad-close,10\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write legacy CSV diagnostics fixture");
            }

            CandleCsvDiagnostics diagnostics{};
            const auto rows = load_candles_csv(csv.string(), &diagnostics);
            t.expect(rows.size() == 1, "legacy CSV loader keeps only the valid row");
            t.expect(diagnostics.had_header, "legacy CSV diagnostics preserve header detection");
            t.expect(diagnostics.physical_line_count == 6, "legacy CSV diagnostics count physical lines");
            t.expect(diagnostics.blank_line_count == 1, "legacy CSV diagnostics count blank lines");
            t.expect(diagnostics.data_row_count == 4, "legacy CSV diagnostics count non-header data rows");
            t.expect(diagnostics.kept_row_count == 1, "legacy CSV diagnostics count kept rows");
            t.expect(diagnostics.skipped_short_row_count == 1, "legacy CSV diagnostics count short skipped rows");
            t.expect(diagnostics.skipped_bad_date_row_count == 1, "legacy CSV diagnostics count bad-date skips");
            t.expect(diagnostics.skipped_bad_numeric_row_count == 1, "legacy CSV diagnostics count bad-numeric skips");
            t.expect(diagnostics.skipped_row_count() == 3, "legacy CSV diagnostics expose total skipped rows");
        }

        {
            const auto csv = dir / "preprocess_diagnostics.csv";
            const std::string body =
                "date,close\n"
                "\n"
                "20240101,1\n"
                "20240102,\n"
                "20240103,3\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write preprocess CSV diagnostics fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.missing_tokens = { "", "NA" };
            spec.missing_default.kind = MissingKind::DropRow;
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            auto schema = std::make_shared<FrameSchema>();
            const auto prep = preprocess_csv(spec, csv.string(), schema);
            t.expect(prep.err.empty(), "modern preprocess diagnostics fixture succeeds");
            t.expect(prep.frame.has_value(), "modern preprocess diagnostics fixture yields NumericFrame");
            t.expect(prep.csv_had_header, "modern preprocess diagnostics preserve header presence");
            t.expect(prep.csv_load.physical_line_count == 5, "modern preprocess diagnostics count physical lines");
            t.expect(prep.csv_load.blank_line_count == 1, "modern preprocess diagnostics count blank lines");
            t.expect(prep.csv_load.data_row_count == 3, "modern preprocess diagnostics count data rows");
            t.expect(prep.dropped_row_count == 1, "modern preprocess diagnostics count drop-row removals");
            const bool blank_warning =
                std::find(prep.warnings.begin(), prep.warnings.end(),
                    "csv loader skipped 1 blank row(s)") != prep.warnings.end();
            const bool drop_warning =
                std::find(prep.warnings.begin(), prep.warnings.end(),
                    "drop-row missing policy removed 1 row(s)") != prep.warnings.end();
            t.expect(blank_warning, "modern preprocess diagnostics preserve blank-row warning");
            t.expect(drop_warning, "modern preprocess diagnostics preserve drop-row warning");
        }

        return true;
    }

    bool test_json_unicode_policy_contracts(ContractAsserts& t) {
        t.out << "[json_unicode_policy] JSON unicode surrogate rejection policy\n";

        using namespace sugar::json_min;

        {
            Value root{};
            ParseError err{};
            const bool ok = parse(R"JSON({"label":"\u0041"})JSON", root, err);
            t.expect(ok, "BMP unicode escape parses successfully");
            if (ok) {
                const auto* label = obj_get(root.as_obj(), "label");
                t.expect(label && label->is_str() && label->as_str() == "A",
                    "BMP unicode escape decodes to the expected UTF-8 text");
            }
        }

        {
            Value root{};
            ParseError err{};
            const bool ok = parse(R"JSON({"note":"\uD834\uDD1E"})JSON", root, err);
            t.expect(!ok, "surrogate-pair escape is rejected explicitly");
            t.expect(err.message.find("surrogate") != std::string::npos,
                "surrogate-pair rejection mentions surrogate policy");
        }

        {
            Value root{};
            ParseError err{};
            const bool ok = parse(R"JSON({"note":"\uDD1E"})JSON", root, err);
            t.expect(!ok, "lone low surrogate escape is rejected explicitly");
            t.expect(err.message.find("surrogate") != std::string::npos,
                "lone low surrogate rejection mentions surrogate policy");
        }

        return true;
    }

    bool test_dataspec_input_profile_contract(ContractAsserts& t) {
        t.out << "[dataspec_input_profile] DataSpec input_profile contract\n";

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "legacy_ohlcv_date"
}
)JSON", ds, err);
            t.expect(ok, "profile-only legacy DataSpec parses");
            if (ok) {
                t.expect(ds.input_profile == InputProfile::LegacyOhlcvDate, "legacy profile is parsed");
                t.expect(ds.time_col == "date", "legacy profile default time_col=date");
                t.expect(ds.time_kind == TimeKind::YyyyMmDd, "legacy profile default time_kind=YyyyMmDd");
                t.expect(ds.primary_value_col == "close", "legacy profile default primary_value_col=close");
                t.expect(ds.value_cols.size() == 5, "legacy profile default value_cols are populated");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "iso_ohlcv_time"
}
)JSON", ds, err);
            t.expect(ok, "profile-only iso DataSpec parses");
            if (ok) {
                t.expect(ds.input_profile == InputProfile::IsoOhlcvTime, "iso profile is parsed");
                t.expect(ds.time_col == "time", "iso profile default time_col=time");
                t.expect(ds.time_kind == TimeKind::Iso8601, "iso profile default time_kind=Iso8601");
                t.expect(ds.primary_value_col == "close", "iso profile default primary_value_col=close");
                t.expect(ds.value_cols.size() == 5, "iso profile default value_cols are populated");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "legacy_ohlcv_date",
  "time_col": "custom_time",
  "time_kind": "Iso8601",
  "value_cols": ["x"],
  "primary_value_col": "x",
  "rename": {"Date": "custom_time"}
}
)JSON", ds, err);
            t.expect(ok, "explicit DataSpec fields override profile defaults");
            if (ok) {
                t.expect(ds.time_col == "custom_time", "time_col override is preserved");
                t.expect(ds.time_kind == TimeKind::Iso8601, "time_kind override is preserved");
                t.expect(ds.value_cols.size() == 1 && ds.value_cols[0] == "x", "value_cols override is preserved");
                t.expect(ds.primary_value_col == "x", "primary_value_col override is preserved");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "time_col": "date",
  "time_kind": "YYYY-MM-DD",
  "value_cols": ["close"]
}
)JSON", ds, err);
            t.expect(ok, "friendly date alias parses with single value_col");
            if (ok) {
                t.expect(ds.time_kind == TimeKind::YyyyMmDd,
                    "YYYY-MM-DD alias maps to YyyyMmDd internally");
                t.expect(ds.primary_value_col == "close",
                    "single value_col infers primary_value_col");
                t.expect(ds.primary_value_col_inferred_from_single_value_col,
                    "single value_col inference is recorded");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "time_col": "date",
  "time_kind": "YYYY-MM-DD",
  "primary_value_col": "measurement"
}
)JSON", ds, err);
            t.expect(ok, "primary-only single-value shorthand parses");
            if (ok) {
                t.expect(ds.value_cols.size() == 1 && ds.value_cols[0] == "measurement",
                    "value_cols is inferred from primary_value_col");
                t.expect(ds.primary_value_col == "measurement",
                    "primary_value_col is preserved for primary-only shorthand");
                t.expect(ds.value_cols_inferred_from_primary_value_col,
                    "primary-only value_cols inference is recorded");
                t.expect(!ds.primary_value_col_inferred_from_single_value_col,
                    "primary-only shorthand does not also record primary inference");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "time_col": "time",
  "time_kind": "ISO-8601",
  "value_cols": ["close"]
}
)JSON", ds, err);
            t.expect(ok, "friendly ISO-8601 alias parses");
            if (ok) {
                t.expect(ds.time_kind == TimeKind::Iso8601,
                    "ISO-8601 alias maps to Iso8601 internally");
                t.expect(ds.primary_value_col == "close",
                    "single ISO value_col infers primary_value_col");
                t.expect(ds.primary_value_col_inferred_from_single_value_col,
                    "single ISO value_col inference is recorded");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "totally_invalid_profile"
}
)JSON", ds, err);
            t.expect(!ok, "invalid input_profile is rejected");
            t.expect(err.find("input_profile must be one of") != std::string::npos,
                "invalid input_profile error message is explicit");
        }

        const auto dir = contract_tmp_case_dir("dataspec_input_profile");
        std::filesystem::create_directories(dir);

        {
            const auto csv = dir / "profile_legacy.csv";
            const std::string body =
                "Date,Open,High,Low,Close,Volume\n"
                "20240101,1,2,0.5,1.5,10\n"
                "20240102,1.5,2.2,1.4,2.1,11\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write profile_legacy fixture");
            }

            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "legacy_ohlcv_date"
}
)JSON", ds, err);
            t.expect(ok, "legacy profile fixture DataSpec parses");
            if (ok) {
                auto schema = std::make_shared<FrameSchema>();
                auto r = preprocess_csv(ds, csv.string(), schema);
                t.expect(r.err.empty(), "legacy profile preprocess succeeds on canonical CSV");
                t.expect(r.frame.has_value(), "legacy profile preprocess yields NumericFrame");
                if (r.frame) t.expect(r.frame->row_count() == 2, "legacy profile preprocess row_count is stable");
            }
        }

        {
            const auto csv = dir / "profile_iso.csv";
            const std::string body =
                "time,open,high,low,close,Volume\n"
                "2024-01-01T00:00:00Z,1,2,0.5,1.5,10\n"
                "2024-01-02T00:00:00Z,1.5,2.2,1.4,2.1,11\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write profile_iso fixture");
            }

            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "input_profile": "iso_ohlcv_time"
}
)JSON", ds, err);
            t.expect(ok, "iso profile fixture DataSpec parses");
            if (ok) {
                auto schema = std::make_shared<FrameSchema>();
                auto r = preprocess_csv(ds, csv.string(), schema);
                t.expect(r.err.empty(), "iso profile preprocess succeeds on canonical CSV");
                t.expect(r.frame.has_value(), "iso profile preprocess yields NumericFrame");
                if (r.frame) t.expect(r.frame->row_count() == 2, "iso profile preprocess row_count is stable");
            }
        }

        return true;
    }

    bool test_spatial_only_preprocess_contracts(ContractAsserts& t) {
        t.out << "[spatial_ingest] spatial-only preprocess and ingest contracts\n";

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "spatial_x_col": "x",
  "spatial_y_col": "y",
  "value_cols": ["close"],
  "primary_value_col": "close"
}
)JSON", ds, err);
            t.expect(ok, "spatial-only DataSpec parses without time_col");
            if (ok) {
                t.expect(!data_spec_has_time_axis(ds), "spatial-only DataSpec does not declare time axis");
                t.expect(data_spec_has_spatial_axis(ds), "spatial-only DataSpec declares spatial axis");
            }
        }

        {
            DataSpec ds{};
            std::string err;
            const bool ok = parse_data_spec_json(R"JSON(
{
  "time_kind": "Index",
  "spatial_x_col": "x",
  "spatial_y_col": "y",
  "value_cols": ["close"],
  "primary_value_col": "close"
}
)JSON", ds, err);
            t.expect(!ok, "spatial-only DataSpec rejects time_kind without time_col");
            t.expect(err.find("time_kind cannot be set when time_col is empty") != std::string::npos,
                "spatial-only time_kind rejection is explicit");
        }

        const auto dir = contract_tmp_case_dir("spatial_ingest");
        std::filesystem::create_directories(dir);

        {
            const auto csv = dir / "spatial_only_single.csv";
            const std::string body =
                "x,y,close\n"
                "5,0,10\n"
                "1,0,20\n"
                "3,0,30\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write spatial_only_single fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.spatial_x_col = "x";
            spec.spatial_y_col = "y";
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;

            auto schema = std::make_shared<FrameSchema>();
            auto r = preprocess_csv(spec, csv.string(), schema);
            t.expect(r.err.empty(), "spatial-only single preprocess succeeds");
            t.expect(r.frame.has_value(), "spatial-only single preprocess yields NumericFrame");
            if (r.frame) {
                const auto& f = *r.frame;
                t.expect(f.row_count() == 3, "spatial-only single row_count is stable");
                t.expect(f.index_schema() != nullptr, "spatial-only single frame carries index_schema");
                t.expect(f.time().empty(), "spatial-only single frame keeps time payload empty");
                if (f.index_schema()) {
                    t.expect(!f.index_schema()->has_kind(AxisKind::Time), "spatial-only single frame omits time axis");
                    t.expect(f.index_schema()->has_kind(AxisKind::Spatial), "spatial-only single frame carries spatial axis");
                }

                ColumnId close_id{};
                ColumnId x_id{};
                ColumnId y_id{};
                const bool ok_close = f.schema().columns().find("close", close_id);
                const bool ok_x = f.schema().columns().find("x", x_id);
                const bool ok_y = f.schema().columns().find("y", y_id);
                t.expect(ok_close, "spatial-only single resolves close column");
                t.expect(ok_x, "spatial-only single resolves x column");
                t.expect(ok_y, "spatial-only single resolves y column");
                if (ok_close && ok_x && ok_y) {
                    const auto close = f.col(close_id);
                    const auto x = f.col(x_id);
                    const auto y = f.col(y_id);
                    t.expect(close.size() == 3 && nearly_equal(close[0], 10.0) && nearly_equal(close[1], 20.0) && nearly_equal(close[2], 30.0),
                        "spatial-only single preserves source row order for values");
                    t.expect(x.size() == 3 && nearly_equal(x[0], 5.0) && nearly_equal(x[1], 1.0) && nearly_equal(x[2], 3.0),
                        "spatial-only single preserves source row order for spatial x");
                    t.expect(y.size() == 3 && nearly_equal(y[0], 0.0) && nearly_equal(y[1], 0.0) && nearly_equal(y[2], 0.0),
                        "spatial-only single preserves spatial y values");
                }

                const auto report = make_preprocess_report(spec, r);
                t.expect(!report.has_time_axis, "spatial-only single report omits time axis");
                t.expect(report.has_spatial_axis, "spatial-only single report records spatial axis");
                t.expect(report.time_resolution == "none", "spatial-only single report time_resolution is none");
            }
        }

        {
            const auto csv = dir / "spatial_only_panel.csv";
            const std::string body =
                "region,x,y,close\n"
                "A,5,0,10\n"
                "B,1,0,100\n"
                "A,3,0,20\n"
                "B,2,0,200\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write spatial_only_panel fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.entity_col = "region";
            spec.spatial_x_col = "x";
            spec.spatial_y_col = "y";
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;

            auto schema = std::make_shared<FrameSchema>();
            auto r = preprocess_csv(spec, csv.string(), schema);
            t.expect(r.err.empty(), "spatial-only panel preprocess succeeds");
            t.expect(r.frameset.has_value(), "spatial-only panel preprocess yields FrameSet");
            if (r.frameset) {
                const auto& fs = *r.frameset;
                t.expect(fs.by_entity.size() == 2, "spatial-only panel splits entities");
                t.expect(fs.index_schema != nullptr, "spatial-only panel carries index_schema");
                if (fs.index_schema) {
                    t.expect(!fs.index_schema->has_kind(AxisKind::Time), "spatial-only panel omits time axis");
                    t.expect(fs.index_schema->has_kind(AxisKind::Entity), "spatial-only panel carries entity axis");
                    t.expect(fs.index_schema->has_kind(AxisKind::Spatial), "spatial-only panel carries spatial axis");
                }

                auto it_a = fs.by_entity.find("A");
                auto it_b = fs.by_entity.find("B");
                t.expect(it_a != fs.by_entity.end(), "spatial-only panel keeps entity A");
                t.expect(it_b != fs.by_entity.end(), "spatial-only panel keeps entity B");
                if (it_a != fs.by_entity.end() && it_b != fs.by_entity.end()) {
                    t.expect(it_a->second.time().empty(), "spatial-only panel frame A keeps time payload empty");
                    t.expect(it_b->second.time().empty(), "spatial-only panel frame B keeps time payload empty");
                    ColumnId close_id{};
                    ColumnId x_id{};
                    const bool ok_close = fs.schema->columns().find("close", close_id);
                    const bool ok_x = fs.schema->columns().find("x", x_id);
                    t.expect(ok_close, "spatial-only panel resolves close column");
                    t.expect(ok_x, "spatial-only panel resolves x column");
                    if (ok_close && ok_x) {
                        const auto a_close = it_a->second.col(close_id);
                        const auto a_x = it_a->second.col(x_id);
                        const auto b_close = it_b->second.col(close_id);
                        const auto b_x = it_b->second.col(x_id);
                        t.expect(a_close.size() == 2 && nearly_equal(a_close[0], 10.0) && nearly_equal(a_close[1], 20.0),
                            "spatial-only panel preserves per-entity source order for entity A");
                        t.expect(a_x.size() == 2 && nearly_equal(a_x[0], 5.0) && nearly_equal(a_x[1], 3.0),
                            "spatial-only panel preserves per-entity spatial order for entity A");
                        t.expect(b_close.size() == 2 && nearly_equal(b_close[0], 100.0) && nearly_equal(b_close[1], 200.0),
                            "spatial-only panel preserves per-entity source order for entity B");
                        t.expect(b_x.size() == 2 && nearly_equal(b_x[0], 1.0) && nearly_equal(b_x[1], 2.0),
                            "spatial-only panel preserves per-entity spatial order for entity B");
                    }
                }

                const auto report = make_preprocess_report(spec, r);
                t.expect(report.produced_frameset, "spatial-only panel report records frameset output");
                t.expect(report.has_entity_axis, "spatial-only panel report records entity axis");
                t.expect(!report.has_time_axis, "spatial-only panel report omits time axis");
                t.expect(report.has_spatial_axis, "spatial-only panel report records spatial axis");
                t.expect(report.time_resolution == "none", "spatial-only panel report time_resolution is none");
            }
        }

        return true;
    }

    bool test_iso8601_timekeys(ContractAsserts& t) {
        t.out << "[iso8601_timekeys] ISO-8601 -> TimeKey fidelity\n";

        const auto dir = contract_tmp_case_dir("iso8601_timekeys");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "iso8601.csv";
        const std::string body =
            "ts,close\n"
            "1970-01-01T00:00:00Z,1\n"
            "1970-01-01T02:00:00+01:00,2\n"
            "1969-12-31T23:59:59Z,3\n";
        if (!write_text_file(csv, body)) {
            return t.expect(false, "failed to write iso8601 fixture");
        }

        DataSpec spec{};
        spec.header_policy = HeaderPolicy::RequiredHeaders;
        spec.time_col = "ts";
        spec.time_kind = TimeKind::Iso8601;
        spec.value_cols = { "close" };
        spec.primary_value_col = "close";
        spec.sort_by_time = true;
        spec.dedupe = DataSpec::DedupePolicy::Error;

        auto schema = std::make_shared<FrameSchema>();
        auto r = preprocess_csv(spec, csv.string(), schema);
        t.expect(r.err.empty(), "iso8601 preprocess succeeds");
        t.expect(r.frame.has_value(), "iso8601 preprocess produces NumericFrame");
        if (!r.frame) return true;

        const auto time = r.frame->time();
        t.expect(time.size() == 3, "iso8601 test has 3 rows");
        if (time.size() == 3) {
            t.expect(time[0] == static_cast<TimeKey>(-1), "iso8601 preserves pre-epoch second");
            t.expect(time[1] == static_cast<TimeKey>(0), "iso8601 preserves Unix epoch second");
            t.expect(time[2] == static_cast<TimeKey>(3600), "iso8601 normalizes timezone to UTC epoch seconds");
            t.expect(time[0] < time[1] && time[1] < time[2], "iso8601 time keys are monotonic after sort_by_time");
        }
        return true;
    }

bool test_alignment_calendar_policy_contracts(ContractAsserts& t) {
        t.out << "[alignment_calendar_policies] alignment union/intersection calendar contracts\n";

        auto schema = std::make_shared<FrameSchema>();
        const ColumnId close_id = schema->columns().get_or_add("close");

        NumericFrame fa(*schema, std::vector<TimeKey>{ 1, 2, 4 });
        {
            auto c = fa.ensure_col(close_id);
            c[0] = 10.0; c[1] = 20.0; c[2] = 40.0;
        }
        NumericFrame fb(*schema, std::vector<TimeKey>{ 2, 3, 4 });
        {
            auto c = fb.ensure_col(close_id);
            c[0] = 200.0; c[1] = 300.0; c[2] = 400.0;
        }

        std::vector<std::pair<std::string, NumericFrame>> in;
        in.emplace_back("A", fa);
        in.emplace_back("B", fb);

        align::AlignConfig ucfg{};
        ucfg.calendar = align::CalendarPolicy::Union;
        ucfg.missing = align::MissingPolicy::NaN;
        const auto ub = align::align_frames(schema, in, ucfg);

        t.expect(ub.time.size() == 4, "union calendar has 4 rows");
        if (ub.time.size() == 4) {
            t.expect(ub.time[0] == 1 && ub.time[1] == 2 && ub.time[2] == 3 && ub.time[3] == 4,
                "union calendar is sorted and complete");
        }
        const std::size_t ia = ub.find_series_index("A");
        const std::size_t ib = ub.find_series_index("B");
        t.expect(ia != static_cast<std::size_t>(-1) && ib != static_cast<std::size_t>(-1), "union contains both series");
        if (ia != static_cast<std::size_t>(-1) && ib != static_cast<std::size_t>(-1)) {
            const auto a = ub.frames[ia].col(close_id);
            const auto b = ub.frames[ib].col(close_id);
            t.expect(a.size() == 4 && b.size() == 4, "union aligned columns have calendar length");
            if (a.size() == 4 && b.size() == 4) {
                t.expect(nearly_equal(a[0], 10.0) && nearly_equal(a[1], 20.0) &&
                    std::isnan(a[2]) && nearly_equal(a[3], 40.0),
                    "union fills missing A rows with NaN");
                t.expect(std::isnan(b[0]) && nearly_equal(b[1], 200.0) &&
                    nearly_equal(b[2], 300.0) && nearly_equal(b[3], 400.0),
                    "union fills missing B rows with NaN");
            }
        }

        align::AlignConfig icfg{};
        icfg.calendar = align::CalendarPolicy::Intersection;
        icfg.missing = align::MissingPolicy::NaN;
        const auto ibundle = align::align_frames(schema, in, icfg);

        t.expect(ibundle.time.size() == 2, "intersection calendar has overlap rows only");
        if (ibundle.time.size() == 2) {
            t.expect(ibundle.time[0] == 2 && ibundle.time[1] == 4, "intersection calendar values are correct");
        }
        const std::size_t ia2 = ibundle.find_series_index("A");
        const std::size_t ib2 = ibundle.find_series_index("B");
        t.expect(ia2 != static_cast<std::size_t>(-1) && ib2 != static_cast<std::size_t>(-1), "intersection contains both series");
        if (ia2 != static_cast<std::size_t>(-1) && ib2 != static_cast<std::size_t>(-1)) {
            const auto a = ibundle.frames[ia2].col(close_id);
            const auto b = ibundle.frames[ib2].col(close_id);
            t.expect(a.size() == 2 && b.size() == 2, "intersection aligned columns have overlap length");
            if (a.size() == 2 && b.size() == 2) {
                t.expect(nearly_equal(a[0], 20.0) && nearly_equal(a[1], 40.0), "intersection preserves A overlap values");
                t.expect(nearly_equal(b[0], 200.0) && nearly_equal(b[1], 400.0), "intersection preserves B overlap values");
            }
        }

        return true;
    }

    bool test_alignment_schema_validation_contracts(ContractAsserts& t) {
        t.out << "[alignment_schema_validation] alignment schema validation contracts\n";

        auto schema = std::make_shared<FrameSchema>();
        const ColumnId close_id = schema->columns().get_or_add("close");

        align::AlignConfig icfg{};
        icfg.calendar = align::CalendarPolicy::Intersection;
        icfg.missing = align::MissingPolicy::NaN;

        {
            auto wider_schema = std::make_shared<FrameSchema>(*schema);
            ColumnId wider_close_id{};
            t.expect(wider_schema->columns().find("close", wider_close_id),
                "wider schema preserves close column id for alignment mismatch test");
            const ColumnId volume_id = wider_schema->columns().get_or_add("volume");

            NumericFrame wider_frame(*wider_schema, std::vector<TimeKey>{ 1, 2 });
            wider_frame.ensure_col(wider_close_id);
            wider_frame.ensure_col(volume_id);

            std::vector<std::pair<std::string, NumericFrame>> extra_input;
            extra_input.emplace_back("A", wider_frame);

            bool threw = false;
            std::string err;
            try {
                (void)align::align_frames(schema, extra_input, icfg);
            }
            catch (const std::invalid_argument& ex) {
                threw = true;
                err = ex.what();
            }

            t.expect(threw, "alignment rejects frames whose schema has extra trailing columns");
            t.expect(err.find("schema mismatch") != std::string::npos,
                "extra trailing-column rejection remains explicit at the alignment boundary");
        }

        {
            auto wider_schema = std::make_shared<FrameSchema>(*schema);
            const ColumnId volume_id = wider_schema->columns().get_or_add("volume");
            (void)volume_id;

            NumericFrame narrower_frame(*schema, std::vector<TimeKey>{ 1, 2 });
            narrower_frame.ensure_col(close_id);

            std::vector<std::pair<std::string, NumericFrame>> missing_input;
            missing_input.emplace_back("A", narrower_frame);

            bool threw = false;
            std::string err;
            try {
                (void)align::align_frames(wider_schema, missing_input, icfg);
            }
            catch (const std::invalid_argument& ex) {
                threw = true;
                err = ex.what();
            }

            t.expect(threw, "alignment rejects frames whose schema is missing trailing columns");
            t.expect(err.find("schema mismatch") != std::string::npos,
                "missing trailing-column rejection remains explicit at the alignment boundary");
        }

        return true;
    }

    bool test_alignment_non_temporal_design_load_contracts(ContractAsserts& t) {
        t.out << "[alignment_non_temporal_design_loads] alignment non-temporal design-load contracts\n";

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");

            NumericFrame fa(schema, static_cast<std::size_t>(2));
            NumericFrame fb(schema, static_cast<std::size_t>(2));
            fa.set_index_schema(std::make_shared<IndexSchema>());
            fb.set_index_schema(std::make_shared<IndexSchema>());
            fa.index_schema()->add_spatial_axis_2d("x", "y");
            fb.index_schema()->add_spatial_axis_2d("x", "y");
            auto fa_close = fa.ensure_col(close_id);
            auto fb_close = fb.ensure_col(close_id);
            fa_close[0] = 10.0;
            fa_close[1] = 11.0;
            fb_close[0] = 20.0;
            fb_close[1] = 21.0;

            std::vector<std::pair<std::string, NumericFrame>> in;
            in.emplace_back("A", fa);
            in.emplace_back("B", fb);

            align::AlignConfig cfg{};
            cfg.non_temporal_multi_series = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;

            const auto bundle = align::align_frames(schema, in, cfg);
            t.expect(bundle.time.empty(), "equal-length non-temporal alignment keeps time axis empty");
            t.expect(bundle.row_count() == 2, "equal-length non-temporal alignment preserves shared row-domain length");
            t.expect(bundle.frames.size() == 2, "equal-length non-temporal alignment keeps all series");
            t.expect(bundle.series_keys.size() == 2 &&
                    bundle.series_keys[0] == "A" &&
                    bundle.series_keys[1] == "B",
                "equal-length non-temporal alignment preserves sorted stable series ordering");
            t.expect(bundle.has_axis(AxisKind::Spatial),
                "equal-length non-temporal alignment preserves shared spatial axis metadata");
            t.expect(bundle.topology_catalog && bundle.topology_catalog->has(builtin_spatial_topology_name()),
                "equal-length non-temporal alignment mounts built-in spatial runtime topology");
        }

        {
            const auto dir = contract_tmp_case_dir("alignment_non_temporal_design");
            std::filesystem::create_directories(dir);
            const auto csv = dir / "spatial_panel.csv";
            const std::string body =
                "region,x,y,close\n"
                "A,0,0,10\n"
                "A,1,0,11\n"
                "B,5,0,20\n"
                "B,6,0,21\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write equal-length non-temporal runtime fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.entity_col = "region";
            spec.spatial_x_col = "x";
            spec.spatial_y_col = "y";
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;
            spec.alignment_non_temporal_multi_series = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;

            const auto options = make_compatibility_runtime_build_options(spec);

            ResolvedRuntimeInput runtime{};
            std::string err;
            const bool ok = build_resolved_runtime_input(
                spec,
                csv.string(),
                "close",
                options,
                runtime,
                err);
            t.expect(ok, "shared runtime build accepts equal-length non-temporal multi-series policy from DataSpec");
            if (ok) {
                t.expect(runtime.primary_selection_mode == RuntimePrimarySelectionMode::HeuristicCompatibility,
                    "compatibility runtime build keeps explicit heuristic-selection provenance");
                t.expect(runtime.run_manifest.preprocess.alignment_non_temporal_multi_series ==
                        align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
                    "shared runtime manifest preserves equal-length non-temporal policy");
                t.expect(runtime.aligned_bundle && runtime.aligned_bundle->row_count() == 2,
                    "shared runtime preserves equal-length non-temporal row domain");
                t.expect(runtime.aligned_bundle && runtime.aligned_bundle->frames.size() == 2,
                    "shared runtime keeps all non-temporal aligned series");
            }
            else {
                t.expect(!err.empty(), "equal-length non-temporal runtime failure remains explicit");
            }
        }

        return true;
    }

    bool test_alignment_non_temporal_policy_contracts(ContractAsserts& t) {
        t.out << "[alignment_non_temporal_policy] alignment non-temporal policy rejection contracts\n";

        auto schema = std::make_shared<FrameSchema>();
        const ColumnId close_id = schema->columns().get_or_add("close");

        NumericFrame fa(schema, static_cast<std::size_t>(2));
        NumericFrame fb(schema, static_cast<std::size_t>(2));
        fa.set_index_schema(std::make_shared<IndexSchema>());
        fb.set_index_schema(std::make_shared<IndexSchema>());
        fa.index_schema()->add_spatial_axis_2d("x", "y");
        fb.index_schema()->add_spatial_axis_2d("x", "y");
        fa.ensure_col(close_id);
        fb.ensure_col(close_id);

        std::vector<std::pair<std::string, NumericFrame>> in;
        in.emplace_back("A", fa);
        in.emplace_back("B", fb);

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;

        bool threw = false;
        std::string err;
        try {
            (void)align::align_frames(schema, in, cfg);
        }
        catch (const std::invalid_argument& ex) {
            threw = true;
            err = ex.what();
        }
        t.expect(threw, "non-temporal multi-series alignment is rejected without explicit policy");
        t.expect(err.find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
            "non-temporal multi-series alignment rejection is explicit");

        FrameSet fs{};
        fs.schema = schema;
        fs.by_entity.emplace("A", fa);
        fs.by_entity.emplace("B", fb);

        std::shared_ptr<align::AlignedBundle> bundle{};
        std::string helper_err;
        const bool helper_ok = sugar::align_frameset(fs, cfg, bundle, helper_err);
        t.expect(!helper_ok, "shared align_frameset helper rejects non-temporal multi-series without policy");
        t.expect(helper_err.find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
            "shared align_frameset helper preserves explicit non-temporal policy failure");

        NumericFrame fc(schema, static_cast<std::size_t>(3));
        fc.set_index_schema(std::make_shared<IndexSchema>());
        fc.index_schema()->add_spatial_axis_2d("x", "y");
        fc.ensure_col(close_id);

        std::vector<std::pair<std::string, NumericFrame>> uneven;
        uneven.emplace_back("A", fa);
        uneven.emplace_back("C", fc);

        cfg.non_temporal_multi_series = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;

        threw = false;
        err.clear();
        try {
            (void)align::align_frames(schema, uneven, cfg);
        }
        catch (const std::invalid_argument& ex) {
            threw = true;
            err = ex.what();
        }
        t.expect(threw, "equal-length non-temporal alignment rejects jagged multi-series row domains");
        t.expect(err.find("requires equal row counts") != std::string::npos,
            "equal-length non-temporal mismatch rejection is explicit");

        FrameSet uneven_fs{};
        uneven_fs.schema = schema;
        uneven_fs.by_entity.emplace("A", fa);
        uneven_fs.by_entity.emplace("C", fc);

        helper_err.clear();
        const bool uneven_ok = sugar::align_frameset(uneven_fs, cfg, bundle, helper_err);
        t.expect(!uneven_ok, "shared align_frameset helper rejects jagged equal-length row-domain policy");
        t.expect(helper_err.find("requires equal row counts") != std::string::npos,
            "shared helper preserves explicit jagged equal-length rejection");

        return true;
    }

    bool test_alignment_policy_metadata_preservation_contracts(ContractAsserts& t) {
        t.out << "[alignment_policy_metadata_preservation] alignment policy metadata/provenance preservation\n";

        const auto dir = contract_tmp_case_dir("alignment_policy_metadata");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "spatial_single.csv";
        const std::string body =
            "x,y,close\n"
            "0,0,10\n"
            "1,0,11\n";
        if (!write_text_file(csv, body)) {
            return t.expect(false, "failed to write alignment-policy fixture");
        }

        DataSpec spec{};
        spec.header_policy = HeaderPolicy::RequiredHeaders;
        spec.spatial_x_col = "x";
        spec.spatial_y_col = "y";
        spec.value_cols = { "close" };
        spec.primary_value_col = "close";
        spec.sort_by_time = true;
        spec.alignment_calendar = align::CalendarPolicy::Union;
        spec.alignment_non_temporal_multi_series = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;

        const auto cfg = make_align_config_from_data_spec(spec);
        t.expect(cfg.calendar == align::CalendarPolicy::Union,
            "shared align-config builder derives calendar policy from DataSpec");
        t.expect(cfg.non_temporal_multi_series == align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
            "shared align-config builder derives non-temporal policy from DataSpec");

        auto schema = std::make_shared<FrameSchema>();
        const auto prep = preprocess_csv(spec, csv.string(), schema);
        t.expect(prep.err.empty(), "alignment-policy fixture preprocess succeeds");
        if (!prep.err.empty()) {
            return true;
        }

        const auto report = make_preprocess_report(spec, prep, cfg);
        t.expect(report.alignment_applied, "preprocess report records alignment policy application");
        t.expect(report.alignment_calendar == align::CalendarPolicy::Union,
            "preprocess report preserves alignment calendar policy");
        t.expect(report.alignment_missing == align::MissingPolicy::NaN,
            "preprocess report preserves alignment missing policy");
        t.expect(report.alignment_non_temporal_multi_series == align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
            "preprocess report preserves non-temporal multi-series policy");

        const auto manifest = make_run_manifest(
            report,
            "__single__",
            {},
            RuntimePrimarySelectionMode::SingleSeriesImplicit);
        t.expect(manifest.preprocess.alignment_non_temporal_multi_series ==
                align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
            "run manifest preserves non-temporal multi-series policy provenance");
        t.expect(manifest.primary_selection_mode == RuntimePrimarySelectionMode::SingleSeriesImplicit,
            "run manifest preserves primary selection provenance alongside alignment policy");

        auto options = make_strict_runtime_build_options(spec);
        t.expect(options.target.require_primary_series_on_multi_series,
            "strict runtime-build options require explicit target selection on multi-series inputs");
        t.expect(options.align_config.calendar == align::CalendarPolicy::Union,
            "strict runtime-build options preserve DataSpec calendar intent");
        t.expect(options.align_config.non_temporal_multi_series ==
                align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
            "strict runtime-build options preserve DataSpec non-temporal intent");

        ResolvedRuntimeInput runtime{};
        std::string err;
        const bool runtime_ok = build_resolved_runtime_input(
            spec,
            csv.string(),
            "close",
            options,
            runtime,
            err);
        t.expect(runtime_ok, "shared runtime build preserves modeled alignment policy on single-series row domain");
        if (runtime_ok) {
            t.expect(runtime.run_manifest.preprocess.alignment_non_temporal_multi_series ==
                    align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
                "shared runtime manifest preserves non-temporal multi-series policy");
            t.expect(runtime.run_manifest.preprocess.alignment_calendar == align::CalendarPolicy::Union,
                "shared runtime manifest preserves DataSpec-derived calendar policy");
        }
        else {
            t.expect(!err.empty(), "shared runtime build reports explicit alignment-policy failure text");
        }

        return true;
    }
    // Direct observation/runtime leaf contracts now live in
    // contract_suite_data_observation.cpp.

    bool test_topology_contracts(ContractAsserts& t) {
        t.out << "[topology_contracts] topology catalog and runtime sidecar contracts\n";

        const auto make_temporal_spec = []() {
            return make_builtin_temporal_topology_spec();
        };

        const auto make_semantic_spec = []() {
            TopologySpec spec{};
            spec.name = "entity.semantic.graph";
            spec.origin = TopologyOrigin::LayoutDeclared;
            spec.bearing = TopologyBearing::MetadataOnly;
            spec.family = TopologyFamily::SemanticGraph;
            spec.domain_scope = TopologyDomainScope::ObservationDomain;
            spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            spec.directed = true;
            spec.weighted = true;
            spec.artifact_ref = TopologyArtifactRef{
                "entity.semantic.graph.v1",
                "artifacts/entity.semantic.graph.parquet",
                "parquet",
                "sha256:entity-semantic-v1",
                "python_pandas"
            };
            spec.metadata.emplace_back("space", "semantic");
            return spec;
        };

        const auto make_probabilistic_spec = []() {
            TopologySpec spec{};
            spec.name = "state.probabilistic.graph";
            spec.origin = TopologyOrigin::LayoutDeclared;
            spec.bearing = TopologyBearing::MetadataOnly;
            spec.family = TopologyFamily::ProbabilisticGraph;
            spec.domain_scope = TopologyDomainScope::ObservationDomain;
            spec.query_kinds = { TopologyQueryKind::TransitionSet };
            spec.directed = true;
            spec.weighted = true;
            spec.metadata.emplace_back("space", "probabilistic");
            return spec;
        };

        {
            TopologyCatalog catalog{};
            t.expect(catalog.empty(), "topology catalog starts empty");
            t.expect(catalog.size() == 0, "topology catalog empty size is zero");

            const auto temporal = make_temporal_spec();
            t.expect(temporal.valid(), "temporal topology spec with name is valid");
            t.expect(temporal.supports_query_kind(TopologyQueryKind::OrderedWindow),
                "temporal topology spec reports supported ordered-window queries");
            t.expect(!temporal.supports_query_kind(TopologyQueryKind::MetricRadius),
                "temporal topology spec rejects unsupported query kinds");
            t.expect(temporal.origin == TopologyOrigin::BuiltinRuntime,
                "temporal topology spec records builtin runtime origin");
            t.expect(temporal.bearing == TopologyBearing::RuntimeBearing,
                "temporal topology spec records runtime-bearing execution status");

            t.expect(catalog.add(temporal), "topology catalog accepts builtin topology registration");
            t.expect(catalog.add(make_semantic_spec()), "topology catalog accepts unique declared topology");

            t.expect(catalog.size() == 2, "topology catalog stores multiple named topology specs");
            t.expect(catalog.has("builtin.temporal.window"), "topology catalog resolves built-in topology by name");
            t.expect(catalog.has_family(TopologyFamily::TemporalWindow), "topology catalog resolves temporal family");
            t.expect(catalog.has_family(TopologyFamily::SemanticGraph), "topology catalog resolves semantic family");

            const auto* semantic = catalog.find("entity.semantic.graph");
            t.expect(semantic != nullptr, "topology catalog finds semantic graph spec");
            if (semantic) {
                t.expect(semantic->directed, "semantic graph spec preserves directed flag");
                t.expect(semantic->weighted, "semantic graph spec preserves weighted flag");
                t.expect(semantic->artifact_ref.has_value(), "semantic graph spec preserves artifact provenance");
                t.expect(semantic->origin == TopologyOrigin::LayoutDeclared,
                    "semantic graph spec records declared origin");
                t.expect(semantic->bearing == TopologyBearing::MetadataOnly,
                    "semantic graph spec records metadata-only bearing");
                if (semantic->artifact_ref.has_value()) {
                    t.expect(semantic->artifact_ref->stable_id == "entity.semantic.graph.v1",
                        "semantic graph spec records stable artifact id provenance");
                    t.expect(semantic->artifact_ref->producer == "python_pandas",
                        "semantic graph spec records future Python/Pandas provenance");
                }
            }

            const auto names = catalog.list_names();
            t.expect(names.size() == 2 &&
                names[0] == "builtin.temporal.window" &&
                names[1] == "entity.semantic.graph",
                "topology catalog preserves insertion order for stable name listing");

            auto replacement = make_semantic_spec();
            replacement.weighted = false;
            replacement.metadata.emplace_back("revision", "2");
            std::string add_err;
            t.expect(!catalog.add(replacement, &add_err),
                "adding duplicate declared topology name is rejected explicitly");
            t.expect(add_err.find("duplicate topology name") != std::string::npos,
                "duplicate declared topology rejection reports explicit reason");
            t.expect(catalog.size() == 2, "rejecting duplicate declared topology leaves catalog size unchanged");
            semantic = catalog.find("entity.semantic.graph");
            if (semantic) {
                t.expect(semantic->weighted, "duplicate declared topology rejection preserves original entry fields");
            }

            const auto snapshot = catalog.snapshot();
            t.expect(catalog.add(make_probabilistic_spec()),
                "topology catalog remains mutable for new unique declared names");
            t.expect(snapshot.size() == 2, "topology catalog snapshot is detached from later mutations");
            t.expect(catalog.size() == 3, "topology catalog remains mutable after snapshot");
            t.expect(std::string(topology_family_name(TopologyFamily::SemanticGraph)) == "semantic_graph",
                "topology family naming helper is stable");
            t.expect(std::string(topology_query_kind_name(TopologyQueryKind::TransitionSet)) == "transition_set",
                "topology query naming helper is stable");

            TopologySpec reserved_name{};
            reserved_name.name = "builtin.temporal.window";
            reserved_name.origin = TopologyOrigin::LayoutDeclared;
            reserved_name.bearing = TopologyBearing::MetadataOnly;
            reserved_name.family = TopologyFamily::SemanticGraph;
            reserved_name.domain_scope = TopologyDomainScope::ObservationDomain;
            reserved_name.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            add_err.clear();
            t.expect(!catalog.add(reserved_name, &add_err),
                "declared topology cannot claim reserved builtin namespace");
            t.expect(add_err.find("reserved for builtin runtime topology") != std::string::npos,
                "reserved builtin namespace rejection is explicit");
        }

        {
            auto bundle = std::make_shared<align::AlignedBundle>();
            bundle->index_schema = IndexSchema::make_time_only();
            bundle->topology_catalog = std::make_shared<TopologyCatalog>();
            bundle->topology_catalog->add(make_semantic_spec());

            t.expect(bundle->has_index_schema(), "aligned bundle can carry index_schema beside topology catalog");
            t.expect(bundle->topology_catalog->has_family(TopologyFamily::SemanticGraph),
                "aligned bundle topology sidecar can carry non-axis semantic topology");
            t.expect(!bundle->index_schema->has_kind(AxisKind::Spatial),
                "non-axis topology sidecar does not leak into IndexSchema axis kinds");
        }

        {
            PreprocessReport preprocess{};
            preprocess.has_time_axis = true;
            preprocess.time_kind = TimeKind::YyyyMmDd;
            preprocess.time_resolution = "calendar_date_yyyymmdd";
            preprocess.series_keys = { "A" };

            auto catalog = std::make_shared<TopologyCatalog>();
            catalog->add(make_semantic_spec());

            auto manifest = make_run_manifest(preprocess, "A", catalog);
            t.expect(manifest.primary_series_key == "A", "run manifest preserves primary series key");
            t.expect(manifest.topologies.size() == 1, "run manifest snapshots topology catalog into manifest provenance");
            if (!manifest.topologies.empty()) {
                t.expect(manifest.topologies.front().name == "entity.semantic.graph",
                    "run manifest preserves topology name provenance");
                t.expect(manifest.topologies.front().origin == TopologyOrigin::LayoutDeclared,
                    "run manifest preserves topology origin provenance");
                t.expect(manifest.topologies.front().bearing == TopologyBearing::MetadataOnly,
                    "run manifest preserves topology bearing provenance");
                t.expect(manifest.topologies.front().artifact_ref.has_value(),
                    "run manifest preserves topology artifact provenance");
                if (manifest.topologies.front().artifact_ref.has_value()) {
                    t.expect(manifest.topologies.front().artifact_ref->stable_id == "entity.semantic.graph.v1",
                        "run manifest preserves topology artifact stable id");
                }
            }

            catalog->add(make_probabilistic_spec());
            t.expect(manifest.topologies.size() == 1,
                "run manifest topology provenance stays detached from later catalog mutation");
        }

        {
            RunManifest manifest{};
            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            append_observation_capability_snapshot(
                diagnose_observation_capabilities(facts),
                manifest);

            const auto it = std::find_if(
                manifest.capability_outcomes.begin(),
                manifest.capability_outcomes.end(),
                [](const CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "observation_surface" &&
                        outcome.subject == "Temporal";
                });
            t.expect(it != manifest.capability_outcomes.end(),
                "run manifest records observation-surface capability outcomes");
            if (it != manifest.capability_outcomes.end()) {
                t.expect(it->status == "valid",
                    "run manifest preserves observation-surface capability status");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            NumericFrame frame(*schema, std::vector<TimeKey>{ 1, 2, 3 });
            auto close = frame.ensure_col(close_id);
            close[0] = 10.0;
            close[1] = 11.0;
            close[2] = 12.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "temporal align_frames initializes topology catalog sidecar");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->size() == 1,
                    "temporal align_frames mounts one built-in runtime topology");
                t.expect(bundle.topology_catalog->has(builtin_temporal_topology_name()),
                    "temporal align_frames mounts built-in temporal topology");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            NumericFrame frame(*schema, static_cast<std::size_t>(3));
            auto close = frame.ensure_col(close_id);
            close[0] = 20.0;
            close[1] = 21.0;
            close[2] = 22.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "row-domain align_frames initializes topology catalog on non-temporal runtime too");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->empty(),
                    "row-domain align_frames topology catalog also starts empty");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            const ColumnId close_id = schema->columns().get_or_add("close");
            const ColumnId x_id = schema->columns().get_or_add("x");
            const ColumnId y_id = schema->columns().get_or_add("y");
            NumericFrame frame(*schema, std::vector<TimeKey>{ 1, 2, 3 });
            frame.set_index_schema(IndexSchema::make_time_only());
            if (frame.index_schema()) {
                frame.index_schema()->add_spatial_axis_2d("x", "y");
            }
            auto close = frame.ensure_col(close_id);
            auto x = frame.ensure_col(x_id);
            auto y = frame.ensure_col(y_id);
            close[0] = 10.0;
            close[1] = 11.0;
            close[2] = 12.0;
            x[0] = 0.0;
            x[1] = 1.0;
            x[2] = 2.0;
            y[0] = 0.0;
            y[1] = 0.0;
            y[2] = 0.0;

            std::vector<std::pair<std::string, NumericFrame>> inputs;
            inputs.emplace_back("A", frame);

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;
            const auto bundle = align::align_frames(schema, inputs, cfg);

            t.expect(bundle.topology_catalog != nullptr,
                "spatio-temporal align_frames initializes topology catalog sidecar");
            if (bundle.topology_catalog) {
                t.expect(bundle.topology_catalog->size() == 3,
                    "spatio-temporal align_frames mounts temporal, spatial, and composite built-ins");
                t.expect(bundle.topology_catalog->has(builtin_temporal_topology_name()),
                    "spatio-temporal align_frames mounts built-in temporal topology");
                t.expect(bundle.topology_catalog->has(builtin_spatial_topology_name()),
                    "spatio-temporal align_frames mounts built-in spatial topology");
                t.expect(bundle.topology_catalog->has(builtin_spatiotemporal_topology_name()),
                    "spatio-temporal align_frames mounts built-in spatio-temporal topology");
            }
        }

        {
            const auto dir = contract_tmp_case_dir("topology_manifest_provenance");
            const auto csv = dir / "topology_runtime_input.csv";
            const std::string body =
                "date,close\n"
                "20240101,100\n"
                "20240102,101\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write topology runtime-input fixture");
            }

            DataSpec spec{};
            spec.header_policy = HeaderPolicy::RequiredHeaders;
            spec.time_col = "date";
            spec.time_kind = TimeKind::YyyyMmDd;
            spec.value_cols = { "close" };
            spec.primary_value_col = "close";
            spec.sort_by_time = true;
            spec.dedupe = DataSpec::DedupePolicy::Error;

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;

            ResolvedRuntimeInput runtime{};
            std::string err;
            const bool ok = build_resolved_runtime_input(spec, csv.string(), "close", cfg, runtime, err);
            t.expect(ok, "build_resolved_runtime_input succeeds with topology sidecar wiring");
            if (ok) {
                t.expect(runtime.aligned_bundle != nullptr, "resolved runtime input owns aligned bundle");
                t.expect(runtime.topology_catalog != nullptr, "resolved runtime input exposes topology catalog");
                t.expect(runtime.topology_catalog == runtime.aligned_bundle->topology_catalog,
                    "resolved runtime input shares topology catalog with aligned bundle");
                t.expect(runtime.run_manifest.topologies.size() == 1,
                    "resolved runtime input manifest snapshots mounted built-in topology provenance by default");
                if (!runtime.run_manifest.topologies.empty()) {
                    t.expect(runtime.run_manifest.topologies.front().name == builtin_temporal_topology_name(),
                        "resolved runtime input manifest preserves mounted built-in topology name");
                    t.expect(runtime.run_manifest.topologies.front().artifact_ref.has_value(),
                        "resolved runtime input manifest preserves built-in topology artifact ref");
                    if (runtime.run_manifest.topologies.front().artifact_ref.has_value()) {
                        t.expect(runtime.run_manifest.topologies.front().artifact_ref->stable_id == "builtin.temporal.window.v1",
                            "resolved runtime input manifest preserves built-in topology artifact stable id");
                    }
                }
                const auto cap_it = std::find_if(
                    runtime.run_manifest.capability_outcomes.begin(),
                    runtime.run_manifest.capability_outcomes.end(),
                    [](const CapabilityOutcomeSnapshot& outcome) {
                        return outcome.domain == "observation_surface" &&
                            outcome.subject == "Temporal";
                    });
                t.expect(cap_it != runtime.run_manifest.capability_outcomes.end(),
                    "resolved runtime input manifest records observation capability outcomes");

                runtime.topology_catalog->add(make_probabilistic_spec());
                t.expect(runtime.run_manifest.topologies.size() == 1,
                    "resolved runtime input manifest remains detached after later topology catalog mutation");
            }
            else {
                t.expect(!err.empty(), "topology runtime-input fixture reports explicit failure text");
            }
        }

        {
            const auto dir = contract_tmp_case_dir("topology_legacy_compat");
            std::filesystem::create_directories(dir);
            const auto data_spec_path = dir / "embedded_single_frame_dataspec.json";
            const auto main_csv = dir / "embedded_single_frame_main.csv";
            const auto hedge_csv = dir / "embedded_single_frame_hedge.csv";
            const auto plan_path = dir / "embedded_single_frame_plan.json";

            const std::string data_spec_body =
                "{\n"
                "  \"header_policy\": \"RequiredHeaders\",\n"
                "  \"time_col\": \"date\",\n"
                "  \"time_kind\": \"YyyyMmDd\",\n"
                "  \"value_cols\": [\"close\"],\n"
                "  \"primary_value_col\": \"close\",\n"
                "  \"sort_by_time\": true,\n"
                "  \"dedupe\": \"Error\"\n"
                "}\n";
            const std::string main_csv_body =
                "date,close\n"
                "20240101,100\n"
                "20240102,101\n";
            const std::string hedge_csv_body =
                "date,close\n"
                "20240101,90\n"
                "20240102,91\n";
            const std::string plan_body =
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"embedded_single_frame_provenance\",\n"
                "  \"series_inputs\": [\n"
                "    {\n"
                "      \"key\": \"main\",\n"
                "      \"path\": \"embedded_single_frame_main.csv\",\n"
                "      \"data_spec\": \"embedded_single_frame_dataspec.json\"\n"
                "    },\n"
                "    {\n"
                "      \"key\": \"hedge\",\n"
                "      \"path\": \"embedded_single_frame_hedge.csv\",\n"
                "      \"data_spec\": \"embedded_single_frame_dataspec.json\"\n"
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

            const bool fixture_ok =
                write_text_file(data_spec_path, data_spec_body) &&
                write_text_file(main_csv, main_csv_body) &&
                write_text_file(hedge_csv, hedge_csv_body) &&
                write_text_file(plan_path, plan_body);
            t.expect(fixture_ok, "embedded single-frame runtime provenance fixture is writable");
            if (!fixture_ok) return true;

            PlanSpec spec{};
            std::string err;
            const bool load_ok = load_plan_spec_file(plan_path.string(), spec, err);
            t.expect(load_ok, "embedded single-frame PlanSpec fixture loads");
            if (!load_ok) return true;

            ResolvedRuntimeInput runtime{};
            const bool runtime_ok = build_embedded_plan_observation_compat_runtime(spec, runtime, err);
            t.expect(runtime_ok, "embedded single-frame legacy runtime compatibility builds successfully");
            if (runtime_ok) {
                t.expect(runtime.primary_ref.series == "main",
                    "embedded single-frame legacy runtime keeps first series as primary");
                t.expect(runtime.primary_entity_key == "main",
                    "embedded single-frame legacy runtime preserves primary provenance as series key");
                t.expect(runtime.series_key_to_entity["main"] == "main",
                    "embedded single-frame legacy runtime preserves main series provenance distinctly");
                t.expect(runtime.series_key_to_entity["hedge"] == "hedge",
                    "embedded single-frame legacy runtime preserves secondary series provenance distinctly");

                const std::size_t main_index = runtime.aligned_bundle->find_series_index("main");
                const std::size_t hedge_index = runtime.aligned_bundle->find_series_index("hedge");
                t.expect(main_index != static_cast<std::size_t>(-1) &&
                    runtime.aligned_bundle->entity_key_for_series_index(main_index) == "main",
                    "embedded single-frame aligned bundle keeps main entity provenance aligned with series key");
                t.expect(hedge_index != static_cast<std::size_t>(-1) &&
                    runtime.aligned_bundle->entity_key_for_series_index(hedge_index) == "hedge",
                    "embedded single-frame aligned bundle keeps hedge entity provenance aligned with series key");
            }
            else {
                t.expect(!err.empty(), "embedded single-frame runtime compatibility reports explicit failure text");
            }
        }

        return true;
    }
    // Direct topology/target-resolution/candle/warmup leaf contracts now live in
    // contract_suite_data_topology.cpp.

} // namespace sugar::contract_suite_internal


