#include "contract_suite_internal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "service/artifact_store.h"
#include "service/network_client.h"
#include "service/network_server.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"
#include "service/request_validation.h"
#include "service/server_runtime.h"
#include "sweep/evaluation_report.h"

namespace sugar::contract_suite_internal {

    namespace {

        sugar::service::RuntimeConfig make_service_runtime_config(
            const std::filesystem::path& root,
            std::size_t max_sweep_candidate_count = 64,
            std::size_t worker_limit = 0) {
            sugar::service::RuntimeConfig config{};
            config.storage_root = root;
            config.quotas.max_upload_size_bytes = 64 * 1024;
            config.quotas.max_parsed_row_count = 128;
            config.quotas.max_sweep_candidate_count = max_sweep_candidate_count;
            config.quotas.max_live_sessions = 32;
            config.quotas.max_concurrent_jobs_per_session = 2;
            config.quotas.max_retained_result_bytes = 64 * 1024;
            config.quotas.max_retained_results = 64;
            config.quotas.server_worker_limit = worker_limit;
            return config;
        }

        std::string single_series_layout_json() {
            return
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

        std::string single_series_logic_json() {
            return
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"service_single_series\",\n"
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

        std::string single_series_csv_text() {
            return
                "date,close\n"
                "20240101,10\n"
                "20240102,11\n"
                "20240103,12\n";
        }

        std::string long_single_series_csv_text(std::size_t rows) {
            std::ostringstream out;
            out << "date,close\n";
            const auto start_date = std::chrono::sys_days{
                std::chrono::year{ 2024 } / std::chrono::January / 1
            };
            for (std::size_t i = 0; i < rows; ++i) {
                const auto ymd = std::chrono::year_month_day{
                    start_date + std::chrono::days{ static_cast<int>(i) }
                };
                out << static_cast<int>(ymd.year())
                    << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.month())
                    << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ymd.day())
                    << std::setfill(' ')
                    << ","
                    << (100.0 + static_cast<double>(i))
                    << "\n";
            }
            return out.str();
        }

        std::string single_series_sweep_logic_json(std::size_t max_evals) {
            std::ostringstream out;
            out
                << "{\n"
                << "  \"version\": 1,\n"
                << "  \"name\": \"service_single_series_sweep\",\n"
                << "  \"trade_mode\": \"LongOnly\",\n"
                << "  \"params\": {\n"
                << "    \"n_fast\": 2\n"
                << "  },\n"
                << "  \"numeric_nodes\": [\n"
                << "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                << "    { \"id\": \"sma\", \"op\": \"ts.sma\", \"args\": [\"c\"], \"params\": { \"n\": { \"ref\": \"n_fast\" } } },\n"
                << "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                << "  ],\n"
                << "  \"bool_nodes\": [\n"
                << "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"sma\", \"z\"] },\n"
                << "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"sma\", \"z\"] }\n"
                << "  ],\n"
                << "  \"bindings\": {\n"
                << "    \"entry_long\": \"entry\",\n"
                << "    \"exit_long\": \"exit\",\n"
                << "    \"entry_short\": \"\",\n"
                << "    \"exit_short\": \"\",\n"
                << "    \"allow_trade\": \"\"\n"
                << "  },\n"
                << "  \"sweep\": {\n"
                << "    \"enabled\": true,\n"
                << "    \"engine\": \"exhaustive\",\n"
                << "    \"top_k\": 3,\n"
                << "    \"max_evals\": " << max_evals << ",\n"
                << "    \"params\": {\n"
                << "      \"n_fast\": {\n"
                << "        \"min\": 2,\n"
                << "        \"max\": " << (1 + max_evals) << ",\n"
                << "        \"step\": 1,\n"
                << "        \"is_int\": true\n"
                << "      }\n"
                << "    }\n"
                << "  }\n"
                << "}\n";
            return out.str();
        }

        std::string single_series_dynamic_logic_json() {
            return
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"service_hosted_dynamic_run\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"route_threshold\", \"op\": \"core.const\", \"params\": { \"value\": 105.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"fallback_entry\", \"op\": \"core.bool_const\", \"params\": { \"value\": false } },\n"
                "    { \"id\": \"route_high\", \"op\": \"cmp.gte\", \"args\": [\"c\", \"route_threshold\"] },\n"
                "    { \"id\": \"selected_entry\", \"op\": \"cmp.gte\", \"args\": [\"c\", \"c\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"fallback_entry\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"execution\": {\n"
                "    \"mode\": \"declared_dynamic\",\n"
                "    \"subgraphs\": [\n"
                "      { \"id\": \"fallback\", \"nodes\": [\"fallback_entry\"] },\n"
                "      { \"id\": \"high_rows\", \"nodes\": [\"c\", \"selected_entry\"] }\n"
                "    ],\n"
                "    \"routes\": [\n"
                "      {\n"
                "        \"id\": \"row_threshold_gate\",\n"
                "        \"condition\": \"route_high\",\n"
                "        \"cases\": [\n"
                "          { \"id\": \"high_row\", \"when\": true, \"activate\": [\"high_rows\"] }\n"
                "        ],\n"
                "        \"fallback\": [\"fallback\"]\n"
                "      }\n"
                "    ],\n"
                "    \"outputs\": { \"entry_long\": \"selected_entry\" },\n"
                "    \"bounds\": {\n"
                "      \"max_routes\": 1,\n"
                "      \"max_active_subgraphs\": 1,\n"
                "      \"max_selected_outputs\": 1,\n"
                "      \"decision_sample_limit\": 16\n"
                "    }\n"
                "  }\n"
                "}\n";
        }

        std::string single_series_dynamic_sweep_logic_json() {
            return
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"service_hosted_dynamic_sweep\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"params\": { \"threshold\": 105.0 },\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"route_threshold\", \"op\": \"core.const\", \"params\": { \"value\": { \"ref\": \"threshold\" } } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"fallback_entry\", \"op\": \"core.bool_const\", \"params\": { \"value\": false } },\n"
                "    { \"id\": \"route_high\", \"op\": \"cmp.gte\", \"args\": [\"c\", \"route_threshold\"] },\n"
                "    { \"id\": \"selected_entry\", \"op\": \"cmp.gte\", \"args\": [\"c\", \"c\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"fallback_entry\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"execution\": {\n"
                "    \"mode\": \"declared_dynamic\",\n"
                "    \"subgraphs\": [\n"
                "      { \"id\": \"fallback\", \"nodes\": [\"fallback_entry\"] },\n"
                "      { \"id\": \"high_rows\", \"nodes\": [\"c\", \"selected_entry\"] }\n"
                "    ],\n"
                "    \"routes\": [\n"
                "      {\n"
                "        \"id\": \"row_threshold_gate\",\n"
                "        \"condition\": \"route_high\",\n"
                "        \"cases\": [\n"
                "          { \"id\": \"high_row\", \"when\": true, \"activate\": [\"high_rows\"] }\n"
                "        ],\n"
                "        \"fallback\": [\"fallback\"]\n"
                "      }\n"
                "    ],\n"
                "    \"outputs\": { \"entry_long\": \"selected_entry\" },\n"
                "    \"bounds\": {\n"
                "      \"max_routes\": 1,\n"
                "      \"max_active_subgraphs\": 1,\n"
                "      \"max_selected_outputs\": 1,\n"
                "      \"decision_sample_limit\": 16\n"
                "    }\n"
                "  },\n"
                "  \"sweep\": {\n"
                "    \"enabled\": true,\n"
                "    \"engine\": \"exhaustive\",\n"
                "    \"top_k\": 3,\n"
                "    \"max_evals\": 3,\n"
                "    \"objective\": \"pnl\",\n"
                "    \"params\": {\n"
                "      \"threshold\": { \"values\": [100.0, 105.0, 110.0], \"is_int\": false }\n"
                "    }\n"
                "  }\n"
                "}\n";
        }

