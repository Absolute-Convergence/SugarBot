#include "contract_suite_internal.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "legacy_candle_frame_adapter.h"
#include "observation_capability.h"
#include "op_kernels_math.h"
#include "op_kernels_spatial.h"
#include "op_kernels_ts.h"
#include "preprocess.h"
#include "run_manifest.h"
#include "run_pipeline.h"

namespace sugar::contract_suite_internal {

    bool test_preprocess_missing_policy(ContractAsserts& t) {
        t.out << "[missing_policy] preprocess missing-token policy\n";

        const auto dir = contract_tmp_dir();
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
  "input_profile": "totally_invalid_profile"
}
)JSON", ds, err);
            t.expect(!ok, "invalid input_profile is rejected");
            t.expect(err.find("input_profile must be one of") != std::string::npos,
                "invalid input_profile error message is explicit");
        }

        const auto dir = contract_tmp_dir();
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

        const auto dir = contract_tmp_dir();
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

    bool test_observation_capability_contracts(ContractAsserts& t) {
        t.out << "[observation_capabilities] observation capability diagnosis\n";

        auto find_surface = [](const ObservationCapabilityReport& report,
            ObservationSurfaceKind surface) -> const ObservationSurfaceCapability* {
            for (const auto& item : report.surfaces) {
                if (item.surface == surface) return &item;
            }
            return nullptr;
        };

        auto has_reason = [](const ObservationSurfaceCapability* item,
            ObservationCapabilityReasonCode code) {
            if (!item) return false;
            return std::find(item->reasons.begin(), item->reasons.end(), code) != item->reasons.end();
        };

        auto has_topology = [](const ObservationWorldFacts& facts, std::string_view name) {
            return std::any_of(facts.available_topologies.begin(),
                facts.available_topologies.end(),
                [&](const TopologySpec& spec) {
                    return spec.name == name;
                });
        };

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("__single__", NumericFrame(schema, 3));

            PreprocessReport report{};
            report.produced_frameset = false;
            report.has_entity_axis = false;
            report.has_time_axis = false;
            report.has_spatial_axis = true;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);

            const auto* row_domain = find_surface(caps, ObservationSurfaceKind::RowDomain);
            const auto* temporal = find_surface(caps, ObservationSurfaceKind::Temporal);
            const auto* spatial = find_surface(caps, ObservationSurfaceKind::Spatial);
            const auto* st = find_surface(caps, ObservationSurfaceKind::SpatioTemporal);
            const auto* temporal_panel = find_surface(caps, ObservationSurfaceKind::TemporalPanel);
            const auto* spatial_panel = find_surface(caps, ObservationSurfaceKind::SpatialPanel);
            const auto* st_panel = find_surface(caps, ObservationSurfaceKind::SpatioTemporalPanel);

            t.expect(facts.series_count == 1, "spatial-only single facts preserve single-series cardinality");
            t.expect(!facts.has_time_axis && facts.has_spatial_axis, "spatial-only single facts preserve axis truth");
            t.expect(has_topology(facts, builtin_spatial_topology_name()),
                "spatial-only single facts expose built-in spatial topology inventory");
            t.expect(!has_topology(facts, builtin_temporal_topology_name()),
                "spatial-only single facts omit built-in temporal topology inventory");
            t.expect(row_domain && row_domain->status == ObservationCapabilityStatus::Valid,
                "spatial-only single is valid for Row domain");
            t.expect(temporal && temporal->status == ObservationCapabilityStatus::Invalid,
                "spatial-only single is invalid for Temporal");
            t.expect(has_reason(temporal, ObservationCapabilityReasonCode::MissingTimeAxis),
                "spatial-only single temporal failure cites missing time axis");
            t.expect(spatial && spatial->status == ObservationCapabilityStatus::Valid,
                "spatial-only single is valid for Spatial");
            t.expect(st && st->status == ObservationCapabilityStatus::Invalid,
                "spatial-only single is invalid for Spatiotemporal");
            t.expect(has_reason(st, ObservationCapabilityReasonCode::MissingTimeAxis),
                "spatial-only single spatiotemporal failure cites missing time axis");
            t.expect(temporal_panel && temporal_panel->status == ObservationCapabilityStatus::Invalid,
                "spatial-only single is invalid for Temporal panel");
            t.expect(has_reason(temporal_panel, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis),
                "spatial-only single temporal panel failure cites missing shared per-entity time axis");
            t.expect(has_reason(temporal_panel, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided),
                "spatial-only single temporal panel failure still notes entity schema can be supplied");
            t.expect(spatial_panel && spatial_panel->status == ObservationCapabilityStatus::Conditional,
                "spatial-only single is conditional for Spatial panel");
            t.expect(has_reason(spatial_panel, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided),
                "spatial-only single spatial-panel condition cites entity schema extension");
            t.expect(st_panel && st_panel->status == ObservationCapabilityStatus::Invalid,
                "spatial-only single is invalid for Spatiotemporal panel");
            t.expect(has_reason(st_panel, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis),
                "spatial-only single spatiotemporal-panel failure cites missing shared per-entity time axis");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("A", NumericFrame(schema, 2));
            fs.by_entity.emplace("B", NumericFrame(schema, 2));

            PreprocessReport report{};
            report.produced_frameset = true;
            report.has_entity_axis = true;
            report.has_time_axis = false;
            report.has_spatial_axis = true;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);

            const auto* temporal_panel = find_surface(caps, ObservationSurfaceKind::TemporalPanel);
            const auto* spatial_panel = find_surface(caps, ObservationSurfaceKind::SpatialPanel);
            const auto* st_panel = find_surface(caps, ObservationSurfaceKind::SpatioTemporalPanel);

            t.expect(facts.series_count == 2, "spatial-only panel facts preserve frameset cardinality");
            t.expect(temporal_panel && temporal_panel->status == ObservationCapabilityStatus::Invalid,
                "spatial-only panel is invalid for Temporal panel");
            t.expect(has_reason(temporal_panel, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis),
                "spatial-only panel temporal-panel failure cites missing shared per-entity time axis");
            t.expect(spatial_panel && spatial_panel->status == ObservationCapabilityStatus::Valid,
                "spatial-only panel is valid for Spatial panel");
            t.expect(st_panel && st_panel->status == ObservationCapabilityStatus::Invalid,
                "spatial-only panel is invalid for Spatiotemporal panel");
            t.expect(has_reason(st_panel, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis),
                "spatial-only panel spatiotemporal-panel failure cites missing shared per-entity time axis");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("A", NumericFrame(schema, std::vector<TimeKey>{
                static_cast<TimeKey>(20240101), static_cast<TimeKey>(20240102) }));
            fs.by_entity.emplace("B", NumericFrame(schema, std::vector<TimeKey>{
                static_cast<TimeKey>(20240101), static_cast<TimeKey>(20240102) }));

            PreprocessReport report{};
            report.produced_frameset = true;
            report.has_entity_axis = true;
            report.has_time_axis = true;
            report.has_spatial_axis = false;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);

            const auto* st = find_surface(caps, ObservationSurfaceKind::SpatioTemporal);
            const auto* temporal_panel = find_surface(caps, ObservationSurfaceKind::TemporalPanel);
            const auto* spatial_panel = find_surface(caps, ObservationSurfaceKind::SpatialPanel);
            const auto* st_panel = find_surface(caps, ObservationSurfaceKind::SpatioTemporalPanel);

            t.expect(has_topology(facts, builtin_temporal_topology_name()),
                "temporal panel facts expose built-in temporal topology inventory");
            t.expect(!has_topology(facts, builtin_spatial_topology_name()),
                "temporal panel facts omit built-in spatial topology inventory when spatial axis is absent");
            t.expect(st && st->status == ObservationCapabilityStatus::Conditional,
                "temporal panel facts make Spatiotemporal conditional when spatial schema is absent");
            t.expect(has_reason(st, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided),
                "temporal panel spatiotemporal condition cites spatial schema extension");
            t.expect(temporal_panel && temporal_panel->status == ObservationCapabilityStatus::Valid,
                "temporal panel facts remain valid for Temporal panel");
            t.expect(spatial_panel && spatial_panel->status == ObservationCapabilityStatus::Conditional,
                "temporal panel facts make Spatial panel conditional when spatial schema is absent");
            t.expect(has_reason(spatial_panel, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided),
                "temporal panel spatial-panel condition cites spatial schema extension");
            t.expect(st_panel && st_panel->status == ObservationCapabilityStatus::Conditional,
                "temporal panel facts make Spatiotemporal panel conditional when spatial schema is absent");
            t.expect(has_reason(st_panel, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided),
                "temporal panel spatiotemporal-panel condition cites spatial schema extension");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("__single__", NumericFrame(schema, std::vector<TimeKey>{
                static_cast<TimeKey>(20240101), static_cast<TimeKey>(20240102) }));

            PreprocessReport report{};
            report.produced_frameset = false;
            report.has_entity_axis = false;
            report.has_time_axis = true;
            report.has_spatial_axis = false;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);

            const auto* temporal_panel = find_surface(caps, ObservationSurfaceKind::TemporalPanel);
            const auto* spatial_panel = find_surface(caps, ObservationSurfaceKind::SpatialPanel);
            const auto* st_panel = find_surface(caps, ObservationSurfaceKind::SpatioTemporalPanel);

            t.expect(temporal_panel && temporal_panel->status == ObservationCapabilityStatus::Conditional,
                "temporal single-series facts make Temporal panel conditional when entity schema is absent");
            t.expect(has_reason(temporal_panel, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided),
                "temporal single-series temporal-panel condition cites entity schema extension");
            t.expect(spatial_panel && spatial_panel->status == ObservationCapabilityStatus::Conditional,
                "temporal single-series facts make Spatial panel conditional when entity and spatial schema are absent");
            t.expect(has_reason(spatial_panel, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided),
                "temporal single-series spatial-panel condition cites entity schema extension");
            t.expect(has_reason(spatial_panel, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided),
                "temporal single-series spatial-panel condition cites spatial schema extension");
            t.expect(st_panel && st_panel->status == ObservationCapabilityStatus::Conditional,
                "temporal single-series facts make Spatiotemporal panel conditional when entity and spatial schema are absent");
            t.expect(has_reason(st_panel, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided),
                "temporal single-series spatiotemporal-panel condition cites entity schema extension");
            t.expect(has_reason(st_panel, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided),
                "temporal single-series spatiotemporal-panel condition cites spatial schema extension");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("__single__", NumericFrame(schema, 0));

            PreprocessReport report{};
            report.produced_frameset = false;
            report.has_entity_axis = false;
            report.has_time_axis = false;
            report.has_spatial_axis = false;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);

            const auto* row_domain = find_surface(caps, ObservationSurfaceKind::RowDomain);
            const auto* temporal = find_surface(caps, ObservationSurfaceKind::Temporal);

            t.expect(row_domain && row_domain->status == ObservationCapabilityStatus::Invalid,
                "empty observation facts are invalid for Row domain");
            t.expect(has_reason(row_domain, ObservationCapabilityReasonCode::EmptyObservationDomain),
                "empty observation row-domain failure cites empty row domain");
            t.expect(temporal && temporal->status == ObservationCapabilityStatus::Invalid,
                "empty observation facts are invalid for Temporal");
            t.expect(has_reason(temporal, ObservationCapabilityReasonCode::EmptyObservationDomain),
                "empty observation temporal failure cites empty row domain");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            FrameSet fs{};
            fs.schema = schema;
            fs.by_entity.emplace("empty", NumericFrame(schema, 0));
            fs.by_entity.emplace("nonempty", NumericFrame(schema, 2));

            PreprocessReport report{};
            report.produced_frameset = true;
            report.has_entity_axis = true;
            report.has_time_axis = false;
            report.has_spatial_axis = false;

            const auto facts = make_observation_world_facts(fs, report);
            const auto caps = diagnose_observation_capabilities(facts);
            const auto* row_domain = find_surface(caps, ObservationSurfaceKind::RowDomain);

            t.expect(facts.row_count == 2,
                "frameset observation facts derive row_count from a non-empty entity");
            t.expect(row_domain && row_domain->status == ObservationCapabilityStatus::Valid,
                "frameset observation facts stay row-domain valid when any entity still has rows");
        }

        return true;
    }

    bool test_iso8601_timekeys(ContractAsserts& t) {
        t.out << "[iso8601_timekeys] ISO-8601 -> TimeKey fidelity\n";

        const auto dir = contract_tmp_dir();
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

    bool test_alignment_contracts(ContractAsserts& t) {
        t.out << "[alignment] alignment union/intersection + fill\n";

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

    bool test_row_storage_contracts(ContractAsserts& t) {
        t.out << "[row_storage] row-domain storage seam contracts\n";

        {
            FrameSchema schema;
            NumericFrame frame(schema);
            const ColumnId close_id = schema.columns().get_or_add("close");

            t.expect(frame.row_count() == 0, "NumericFrame default constructor still starts with zero row_count");
            t.expect(frame.time().empty(), "NumericFrame without time payload exposes empty legacy time storage");
            t.expect(!frame.has_index_schema(), "NumericFrame default constructor starts without index_schema");
            t.expect(!frame.has_time_axis(), "NumericFrame default constructor starts without semantic time axis");
            t.expect(!frame.has_schema_owner(), "NumericFrame default constructor does not hold shared schema ownership");

            const auto close = frame.ensure_col(close_id);
            t.expect(close.empty(), "NumericFrame default constructor still materializes zero rows");
        }

        {
            FrameSchema schema;
            NumericFrame frame(schema, static_cast<std::size_t>(3));
            const ColumnId close_id = schema.columns().get_or_add("close");
            auto close = frame.ensure_col(close_id);
            close[0] = 10.0;
            close[1] = 20.0;
            close[2] = 30.0;

            t.expect(frame.row_count() == 3, "NumericFrame row-domain constructor sets row_count without time payload");
            t.expect(frame.time().empty(), "NumericFrame row-domain constructor keeps legacy time storage empty");
            t.expect(close.size() == 3, "NumericFrame columns now size against row_count without time payload");
            t.expect(!frame.has_time_axis(), "row-domain-only NumericFrame does not imply a semantic time axis");

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_frame(frame, bundle, primary);
            t.expect(okdv && bundle != nullptr, "row-domain-only NumericFrame still wraps into aligned runtime helper");
            if (okdv && bundle) {
                t.expect(bundle->row_count() == frame.row_count(),
                    "row-domain-only NumericFrame now keeps row_count through aligned runtime");
                t.expect(bundle->time.empty(),
                    "row-domain-only NumericFrame no longer needs legacy aligned time storage");
                t.expect(!bundle->has_index_schema(),
                    "row-domain-only NumericFrame keeps aligned runtime free of invented index_schema");
                t.expect(bundle->time_axis().empty(),
                    "row-domain-only NumericFrame still yields empty semantic time axis at aligned runtime");
            }
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            NumericFrame frame(schema, static_cast<std::size_t>(2));
            const ColumnId close_id = schema->columns().get_or_add("close");
            const auto close = frame.ensure_col(close_id);

            t.expect(frame.has_schema_owner(), "shared-schema NumericFrame keeps explicit schema ownership");
            t.expect(frame.row_count() == 2, "shared-schema NumericFrame preserves row_count");
            t.expect(close.size() == 2, "shared-schema NumericFrame column sizing follows row_count");
        }

        {
            FrameSchema schema;
            NumericFrame frame(schema, std::vector<TimeKey>{ 11, 12, 13 });
            const ColumnId close_id = schema.columns().get_or_add("close");
            const auto close = frame.ensure_col(close_id);

            t.expect(frame.row_count() == 3, "NumericFrame row_count currently follows legacy time storage");
            t.expect(frame.time().size() == 3, "NumericFrame legacy time payload size matches row_count");
            t.expect(close.size() == 3, "NumericFrame column sizing currently follows legacy time storage");
            t.expect(frame.has_time_axis(), "legacy time-backed NumericFrame still reports time axis by compatibility");
        }

        {
            align::AlignedBundle bundle{};
            bundle.index_schema = std::make_shared<IndexSchema>();
            bundle.index_schema->add_spatial_axis_2d("x", "y");
            bundle.time = { 0, 1, 2 };

            t.expect(bundle.row_count() == 3, "AlignedBundle row_count still follows legacy time storage when time payload exists");
            t.expect(bundle.time.size() == bundle.row_count(), "AlignedBundle legacy time storage size matches row_count");
            t.expect(bundle.time_axis().empty(), "AlignedBundle semantic time_axis stays empty when Time axis is absent");
        }

        {
            auto schema = std::make_shared<FrameSchema>();
            NumericFrame frame(schema, static_cast<std::size_t>(4));
            const ColumnId close_id = schema->columns().get_or_add("close");
            const auto close = frame.ensure_col(close_id);

            align::AlignedBundle bundle{};
            bundle.schema = schema;
            bundle.frames.push_back(frame);

            t.expect(close.size() == 4, "frame-backed AlignedBundle fixture has expected column size");
            t.expect(bundle.row_count() == 4, "AlignedBundle row_count now survives from frame row domain when time payload is empty");
            t.expect(bundle.time.empty(), "frame-backed AlignedBundle fixture keeps time payload empty");
        }

        {
            const auto dir = contract_tmp_dir();
            std::filesystem::create_directories(dir);
            const auto csv = dir / "row_storage_spatial_only.csv";
            const std::string body =
                "x,y,close\n"
                "5,0,10\n"
                "1,0,20\n"
                "3,0,30\n";
            if (!write_text_file(csv, body)) {
                return t.expect(false, "failed to write row_storage spatial-only fixture");
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
            t.expect(r.err.empty(), "spatial-only preprocess succeeds for row_storage contract");
            t.expect(r.frame.has_value(), "spatial-only preprocess yields NumericFrame for row_storage contract");
            if (r.frame) {
                const auto& frame = *r.frame;
                t.expect(frame.row_count() == 3, "spatial-only frame row_count remains stable");
                t.expect(frame.has_index_schema(), "spatial-only frame carries index_schema");
                t.expect(frame.has_schema_owner(), "spatial-only preprocess frame keeps shared schema ownership");
                t.expect(frame.time().empty(), "spatial-only frame no longer materializes legacy row-order time storage");
                t.expect(!frame.has_time_axis(), "spatial-only frame omits semantic time axis");
                if (frame.index_schema()) {
                    t.expect(!frame.index_schema()->has_kind(AxisKind::Time), "spatial-only frame omits semantic time axis");
                    t.expect(frame.index_schema()->has_kind(AxisKind::Spatial), "spatial-only frame carries spatial axis");
                }

                std::shared_ptr<align::AlignedBundle> bundle;
                ColumnRef primary{};
                const bool okdv = make_single_series_dataview_from_frame(frame, bundle, primary);
                t.expect(okdv && bundle != nullptr, "spatial-only stored frame converts into DataView input");
                if (okdv && bundle) {
                    const DataView dv(*bundle);
                    t.expect(bundle->row_count() == frame.row_count(),
                        "spatial-only aligned runtime keeps row_count despite omitted semantic time");
                    t.expect(bundle->time.empty(),
                        "spatial-only aligned runtime no longer carries legacy row-order time storage");
                    t.expect(bundle->time_axis().empty(),
                        "spatial-only aligned runtime omits semantic time axis");
                    t.expect(dv.row_count() == frame.row_count(),
                        "spatial-only DataView row_count stays authoritative without semantic time");
                    t.expect(dv.time_axis().empty(),
                        "spatial-only DataView semantic time axis is empty");
                }
            }
        }

        {
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
        }

        return true;
    }

    bool test_index_schema_propagation(ContractAsserts& t) {
        t.out << "[index_schema] index schema propagation (preprocess -> align -> dataview)\n";

        const auto dir = contract_tmp_dir();
        std::filesystem::create_directories(dir);
        const auto csv = dir / "index_schema_spatial.csv";
        const std::string body =
            "date,entity,close,x,y\n"
            "20240101,A,10,1.0,2.0\n"
            "20240102,A,11,1.0,2.0\n"
            "20240101,B,20,1.0,2.0\n"
            "20240102,B,21,1.0,2.0\n";
        if (!write_text_file(csv, body)) {
            return t.expect(false, "failed to write index schema fixture");
        }

        DataSpec spec{};
        spec.header_policy = HeaderPolicy::RequiredHeaders;
        spec.time_col = "date";
        spec.time_kind = TimeKind::YyyyMmDd;
        spec.entity_col = "entity";
        spec.value_cols = { "close" };
        spec.primary_value_col = "close";
        spec.spatial_x_col = "x";
        spec.spatial_y_col = "y";
        spec.sort_by_time = true;
        spec.dedupe = DataSpec::DedupePolicy::Error;

        auto schema = std::make_shared<FrameSchema>();
        auto prep = preprocess_csv(spec, csv.string(), schema);
        t.expect(prep.err.empty(), "spatial preprocess succeeds");
        t.expect(prep.frameset.has_value(), "spatial preprocess produces FrameSet");
        if (!prep.frameset) return true;

        const auto& fs = *prep.frameset;
        t.expect(fs.index_schema != nullptr, "frameset has index_schema metadata");
        if (fs.index_schema) {
            t.expect(fs.index_schema->has_kind(AxisKind::Time), "frameset schema has time axis");
            t.expect(fs.index_schema->has_kind(AxisKind::Entity), "frameset schema has entity axis");
            t.expect(fs.index_schema->has_kind(AxisKind::Spatial), "frameset schema has spatial axis");

            const auto spatial = fs.index_schema->spatial_axis_2d();
            t.expect(spatial.has_value(), "frameset schema exposes spatial 2D metadata");
            if (spatial) {
                t.expect(spatial->x_col == "x" && spatial->y_col == "y",
                    "frameset schema spatial columns are preserved");
            }

            bool has_time_axis = false;
            for (const auto& axis : fs.index_schema->axes()) {
                if (axis.kind == AxisKind::Time) {
                    has_time_axis = true;
                    t.expect(axis.align_hint == AxisAlignmentHint::Calendar,
                        "time axis alignment hint defaults to calendar");
                    break;
                }
            }
            t.expect(has_time_axis, "frameset schema includes time axis definition");
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        for (const auto& kv : fs.by_entity) {
            inputs.emplace_back(kv.first, kv.second);
        }
        t.expect(inputs.size() == 2, "spatial fixture contains two entities for alignment");
        if (inputs.size() < 2) return true;

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        const auto bundle = align::align_frames(schema, inputs, cfg);

        t.expect(bundle.index_schema != nullptr, "aligned bundle preserves index_schema metadata");
        if (bundle.index_schema) {
            t.expect(bundle.index_schema->has_kind(AxisKind::Spatial),
                "aligned bundle preserves spatial axis");
            const auto spatial = bundle.index_schema->spatial_axis_2d();
            t.expect(spatial.has_value(), "aligned bundle preserves spatial 2D metadata");
            if (spatial) {
                t.expect(spatial->x_col == "x" && spatial->y_col == "y",
                    "aligned bundle spatial columns remain stable");
            }
        }

        const DataView dv(bundle);
        t.expect(dv.index_schema() != nullptr, "DataView exposes index_schema metadata");
        if (dv.index_schema()) {
            t.expect(dv.index_schema()->has_kind(AxisKind::Spatial),
                "DataView index_schema preserves spatial axis");
        }

        return true;
    }

    bool test_topology_contracts(ContractAsserts& t) {
        t.out << "[topology_contracts] topology catalog and runtime sidecar contracts\n";

        const auto make_temporal_spec = []() {
            return make_builtin_temporal_topology_spec();
        };

        const auto make_semantic_spec = []() {
            TopologySpec spec{};
            spec.name = "entity.semantic.graph";
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

            catalog.add(temporal);
            catalog.add(make_semantic_spec());

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
            catalog.add(replacement);
            t.expect(catalog.size() == 2, "adding duplicate topology name replaces existing entry");
            semantic = catalog.find("entity.semantic.graph");
            if (semantic) {
                t.expect(!semantic->weighted, "topology catalog replacement updates existing entry fields");
            }

            const auto snapshot = catalog.snapshot();
            catalog.add(make_probabilistic_spec());
            t.expect(snapshot.size() == 2, "topology catalog snapshot is detached from later mutations");
            t.expect(catalog.size() == 3, "topology catalog remains mutable after snapshot");
            t.expect(std::string(topology_family_name(TopologyFamily::SemanticGraph)) == "semantic_graph",
                "topology family naming helper is stable");
            t.expect(std::string(topology_query_kind_name(TopologyQueryKind::TransitionSet)) == "transition_set",
                "topology query naming helper is stable");
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
            const auto dir = contract_tmp_dir();
            std::filesystem::create_directories(dir);
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
            const auto dir = contract_tmp_dir();
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

    bool test_candle_adapter_contracts(ContractAsserts& t) {
        t.out << "[candle_adapters] candle/cache + frame/DataView conversion\n";

        const CandleSeries base = make_simple_candles(8);
        const auto& rows = base.rows();

        t.expect(base.size() == rows.size(), "CandleSeries size matches row count");
        t.expect(base.closes().size() == rows.size(), "CandleSeries close cache size matches rows");
        t.expect(base.opens().size() == rows.size(), "CandleSeries open cache size matches rows");
        t.expect(base.highs().size() == rows.size(), "CandleSeries high cache size matches rows");
        t.expect(base.lows().size() == rows.size(), "CandleSeries low cache size matches rows");
        t.expect(base.volumes().size() == rows.size(), "CandleSeries volume cache size matches rows");
        t.expect(base.dates().size() == rows.size(), "CandleSeries date cache size matches rows");

        bool cache_values_ok = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Candle& c = rows[i];
            cache_values_ok =
                cache_values_ok &&
                equal_or_both_nonfinite(base.opens()[i], c.open) &&
                equal_or_both_nonfinite(base.highs()[i], c.high) &&
                equal_or_both_nonfinite(base.lows()[i], c.low) &&
                equal_or_both_nonfinite(base.closes()[i], c.close) &&
                equal_or_both_nonfinite(base.volumes()[i], c.volume) &&
                (base.dates()[i] == c.date);
        }
        t.expect(cache_values_ok, "CandleSeries caches preserve row-field parity");

        FrameSchema schema;
        NumericFrame frame = frame_from_candles(base, schema);
        ColumnId open_id{};
        ColumnId high_id{};
        ColumnId low_id{};
        ColumnId close_id{};
        ColumnId volume_id{};
        const bool has_open = schema.columns().find("open", open_id);
        const bool has_high = schema.columns().find("high", high_id);
        const bool has_low = schema.columns().find("low", low_id);
        const bool has_close = schema.columns().find("close", close_id);
        const bool has_volume = schema.columns().find("volume", volume_id);
        t.expect(has_open, "frame_from_candles exposes open column");
        t.expect(has_high, "frame_from_candles exposes high column");
        t.expect(has_low, "frame_from_candles exposes low column");
        t.expect(has_close, "frame_from_candles exposes close column");
        t.expect(has_volume, "frame_from_candles exposes volume column");
        if (!has_open || !has_high || !has_low || !has_close || !has_volume) return true;

        const auto open = frame.col(open_id);
        const auto high = frame.col(high_id);
        const auto low = frame.col(low_id);
        const auto close = frame.col(close_id);
        const auto volume = frame.col(volume_id);
        const auto time = frame.time();
        t.expect(open.size() == base.opens().size(), "NumericFrame open span size matches CandleSeries");
        t.expect(high.size() == base.highs().size(), "NumericFrame high span size matches CandleSeries");
        t.expect(low.size() == base.lows().size(), "NumericFrame low span size matches CandleSeries");
        t.expect(close.size() == base.closes().size(), "NumericFrame close span size matches CandleSeries");
        t.expect(volume.size() == base.volumes().size(), "NumericFrame volume span size matches CandleSeries");
        t.expect(time.size() == base.dates().size(), "NumericFrame time span size matches CandleSeries");

        bool frame_ok = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            frame_ok =
                frame_ok &&
                equal_or_both_nonfinite(open[i], base.opens()[i]) &&
                equal_or_both_nonfinite(high[i], base.highs()[i]) &&
                equal_or_both_nonfinite(low[i], base.lows()[i]) &&
                equal_or_both_nonfinite(close[i], base.closes()[i]) &&
                equal_or_both_nonfinite(volume[i], base.volumes()[i]) &&
                (time[i] == base.dates()[i]);
        }
        t.expect(frame_ok, "frame_from_candles preserves OHLCV/time values in NumericFrame");

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_frame(std::move(frame), bundle, primary);
        t.expect(okdv && bundle != nullptr, "candle-derived NumericFrame converts into aligned DataView");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(primary.series == "__single__", "candle-derived DataView primary series name is stable");
        t.expect(dv.size() == base.size(), "candle-derived DataView size matches CandleSeries rows");
        t.expect(dv.has_col(primary), "candle-derived DataView exposes primary close column");
        if (dv.has_col(primary)) {
            const auto primary_close = dv.col(primary);
            bool close_ok = primary_close.size() == base.closes().size();
            for (std::size_t i = 0; i < primary_close.size() && close_ok; ++i) {
                close_ok = equal_or_both_nonfinite(primary_close[i], base.closes()[i]);
            }
            t.expect(close_ok, "candle-derived DataView preserves close values");
        }
        const auto bundle_time = dv.time();
        t.expect(bundle_time.size() == base.dates().size(), "candle-derived DataView time size matches CandleSeries");
        if (bundle_time.size() == base.dates().size()) {
            bool time_ok = true;
            for (std::size_t i = 0; i < bundle_time.size(); ++i) {
                time_ok = time_ok && (bundle_time[i] == base.dates()[i]);
            }
            t.expect(time_ok, "candle-derived DataView preserves time values");
        }

        return true;
    }

    bool test_kernel_warmup_contracts(ContractAsserts& t) {
        t.out << "[kernel_warmup] warmup + derived-series invariants\n";

        std::vector<double> close;
        std::vector<double> high;
        std::vector<double> low;
        close.reserve(64);
        high.reserve(64);
        low.reserve(64);
        for (std::size_t i = 0; i < 64; ++i) {
            const double c = 100.0 + static_cast<double>(i);
            close.push_back(c);
            high.push_back(c + 0.2);
            low.push_back(c - 0.3);
        }

        const std::size_t n = 14;
        const std::size_t n_dc = 20;
        const std::size_t exp_sd_sma = n - 1;
        const std::size_t exp_sd_ema = (2 * n) - 2;
        const std::size_t exp_atr = n - 1;
        const std::size_t exp_rsi = n;
        const std::size_t exp_dc = n_dc - 1;

        const auto close_span = std::span<const double>(close.data(), close.size());
        const auto high_span = std::span<const double>(high.data(), high.size());
        const auto low_span = std::span<const double>(low.data(), low.size());
        const auto tr = op_kernels::ts_true_range_ohlc(high_span, low_span, close_span);
        const auto atr = op_kernels::ts_atr_wilder(std::span<const double>(tr.data(), tr.size()), n);
        const auto rsi = op_kernels::ts_rsi_wilder(close_span, n);
        const auto sd_sma = op_kernels_math::stddev(close_span, n);
        const auto sd_ema = op_kernels_math::stddev_ema(close_span, n);
        const auto z_sma = op_kernels_math::zscore_sma(close_span, n);
        const auto z_ema = op_kernels_math::zscore_ema(close_span, n);
        const auto dc_u = op_kernels::ts_roll_max(high_span, n_dc);
        const auto dc_l = op_kernels::ts_roll_min(low_span, n_dc);

        std::vector<double> dc_m(close.size(), std::numeric_limits<double>::quiet_NaN());
        std::vector<double> dc_w(close.size(), std::numeric_limits<double>::quiet_NaN());
        for (std::size_t i = 0; i < close.size(); ++i) {
            if (!std::isfinite(dc_u[i]) || !std::isfinite(dc_l[i])) continue;
            dc_m[i] = (dc_u[i] + dc_l[i]) * 0.5;
            dc_w[i] = dc_u[i] - dc_l[i];
        }

        t.expect(first_finite_index(sd_sma) == exp_sd_sma, "stddev_sma warmup matches contract");
        t.expect(first_finite_index(z_sma) == exp_sd_sma, "zscore_sma warmup matches contract");
        t.expect(first_finite_index(sd_ema) == exp_sd_ema, "stddev_ema warmup matches contract");
        t.expect(first_finite_index(z_ema) == exp_sd_ema, "zscore_ema warmup matches contract");
        t.expect(first_finite_index(atr) == exp_atr, "atr_wilder warmup matches contract");
        t.expect(first_finite_index(rsi) == exp_rsi, "rsi_wilder warmup matches contract");
        t.expect(first_finite_index(dc_u) == exp_dc, "donchian upper warmup matches contract");
        t.expect(first_finite_index(dc_l) == exp_dc, "donchian lower warmup matches contract");
        t.expect(first_finite_index(dc_m) == exp_dc, "donchian mid warmup matches contract");
        t.expect(first_finite_index(dc_w) == exp_dc, "donchian width warmup matches contract");

        bool dc_formula_ok = true;
        for (std::size_t i = exp_dc; i < close.size(); ++i) {
            dc_formula_ok =
                dc_formula_ok &&
                equal_or_both_nonfinite(dc_m[i], (dc_u[i] + dc_l[i]) * 0.5) &&
                equal_or_both_nonfinite(dc_w[i], dc_u[i] - dc_l[i]);
        }
        t.expect(dc_formula_ok, "donchian mid/width remain derived from upper/lower bands");

        {
            const std::vector<double> spatial_src = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 1.0, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };
            const SpatialNeighborhoodQuery2D q(
                std::span<const double>(x.data(), x.size()),
                std::span<const double>(y.data(), y.size()));
            const auto spatial_mean = op_kernels::spatial_radius_mean(
                std::span<const double>(spatial_src.data(), spatial_src.size()),
                q,
                1.1,
                true);
            t.expect(first_finite_index(spatial_mean) == 0, "spatial.radius_mean warmup matches child_warmup contract");
            const bool spatial_ok =
                nearly_equal(spatial_mean[0], 15.0) &&
                nearly_equal(spatial_mean[1], 15.0) &&
                nearly_equal(spatial_mean[2], 30.0);
            t.expect(spatial_ok, "spatial.radius_mean follows radius-neighborhood mean contract");
        }

        {
            const std::vector<double> st_src = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<TimeKey> time = {
                static_cast<TimeKey>(20240101),
                static_cast<TimeKey>(20240102),
                static_cast<TimeKey>(20240103),
                static_cast<TimeKey>(20240104)
            };
            const std::vector<double> x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };
            const SpatioTemporalNeighborhoodQuery2D q(
                std::span<const TimeKey>(time.data(), time.size()),
                std::span<const double>(x.data(), x.size()),
                std::span<const double>(y.data(), y.size()));
            const auto st_mean = op_kernels::st_radius_mean(
                std::span<const double>(st_src.data(), st_src.size()),
                q,
                2,
                0.5);
            t.expect(first_finite_index(st_mean) == 1, "st.radius_mean warmup matches child_warmup + (last_n-1) contract");
            const bool st_ok =
                !std::isfinite(st_mean[0]) &&
                nearly_equal(st_mean[1], 15.0) &&
                nearly_equal(st_mean[2], 30.0) &&
                nearly_equal(st_mean[3], 35.0);
            t.expect(st_ok, "st.radius_mean follows spatio-temporal neighborhood mean contract");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
