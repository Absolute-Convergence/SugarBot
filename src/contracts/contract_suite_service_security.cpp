#include "contract_suite_internal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "service/protocol_dispatch.h"
#include "service/request_validation.h"
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

    } // namespace
    bool test_service_upload_path_abuse_contracts(ContractAsserts& t) {
        t.out << "[service_upload_path_abuse] service-core upload and path abuse rejection\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_upload_path_abuse")));
        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for upload/path abuse test");
        }

        auto expect_submit_rejected = [&](sugar::service::SubmitRequest request,
                                           std::string_view expected_detail,
                                           std::string_view label) {
            std::string job_id;
            err.clear();
            const bool ok = runtime.submit_job(session.session_id, request, job_id, err);
            t.expect(!ok, std::string(label));
            t.expect(!ok && err.find(expected_detail) != std::string::npos,
                std::string(label) + " reports expected detail");
        };

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.layout.client_name.clear();
            expect_submit_rejected(
                std::move(request),
                "client name is empty",
                "service rejects empty layout client name");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.layout.client_name =
                (std::filesystem::temp_directory_path() / "layout.json").string();
            expect_submit_rejected(
                std::move(request),
                "absolute",
                "service rejects absolute layout client name");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.layout.client_name = "nested/layout.json";
            expect_submit_rejected(
                std::move(request),
                "single filename",
                "service rejects slash-separated nested layout client name");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.layout.client_name = "nested\\layout.json";
            expect_submit_rejected(
                std::move(request),
                "single filename",
                "service rejects backslash-separated nested layout client name");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.logic.client_name = "logic..json";
            expect_submit_rejected(
                std::move(request),
                "parent traversal",
                "service rejects dot-dot logic client name");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.logic.client_name = "logic.txt";
            expect_submit_rejected(
                std::move(request),
                ".json",
                "service rejects wrong logic artifact extension");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.data.client_name = "data.txt";
            expect_submit_rejected(
                std::move(request),
                ".csv",
                "service rejects wrong data artifact extension");
        }

        {
            auto request = make_single_series_request(sugar::service::JobIntent::Execute);
            request.data.content.assign((64 * 1024) + 1, 'x');
            expect_submit_rejected(
                std::move(request),
                "max upload size",
                "service rejects oversized data artifact content");
        }

        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime = make_service_runtime_config(
            contract_tmp_case_dir("service_upload_path_abuse_protocol"),
            64,
            1);
        config.source_root = std::filesystem::current_path();
        sugar::service::ProtocolDispatcher dispatcher(config);

        sugar::service::ProtocolRequest connect{};
        connect.command = sugar::service::ProtocolCommand::Connect;
        sugar::service::ProtocolResponse connect_response{};
        const bool connected = dispatcher.handle_request(connect, connect_response);
        t.expect(connected && !connect_response.session_id.empty(),
            "protocol dispatcher creates session for upload abuse test");
        if (!connected || connect_response.session_id.empty()) {
            return true;
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::LayoutSpec;
            upload.upload_client_name = "layout.json";
            upload.upload_content.clear();
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(!ok, "protocol dispatcher rejects empty upload content");
            t.expect(!ok && response.error_code == sugar::service::ProtocolErrorCode::InvalidArtifact,
                "protocol dispatcher classifies empty upload as invalid_artifact");
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::DataCsv;
            upload.upload_client_name = "data.csv";
            upload.upload_content.assign((64 * 1024) + 1, 'x');
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(!ok, "protocol dispatcher rejects oversized upload content");
            t.expect(!ok && response.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                "protocol dispatcher classifies oversized upload as quota_exceeded");
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::LayoutSpec;
            upload.upload_client_name = "layout";
            upload.upload_content = single_series_layout_json();
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(ok && response.upload.has_value(),
                "protocol dispatcher accepts extensionless layout upload names");
            t.expect(ok && response.upload.has_value() && response.upload->client_name == "layout.json",
                "protocol dispatcher normalizes extensionless layout upload names");
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::DataCsv;
            upload.upload_client_name.clear();
            upload.upload_content = single_series_csv_text();
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(ok && response.upload.has_value(),
                "protocol dispatcher accepts omitted data upload names");
            t.expect(ok && response.upload.has_value() && response.upload->client_name == "data.csv",
                "protocol dispatcher supplies a default data upload filename");
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::DataCsv;
            upload.upload_client_name = "data.txt";
            upload.upload_content = single_series_csv_text();
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(!ok, "protocol dispatcher rejects wrong-extension data upload names immediately");
            t.expect(!ok && response.error_code == sugar::service::ProtocolErrorCode::InvalidArtifact,
                "protocol dispatcher classifies wrong-extension upload names as invalid_artifact");
            t.expect(!ok && response.message.find(".csv") != std::string::npos,
                "protocol dispatcher reports the expected data upload extension");
        }

        {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = connect_response.session_id;
            upload.upload_kind = sugar::service::ArtifactKind::LayoutSpec;
            upload.upload_client_name = "nested/layout";
            upload.upload_content = single_series_layout_json();
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(!ok, "protocol dispatcher rejects nested upload client names immediately");
            t.expect(!ok && response.message.find("single filename") != std::string::npos,
                "protocol dispatcher explains nested upload client-name rejection");
        }

        return true;
    }

    bool test_service_session_object_probe_abuse_contracts(ContractAsserts& t) {
        t.out << "[service_session_object_probe_abuse] service protocol session and object probe abuse rejection\n";

        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime = make_service_runtime_config(
            contract_tmp_case_dir("service_session_object_probe_abuse"),
            64,
            1);
        config.source_root = std::filesystem::current_path();
        sugar::service::ProtocolDispatcher dispatcher(config);

        auto connect_session = [&](std::string& out_session_id) {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(request, response);
            out_session_id = response.session_id;
            return ok && !out_session_id.empty();
        };

        std::string owner_session_id;
        std::string intruder_session_id;
        if (!connect_session(owner_session_id) || !connect_session(intruder_session_id)) {
            return t.expect(false, "protocol dispatcher creates owner and intruder sessions for probe test");
        }

        {
            sugar::service::ProtocolRequest status{};
            status.command = sugar::service::ProtocolCommand::CheckStatus;
            status.session_id = "sess999999";
            status.job_id = "job1";
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(status, response);
            t.expect(!ok, "protocol dispatcher rejects status probe with unknown session");
            t.expect(!ok && response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
                "protocol dispatcher classifies unknown-session status probe as unauthorized");
        }

        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        auto upload = [&](sugar::service::ArtifactKind kind, const std::string& name, const std::string& content) {
            sugar::service::ProtocolRequest upload_request{};
            upload_request.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload_request.session_id = owner_session_id;
            upload_request.upload_kind = kind;
            upload_request.upload_client_name = name;
            upload_request.upload_content = content;
            sugar::service::ProtocolResponse upload_response{};
            const bool ok = dispatcher.handle_request(upload_request, upload_response);
            t.expect(ok && upload_response.upload.has_value(),
                std::string("protocol dispatcher accepts owner upload for probe test: ") + name);
            return upload_response.upload.has_value() ? upload_response.upload->upload_id : std::string{};
        };

        const std::string layout_upload_id = upload(
            sugar::service::ArtifactKind::LayoutSpec,
            request.layout.client_name,
            request.layout.content);
        const std::string logic_upload_id = upload(
            sugar::service::ArtifactKind::LogicSpec,
            request.logic.client_name,
            request.logic.content);
        const std::string data_upload_id = upload(
            sugar::service::ArtifactKind::DataCsv,
            request.data.client_name,
            request.data.content);
        if (layout_upload_id.empty() || logic_upload_id.empty() || data_upload_id.empty()) {
            return true;
        }

        sugar::service::ProtocolRequest submit{};
        submit.command = sugar::service::ProtocolCommand::SubmitJob;
        submit.session_id = owner_session_id;
        submit.layout_upload_id = layout_upload_id;
        submit.logic_upload_id = logic_upload_id;
        submit.data_upload_id = data_upload_id;
        sugar::service::ProtocolResponse submit_response{};
        const bool submitted = dispatcher.handle_request(submit, submit_response);
        t.expect(submitted && !submit_response.job_id.empty(),
            "protocol dispatcher accepts owner submit for probe test");
        if (!submitted || submit_response.job_id.empty()) {
            return true;
        }

        auto wait_for_terminal_status = [&](const std::string& job_id, sugar::service::JobStatus& out_status) {
            for (int attempt = 0; attempt < 2000; ++attempt) {
                sugar::service::ProtocolRequest status_request{};
                status_request.command = sugar::service::ProtocolCommand::CheckStatus;
                status_request.session_id = owner_session_id;
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

        sugar::service::JobStatus owner_terminal_status{};
        const bool reached_terminal = wait_for_terminal_status(submit_response.job_id, owner_terminal_status);
        t.expect(reached_terminal && owner_terminal_status.state == sugar::service::JobState::Completed,
            "owner job reaches completed state before object-probe checks");

        auto expect_job_probe_rejected = [&](sugar::service::ProtocolCommand command,
                                             const std::string& session_id,
                                             const std::string& job_id,
                                             std::string_view label) {
            sugar::service::ProtocolRequest probe{};
            probe.command = command;
            probe.session_id = session_id;
            probe.job_id = job_id;
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(probe, response);
            t.expect(!ok, std::string(label));
            t.expect(!ok && response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
                std::string(label) + " is classified as unauthorized");
            t.expect(!ok && response.message == "job is unavailable to the requesting session",
                std::string(label) + " uses the generic job-probe response");
            return response;
        };

        const auto foreign_status = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::CheckStatus,
            intruder_session_id,
            submit_response.job_id,
            "foreign session cannot probe owner job status");
        const auto unknown_status = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::CheckStatus,
            owner_session_id,
            "job999999",
            "owner session cannot distinguish unknown job status");
        t.expect(foreign_status.error_code == unknown_status.error_code &&
                foreign_status.message == unknown_status.message,
            "foreign and unknown status probes return indistinguishable external errors");

        const auto foreign_result = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::RetrieveResults,
            intruder_session_id,
            submit_response.job_id,
            "foreign session cannot probe owner job result");
        const auto unknown_result = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::RetrieveResults,
            owner_session_id,
            "job999999",
            "owner session cannot distinguish unknown job result");
        t.expect(foreign_result.error_code == unknown_result.error_code &&
                foreign_result.message == unknown_result.message,
            "foreign and unknown result probes return indistinguishable external errors");

        const auto foreign_cancel = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::CancelJob,
            intruder_session_id,
            submit_response.job_id,
            "foreign session cannot cancel owner job");
        const auto unknown_cancel = expect_job_probe_rejected(
            sugar::service::ProtocolCommand::CancelJob,
            owner_session_id,
            "job999999",
            "owner session cannot distinguish unknown job cancellation");
        t.expect(foreign_cancel.error_code == unknown_cancel.error_code &&
                foreign_cancel.message == unknown_cancel.message,
            "foreign and unknown cancel probes return indistinguishable external errors");

        sugar::service::ProtocolRequest owner_result{};
        owner_result.command = sugar::service::ProtocolCommand::RetrieveResults;
        owner_result.session_id = owner_session_id;
        owner_result.job_id = submit_response.job_id;
        sugar::service::ProtocolResponse owner_result_response{};
        const bool owner_can_fetch = dispatcher.handle_request(owner_result, owner_result_response);
        t.expect(owner_can_fetch && owner_result_response.job_result.has_value(),
            "owner result retrieval still succeeds after rejected probe attempts");

        return true;
    }

    bool test_service_session_upload_quota_abuse_contracts(ContractAsserts& t) {
        t.out << "[service_session_upload_quota_abuse] service protocol session and upload quota abuse rejection\n";

        {
            sugar::service::RuntimeConfig runtime = make_service_runtime_config(
                contract_tmp_case_dir("service_session_upload_quota_abuse_connect"),
                64,
                1);
            runtime.quotas.max_live_sessions = 2;
            runtime.quotas.max_staged_uploads_per_session = 2;
            runtime.quotas.max_staged_upload_bytes_per_session = 256;

            sugar::service::ProtocolDispatcherConfig config{};
            config.runtime = runtime;
            config.source_root = std::filesystem::current_path();
            sugar::service::ProtocolDispatcher dispatcher(config);

            auto connect_session = [&](std::string& out_session_id) {
                sugar::service::ProtocolRequest request{};
                request.command = sugar::service::ProtocolCommand::Connect;
                sugar::service::ProtocolResponse response{};
                const bool ok = dispatcher.handle_request(request, response);
                out_session_id = response.session_id;
                return std::pair{ ok, response };
            };

            std::string first_session_id;
            std::string second_session_id;
            const auto [first_connected, first_response] = connect_session(first_session_id);
            const auto [second_connected, second_response] = connect_session(second_session_id);
            t.expect(first_connected && !first_session_id.empty(),
                "protocol dispatcher accepts first session under live-session quota");
            t.expect(second_connected && !second_session_id.empty(),
                "protocol dispatcher accepts second session under live-session quota");
            if (!first_connected || !second_connected) {
                return true;
            }

            std::string rejected_session_id;
            const auto [third_connected, third_response] = connect_session(rejected_session_id);
            t.expect(!third_connected, "protocol dispatcher rejects session creation above live-session quota");
            t.expect(!third_connected &&
                    third_response.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                "protocol dispatcher classifies live-session exhaustion as quota_exceeded");
            t.expect(!third_connected &&
                    third_response.message.find("max live sessions") != std::string::npos,
                "protocol dispatcher exposes explicit live-session quota detail");

            sugar::service::ProtocolRequest disconnect{};
            disconnect.command = sugar::service::ProtocolCommand::Disconnect;
            disconnect.session_id = second_session_id;
            sugar::service::ProtocolResponse disconnect_response{};
            const bool disconnected = dispatcher.handle_request(disconnect, disconnect_response);
            t.expect(disconnected, "protocol dispatcher disconnect releases one live-session slot");
            if (!disconnected) {
                return true;
            }

            std::string replacement_session_id;
            const auto [replacement_connected, replacement_response] = connect_session(replacement_session_id);
            t.expect(replacement_connected && !replacement_session_id.empty(),
                "protocol dispatcher accepts a new session after live-session recovery");
            (void)first_response;
            (void)second_response;
            (void)replacement_response;

            auto upload = [&](const std::string& session_id,
                              sugar::service::ArtifactKind kind,
                              std::string_view client_name,
                              std::string_view content,
                              sugar::service::ProtocolResponse& out_response) {
                sugar::service::ProtocolRequest request{};
                request.command = sugar::service::ProtocolCommand::UploadArtifact;
                request.session_id = session_id;
                request.upload_kind = kind;
                request.upload_client_name = std::string(client_name);
                request.upload_content = std::string(content);
                return dispatcher.handle_request(request, out_response);
            };

            sugar::service::ProtocolResponse upload_one{};
            const bool uploaded_one = upload(
                first_session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                "{\n}\n",
                upload_one);
            t.expect(uploaded_one && upload_one.upload.has_value(),
                "protocol dispatcher accepts first staged upload under per-session upload quota");

            sugar::service::ProtocolResponse upload_two{};
            const bool uploaded_two = upload(
                first_session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                "{\n}\n",
                upload_two);
            t.expect(uploaded_two && upload_two.upload.has_value(),
                "protocol dispatcher accepts second staged upload under per-session upload quota");

            sugar::service::ProtocolResponse upload_three{};
            const bool uploaded_three = upload(
                first_session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                "date,close\n20240101,10\n",
                upload_three);
            t.expect(!uploaded_three,
                "protocol dispatcher rejects staged upload above per-session upload-count quota");
            t.expect(!uploaded_three &&
                    upload_three.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                "protocol dispatcher classifies upload-count exhaustion as quota_exceeded");
            t.expect(!uploaded_three &&
                    upload_three.message.find("max staged uploads") != std::string::npos,
                "protocol dispatcher exposes explicit upload-count quota detail");

            sugar::service::ProtocolRequest first_disconnect{};
            first_disconnect.command = sugar::service::ProtocolCommand::Disconnect;
            first_disconnect.session_id = first_session_id;
            sugar::service::ProtocolResponse first_disconnect_response{};
            const bool first_disconnected = dispatcher.handle_request(first_disconnect, first_disconnect_response);
            t.expect(first_disconnected, "protocol dispatcher disconnect clears staged uploads with the session");
            if (!first_disconnected) {
                return true;
            }

            std::string reset_session_id;
            const auto [reset_connected, reset_connect_response] = connect_session(reset_session_id);
            t.expect(reset_connected && !reset_session_id.empty(),
                "protocol dispatcher accepts a new session after staged-upload cleanup");
            if (!reset_connected) {
                return true;
            }
            (void)reset_connect_response;

            sugar::service::ProtocolResponse reset_upload{};
            const bool reset_uploaded = upload(
                reset_session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                "{\n}\n",
                reset_upload);
            t.expect(reset_uploaded && reset_upload.upload.has_value(),
                "fresh session starts with an empty staged-upload budget after disconnect cleanup");
        }

        {
            sugar::service::RuntimeConfig runtime = make_service_runtime_config(
                contract_tmp_case_dir("service_session_upload_quota_abuse_bytes"),
                64,
                1);
            runtime.quotas.max_live_sessions = 4;
            runtime.quotas.max_staged_uploads_per_session = 8;
            runtime.quotas.max_staged_upload_bytes_per_session = 16;
            runtime.quotas.max_upload_size_bytes = 32;

            sugar::service::ProtocolDispatcherConfig config{};
            config.runtime = runtime;
            config.source_root = std::filesystem::current_path();
            sugar::service::ProtocolDispatcher dispatcher(config);

            sugar::service::ProtocolRequest connect{};
            connect.command = sugar::service::ProtocolCommand::Connect;
            sugar::service::ProtocolResponse connect_response{};
            const bool connected = dispatcher.handle_request(connect, connect_response);
            t.expect(connected && !connect_response.session_id.empty(),
                "protocol dispatcher creates session for staged-upload byte quota test");
            if (!connected || connect_response.session_id.empty()) {
                return true;
            }

            auto upload = [&](std::string_view client_name,
                              std::string_view content,
                              sugar::service::ProtocolResponse& out_response) {
                sugar::service::ProtocolRequest request{};
                request.command = sugar::service::ProtocolCommand::UploadArtifact;
                request.session_id = connect_response.session_id;
                request.upload_kind = sugar::service::ArtifactKind::LayoutSpec;
                request.upload_client_name = std::string(client_name);
                request.upload_content = std::string(content);
                return dispatcher.handle_request(request, out_response);
            };

            sugar::service::ProtocolResponse first_upload{};
            const bool first_ok = upload("layout.json", "1234567890", first_upload);
            t.expect(first_ok && first_upload.upload.has_value(),
                "protocol dispatcher accepts staged upload within byte quota");

            sugar::service::ProtocolResponse second_upload{};
            const bool second_ok = upload("layout.json", "abcdefg", second_upload);
            t.expect(!second_ok, "protocol dispatcher rejects staged upload above byte quota");
            t.expect(!second_ok &&
                    second_upload.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                "protocol dispatcher classifies upload-byte exhaustion as quota_exceeded");
            t.expect(!second_ok &&
                    second_upload.message.find("max staged upload bytes") != std::string::npos,
                "protocol dispatcher exposes explicit upload-byte quota detail");
        }

        return true;
    }

    bool test_service_expensive_sweep_policy_abuse_contracts(ContractAsserts& t) {
        t.out << "[service_expensive_sweep_policy_abuse] service protocol expensive sweep policy abuse rejection\n";

        sugar::service::RuntimeConfig runtime = make_service_runtime_config(
            contract_tmp_case_dir("service_expensive_sweep_policy_abuse"),
            10,
            1);
        runtime.quotas.max_live_sessions = 4;
        runtime.quotas.max_total_active_jobs = 64;
        runtime.quotas.max_concurrent_jobs_per_session = 32;
        runtime.quotas.sweep_warning_minutes = 15;
        runtime.quotas.sweep_reject_minutes = 30;
        runtime.quotas.expensive_sweep_reject_disconnect_streak = 3;
        runtime.quotas.expensive_sweep_reject_pair_disconnect_count = 3;
        runtime.quotas.expensive_sweep_reject_total_before_disconnect = 8;

        const std::size_t moderate_sweep_minutes =
            sugar::service::estimate_sweep_runtime_minutes(
                12354,
                512,
                sugar::PlanSweepEngineKind::Exhaustive);
        t.expect(
            moderate_sweep_minutes > 0 && moderate_sweep_minutes < 10,
            "hosted sweep estimate no longer inflates modest exhaustive workloads to quota-ceiling time scales");

        sugar::service::ServiceProfile parsed_profile{};
        t.expect(
            sugar::service::parse_service_profile("dev", parsed_profile) &&
                parsed_profile == sugar::service::ServiceProfile::LocalAi,
            "service profile parsing accepts the local-ai development alias");
        const auto local_ai_quotas = sugar::service::make_quota_config_for_service_profile(
            sugar::service::ServiceProfile::LocalAi);
        const auto hosted_public_quotas = sugar::service::make_quota_config_for_service_profile(
            sugar::service::ServiceProfile::HostedPublic);
        t.expect(local_ai_quotas.max_live_sessions > hosted_public_quotas.max_live_sessions,
            "local-ai service profile relaxes live-session capacity compared with hosted-public");
        t.expect(local_ai_quotas.expensive_sweep_reject_disconnect_streak == 0,
            "local-ai service profile disables forced disconnects for repeated expensive-sweep rejects");
        t.expect(hosted_public_quotas.expensive_sweep_reject_disconnect_streak > 0,
            "hosted-public service profile keeps repeated expensive-sweep forced-disconnect protection");

        std::vector<std::string> security_log_lines;
        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime = runtime;
        config.source_root = std::filesystem::current_path();
        config.security_log = [&](std::string_view line) {
            security_log_lines.emplace_back(line);
        };
        sugar::service::ProtocolDispatcher dispatcher(config);

        auto log_contains = [&](std::string_view needle) {
            return std::any_of(
                security_log_lines.begin(),
                security_log_lines.end(),
                [&](const std::string& line) {
                    return line.find(needle) != std::string::npos;
                });
        };

        auto connect_session = [&]() {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(request, response);
            t.expect(ok && !response.session_id.empty(),
                "protocol dispatcher creates session for expensive-sweep policy test");
            return response.session_id;
        };

        auto upload_artifact = [&](const std::string& session_id,
                                   sugar::service::ArtifactKind kind,
                                   std::string_view client_name,
                                   std::string_view content) {
            sugar::service::ProtocolRequest upload{};
            upload.command = sugar::service::ProtocolCommand::UploadArtifact;
            upload.session_id = session_id;
            upload.upload_kind = kind;
            upload.upload_client_name = std::string(client_name);
            upload.upload_content = std::string(content);
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(upload, response);
            t.expect(ok && response.upload.has_value(),
                std::string("protocol dispatcher stages expensive-sweep artifact: ") + std::string(client_name));
            return response.upload.has_value() ? response.upload->upload_id : std::string{};
        };

        auto submit_with_uploads = [&](const std::string& session_id,
                                       const std::string& layout_upload_id,
                                       const std::string& logic_upload_id,
                                       const std::string& data_upload_id) {
            sugar::service::ProtocolRequest submit{};
            submit.command = sugar::service::ProtocolCommand::SubmitJob;
            submit.session_id = session_id;
            submit.layout_upload_id = layout_upload_id;
            submit.logic_upload_id = logic_upload_id;
            submit.data_upload_id = data_upload_id;
            sugar::service::ProtocolResponse response{};
            const bool ok = dispatcher.handle_request(submit, response);
            return std::pair{ ok, response };
        };

        const std::string layout_json = single_series_layout_json();
        const std::string data_csv = single_series_csv_text();
        const std::string warning_logic_json = single_series_sweep_logic_json(6);
        const std::string reject_logic_json = single_series_sweep_logic_json(11);

        {
            const std::string session_id = connect_session();
            if (session_id.empty()) {
                return true;
            }

            const std::string layout_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                layout_json);
            const std::string warning_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                warning_logic_json);
            const std::string data_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                data_csv);

            const auto [warning_ok, warning_response] = submit_with_uploads(
                session_id,
                layout_upload_id,
                warning_logic_upload_id,
                data_upload_id);
            t.expect(warning_ok && !warning_response.job_id.empty(),
                "protocol dispatcher allows a hosted sweep inside the 30-minute budget");
            t.expect(warning_ok &&
                    warning_response.message.find("warning: sweep candidate estimate") != std::string::npos,
                "protocol dispatcher surfaces the hosted long-sweep warning before execution");
            t.expect(warning_ok &&
                    warning_response.message.find("rough runtime estimate about") != std::string::npos,
                "protocol dispatcher warning text separates candidate-budget policy from runtime heuristics");
        }

        {
            const std::string session_id = connect_session();
            if (session_id.empty()) {
                return true;
            }

            const std::string layout_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                layout_json);
            const std::string reject_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                reject_logic_json);
            const std::string data_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                data_csv);

            sugar::service::ProtocolResponse third_response{};
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto [ok, response] = submit_with_uploads(
                    session_id,
                    layout_upload_id,
                    reject_logic_upload_id,
                    data_upload_id);
                t.expect(!ok, "protocol dispatcher rejects over-budget hosted sweep");
                t.expect(!ok &&
                        response.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                    "protocol dispatcher classifies over-budget hosted sweep as quota_exceeded");
                if (attempt < 2) {
                    t.expect(!response.close_connection,
                        "protocol dispatcher leaves the session open before the streak threshold");
                }
                third_response = response;
            }

            t.expect(third_response.close_connection,
                "protocol dispatcher closes the session after three over-budget sweeps in a row");
            t.expect(third_response.message.find("session closed after repeated over-limit sweep submissions") != std::string::npos,
                "protocol dispatcher exposes explicit forced-disconnect detail after the streak threshold");

            sugar::service::ProtocolRequest follow_up{};
            follow_up.command = sugar::service::ProtocolCommand::ListDatasets;
            follow_up.session_id = session_id;
            sugar::service::ProtocolResponse follow_up_response{};
            const bool follow_up_ok = dispatcher.handle_request(follow_up, follow_up_response);
            t.expect(!follow_up_ok && follow_up_response.close_connection,
                "blocked session cannot continue issuing protocol requests after forced disconnect");
        }

        {
            const std::string session_id = connect_session();
            if (session_id.empty()) {
                return true;
            }

            const std::string layout_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                layout_json);
            const std::string warning_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic-warning.json",
                warning_logic_json);
            const std::string reject_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic-reject.json",
                reject_logic_json);
            const std::string data_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                data_csv);

            sugar::service::ProtocolResponse response{};
            for (int cycle = 0; cycle < 3; ++cycle) {
                const auto [first_ok, first_response] = submit_with_uploads(
                    session_id,
                    layout_upload_id,
                    reject_logic_upload_id,
                    data_upload_id);
                const auto [second_ok, second_response] = submit_with_uploads(
                    session_id,
                    layout_upload_id,
                    reject_logic_upload_id,
                    data_upload_id);
                t.expect(!first_ok && !second_ok,
                    "protocol dispatcher records consecutive over-budget sweeps for pair counting");
                response = second_response;
                if (cycle < 2) {
                    t.expect(!response.close_connection,
                        "protocol dispatcher waits for the third repeated 2-in-a-row episode before forced disconnect");
                }

                if (cycle < 2) {
                    const auto [warning_ok, warning_response] = submit_with_uploads(
                        session_id,
                        layout_upload_id,
                        warning_logic_upload_id,
                        data_upload_id);
                    t.expect(warning_ok && !warning_response.close_connection,
                        "allowed hosted sweep resets the consecutive over-budget streak between pair episodes");
                }
            }

            t.expect(response.close_connection,
                "protocol dispatcher closes the session after three repeated 2-in-a-row over-budget episodes");
        }

        {
            const std::string session_id = connect_session();
            if (session_id.empty()) {
                return true;
            }

            const std::string layout_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                layout_json);
            const std::string warning_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic-warning.json",
                warning_logic_json);
            const std::string reject_logic_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic-reject.json",
                reject_logic_json);
            const std::string data_upload_id = upload_artifact(
                session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                data_csv);

            sugar::service::ProtocolResponse final_response{};
            for (int attempt = 0; attempt < 9; ++attempt) {
                const auto [reject_ok, reject_response] = submit_with_uploads(
                    session_id,
                    layout_upload_id,
                    reject_logic_upload_id,
                    data_upload_id);
                t.expect(!reject_ok, "protocol dispatcher rejects each over-budget sweep in total-count escalation test");
                final_response = reject_response;
                if (attempt < 8) {
                    const auto [warning_ok, warning_response] = submit_with_uploads(
                        session_id,
                        layout_upload_id,
                        warning_logic_upload_id,
                        data_upload_id);
                    t.expect(warning_ok && !warning_response.close_connection,
                        "allowed hosted sweep keeps total-count escalation from triggering on streak alone");
                }
            }

            t.expect(final_response.close_connection,
                "protocol dispatcher closes the session after more than eight total over-budget sweeps in one session");
        }

        {
            sugar::service::RuntimeConfig local_runtime = runtime;
            local_runtime.quotas = local_ai_quotas;
            local_runtime.quotas.max_sweep_candidate_count = 10;
            local_runtime.quotas.sweep_warning_minutes = 15;
            local_runtime.quotas.sweep_reject_minutes = 30;

            sugar::service::ProtocolDispatcherConfig local_config{};
            local_config.runtime = local_runtime;
            local_config.source_root = std::filesystem::current_path();
            sugar::service::ProtocolDispatcher local_dispatcher(local_config);

            sugar::service::ProtocolRequest connect{};
            connect.command = sugar::service::ProtocolCommand::Connect;
            sugar::service::ProtocolResponse connect_response{};
            const bool connected = local_dispatcher.handle_request(connect, connect_response);
            t.expect(connected && !connect_response.session_id.empty(),
                "local-ai profile creates a development session for expensive-sweep rejection checks");

            auto local_upload = [&](sugar::service::ArtifactKind kind,
                                    std::string_view client_name,
                                    std::string_view content) {
                sugar::service::ProtocolRequest upload{};
                upload.command = sugar::service::ProtocolCommand::UploadArtifact;
                upload.session_id = connect_response.session_id;
                upload.upload_kind = kind;
                upload.upload_client_name = std::string(client_name);
                upload.upload_content = std::string(content);
                sugar::service::ProtocolResponse response{};
                const bool ok = local_dispatcher.handle_request(upload, response);
                t.expect(ok && response.upload.has_value(),
                    std::string("local-ai profile stages artifact: ") + std::string(client_name));
                return response.upload.has_value() ? response.upload->upload_id : std::string{};
            };

            const std::string layout_upload_id = local_upload(
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                layout_json);
            const std::string logic_upload_id = local_upload(
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                reject_logic_json);
            const std::string data_upload_id = local_upload(
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                data_csv);

            sugar::service::ProtocolResponse final_reject_response{};
            for (int attempt = 0; attempt < 5; ++attempt) {
                sugar::service::ProtocolRequest submit{};
                submit.command = sugar::service::ProtocolCommand::SubmitJob;
                submit.session_id = connect_response.session_id;
                submit.layout_upload_id = layout_upload_id;
                submit.logic_upload_id = logic_upload_id;
                submit.data_upload_id = data_upload_id;
                sugar::service::ProtocolResponse response{};
                const bool ok = local_dispatcher.handle_request(submit, response);
                t.expect(!ok &&
                        response.error_code == sugar::service::ProtocolErrorCode::QuotaExceeded,
                    "local-ai profile still rejects over-budget hosted sweeps");
                t.expect(!response.close_connection,
                    "local-ai profile preserves the session after repeated over-budget sweep rejects");
                final_reject_response = response;
            }

            sugar::service::ProtocolRequest list_datasets{};
            list_datasets.command = sugar::service::ProtocolCommand::ListDatasets;
            list_datasets.session_id = connect_response.session_id;
            sugar::service::ProtocolResponse list_response{};
            const bool listed = local_dispatcher.handle_request(list_datasets, list_response);
            t.expect(listed && !final_reject_response.close_connection,
                "local-ai profile keeps rejected sessions usable for follow-up debugging requests");
        }

        t.expect(log_contains("event=submit result=warning"),
            "security log records hosted long-sweep warning events");
        t.expect(log_contains("event=submit result=reject_over_budget"),
            "security log records over-budget sweep rejections");
        t.expect(log_contains("event=session_block result=closed"),
            "security log records forced session closure for repeated over-budget sweeps");
        t.expect(log_contains("schema_version=sugarbot.hosted.v1 category=job event=submit"),
            "security log stamps job audit events with the hosted schema and category");
        t.expect(log_contains("schema_version=sugarbot.hosted.v1 category=session event=session_block"),
            "security log stamps session audit events with the hosted schema and category");

        return true;
    }

} // namespace sugar::contract_suite_internal