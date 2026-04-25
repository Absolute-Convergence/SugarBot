#include "service/server_runtime.h"

#include <algorithm>
#include <optional>
#include <sstream>

#include "sweep/metrics.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_sweep.h"

namespace sugar::service {

    namespace {
        struct ProgressCallbackState {
            void record_failure(std::string detail) {
                std::lock_guard lock(mutex);
                if (!first_failure.has_value()) {
                    first_failure = std::move(detail);
                }
            }

            std::optional<std::string> get_failure() const {
                std::lock_guard lock(mutex);
                return first_failure;
            }

            mutable std::mutex mutex{};
            std::optional<std::string> first_failure{};
        };

        std::string append_secondary_detail(
            std::string_view primary,
            std::string_view secondary_label,
            std::string_view secondary_detail) {
            if (secondary_detail.empty()) {
                return std::string(primary);
            }
            if (primary.empty()) {
                return std::string(secondary_label) + ": " + std::string(secondary_detail);
            }
            return std::string(primary) + " [additional service error: " +
                std::string(secondary_label) + ": " + std::string(secondary_detail) + "]";
        }

        std::string append_progress_callback_detail(
            std::string_view detail,
            const std::shared_ptr<ProgressCallbackState>& progress_state) {
            if (!progress_state) {
                return std::string(detail);
            }
            if (const auto progress_failure = progress_state->get_failure();
                progress_failure.has_value()) {
                return append_secondary_detail(
                    detail,
                    "progress recording failed",
                    *progress_failure);
            }
            return std::string(detail);
        }

        std::string make_validation_summary(
            const ValidationMetadata& metadata,
            const std::string& title,
            const std::string* detail = nullptr) {
            std::ostringstream out;
            out << title << "\n"
                << "resolved_mode: " << resolved_execution_mode_name(metadata.resolved_mode) << "\n"
                << "series_count: " << metadata.series_count << "\n"
                << "row_count: " << metadata.row_count << "\n"
                << "input_warnings: " << metadata.input_warning_count << "\n"
                << "incompatible_nodes: " << metadata.incompatible_nodes << "\n"
                << "incompatible_bindings: " << metadata.incompatible_bindings << "\n"
                << "sweep_candidates: " << metadata.sweep_candidate_count << "\n"
                << "estimated_sweep_minutes: " << metadata.estimated_sweep_minutes << "\n";
            if (metadata.long_sweep_warning) {
                out << "warning: sweep candidate count exceeds the hosted warning threshold\n";
            }
            if (detail && !detail->empty()) {
                out << "detail: " << *detail << "\n";
            }
            return out.str();
        }

        std::string make_primary_run_summary(
            const ValidationMetadata& metadata,
            const BacktestResult& result,
            const RunProvenanceSummary* provenance = nullptr) {
            std::ostringstream out;
            out << make_validation_summary(metadata, "Execution completed");
            print_run_report_summary(build_run_report(result, nullptr, provenance), out);
            return out.str();
        }

        std::string make_panel_summary(
            const ValidationMetadata& metadata,
            const PanelResult& result,
            const RunProvenanceSummary* provenance = nullptr) {
            const PanelReport report = build_panel_report(result, provenance);
            std::ostringstream out;
            out << make_validation_summary(metadata, "Panel execution completed");
            out << "entities_total: " << report.entities_total << "\n"
                << "entities_ok: " << report.entities_ok << "\n"
                << "entities_failed: " << report.entities_failed << "\n"
                << "topk_count: " << report.top_rows.size() << "\n"
                << "ranking_objective: " << describe_ranking_objective(report.resolved_objective) << "\n";
            print_run_provenance_summary(report.run_provenance, out);
            if (!report.top_rows.empty()) {
                out << "top_entity: " << report.top_rows.front().entity_key << "\n"
                    << "top_series: " << report.top_rows.front().series_key << "\n"
                    << "top_ranking_score: " << report.top_rows.front().ranking_score << "\n"
                    << "top_objective_rankability: "
                    << run_rankability_state_name(report.top_rows.front().run.objective.rankability) << "\n";
            }
            return out.str();
        }

