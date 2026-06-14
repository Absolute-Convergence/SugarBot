#include "ingest/run_pipeline.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "ingest/align.h"
#include "ingest/data_spec.h"
#include "ingest/json_min.h"
#include "ingest/observation_capability.h"
#include "ingest/preprocess.h"
#include "ingest/utils.h"

namespace sugar {
    namespace fs = std::filesystem;

    namespace {
        struct LegacyEmbeddedObservationInput {
            std::string key;
            std::string path;
            std::string data_spec;
            std::string entity;
        };

        void apply_series_key_entity_mapping(
            align::AlignedBundle& bundle,
            const std::unordered_map<std::string, std::string>& series_key_to_entity);

        bool mount_declared_topologies(
            const std::vector<TopologySpec>& declared_topologies,
            TopologyCatalog& catalog,
            std::string& err) {
            for (const auto& topology : declared_topologies) {
                std::string add_err;
                if (!catalog.add(topology, &add_err)) {
                    err = "layout topology registration failed for '" + topology.name + "': " + add_err;
                    return false;
                }
            }
            return true;
        }

        bool resolve_primary_series_key(
            const FrameSet& fs,
            const RuntimeTargetSpec& target,
            std::string& out_primary_series_key,
            RuntimePrimarySelectionMode& out_mode,
            std::string& err) {
            out_primary_series_key.clear();
            out_mode = RuntimePrimarySelectionMode::Unknown;

            if (fs.by_entity.empty()) {
                err = "runtime target resolution failed: frameset is empty";
                return false;
            }

            if (!target.primary_series_key.empty()) {
                if (fs.by_entity.find(target.primary_series_key) == fs.by_entity.end()) {
                    err = "explicit primary series '" + target.primary_series_key + "' is not present in frameset";
                    return false;
                }
                out_primary_series_key = target.primary_series_key;
                out_mode = RuntimePrimarySelectionMode::ExplicitSeries;
                return true;
            }

            if (fs.by_entity.size() == 1) {
                out_primary_series_key = fs.by_entity.begin()->first;
                out_mode = RuntimePrimarySelectionMode::SingleSeriesImplicit;
                return true;
            }

            if (target.require_primary_series_on_multi_series) {
                err = "multi-series runtime requires explicit primary series selection";
                return false;
            }

            out_primary_series_key = pick_primary_entity_key(fs);
            out_mode = RuntimePrimarySelectionMode::HeuristicCompatibility;
            if (out_primary_series_key.empty()) {
                err = "runtime target resolution failed: heuristic primary selection returned empty series key";
                return false;
            }
            return true;
        }

        std::optional<ParamValue> find_param_value(const ParamMap& plan_params,
            const ParamMap* overrides,
            const std::string& name) {
            if (overrides) {
                auto it = overrides->find(name);
                if (it != overrides->end()) return it->second;
            }
            auto it = plan_params.find(name);
            if (it != plan_params.end()) return it->second;
            return std::nullopt;
        }

        std::string resolve_planspec_relative_path(const PlanSpec& spec, const std::string& raw_path) {
            const std::string normalized_raw = normalize_path_for_runtime(raw_path);
            fs::path p(normalized_raw);
            if (p.is_absolute() || spec.source_path.empty()) {
                return normalized_raw;
            }

            const fs::path base = fs::path(normalize_path_for_runtime(spec.source_path)).parent_path();
            if (base.empty()) {
                return normalized_raw;
            }

            return normalize_path_for_runtime((base / p).lexically_normal().string());
        }

        bool parse_legacy_embedded_observation_inputs(
            const json_min::Value& v,
            std::vector<LegacyEmbeddedObservationInput>& out,
            std::string& err)
        {
            using namespace sugar::json_min;

            if (!v.is_arr()) {
                err = "series_inputs must be an array";
                return false;
            }

            out.clear();
            for (const auto& it : v.as_arr()) {
                if (!it.is_obj()) {
                    err = "series_inputs entries must be objects";
                    return false;
                }

                const auto& o = it.as_obj();
                LegacyEmbeddedObservationInput s{};

                auto get_opt_str = [&](const char* key, std::string& dst) -> bool {
                    if (auto* p = obj_get(o, key)) {
                        if (!p->is_str()) {
                            err = std::string("series_inputs.") + key + " must be string";
                            return false;
                        }
                        dst = p->as_str();
                    }
                    return true;
                };

                if (!get_opt_str("key", s.key)) return false;
                if (!get_opt_str("path", s.path)) return false;
                if (!get_opt_str("data_spec", s.data_spec)) return false;
                if (!get_opt_str("entity", s.entity)) return false;

                out.push_back(std::move(s));
            }
            return true;
        }

