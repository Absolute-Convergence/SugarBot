#include "contract_suite_internal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

#include "service/server_runtime.h"

namespace sugar::contract_suite_internal {

    namespace {
        constexpr auto kStatusPollInterval = std::chrono::milliseconds(1);
        constexpr auto kTerminalWaitTimeout = std::chrono::seconds(10);
        constexpr auto kMultiTerminalWaitTimeout = std::chrono::seconds(20);

        sugar::service::RuntimeConfig make_service_runtime_config(
            const std::filesystem::path& root,
            std::size_t max_sweep_candidate_count,
            std::size_t worker_limit,
            std::size_t max_concurrent_jobs_per_session = 2,
            std::size_t max_total_active_jobs = 0) {
            sugar::service::RuntimeConfig config{};
            config.storage_root = root;
            config.quotas.max_upload_size_bytes = 64 * 1024;
            config.quotas.max_parsed_row_count = 256;
            config.quotas.max_sweep_candidate_count = max_sweep_candidate_count;
            config.quotas.max_live_sessions = 32;
            config.quotas.max_concurrent_jobs_per_session = max_concurrent_jobs_per_session;
            config.quotas.max_total_active_jobs = max_total_active_jobs;
            config.quotas.max_retained_result_bytes = 64 * 1024;
            config.quotas.max_retained_results = 64;
            config.quotas.server_worker_limit = worker_limit;
            return config;
        }

        std::string single_series_layout_json() {
            return
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"row_id\",\n"
                "    \"time_kind\": \"Index\",\n"
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

        std::string single_series_sweep_logic_json(std::size_t max_evals) {
            std::ostringstream out;
            out
                << "{\n"
                << "  \"version\": 1,\n"
                << "  \"name\": \"service_phase4_sweep\",\n"
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

        std::string single_series_csv_text(std::size_t rows = 3) {
            std::ostringstream out;
            out << "row_id,close\n";
            for (std::size_t i = 0; i < rows; ++i) {
                out << (1 + i)
                    << ","
                    << (100.0 + static_cast<double>(i))
                    << "\n";
            }
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

        sugar::service::SubmitRequest make_sweep_request(std::size_t max_evals, std::size_t rows) {
            sugar::service::SubmitRequest request{};
            request.intent = sugar::service::JobIntent::Execute;
            request.layout.client_name = "layout.json";
            request.layout.content = single_series_layout_json();
            request.logic.client_name = "logic.json";
            request.logic.content = single_series_sweep_logic_json(max_evals);
            request.data.client_name = "data.csv";
            request.data.content = single_series_csv_text(rows);
            return request;
        }

        bool wait_for_terminal_status(
            sugar::service::ServerRuntime& runtime,
            const std::string& session_id,
            const std::string& job_id,
            sugar::service::JobStatus& out_status,
            bool& out_saw_parallel_activity) {
            using sugar::service::JobState;

            std::string err;
            const auto deadline = std::chrono::steady_clock::now() + kTerminalWaitTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!runtime.get_job_status(session_id, job_id, out_status, err)) {
                    return false;
                }
                if (out_status.state == JobState::Validating ||
                    out_status.state == JobState::Running ||
                    out_status.state == JobState::CancelRequested) {
                    out_saw_parallel_activity = true;
                }
                if (out_status.state == JobState::Completed ||
                    out_status.state == JobState::Rejected ||
                    out_status.state == JobState::Failed ||
                    out_status.state == JobState::Canceled) {
                    return true;
                }
                std::this_thread::sleep_for(kStatusPollInterval);
            }
            return false;
        }

        bool wait_for_cancellable_status(
            sugar::service::ServerRuntime& runtime,
            const std::string& session_id,
            const std::string& job_id,
            sugar::service::JobStatus& out_status) {
            using sugar::service::JobState;

            std::string err;
            const auto deadline = std::chrono::steady_clock::now() + kTerminalWaitTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!runtime.get_job_status(session_id, job_id, out_status, err)) {
                    return false;
                }
                if (out_status.state == JobState::Validating ||
                    out_status.state == JobState::Running ||
                    out_status.state == JobState::CancelRequested) {
                    return true;
                }
                if (out_status.state == JobState::Completed ||
                    out_status.state == JobState::Rejected ||
                    out_status.state == JobState::Failed ||
                    out_status.state == JobState::Canceled) {
                    return false;
                }
                std::this_thread::sleep_for(kStatusPollInterval);
            }
            return false;
        }

