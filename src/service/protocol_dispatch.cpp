#include "service/protocol_dispatch.h"

#include <algorithm>
#include <sstream>
namespace sugar::service {

    namespace {
        constexpr std::string_view kBlockedSessionMessage =
            "session closed after repeated over-limit sweep submissions";

        template <class UploadMap>
        std::size_t staged_upload_bytes(
            const UploadMap& uploads) {
            std::size_t total = 0;
            for (const auto& [upload_id, upload] : uploads) {
                (void)upload_id;
                total += upload.content.size();
            }
            return total;
        }

        bool is_job_access_probe_detail(std::string_view detail) {
            return detail.find("unknown job") != std::string_view::npos ||
                detail.find("does not belong") != std::string_view::npos;
        }

        bool is_expensive_sweep_policy_detail(std::string_view detail) {
            return detail.find("hosted sweep budget") != std::string_view::npos ||
                detail.find("estimated sweep duration") != std::string_view::npos;
        }

        std::string external_error_message(
            ProtocolErrorCode code,
            std::string_view detail) {
            if (code == ProtocolErrorCode::UnauthorizedRequest &&
                is_job_access_probe_detail(detail)) {
                return "job is unavailable to the requesting session";
            }
            return std::string(detail);
        }
    } // namespace

    ProtocolDispatcher::ProtocolDispatcher(ProtocolDispatcherConfig config)
        : config_(std::move(config)),
          runtime_(config_.runtime) {
        if (config_.source_root.empty()) {
#ifdef SUGARBOT_SOURCE_DIR
            config_.source_root = SUGARBOT_SOURCE_DIR;
#else
            config_.source_root = std::filesystem::current_path();
#endif
        }
    }

    void ProtocolDispatcher::emit_security_log(std::string line) const {
        if (!config_.security_log) {
            return;
        }
        config_.security_log(line);
    }

    std::optional<std::string> ProtocolDispatcher::blocked_session_message(const std::string& session_id) const {
        std::lock_guard<std::mutex> lock(abuse_mutex_);
        const auto it = abuse_by_session_.find(session_id);
        if (it == abuse_by_session_.end() || !it->second.blocked) {
            return std::nullopt;
        }
        return it->second.blocked_message;
    }

    void ProtocolDispatcher::reset_expensive_reject_streak(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(abuse_mutex_);
        auto it = abuse_by_session_.find(session_id);
        if (it == abuse_by_session_.end()) {
            return;
        }
        it->second.consecutive_expensive_rejects = 0;
    }

    void ProtocolDispatcher::note_expensive_reject_success(const std::string& session_id) {
        reset_expensive_reject_streak(session_id);
    }

