#include "service/protocol_framing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

#include "ingest/json_min.h"

namespace sugar::service {

    namespace {

        std::string escape_json_string(std::string_view value) {
            std::string out;
            out.reserve(value.size() * 2);
            for (unsigned char ch : value) {
                switch (ch) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        char buf[7]{};
                        std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(ch));
                        out += buf;
                    }
                    else {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }
            return out;
        }

        void append_json_string_field(
            std::ostringstream& out,
            std::string_view key,
            std::string_view value,
            bool& first) {
            if (!first) out << ",";
            first = false;
            out << "\"" << key << "\":\"" << escape_json_string(value) << "\"";
        }

        void append_json_bool_field(
            std::ostringstream& out,
            std::string_view key,
            bool value,
            bool& first) {
            if (!first) out << ",";
            first = false;
            out << "\"" << key << "\":" << (value ? "true" : "false");
        }

        void append_json_u64_field(
            std::ostringstream& out,
            std::string_view key,
            std::uint64_t value,
            bool& first) {
            if (!first) out << ",";
            first = false;
            out << "\"" << key << "\":" << value;
        }

        void append_json_double_field(
            std::ostringstream& out,
            std::string_view key,
            double value,
            bool& first) {
            if (!first) out << ",";
            first = false;
            out << "\"" << key << "\":";
            if (std::isfinite(value)) {
                out << value;
            }
            else {
                out << "null";
            }
        }

        bool json_string_field(
            const json_min::Object& object,
            const char* key,
            std::string& out,
            std::string& err,
            bool required = true) {
            const auto* value = json_min::obj_get(object, key);
            if (!value) {
                if (required) {
                    err = std::string("missing required string field '") + key + "'";
                    return false;
                }
                out.clear();
                return true;
            }
            if (!value->is_str()) {
                err = std::string("field '") + key + "' must be a string";
                return false;
            }
            out = value->as_str();
            return true;
        }

        bool json_object_field(
            const json_min::Object& object,
            const char* key,
            const json_min::Object*& out,
            std::string& err) {
            const auto* value = json_min::obj_get(object, key);
            if (!value || !value->is_obj()) {
                err = std::string("field '") + key + "' must be an object";
                return false;
            }
            out = &value->as_obj();
            return true;
        }

        ArtifactKind artifact_kind_from_protocol(std::string_view kind, bool& ok) {
            ok = true;
            if (kind == "layout") return ArtifactKind::LayoutSpec;
            if (kind == "logic") return ArtifactKind::LogicSpec;
            if (kind == "data") return ArtifactKind::DataCsv;
            ok = false;
            return ArtifactKind::LayoutSpec;
        }

        void append_artifact_record_json(std::ostringstream& out, const ArtifactRecord& artifact) {
            bool first = true;
            out << "{";
            append_json_string_field(out, "artifact_id", artifact.artifact_id, first);
            append_json_string_field(out, "kind", artifact_kind_name(artifact.kind), first);
            append_json_string_field(out, "owner_job_id", artifact.owner_job_id, first);
            append_json_u64_field(out, "byte_count", static_cast<std::uint64_t>(artifact.byte_count), first);
            out << "}";
        }

        void append_progress_event_json(std::ostringstream& out, const ProgressEvent& event) {
            bool first = true;
            out << "{";
            append_json_string_field(out, "state", job_state_name(event.state), first);
            append_json_string_field(out, "message", event.message, first);
            append_json_u64_field(out, "completed", static_cast<std::uint64_t>(event.completed), first);
            append_json_u64_field(out, "total", static_cast<std::uint64_t>(event.total), first);
            out << "}";
        }

        void append_validation_metadata_json(std::ostringstream& out, const ValidationMetadata& metadata) {
            bool first = true;
            out << "{";
            append_json_bool_field(out, "valid", metadata.valid, first);
            append_json_string_field(out, "resolved_mode", resolved_execution_mode_name(metadata.resolved_mode), first);
            append_json_bool_field(out, "multi_series", metadata.multi_series, first);
            append_json_u64_field(out, "series_count", static_cast<std::uint64_t>(metadata.series_count), first);
            append_json_u64_field(out, "row_count", static_cast<std::uint64_t>(metadata.row_count), first);
            append_json_u64_field(out, "input_warning_count", static_cast<std::uint64_t>(metadata.input_warning_count), first);
            append_json_u64_field(out, "incompatible_nodes", static_cast<std::uint64_t>(metadata.incompatible_nodes), first);
            append_json_u64_field(out, "incompatible_bindings", static_cast<std::uint64_t>(metadata.incompatible_bindings), first);
            append_json_u64_field(out, "sweep_candidate_count", static_cast<std::uint64_t>(metadata.sweep_candidate_count), first);
            out << "}";
        }