        bool wait_for_terminal_statuses(
            sugar::service::ServerRuntime& runtime,
            const std::vector<sugar::service::SessionInfo>& sessions,
            const std::vector<std::vector<std::string>>& job_ids,
            bool& out_saw_parallel_activity,
            std::vector<std::vector<sugar::service::JobStatus>>& out_statuses) {
            out_statuses.assign(sessions.size(), {});
            for (std::size_t i = 0; i < sessions.size(); ++i) {
                out_statuses[i].resize(job_ids[i].size());
            }

            std::size_t remaining = 0;
            for (const auto& by_session : job_ids) {
                remaining += by_session.size();
            }

            const auto deadline = std::chrono::steady_clock::now() + kMultiTerminalWaitTimeout;
            std::vector<std::vector<bool>> done(sessions.size());
            for (std::size_t i = 0; i < sessions.size(); ++i) {
                done[i].assign(job_ids[i].size(), false);
            }

            std::string err;
            while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
                std::size_t active_jobs_in_pass = 0;
                for (std::size_t i = 0; i < sessions.size(); ++i) {
                    for (std::size_t j = 0; j < job_ids[i].size(); ++j) {
                        if (done[i][j]) {
                            continue;
                        }

                        if (!runtime.get_job_status(
                                sessions[i].session_id,
                                job_ids[i][j],
                                out_statuses[i][j],
                                err)) {
                            return false;
                        }

                        const auto state = out_statuses[i][j].state;
                        if (state == sugar::service::JobState::Validating ||
                            state == sugar::service::JobState::Running ||
                            state == sugar::service::JobState::CancelRequested) {
                            ++active_jobs_in_pass;
                        }
                        if (state == sugar::service::JobState::Completed ||
                            state == sugar::service::JobState::Rejected ||
                            state == sugar::service::JobState::Failed ||
                            state == sugar::service::JobState::Canceled) {
                            done[i][j] = true;
                            --remaining;
                        }
                    }
                }
                out_saw_parallel_activity = out_saw_parallel_activity || active_jobs_in_pass > 1;
                if (remaining > 0) {
                    std::this_thread::sleep_for(kStatusPollInterval);
                }
            }
            return remaining == 0;
        }

    } // namespace

    bool test_service_scheduler_fairness_contracts(ContractAsserts& t) {
        t.out << "[service_scheduler_fairness] per-session fair queue ordering\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_scheduler_fairness"),
            64,
            0));

        sugar::service::SessionInfo first{};
        sugar::service::SessionInfo second{};
        std::string err;
        if (!runtime.create_session(first, err) || !runtime.create_session(second, err)) {
            return t.expect(false, "service runtime creates sessions for fairness test");
        }

        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        std::string first_job_a;
        std::string first_job_b;
        std::string second_job;
        const bool submitted_first_a = runtime.submit_job(first.session_id, request, first_job_a, err);
        const bool submitted_first_b = runtime.submit_job(first.session_id, request, first_job_b, err);
        const bool submitted_second = runtime.submit_job(second.session_id, request, second_job, err);
        t.expect(submitted_first_a && submitted_first_b && submitted_second,
            "service fairness test stages queued jobs for two sessions");
        if (!submitted_first_a || !submitted_first_b || !submitted_second) {
            return true;
        }

        std::string processed_one;
        std::string processed_two;
        std::string processed_three;
        const bool processed_first = runtime.process_next_queued_job(processed_one, err);
        const bool processed_second_job = runtime.process_next_queued_job(processed_two, err);
        const bool processed_third = runtime.process_next_queued_job(processed_three, err);
        t.expect(processed_first && processed_second_job && processed_third,
            "service fairness test can drain queued jobs deterministically");
        t.expect(processed_first && processed_one == first_job_a,
            "service scheduler keeps the first queued job first");
        t.expect(processed_second_job && processed_two == second_job,
            "service scheduler rotates to the next session before serving a second job from the first session");
        t.expect(processed_third && processed_three == first_job_b,
            "service scheduler returns to the original session after fair rotation");

        return true;
    }

    bool test_service_background_worker_pool_contracts(ContractAsserts& t) {
        t.out << "[service_background_worker_pool] bounded background workers execute concurrent jobs\n";

        constexpr std::size_t kWorkerLimit = 2;
        constexpr std::size_t kSweepEvals = 2048;
        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_background_worker_pool"),
            kSweepEvals,
            kWorkerLimit));

        std::vector<sugar::service::SessionInfo> sessions(3);
        std::string err;
        for (auto& session : sessions) {
            if (!runtime.create_session(session, err)) {
                return t.expect(false, "service runtime creates sessions for worker-pool test");
            }
        }

        const auto request = make_sweep_request(kSweepEvals, 128);
        std::vector<std::string> job_ids(sessions.size());
        std::vector<std::string> submit_errors(sessions.size());
        std::vector<std::thread> submitters;
        submitters.reserve(sessions.size());
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            submitters.emplace_back([&, i] {
                (void)runtime.submit_job(sessions[i].session_id, request, job_ids[i], submit_errors[i]);
            });
        }
        for (auto& thread : submitters) {
            thread.join();
        }

        bool all_submitted = true;
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            all_submitted = all_submitted &&
                !job_ids[i].empty() &&
                submit_errors[i].empty();
        }
        t.expect(all_submitted, "service background worker pool accepts concurrent submissions");
        if (!all_submitted) {
            return true;
        }

        std::vector<std::vector<std::string>> grouped_job_ids(sessions.size());
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            grouped_job_ids[i].push_back(job_ids[i]);
        }

        bool saw_parallel_activity = false;
        std::vector<std::vector<sugar::service::JobStatus>> statuses;
        const bool all_completed = wait_for_terminal_statuses(
            runtime,
            sessions,
            grouped_job_ids,
            saw_parallel_activity,
            statuses);
        t.expect(all_completed, "service background worker pool reaches terminal states for submitted jobs");
        for (std::size_t i = 0; i < sessions.size() && all_completed; ++i) {
            const auto& status = statuses[i].front();
            if (status.state != sugar::service::JobState::Completed) {
                t.out << "  observed terminal state for " << job_ids[i]
                    << ": " << sugar::service::job_state_name(status.state)
                    << " | detail=" << status.failure_message << "\n";
            }
            t.expect(status.state == sugar::service::JobState::Completed,
                "service background worker pool completes submitted jobs without manual processing");
        }
        t.expect(saw_parallel_activity,
            "service background worker pool exposes overlapping active execution while jobs are still in flight");

        for (std::size_t i = 0; i < sessions.size(); ++i) {
            sugar::service::JobResult result{};
            err.clear();
            const bool has_result = runtime.get_job_result(sessions[i].session_id, job_ids[i], result, err);
            t.expect(has_result, "service background worker pool retains owned result payloads");
            if (has_result && !result.metadata.primary_backtest.has_value()) {
                t.out << "  result for " << job_ids[i]
                    << " has no primary_backtest; summary follows:\n"
                    << result.summary_text << "\n";
            }
            t.expect(has_result && result.metadata.primary_backtest.has_value(),
                "service background worker pool preserves machine-readable execution results");
            t.expect(has_result && result.metadata.sweep_report.has_value(),
                "service background worker pool preserves machine-readable sweep reports");
        }

        return true;
    }

    bool test_service_background_worker_cancellation_contracts(ContractAsserts& t) {
        t.out << "[service_background_worker_cancellation] worker-backed cancellation reaches canceled terminal state\n";

        constexpr std::size_t kSweepEvals = 131072;
        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_background_worker_cancellation"),
            kSweepEvals,
            1));

        sugar::service::SessionInfo session{};
        std::string err;
        if (!runtime.create_session(session, err)) {
            return t.expect(false, "service runtime creates session for worker-backed cancellation test");
        }

        const auto request = make_sweep_request(kSweepEvals, 256);
        std::string job_id;
        const bool submitted = runtime.submit_job(session.session_id, request, job_id, err);
        t.expect(submitted, "worker-backed cancellation test submits a long-running sweep job");
        if (!submitted) {
            return true;
        }

        sugar::service::JobStatus active_status{};
        const bool saw_cancellable_state = wait_for_cancellable_status(
            runtime,
            session.session_id,
            job_id,
            active_status);
        t.expect(saw_cancellable_state,
            "worker-backed cancellation test observes an active job before issuing cancellation");
        if (!saw_cancellable_state) {
            return true;
        }

        err.clear();
        const bool canceled = runtime.cancel_job(session.session_id, job_id, err);
        t.expect(canceled, "worker-backed cancellation path accepts cancellation for an active job");
        if (!canceled) {
            return true;
        }

        bool saw_parallel_activity = false;
        sugar::service::JobStatus terminal_status{};
        const bool reached_terminal = wait_for_terminal_status(
            runtime,
            session.session_id,
            job_id,
            terminal_status,
            saw_parallel_activity);
        t.expect(reached_terminal, "worker-backed cancellation path reaches a terminal state");
        t.expect(reached_terminal && terminal_status.state == sugar::service::JobState::Canceled,
            "worker-backed cancellation path terminates the job as canceled");

        const bool has_cancel_requested_event =
            reached_terminal &&
            std::any_of(terminal_status.progress.begin(), terminal_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::CancelRequested;
                });
        t.expect(has_cancel_requested_event,
            "worker-backed cancellation path preserves the cancel_requested transition in progress");

        const bool has_canceled_event =
            reached_terminal &&
            std::any_of(terminal_status.progress.begin(), terminal_status.progress.end(),
                [](const sugar::service::ProgressEvent& event) {
                    return event.state == sugar::service::JobState::Canceled;
                });
        t.expect(has_canceled_event,
            "worker-backed cancellation path preserves the canceled terminal transition in progress");

        sugar::service::JobResult canceled_result{};
        err.clear();
        const bool has_result = runtime.get_job_result(session.session_id, job_id, canceled_result, err);
        t.expect(!has_result, "worker-backed cancellation does not retain a result payload for canceled work");
        t.expect(!has_result && err.find("not available") != std::string::npos,
            "worker-backed cancellation leaves result retrieval explicitly unavailable");

        return true;
    }

    bool test_service_cross_client_isolation_contracts(ContractAsserts& t) {
        t.out << "[service_cross_client_isolation] cross-client status/result isolation under workers\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_cross_client_isolation"),
            512,
            2));

        sugar::service::SessionInfo owner{};
        sugar::service::SessionInfo intruder{};
        std::string err;
        if (!runtime.create_session(owner, err) || !runtime.create_session(intruder, err)) {
            return t.expect(false, "service runtime creates sessions for cross-client isolation test");
        }

        std::string job_id;
        const bool submitted = runtime.submit_job(
            owner.session_id,
            make_sweep_request(512, 96),
            job_id,
            err);
        t.expect(submitted, "service worker-backed path accepts owned submission for isolation test");
        if (!submitted) {
            return true;
        }

        sugar::service::JobStatus foreign_status{};
        err.clear();
        const bool intruder_status = runtime.get_job_status(intruder.session_id, job_id, foreign_status, err);
        t.expect(!intruder_status, "non-owning session cannot query another session job status while workers are active");
        t.expect(!intruder_status && err.find("does not belong") != std::string::npos,
            "cross-client status query fails with explicit ownership error");

        bool saw_parallel_activity = false;
        sugar::service::JobStatus owner_status{};
        const bool completed = wait_for_terminal_status(runtime, owner.session_id, job_id, owner_status, saw_parallel_activity);
        t.expect(completed, "owned job reaches terminal state for cross-client isolation test");
        t.expect(completed && owner_status.state == sugar::service::JobState::Completed,
            "owned job completes normally under workers for cross-client isolation test");

        sugar::service::JobResult foreign_result{};
        err.clear();
        const bool intruder_result = runtime.get_job_result(intruder.session_id, job_id, foreign_result, err);
        t.expect(!intruder_result, "non-owning session cannot fetch another session result after completion");
        t.expect(!intruder_result && err.find("does not belong") != std::string::npos,
            "cross-client result retrieval fails with explicit ownership error");

        return true;
    }

    bool test_service_scheduler_starvation_resistance_contracts(ContractAsserts& t) {
        t.out << "[service_scheduler_starvation_resistance] flood session does not starve smaller session\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_scheduler_starvation_resistance"),
            64,
            0,
            32));

        sugar::service::SessionInfo flood{};
        sugar::service::SessionInfo light{};
        std::string err;
        if (!runtime.create_session(flood, err) || !runtime.create_session(light, err)) {
            return t.expect(false, "service runtime creates sessions for starvation-resistance test");
        }

        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        std::vector<std::string> flood_jobs;
        flood_jobs.reserve(20);
        for (std::size_t i = 0; i < 20; ++i) {
            std::string job_id;
            const bool submitted = runtime.submit_job(flood.session_id, request, job_id, err);
            t.expect(submitted, "flood session submission succeeds while building starvation scenario");
            if (!submitted) {
                return true;
            }
            flood_jobs.push_back(job_id);
        }

        std::string light_job;
        const bool light_submitted = runtime.submit_job(light.session_id, request, light_job, err);
        t.expect(light_submitted, "light session submission succeeds beside flood session");
        if (!light_submitted) {
            return true;
        }

        std::string first_processed;
        std::string second_processed;
        const bool processed_first = runtime.process_next_queued_job(first_processed, err);
        const bool processed_second = runtime.process_next_queued_job(second_processed, err);
        t.expect(processed_first && processed_second,
            "starvation-resistance scenario can drain the first two scheduler slots");
        t.expect(processed_first && first_processed == flood_jobs.front(),
            "flood session keeps the first scheduler slot");
        t.expect(processed_second && second_processed == light_job,
            "light session job is promoted into the second scheduler slot despite flood pressure");

        return true;
    }

    bool test_service_multi_client_stress_contracts(ContractAsserts& t) {
        t.out << "[service_multi_client_stress] higher-load multi-session stress and owned result retrieval\n";

        constexpr std::size_t kSessionCount = 12;
        constexpr std::size_t kJobsPerSession = 4;
        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_multi_client_stress"),
            64,
            4,
            8,
            64));

        std::vector<sugar::service::SessionInfo> sessions(kSessionCount);
        std::string err;
        for (auto& session : sessions) {
            if (!runtime.create_session(session, err)) {
                return t.expect(false, "service runtime creates sessions for multi-client stress test");
            }
        }

        const auto request = make_sweep_request(8, 64);
        std::vector<std::vector<std::string>> job_ids(kSessionCount);
        std::vector<std::vector<std::string>> submit_errors(kSessionCount);
        std::vector<std::thread> submitters;
        submitters.reserve(kSessionCount);
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            submitters.emplace_back([&, i] {
                job_ids[i].resize(kJobsPerSession);
                submit_errors[i].resize(kJobsPerSession);
                for (std::size_t j = 0; j < kJobsPerSession; ++j) {
                    (void)runtime.submit_job(sessions[i].session_id, request, job_ids[i][j], submit_errors[i][j]);
                }
            });
        }
        for (auto& thread : submitters) {
            thread.join();
        }

        bool all_submitted = true;
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            for (std::size_t j = 0; j < kJobsPerSession; ++j) {
                all_submitted = all_submitted &&
                    !job_ids[i][j].empty() &&
                    submit_errors[i][j].empty();
            }
        }
        t.expect(all_submitted, "multi-client stress accepts concurrent submissions across many sessions");
        if (!all_submitted) {
            return true;
        }

        bool saw_parallel_activity = false;
        std::vector<std::vector<sugar::service::JobStatus>> statuses;
        const bool all_completed = wait_for_terminal_statuses(
            runtime,
            sessions,
            job_ids,
            saw_parallel_activity,
            statuses);
        t.expect(all_completed, "multi-client stress reaches terminal states for all submitted jobs");
        t.expect(saw_parallel_activity, "multi-client stress exposes overlapping active execution while jobs are in flight");
        if (!all_completed) {
            return true;
        }

        bool all_completed_successfully = true;
        for (const auto& by_session : statuses) {
            for (const auto& status : by_session) {
                all_completed_successfully =
                    all_completed_successfully &&
                    status.state == sugar::service::JobState::Completed;
            }
        }
        t.expect(all_completed_successfully,
            "multi-client stress completes every submitted job without cross-session state corruption");

        bool all_results_owned = true;
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            for (std::size_t j = 0; j < kJobsPerSession; ++j) {
                sugar::service::JobResult result{};
                err.clear();
                const bool has_result = runtime.get_job_result(sessions[i].session_id, job_ids[i][j], result, err);
                all_results_owned =
                    all_results_owned &&
                    has_result &&
                    result.owner_session_id == sessions[i].session_id &&
                    result.metadata.primary_backtest.has_value() &&
                    result.metadata.sweep_report.has_value();
            }
        }
        t.expect(all_results_owned,
            "multi-client stress preserves owned machine-readable sweep results for every completed job");

        return true;
    }

    bool test_service_server_backpressure_contracts(ContractAsserts& t) {
        t.out << "[service_server_backpressure] server-wide active-job backpressure and recovery\n";

        sugar::service::ServerRuntime runtime(make_service_runtime_config(
            contract_tmp_case_dir("service_server_backpressure"),
            64,
            0,
            4,
            2));

        sugar::service::SessionInfo first{};
        sugar::service::SessionInfo second{};
        std::string err;
        if (!runtime.create_session(first, err) || !runtime.create_session(second, err)) {
            return t.expect(false, "service runtime creates sessions for backpressure test");
        }

        const auto request = make_single_series_request(sugar::service::JobIntent::Execute);
        std::string first_job;
        std::string second_job;
        std::string blocked_job;
        const bool first_submitted = runtime.submit_job(first.session_id, request, first_job, err);
        const bool second_submitted = runtime.submit_job(second.session_id, request, second_job, err);
        t.expect(first_submitted && second_submitted,
            "server-wide backpressure allows submissions up to the configured active-job cap");
        if (!first_submitted || !second_submitted) {
            return true;
        }

        err.clear();
        const bool blocked = runtime.submit_job(first.session_id, request, blocked_job, err);
        t.expect(!blocked, "server-wide backpressure rejects submissions beyond the configured active-job cap");
        t.expect(!blocked && err.find("server exceeded max active jobs") != std::string::npos,
            "server-wide backpressure exposes an explicit saturation error");

        std::string processed_job;
        err.clear();
        const bool processed = runtime.process_next_queued_job(processed_job, err);
        t.expect(processed, "backpressure scenario can retire one queued job to free capacity");

        err.clear();
        const bool unblocked = runtime.submit_job(first.session_id, request, blocked_job, err);
        t.expect(unblocked, "server-wide backpressure allows new submissions again after capacity is freed");

        return true;
    }

} // namespace sugar::contract_suite_internal