        std::optional<ArtifactRecord> find_artifact(
            const JobStatus& job,
            ArtifactKind kind) {
            const auto it = std::find_if(
                job.staged_artifacts.begin(),
                job.staged_artifacts.end(),
                [kind](const ArtifactRecord& artifact) {
                    return artifact.kind == kind;
                });
            if (it != job.staged_artifacts.end()) {
                return *it;
            }
            return std::nullopt;
        }
    } // namespace

    ServerRuntime::ServerRuntime(RuntimeConfig config)
        : config_(std::move(config)),
          sessions_(config_.quotas),
          artifact_store_(config_.storage_root, config_.quotas),
          results_(config_.quotas) {
        if (config_.quotas.server_worker_limit > 0) {
            workers_.start(
                config_.quotas.server_worker_limit,
                scheduler_,
                [this](const std::string& job_id) {
                    std::string ignored_err;
                    (void)process_job(job_id, ignored_err);
                });
        }
    }

    ServerRuntime::~ServerRuntime() {
        workers_.stop();
    }

    bool ServerRuntime::create_session(SessionInfo& out, std::string& err) {
        if (!ensure_initialized(err)) {
            return false;
        }
        return sessions_.create_session(out, err);
    }

    bool ServerRuntime::get_session_info(const std::string& session_id, SessionInfo& out, std::string& err) const {
        return sessions_.get_session(session_id, out, err);
    }

    bool ServerRuntime::close_session(const std::string& session_id, std::string& err) {
        if (!ensure_initialized(err)) {
            return false;
        }
        return sessions_.close_session(session_id, err);
    }

    bool ServerRuntime::submit_job(
        const std::string& session_id,
        const SubmitRequest& request,
        std::string& out_job_id,
        ValidationMetadata* out_preflight_metadata,
        std::string& err) {
        out_job_id.clear();
        if (out_preflight_metadata) {
            *out_preflight_metadata = ValidationMetadata{};
        }
        if (!ensure_initialized(err)) {
            return false;
        }
        if (!sessions_.ensure_session_exists(session_id, err)) {
            return false;
        }
        ValidationMetadata preflight_metadata{};
        if (!validate_submit_request_shape(request, config_.quotas, &preflight_metadata, err)) {
            return false;
        }
        if (out_preflight_metadata) {
            *out_preflight_metadata = preflight_metadata;
        }
        if (!sessions_.begin_job(session_id, err)) {
            return false;
        }

        out_job_id = jobs_.next_job_id();

        std::filesystem::path workspace{};
        if (!artifact_store_.create_job_workspace(out_job_id, workspace, err)) {
            sessions_.finish_job(session_id);
            return false;
        }

        ArtifactRecord layout_artifact{};
        ArtifactRecord logic_artifact{};
        ArtifactRecord data_artifact{};
        if (!artifact_store_.stage_input_artifact(
                out_job_id,
                ArtifactKind::LayoutSpec,
                request.layout.content,
                layout_artifact,
                err) ||
            !artifact_store_.stage_input_artifact(
                out_job_id,
                ArtifactKind::LogicSpec,
                request.logic.content,
                logic_artifact,
                err) ||
            !artifact_store_.stage_input_artifact(
                out_job_id,
                ArtifactKind::DataCsv,
                request.data.content,
                data_artifact,
                err)) {
            std::string cleanup_err;
            artifact_store_.cleanup_job_workspace(out_job_id, cleanup_err);
            sessions_.finish_job(session_id);
            return false;
        }

        if (!jobs_.create_job(
                out_job_id,
                session_id,
                request,
                { layout_artifact, logic_artifact, data_artifact },
                config_.quotas.max_total_active_jobs,
                err)) {
            std::string cleanup_err;
            artifact_store_.cleanup_job_workspace(out_job_id, cleanup_err);
            sessions_.finish_job(session_id);
            return false;
        }

        scheduler_.enqueue(session_id, out_job_id);
        err.clear();
        return true;
    }

    bool ServerRuntime::process_next_queued_job(std::string& out_job_id, std::string& err) {
        out_job_id.clear();
        if (!ensure_initialized(err)) {
            return false;
        }
        if (!scheduler_.dequeue_next(out_job_id)) {
            err = "no queued jobs are available";
            return false;
        }
        return process_job(out_job_id, err);
    }