        bool load_legacy_embedded_observation_inputs(
            const PlanSpec& spec,
            std::vector<LegacyEmbeddedObservationInput>& out,
            std::string& err)
        {
            using namespace sugar::json_min;

            out.clear();
            if (spec.source_path.empty()) {
                return true;
            }

            std::ifstream fp(spec.source_path, std::ios::binary);
            if (!fp) {
                err = "could not reopen PlanSpec file for legacy observation compatibility: " + spec.source_path;
                return false;
            }

            std::ostringstream oss;
            oss << fp.rdbuf();

            Value root;
            ParseError perr;
            if (!parse(oss.str(), root, perr)) {
                err = perr.message + " (line " + std::to_string(perr.line) + ", col " + std::to_string(perr.col) + ")";
                return false;
            }
            if (!root.is_obj()) {
                err = "PlanSpec root must be object";
                return false;
            }

            const auto& o = root.as_obj();
            const auto* legacy = obj_get(o, "series_inputs");
            if (!legacy) {
                return true;
            }

            return parse_legacy_embedded_observation_inputs(*legacy, out, err);
        }

        bool build_embedded_observation_runtime_from_legacy_inputs(
            const PlanSpec& spec,
            const std::vector<LegacyEmbeddedObservationInput>& legacy_inputs,
            const std::string& primary_col_name,
            ResolvedRuntimeInput& out,
            std::string& err)
        {
            out = ResolvedRuntimeInput{};
            out.source_kind = RuntimeObservationSourceKind::LegacyEmbeddedPlanCompat;

            if (legacy_inputs.empty()) {
                err = "legacy embedded observation adapter found no embedded inputs.";
                return false;
            }
            if (primary_col_name.empty()) {
                err = "PlanSpec primary column name is empty.";
                return false;
            }

            align::AlignConfig cfg{};
            cfg.calendar = align::CalendarPolicy::Intersection;
            cfg.missing = align::MissingPolicy::NaN;

            auto schema = std::make_shared<FrameSchema>();
            FrameSet selected_frames{};
            selected_frames.schema = schema;
            PreprocessReport primary_preprocess{};

            for (std::size_t i = 0; i < legacy_inputs.size(); ++i) {
                const auto& si = legacy_inputs[i];
                const std::string idx = "series_inputs[" + std::to_string(i) + "]";

                if (si.key.empty()) {
                    err = idx + ": missing required 'key'.";
                    return false;
                }
                if (si.path.empty()) {
                    err = idx + " (" + si.key + "): missing required 'path'.";
                    return false;
                }
                if (si.data_spec.empty()) {
                    err = idx + " (" + si.key + "): missing required 'data_spec'.";
                    return false;
                }
                if (out.series_key_to_entity.find(si.key) != out.series_key_to_entity.end()) {
                    err = idx + ": duplicate series key '" + si.key + "'.";
                    return false;
                }

                DataSpec ds{};
                std::string ds_err;
                const std::string resolved_data_spec_path = resolve_planspec_relative_path(spec, si.data_spec);
                if (!load_data_spec_file(resolved_data_spec_path, ds, ds_err)) {
                    err = idx + " (" + si.key + "): failed to load data_spec '" + si.data_spec + "' (resolved '" + resolved_data_spec_path + "'): " + ds_err;
                    return false;
                }

                const std::string resolved_data_path = resolve_planspec_relative_path(spec, si.path);
                FrameSet fs{};
                PreprocessReport report{};
                std::string prep_err;
                if (!preprocess_to_frameset(ds, resolved_data_path, fs, report, prep_err, cfg, schema)) {
                    err = idx + " (" + si.key + "): " + prep_err
                        + " for path '" + si.path + "' (resolved '" + resolved_data_path + "')";
                    return false;
                }

                NumericFrame frame(*schema);
                std::string selected_entity = "__single__";
                std::string resolved_entity = "__single__";
                if (!report.produced_frameset && !si.entity.empty()) {
                    err = idx + " (" + si.key + "): entity was provided but data_spec produced a single frame (no entity split).";
                    return false;
                }
                if (si.entity.empty()) {
                    if (fs.by_entity.size() != 1) {
                        err = idx + " (" + si.key + "): entity is required because data_spec path produced multiple entities.";
                        return false;
                    }
                    selected_entity = fs.by_entity.begin()->first;
                    // Single-frame preprocess uses a synthetic internal key; preserve
                    // distinct provenance per embedded series at the runtime boundary.
                    resolved_entity = report.produced_frameset ? selected_entity : si.key;
                }
                else {
                    selected_entity = si.entity;
                    resolved_entity = si.entity;
                }

                auto it = fs.by_entity.find(selected_entity);
                if (it == fs.by_entity.end()) {
                    err = idx + " (" + si.key + "): entity '" + selected_entity + "' not found in preprocessed data.";
                    return false;
                }
                frame = it->second;

                if (i == 0) {
                    out.ingest_spec = ds;
                    out.primary_input_path = resolved_data_path;
                    out.primary_entity_key = resolved_entity;
                    primary_preprocess = report;
                }

                out.series_key_to_entity.emplace(si.key, resolved_entity);
                selected_frames.by_entity.emplace(si.key, std::move(frame));
            }

            // Legacy compatibility policy: embedded inputs imply the first declared
            // series is the primary series unless the caller provides observations.
            const std::string primary_series = legacy_inputs.front().key;
            out.frameset = std::make_shared<FrameSet>(std::move(selected_frames));

            PreprocessReport runtime_preprocess = primary_preprocess;
            runtime_preprocess.series_keys.clear();
            runtime_preprocess.series_keys.reserve(out.frameset->by_entity.size());
            for (const auto& [series_key, _] : out.frameset->by_entity) {
                runtime_preprocess.series_keys.push_back(series_key);
            }
            std::sort(runtime_preprocess.series_keys.begin(), runtime_preprocess.series_keys.end());
            runtime_preprocess.produced_frameset = out.frameset->by_entity.size() > 1;
            runtime_preprocess.has_entity_axis = out.frameset->by_entity.size() > 1;

            if (!out.ingest_spec.has_value()) {
                out.ingest_spec = DataSpec{};
            }

            if (!out.frameset) {
                err = "embedded observation runtime frameset is missing.";
                return false;
            }

            if (!align_frameset(*out.frameset, cfg, out.aligned_bundle, err)) {
                err = "embedded observation alignment failed: " + err;
                return false;
            }
            apply_series_key_entity_mapping(*out.aligned_bundle, out.series_key_to_entity);

            ColumnId primary_col{};
            if (!out.aligned_bundle->schema || !out.aligned_bundle->schema->columns().find(primary_col_name, primary_col)) {
                err = "PlanSpec primary column '" + primary_col_name + "' is unknown in aligned schema.";
                return false;
            }

            out.primary_ref = ColumnRef{ primary_series, primary_col };
            out.primary_series_index = out.aligned_bundle->find_series_index(primary_series);
            out.primary_selection_mode = RuntimePrimarySelectionMode::LegacyCompatibility;
            if (out.primary_series_index == static_cast<std::size_t>(-1) ||
                out.primary_series_index >= out.aligned_bundle->frames.size())
            {
                err = "primary series '" + primary_series + "' is missing after alignment.";
                return false;
            }

            out.topology_catalog = out.aligned_bundle->topology_catalog;
            out.run_manifest = make_run_manifest(
                runtime_preprocess,
                primary_series,
                out.topology_catalog,
                out.primary_selection_mode);
            append_observation_capability_snapshot(
                diagnose_observation_capabilities(make_observation_world_facts(out)),
                out.run_manifest);
            const DataView dv(*out.aligned_bundle);
            if (!dv.has_col(out.primary_ref)) {
                err = "PlanSpec primary column '" + primary_col_name + "' is missing for primary series '" + primary_series + "'.";
                return false;
            }

            return true;
        }