        void append_backtest_json(std::ostringstream& out, const BacktestResult& result) {
            bool first = true;
            out << "{";
            append_json_double_field(out, "pnl", result.pnl, first);
            append_json_u64_field(out, "trades", static_cast<std::uint64_t>(result.trades), first);
            append_json_double_field(out, "max_drawdown", result.max_drawdown, first);
            append_json_u64_field(out, "long_trades", static_cast<std::uint64_t>(result.long_trades), first);
            append_json_u64_field(out, "short_trades", static_cast<std::uint64_t>(result.short_trades), first);
            append_json_double_field(out, "pnl_long", result.pnl_long, first);
            append_json_double_field(out, "pnl_short", result.pnl_short, first);
            append_json_u64_field(out, "trade_hash", static_cast<std::uint64_t>(result.trade_hash), first);
            if (result.has_best_start_date) {
                append_json_u64_field(out, "best_start_date", static_cast<std::uint64_t>(result.best_start_date), first);
            }
            out << "}";
        }

        void append_panel_json(std::ostringstream& out, const PanelResult& panel) {
            bool first = true;
            out << "{";
            append_json_u64_field(out, "entities_total", static_cast<std::uint64_t>(panel.entities_total), first);
            append_json_u64_field(out, "entities_ok", static_cast<std::uint64_t>(panel.entities_ok), first);
            append_json_u64_field(out, "entities_failed", static_cast<std::uint64_t>(panel.entities_failed), first);
            append_json_bool_field(out, "canceled", panel.canceled, first);
            append_json_string_field(out, "cancel_message", panel.cancel_message, first);
            if (!first) out << ",";
            first = false;
            out << "\"topk\":[";
            for (std::size_t i = 0; i < panel.topk.size(); ++i) {
                if (i != 0) out << ",";
                bool row_first = true;
                out << "{";
                append_json_string_field(out, "entity_key", panel.topk[i].entity_key, row_first);
                append_json_string_field(out, "series_key", panel.topk[i].series_key, row_first);
                append_json_string_field(out, "ranking_metric", panel_ranking_metric_name(panel.topk[i].ranking_metric), row_first);
                append_json_double_field(out, "ranking_score", panel.topk[i].ranking_score, row_first);
                append_json_double_field(out, "pnl", panel.topk[i].metrics.pnl, row_first);
                append_json_u64_field(out, "trades", static_cast<std::uint64_t>(panel.topk[i].metrics.trades), row_first);
                append_json_double_field(out, "max_drawdown", panel.topk[i].metrics.max_drawdown, row_first);
                out << "}";
            }
            out << "]";
            out << "}";
        }

        void append_job_status_json(std::ostringstream& out, const JobStatus& status) {
            bool first = true;
            out << "{";
            append_json_string_field(out, "job_id", status.job_id, first);
            append_json_string_field(out, "owner_session_id", status.owner_session_id, first);
            append_json_string_field(out, "state", job_state_name(status.state), first);
            append_json_string_field(out, "failure_message", status.failure_message, first);
            if (status.result_id.has_value()) {
                append_json_string_field(out, "result_id", *status.result_id, first);
            }
            if (!first) out << ",";
            first = false;
            out << "\"staged_artifacts\":[";
            for (std::size_t i = 0; i < status.staged_artifacts.size(); ++i) {
                if (i != 0) out << ",";
                append_artifact_record_json(out, status.staged_artifacts[i]);
            }
            out << "],\"progress\":[";
            for (std::size_t i = 0; i < status.progress.size(); ++i) {
                if (i != 0) out << ",";
                append_progress_event_json(out, status.progress[i]);
            }
            out << "]";
            out << "}";
        }

