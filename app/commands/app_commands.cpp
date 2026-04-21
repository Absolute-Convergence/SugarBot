#include "commands/app_commands.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "reporting/app_reporters.h"
#include "runtime/app_runtime.h"

#include "app_support/feature_dump.h"
#include "contracts/contract_suite.h"
#include "ops/op_registry.h"
#include "sweep/panel_runner.h"
#include "sweep/strategy_menu.h"
#include "ingest/utils.h"

namespace {

    std::optional<std::pair<sugar::TimeKey, sugar::TimeKey>> time_minmax(const sugar::NumericFrame& f) {
        if (!f.has_time_axis()) return std::nullopt;
        const auto t = f.time();
        if (t.empty()) return std::nullopt;
        const auto [mn, mx] = std::minmax_element(t.begin(), t.end());
        return std::make_pair(*mn, *mx);
    }

    sugar::PanelRankingSpec default_panel_ranking_spec() {
        sugar::PanelRankingSpec spec{};
        spec.metric = sugar::PanelRankingMetric::PnlDd;
        spec.dd_weight = 0.25;
        return spec;
    }

    std::size_t observation_world_series_count(const sugar::ResolvedObservationWorld& world) {
        if (world.aligned_bundle) {
            return world.aligned_bundle->series_keys.size();
        }
        if (world.frameset) {
            return world.frameset->by_entity.size();
        }
        return 0;
    }

    bool observation_world_is_multi_series(const sugar::ResolvedObservationWorld& world) {
        return observation_world_series_count(world) > 1;
    }

    bool resolve_target_entity_series_key(
        const sugar::ResolvedObservationWorld& world,
        const std::string& target_entity,
        std::string& out_series_key,
        std::string& err) {
        out_series_key.clear();
        if (target_entity.empty()) {
            err = "logic.run.target_entity is empty";
            return false;
        }

        std::vector<std::string> matches;
        for (const auto& [series_key, entity_key] : world.series_key_to_entity) {
            if (entity_key == target_entity) {
                matches.push_back(series_key);
            }
        }

        if (matches.empty()) {
            err = "logic.run.target_entity '" + target_entity + "' is not present in the observation world";
            return false;
        }
        if (matches.size() > 1) {
            std::sort(matches.begin(), matches.end());
            err = "logic.run.target_entity '" + target_entity + "' is ambiguous across multiple series";
            return false;
        }

        out_series_key = matches.front();
        return true;
    }

    bool resolve_world_primary_col_id(
        const sugar::ResolvedObservationWorld& world,
        sugar::ColumnId& out,
        std::string& err) {
        if (!world.aligned_bundle || !world.aligned_bundle->schema || !world.ingest_spec.has_value()) {
            err = "observation world is missing aligned schema";
            return false;
        }
        const std::string& primary_name = world.ingest_spec->primary_value_col;
        if (!world.aligned_bundle->schema->columns().find(primary_name, out)) {
            err = "layout primary_value_col '" + primary_name + "' is not present in aligned schema";
            return false;
        }
        return true;
    }

    bool resolve_run_ranking_spec(
        const sugar::PlanRunSpec& run,
        sugar::PanelRankingSpec& out,
        std::string& err) {
        const sugar::PlanRunRankingMetric metric =
            run.ranking_metric.value_or(sugar::PlanRunRankingMetric::Pnl);
        switch (metric) {
        case sugar::PlanRunRankingMetric::Pnl:
            if (run.ranking_dd_weight.has_value()) {
                err = "logic.run.ranking_dd_weight applies only when logic.run.ranking_metric=pnl_dd";
                return false;
            }
            out.metric = sugar::PanelRankingMetric::Pnl;
            out.dd_weight = 0.0;
            return true;
        case sugar::PlanRunRankingMetric::PnlDd:
            if (!run.ranking_dd_weight.has_value()) {
                err = "logic.run.ranking_dd_weight is required when logic.run.ranking_metric=pnl_dd";
                return false;
            }
            out.metric = sugar::PanelRankingMetric::PnlDd;
            out.dd_weight = *run.ranking_dd_weight;
            return true;
        }
        err = "logic.run.ranking_metric is invalid";
        return false;
    }

    bool load_layout_and_world(
        const std::string& layout_path,
        const std::string& csv_path,
        sugar::LayoutSpec& layout,
        sugar::ResolvedObservationWorld& world,
        std::string& err) {
        std::string layout_err;
        if (!sugar::load_layout_spec_file(layout_path, layout, layout_err)) {
            err = "failed to load layout spec: " + layout_err;
            return false;
        }

        std::string world_err;
        if (!sugar::build_resolved_observation_world(layout, csv_path, world, world_err)) {
            err = world_err;
            return false;
        }

        return true;
    }

    bool load_logic_for_report(
        const std::string& logic_path,
        sugar::PlanSpec& spec,
        std::string& err) {
        bool has_embedded_observations = false;
        std::string plan_err;
        if (!sugar::app::load_noninteractive_planspec(
                logic_path,
                spec,
                has_embedded_observations,
                plan_err)) {
            err = "failed to load logic spec: " + plan_err;
            return false;
        }
        if (has_embedded_observations) {
            err = "logic.json embeds legacy observation inputs. That compatibility path is interactive-only.";
            return false;
        }
        return true;
    }

    void print_layout_contract_brief(std::ostream& out) {
        out
            << "Layout contract:\n"
            << "  - `layout.json` can be either a raw `DataSpec` object or a wrapped `LayoutSpec` with an `ingest` object.\n"
            << "  - `data_spec` is not a supported wrapper key; use the raw object directly or wrap it under `ingest`.\n"
            << "  - The same ingest schema is used by `run`, `--obs-report`, `--plan-report`, `--feature-dump`, `--prep-dump`, `--align-dump`, `--run-panel`, and interactive `--data-spec`.\n"
            << "  - Need at least one observation axis: `time_col` + `time_kind`, or both `spatial_x_col` + `spatial_y_col`.\n"
            << "  - Valid `time_kind`: `YyyyMmDd`, `Iso8601`, `YearQuarter`, `YearMonth`, `Index`.\n"
            << "  - Run `sugarbot --dump-layout` for required fields, defaults, enum values, and worked examples.\n";
    }