        void apply_series_key_entity_mapping(
            align::AlignedBundle& bundle,
            const std::unordered_map<std::string, std::string>& series_key_to_entity) {
            bundle.entity_keys.clear();
            bundle.entity_keys.reserve(bundle.series_keys.size());
            for (const auto& series_key : bundle.series_keys) {
                auto it = series_key_to_entity.find(series_key);
                if (it != series_key_to_entity.end() && !it->second.empty()) {
                    bundle.entity_keys.push_back(it->second);
                }
                else {
                    bundle.entity_keys.push_back(series_key);
                }
            }
        }
    } // namespace

    bool preprocess_to_frameset(const DataSpec& spec,
        const std::string& csv_path,
        FrameSet& out,
        PreprocessReport& out_report,
        std::string& err,
        const std::optional<align::AlignConfig>& align_cfg,
        std::shared_ptr<FrameSchema> schema,
        TabularFileFormat input_format)
    {
        if (!schema) {
            schema = std::make_shared<FrameSchema>();
        }
        auto prep = preprocess_tabular(spec, csv_path, input_format, schema);
        if (!prep.err.empty()) {
            out_report = make_preprocess_report(spec, prep, align_cfg);
            err = "preprocess failed: " + prep.err;
            return false;
        }

        FrameSet fs{};
        fs.schema = schema;
        if (prep.frameset) {
            fs = std::move(*prep.frameset);
        }
        else if (prep.frame) {
            auto f = std::move(*prep.frame);
            fs.index_schema = f.index_schema();
            if (!fs.index_schema) {
                fs.index_schema = IndexSchema::make_time_entity();
                f.set_index_schema(fs.index_schema);
            }
            fs.by_entity.emplace("__single__", std::move(f));
        }
        else {
            err = "preprocess returned no output frame(s)";
            return false;
        }

        if (fs.by_entity.empty()) {
            err = "preprocess returned empty FrameSet";
            return false;
        }

        out_report = make_preprocess_report(spec, prep, align_cfg);
        out = std::move(fs);
        return true;
    }

