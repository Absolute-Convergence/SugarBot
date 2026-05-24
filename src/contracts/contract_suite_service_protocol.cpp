#include "contract_suite_internal.h"
#include "contract_suite_service_protocol_support.h"

#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "service/network_client.h"
#include "service/network_server.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"

namespace sugar::contract_suite_internal {

    using namespace service_protocol_contract;

    bool test_service_protocol_live_client_run_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_client_run] live TCP one-command client run\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live client run contract");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        const auto case_dir = contract_tmp_case_dir("service_protocol_live_client_run");
        const auto fixture_dir = case_dir / "fixtures";
        std::filesystem::create_directories(fixture_dir);
        const auto layout_path = fixture_dir / "layout.json";
        const auto logic_path = fixture_dir / "logic.json";
        const auto data_path = fixture_dir / "data.csv";

        std::string err;
        const bool fixtures_written =
            write_text_file(layout_path, single_series_layout_json(), err) &&
            write_text_file(logic_path, single_series_sweep_logic_json(8), err) &&
            write_text_file(data_path, single_series_csv_text(32), err);
        t.expect(fixtures_written, "live client run contract writes layout, logic, and data fixtures");
        if (!fixtures_written) {
            t.out << "  fixture error: " << err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = case_dir / "storage";
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 2;
        options.connection_worker_limit = 2;
        options.max_pending_connections = 8;

        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for one-command client run");
        if (!started) {
            t.out << "  server start error: " << err << "\n";
            return true;
        }

        std::vector<std::string> args{
            "sugar_client",
            "run",
            "--host",
            "127.0.0.1",
            "--port",
            std::to_string(server.port()),
            "--layout",
            layout_path.string(),
            "--logic",
            logic_path.string(),
            "--data",
            data_path.string(),
            "--poll-ms",
            "25",
            "--wait-timeout-ms",
            "30000",
            "--raw-json",
        };
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }

        std::ostringstream client_out;
        std::ostringstream client_err;
        const int rc = sugar::service::run_tcp_client_from_cli(
            static_cast<int>(argv.size()),
            argv.data(),
            client_out,
            client_err);
        t.expect(rc == 0, "sugar_client run completes a hosted layout/logic/data job over live TCP");
        t.expect(client_err.str().empty(), "sugar_client run --raw-json keeps progress chatter off stderr on success");

        ParsedProtocolResponse result_response{};
        std::string parse_err;
        const bool parsed = parse_protocol_response_json(client_out.str(), result_response, parse_err);
        t.expect(parsed, "sugar_client run prints parseable final hosted result JSON");
        if (!parsed) {
            t.out << "  client parse error: " << parse_err << "\n";
            t.out << "  client stdout:\n" << client_out.str() << "\n";
            t.out << "  client stderr:\n" << client_err.str() << "\n";
        }
        t.expect(
            parsed &&
                result_response.ok &&
                result_response.command == sugar::service::ProtocolCommand::RetrieveResults &&
                result_response.has_job_result,
            "sugar_client run returns the final retrieve_results payload instead of intermediate ids");
        t.expect(
            parsed &&
                result_response.result_has_sweep_report &&
                result_response.result_sweep_top_candidate_count > 0,
            "sugar_client run preserves hosted sweep report metadata in the final result");

        std::vector<std::string> compact_args = args;
        compact_args.back() = "--compact";
        std::vector<char*> compact_argv;
        compact_argv.reserve(compact_args.size());
        for (auto& arg : compact_args) {
            compact_argv.push_back(arg.data());
        }

        std::ostringstream compact_out;
        std::ostringstream compact_err;
        const int compact_rc = sugar::service::run_tcp_client_from_cli(
            static_cast<int>(compact_argv.size()),
            compact_argv.data(),
            compact_out,
            compact_err);
        const std::string compact_text = compact_out.str();
        t.expect(compact_rc == 0, "sugar_client run completes in compact hosted-report mode");
        t.expect(compact_err.str().empty(), "sugar_client run --compact keeps progress chatter off stderr on success");
        t.expect(
            compact_text.find("Hosted compact report:") != std::string::npos &&
                compact_text.find("sweep_outcome:") != std::string::npos &&
                compact_text.find("candidate_outcomes:") != std::string::npos &&
                compact_text.find("best_run:") != std::string::npos &&
                compact_text.find("best_params:") != std::string::npos,
            "sugar_client run --compact prints outcome-first hosted report facts");
        t.expect(
            compact_text.find("\"job_result\"") == std::string::npos,
            "sugar_client run --compact avoids dumping raw hosted JSON");

        std::vector<std::string> human_args = args;
        human_args.pop_back();
        std::vector<char*> human_argv;
        human_argv.reserve(human_args.size());
        for (auto& arg : human_args) {
            human_argv.push_back(arg.data());
        }

        std::ostringstream human_out;
        std::ostringstream human_err;
        const int human_rc = sugar::service::run_tcp_client_from_cli(
            static_cast<int>(human_argv.size()),
            human_argv.data(),
            human_out,
            human_err);
        t.expect(human_rc == 0, "sugar_client run completes in default human-output mode");
        t.expect(
            human_out.str().find("Execution completed") != std::string::npos &&
                human_out.str().find("PlanSpec Sweep Results") != std::string::npos,
            "sugar_client run default output prints the hosted summary text");
        t.expect(
            human_out.str().find("\"job_result\"") == std::string::npos,
            "sugar_client run default output avoids dumping raw hosted JSON");
        t.expect(
            human_err.str().find("Connected session") != std::string::npos &&
                human_err.str().find("Submitted job") != std::string::npos,
            "sugar_client run default output keeps progress chatter on stderr");

        const auto timeout_logic_path = fixture_dir / "timeout_logic.json";
        const auto timeout_data_path = fixture_dir / "timeout_data.csv";
        const bool timeout_fixtures_written =
            write_text_file(timeout_logic_path, single_series_sweep_logic_json(10000), err) &&
            write_text_file(timeout_data_path, single_series_csv_text(256), err);
        t.expect(timeout_fixtures_written, "live client run timeout contract writes long-running fixtures");
        if (!timeout_fixtures_written) {
            t.out << "  timeout fixture error: " << err << "\n";
        }

        if (timeout_fixtures_written) {
            std::vector<std::string> timeout_args{
                "sugar_client",
                "run",
                "--host",
                "127.0.0.1",
                "--port",
                std::to_string(server.port()),
                "--layout",
                layout_path.string(),
                "--logic",
                timeout_logic_path.string(),
                "--data",
                timeout_data_path.string(),
                "--poll-ms",
                "1",
                "--wait-timeout-ms",
                "1",
                "--raw-json",
            };
            std::vector<char*> timeout_argv;
            timeout_argv.reserve(timeout_args.size());
            for (auto& arg : timeout_args) {
                timeout_argv.push_back(arg.data());
            }

            std::ostringstream timeout_out;
            std::ostringstream timeout_err;
            const int timeout_rc = sugar::service::run_tcp_client_from_cli(
                static_cast<int>(timeout_argv.size()),
                timeout_argv.data(),
                timeout_out,
                timeout_err);
            t.expect(timeout_rc == 1, "sugar_client run returns failure after wait-timeout expiry");
            t.expect(
                timeout_err.str().find("reuse the same session id") != std::string::npos &&
                    timeout_err.str().find("Creating a new session will not have access to this job.") != std::string::npos,
                "sugar_client run timeout guidance tells operators to reuse the same session id");

            ParsedProtocolResponse timeout_status{};
            std::string timeout_parse_err;
            const bool timeout_status_parsed = parse_protocol_response_json(
                timeout_out.str(),
                timeout_status,
                timeout_parse_err);
            t.expect(timeout_status_parsed, "sugar_client run timeout prints parseable latest status JSON");
            if (timeout_status_parsed) {
                t.expect(
                    timeout_status.command == sugar::service::ProtocolCommand::CheckStatus &&
                        timeout_status.has_job_status &&
                        !timeout_status.session_id.empty() &&
                        !timeout_status.job_id.empty(),
                    "sugar_client run timeout preserves the original session and job ids in the latest status payload");

                sugar::service::JobState terminal_state{};
                bool saw_active_state = false;
                std::string failure_message;
                std::size_t poll_count = 0;
                std::string wait_err;
                const bool reached_terminal = wait_for_terminal_status_over_tcp(
                    server.port(),
                    timeout_status.session_id,
                    timeout_status.job_id,
                    terminal_state,
                    saw_active_state,
                    failure_message,
                    poll_count,
                    wait_err);
                t.expect(reached_terminal, "same-session polling can resume after sugar_client run times out");
                if (reached_terminal) {
                    ParsedProtocolResponse resumed_result{};
                    std::string resumed_result_json;
                    std::string resumed_err;
                    sugar::service::ProtocolRequest resumed_request{};
                    resumed_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                    resumed_request.session_id = timeout_status.session_id;
                    resumed_request.job_id = timeout_status.job_id;
                    const bool resumed_ok = send_tcp_protocol_request(
                        server.port(),
                        resumed_request,
                        resumed_result,
                        resumed_result_json,
                        resumed_err);
                    t.expect(
                        resumed_ok &&
                            resumed_result.ok &&
                            resumed_result.command == sugar::service::ProtocolCommand::RetrieveResults,
                        "same-session result retrieval succeeds after sugar_client run timeout guidance");
                }
            }
        }

        server.stop();
        t.expect(server.exit_code() == 0, "live client run server stops cleanly after one-command run");
        if (server.exit_code() != 0 || rc != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
            t.out << "  server stdout:\n" << server.stdout_text();
            t.out << "  client stderr:\n" << client_err.str();
            t.out << "  client stdout:\n" << client_out.str();
        }

        return true;
    }

    bool test_service_protocol_live_multi_client_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_multi_client] live TCP multi-client submit/status/result concurrency\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live multi-client contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = contract_tmp_case_dir("service_protocol_live_multi_client");
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 3;
        options.max_live_sessions = 16;
        options.connection_worker_limit = 4;
        options.max_pending_connections = 16;
        options.max_active_jobs = 32;

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for multi-client protocol test");
        if (!started) {
            return true;
        }

        constexpr std::size_t kClientCount = 8;
        struct ClientRun {
            std::string session_id{};
            std::string job_id{};
            std::string error{};
            bool saw_active_state{ false };
            std::size_t status_polls{ 0 };
            ParsedProtocolResponse result_response{};
        };

        std::vector<ClientRun> runs(kClientCount);
        const std::string layout_json = single_series_layout_json();
        const std::string logic_json = single_series_sweep_logic_json(768);
        const std::string data_csv = single_series_csv_text(96);
        std::vector<std::thread> clients;
        clients.reserve(kClientCount);
        for (std::size_t i = 0; i < kClientCount; ++i) {
            clients.emplace_back([&, i] {
                std::string local_err;
                std::string layout_upload_id;
                std::string logic_upload_id;
                std::string data_upload_id;

                if (!connect_session(server.port(), runs[i].session_id, local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::LayoutSpec,
                        "layout.json",
                        layout_json,
                        layout_upload_id,
                        local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::LogicSpec,
                        "logic.json",
                        logic_json,
                        logic_upload_id,
                        local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::DataCsv,
                        "data.csv",
                        data_csv,
                        data_upload_id,
                        local_err) ||
                    !submit_job(
                        server.port(),
                        runs[i].session_id,
                        layout_upload_id,
                        logic_upload_id,
                        data_upload_id,
                        runs[i].job_id,
                        local_err)) {
                    runs[i].error = local_err;
                    return;
                }

                sugar::service::JobState terminal_state = sugar::service::JobState::Queued;
                std::string terminal_failure;
                if (!wait_for_terminal_status_over_tcp(
                        server.port(),
                        runs[i].session_id,
                        runs[i].job_id,
                        terminal_state,
                        runs[i].saw_active_state,
                        terminal_failure,
                        runs[i].status_polls,
                        local_err)) {
                    runs[i].error = local_err;
                    return;
                }
                if (terminal_state != sugar::service::JobState::Completed) {
                    runs[i].error =
                        "job reached unexpected terminal state: " +
                        std::string(sugar::service::job_state_name(terminal_state));
                    if (!terminal_failure.empty()) {
                        runs[i].error += " | " + terminal_failure;
                    }
                    return;
                }

                sugar::service::ProtocolRequest result_request{};
                result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                result_request.session_id = runs[i].session_id;
                result_request.job_id = runs[i].job_id;
                if (!send_tcp_protocol_request(server.port(), result_request, runs[i].result_response, local_err)) {
                    runs[i].error = local_err;
                    return;
                }
                if (!runs[i].result_response.ok ||
                    !runs[i].result_response.has_job_result ||
                    runs[i].result_response.result_owner_session_id != runs[i].session_id ||
                    !runs[i].result_response.result_has_primary_backtest ||
                    !runs[i].result_response.result_has_sweep_report ||
                    runs[i].result_response.result_sweep_top_candidate_count == 0) {
                    runs[i].error = runs[i].result_response.message.empty()
                        ? "result response did not preserve owned hosted sweep metadata"
                        : runs[i].result_response.message;
                    return;
                }

                if (!disconnect_session(server.port(), runs[i].session_id, local_err)) {
                    runs[i].error = local_err;
                }
            });
        }
        for (auto& client : clients) {
            client.join();
        }

        bool all_clients_ok = true;
        std::set<std::string> session_ids;
        std::set<std::string> job_ids;
        bool saw_active_state = false;
        for (const auto& run : runs) {
            all_clients_ok = all_clients_ok &&
                run.error.empty() &&
                !run.session_id.empty() &&
                !run.job_id.empty() &&
                run.result_response.has_job_result;
            if (!run.session_id.empty()) {
                session_ids.insert(run.session_id);
            }
            if (!run.job_id.empty()) {
                job_ids.insert(run.job_id);
            }
            saw_active_state = saw_active_state || run.saw_active_state;
        }
        t.expect(all_clients_ok, "live TCP multi-client workers submit, observe, and retrieve results without per-client failures");
        if (!all_clients_ok) {
            t.out << "  server exit code while diagnosing multi-client failure: " << server.exit_code() << "\n";
            const std::string server_stderr = server.stderr_text();
            const std::string server_stdout = server.stdout_text();
            if (!server_stderr.empty()) {
                t.out << "  server stderr:\n" << server_stderr;
            }
            if (!server_stdout.empty()) {
                t.out << "  server stdout:\n" << server_stdout;
            }
            for (std::size_t i = 0; i < runs.size(); ++i) {
                const auto& run = runs[i];
                if (!run.error.empty()) {
                    t.out << "  client " << (i + 1) << " error: " << run.error << "\n";
                }
            }
        }
        t.expect(session_ids.size() == kClientCount, "live TCP multi-client run assigns distinct session ids");
        t.expect(job_ids.size() == kClientCount, "live TCP multi-client run assigns distinct job ids");
        t.expect(saw_active_state, "live TCP multi-client polling observes queued/running progress while jobs are in flight");

        if (runs.size() >= 2 && !runs[0].job_id.empty() && !runs[1].session_id.empty()) {
            sugar::service::ProtocolRequest foreign_result_request{};
            foreign_result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
            foreign_result_request.session_id = runs[1].session_id;
            foreign_result_request.job_id = runs[0].job_id;

            ParsedProtocolResponse foreign_result_response{};
            err.clear();
            const bool foreign_replied =
                send_tcp_protocol_request(server.port(), foreign_result_request, foreign_result_response, err);
            t.expect(foreign_replied, "live TCP multi-client run returns a structured reply for foreign result retrieval");
            t.expect(
                foreign_replied &&
                    !foreign_result_response.ok &&
                    foreign_result_response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
                "live TCP multi-client run rejects foreign result retrieval without cross-talk");
        }

        server.stop();
        t.expect(server.exit_code() == 0, "live TCP multi-client server stops cleanly after concurrent traffic");
        if (server.exit_code() != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
        }

        return true;
    }

    bool test_service_protocol_live_cancel_polling_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_cancel_polling] live TCP repeated status polling and cancellation\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live cancel-polling contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = contract_tmp_case_dir("service_protocol_live_cancel_polling");
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 1;
        options.connection_worker_limit = 4;
        options.max_pending_connections = 16;
        options.max_active_jobs = 4;

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for cancel-polling protocol test");
        if (!started) {
            return true;
        }

        std::string owner_session_id;
        std::string intruder_session_id;
        std::string layout_upload_id;
        std::string logic_upload_id;
        std::string data_upload_id;
        std::string job_id;
        const bool staged =
            connect_session(server.port(), owner_session_id, err) &&
            connect_session(server.port(), intruder_session_id, err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                single_series_layout_json(),
                layout_upload_id,
                err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                single_series_sweep_logic_json(10000),
                logic_upload_id,
                err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                single_series_csv_text(256),
                data_upload_id,
                err) &&
            submit_job(
                server.port(),
                owner_session_id,
                layout_upload_id,
                logic_upload_id,
                data_upload_id,
                job_id,
                err);
        t.expect(staged, "live TCP cancel-polling path stages owner/intruder sessions and submits a long-running job");
        if (!staged) {
            server.stop();
            return true;
        }

        sugar::service::ProtocolRequest foreign_status_request{};
        foreign_status_request.command = sugar::service::ProtocolCommand::CheckStatus;
        foreign_status_request.session_id = intruder_session_id;
        foreign_status_request.job_id = job_id;
        ParsedProtocolResponse foreign_status_response{};
        err.clear();
        const bool foreign_status_replied =
            send_tcp_protocol_request(server.port(), foreign_status_request, foreign_status_response, err);
        t.expect(foreign_status_replied, "live TCP cancel-polling path returns a structured reply for foreign status access");
        t.expect(
            foreign_status_replied &&
                !foreign_status_response.ok &&
                foreign_status_response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
            "live TCP cancel-polling path rejects foreign status polling without leaking state");

        std::atomic_bool saw_active_state{ false };
        std::atomic_bool stop_pollers{ false };
        std::mutex poll_mutex{};
        std::vector<sugar::service::JobState> polled_states;
        std::vector<std::string> poll_errors;
        std::size_t result_poll_count = 0;
        std::vector<std::string> result_poll_errors;
        constexpr std::size_t kPollerCount = 3;
        std::vector<std::thread> pollers;
        pollers.reserve(kPollerCount);
        for (std::size_t i = 0; i < kPollerCount; ++i) {
            pollers.emplace_back([&, i] {
                using namespace std::chrono_literals;

                const auto deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
                while (!stop_pollers.load(std::memory_order_relaxed) &&
                    std::chrono::steady_clock::now() < deadline) {
                    sugar::service::ProtocolRequest status_request{};
                    status_request.command = sugar::service::ProtocolCommand::CheckStatus;
                    status_request.session_id = owner_session_id;
                    status_request.job_id = job_id;

                    ParsedProtocolResponse status_response{};
                    std::string local_err;
                    if (!send_tcp_protocol_request(server.port(), status_request, status_response, local_err)) {
                        std::lock_guard lock(poll_mutex);
                        poll_errors.push_back("poller " + std::to_string(i + 1) + ": " + local_err);
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (!status_response.ok || !status_response.has_job_status) {
                        std::lock_guard lock(poll_mutex);
                        poll_errors.push_back(
                            "poller " + std::to_string(i + 1) + ": " +
                            (status_response.message.empty() ? "missing status payload" : status_response.message));
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    {
                        std::lock_guard lock(poll_mutex);
                        polled_states.push_back(status_response.job_state);
                    }
                    if (status_response.job_state == sugar::service::JobState::Validating ||
                        status_response.job_state == sugar::service::JobState::Running ||
                        status_response.job_state == sugar::service::JobState::CancelRequested) {
                        saw_active_state.store(true, std::memory_order_relaxed);
                    }
                    if (status_response.job_state == sugar::service::JobState::Completed ||
                        status_response.job_state == sugar::service::JobState::Rejected ||
                        status_response.job_state == sugar::service::JobState::Failed ||
                        status_response.job_state == sugar::service::JobState::Canceled) {
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    std::this_thread::sleep_for(kTcpPollInterval);
                }
            });
        }
        constexpr std::size_t kResultPollerCount = 2;
        std::vector<std::thread> result_pollers;
        result_pollers.reserve(kResultPollerCount);
        for (std::size_t i = 0; i < kResultPollerCount; ++i) {
            result_pollers.emplace_back([&, i] {
                using namespace std::chrono_literals;

                const auto deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
                while (!stop_pollers.load(std::memory_order_relaxed) &&
                    std::chrono::steady_clock::now() < deadline) {
                    sugar::service::ProtocolRequest result_request{};
                    result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                    result_request.session_id = owner_session_id;
                    result_request.job_id = job_id;

                    ParsedProtocolResponse result_response{};
                    std::string local_err;
                    if (!send_tcp_protocol_request(server.port(), result_request, result_response, local_err)) {
                        std::lock_guard lock(poll_mutex);
                        result_poll_errors.push_back("result poller " + std::to_string(i + 1) + ": " + local_err);
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    {
                        std::lock_guard lock(poll_mutex);
                        ++result_poll_count;
                    }

                    if (result_response.ok ||
                        result_response.error_code != sugar::service::ProtocolErrorCode::InvalidState) {
                        std::lock_guard lock(poll_mutex);
                        result_poll_errors.push_back(
                            "result poller " + std::to_string(i + 1) + ": unexpected result response");
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    std::this_thread::sleep_for(kTcpPollInterval);
                }
            });
        }

        const auto active_deadline = std::chrono::steady_clock::now() + kTcpCancelActiveWaitTimeout;
        while (!saw_active_state.load(std::memory_order_relaxed) &&
            !stop_pollers.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() < active_deadline) {
            std::this_thread::sleep_for(kTcpWaitInterval);
        }
        t.expect(saw_active_state.load(std::memory_order_relaxed),
            "live TCP cancel-polling path exposes validating/running state before cancellation");

        sugar::service::ProtocolRequest cancel_request{};
        cancel_request.command = sugar::service::ProtocolCommand::CancelJob;
        cancel_request.session_id = owner_session_id;
        cancel_request.job_id = job_id;

        ParsedProtocolResponse cancel_response{};
        err.clear();
        const bool cancel_replied = send_tcp_protocol_request(server.port(), cancel_request, cancel_response, err);
        t.expect(cancel_replied, "live TCP cancel-polling path returns a structured reply for cancel");
        t.expect(cancel_replied && cancel_response.ok, "live TCP cancel-polling path accepts owner cancellation");

        const auto cancel_deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
        while (!stop_pollers.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() < cancel_deadline) {
            std::this_thread::sleep_for(kTcpWaitInterval);
        }
        stop_pollers.store(true, std::memory_order_relaxed);
        for (auto& poller : pollers) {
            poller.join();
        }
        for (auto& poller : result_pollers) {
            poller.join();
        }

        bool pollers_clean = true;
        {
            std::lock_guard lock(poll_mutex);
            pollers_clean = poll_errors.empty() && result_poll_errors.empty();
        }
        t.expect(pollers_clean, "live TCP cancel-polling path keeps repeated status and result polling stable under cancellation");
        if (!pollers_clean) {
            std::lock_guard lock(poll_mutex);
            for (const auto& poll_error : poll_errors) {
                t.out << "  poll error: " << poll_error << "\n";
            }
            for (const auto& poll_error : result_poll_errors) {
                t.out << "  result poll error: " << poll_error << "\n";
            }
        }

        std::size_t observed_poll_count = 0;
        bool saw_cancel_requested_state = false;
        bool saw_canceled_state = false;
        {
            std::lock_guard lock(poll_mutex);
            observed_poll_count = polled_states.size();
            for (const auto state : polled_states) {
                saw_cancel_requested_state =
                    saw_cancel_requested_state || state == sugar::service::JobState::CancelRequested;
                saw_canceled_state = saw_canceled_state || state == sugar::service::JobState::Canceled;
            }
        }
        t.expect(observed_poll_count >= kPollerCount,
            "live TCP cancel-polling path performs repeated status reads instead of a single terminal fetch");
        t.expect(result_poll_count >= kResultPollerCount,
            "live TCP cancel-polling path performs repeated result reads while cancellation is in flight");

        sugar::service::JobState final_state = sugar::service::JobState::Queued;
        bool saw_final_active_state = false;
        std::string final_failure;
        std::size_t final_poll_count = 0;
        err.clear();
        const bool reached_terminal = wait_for_terminal_status_over_tcp(
            server.port(),
            owner_session_id,
            job_id,
            final_state,
            saw_final_active_state,
            final_failure,
            final_poll_count,
            err);
        t.expect(reached_terminal, "live TCP cancel-polling path reaches a terminal state after cancellation");
        t.expect(reached_terminal && (final_poll_count > 0 || saw_final_active_state),
            "live TCP cancel-polling path can still query terminal owner status after cancellation");
        t.expect(reached_terminal && final_state == sugar::service::JobState::Canceled,
            "live TCP cancel-polling path terminates the owner job as canceled");
        if (reached_terminal && final_state != sugar::service::JobState::Canceled && !final_failure.empty()) {
            t.out << "  final failure detail: " << final_failure << "\n";
        }
        t.expect(saw_cancel_requested_state || saw_canceled_state,
            "live TCP cancel-polling path exposes cancel-related states during repeated polling");

        {
            std::vector<std::string> compact_status_args{
                "sugar_client",
                "status",
                "--session",
                owner_session_id,
                "--job",
                job_id,
                "--port",
                std::to_string(server.port()),
                "--compact",
            };
            std::vector<char*> compact_status_argv;
            compact_status_argv.reserve(compact_status_args.size());
            for (auto& arg : compact_status_args) {
                compact_status_argv.push_back(arg.data());
            }
            std::ostringstream compact_status_out;
            std::ostringstream compact_status_err;
            const int compact_status_rc = sugar::service::run_tcp_client_from_cli(
                static_cast<int>(compact_status_argv.size()),
                compact_status_argv.data(),
                compact_status_out,
                compact_status_err);
            const std::string compact_status_text = compact_status_out.str();
            t.expect(compact_status_rc == 0,
                "sugar_client status --compact succeeds for a terminal canceled job");
            t.expect(compact_status_err.str().empty(),
                "sugar_client status --compact keeps raw progress chatter off stderr");
            t.expect(
                compact_status_text.find("Hosted compact status:") != std::string::npos &&
                    compact_status_text.find("progress:") != std::string::npos &&
                    compact_status_text.find("retention:") != std::string::npos &&
                    compact_status_text.find("no_result_reason=canceled") != std::string::npos,
                "sugar_client status --compact prints progress and no-result retention facts");
            t.expect(compact_status_text.find("\"job_status\"") == std::string::npos,
                "sugar_client status --compact avoids dumping raw hosted JSON");
        }

        sugar::service::ProtocolRequest result_request{};
        result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
        result_request.session_id = owner_session_id;
        result_request.job_id = job_id;
        ParsedProtocolResponse canceled_result_response{};
        err.clear();
        const bool canceled_result_replied =
            send_tcp_protocol_request(server.port(), result_request, canceled_result_response, err);
        t.expect(canceled_result_replied, "live TCP cancel-polling path returns a structured reply for canceled result retrieval");
        t.expect(
            canceled_result_replied &&
                !canceled_result_response.ok &&
                canceled_result_response.message.find("not available") != std::string::npos,
            "live TCP cancel-polling path leaves canceled result retrieval explicitly unavailable");

        err.clear();
        const bool disconnected_owner = disconnect_session(server.port(), owner_session_id, err);
        const bool disconnected_intruder = disconnect_session(server.port(), intruder_session_id, err);
        t.expect(disconnected_owner && disconnected_intruder,
            "live TCP cancel-polling path disconnects both owner and intruder sessions cleanly");

        server.stop();
        t.expect(server.exit_code() == 0, "live TCP cancel-polling server stops cleanly after cancel traffic");
        if (server.exit_code() != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
        }

        return true;
    }


} // namespace sugar::contract_suite_internal