    bool ServerRuntime::process_job(const std::string& job_id, std::string& err) {
        if (!ensure_initialized(err)) {
            return false;
        }

        scheduler_.remove(job_id);

        JobManager::JobExecutionContext job{};
        bool started = false;
        if (!jobs_.begin_execution(job_id, job, started, err)) {
            return false;
        }

        const std::string owner_session_id = job.status.owner_session_id;
        if (!started) {
            err.clear();
            return true;
        }
        if (job.cancel_requested && job.cancel_requested->load()) {
            return finalize_cancellation(owner_session_id, job_id, "job canceled before execution began", err);
        }

        const auto layout_artifact = find_artifact(job.status, ArtifactKind::LayoutSpec);
        const auto logic_artifact = find_artifact(job.status, ArtifactKind::LogicSpec);
        const auto data_artifact = find_artifact(job.status, ArtifactKind::DataCsv);
        if (!layout_artifact.has_value() || !logic_artifact.has_value() || !data_artifact.has_value()) {
            return finalize_failure(
                owner_session_id,
                job_id,
                ValidationMetadata{},
                "job is missing staged input artifacts");
        }

        ValidatedRequest validated{};
        std::string validation_err;
        if (!validate_staged_request(
                layout_artifact->storage_path,
                logic_artifact->storage_path,
                data_artifact->storage_path,
                config_.quotas,
                validated,
                validation_err)) {
            return finalize_rejection(owner_session_id, job_id, validated, validation_err);
        }

        if (!jobs_.record_progress(job_id, JobState::Validating, "validation completed", 0, 0, err)) {
            return false;
        }
        if (job.cancel_requested && job.cancel_requested->load()) {
            return finalize_cancellation(owner_session_id, job_id, "job canceled after validation", err);
        }

        if (job.request.intent == JobIntent::ValidateOnly) {
            JobResult result{};
            result.result_id = jobs_.next_result_id();
            result.owner_session_id = owner_session_id;
            result.job_id = job_id;
            result.metadata.validation = validated.metadata;
            result.metadata.validation.resolved_mode = ResolvedExecutionMode::ValidationOnly;
            result.summary_text = make_validation_summary(validated.metadata, "Validation completed");

            ArtifactRecord summary_artifact{};
            std::string artifact_err;
            if (artifact_store_.write_output_artifact(job_id, "validation", result.summary_text, summary_artifact, artifact_err)) {
                result.output_artifacts.push_back(summary_artifact);
            }
            return finalize_completion(owner_session_id, job_id, std::move(result), err);
        }

        if (!jobs_.record_progress(job_id, JobState::Running, "job started", 0, 0, err)) {
            return false;
        }

        PlanSweepRuntimeOptions runtime_options{};
        const auto progress_state = std::make_shared<ProgressCallbackState>();
        runtime_options.cancel_requested = [cancel = job.cancel_requested]() {
            return cancel && cancel->load();
        };
        runtime_options.progress_cb = [this, job_id, progress_state](const SweepProgress& progress) {
            std::string progress_err;
            std::string progress_message = "execution progress";
            const auto stage_name = sweep_progress_stage_name(progress.stage);
            if (!stage_name.empty()) {
                progress_message += " (";
                progress_message += stage_name;
                progress_message += ")";
            }
            if (!jobs_.record_progress(
                job_id,
                JobState::Running,
                progress_message,
                progress.done,
                progress.total,
                progress_err)) {
                progress_state->record_failure(
                    progress_err.empty() ? "unknown progress update failure" : progress_err);
            }
        };

        if (validated.runtime.has_value()) {
            const DataView dv(*validated.runtime->aligned_bundle);
            const auto run_provenance =
                build_run_provenance_summary(validated.runtime->run_manifest);
            BacktestResult backtest{};
            std::ostringstream run_log;
            if (!run_custom_plan_spec(
                    dv,
                    validated.runtime->primary_ref,
                    validated.plan,
                    runtime_options,
                    run_log,
                    &backtest,
                    nullptr,
                    &run_provenance)) {
                const std::string detail = append_progress_callback_detail(
                    run_log.str().empty() ? "execution failed" : run_log.str(),
                    progress_state);
                if (runtime_options.cancel_requested && runtime_options.cancel_requested()) {
                    return finalize_cancellation(owner_session_id, job_id, detail, err);
                }
                return finalize_failure(owner_session_id, job_id, validated.metadata, detail);
            }
            if (const auto progress_failure = progress_state->get_failure();
                progress_failure.has_value()) {
                return finalize_failure(
                    owner_session_id,
                    job_id,
                    validated.metadata,
                    append_secondary_detail(
                        "execution completed",
                        "progress recording failed",
                        *progress_failure));
            }

            JobResult result{};
            result.result_id = jobs_.next_result_id();
            result.owner_session_id = owner_session_id;
            result.job_id = job_id;
            result.metadata.validation = validated.metadata;
            result.metadata.primary_backtest = backtest;
            result.metadata.run_provenance = run_provenance;
            result.summary_text = make_primary_run_summary(
                validated.metadata,
                backtest,
                &run_provenance);

            ArtifactRecord summary_artifact{};
            std::string artifact_err;
            if (artifact_store_.write_output_artifact(job_id, "run", result.summary_text, summary_artifact, artifact_err)) {
                result.output_artifacts.push_back(summary_artifact);
            }
            return finalize_completion(owner_session_id, job_id, std::move(result), err);
        }

        if (!validated.primary_col_id.has_value() || !validated.top_k.has_value() || !validated.ranking.has_value()) {
            return finalize_failure(
                owner_session_id,
                job_id,
                validated.metadata,
                "validated panel request is missing execution metadata");
        }

        const DataView dv(*validated.world.aligned_bundle);
        const auto panel_provenance =
            build_run_provenance_summary(validated.world.run_manifest);
        const auto panel = run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            validated.plan,
            runtime_options,
            *validated.primary_col_id,
            *validated.top_k,
            *validated.ranking);