    bool align_frameset(const FrameSet& fs,
        const align::AlignConfig& cfg,
        std::shared_ptr<align::AlignedBundle>& out,
        std::string& err)
    {
        if (!fs.schema) {
            err = "frameset schema is missing";
            return false;
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            inputs.emplace_back(kv.first, kv.second);
        }
        try {
            out = std::make_shared<align::AlignedBundle>(align::align_frames(fs.schema, inputs, cfg));
            if (!out->topology_catalog) {
                out->topology_catalog = std::make_shared<TopologyCatalog>();
            }
            return true;
        }
        catch (const std::exception& ex) {
            err = std::string("alignment failed: ") + ex.what();
            out.reset();
            return false;
        }
    }

    std::string pick_primary_entity_key(const FrameSet& fs) {
        std::string best;
        std::size_t best_rows = 0;
        for (const auto& kv : fs.by_entity) {
            const auto rows = kv.second.row_count();
            if (best.empty() || rows > best_rows || (rows == best_rows && kv.first < best)) {
                best = kv.first;
                best_rows = rows;
            }
        }
        return best;
    }

    align::AlignConfig make_align_config_from_data_spec(const DataSpec& spec) {
        align::AlignConfig cfg{};
        cfg.calendar = spec.alignment_calendar;
        cfg.missing = align::MissingPolicy::NaN;
        cfg.non_temporal_multi_series = spec.alignment_non_temporal_multi_series;
        return cfg;
    }

    RuntimeBuildOptions make_strict_runtime_build_options(
        const DataSpec& spec,
        RuntimeTargetSpec target) {
        RuntimeBuildOptions options{};
        options.align_config = make_align_config_from_data_spec(spec);
        options.target = std::move(target);
        options.target.require_primary_series_on_multi_series = true;
        return options;
    }