        void append_job_result_json(std::ostringstream& out, const JobResult& result) {
            bool first = true;
            out << "{";
            append_json_string_field(out, "result_id", result.result_id, first);
            append_json_string_field(out, "owner_session_id", result.owner_session_id, first);
            append_json_string_field(out, "job_id", result.job_id, first);
            append_json_string_field(out, "summary_text", result.summary_text, first);
            if (!first) out << ",";
            first = false;
            out << "\"metadata\":{";
            bool meta_first = true;
            if (!meta_first) out << ",";
            meta_first = false;
            out << "\"validation\":";
            append_validation_metadata_json(out, result.metadata.validation);
            if (result.metadata.primary_backtest.has_value()) {
                out << ",\"primary_backtest\":";
                append_backtest_json(out, *result.metadata.primary_backtest);
            }
            if (result.metadata.panel.has_value()) {
                out << ",\"panel\":";
                append_panel_json(out, *result.metadata.panel);
            }
            out << "},\"output_artifacts\":[";
            for (std::size_t i = 0; i < result.output_artifacts.size(); ++i) {
                if (i != 0) out << ",";
                append_artifact_record_json(out, result.output_artifacts[i]);
            }
            out << "]";
            out << "}";
        }

        ProtocolCommand protocol_command_from_string(std::string_view op) {
            if (op == "connect") return ProtocolCommand::Connect;
            if (op == "list_datasets") return ProtocolCommand::ListDatasets;
            if (op == "list_templates") return ProtocolCommand::ListTemplates;
            if (op == "upload_artifact") return ProtocolCommand::UploadArtifact;
            if (op == "validate_request") return ProtocolCommand::ValidateRequest;
            if (op == "submit_job") return ProtocolCommand::SubmitJob;
            if (op == "check_status") return ProtocolCommand::CheckStatus;
            if (op == "retrieve_results") return ProtocolCommand::RetrieveResults;
            if (op == "cancel_job") return ProtocolCommand::CancelJob;
            if (op == "disconnect") return ProtocolCommand::Disconnect;
            return ProtocolCommand::Unknown;
        }

    } // namespace

    std::string_view protocol_command_name(ProtocolCommand command) {
        switch (command) {
        case ProtocolCommand::Connect: return "connect";
        case ProtocolCommand::ListDatasets: return "list_datasets";
        case ProtocolCommand::ListTemplates: return "list_templates";
        case ProtocolCommand::UploadArtifact: return "upload_artifact";
        case ProtocolCommand::ValidateRequest: return "validate_request";
        case ProtocolCommand::SubmitJob: return "submit_job";
        case ProtocolCommand::CheckStatus: return "check_status";
        case ProtocolCommand::RetrieveResults: return "retrieve_results";
        case ProtocolCommand::CancelJob: return "cancel_job";
        case ProtocolCommand::Disconnect: return "disconnect";
        case ProtocolCommand::Unknown:
        default:
            return "unknown";
        }
    }

    std::string_view protocol_error_code_name(ProtocolErrorCode code) {
        switch (code) {
        case ProtocolErrorCode::None: return "none";
        case ProtocolErrorCode::MalformedRequest: return "malformed_request";
        case ProtocolErrorCode::UnauthorizedRequest: return "unauthorized_request";
        case ProtocolErrorCode::InvalidArtifact: return "invalid_artifact";
        case ProtocolErrorCode::InvalidPlanDataCompatibility: return "invalid_plan_data_compatibility";
        case ProtocolErrorCode::QuotaExceeded: return "quota_exceeded";
        case ProtocolErrorCode::NotFound: return "not_found";
        case ProtocolErrorCode::InvalidState: return "invalid_state";
        case ProtocolErrorCode::UnsupportedPlatform: return "unsupported_platform";
        case ProtocolErrorCode::InternalError: return "internal_error";
        }
        return "internal_error";
    }

    std::string serialize_protocol_request_json(const ProtocolRequest& request) {
        std::ostringstream out;
        bool first = true;
        out << "{";
        append_json_string_field(out, "op", protocol_command_name(request.command), first);
        if (!request.session_id.empty()) {
            append_json_string_field(out, "session_id", request.session_id, first);
        }
        if (!request.job_id.empty()) {
            append_json_string_field(out, "job_id", request.job_id, first);
        }
        if (request.command == ProtocolCommand::UploadArtifact) {
            if (!first) out << ",";
            first = false;
            out << "\"artifact\":{";
            bool artifact_first = true;
            append_json_string_field(out, "kind", artifact_kind_name(request.upload_kind), artifact_first);
            append_json_string_field(out, "client_name", request.upload_client_name, artifact_first);
            append_json_string_field(out, "content", request.upload_content, artifact_first);
            out << "}";
        }
        if (request.command == ProtocolCommand::ValidateRequest ||
            request.command == ProtocolCommand::SubmitJob) {
            if (!first) out << ",";
            first = false;
            out << "\"uploads\":{";
            bool uploads_first = true;
            append_json_string_field(out, "layout_upload_id", request.layout_upload_id, uploads_first);
            append_json_string_field(out, "logic_upload_id", request.logic_upload_id, uploads_first);
            append_json_string_field(out, "data_upload_id", request.data_upload_id, uploads_first);
            out << "}";
        }
        out << "}";
        return out.str();
    }

    std::string serialize_protocol_response_json(const ProtocolResponse& response) {
        std::ostringstream out;
        bool first = true;
        out << "{";
        append_json_bool_field(out, "ok", response.ok, first);
        append_json_string_field(out, "op", protocol_command_name(response.command), first);
        append_json_string_field(out, "error_code", protocol_error_code_name(response.error_code), first);
        append_json_string_field(out, "message", response.message, first);
        if (!response.session_id.empty()) {
            append_json_string_field(out, "session_id", response.session_id, first);
        }
        if (!response.job_id.empty()) {
            append_json_string_field(out, "job_id", response.job_id, first);
        }
        if (response.upload.has_value()) {
            if (!first) out << ",";
            first = false;
            out << "\"upload\":{";
            bool upload_first = true;
            append_json_string_field(out, "upload_id", response.upload->upload_id, upload_first);
            append_json_string_field(out, "kind", artifact_kind_name(response.upload->kind), upload_first);
            append_json_string_field(out, "client_name", response.upload->client_name, upload_first);
            append_json_u64_field(out, "byte_count", static_cast<std::uint64_t>(response.upload->byte_count), upload_first);
            out << "}";
        }
        if (!response.catalog.empty()) {
            if (!first) out << ",";
            first = false;
            out << "\"catalog\":[";
            for (std::size_t i = 0; i < response.catalog.size(); ++i) {
                if (i != 0) out << ",";
                bool catalog_first = true;
                out << "{";
                append_json_string_field(out, "id", response.catalog[i].id, catalog_first);
                append_json_string_field(out, "display_name", response.catalog[i].display_name, catalog_first);
                append_json_u64_field(out, "byte_count", static_cast<std::uint64_t>(response.catalog[i].byte_count), catalog_first);
                out << "}";
            }
            out << "]";
        }
        if (response.job_status.has_value()) {
            if (!first) out << ",";
            first = false;
            out << "\"job_status\":";
            append_job_status_json(out, *response.job_status);
        }
        if (response.job_result.has_value()) {
            if (!first) out << ",";
            first = false;
            out << "\"job_result\":";
            append_job_result_json(out, *response.job_result);
        }
        out << "}";
        return out.str();
    }

    bool parse_protocol_request_json(std::string_view json, ProtocolRequest& out, std::string& err) {
        json_min::Value root{};
        json_min::ParseError parse_err{};
        if (!json_min::parse(json, root, parse_err)) {
            std::ostringstream detail;
            detail << parse_err.message << " at line " << parse_err.line << ", col " << parse_err.col;
            err = detail.str();
            return false;
        }
        if (!root.is_obj()) {
            err = "protocol request root must be an object";
            return false;
        }

        const auto& object = root.as_obj();
        std::string op_name;
        if (!json_string_field(object, "op", op_name, err)) {
            return false;
        }
        out = ProtocolRequest{};
        out.command = protocol_command_from_string(op_name);
        if (out.command == ProtocolCommand::Unknown) {
            err = "unknown protocol op";
            return false;
        }

        switch (out.command) {
        case ProtocolCommand::Connect:
            return true;
        case ProtocolCommand::ListDatasets:
        case ProtocolCommand::ListTemplates:
        case ProtocolCommand::Disconnect:
            return json_string_field(object, "session_id", out.session_id, err);
        case ProtocolCommand::CheckStatus:
        case ProtocolCommand::RetrieveResults:
        case ProtocolCommand::CancelJob:
            return json_string_field(object, "session_id", out.session_id, err) &&
                json_string_field(object, "job_id", out.job_id, err);
        case ProtocolCommand::UploadArtifact:
        {
            if (!json_string_field(object, "session_id", out.session_id, err)) {
                return false;
            }
            const json_min::Object* artifact = nullptr;
            if (!json_object_field(object, "artifact", artifact, err)) {
                return false;
            }
            std::string kind_name;
            if (!json_string_field(*artifact, "kind", kind_name, err) ||
                !json_string_field(*artifact, "client_name", out.upload_client_name, err) ||
                !json_string_field(*artifact, "content", out.upload_content, err)) {
                return false;
            }
            bool ok = false;
            out.upload_kind = artifact_kind_from_protocol(kind_name, ok);
            if (!ok) {
                err = "artifact.kind must be one of layout, logic, or data";
                return false;
            }
            return true;
        }
        case ProtocolCommand::ValidateRequest:
        case ProtocolCommand::SubmitJob:
        {
            if (!json_string_field(object, "session_id", out.session_id, err)) {
                return false;
            }
            const json_min::Object* uploads = nullptr;
            if (!json_object_field(object, "uploads", uploads, err)) {
                return false;
            }
            return json_string_field(*uploads, "layout_upload_id", out.layout_upload_id, err) &&
                json_string_field(*uploads, "logic_upload_id", out.logic_upload_id, err) &&
                json_string_field(*uploads, "data_upload_id", out.data_upload_id, err);
        }
        case ProtocolCommand::Unknown:
        default:
            err = "unknown protocol op";
            return false;
        }
    }

    bool parse_protocol_response_ok(std::string_view json, bool& out_ok, std::string& err) {
        json_min::Value root{};
        json_min::ParseError parse_err{};
        if (!json_min::parse(json, root, parse_err)) {
            std::ostringstream detail;
            detail << parse_err.message << " at line " << parse_err.line << ", col " << parse_err.col;
            err = detail.str();
            return false;
        }
        if (!root.is_obj()) {
            err = "protocol response root must be an object";
            return false;
        }
        const auto& object = root.as_obj();
        const auto* ok_value = json_min::obj_get(object, "ok");
        if (!ok_value || !ok_value->is_bool()) {
            err = "protocol response is missing bool field 'ok'";
            return false;
        }
        out_ok = ok_value->as_bool();
        err.clear();
        return true;
    }

    std::string frame_json_message(std::string_view json) {
        const auto length = static_cast<std::uint32_t>(json.size());
        std::string out(4, '\0');
        out[0] = static_cast<char>((length >> 24) & 0xFF);
        out[1] = static_cast<char>((length >> 16) & 0xFF);
        out[2] = static_cast<char>((length >> 8) & 0xFF);
        out[3] = static_cast<char>(length & 0xFF);
        out.append(json.data(), json.size());
        return out;
    }

    FramedMessageParseStatus try_extract_framed_json(
        std::string& buffer,
        std::string& out_json,
        std::size_t max_message_bytes,
        std::string& err) {
        out_json.clear();
        err.clear();
        if (buffer.size() < 4) {
            return FramedMessageParseStatus::Incomplete;
        }

        const auto b0 = static_cast<std::uint8_t>(buffer[0]);
        const auto b1 = static_cast<std::uint8_t>(buffer[1]);
        const auto b2 = static_cast<std::uint8_t>(buffer[2]);
        const auto b3 = static_cast<std::uint8_t>(buffer[3]);
        const std::uint32_t message_size =
            (static_cast<std::uint32_t>(b0) << 24) |
            (static_cast<std::uint32_t>(b1) << 16) |
            (static_cast<std::uint32_t>(b2) << 8) |
            static_cast<std::uint32_t>(b3);

        if (message_size == 0) {
            err = "framed message length must be non-zero";
            return FramedMessageParseStatus::InvalidLength;
        }
        if (message_size > max_message_bytes) {
            err = "framed message exceeds configured maximum";
            return FramedMessageParseStatus::Oversized;
        }
        if (buffer.size() < 4u + static_cast<std::size_t>(message_size)) {
            return FramedMessageParseStatus::Incomplete;
        }

        out_json.assign(buffer.data() + 4, static_cast<std::size_t>(message_size));
        buffer.erase(0, 4u + static_cast<std::size_t>(message_size));
        return FramedMessageParseStatus::Complete;
    }

} // namespace sugar::service
