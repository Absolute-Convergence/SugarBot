#include "contract_suite_internal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include "service/artifact_store.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"
#include "service/server_runtime.h"

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
            for (std::size_t i = 0; i < rows; ++i) {
                out << (20240101 + static_cast<int>(i))
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
        t.expect(owner_can_read && !owned_result.output_artifacts.empty(),
            "owned result preserves summary output artifact");

        sugar::service::JobResult intruder_result{};
        err.clear();
        const bool intruder_can_read = runtime.get_job_result(intruder.session_id, job_id, intruder_result, err);
        t.expect(!intruder_can_read, "non-owning session cannot fetch another session result");
        t.expect(!intruder_can_read && err.find("does not belong") != std::string::npos,
            "non-owning result fetch fails with explicit ownership error");

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

        err.clear();
        const bool reprocessed = runtime.process_job(job_id, err);
        t.expect(reprocessed, "service ignores canceled job processing cleanly");

        sugar::service::JobResult canceled_result{};
        err.clear();
        const bool has_result = runtime.get_job_result(session.session_id, job_id, canceled_result, err);
        t.expect(!has_result, "service does not fabricate result payload for canceled queued job");
        return true;
    }

    bool test_service_running_cancellation_contracts(ContractAsserts& t) {
        t.out << "[service_running_cancellation] service-core cooperative running-job cancellation\n";

        constexpr std::size_t kSweepEvals = 100000;
        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_running_cancellation"),
            kSweepEvals));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for running cancellation test");
        }

        auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        request.logic.content = single_series_sweep_logic_json(kSweepEvals);
        request.data.content = long_single_series_csv_text(128);

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

        bool saw_running = false;
        sugar::service::JobStatus running_status{};
        bool canceled = false;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            err.clear();
            if (runtime.get_job_status(session.session_id, job_id, running_status, err)) {
                saw_running =
                    std::any_of(running_status.progress.begin(), running_status.progress.end(),
                        [](const sugar::service::ProgressEvent& event) {
                            return event.completed > 0 && event.total > 0;
                        });
                if (saw_running) {
                    err.clear();
                    canceled = runtime.cancel_job(session.session_id, job_id, err);
                    break;
                }
                if (running_status.state == sugar::service::JobState::Validating ||
                    running_status.state == sugar::service::JobState::Running ||
                    running_status.state == sugar::service::JobState::CancelRequested) {
                    saw_running = true;
                    err.clear();
                    canceled = runtime.cancel_job(session.session_id, job_id, err);
                    if (canceled) {
                        break;
                    }
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

        t.expect(processed, "service processing exits cleanly after running-job cancellation");

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
            std::any_of(final_status.progress.begin(), final_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.completed > 0 && event.total > 0;
                });
        t.expect(has_execution_progress,
            "service running cancellation exposes execution progress before cancellation");

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