    RuntimeBuildOptions make_compatibility_runtime_build_options(
        const DataSpec& spec,
        RuntimeTargetSpec target) {
        RuntimeBuildOptions options{};
        options.align_config = make_align_config_from_data_spec(spec);
        options.target = std::move(target);
        options.target.require_primary_series_on_multi_series = false;
        return options;
    }

    bool resolve_planspec_primary_col_name(
        const PlanSpec& spec,
        const ParamMap* overrides,
        std::string& out_name,
        std::string& err) {
        out_name = "close";
        auto pv = find_param_value(spec.params, overrides, "primary_value_col");
        if (!pv.has_value()) return true;
        if (std::holds_alternative<std::string>(*pv)) {
            out_name = std::get<std::string>(*pv);
            return true;
        }
        if (std::holds_alternative<ParamRef>(*pv)) {
            const std::string& ref = std::get<ParamRef>(*pv).name;
            auto rv = find_param_value(spec.params, overrides, ref);
            if (rv.has_value() && std::holds_alternative<std::string>(*rv)) {
                out_name = std::get<std::string>(*rv);
                return true;
            }
            err = "primary_value_col param ref '" + ref + "' did not resolve to string";
            return false;
        }
        err = "primary_value_col must be string or {ref:...}";
        return false;
    }

    bool plan_spec_has_embedded_observation_inputs(
        const PlanSpec& spec,
        bool& out_has,
        std::string& err)
    {
        out_has = false;

        std::vector<LegacyEmbeddedObservationInput> legacy_inputs;
        if (!load_legacy_embedded_observation_inputs(spec, legacy_inputs, err)) {
            return false;
        }

        out_has = !legacy_inputs.empty();
        return true;
    }

    bool build_resolved_observation_world(
        const DataSpec& spec,
        const std::string& csv_path,
        const align::AlignConfig& cfg,
        const std::vector<TopologySpec>& declared_topologies,
        ResolvedObservationWorld& out,
        std::string& err,
        TabularFileFormat input_format)
    {
        out = ResolvedObservationWorld{};
        out.source_kind = RuntimeObservationSourceKind::AppSuppliedObservations;
        out.ingest_spec = spec;
        out.primary_input_path = normalize_path_for_runtime(csv_path);

        FrameSet fs{};
        PreprocessReport preprocess_report{};
        if (!preprocess_to_frameset(
                spec,
                out.primary_input_path,
                fs,
                preprocess_report,
                err,
                cfg,
                {},
                input_format)) {
            out.run_manifest = make_run_manifest(
                preprocess_report,
                {},
                {},
                RuntimePrimarySelectionMode::Unknown);
            return false;
        }

        out.frameset = std::make_shared<FrameSet>(std::move(fs));
        for (const auto& [series_key, _] : out.frameset->by_entity) {
            out.series_key_to_entity.emplace(series_key, series_key);
        }
        out.run_manifest = make_run_manifest(
            preprocess_report,
            {},
            {},
            RuntimePrimarySelectionMode::Unknown);

        if (!align_frameset(*out.frameset, cfg, out.aligned_bundle, err)) {
            return false;
        }
        apply_series_key_entity_mapping(*out.aligned_bundle, out.series_key_to_entity);
        if (!out.aligned_bundle->topology_catalog) {
            out.aligned_bundle->topology_catalog = std::make_shared<TopologyCatalog>();
        }
        if (!mount_declared_topologies(declared_topologies, *out.aligned_bundle->topology_catalog, err)) {
            return false;
        }

        out.topology_catalog = out.aligned_bundle->topology_catalog;
        out.run_manifest = make_run_manifest(
            preprocess_report,
            {},
            out.topology_catalog,
            RuntimePrimarySelectionMode::Unknown);
        append_observation_capability_snapshot(
            diagnose_observation_capabilities(make_observation_world_facts(out)),
            out.run_manifest);
        return true;
    }

    bool build_resolved_observation_world(
        const LayoutSpec& layout,
        const std::string& csv_path,
        ResolvedObservationWorld& out,
        std::string& err,
        TabularFileFormat input_format)
    {
        return build_resolved_observation_world(
            layout.ingest,
            csv_path,
            make_align_config_from_data_spec(layout.ingest),
            layout.topologies,
            out,
            err,
            input_format);
    }