    void ProtocolDispatcher::clear_session_state(const std::string& session_id) {
        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            uploads_by_session_.erase(session_id);
        }
        {
            std::lock_guard<std::mutex> lock(abuse_mutex_);
            abuse_by_session_.erase(session_id);
        }
    }

    bool ProtocolDispatcher::apply_expensive_reject_policy(
        const std::string& session_id,
        std::string_view detail,
        ProtocolResponse& response) {
        bool should_block = false;
        std::size_t streak = 0;
        std::size_t pair_count = 0;
        std::size_t total = 0;

        {
            std::lock_guard<std::mutex> lock(abuse_mutex_);
            auto& state = abuse_by_session_[session_id];
            ++state.total_expensive_rejects;
            ++state.consecutive_expensive_rejects;
            if (state.consecutive_expensive_rejects == 2) {
                ++state.expensive_reject_pair_count;
            }

            streak = state.consecutive_expensive_rejects;
            pair_count = state.expensive_reject_pair_count;
            total = state.total_expensive_rejects;

            should_block =
                (config_.runtime.quotas.expensive_sweep_reject_disconnect_streak > 0 &&
                 streak >= config_.runtime.quotas.expensive_sweep_reject_disconnect_streak) ||
                (config_.runtime.quotas.expensive_sweep_reject_pair_disconnect_count > 0 &&
                 pair_count >= config_.runtime.quotas.expensive_sweep_reject_pair_disconnect_count) ||
                (config_.runtime.quotas.expensive_sweep_reject_total_before_disconnect > 0 &&
                 total > config_.runtime.quotas.expensive_sweep_reject_total_before_disconnect);

            if (should_block) {
                state.blocked = true;
                state.blocked_message = std::string(kBlockedSessionMessage);
            }
        }

        if (!should_block) {
            emit_security_log(
                "event=submit result=reject_over_budget session=" + session_id +
                " streak=" + std::to_string(streak) +
                " pair_count=" + std::to_string(pair_count) +
                " total=" + std::to_string(total) +
                " detail=" + std::string(detail));
            return false;
        }

        std::string close_err;
        (void)runtime_.close_session(session_id, close_err);
        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            uploads_by_session_.erase(session_id);
        }

        response.close_connection = true;
        response.message =
            std::string(kBlockedSessionMessage) + " | " + std::string(detail);
        emit_security_log(
            "event=session_block result=closed session=" + session_id +
            " streak=" + std::to_string(streak) +
            " pair_count=" + std::to_string(pair_count) +
            " total=" + std::to_string(total) +
            " detail=" + std::string(detail));
        return true;
    }

    bool ProtocolDispatcher::handle_request(const ProtocolRequest& request, ProtocolResponse& response) {
        response = ProtocolResponse{};
        response.command = request.command;

        switch (request.command) {
        case ProtocolCommand::Connect:
            return handle_connect(response);
        case ProtocolCommand::ListDatasets:
            return handle_list_catalog(
                request,
                config_.source_root / "data",
                ".csv",
                response);
        case ProtocolCommand::ListTemplates:
            return handle_list_catalog(
                request,
                config_.source_root / "graphs" / "templates",
                ".json",
                response);
        case ProtocolCommand::UploadArtifact:
            return handle_upload_artifact(request, response);
        case ProtocolCommand::ValidateRequest:
            return handle_validate_or_submit(request, JobIntent::ValidateOnly, response);
        case ProtocolCommand::SubmitJob:
            return handle_validate_or_submit(request, JobIntent::Execute, response);
        case ProtocolCommand::CheckStatus:
            return handle_status(request, response);
        case ProtocolCommand::RetrieveResults:
            return handle_results(request, response);
        case ProtocolCommand::CancelJob:
            return handle_cancel(request, response);
        case ProtocolCommand::Disconnect:
            return handle_disconnect(request, response);
        case ProtocolCommand::Unknown:
        default:
            response.ok = false;
            response.error_code = ProtocolErrorCode::MalformedRequest;
            response.message = "unknown protocol command";
            emit_security_log("event=malformed_request result=reject detail=" + response.message);
            return false;
        }
    }

    bool ProtocolDispatcher::handle_json_request(std::string_view request_json, std::string& response_json) {
        ProtocolRequest request{};
        std::string parse_err;
        if (!parse_protocol_request_json(request_json, request, parse_err)) {
            ProtocolResponse error{};
            error.command = ProtocolCommand::Unknown;
            error.ok = false;
            error.error_code = ProtocolErrorCode::MalformedRequest;
            error.message = parse_err;
            emit_security_log("event=malformed_request result=reject detail=" + parse_err);
            response_json = serialize_protocol_response_json(error);
            return false;
        }

        ProtocolResponse response{};
        const bool ok = handle_request(request, response);
        response_json = serialize_protocol_response_json(response);
        return ok;
    }

    bool ProtocolDispatcher::ensure_session_exists(
        const std::string& session_id,
        ProtocolResponse& response,
        bool allow_blocked) {
        if (!allow_blocked) {
            if (const auto blocked = blocked_session_message(session_id); blocked.has_value()) {
                response.ok = false;
                response.error_code = ProtocolErrorCode::UnauthorizedRequest;
                response.message = *blocked;
                response.session_id = session_id;
                response.close_connection = true;
                return false;
            }
        }

        SessionInfo info{};
        std::string err;
        if (!runtime_.get_session_info(session_id, info, err)) {
            response.ok = false;
            response.error_code = ProtocolErrorCode::UnauthorizedRequest;
            response.message = err;
            response.session_id = session_id;
            return false;
        }
        response.session_id = info.session_id;
        return true;
    }

    bool ProtocolDispatcher::handle_connect(ProtocolResponse& response) {
        SessionInfo session{};
        std::string err;
        if (!runtime_.create_session(session, err)) {
            const auto code = map_error_code(err);
            response.ok = false;
            response.error_code = code;
            response.message = external_error_message(code, err);
            emit_security_log(
                "event=connect result=reject reason=" + std::string(protocol_error_code_name(code)) +
                " detail=" + response.message);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            uploads_by_session_.try_emplace(session.session_id);
        }
        {
            std::lock_guard<std::mutex> lock(abuse_mutex_);
            abuse_by_session_[session.session_id] = SessionAbuseState{};
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "session created";
        response.session_id = session.session_id;
        emit_security_log("event=connect result=ok session=" + session.session_id);
        return true;
    }

    bool ProtocolDispatcher::handle_list_catalog(
        const ProtocolRequest& request,
        const std::filesystem::path& root,
        const char* extension,
        ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }

        std::error_code ec;
        if (std::filesystem::exists(root, ec) && std::filesystem::is_directory(root, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (entry.path().extension() != extension) {
                    continue;
                }
                CatalogEntry item{};
                item.id = std::filesystem::relative(entry.path(), config_.source_root, ec).generic_string();
                item.display_name = entry.path().filename().string();
                item.byte_count = static_cast<std::size_t>(entry.file_size(ec));
                response.catalog.push_back(std::move(item));
            }
            std::sort(response.catalog.begin(), response.catalog.end(),
                [](const CatalogEntry& lhs, const CatalogEntry& rhs) {
                    return lhs.display_name < rhs.display_name;
                });
        }

        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "catalog listed";
        emit_security_log(
            "event=list_catalog result=ok session=" + request.session_id +
            " count=" + std::to_string(response.catalog.size()));
        return true;
    }

    bool ProtocolDispatcher::handle_upload_artifact(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        if (request.upload_content.empty()) {
            response.ok = false;
            response.error_code = ProtocolErrorCode::InvalidArtifact;
            response.message = "artifact content is empty";
            emit_security_log(
                "event=upload result=reject session=" + request.session_id +
                " reason=empty_artifact");
            return false;
        }
        if (request.upload_content.size() > runtime_.quotas().max_upload_size_bytes) {
            response.ok = false;
            response.error_code = ProtocolErrorCode::QuotaExceeded;
            response.message = "artifact exceeds max upload size";
            emit_security_log(
                "event=upload result=reject session=" + request.session_id +
                " reason=max_upload_size");
            return false;
        }

        StoredUpload upload{};
        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            auto& session_uploads = uploads_by_session_[request.session_id];
            if (runtime_.quotas().max_staged_uploads_per_session > 0 &&
                session_uploads.size() >= runtime_.quotas().max_staged_uploads_per_session) {
                response.ok = false;
                response.error_code = ProtocolErrorCode::QuotaExceeded;
                response.message = "session exceeded max staged uploads";
                emit_security_log(
                    "event=upload result=reject session=" + request.session_id +
                    " reason=max_staged_uploads");
                return false;
            }
            if (runtime_.quotas().max_staged_upload_bytes_per_session > 0 &&
                staged_upload_bytes(session_uploads) + request.upload_content.size() >
                    runtime_.quotas().max_staged_upload_bytes_per_session) {
                response.ok = false;
                response.error_code = ProtocolErrorCode::QuotaExceeded;
                response.message = "session exceeded max staged upload bytes";
                emit_security_log(
                    "event=upload result=reject session=" + request.session_id +
                    " reason=max_staged_upload_bytes");
                return false;
            }

            upload.receipt.upload_id = next_upload_id_unlocked();
            upload.receipt.kind = request.upload_kind;
            upload.receipt.client_name = request.upload_client_name;
            upload.receipt.byte_count = request.upload_content.size();
            upload.content = request.upload_content;
            session_uploads[upload.receipt.upload_id] = upload;
        }

        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "artifact uploaded";
        response.upload = upload.receipt;
        emit_security_log(
            "event=upload result=ok session=" + request.session_id +
            " upload_id=" + upload.receipt.upload_id +
            " kind=" + std::string(artifact_kind_name(upload.receipt.kind)) +
            " bytes=" + std::to_string(upload.receipt.byte_count));
        return true;
    }

    bool ProtocolDispatcher::handle_validate_or_submit(
        const ProtocolRequest& request,
        JobIntent intent,
        ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }

        SubmitRequest submit{};
        if (!build_submit_request(request, submit, response)) {
            emit_security_log(
                "event=submit result=reject session=" + request.session_id +
                " reason=" + std::string(protocol_error_code_name(response.error_code)) +
                " detail=" + response.message);
            return false;
        }
        submit.intent = intent;

        std::string err;
        std::string job_id;
        ValidationMetadata preflight_metadata{};
        if (!runtime_.submit_job(request.session_id, submit, job_id, &preflight_metadata, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            if (is_expensive_sweep_policy_detail(err)) {
                response.error_code = ProtocolErrorCode::QuotaExceeded;
                const bool session_blocked =
                    apply_expensive_reject_policy(request.session_id, err, response);
                if (session_blocked) {
                    response.error_code = ProtocolErrorCode::QuotaExceeded;
                }
            }
            emit_security_log(
                "event=submit result=reject session=" + request.session_id +
                " reason=" + std::string(protocol_error_code_name(response.error_code)) +
                " detail=" + response.message);
            return false;
        }
        note_expensive_reject_success(request.session_id);

        response.job_id = job_id;

        JobStatus status{};
        if (runtime_.get_job_status(request.session_id, job_id, status, err)) {
            response.job_status = status;
        }
        JobResult result{};
        if (runtime_.get_job_result(request.session_id, job_id, result, err)) {
            response.job_result = result;
        }

        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = intent == JobIntent::ValidateOnly
            ? "validation job queued"
            : "job queued";
        if (metadata_has_long_sweep_warning(preflight_metadata, runtime_.quotas())) {
            const std::string warning = make_sweep_budget_warning_message(preflight_metadata, runtime_.quotas());
            if (!warning.empty()) {
                response.message += " | " + warning;
            }
            emit_security_log(
                "event=submit result=warning session=" + request.session_id +
                " job_id=" + job_id +
                " detail=" + warning);
        }
        emit_security_log(
            "event=submit result=ok session=" + request.session_id +
            " job_id=" + job_id +
            " intent=" + std::string(job_intent_name(intent)));
        return true;
    }

    bool ProtocolDispatcher::handle_status(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        JobStatus status{};
        std::string err;
        if (!runtime_.get_job_status(request.session_id, request.job_id, status, err)) {
            const auto code = map_error_code(err);
            response.ok = false;
            response.error_code = code;
            response.message = external_error_message(code, err);
            response.job_id = request.job_id;
            emit_security_log(
                "event=status result=reject session=" + request.session_id +
                " job_id=" + request.job_id +
                " reason=" + std::string(protocol_error_code_name(code)));
            return false;
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "job status retrieved";
        response.job_id = request.job_id;
        response.job_status = status;
        return true;
    }

    bool ProtocolDispatcher::handle_results(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        JobResult result{};
        std::string err;
        if (!runtime_.get_job_result(request.session_id, request.job_id, result, err)) {
            const auto code = map_error_code(err);
            response.ok = false;
            response.error_code = code;
            response.message = external_error_message(code, err);
            response.job_id = request.job_id;
            emit_security_log(
                "event=result result=reject session=" + request.session_id +
                " job_id=" + request.job_id +
                " reason=" + std::string(protocol_error_code_name(code)));
            return false;
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "job result retrieved";
        response.job_id = request.job_id;
        response.job_result = result;
        emit_security_log(
            "event=result result=ok session=" + request.session_id +
            " job_id=" + request.job_id);
        return true;
    }

    bool ProtocolDispatcher::handle_cancel(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        std::string err;
        if (!runtime_.cancel_job(request.session_id, request.job_id, err)) {
            const auto code = map_error_code(err);
            response.ok = false;
            response.error_code = code;
            response.message = external_error_message(code, err);
            response.job_id = request.job_id;
            emit_security_log(
                "event=cancel result=reject session=" + request.session_id +
                " job_id=" + request.job_id +
                " reason=" + std::string(protocol_error_code_name(code)));
            return false;
        }

        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "job cancellation requested";
        response.job_id = request.job_id;
        JobStatus status{};
        if (runtime_.get_job_status(request.session_id, request.job_id, status, err)) {
            response.job_status = status;
        }
        emit_security_log(
            "event=cancel result=ok session=" + request.session_id +
            " job_id=" + request.job_id);
        return true;
    }

    bool ProtocolDispatcher::handle_disconnect(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response, true)) {
            return false;
        }
        std::string err;
        if (!runtime_.close_session(request.session_id, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            emit_security_log(
                "event=disconnect result=reject session=" + request.session_id +
                " reason=" + std::string(protocol_error_code_name(response.error_code)));
            return false;
        }
        clear_session_state(request.session_id);
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "session closed";
        response.close_connection = true;
        emit_security_log("event=disconnect result=ok session=" + request.session_id);
        return true;
    }

    bool ProtocolDispatcher::build_submit_request(
        const ProtocolRequest& request,
        SubmitRequest& out,
        ProtocolResponse& response) const {
        std::lock_guard<std::mutex> lock(uploads_mutex_);
        const auto session_it = uploads_by_session_.find(request.session_id);
        if (session_it == uploads_by_session_.end()) {
            response.ok = false;
            response.error_code = ProtocolErrorCode::UnauthorizedRequest;
            response.message = "unknown session id";
            return false;
        }

        auto lookup = [&](const std::string& upload_id, ArtifactKind kind, ArtifactPayload& payload) -> bool {
            const auto it = session_it->second.find(upload_id);
            if (it == session_it->second.end()) {
                response.ok = false;
                response.error_code = ProtocolErrorCode::NotFound;
                response.message = "unknown upload id";
                return false;
            }
            if (it->second.receipt.kind != kind) {
                response.ok = false;
                response.error_code = ProtocolErrorCode::InvalidArtifact;
                response.message = "upload kind does not match request slot";
                return false;
            }
            payload.client_name = it->second.receipt.client_name;
            payload.content = it->second.content;
            return true;
        };

        out = SubmitRequest{};
        return lookup(request.layout_upload_id, ArtifactKind::LayoutSpec, out.layout) &&
            lookup(request.logic_upload_id, ArtifactKind::LogicSpec, out.logic) &&
            lookup(request.data_upload_id, ArtifactKind::DataCsv, out.data);
    }

    ProtocolErrorCode ProtocolDispatcher::map_error_code(std::string_view detail) const {
        if (detail.find("unknown session") != std::string_view::npos ||
            detail.find("unknown job") != std::string_view::npos ||
            detail.find("does not belong") != std::string_view::npos) {
            return ProtocolErrorCode::UnauthorizedRequest;
        }
        if (detail.find("not available") != std::string_view::npos) {
            return ProtocolErrorCode::InvalidState;
        }
        if (detail.find("unknown upload") != std::string_view::npos ||
            detail.find("not found") != std::string_view::npos) {
            return ProtocolErrorCode::NotFound;
        }
        if (detail.find("artifact") != std::string_view::npos ||
            detail.find(".json") != std::string_view::npos ||
            detail.find(".csv") != std::string_view::npos ||
            detail.find("filename") != std::string_view::npos) {
            return ProtocolErrorCode::InvalidArtifact;
        }
        if (detail.find("max") != std::string_view::npos ||
            detail.find("budget") != std::string_view::npos ||
            detail.find("exceeds") != std::string_view::npos ||
            detail.find("quota") != std::string_view::npos ||
            detail.find("exceeded") != std::string_view::npos) {
            return ProtocolErrorCode::QuotaExceeded;
        }
        if (detail.find("compatib") != std::string_view::npos ||
            detail.find("logic.run") != std::string_view::npos ||
            detail.find("Validation rejected") != std::string_view::npos) {
            return ProtocolErrorCode::InvalidPlanDataCompatibility;
        }
        if (detail.find("active jobs") != std::string_view::npos ||
            detail.find("cannot") != std::string_view::npos) {
            return ProtocolErrorCode::InvalidState;
        }
        return ProtocolErrorCode::InternalError;
    }

    std::string ProtocolDispatcher::next_upload_id_unlocked() {
        return make_counter_id("upl", next_upload_seq_++);
    }

} // namespace sugar::service