    void print_layout_contract_dump(std::ostream& out) {
        out
            << "SugarBot Layout Contract\n"
            << "\n"
            << "`layout.json` for `sugarbot run <layout.json> <logic.json> <data.csv>` is not a special app wrapper.\n"
            << "Use one of these two supported shapes:\n"
            << "  1. Raw `DataSpec` object at the JSON root.\n"
            << "  2. Wrapped `LayoutSpec` object with `ingest`, optional `alignment`, and optional `topologies`.\n"
            << "\n"
            << "`data_spec` is not a supported wrapper key.\n"
            << "Do not use `{ \"data_spec\": { ... } }`.\n"
            << "If you want a wrapper, the key is `ingest`.\n"
            << "\n"
            << "The same ingest contract is accepted by:\n"
            << "  - `run`, `--obs-report`, `--plan-report`, `--feature-dump`\n"
            << "  - `--prep-dump`, `--align-dump`, `--run-panel`\n"
            << "  - interactive `--data-spec`\n"
            << "\n"
            << "Required and conditional fields\n"
            << "  - Need at least one observation axis:\n"
            << "    - temporal: `time_col` plus `time_kind`\n"
            << "    - spatial: both `spatial_x_col` and `spatial_y_col`\n"
            << "  - `value_cols` is required unless an explicit `input_profile` fills it.\n"
            << "  - `primary_value_col` is required unless an explicit `input_profile` fills it.\n"
            << "  - If `input_profile` is `auto` and `time_col` is present, `time_kind` is required.\n"
            << "  - If `time_col` is omitted, `time_kind` must also be omitted.\n"
            << "  - `columns_by_index` is required when `header_policy=HeaderlessPositional`.\n"
            << "  - `entity_col` is optional, but it is the normal way to declare multi-entity or panel inputs.\n"
            << "  - `rename` is optional and is used to normalize incoming CSV header names.\n"
            << "  - `missing_tokens` is optional. Blank cells are still treated as missing even if this list is empty.\n"
            << "\n"
            << "Defaults when omitted\n"
            << "  - `input_profile`: `auto`\n"
            << "  - `header_policy`: `RequiredHeaders`\n"
            << "  - `sort_by_time`: `true`\n"
            << "  - `dedupe`: `Error`\n"
            << "  - `missing_default`: `{ \"kind\": \"NaN\" }`\n"
            << "  - `alignment_calendar`: `Intersection`\n"
            << "  - `alignment_non_temporal_multi_series`: `Reject`\n"
            << "  - `entity_col`, `spatial_x_col`, `spatial_y_col`, `rename`, `columns_by_index`: omitted\n"
            << "  - `missing_tokens`: no named tokens beyond blank-cell handling\n"
            << "\n"
            << "Valid enum values\n"
            << "  - `input_profile`: `auto`, `legacy_ohlcv_date`, `iso_ohlcv_time`\n"
            << "  - `header_policy`: `RequiredHeaders`, `OptionalHeaders`, `HeaderlessPositional`\n"
            << "  - `time_kind`:\n"
            << "    - `YyyyMmDd`: date-only values such as `20211014`, `2021-10-14`, or `2021/10/14`\n"
            << "    - `Iso8601`: timestamps such as `2024-10-09T01:00:00-06:00` or `2024-10-09T07:00:00Z`\n"
            << "    - `YearQuarter`: `2024Q1` or `2024-Q1`\n"
            << "    - `YearMonth`: `202410` or `2024-10`\n"
            << "    - `Index`: integer row keys; blank values fall back to the row index\n"
            << "  - `dedupe`: `Error`, `First`, `Last`\n"
            << "  - `missing_default.kind`: `DropRow`, `NaN`, `Ffill`, `Const`\n"
            << "  - `alignment_calendar`: `Intersection`, `Union`\n"
            << "  - `alignment_non_temporal_multi_series`: `Reject`, `EqualLengthRowDomain`\n"
            << "\n"
            << "Input profile presets\n"
            << "  - `legacy_ohlcv_date` fills `time_col=\"date\"`, `time_kind=\"YyyyMmDd\"`,\n"
            << "    `value_cols=[\"open\",\"high\",\"low\",\"close\",\"volume\"]`, `primary_value_col=\"close\"`,\n"
            << "    and a basic OHLCV header rename map.\n"
            << "  - `iso_ohlcv_time` fills `time_col=\"time\"`, `time_kind=\"Iso8601\"`,\n"
            << "    `value_cols=[\"open\",\"high\",\"low\",\"close\",\"volume\"]`, `primary_value_col=\"close\"`,\n"
            << "    and a timestamp-oriented OHLCV header rename map.\n"
            << "  - `auto` does not fill those observation fields for you.\n"
            << "\n"
            << "Example: raw single-series `layout.json`\n"
            << "{\n"
            << "  \"time_col\": \"date\",\n"
            << "  \"time_kind\": \"YyyyMmDd\",\n"
            << "  \"header_policy\": \"RequiredHeaders\",\n"
            << "  \"value_cols\": [\"close\"],\n"
            << "  \"primary_value_col\": \"close\",\n"
            << "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
            << "  \"missing_default\": { \"kind\": \"NaN\" },\n"
            << "  \"sort_by_time\": true,\n"
            << "  \"dedupe\": \"Error\"\n"
            << "}\n"
            << "\n"
            << "Example: raw multi-entity temporal `layout.json`\n"
            << "{\n"
            << "  \"time_col\": \"date\",\n"
            << "  \"time_kind\": \"YyyyMmDd\",\n"
            << "  \"entity_col\": \"symbol\",\n"
            << "  \"value_cols\": [\"close\", \"volume\"],\n"
            << "  \"primary_value_col\": \"close\",\n"
            << "  \"alignment_calendar\": \"Intersection\",\n"
            << "  \"alignment_non_temporal_multi_series\": \"Reject\"\n"
            << "}\n"
            << "\n"
            << "Example: raw spatial-only `layout.json`\n"
            << "{\n"
            << "  \"spatial_x_col\": \"x\",\n"
            << "  \"spatial_y_col\": \"y\",\n"
            << "  \"entity_col\": \"region\",\n"
            << "  \"value_cols\": [\"price_index\"],\n"
            << "  \"primary_value_col\": \"price_index\"\n"
            << "}\n"
            << "\n"
            << "Example: wrapped `LayoutSpec` with `ingest`\n"
            << "{\n"
            << "  \"ingest\": {\n"
            << "    \"time_col\": \"date\",\n"
            << "    \"time_kind\": \"YyyyMmDd\",\n"
            << "    \"value_cols\": [\"close\"],\n"
            << "    \"primary_value_col\": \"close\"\n"
            << "  },\n"
            << "  \"alignment\": {\n"
            << "    \"calendar\": \"Intersection\",\n"
            << "    \"non_temporal_multi_series\": \"Reject\"\n"
            << "  },\n"
            << "  \"topologies\": []\n"
            << "}\n"
            << "\n"
            << "Validation workflow\n"
            << "  1. `sugarbot --obs-report layout.json data.csv`\n"
            << "  2. `sugarbot --plan-report layout.json logic.json data.csv`\n"
            << "  3. `sugarbot run layout.json logic.json data.csv`\n";
    }