    bool bind_resolved_runtime_target(
        const ResolvedObservationWorld& world,
        const std::string& primary_col_name,
        const RuntimeTargetSpec& target,
        ResolvedRuntimeInput& out,
        std::string& err)
    {
        out = ResolvedRuntimeInput{};
        out.source_kind = world.source_kind;
        out.ingest_spec = world.ingest_spec;
        out.primary_input_path = world.primary_input_path;
        out.frameset = world.frameset;
        out.aligned_bundle = world.aligned_bundle;
        out.topology_catalog = world.topology_catalog;
        out.run_manifest = world.run_manifest;
        out.series_key_to_entity = world.series_key_to_entity;

        if (!out.frameset || !out.aligned_bundle) {
            err = "runtime target binding requires a resolved observation world";
            return false;
        }

        std::string primary_series_key;
        if (!resolve_primary_series_key(
                *out.frameset,
                target,
                primary_series_key,
                out.primary_selection_mode,
                err)) {
            return false;
        }

        ColumnId primary_id{};
        if (!out.aligned_bundle->schema || !out.aligned_bundle->schema->columns().find(primary_col_name, primary_id)) {
            err = "primary column not found in schema: " + primary_col_name;
            return false;
        }

        out.primary_entity_key = primary_series_key;
        out.primary_ref = ColumnRef{ primary_series_key, primary_id };
        out.primary_series_index = out.aligned_bundle->find_series_index(out.primary_ref.series);
        if (out.primary_series_index == static_cast<std::size_t>(-1) ||
            out.primary_series_index >= out.aligned_bundle->frames.size())
        {
            err = "selected series missing after alignment";
            return false;
        }

        out.run_manifest.primary_series_key = out.primary_ref.series;
        out.run_manifest.primary_selection_mode = out.primary_selection_mode;
        return true;
    }

    bool build_resolved_runtime_input(
        const DataSpec& spec,
        const std::string& csv_path,
        const std::string& primary_col_name,
        const RuntimeBuildOptions& options,
        ResolvedRuntimeInput& out,
        std::string& err,
        TabularFileFormat input_format)
    {
        ResolvedObservationWorld world{};
        if (!build_resolved_observation_world(
                spec,
                csv_path,
                options.align_config,
                {},
                world,
                err,
                input_format)) {
            return false;
        }
        return bind_resolved_runtime_target(world, primary_col_name, options.target, out, err);
    }

    bool build_resolved_runtime_input(
        const DataSpec& spec,
        const std::string& csv_path,
        const std::string& primary_col_name,
        const align::AlignConfig& cfg,
        ResolvedRuntimeInput& out,
        std::string& err,
        TabularFileFormat input_format)
    {
        RuntimeBuildOptions options = make_compatibility_runtime_build_options(spec);
        options.align_config = cfg;
        return build_resolved_runtime_input(spec, csv_path, primary_col_name, options, out, err, input_format);
    }

    bool build_embedded_plan_observation_compat_runtime(
        const PlanSpec& spec,
        ResolvedRuntimeInput& out,
        std::string& err)
    {
        std::vector<LegacyEmbeddedObservationInput> legacy_inputs;
        if (!load_legacy_embedded_observation_inputs(spec, legacy_inputs, err)) {
            return false;
        }
        if (legacy_inputs.empty()) {
            err = "embedded legacy observation compatibility found no embedded inputs; canonical custom execution expects caller-supplied DataView observations";
            return false;
        }

        std::string primary_col_name;
        std::string primary_err;
        if (!resolve_planspec_primary_col_name(spec, nullptr, primary_col_name, primary_err)) {
            err = "PlanSpec primary_value_col: " + primary_err;
            return false;
        }

        std::string input_err;
        if (!build_embedded_observation_runtime_from_legacy_inputs(spec, legacy_inputs, primary_col_name, out, input_err)) {
            err = "PlanSpec embedded legacy-observation compatibility adapter failed: " + input_err;
            return false;
        }
        return true;
    }

} // namespace sugar