        std::string topology_route_layout_json() {
            return
                "{\n"
                "  \"ingest\": {\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  },\n"
                "  \"topologies\": [\n"
                "    {\n"
                "      \"name\": \"f8.row.market.graph\",\n"
                "      \"bearing\": \"query_bearing\",\n"
                "      \"family\": \"relational_graph\",\n"
                "      \"domain_scope\": \"row_domain\",\n"
                "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\"],\n"
                "      \"directed\": true,\n"
                "      \"weighted\": true,\n"
                "      \"node_count\": 6,\n"
                "      \"artifact_ref\": {\n"
                "        \"stable_id\": \"f8.row.market.graph.v1\",\n"
                "        \"locator\": \"inline://contracts/service/hosted_report/topology\",\n"
                "        \"format\": \"layout_static_edges\",\n"
                "        \"digest\": \"sha256:f8-row-market-graph-v1\",\n"
                "        \"producer\": \"service_hosted_report_contract\"\n"
                "      },\n"
                "      \"edges\": [\n"
                "        { \"source\": 0, \"target\": 1, \"weight\": 1.0, \"valid_from\": 20240101, \"valid_to\": 20240104 },\n"
                "        { \"source\": 1, \"target\": 2, \"weight\": 1.5 },\n"
                "        { \"source\": 2, \"target\": 3, \"weight\": 2.0, \"valid_from\": 20240103 },\n"
                "        { \"source\": 3, \"target\": 5, \"weight\": 1.0, \"valid_to\": 20240105 },\n"
                "        { \"source\": 5, \"target\": 0, \"weight\": 0.8 }\n"
                "      ]\n"
                "    }\n"
                "  ]\n"
                "}\n";
        }

        std::string topology_route_logic_json() {
            return
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"service_hosted_topology_dynamic_run\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"degree\", \"op\": \"topology.degree\", \"params\": { \"topology\": \"f8.row.market.graph\" } },\n"
                "    { \"id\": \"dense_threshold\", \"op\": \"core.const\", \"params\": { \"value\": 0.5 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"fallback_entry\", \"op\": \"core.bool_const\", \"params\": { \"value\": false } },\n"
                "    { \"id\": \"route_dense\", \"op\": \"cmp.gt\", \"args\": [\"degree\", \"dense_threshold\"] },\n"
                "    { \"id\": \"selected_entry\", \"op\": \"cmp.gte\", \"args\": [\"c\", \"c\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"fallback_entry\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"execution\": {\n"
                "    \"mode\": \"declared_dynamic\",\n"
                "    \"subgraphs\": [\n"
                "      { \"id\": \"fallback\", \"nodes\": [\"fallback_entry\"] },\n"
                "      { \"id\": \"dense_rows\", \"nodes\": [\"selected_entry\"] }\n"
                "    ],\n"
                "    \"routes\": [\n"
                "      {\n"
                "        \"id\": \"topology_degree_gate\",\n"
                "        \"condition\": \"route_dense\",\n"
                "        \"cases\": [\n"
                "          { \"id\": \"dense_graph_row\", \"when\": true, \"activate\": [\"dense_rows\"] }\n"
                "        ],\n"
                "        \"fallback\": [\"fallback\"]\n"
                "      }\n"
                "    ],\n"
                "    \"outputs\": { \"entry_long\": \"selected_entry\" },\n"
                "    \"bounds\": {\n"
                "      \"max_routes\": 1,\n"
                "      \"max_active_subgraphs\": 1,\n"
                "      \"max_selected_outputs\": 1,\n"
                "      \"decision_sample_limit\": 8\n"
                "    }\n"
                "  }\n"
                "}\n";
        }