    const char* valid_run_scope_values_text() {
        return "valid values: primary_series or per_entity_ranked";
    }

    std::string missing_run_scope_hint_text() {
        return std::string("multi-entity runs require logic.run.scope in logic.json; ") +
            valid_run_scope_values_text();
    }

    void print_observation_stage_header(std::ostream& out, const char* label) {
        out << "=== " << label << " ===\n";
    }

    void print_prebinding_assessment_header(std::ostream& out, bool has_post_binding_stage) {
        print_observation_stage_header(
            out,
            has_post_binding_stage
            ? "[1/2] Pre-binding observation assessment"
            : "[1/1] Pre-binding observation assessment");
    }

    void print_runtime_binding_header(std::ostream& out) {
        print_observation_stage_header(out, "DAG runtime binding");
    }

    void print_postbinding_assessment_header(std::ostream& out) {
        print_observation_stage_header(out, "[2/2] Post-binding runtime assessment");
    }

    void print_plan_contract_brief(std::ostream& out) {
        out
            << "Plan contract:\n"
            << "  - `logic.json` is a raw `PlanSpec` object.\n"
            << "  - `--dump-plan` documents `run`, `sweep`, `exec_settings`, node wiring, and binding rules.\n"
            << "  - Use `--align-dump` before authoring `ms.*` or ranked plans so you know the aligned `series_count`, keys, and missingness.\n"
            << "  - `ms.*` ops are cross-series tools; if you want same-series arithmetic such as `close - open`, use `ts.sub` instead of `ms.diff`.\n";
    }