        if (panel.canceled) {
            const std::string detail = append_progress_callback_detail(
                panel.cancel_message.empty() ? "panel execution canceled" : panel.cancel_message,
                progress_state);
            return finalize_cancellation(owner_session_id, job_id, detail, err);
        }
        if (const auto progress_failure = progress_state->get_failure();
            progress_failure.has_value()) {
            return finalize_failure(
                owner_session_id,
                job_id,
                validated.metadata,
                append_secondary_detail(
                    "panel execution completed",
                    "progress recording failed",
                    *progress_failure));
        }
        if (panel.entities_ok == 0) {
            return finalize_failure(
                owner_session_id,
                job_id,
                validated.metadata,
                append_progress_callback_detail(
                    "panel execution produced no successful entity results",
                    progress_state));
        }

        JobResult result{};
        result.result_id = jobs_.next_result_id();
        result.owner_session_id = owner_session_id;
        result.job_id = job_id;
        result.metadata.validation = validated.metadata;
        result.metadata.panel = panel;
        result.metadata.run_provenance = panel_provenance;
        result.summary_text = make_panel_summary(validated.metadata, panel, &panel_provenance);

        ArtifactRecord summary_artifact{};
        std::string artifact_err;
        if (artifact_store_.write_output_artifact(job_id, "panel", result.summary_text, summary_artifact, artifact_err)) {
            result.output_artifacts.push_back(summary_artifact);
        }
        return finalize_completion(owner_session_id, job_id, std::move(result), err);
    }

    bool ServerRuntime::cancel_job(const std::string& session_id, const std::string& job_id, std::string& err) {
        if (!ensure_initialized(err)) {
            return false;
        }
        if (!ensure_owned_job(session_id, job_id, err)) {
            return false;
        }

        JobState canceled_state = JobState::Queued;
        if (!jobs_.request_cancel(session_id, job_id, canceled_state, err)) {
            return false;
        }
        if (canceled_state == JobState::Canceled) {
            scheduler_.remove(job_id);
            sessions_.finish_job(session_id);
        }
        err.clear();
        return true;
    }

    bool ServerRuntime::get_job_status(
        const std::string& session_id,
        const std::string& job_id,
        JobStatus& out,
        std::string& err) const {
        if (!ensure_owned_job(session_id, job_id, err)) {
            return false;
        }
        return jobs_.get_owned_status(session_id, job_id, out, err);
    }

    bool ServerRuntime::get_job_result(
        const std::string& session_id,
        const std::string& job_id,
        JobResult& out,
        std::string& err) const {
        if (!ensure_owned_job(session_id, job_id, err)) {
            return false;
        }
        return results_.fetch_owned(session_id, job_id, out, err);
    }

    bool ServerRuntime::cleanup_job(const std::string& session_id, const std::string& job_id, std::string& err) {
        if (!ensure_owned_job(session_id, job_id, err)) {
            return false;
        }

        JobStatus status{};
        if (!jobs_.get_owned_status(session_id, job_id, status, err)) {
            return false;
        }
        if (status.state == JobState::Queued ||
            status.state == JobState::Validating ||
            status.state == JobState::Running ||
            status.state == JobState::CancelRequested) {
            err = "active jobs cannot be cleaned up";
            return false;
        }

        if (!results_.erase_owned(session_id, job_id, err)) {
            return false;
        }
        if (!artifact_store_.cleanup_job_workspace(job_id, err)) {
            return false;
        }
        return jobs_.cleanup_terminal(session_id, job_id, err);
    }

    bool ServerRuntime::ensure_initialized(std::string& err) {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialized_) {
            err.clear();
            return true;
        }
        if (!artifact_store_.initialize(err)) {
            return false;
        }
        initialized_ = true;
        err.clear();
        return true;
    }

    bool ServerRuntime::ensure_owned_job(
        const std::string& session_id,
        const std::string& job_id,
        std::string& err) const {
        if (!sessions_.ensure_session_exists(session_id, err)) {
            return false;
        }
        return jobs_.ensure_owned(session_id, job_id, err);
    }

    bool ServerRuntime::finalize_rejection(
        const std::string& owner_session_id,
        const std::string& job_id,
        const ValidatedRequest& validated,
        const std::string& detail) {
        JobResult result{};
        result.result_id = jobs_.next_result_id();
        result.owner_session_id = owner_session_id;
        result.job_id = job_id;
        result.metadata.validation = validated.metadata;
        result.summary_text = make_validation_summary(validated.metadata, "Validation rejected", &detail);

        ArtifactRecord summary_artifact{};
        std::string artifact_err;
        if (artifact_store_.write_output_artifact(job_id, "rejected", result.summary_text, summary_artifact, artifact_err)) {
            result.output_artifacts.push_back(summary_artifact);
        }

        std::string store_err;
        if (!results_.store(result, store_err)) {
            return finalize_failure(owner_session_id, job_id, validated.metadata, store_err);
        }

        std::string job_err;
        jobs_.finalize_rejected(job_id, result.result_id, detail, job_err);
        sessions_.finish_job(owner_session_id);
        return false;
    }

    bool ServerRuntime::finalize_failure(
        const std::string& owner_session_id,
        const std::string& job_id,
        const ValidationMetadata& metadata,
        const std::string& detail) {
        JobResult result{};
        result.result_id = jobs_.next_result_id();
        result.owner_session_id = owner_session_id;
        result.job_id = job_id;
        result.metadata.validation = metadata;
        result.summary_text = make_validation_summary(metadata, "Execution failed", &detail);

        ArtifactRecord summary_artifact{};
        std::string artifact_err;
        if (artifact_store_.write_output_artifact(job_id, "failed", result.summary_text, summary_artifact, artifact_err)) {
            result.output_artifacts.push_back(summary_artifact);
        }

        std::optional<std::string> result_id = result.result_id;
        std::string store_err;
        std::string final_detail = detail;
        if (!results_.store(result, store_err)) {
            result_id.reset();
            final_detail = append_secondary_detail(detail, "result storage failed", store_err);
        }

        std::string job_err;
        jobs_.finalize_failed(job_id, final_detail, result_id, job_err);
        sessions_.finish_job(owner_session_id);
        return false;
    }

    bool ServerRuntime::finalize_completion(
        const std::string& owner_session_id,
        const std::string& job_id,
        JobResult result,
        std::string& err) {
        if (!results_.store(result, err)) {
            return finalize_failure(owner_session_id, job_id, result.metadata.validation, err);
        }
        if (!jobs_.finalize_completed(job_id, result.result_id, err)) {
            return finalize_failure(owner_session_id, job_id, result.metadata.validation, err);
        }
        sessions_.finish_job(owner_session_id);
        err.clear();
        return true;
    }

    bool ServerRuntime::finalize_cancellation(
        const std::string& owner_session_id,
        const std::string& job_id,
        const std::string& detail,
        std::string& err) {
        if (!jobs_.finalize_canceled(job_id, detail, err)) {
            return false;
        }
        sessions_.finish_job(owner_session_id);
        err.clear();
        return true;
    }

} // namespace sugar::service
