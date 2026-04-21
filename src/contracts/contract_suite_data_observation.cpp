#include "contract_suite_internal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ingest/observation_capability.h"
#include "ingest/preprocess.h"

namespace sugar::contract_suite_internal {

    bool test_row_storage_numeric_domain_contracts(ContractAsserts& t) {
        t.out << "[row_storage_numeric_domain] row-domain NumericFrame/AlignedBundle storage contracts\n";

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

        return true;
    }

    bool test_row_storage_spatial_runtime_contracts(ContractAsserts& t) {
        t.out << "[row_storage_spatial_runtime] row-domain spatial-only runtime storage contracts\n";

        const auto dir = contract_tmp_case_dir("row_storage_spatial_runtime");
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

        return true;
    }

    bool test_observation_capability_spatial_surface_contracts(ContractAsserts& t) {
        t.out << "[observation_capability_spatial_surfaces] spatial observation-surface capability diagnosis\n";

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

        return true;
    }

    bool test_observation_capability_temporal_extension_contracts(ContractAsserts& t) {
        t.out << "[observation_capability_temporal_extensions] temporal observation-surface extension diagnosis\n";

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

        return true;
    }

    bool test_observation_capability_row_domain_contracts(ContractAsserts& t) {
        t.out << "[observation_capability_row_domain] row-domain observation capability diagnosis\n";

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

    bool test_index_schema_propagation(ContractAsserts& t) {
        t.out << "[index_schema] index schema propagation (preprocess -> align -> dataview)\n";

        const auto dir = contract_tmp_case_dir("index_schema");
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

} // namespace sugar::contract_suite_internal