    void print_plan_contract_dump(std::ostream& out) {
        out
            << "SugarBot Plan Contract\n"
            << "\n"
            << "`logic.json` for `sugarbot run <layout.json> <logic.json> <data.csv>` is a raw `PlanSpec` root object.\n"
            << "Use `--dump-ops` for the operator catalog and `--align-dump` to inspect the aligned data shape that the DAG will actually see.\n"
            << "\n"
            << "Authoring workflow\n"
            << "  1. `sugarbot --dump-layout` to define the ingest/layout contract.\n"
            << "  2. `sugarbot --obs-report layout.json data.csv` to confirm available axes and capabilities.\n"
            << "  3. `sugarbot --align-dump layout.json data.csv --series-key-col <col>` when multi-series shape matters.\n"
            << "  4. `sugarbot --dump-plan` to check `run`, `sweep`, and execution-schema rules.\n"
            << "  5. `sugarbot --dump-ops` to choose valid ops and signatures for this build.\n"
            << "  6. `sugarbot --plan-report layout.json logic.json data.csv` before `run`.\n"
            << "\n"
            << "Root schema\n"
            << "  - required: `bindings`\n"
            << "  - optional:\n"
            << "    - `version` (default `1`)\n"
            << "    - `name`\n"
            << "    - `required` (registry-module declarations)\n"
            << "    - `trade_mode` (default `LongShort`)\n"
            << "    - `exec_settings`\n"
            << "    - `params`\n"
            << "    - `numeric_nodes`\n"
            << "    - `bool_nodes`\n"
            << "    - `sweep`\n"
            << "    - `run`\n"
            << "\n"
            << "Bindings\n"
            << "  - `bindings.entry_long`, `exit_long`, `entry_short`, `exit_short`, `allow_trade`\n"
            << "  - each binding value is a bool-node id or an empty string\n"
            << "  - `bindings` itself is always required even if some entries are empty\n"
            << "\n"
            << "Param values\n"
            << "  - literal types: `null`, `bool`, integer, float, string\n"
            << "  - references: `{ \"ref\": \"top_level_param_name\" }`\n"
            << "  - objects other than `{ \"ref\": ... }` are rejected in param positions\n"
            << "\n"
            << "NodeSpec\n"
            << "  - `id` (required string; unique within numeric or bool node lists)\n"
            << "  - `op` (required string; must exist in `--dump-ops` for this build)\n"
            << "  - `args` (optional array of prior node ids; arity must match the op signature exactly)\n"
            << "  - `params` (optional object of literal values or `{ \"ref\": ... }`)\n"
            << "  - inline expressions are not allowed inside `args`\n"
            << "\n"
            << "Trade and execution settings\n"
            << "  - `trade_mode`: `LongOnly` or `LongShort` (default `LongShort`)\n"
            << "  - `exec_settings.initial_equity` (default `1.0`)\n"
            << "  - `exec_settings.eval_mode`: `SameBar`/`same` or `NextBar`/`next` (default `SameBar`)\n"
            << "  - `exec_settings.fill_timing`: `SameBarClose`/`close` or `NextBarOpen`/`open` (default `SameBarClose`)\n"
            << "  - `exec_settings.costs.fee_bps` and `slippage_bps` (default `0.0`)\n"
            << "  - `exec_settings.exits_first` (default `true`)\n"
            << "  - `exec_settings.allow_same_bar_reversal` (default `false`)\n"
            << "  - `exec_settings.close_out_at_end` (default `true`)\n"
            << "\n"
            << "Run dispatch (`run`)\n"
            << "  - omitted:\n"
            << "    - valid for single-series CLI runs\n"
            << "    - rejected for multi-entity CLI runs because a targeting mode is ambiguous\n"
            << "  - `run.scope` values:\n"
            << "    - `primary_series`\n"
            << "    - `per_entity_ranked`\n"
            << "  - `run.scope=primary_series`:\n"
            << "    - binds one runtime target after observation-world assessment\n"
            << "    - on multi-entity data, `run.target_entity` is required\n"
            << "    - `run.top_k`, `run.ranking_metric`, and `run.ranking_dd_weight` do not apply\n"
            << "  - `run.scope=per_entity_ranked`:\n"
            << "    - evaluates compatibility against the pre-binding observation world rather than one bound primary series\n"
            << "    - requires multi-entity input and `run.top_k`\n"
            << "    - `run.target_entity` does not apply\n"
            << "    - `run.ranking_metric` defaults to `pnl` when omitted\n"
            << "    - `run.ranking_dd_weight` is required only when `run.ranking_metric=pnl_dd`\n"
            << "  - `run.ranking_metric` values: `pnl`, `pnl_dd`\n"
            << "\n"
            << "Sweep (`sweep`)\n"
            << "  - optional object\n"
            << "  - defaults:\n"
            << "    - `enabled=false`\n"
            << "    - `top_k=10`\n"
            << "    - `max_evals=5000`\n"
            << "    - `objective=\"pnl\"`\n"
            << "    - `dd_weight=1.0`\n"
            << "    - `engine=\"exhaustive\"`\n"
            << "  - `sweep.engine` values: `exhaustive`, `coarse_fine`, `monte_carlo`\n"
            << "  - `sweep.params.<name>` forms:\n"
            << "    - discrete: `{ \"values\": [5, 10, 20], \"is_int\": true }`\n"
            << "    - stepped range: `{ \"min\": 5, \"max\": 50, \"step\": 5, \"is_int\": true }`\n"
            << "  - `sweep.engine_config.coarse_fine` fields:\n"
            << "    - `k_stage1` (default `25`)\n"
            << "    - `k_stage2` (default `50`)\n"
            << "    - `depollute_stage1` (default `true`)\n"
            << "    - `dedupe_stage2` (default `true`)\n"
            << "    - `suppression_hops` (default `1`)\n"
            << "    - `stage2_hops` (default `1`)\n"
            << "    - `stage1_stride` (default `0`)\n"
            << "  - `sweep.engine_config.monte_carlo` fields:\n"
            << "    - `draws` (default `10000`)\n"
            << "    - `seed` (default `0`, which means non-deterministic random_device seeding)\n"
            << "    - `dedupe` (default `true`)\n"
            << "    - `top_k` (default `0`, which means use `sweep.top_k`)\n"
            << "\n"
            << "Cross-series and alignment guidance\n"
            << "  - `--align-dump` is the planning tool for advanced DAGs, not just an alignment debugger.\n"
            << "  - `series_count=1` means `ms.*` ops will fail because there is only one aligned series lane.\n"
            << "  - `series_count>1` is the signal that cross-series ops and `run.scope=per_entity_ranked` may be valid.\n"
            << "  - `ms.diff`, `ms.ratio`, `ms.zscore`, and other `ms.*` ops are for comparisons across aligned series/entity lanes.\n"
            << "  - if you want arithmetic between two features from the same series, prefer same-series ops such as `ts.sub`, `ts.div`, or row-wise composition.\n"
            << "\n"
            << "Worked examples\n"
            << "  - minimal single-series plan:\n"
            << "    {\n"
            << "      \"version\": 1,\n"
            << "      \"trade_mode\": \"LongOnly\",\n"
            << "      \"numeric_nodes\": [\n"
            << "        { \"id\": \"c\", \"op\": \"core.close\" },\n"
            << "        { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
            << "      ],\n"
            << "      \"bool_nodes\": [\n"
            << "        { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
            << "        { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
            << "      ],\n"
            << "      \"bindings\": {\n"
            << "        \"entry_long\": \"entry\",\n"
            << "        \"exit_long\": \"exit\",\n"
            << "        \"entry_short\": \"\",\n"
            << "        \"exit_short\": \"\",\n"
            << "        \"allow_trade\": \"\"\n"
            << "      }\n"
            << "    }\n"
            << "  - primary-series multi-entity dispatch:\n"
            << "    {\n"
            << "      \"run\": {\n"
            << "        \"scope\": \"primary_series\",\n"
            << "        \"target_entity\": \"BBB\"\n"
            << "      }\n"
            << "    }\n"
            << "  - ranked multi-entity dispatch:\n"
            << "    {\n"
            << "      \"run\": {\n"
            << "        \"scope\": \"per_entity_ranked\",\n"
            << "        \"top_k\": 5,\n"
            << "        \"ranking_metric\": \"pnl_dd\",\n"
            << "        \"ranking_dd_weight\": 0.25\n"
            << "      }\n"
            << "    }\n"
            << "  - sweep skeleton:\n"
            << "    {\n"
            << "      \"sweep\": {\n"
            << "        \"enabled\": true,\n"
            << "        \"engine\": \"coarse_fine\",\n"
            << "        \"objective\": \"pnl\",\n"
            << "        \"top_k\": 10,\n"
            << "        \"max_evals\": 5000,\n"
            << "        \"params\": {\n"
            << "          \"fast\": { \"min\": 5, \"max\": 25, \"step\": 5, \"is_int\": true }\n"
            << "        }\n"
            << "      }\n"
            << "    }\n";
    }

} // namespace