        std::string hosted_report_advanced_sweep_logic_json() {
            return
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"service_hosted_advanced_sweep_report\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"params\": { \"alpha\": 1, \"beta\": 1 },\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"zero\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } },\n"
                "    { \"id\": \"one\", \"op\": \"core.const\", \"params\": { \"value\": 1.0 } },\n"
                "    { \"id\": \"return_value\", \"op\": \"core.const\", \"params\": { \"value\": { \"ref\": \"alpha\" } } },\n"
                "    { \"id\": \"risk_value\", \"op\": \"core.const\", \"params\": { \"value\": { \"ref\": \"beta\" } } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"one\", \"zero\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"entry\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"sweep\": {\n"
                "    \"enabled\": true,\n"
                "    \"engine\": \"bayesian_posterior\",\n"
                "    \"top_k\": 3,\n"
                "    \"max_evals\": 9,\n"
                "    \"objective\": { \"value\": { \"source\": \"numeric_node_output\", \"name\": \"return_value\" }, \"direction\": \"maximize\" },\n"
                "    \"result_diversity\": {\n"
                "      \"dedupe_trade_hash\": false,\n"
                "      \"basin_aware\": true,\n"
                "      \"oversample_multiplier\": 4,\n"
                "      \"max_per_basin\": 1,\n"
                "      \"param_bucket_size\": 2\n"
                "    },\n"
                "    \"pareto\": {\n"
                "      \"retained_candidate_limit\": 9,\n"
                "      \"primary_sort_index\": 0,\n"
                "      \"objectives\": [\n"
                "        { \"value\": { \"source\": \"numeric_node_output\", \"name\": \"return_value\" }, \"direction\": \"maximize\" },\n"
                "        { \"value\": { \"source\": \"numeric_node_output\", \"name\": \"risk_value\" }, \"direction\": \"minimize\" }\n"
                "      ]\n"
                "    },\n"
                "    \"engine_config\": {\n"
                "      \"bayesian_posterior\": {\n"
                "        \"initial_design_size\": 5,\n"
                "        \"iterations\": 4,\n"
                "        \"seed\": 17,\n"
                "        \"top_k\": 3,\n"
                "        \"normalize_inputs\": false\n"
                "      }\n"
                "    },\n"
                "    \"params\": {\n"
                "      \"alpha\": { \"values\": [1, 2, 3], \"is_int\": true },\n"
                "      \"beta\": { \"values\": [1, 2, 3], \"is_int\": true }\n"
                "    }\n"
                "  }\n"
                "}\n";
        }

        std::string single_series_monte_carlo_sweep_logic_json(
            std::size_t max_evals,
            std::size_t draws) {
            std::ostringstream out;
            out
                << "{\n"
                << "  \"version\": 1,\n"
                << "  \"name\": \"service_single_series_monte_carlo_sweep\",\n"
                << "  \"trade_mode\": \"LongOnly\",\n"
                << "  \"params\": {\n"
                << "    \"n_fast\": 2\n"
                << "  },\n"
                << "  \"numeric_nodes\": [\n"
                << "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                << "    { \"id\": \"sma\", \"op\": \"ts.sma\", \"args\": [\"c\"], \"params\": { \"n\": { \"ref\": \"n_fast\" } } },\n"
                << "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                << "  ],\n"
                << "  \"bool_nodes\": [\n"
                << "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"sma\", \"z\"] },\n"
                << "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"sma\", \"z\"] }\n"
                << "  ],\n"
                << "  \"bindings\": {\n"
                << "    \"entry_long\": \"entry\",\n"
                << "    \"exit_long\": \"exit\",\n"
                << "    \"entry_short\": \"\",\n"
                << "    \"exit_short\": \"\",\n"
                << "    \"allow_trade\": \"\"\n"
                << "  },\n"
                << "  \"sweep\": {\n"
                << "    \"enabled\": true,\n"
                << "    \"engine\": \"monte_carlo\",\n"
                << "    \"top_k\": 3,\n"
                << "    \"max_evals\": " << max_evals << ",\n"
                << "    \"params\": {\n"
                << "      \"n_fast\": {\n"
                << "        \"min\": 2,\n"
                << "        \"max\": 11,\n"
                << "        \"step\": 1,\n"
                << "        \"is_int\": true\n"
                << "      }\n"
                << "    },\n"
                << "    \"engine_config\": {\n"
                << "      \"monte_carlo\": {\n"
                << "        \"draws\": " << draws << ",\n"
                << "        \"seed\": 12345,\n"
                << "        \"dedupe\": true\n"
                << "      }\n"
                << "    }\n"
                << "  }\n"
                << "}\n";
            return out.str();
        }

        std::string multi_entity_layout_json() {
            return
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
                "  }\n"
                "}\n";
        }

        std::string multi_entity_csv_text() {
            return
                "symbol,time,close\n"
                "AAA,2024-01-01T00:00:00Z,100.0\n"
                "BBB,2024-01-01T00:00:00Z,90.0\n"
                "AAA,2024-01-02T00:00:00Z,110.0\n"
                "BBB,2024-01-02T00:00:00Z,80.0\n";
        }

        sugar::service::SubmitRequest make_single_series_request(sugar::service::JobIntent intent) {
            sugar::service::SubmitRequest request{};
            request.intent = intent;
            request.layout.client_name = "layout.json";
            request.layout.content = single_series_layout_json();
            request.logic.client_name = "logic.json";
            request.logic.content = single_series_logic_json();
            request.data.client_name = "data.csv";
            request.data.content = single_series_csv_text();
            return request;
        }

        sugar::service::SubmitRequest make_multi_entity_missing_run_request() {
            sugar::service::SubmitRequest request{};
            request.intent = sugar::service::JobIntent::ValidateOnly;
            request.layout.client_name = "layout.json";
            request.layout.content = multi_entity_layout_json();
            request.logic.client_name = "logic.json";
            request.logic.content = single_series_logic_json();
            request.data.client_name = "data.csv";
            request.data.content = multi_entity_csv_text();
            return request;
        }

        sugar::service::SubmitRequest make_hosted_report_request(
            std::string layout_json,
            std::string logic_json,
            std::string data_csv) {
            sugar::service::SubmitRequest request{};
            request.intent = sugar::service::JobIntent::Execute;
            request.layout.client_name = "layout.json";
            request.layout.content = std::move(layout_json);
            request.logic.client_name = "logic.json";
            request.logic.content = std::move(logic_json);
            request.data.client_name = "data.csv";
            request.data.content = std::move(data_csv);
            return request;
        }

        bool contains_all(
            const std::string& text,
            std::initializer_list<std::string_view> needles) {
            return std::all_of(
                needles.begin(),
                needles.end(),
                [&](std::string_view needle) {
                    return text.find(needle) != std::string::npos;
                });
        }

        bool expect_contains_all(
            ContractAsserts& t,
            const std::string& text,
            std::initializer_list<std::string_view> needles,
            const std::string& label) {
            const bool ok = contains_all(text, needles);
            if (!ok) {
                for (std::string_view needle : needles) {
                    if (text.find(needle) == std::string::npos) {
                        t.out << "  missing fragment: " << needle << "\n";
                    }
                }
            }
            return t.expect(ok, label);
        }

        bool run_hosted_report_job_json(
            ContractAsserts& t,
            const std::string& case_name,
            const sugar::service::SubmitRequest& request,
            std::string& out_json,
            sugar::service::JobResult* out_result = nullptr) {
            sugar::service::ServerRuntime runtime(make_service_runtime_config(
                contract_tmp_case_dir(case_name),
                64));
            sugar::service::SessionInfo session{};
            std::string err;
            if (!t.expect(runtime.create_session(session, err),
                    case_name + " creates hosted service session")) {
                t.out << "  session error: " << err << "\n";
                return false;
            }

            std::string job_id;
            if (!t.expect(runtime.submit_job(session.session_id, request, job_id, err),
                    case_name + " submits hosted request")) {
                t.out << "  submit error: " << err << "\n";
                return false;
            }

            err.clear();
            if (!t.expect(runtime.process_job(job_id, err),
                    case_name + " processes hosted request")) {
                t.out << "  process error: " << err << "\n";
                return false;
            }

            sugar::service::JobResult result{};
            err.clear();
            if (!t.expect(runtime.get_job_result(session.session_id, job_id, result, err),
                    case_name + " retrieves hosted result")) {
                t.out << "  result error: " << err << "\n";
                return false;
            }
            if (!t.expect(result.metadata.report.has_value(),
                    case_name + " retains unified evaluation report")) {
                return false;
            }

            sugar::service::ProtocolResponse response{};
            response.command = sugar::service::ProtocolCommand::RetrieveResults;
            response.ok = true;
            response.message = "job result retrieved";
            response.job_id = job_id;
            response.job_result = result;
            out_json = sugar::service::serialize_protocol_response_json(response);
            if (out_result) {
                *out_result = std::move(result);
            }
            return true;
        }

    } // namespace

    bool test_service_invalid_request_rejection_contracts(ContractAsserts& t) {
        t.out << "[service_invalid_request_rejection] service-core invalid request rejection\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_invalid_request_rejection")));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for invalid-request test");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.logic.content.clear();
            std::string job_id;
            const bool ok = runtime.submit_job(session.session_id, request, job_id, err);
            t.expect(!ok, "service submit rejects empty logic artifact");
            t.expect(!ok && err.find("logic artifact content is empty") != std::string::npos,
                "service submit exposes explicit empty-artifact error");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.layout.client_name = "..\\layout.json";
            std::string job_id;
            err.clear();
            const bool ok = runtime.submit_job(session.session_id, request, job_id, err);
            t.expect(!ok, "service submit rejects layout client-name traversal");
            t.expect(!ok && err.find("single filename") != std::string::npos,
                "service submit exposes explicit client-name traversal error");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.data.client_name = "data.txt";
            std::string job_id;
            err.clear();
            const bool ok = runtime.submit_job(session.session_id, request, job_id, err);
            t.expect(!ok, "service submit rejects wrong data artifact extension");
            t.expect(!ok && err.find(".csv") != std::string::npos,
                "service submit exposes explicit data-extension error");
        }

        {
            const auto request = make_multi_entity_missing_run_request();
            std::string job_id;
            const bool submitted = runtime.submit_job(session.session_id, request, job_id, err);
            t.expect(submitted, "service submit accepts shape-valid multi-entity request before validation");

            err.clear();
            const bool processed = runtime.process_job(job_id, err);
            t.expect(!processed, "service processing rejects multi-entity request with missing run scope");

            sugar::service::JobStatus status{};
            const bool has_status = runtime.get_job_status(session.session_id, job_id, status, err);
            t.expect(has_status, "service status remains queryable after rejection");
            t.expect(has_status && status.state == sugar::service::JobState::Rejected,
                "service rejection persists rejected job state");

            sugar::service::JobResult result{};
            const bool has_result = runtime.get_job_result(session.session_id, job_id, result, err);
            t.expect(has_result, "service rejection stores owned validation result");
            t.expect(has_result && result.summary_text.find("Validation rejected") != std::string::npos,
                "service rejection result preserves rejection summary text");
        }

        return true;
    }

    bool test_service_result_ownership_contracts(ContractAsserts& t) {
        t.out << "[service_result_ownership] service-core result ownership\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_result_ownership")));
        sugar::service::SessionInfo owner{};
        sugar::service::SessionInfo intruder{};
        std::string err;
        if (!runtime.create_session(owner, err) || !runtime.create_session(intruder, err)) {
            return t.expect(false, "service runtime creates sessions for ownership test");
        }

        std::string job_id;
        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        const bool submitted = runtime.submit_job(owner.session_id, request, job_id, err);
        t.expect(submitted, "service submit accepts owned execute request");

        err.clear();
        const bool processed = runtime.process_job(job_id, err);
        t.expect(processed, "service processes owned execute request");

        sugar::service::JobResult owned_result{};
        const bool owner_can_read = runtime.get_job_result(owner.session_id, job_id, owned_result, err);
        t.expect(owner_can_read, "owning session can fetch job result");
        t.expect(owner_can_read && owned_result.metadata.primary_backtest.has_value(),
            "owned result preserves primary backtest metrics");
        t.expect(owner_can_read && owned_result.input_artifacts.size() == 3,
            "owned result preserves input artifact audit trace");
        t.expect(owner_can_read && !owned_result.output_artifacts.empty(),
            "owned result preserves summary output artifact");

        err.clear();
        const bool cancel_completed = runtime.cancel_job(owner.session_id, job_id, err);
        t.expect(cancel_completed,
            "service treats cancellation of a completed owned job as a terminal no-op success");

        sugar::service::JobStatus completed_cancel_status{};
        err.clear();
        const bool has_completed_cancel_status =
            runtime.get_job_status(owner.session_id, job_id, completed_cancel_status, err);
        t.expect(has_completed_cancel_status &&
                completed_cancel_status.state == sugar::service::JobState::Completed &&
                completed_cancel_status.result_id.has_value() &&
                *completed_cancel_status.result_id == owned_result.result_id,
            "service completed-job cancel no-op preserves completed status and result id");

        sugar::service::JobResult result_after_completed_cancel{};
        err.clear();
        const bool owner_can_read_after_cancel =
            runtime.get_job_result(owner.session_id, job_id, result_after_completed_cancel, err);
        t.expect(owner_can_read_after_cancel &&
                result_after_completed_cancel.result_id == owned_result.result_id,
            "service completed-job cancel no-op leaves retained result retrievable");

        sugar::service::JobResult intruder_result{};
        err.clear();
        const bool intruder_can_read = runtime.get_job_result(intruder.session_id, job_id, intruder_result, err);
        t.expect(!intruder_can_read, "non-owning session cannot fetch another session result");
        t.expect(!intruder_can_read && err.find("does not belong") != std::string::npos,
            "non-owning result fetch fails with explicit ownership error");

        auto sweep_request = make_single_series_request(sugar::service::JobIntent::Execute);
        sweep_request.logic.content = single_series_monte_carlo_sweep_logic_json(7, 3);
        std::string sweep_job_id;
        err.clear();
        const bool sweep_submitted = runtime.submit_job(owner.session_id, sweep_request, sweep_job_id, err);
        t.expect(sweep_submitted, "service submit accepts monte_carlo sweep with explicit draw count below max_evals");

        err.clear();
        const bool sweep_processed = runtime.process_job(sweep_job_id, err);
        t.expect(sweep_processed, "service processes monte_carlo sweep result-metadata regression job");

        sugar::service::JobResult sweep_result{};
        err.clear();
        const bool sweep_owner_can_read = runtime.get_job_result(owner.session_id, sweep_job_id, sweep_result, err);
        t.expect(sweep_owner_can_read && sweep_result.metadata.sweep_report.has_value(),
            "owned monte_carlo sweep result preserves the structured sweep report");
        if (sweep_owner_can_read && sweep_result.metadata.sweep_report.has_value()) {
            const auto& report = *sweep_result.metadata.sweep_report;
            t.expect(report.evaluations_run == 3,
                "monte_carlo hosted sweep report preserves the actual evaluation count");
            t.expect(sweep_result.metadata.validation.sweep_candidate_count == report.evaluations_run,
                "hosted result validation metadata reports actual sweep evaluations, not max_evals");
            t.expect(sweep_result.summary_text.find("sweep_candidates: 3") != std::string::npos,
                "hosted result summary displays the actual sweep evaluation count");
        }

        return true;
    }

    bool test_service_hosted_unified_report_contracts(ContractAsserts& t) {
        t.out << "[service_hosted_unified_report] hosted v1 unified evaluation report promotion\n";

        {
            const auto request = make_hosted_report_request(
                single_series_layout_json(),
                single_series_dynamic_logic_json(),
                long_single_series_csv_text(6));
            std::string json;
            sugar::service::JobResult result{};
            const bool ok = run_hosted_report_job_json(
                t,
                "service_hosted_dynamic_run_report",
                request,
                json,
                &result);
            t.expect(ok &&
                    result.metadata.validation.resolved_mode ==
                        sugar::service::ResolvedExecutionMode::PrimarySeriesRun,
                "hosted dynamic run executes as a primary-series run");
            ok && expect_contains_all(t, json, {
                    "\"schema_version\":\"sugarbot.hosted.v1\"",
                    "\"report\":{\"schema_version\":\"sugarbot.evaluation_report.v1\",\"kind\":\"run\"",
                    "\"dynamic_execution\":{\"enabled\":true,\"mode\":\"declared_dynamic\"",
                    "\"route_decision_digest\":",
                    "\"decision_samples\":["
                },
                "hosted run metadata.report promotes dynamic execution diagnostics");
        }

        {
            const auto request = make_hosted_report_request(
                single_series_layout_json(),
                single_series_dynamic_sweep_logic_json(),
                long_single_series_csv_text(6));
            std::string json;
            sugar::service::JobResult result{};
            const bool ok = run_hosted_report_job_json(
                t,
                "service_hosted_dynamic_sweep_report",
                request,
                json,
                &result);
            t.expect(ok &&
                    result.metadata.validation.resolved_mode ==
                        sugar::service::ResolvedExecutionMode::PrimarySeriesSweep,
                "hosted dynamic sweep executes as a primary-series sweep");
            ok && expect_contains_all(t, json, {
                    "\"report\":{\"schema_version\":\"sugarbot.evaluation_report.v1\",\"kind\":\"sweep\"",
                    "\"proposal_trace\":{\"captured\":true",
                    "\"exact_replay_from_report\":",
                    "\"candidates\":[",
                    "\"dynamic_execution\":{\"enabled\":true,\"mode\":\"declared_dynamic\"",
                    "\"route_decision_digest\":"
                },
                "hosted sweep metadata.report promotes replay trace and per-candidate dynamic diagnostics");
        }

        {
            const auto request = make_hosted_report_request(
                topology_route_layout_json(),
                topology_route_logic_json(),
                long_single_series_csv_text(6));
            std::string json;
            const bool ok = run_hosted_report_job_json(
                t,
                "service_hosted_topology_dynamic_report",
                request,
                json);
            ok && expect_contains_all(t, json, {
                    "\"report\":{\"schema_version\":\"sugarbot.evaluation_report.v1\",\"kind\":\"run\"",
                    "\"dynamic_execution\":{\"enabled\":true,\"mode\":\"declared_dynamic\"",
                    "\"topology_names\":[",
                    "\"f8.row.market.graph\"",
                    "\"topology_usage_count\":1",
                    "\"topologies_reported_count\":",
                    "\"topology_usages_reported_count\":1",
                    "\"topology_name\":\"f8.row.market.graph\"",
                    "\"stable_id\":\"f8.row.market.graph.v1\""
                },
                "hosted run metadata.report promotes topology provenance and usage facts");
        }

        {
            const auto request = make_hosted_report_request(
                single_series_layout_json(),
                hosted_report_advanced_sweep_logic_json(),
                long_single_series_csv_text(8));
            std::string json;
            sugar::service::JobResult result{};
            const bool ok = run_hosted_report_job_json(
                t,
                "service_hosted_advanced_sweep_report",
                request,
                json,
                &result);
            t.expect(ok &&
                    result.metadata.validation.resolved_mode ==
                        sugar::service::ResolvedExecutionMode::PrimarySeriesSweep,
                "hosted advanced sweep executes as a primary-series sweep");
            ok && expect_contains_all(t, json, {
                    "\"engine\":\"bayesian_posterior\"",
                    "\"proposal_trace\":{\"captured\":true,\"policy_name\":\"bayesian_posterior\"",
                    "\"search_geometry\":{",
                    "\"basins\":[",
                    "\"pareto\":{\"requested\":true",
                    "\"source\":",
                    "\"objectives\":[",
                    "\"posterior\":{\"requested\":true,\"computed\":true",
                    "\"training_observation_count\":",
                    "\"proposal_reasons\":[",
                    "\"result_diversity\":{\"requested\":true"
                },
                "hosted sweep metadata.report promotes posterior, Pareto, search geometry, trace, and diversity blocks");
        }

        return true;
    }

    bool test_service_cancellation_contracts(ContractAsserts& t) {
        t.out << "[service_cancellation] service-core queued cancellation state transitions\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_cancellation")));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for cancellation test");
        }

        std::string job_id;
        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        const bool submitted = runtime.submit_job(session.session_id, request, job_id, err);
        t.expect(submitted, "service submit queues cancelable job");

        sugar::service::JobStatus queued_status{};
        const bool has_queued_status = runtime.get_job_status(session.session_id, job_id, queued_status, err);
        t.expect(has_queued_status && queued_status.state == sugar::service::JobState::Queued,
            "service status reports queued state before cancellation");
        const bool has_job_queued_event =
            has_queued_status &&
            std::any_of(queued_status.progress.begin(), queued_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::Queued &&
                        event.message == "job queued";
                });
        t.expect(has_job_queued_event,
            "service progress preserves explicit job queued transition");

        err.clear();
        const bool canceled = runtime.cancel_job(session.session_id, job_id, err);
        t.expect(canceled, "service can cancel queued job");

        sugar::service::JobStatus canceled_status{};
        const bool has_canceled_status = runtime.get_job_status(session.session_id, job_id, canceled_status, err);
        t.expect(has_canceled_status && canceled_status.state == sugar::service::JobState::Canceled,
            "service queued cancellation reaches canceled state");
        const bool has_cancel_requested_event =
            has_canceled_status &&
            std::any_of(canceled_status.progress.begin(), canceled_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::CancelRequested;
                });
        t.expect(has_cancel_requested_event,
            "service queued cancellation preserves cancel_requested transition in progress log");

        sugar::service::ProtocolResponse canceled_status_response{};
        canceled_status_response.command = sugar::service::ProtocolCommand::CheckStatus;
        canceled_status_response.ok = true;
        canceled_status_response.message = "status";
        canceled_status_response.session_id = session.session_id;
        canceled_status_response.job_id = job_id;
        canceled_status_response.job_status = canceled_status;
        const std::string canceled_status_json =
            sugar::service::serialize_protocol_response_json(canceled_status_response);
        t.expect(canceled_status_json.find("\"progress_summary\":") != std::string::npos &&
                canceled_status_json.find("\"cancel_requested\":true") != std::string::npos,
            "service canceled status serializes compact progress feedback");
        t.expect(
            canceled_status_json.find(
                "\"retention\":{\"result_retained\":false,\"report_status\":\"not_retained\","
                "\"cleanup_eligible\":true,\"no_result_reason\":\"canceled\"}") != std::string::npos,
            "service canceled status serializes no-result retention and cleanup facts");

        const std::size_t canceled_progress_events = canceled_status.progress.size();
        err.clear();
        const bool repeated_cancel = runtime.cancel_job(session.session_id, job_id, err);
        t.expect(repeated_cancel,
            "service treats repeated cancellation of a canceled owned job as an idempotent no-op");

        sugar::service::JobStatus repeated_cancel_status{};
        err.clear();
        const bool has_repeated_cancel_status =
            runtime.get_job_status(session.session_id, job_id, repeated_cancel_status, err);
        t.expect(has_repeated_cancel_status &&
                repeated_cancel_status.state == sugar::service::JobState::Canceled &&
                repeated_cancel_status.progress.size() == canceled_progress_events,
            "service repeated cancel preserves canceled state without adding progress noise");

        err.clear();
        const bool reprocessed = runtime.process_job(job_id, err);
        t.expect(reprocessed, "service ignores canceled job processing cleanly");

        sugar::service::JobResult canceled_result{};
        err.clear();
        const bool has_result = runtime.get_job_result(session.session_id, job_id, canceled_result, err);
        t.expect(!has_result, "service does not fabricate result payload for canceled queued job");

        const std::filesystem::path canceled_workspace =
            has_canceled_status && !canceled_status.staged_artifacts.empty()
                ? canceled_status.staged_artifacts.front().storage_path.parent_path()
                : std::filesystem::path{};
        t.expect(!canceled_workspace.empty() && std::filesystem::exists(canceled_workspace),
            "service canceled job still exposes its staged workspace before explicit cleanup");

        err.clear();
        const bool cleaned = runtime.cleanup_job(session.session_id, job_id, err);
        t.expect(cleaned, "service cleanup removes canceled terminal job state");
        t.expect(cleaned && !std::filesystem::exists(canceled_workspace),
            "service cleanup removes canceled job artifact workspace");

        sugar::service::JobStatus status_after_cleanup{};
        err.clear();
        const bool has_status_after_cleanup =
            runtime.get_job_status(session.session_id, job_id, status_after_cleanup, err);
        t.expect(!has_status_after_cleanup, "service cleanup removes canceled job status records");
        return true;
    }

    bool test_service_running_cancellation_contracts(ContractAsserts& t) {
        t.out << "[service_running_cancellation] service-core cooperative running-job cancellation\n";

        constexpr std::size_t kSweepEvals = 100000;
        constexpr std::size_t kRowCount = 1024;
        sugar::service::RuntimeConfig config = make_service_runtime_config(
            contract_tmp_case_dir("service_running_cancellation"),
            kSweepEvals);
        // Release builds can complete the 128-row sweep before the cancel request lands once
        // the broader service_core group has already warmed the process.
        config.quotas.max_parsed_row_count = kRowCount;
        sugar::service::ServerRuntime runtime(std::move(config));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for running cancellation test");
        }

        auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        request.logic.content = single_series_sweep_logic_json(kSweepEvals);
        request.data.content = long_single_series_csv_text(kRowCount);

        std::string job_id;
        const bool submitted = runtime.submit_job(session.session_id, request, job_id, err);
        t.expect(submitted, "service submit accepts long-running sweep job");
        if (!submitted) {
            return true;
        }

        bool processed = false;
        std::string process_err;
        std::thread worker([&] {
            processed = runtime.process_job(job_id, process_err);
        });

        const auto has_runtime_execution_progress = [](const sugar::service::JobStatus& status) {
            return std::any_of(status.progress.begin(), status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::Running &&
                        event.completed > 0 &&
                        event.total > 0 &&
                        event.message.find("execution progress") != std::string::npos;
                });
        };

        bool saw_running = false;
        sugar::service::JobStatus running_status{};
        bool canceled = false;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            err.clear();
            if (runtime.get_job_status(session.session_id, job_id, running_status, err)) {
                saw_running = has_runtime_execution_progress(running_status);
                if (saw_running) {
                    err.clear();
                    canceled = runtime.cancel_job(session.session_id, job_id, err);
                    break;
                }
                if (running_status.state == sugar::service::JobState::Completed ||
                    running_status.state == sugar::service::JobState::Failed ||
                    running_status.state == sugar::service::JobState::Rejected ||
                    running_status.state == sugar::service::JobState::Canceled) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t.expect(saw_running, "service status reaches active execution before cooperative cancellation");
        t.expect(canceled, "service accepts cancellation for running job");

        worker.join();

        t.expect(processed,
            "service processing exits cleanly after running-job cancellation" +
            (processed || process_err.empty() ? std::string{} : ": " + process_err));

        sugar::service::JobStatus final_status{};
        const bool has_final_status = runtime.get_job_status(session.session_id, job_id, final_status, err);
        t.expect(has_final_status, "service status remains available after running-job cancellation");
        t.expect(has_final_status && final_status.state == sugar::service::JobState::Canceled,
            "service running cancellation reaches canceled terminal state");
        const bool has_cancel_requested_event =
            has_final_status &&
            std::any_of(final_status.progress.begin(), final_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::CancelRequested;
                });
        t.expect(has_cancel_requested_event,
            "service running cancellation preserves cancel_requested transition in progress log");
        const bool has_execution_progress =
            has_final_status &&
            has_runtime_execution_progress(final_status);
        t.expect(has_execution_progress,
            "service running cancellation exposes execution progress before cancellation");
        sugar::service::ProtocolResponse final_status_response{};
        final_status_response.command = sugar::service::ProtocolCommand::CheckStatus;
        final_status_response.ok = true;
        final_status_response.message = "status";
        final_status_response.session_id = session.session_id;
        final_status_response.job_id = job_id;
        final_status_response.job_status = final_status;
        const std::string final_status_json =
            sugar::service::serialize_protocol_response_json(final_status_response);
        t.expect(final_status_json.find("\"latest_work_total\":100000") != std::string::npos &&
                final_status_json.find("\"work_event_count\":") != std::string::npos &&
                final_status_json.find("\"cancel_requested\":true") != std::string::npos,
            "service running cancellation keeps compact work-progress facts after the terminal cancel event");
        t.expect(
            final_status_json.find(
                "\"retention\":{\"result_retained\":false,\"report_status\":\"not_retained\","
                "\"cleanup_eligible\":true,\"no_result_reason\":\"canceled\"}") != std::string::npos,
            "service running cancellation serializes no-result retention facts");

        sugar::service::JobResult canceled_result{};
        err.clear();
        const bool has_result = runtime.get_job_result(session.session_id, job_id, canceled_result, err);
        t.expect(!has_result, "service does not retain result payload for cooperatively canceled running job");

        return true;
    }

    bool test_service_artifact_sandbox_cleanup_contracts(ContractAsserts& t) {
        t.out << "[service_artifact_sandbox_cleanup] service-core artifact sandboxing and cleanup\n";

        const auto root = contract_tmp_case_dir("service_artifact_cleanup");
        sugar::service::ArtifactStore store(root / "artifact_store");
        std::string err;
        t.expect(store.initialize(err), "artifact store initializes under sandbox root");

        std::filesystem::path workspace{};
        t.expect(store.create_job_workspace("job-escape", workspace, err),
            "artifact store creates job workspace");

        std::filesystem::path resolved{};
        const bool relative_escape = store.resolve_job_relative_path("job-escape", "../escape.txt", resolved, err);
        t.expect(!relative_escape, "artifact store rejects parent-path escape");
        const bool absolute_escape = store.resolve_job_relative_path(
            "job-escape",
            (root / "outside.txt").string(),
            resolved,
            err);
        t.expect(!absolute_escape, "artifact store rejects absolute client-controlled path");

#ifndef _WIN32
        {
            const auto outside_root = root / "outside_root";
            const auto symlink_workspace = root / "artifact_store" / "jobs" / "job-symlink";
            std::error_code ec;
            std::filesystem::create_directories(outside_root, ec);
            std::filesystem::create_directories(symlink_workspace.parent_path(), ec);
            std::filesystem::create_directory_symlink(outside_root, symlink_workspace, ec);

            std::filesystem::path symlink_out{};
            err.clear();
            const bool created = store.create_job_workspace("job-symlink", symlink_out, err);
            t.expect(!created, "artifact store rejects a symlinked job workspace that escapes the storage root");
            t.expect(!created && err.find("escapes") != std::string::npos,
                "artifact store reports explicit escape detail for symlinked workspaces");

            std::filesystem::remove(symlink_workspace, ec);
        }
#endif

        sugar::service::ServerRuntime runtime(make_service_runtime_config(root / "runtime"));
        sugar::service::SessionInfo session{};
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for cleanup test");
        }

        std::string job_id;
        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        const bool submitted = runtime.submit_job(session.session_id, request, job_id, err);
        t.expect(submitted, "service submit accepts cleanup test request");

        err.clear();
        const bool processed = runtime.process_job(job_id, err);
        t.expect(processed, "service processes cleanup test request");

        sugar::service::JobStatus status{};
        const bool has_status = runtime.get_job_status(session.session_id, job_id, status, err);
        t.expect(has_status && !status.staged_artifacts.empty(),
            "service status exposes staged artifacts before cleanup");

        const std::filesystem::path job_workspace =
            has_status ? status.staged_artifacts.front().storage_path.parent_path() : std::filesystem::path{};
        t.expect(has_status && std::filesystem::exists(job_workspace),
            "service staged artifacts live under a concrete job workspace before cleanup");

        err.clear();
        const bool cleaned = runtime.cleanup_job(session.session_id, job_id, err);
        t.expect(cleaned, "service cleanup removes terminal job state");
        t.expect(cleaned && !std::filesystem::exists(job_workspace),
            "service cleanup removes per-job artifact workspace");

        sugar::service::JobResult cleaned_result{};
        err.clear();
        const bool has_result_after_cleanup = runtime.get_job_result(session.session_id, job_id, cleaned_result, err);
        t.expect(!has_result_after_cleanup, "service cleanup removes retained job result");

        return true;
    }

    bool test_service_local_request_replay_contracts(ContractAsserts& t) {
        t.out << "[service_local_request_replay] service-core deterministic local request replay\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_local_request_replay")));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for replay test");
        }

        std::string first_job_id;
        std::string second_job_id;
        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        const bool first_submitted = runtime.submit_job(session.session_id, request, first_job_id, err);
        const bool second_submitted = runtime.submit_job(session.session_id, request, second_job_id, err);
        t.expect(first_submitted && second_submitted, "service accepts repeated local replay submissions");

        err.clear();
        const bool first_processed = runtime.process_job(first_job_id, err);
        err.clear();
        const bool second_processed = runtime.process_job(second_job_id, err);
        t.expect(first_processed && second_processed, "service processes repeated local replay submissions deterministically");

        sugar::service::JobResult first_result{};
        sugar::service::JobResult second_result{};
        const bool has_first = runtime.get_job_result(session.session_id, first_job_id, first_result, err);
        const bool has_second = runtime.get_job_result(session.session_id, second_job_id, second_result, err);
        t.expect(has_first && has_second, "service replay results remain available for comparison");

        const bool same_backtest =
            has_first &&
            has_second &&
            first_result.metadata.primary_backtest.has_value() &&
            second_result.metadata.primary_backtest.has_value() &&
            backtest_equal(
                *first_result.metadata.primary_backtest,
                *second_result.metadata.primary_backtest);
        t.expect(same_backtest, "service replay preserves deterministic backtest metrics");
        t.expect(has_first && has_second && first_result.summary_text == second_result.summary_text,
            "service replay preserves deterministic summary output");

        return true;
    }

    bool test_service_protocol_framing_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_framing] service protocol framing and request parsing\n";

        {
            char exe[] = "sugar_client";
            char arg[] = "--help";
            char* argv[] = { exe, arg };
            std::ostringstream out;
            std::ostringstream err;
            const int rc = sugar::service::run_tcp_client_from_cli(2, argv, out, err);
            t.expect(rc == 0, "sugar_client --help returns success");
            t.expect(err.str().empty(), "sugar_client --help does not write errors");
            t.expect(out.str().find("sugar_client upload") != std::string::npos,
                "sugar_client --help describes upload usage");
            t.expect(out.str().find("sugar_client run --layout") != std::string::npos,
                "sugar_client --help describes one-command hosted run usage");
            t.expect(
                out.str().find("--compact") != std::string::npos &&
                    out.str().find("--raw-json") != std::string::npos,
                "sugar_client --help describes compact reports and raw JSON output");
        }

        {
            char exe[] = "sugar_client";
            char cmd[] = "run";
            char logic_flag[] = "--logic";
            char logic_path[] = "logic.json";
            char data_flag[] = "--data";
            char data_path[] = "data.csv";
            char* argv[] = { exe, cmd, logic_flag, logic_path, data_flag, data_path };
            std::ostringstream out;
            std::ostringstream err;
            const int rc = sugar::service::run_tcp_client_from_cli(6, argv, out, err);
            t.expect(rc == 1, "sugar_client run rejects missing layout path before network access");
            t.expect(out.str().empty(), "sugar_client run missing-argument error does not print JSON");
            t.expect(err.str().find("--layout") != std::string::npos,
                "sugar_client run missing-argument error identifies the required layout flag");
        }

        {
            char exe[] = "sugar_server";
            char arg[] = "--help";
            char* argv[] = { exe, arg };
            std::ostringstream out;
            std::ostringstream err;
            const int rc = sugar::service::run_tcp_server_from_cli(2, argv, out, err);
            t.expect(rc == 0, "sugar_server --help returns success");
            t.expect(err.str().empty(), "sugar_server --help does not write errors");
            t.expect(out.str().find("--service-profile") != std::string::npos,
                "sugar_server --help describes service profile selection");
            t.expect(out.str().find("--storage-root") != std::string::npos,
                "sugar_server --help describes hosted storage configuration");
        }

        sugar::service::ProtocolRequest request{};
        request.command = sugar::service::ProtocolCommand::UploadArtifact;
        request.session_id = "sess-000001";
        request.upload_kind = sugar::service::ArtifactKind::LayoutSpec;
        request.upload_client_name = "layout.json";
        request.upload_content = "{\n  \"version\": 1\n}\n";

        const std::string request_json = sugar::service::serialize_protocol_request_json(request);
        sugar::service::ProtocolRequest parsed{};
        std::string err;
        const bool parsed_ok = sugar::service::parse_protocol_request_json(request_json, parsed, err);
        t.expect(parsed_ok, "protocol framing parses serialized upload request JSON");
        t.expect(parsed_ok && parsed.command == sugar::service::ProtocolCommand::UploadArtifact,
            "protocol framing preserves serialized upload command");
        t.expect(parsed_ok && parsed.upload_client_name == request.upload_client_name,
            "protocol framing preserves serialized upload artifact name");
        t.expect(parsed_ok && parsed.upload_content == request.upload_content,
            "protocol framing preserves serialized upload artifact content");

        sugar::service::ProtocolResponse response{};
        response.command = sugar::service::ProtocolCommand::Connect;
        response.ok = true;
        response.message = "connected";
        response.session_id = "sess-000001";
        const std::string response_json = sugar::service::serialize_protocol_response_json(response);
        const std::string expected_schema_fragment =
            std::string("\"schema_version\":\"") +
            std::string(sugar::service::hosted_observability_schema_version()) +
            "\"";
        t.expect(response_json.find(expected_schema_fragment) != std::string::npos,
            "protocol framing stamps every response with the hosted observability schema version");

        const std::string audit_line = sugar::service::format_hosted_audit_log_line(
            "event=result result=ok session=sess-000001 job_id=job-000001");
        t.expect(audit_line.find("schema_version=sugarbot.hosted.v1") != std::string::npos,
            "hosted audit log formatting stamps the schema version");
        t.expect(audit_line.find("category=result_lifecycle event=result") != std::string::npos,
            "hosted audit log formatting derives the result lifecycle category from the event");
        t.expect(
            sugar::service::format_hosted_audit_log_line(audit_line) == audit_line,
            "hosted audit log formatting is idempotent for already-stamped log lines");

        sugar::service::ProtocolResponse result_response{};
        result_response.command = sugar::service::ProtocolCommand::RetrieveResults;
        result_response.ok = true;
        result_response.message = "job result retrieved";
        result_response.job_id = "job-000001";
        sugar::service::JobResult retained_result{};
        retained_result.result_id = "res-000001";
        retained_result.owner_session_id = "sess-000001";
        retained_result.job_id = "job-000001";
        retained_result.summary_text = "Execution completed";
        retained_result.metadata.validation.valid = true;
        retained_result.metadata.validation.resolved_mode = sugar::service::ResolvedExecutionMode::PrimarySeriesRun;
        sugar::BacktestResult retained_backtest{};
        retained_backtest.trade_hash = 13693342942469747168ull;
        retained_result.metadata.primary_backtest = retained_backtest;
        sugar::PlanSweepReport retained_sweep_report{};
        retained_sweep_report.requested_sweep.enabled = true;
        retained_sweep_report.requested_sweep.top_k = 3;
        retained_sweep_report.requested_sweep.max_evals = 16;
        retained_sweep_report.result_diversity.requested = true;
        retained_sweep_report.result_diversity.dedupe_trade_hash = true;
        retained_sweep_report.result_diversity.basin_aware = true;
        retained_sweep_report.result_diversity.retained_candidate_pool = 12;
        retained_sweep_report.result_diversity.returned_unique_trade_hashes = 2;
        retained_sweep_report.result_diversity.suppressed_duplicate_trade_hash = 7;
        retained_sweep_report.result_diversity.suppressed_basin = 3;
        retained_sweep_report.posterior.requested = true;
        retained_sweep_report.posterior.computed = true;
        retained_sweep_report.posterior.status =
            sugar::SweepPosteriorDiagnosticsStatus::Computed;
        retained_sweep_report.posterior.training_observation_count = 5;
        retained_sweep_report.warnings.push_back("sweep boundary concentration: all candidates use n=12");
        retained_result.metadata.sweep_report = retained_sweep_report;
        retained_result.metadata.report = sugar::make_sweep_evaluation_report(retained_sweep_report);
        sugar::RunProvenanceSummary retained_provenance{};
        retained_provenance.primary_series_key = "primary";
        retained_provenance.primary_selection_mode = "single_series_implicit";
        retained_provenance.series_count = 1;
        retained_provenance.input_warning_count = 1;
        retained_provenance.csv_had_header = true;
        retained_provenance.csv_physical_line_count = 5;
        retained_provenance.csv_blank_line_count = 1;
        retained_provenance.csv_data_row_count = 3;
        retained_provenance.dropped_row_count = 2;
        retained_provenance.input_warnings.push_back("blank row skipped");
        retained_result.metadata.run_provenance = retained_provenance;
        retained_result.input_artifacts.push_back(
            { "art-000001", sugar::service::ArtifactKind::LayoutSpec, "job-000001", {}, 42 });
        retained_result.output_artifacts.push_back(
            { "art-000004", sugar::service::ArtifactKind::SummaryText, "job-000001", {}, 128 });
        result_response.job_result = retained_result;
        const std::string result_response_json =
            sugar::service::serialize_protocol_response_json(result_response);
        t.expect(result_response_json.find("\"input_artifacts\"") != std::string::npos,
            "protocol framing serializes retained input artifact audit records");
        t.expect(result_response_json.find("\"audit\"") != std::string::npos,
            "protocol framing serializes retained result audit metadata");
        t.expect(result_response_json.find("\"report_sources\"") != std::string::npos,
            "protocol framing serializes retained result report source labels");
        t.expect(result_response_json.find("\"csv_blank_line_count\":1") != std::string::npos,
            "protocol framing serializes hosted parser blank-line diagnostics");
        t.expect(result_response_json.find("\"dropped_row_count\":2") != std::string::npos,
            "protocol framing serializes hosted preprocessing drop-row diagnostics");
        t.expect(result_response_json.find("\"input_warnings\":[\"blank row skipped\"]") != std::string::npos,
            "protocol framing serializes hosted input warning text");
        t.expect(result_response_json.find("\"result_diversity\"") != std::string::npos &&
                result_response_json.find("\"suppressed_duplicate_trade_hash\":7") != std::string::npos &&
                result_response_json.find("\"suppressed_basin\":3") != std::string::npos,
            "protocol framing serializes hosted sweep result-diversity diagnostics");
        t.expect(result_response_json.find("\"report\":{\"schema_version\":\"sugarbot.evaluation_report.v1\"") != std::string::npos &&
                result_response_json.find("\"posterior\"") != std::string::npos &&
                result_response_json.find("\"training_observation_count\":5") != std::string::npos,
            "protocol framing serializes the unified hosted evaluation report with posterior diagnostics");
        t.expect(result_response_json.find("\"report_sources\":[\"service.validation_metadata\",\"engine.run_provenance\",\"engine.evaluation_report\"") != std::string::npos,
            "protocol framing records the unified hosted evaluation report source");
        t.expect(
            result_response_json.find("\"canonical_report_field\":\"job_result.metadata.report\"") !=
                    std::string::npos &&
                result_response_json.find("\"legacy_alias_policy\":\"deprecated_compatibility_aliases\"") !=
                    std::string::npos &&
                result_response_json.find(
                    "\"deprecated_report_fields\":[\"job_result.metadata.primary_backtest\","
                    "\"job_result.metadata.sweep\"]") != std::string::npos,
            "protocol framing marks legacy hosted report fields as deprecated aliases of metadata.report");
        t.expect(result_response_json.find("\"warnings\":[\"sweep boundary concentration: all candidates use n=12\"]") != std::string::npos,
            "protocol framing serializes hosted sweep warning text");
        t.expect(result_response_json.find("\"trade_hash\":13693342942469747168") != std::string::npos,
            "protocol framing preserves exact top-level trade_hash integers");
        t.expect(
            result_response_json.find("\"name\":\"trade_hash\",\"kind\":\"core_metric\",\"value\":13693342942469747168") != std::string::npos,
            "protocol framing preserves exact metric_catalog trade_hash integers");

        std::string framed = sugar::service::frame_json_message(request_json);
        std::string extracted{};
        const auto full_status = sugar::service::try_extract_framed_json(
            framed,
            extracted,
            4096,
            err);
        t.expect(full_status == sugar::service::FramedMessageParseStatus::Complete,
            "protocol framing extracts complete framed JSON payload");
        t.expect(full_status == sugar::service::FramedMessageParseStatus::Complete && extracted == request_json,
            "protocol framing preserves the exact framed JSON payload");

        std::string truncated = sugar::service::frame_json_message("{\"op\":\"connect\"}");
        truncated.pop_back();
        extracted.clear();
        const auto truncated_status = sugar::service::try_extract_framed_json(
            truncated,
            extracted,
            4096,
            err);
        t.expect(truncated_status == sugar::service::FramedMessageParseStatus::Incomplete,
            "protocol framing reports incomplete truncated payloads");

        err.clear();
        const bool malformed_ok = sugar::service::parse_protocol_request_json(
            "{\"op\":\"upload_artifact\",\"artifact\":[]}",
            parsed,
            err);
        t.expect(!malformed_ok, "protocol framing rejects malformed request JSON");
        t.expect(!malformed_ok && err.find("session_id") != std::string::npos,
            "protocol framing exposes explicit malformed-request diagnostics");

        return true;
    }

    bool test_service_protocol_dispatch_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_dispatch] service protocol dispatch surface\n";

        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime = make_service_runtime_config(
            contract_tmp_case_dir("service_protocol_dispatch"),
            64,
            1);
        config.source_root = std::filesystem::current_path();
        sugar::service::ProtocolDispatcher dispatcher(config);

        sugar::service::ProtocolRequest connect{};
        connect.command = sugar::service::ProtocolCommand::Connect;
        sugar::service::ProtocolResponse connect_response{};
        const bool connected = dispatcher.handle_request(connect, connect_response);
        t.expect(connected, "protocol dispatcher creates session on connect");
        t.expect(connected && !connect_response.session_id.empty(),
            "protocol dispatcher returns a concrete session id");
        if (!connected || connect_response.session_id.empty()) {
            return true;
        }
        const std::string session_id = connect_response.session_id;

        sugar::service::ProtocolRequest list_templates{};
        list_templates.command = sugar::service::ProtocolCommand::ListTemplates;
        list_templates.session_id = session_id;
        sugar::service::ProtocolResponse list_templates_response{};
        const bool listed_templates = dispatcher.handle_request(list_templates, list_templates_response);
        t.expect(listed_templates, "protocol dispatcher lists built-in templates");
        t.expect(listed_templates && !list_templates_response.catalog.empty(),
            "protocol dispatcher exposes non-empty built-in template catalog");

        sugar::service::ProtocolRequest list_datasets{};
        list_datasets.command = sugar::service::ProtocolCommand::ListDatasets;
        list_datasets.session_id = session_id;
        sugar::service::ProtocolResponse list_datasets_response{};
        const bool listed_datasets = dispatcher.handle_request(list_datasets, list_datasets_response);
        t.expect(listed_datasets, "protocol dispatcher lists hosted datasets");

        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);

        auto upload = [&](sugar::service::ArtifactKind kind, const std::string& name, const std::string& content) {
            sugar::service::ProtocolRequest upload_request{};
            upload_request.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload_request.session_id = session_id;
            upload_request.upload_kind = kind;
            upload_request.upload_client_name = name;
            upload_request.upload_content = content;
            sugar::service::ProtocolResponse upload_response{};
            const bool ok = dispatcher.handle_request(upload_request, upload_response);
            t.expect(ok, std::string("protocol dispatcher uploads artifact kind '") +
                std::string(sugar::service::artifact_kind_name(kind)) + "'");
            return upload_response;
        };

        const auto layout_upload = upload(
            sugar::service::ArtifactKind::LayoutSpec,
            request.layout.client_name,
            request.layout.content);
        const auto logic_upload = upload(
            sugar::service::ArtifactKind::LogicSpec,
            request.logic.client_name,
            request.logic.content);
        const auto data_upload = upload(
            sugar::service::ArtifactKind::DataCsv,
            request.data.client_name,
            request.data.content);

        t.expect(layout_upload.upload.has_value() && logic_upload.upload.has_value() && data_upload.upload.has_value(),
            "protocol dispatcher returns upload ids for each staged artifact");
        if (!layout_upload.upload.has_value() || !logic_upload.upload.has_value() || !data_upload.upload.has_value()) {
            return true;
        }

        sugar::service::ProtocolRequest validate{};
        validate.command = sugar::service::ProtocolCommand::ValidateRequest;
        validate.session_id = session_id;
        validate.layout_upload_id = layout_upload.upload->upload_id;
        validate.logic_upload_id = logic_upload.upload->upload_id;
        validate.data_upload_id = data_upload.upload->upload_id;
        sugar::service::ProtocolResponse validate_response{};
        const bool validated = dispatcher.handle_request(validate, validate_response);
        t.expect(validated, "protocol dispatcher accepts validate_request over staged uploads");
        t.expect(validated && !validate_response.job_id.empty(),
            "protocol dispatcher returns a validation job id for asynchronous processing");
        if (!validated || validate_response.job_id.empty()) {
            return true;
        }

        auto wait_for_terminal_status = [&](const std::string& job_id, sugar::service::JobStatus& out_status) {
            for (int attempt = 0; attempt < 2000; ++attempt) {
                sugar::service::ProtocolRequest status_request{};
                status_request.command = sugar::service::ProtocolCommand::CheckStatus;
                status_request.session_id = session_id;
                status_request.job_id = job_id;
                sugar::service::ProtocolResponse status_response{};
                if (!dispatcher.handle_request(status_request, status_response) ||
                    !status_response.job_status.has_value()) {
                    return false;
                }
                out_status = *status_response.job_status;
                if (out_status.state == sugar::service::JobState::Completed ||
                    out_status.state == sugar::service::JobState::Rejected ||
                    out_status.state == sugar::service::JobState::Failed ||
                    out_status.state == sugar::service::JobState::Canceled) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };

        sugar::service::JobStatus validate_status{};
        const bool validate_completed = wait_for_terminal_status(validate_response.job_id, validate_status);
        t.expect(validate_completed, "protocol dispatcher validation job reaches a terminal state");
        t.expect(validate_completed && validate_status.state == sugar::service::JobState::Completed,
            "protocol dispatcher validation job completes asynchronously");

        sugar::service::ProtocolRequest validate_result{};
        validate_result.command = sugar::service::ProtocolCommand::RetrieveResults;
        validate_result.session_id = session_id;
        validate_result.job_id = validate_response.job_id;
        sugar::service::ProtocolResponse validate_result_response{};
        const bool has_validate_result = dispatcher.handle_request(validate_result, validate_result_response);
        t.expect(has_validate_result, "protocol dispatcher serves completed validation results");
        t.expect(has_validate_result && validate_result_response.job_result.has_value() &&
                validate_result_response.job_result->metadata.validation.resolved_mode ==
                    sugar::service::ResolvedExecutionMode::ValidationOnly,
            "protocol dispatcher preserves validation-only result metadata after asynchronous completion");

        sugar::service::ProtocolRequest submit{};
        submit.command = sugar::service::ProtocolCommand::SubmitJob;
        submit.session_id = session_id;
        submit.layout_upload_id = layout_upload.upload->upload_id;
        submit.logic_upload_id = logic_upload.upload->upload_id;
        submit.data_upload_id = data_upload.upload->upload_id;
        sugar::service::ProtocolResponse submit_response{};
        const bool submitted = dispatcher.handle_request(submit, submit_response);
        t.expect(submitted, "protocol dispatcher accepts submit_job over staged uploads");
        t.expect(submitted && !submit_response.job_id.empty(),
            "protocol dispatcher returns an execution job id for asynchronous processing");
        if (!submitted || submit_response.job_id.empty()) {
            return true;
        }

        sugar::service::JobStatus submit_status{};
        const bool submit_completed = wait_for_terminal_status(submit_response.job_id, submit_status);
        t.expect(submit_completed, "protocol dispatcher execution job reaches a terminal state");
        t.expect(submit_completed && submit_status.state == sugar::service::JobState::Completed,
            "protocol dispatcher execution job completes asynchronously");

        sugar::service::ProtocolRequest status{};
        status.command = sugar::service::ProtocolCommand::CheckStatus;
        status.session_id = session_id;
        status.job_id = submit_response.job_id;
        sugar::service::ProtocolResponse status_response{};
        const bool has_status = dispatcher.handle_request(status, status_response);
        t.expect(has_status, "protocol dispatcher serves status checks for completed jobs");
        t.expect(has_status && status_response.job_status.has_value() &&
                status_response.job_status->state == sugar::service::JobState::Completed,
            "protocol dispatcher preserves completed job state on status checks");

        sugar::service::ProtocolRequest cancel_completed{};
        cancel_completed.command = sugar::service::ProtocolCommand::CancelJob;
        cancel_completed.session_id = session_id;
        cancel_completed.job_id = submit_response.job_id;
        sugar::service::ProtocolResponse cancel_completed_response{};
        const bool canceled_completed =
            dispatcher.handle_request(cancel_completed, cancel_completed_response);
        t.expect(canceled_completed && cancel_completed_response.ok,
            "protocol dispatcher accepts cancel on completed owned jobs as terminal no-op success");
        t.expect(canceled_completed &&
                cancel_completed_response.message.find("no-op") != std::string::npos &&
                cancel_completed_response.job_status.has_value() &&
                cancel_completed_response.job_status->state == sugar::service::JobState::Completed,
            "protocol dispatcher completed-job cancel response returns explicit current terminal status");

        sugar::service::ProtocolRequest result{};
        result.command = sugar::service::ProtocolCommand::RetrieveResults;
        result.session_id = session_id;
        result.job_id = submit_response.job_id;
        sugar::service::ProtocolResponse result_response{};
        const bool has_result = dispatcher.handle_request(result, result_response);
        t.expect(has_result, "protocol dispatcher serves result retrieval for completed jobs");
        t.expect(has_result && result_response.job_result.has_value() &&
                result_response.job_result->metadata.primary_backtest.has_value() &&
                result_response.job_result->summary_text.find("Execution completed") != std::string::npos,
            "protocol dispatcher preserves human summary text in result retrieval");

        sugar::service::ProtocolRequest missing_validate{};
        missing_validate.command = sugar::service::ProtocolCommand::ValidateRequest;
        missing_validate.session_id = session_id;
        missing_validate.layout_upload_id = "upl-missing";
        missing_validate.logic_upload_id = logic_upload.upload->upload_id;
        missing_validate.data_upload_id = data_upload.upload->upload_id;
        sugar::service::ProtocolResponse missing_validate_response{};
        const bool missing_validate_ok = dispatcher.handle_request(missing_validate, missing_validate_response);
        t.expect(!missing_validate_ok, "protocol dispatcher rejects validate_request with unknown upload ids");
        t.expect(!missing_validate_ok &&
                missing_validate_response.error_code == sugar::service::ProtocolErrorCode::NotFound,
            "protocol dispatcher classifies missing upload ids as not_found");

        sugar::service::ProtocolRequest disconnect{};
        disconnect.command = sugar::service::ProtocolCommand::Disconnect;
        disconnect.session_id = session_id;
        sugar::service::ProtocolResponse disconnect_response{};
        const bool disconnected = dispatcher.handle_request(disconnect, disconnect_response);
        t.expect(disconnected, "protocol dispatcher closes sessions on disconnect");

        sugar::service::ProtocolResponse after_disconnect_response{};
        const bool after_disconnect = dispatcher.handle_request(list_templates, after_disconnect_response);
        t.expect(!after_disconnect, "protocol dispatcher rejects commands after disconnect");
        t.expect(!after_disconnect &&
                after_disconnect_response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
            "protocol dispatcher classifies post-disconnect access as unauthorized");

        return true;
    }

} // namespace sugar::contract_suite_internal
