#include "service/protocol_dispatch.h"

#include <algorithm>
namespace sugar::service {

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
            response_json = serialize_protocol_response_json(error);
            return false;
        }

        ProtocolResponse response{};
        const bool ok = handle_request(request, response);
        response_json = serialize_protocol_response_json(response);
        return ok;
    }

    bool ProtocolDispatcher::ensure_session_exists(const std::string& session_id, ProtocolResponse& response) {
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
            response.ok = false;
            response.error_code = ProtocolErrorCode::InternalError;
            response.message = err;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            uploads_by_session_.try_emplace(session.session_id);
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "session created";
        response.session_id = session.session_id;
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
            return false;
        }
        if (request.upload_content.size() > runtime_.quotas().max_upload_size_bytes) {
            response.ok = false;
            response.error_code = ProtocolErrorCode::QuotaExceeded;
            response.message = "artifact exceeds max upload size";
            return false;
        }

        StoredUpload upload{};
        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            upload.receipt.upload_id = next_upload_id_unlocked();
            upload.receipt.kind = request.upload_kind;
            upload.receipt.client_name = request.upload_client_name;
            upload.receipt.byte_count = request.upload_content.size();
            upload.content = request.upload_content;
            uploads_by_session_[request.session_id][upload.receipt.upload_id] = upload;
        }

        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "artifact uploaded";
        response.upload = upload.receipt;
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
            return false;
        }
        submit.intent = intent;

        std::string err;
        std::string job_id;
        if (!runtime_.submit_job(request.session_id, submit, job_id, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            return false;
        }

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
        return true;
    }

    bool ProtocolDispatcher::handle_status(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        JobStatus status{};
        std::string err;
        if (!runtime_.get_job_status(request.session_id, request.job_id, status, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            response.job_id = request.job_id;
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
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            response.job_id = request.job_id;
            return false;
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "job result retrieved";
        response.job_id = request.job_id;
        response.job_result = result;
        return true;
    }

    bool ProtocolDispatcher::handle_cancel(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        std::string err;
        if (!runtime_.cancel_job(request.session_id, request.job_id, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            response.job_id = request.job_id;
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
        return true;
    }

    bool ProtocolDispatcher::handle_disconnect(const ProtocolRequest& request, ProtocolResponse& response) {
        if (!ensure_session_exists(request.session_id, response)) {
            return false;
        }
        std::string err;
        if (!runtime_.close_session(request.session_id, err)) {
            response.ok = false;
            response.error_code = map_error_code(err);
            response.message = err;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(uploads_mutex_);
            uploads_by_session_.erase(request.session_id);
        }
        response.ok = true;
        response.error_code = ProtocolErrorCode::None;
        response.message = "session closed";
        response.close_connection = true;
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
            detail.find("does not belong") != std::string_view::npos) {
            return ProtocolErrorCode::UnauthorizedRequest;
        }
        if (detail.find("unknown upload") != std::string_view::npos ||
            detail.find("unknown job") != std::string_view::npos ||
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