namespace sugar::app {

    int run_help_command(std::ostream& out) {
        out
            << "SugarBot CLI\n"
            << "\n"
            << "Usage:\n"
            << "  sugarbot --help\n"
            << "  sugarbot --dump-layout\n"
            << "  sugarbot --dump-plan\n"
            << "  sugarbot --dump-ops\n"
            << "  sugarbot run <layout.json> <logic.json> <data.csv>\n"
            << "  sugarbot --obs-report <layout.json> <data.csv>\n"
            << "  sugarbot --plan-report <layout.json> <logic.json> <data.csv>\n"
            << "  sugarbot --feature-dump <layout.json> <logic.json> <data.csv>\n"
            << "  sugarbot --prep-dump <layout.json> <csv>\n"
            << "  sugarbot --align-dump <layout.json> <csv> --series-key-col <col> [--calendar union|intersection]\n"
            << "  sugarbot --run-panel <layout.json> <csv> [--primary <col>] [--topk N]\n"
            << "  sugarbot --run-contract-suite [--contract-section <key> ...]\n"
            << "  sugarbot [csv] [--data-spec layout.json] [--format auto|legacy_ohlcv_date|iso_ohlcv_time]\n"
            << "\n"
            << "Quick start:\n"
            << "  1. `sugarbot --dump-layout` explains the accepted raw/wrapped layout formats.\n"
            << "  2. `sugarbot --obs-report layout.json data.csv` validates ingestion before strategy work.\n"
            << "  3. `sugarbot --align-dump layout.json data.csv --series-key-col <col>` shows the aligned series/entity shape before you author `ms.*` or ranked plans.\n"
            << "  4. `sugarbot --dump-plan` documents `logic.json` (`PlanSpec`) root fields, `run`, `sweep`, `exec_settings`, and worked examples.\n"
            << "  5. `sugarbot --dump-ops` prints the operator capability contract for this build.\n"
            << "  6. `sugarbot --plan-report layout.json logic.json data.csv` validates the strategy without running it.\n"
            << "  7. `sugarbot run layout.json logic.json data.csv` executes the noninteractive path.\n"
            << "\n";
        print_layout_contract_brief(out);
        out
            << "\n"
            << "Alias:\n"
            << "  - `--dump-logic` is accepted as an alias for `--dump-plan`.\n"
            << "\n";
        print_plan_contract_brief(out);
        out
            << "\n"
            << "Notes:\n"
            << "  - No mode flag enters interactive mode.\n"
            << "  - `run` executes the noninteractive LayoutSpec + PlanSpec path.\n"
            << "  - `--obs-report` and `--plan-report` validate/report without executing a full run.\n"
            << "  - `--feature-dump` emits the evaluated numeric-node feature matrix as CSV.\n";
        return 0;
    }

    int run_obs_report_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (!load_layout_and_world(cmd.obs_report.layout_path, cmd.obs_report.csv_path, layout, world, load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        out << "[Info] obs-report active.\n";
        print_observation_capability_report(world, out);
        return 0;
    }

    int run_plan_report_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (!load_layout_and_world(cmd.plan_report.layout_path, cmd.plan_report.csv_path, layout, world, load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        sugar::PlanSpec spec{};
        std::string logic_err;
        if (!load_logic_for_report(cmd.plan_report.logic_path, spec, logic_err)) {
            err << "Error: " << logic_err << "\n";
            return 1;
        }

        out << "[Info] plan-report active.\n";
        const bool will_bind_runtime = !spec.run.has_value() ? !observation_world_is_multi_series(world) :
            spec.run->scope == sugar::PlanRunScope::PrimarySeries;
        print_prebinding_assessment_header(out, will_bind_runtime);
        print_observation_capability_report(world, out);

        const bool multi_series = observation_world_is_multi_series(world);
        const auto facts = sugar::make_observation_world_facts(world);

        auto print_facts_report = [&]() {
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, facts);
            print_plan_capability_report(plan_report, out);
            return plan_report;
        };

        if (!spec.run.has_value()) {
            if (multi_series) {
                const auto plan_report = print_facts_report();
                err << "Error: multi-entity plan-report requires logic.run.scope for runtime-targeted diagnostics; "
                    << valid_run_scope_values_text() << "\n";
                return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out);
            return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope == sugar::PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.ranking_metric.has_value() || run.ranking_dd_weight.has_value()) {
                const auto plan_report = print_facts_report();
                err << "Error: logic.run.top_k and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked\n";
                return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
            }

            sugar::RuntimeTargetSpec target{};
            target.require_primary_series_on_multi_series = multi_series;
            if (multi_series) {
                if (run.target_entity.empty()) {
                    const auto plan_report = print_facts_report();
                    err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    const auto plan_report = print_facts_report();
                    err << "Error: " << resolve_err << "\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                target.primary_series_key = std::move(series_key);
            }
            else if (!run.target_entity.empty()) {
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    const auto plan_report = print_facts_report();
                    err << "Error: " << resolve_err << "\n";
                    return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 1 : 1;
                }
                target.primary_series_key = std::move(series_key);
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    target,
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            out << "[Info] plan-report scope: primary_series";
            if (!runtime.primary_entity_key.empty()) {
                out << " target_entity=" << runtime.primary_entity_key;
            }
            out << "\n";
            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out);
            return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
        }

        const auto plan_report = print_facts_report();
        if (!multi_series) {
            err << "Error: logic.run.scope=per_entity_ranked requires multi-entity observation input\n";
            return 1;
        }
        if (!run.top_k.has_value()) {
            err << "Error: logic.run.top_k is required when logic.run.scope=per_entity_ranked\n";
            return 1;
        }
        if (!run.target_entity.empty()) {
            err << "Error: logic.run.target_entity does not apply when logic.run.scope=per_entity_ranked\n";
            return 1;
        }

        sugar::PanelRankingSpec ranking{};
        std::string ranking_err;
        if (!resolve_run_ranking_spec(run, ranking, ranking_err)) {
            err << "Error: " << ranking_err << "\n";
            return 1;
        }

        out << "[Info] plan-report scope: per_entity_ranked top_k=" << *run.top_k
            << " ranking_metric=" << sugar::plan_run_ranking_metric_name(
                run.ranking_metric.value_or(sugar::PlanRunRankingMetric::Pnl))
            << "\n";

        return plan_report.status == sugar::PlanCapabilityStatus::Valid ? 0 : 1;
    }

    int run_feature_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        sugar::ResolvedObservationWorld world{};
        std::string load_err;
        if (!load_layout_and_world(cmd.feature_dump.layout_path, cmd.feature_dump.csv_path, layout, world, load_err)) {
            err << "Error: " << load_err << "\n";
            return 1;
        }

        sugar::PlanSpec spec{};
        std::string logic_err;
        if (!load_logic_for_report(cmd.feature_dump.logic_path, spec, logic_err)) {
            err << "Error: " << logic_err << "\n";
            return 1;
        }

        const bool multi_series = observation_world_is_multi_series(world);

        if (!spec.run.has_value()) {
            if (multi_series) {
                err << "Error: multi-entity feature-dump requires logic.run.scope=primary_series in logic.json\n";
                return 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            std::string dump_err;
            if (!dump_feature_table(runtime, spec, out, dump_err)) {
                err << "Error: " << dump_err << "\n";
                return 1;
            }
            return 0;
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope != sugar::PlanRunScope::PrimarySeries) {
            err << "Error: feature-dump supports only single-series input or logic.run.scope=primary_series\n";
            return 1;
        }
        if (run.top_k.has_value() || run.ranking_metric.has_value() || run.ranking_dd_weight.has_value()) {
            err << "Error: feature-dump does not use logic.run.top_k or logic.run.ranking_*\n";
            return 1;
        }

        sugar::RuntimeTargetSpec target{};
        target.require_primary_series_on_multi_series = multi_series;
        if (multi_series) {
            if (run.target_entity.empty()) {
                err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                return 1;
            }
            std::string series_key;
            std::string resolve_err;
            if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                err << "Error: " << resolve_err << "\n";
                return 1;
            }
            target.primary_series_key = std::move(series_key);
        }
        else if (!run.target_entity.empty()) {
            std::string series_key;
            std::string resolve_err;
            if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                err << "Error: " << resolve_err << "\n";
                return 1;
            }
            target.primary_series_key = std::move(series_key);
        }

        sugar::ResolvedRuntimeInput runtime{};
        std::string bind_err;
        if (!sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                target,
                runtime,
                bind_err)) {
            err << "Error: " << bind_err << "\n";
            return 1;
        }

        std::string dump_err;
        if (!dump_feature_table(runtime, spec, out, dump_err)) {
            err << "Error: " << dump_err << "\n";
            return 1;
        }
        return 0;
    }

    int run_dump_layout_command(std::ostream& out) {
        print_layout_contract_dump(out);
        return 0;
    }

    int run_dump_plan_command(std::ostream& out) {
        print_plan_contract_dump(out);
        return 0;
    }

    int run_unified_cli_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::LayoutSpec layout{};
        std::string layout_err;
        if (!sugar::load_layout_spec_file(cmd.run.layout_path, layout, layout_err)) {
            err << "Error: failed to load layout spec: " << layout_err << "\n";
            return 1;
        }

        sugar::PlanSpec spec{};
        bool has_embedded_observations = false;
        std::string plan_err;
        if (!sugar::app::load_noninteractive_planspec(
                cmd.run.logic_path,
                spec,
                has_embedded_observations,
                plan_err)) {
            err << "Error: failed to load logic spec: " << plan_err << "\n";
            return 1;
        }
        if (has_embedded_observations) {
            err << "Error: logic.json embeds legacy observation inputs. That compatibility path is interactive-only.\n";
            return 1;
        }

        sugar::ResolvedObservationWorld world{};
        std::string world_err;
        if (!sugar::build_resolved_observation_world(layout, cmd.run.csv_path, world, world_err)) {
            err << "Error: " << world_err << "\n";
            return 1;
        }

        out << "[Info] noninteractive artifact run active.\n";
        const bool will_bind_runtime = !spec.run.has_value() ? !observation_world_is_multi_series(world) :
            spec.run->scope == sugar::PlanRunScope::PrimarySeries;
        print_prebinding_assessment_header(out, will_bind_runtime);
        print_observation_capability_report(world, out);

        const bool multi_series = observation_world_is_multi_series(world);
        const auto facts = sugar::make_observation_world_facts(world);

        if (!spec.run.has_value()) {
            if (multi_series) {
                err << "Error: " << missing_run_scope_hint_text() << "\n";
                return 1;
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    sugar::RuntimeTargetSpec{},
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out);
            if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
                err << "Error: logic.json is not compatible with the resolved runtime target\n";
                return 1;
            }

            const sugar::DataView dv(*runtime.aligned_bundle);
            sugar::PlanSweepRuntimeOptions rt{};
            const bool ran = sugar::run_custom_plan_spec(dv, runtime.primary_ref, spec, rt, out);
            return ran ? 0 : 1;
        }

        const sugar::PlanRunSpec& run = *spec.run;
        if (run.scope == sugar::PlanRunScope::PrimarySeries) {
            if (run.top_k.has_value() || run.ranking_metric.has_value() || run.ranking_dd_weight.has_value()) {
                err << "Error: logic.run.top_k and logic.run.ranking_* apply only to logic.run.scope=per_entity_ranked\n";
                return 1;
            }

            sugar::RuntimeTargetSpec target{};
            target.require_primary_series_on_multi_series = multi_series;
            if (multi_series) {
                if (run.target_entity.empty()) {
                    err << "Error: logic.run.target_entity is required when logic.run.scope=primary_series on multi-entity data\n";
                    return 1;
                }
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    err << "Error: " << resolve_err << "\n";
                    return 1;
                }
                target.primary_series_key = std::move(series_key);
            }
            else if (!run.target_entity.empty()) {
                std::string series_key;
                std::string resolve_err;
                if (!resolve_target_entity_series_key(world, run.target_entity, series_key, resolve_err)) {
                    err << "Error: " << resolve_err << "\n";
                    return 1;
                }
                target.primary_series_key = std::move(series_key);
            }

            sugar::ResolvedRuntimeInput runtime{};
            std::string bind_err;
            print_runtime_binding_header(out);
            if (!sugar::bind_resolved_runtime_target(
                    world,
                    layout.ingest.primary_value_col,
                    target,
                    runtime,
                    bind_err)) {
                err << "Error: " << bind_err << "\n";
                return 1;
            }

            out << "[Info] run scope: primary_series";
            if (!runtime.primary_entity_key.empty()) {
                out << " target_entity=" << runtime.primary_entity_key;
            }
            out << "\n";
            print_postbinding_assessment_header(out);
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            print_plan_capability_report(plan_report, out);
            if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
                err << "Error: logic.json is not compatible with the resolved runtime target\n";
                return 1;
            }

            const sugar::DataView dv(*runtime.aligned_bundle);
            sugar::PlanSweepRuntimeOptions rt{};
            const bool ran = sugar::run_custom_plan_spec(dv, runtime.primary_ref, spec, rt, out);
            return ran ? 0 : 1;
        }

        if (!multi_series) {
            err << "Error: logic.run.scope=per_entity_ranked requires multi-entity observation input\n";
            return 1;
        }
        if (!run.top_k.has_value()) {
            err << "Error: logic.run.top_k is required when logic.run.scope=per_entity_ranked\n";
            return 1;
        }
        if (!run.target_entity.empty()) {
            err << "Error: logic.run.target_entity does not apply when logic.run.scope=per_entity_ranked\n";
            return 1;
        }

        sugar::PanelRankingSpec ranking{};
        std::string ranking_err;
        if (!resolve_run_ranking_spec(run, ranking, ranking_err)) {
            err << "Error: " << ranking_err << "\n";
            return 1;
        }

        const auto plan_report = sugar::diagnose_plan_capabilities(spec, facts);
        print_plan_capability_report(plan_report, out);
        if (plan_report.status != sugar::PlanCapabilityStatus::Valid) {
            err << "Error: logic.json is not compatible with the resolved observation world\n";
            return 1;
        }

        sugar::ColumnId primary_col{};
        std::string primary_err;
        if (!resolve_world_primary_col_id(world, primary_col, primary_err)) {
            err << "Error: " << primary_err << "\n";
            return 1;
        }

        out << "[Info] run scope: per_entity_ranked top_k=" << *run.top_k
            << " ranking_metric=" << sugar::plan_run_ranking_metric_name(
                run.ranking_metric.value_or(sugar::PlanRunRankingMetric::Pnl))
            << "\n";

        const sugar::DataView dv(*world.aligned_bundle);
        sugar::PlanSweepRuntimeOptions rt{};
        const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            spec,
            rt,
            primary_col,
            *run.top_k,
            ranking);
        print_panel_result(pres, out);
        return pres.entities_ok > 0 ? 0 : 1;
    }

    int run_dump_ops_command(std::ostream& out) {
        const auto reg = sugar::make_default_registry();
        out << reg.describe_text(true);
        return 0;
    }

    int run_contract_suite_command(const AppCommand& cmd, std::ostream& out, std::ostream&) {
        const bool ok = sugar::run_contract_suite(out, cmd.contract_suite);
        return ok ? 0 : 1;
    }

    int run_prep_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::DataSpec ds;
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.prep_dump.spec_path, ds, load_err)) {
            err << "Error: failed to load DataSpec: " << load_err << "\n";
            return 1;
        }

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.prep_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_data_spec_summary(ds, out);
        print_preprocess_diagnostics(report, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);
        print_column_registry(*fs.schema, out);

        if (!report.produced_frameset) {
            out << "Preprocess output: NumericFrame\n";
            print_frame_summary(fs.by_entity.begin()->second, out, "  ");
            return 0;
        }

        out << "Preprocess output: FrameSet\n";
        out << "  entity_count: " << fs.by_entity.size() << "\n";

        std::vector<std::pair<std::string, std::size_t>> entities;
        entities.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            entities.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(entities.begin(), entities.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t topn = std::min<std::size_t>(10, entities.size());
        out << "  top entities by rows (up to 10):\n";
        for (std::size_t i = 0; i < topn; ++i) {
            const auto& [ekey, rc] = entities[i];
            auto it = fs.by_entity.find(ekey);
            if (it == fs.by_entity.end()) continue;
            out << "    " << ekey << ": rows=" << rc;
            if (rc > 0) {
                if (const auto mm = time_minmax(it->second)) {
                    const auto [mn, mx] = *mm;
                    out << " time=" << mn << ".." << mx;
                }
                else {
                    out << " row_domain=0.." << (rc - 1);
                }
            }
            out << "\n";
        }

        for (const auto& [ekey, frame] : fs.by_entity) {
            out << "  entity: " << ekey << "\n";
            print_frame_summary(frame, out, "    ");
        }

        return 0;
    }

    int run_align_dump_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        sugar::DataSpec ds;
        std::string load_err;
        if (!sugar::load_data_spec_file(cmd.align_dump.spec_path, ds, load_err)) {
            err << "Error: failed to load DataSpec: " << load_err << "\n";
            return 1;
        }
        ds.entity_col = cmd.align_dump.series_key_col;

        sugar::FrameSet fs{};
        sugar::PreprocessReport report{};
        std::string runtime_err;
        if (!sugar::app::preprocess_to_frameset(ds, cmd.align_dump.csv_path, fs, report, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }
        if (!report.produced_frameset) {
            err << "Error: --align-dump requires multiple series; preprocess did not produce FrameSet\n";
            return 1;
        }
        if (fs.by_entity.size() < 2) {
            err << "Error: need at least 2 series for alignment dump\n";
            return 1;
        }

        print_preprocess_diagnostics(report, out);
        print_observation_capability_report(
            sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(fs, report)),
            out);

        std::vector<std::pair<std::string, std::size_t>> ranked;
        ranked.reserve(fs.by_entity.size());
        for (const auto& kv : fs.by_entity) {
            ranked.emplace_back(kv.first, kv.second.row_count());
        }
        std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

        const std::size_t nsel = std::min<std::size_t>(5, ranked.size());
        std::vector<std::pair<std::string, sugar::NumericFrame>> inputs;
        inputs.reserve(nsel);
        for (std::size_t i = 0; i < nsel; ++i) {
            const auto& key = ranked[i].first;
            auto it = fs.by_entity.find(key);
            if (it != fs.by_entity.end()) {
                inputs.emplace_back(it->first, it->second);
            }
        }
        if (inputs.size() < 2) {
            err << "Error: failed to select at least 2 series for alignment dump\n";
            return 1;
        }

        sugar::align::AlignConfig cfg{};
        cfg.calendar = cmd.align_dump.calendar;
        cfg.missing = sugar::align::MissingPolicy::NaN;
        cfg.non_temporal_multi_series = ds.alignment_non_temporal_multi_series;
        sugar::FrameSet selected{};
        selected.schema = fs.schema;
        selected.index_schema = fs.index_schema;
        for (auto& kv : inputs) {
            selected.by_entity.emplace(kv.first, std::move(kv.second));
        }

        std::shared_ptr<sugar::align::AlignedBundle> bundle{};
        std::string align_err;
        if (!sugar::app::align_frameset(selected, cfg, bundle, align_err)) {
            err << "Error: " << align_err << "\n";
            return 1;
        }

        print_align_dump(*bundle, out);
        return 0;
    }

    int run_panel_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        const sugar::StrategySource source = sugar::prompt_strategy_source(in, out);
        if (source == sugar::StrategySource::CustomPlanSpec) {
            sugar::CustomPlanSpecRunOptions custom_opt{};
            sugar::prompt_custom_user_planspec(in, out, custom_opt);
            if (custom_opt.plan_spec_path.empty()) {
                out << "[Info] Custom strategy cancelled (no PlanSpec JSON provided).\n";
                return 0;
            }

            sugar::PlanSpec spec{};
            sugar::PlanSweepRuntimeOptions rt{};
            bool has_embedded_observations = false;
            std::string spec_err;
            if (!load_custom_planspec_bundle(custom_opt, spec, rt, has_embedded_observations, spec_err)) {
                err << "Error: failed to load PlanSpec runtime: " << spec_err << "\n";
                return 1;
            }

            bool use_embedded_observations = false;
            if (has_embedded_observations) {
                out << "[Info] PlanSpec embeds legacy observation inputs. --run-panel custom execution can use either those "
                    << "embedded observations or the CLI DataSpec/CSV input.\n";
                use_embedded_observations = sugar::prompt_yes_no(
                    in,
                    out,
                    "Use embedded legacy observations instead of --run-panel DataSpec/CSV input?",
                    true);
            }

            if (use_embedded_observations) {
                AlignedRuntimeInput runtime{};
                std::string runtime_err;
                if (!build_embedded_observation_runtime(spec, runtime, runtime_err)) {
                    err << "Error: " << runtime_err << "\n";
                    return 1;
                }
                if (!cmd.run_panel.primary_col.empty()) {
                    out << "[Info] --primary is ignored when using embedded legacy observations.\n";
                }

                out << "[Info] custom panel execution using embedded legacy observations.\n";
                print_observation_capability_report(runtime, out);
                const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
                sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
                print_plan_capability_report(plan_report, out);
                const sugar::DataView panel_dv(*runtime.aligned_bundle);
                const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
                    panel_dv,
                    spec,
                    rt,
                    runtime.primary_ref.col,
                    cmd.run_panel.topk,
                    default_panel_ranking_spec());
                print_panel_result(pres, out);
                return 0;
            }

            AlignedRuntimeInput runtime{};
            std::string runtime_err;
            if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
                err << "Error: " << runtime_err << "\n";
                return 1;
            }

            out << "[Info] custom panel execution using --run-panel DataSpec/CSV observations.\n";
            print_observation_capability_report(runtime, out);
            const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
            sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
            print_plan_capability_report(plan_report, out);
            const sugar::DataView panel_dv(*runtime.aligned_bundle);
            const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
                panel_dv,
                spec,
                rt,
                runtime.primary_ref.col,
                cmd.run_panel.topk,
                default_panel_ranking_spec());
            print_panel_result(pres, out);
            return 0;
        }

        const sugar::StrategyTemplate templ = sugar::prompt_strategy_template(in, out);
        const sugar::TradeMode mode = sugar::prompt_trade_mode(in, out, sugar::TradeMode::LongShort);

        sugar::PlanSpec spec{};
        std::string map_err;
        if (!sugar::load_template_planspec(templ, spec, map_err)) {
            err << "Error: template PlanSpec mapping failed: " << map_err << "\n";
            return 1;
        }

        AlignedRuntimeInput runtime{};
        std::string runtime_err;
        if (!build_run_panel_runtime_input(cmd.run_panel, runtime, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_observation_capability_report(runtime, out);
        const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
        sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
        print_plan_capability_report(plan_report, out);
        const sugar::DataView panel_dv(*runtime.aligned_bundle);
        sugar::PlanSweepRuntimeOptions rt{};
        rt.override_mode = true;
        rt.custom_mode = mode;
        const auto pres = sugar::run_ranked_multi_entity_custom_planspec_over_dataview(
            panel_dv,
            spec,
            rt,
            runtime.primary_ref.col,
            cmd.run_panel.topk,
            default_panel_ranking_spec());
        print_panel_result(pres, out);
        return 0;
    }

} // namespace sugar::app
