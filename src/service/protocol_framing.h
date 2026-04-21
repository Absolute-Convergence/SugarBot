#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/service_types.h"

namespace sugar::service {

    enum class ProtocolCommand {
        Unknown = 0,
        Connect = 1,
        ListDatasets = 2,
        ListTemplates = 3,
        UploadArtifact = 4,
        ValidateRequest = 5,
        SubmitJob = 6,
        CheckStatus = 7,
        RetrieveResults = 8,
        CancelJob = 9,
        Disconnect = 10,
    };

    enum class ProtocolErrorCode {
        None = 0,
        MalformedRequest = 1,
        UnauthorizedRequest = 2,
        InvalidArtifact = 3,
        InvalidPlanDataCompatibility = 4,
        QuotaExceeded = 5,
        NotFound = 6,
        InvalidState = 7,
        UnsupportedPlatform = 8,
        InternalError = 9,
    };

    enum class FramedMessageParseStatus {
        Complete = 0,
        Incomplete = 1,
        InvalidLength = 2,
        Oversized = 3,
    };

    struct CatalogEntry {
        std::string id{};
        std::string display_name{};
        std::size_t byte_count{ 0 };
    };

    struct UploadedArtifact {
        std::string upload_id{};
        ArtifactKind kind{ ArtifactKind::LayoutSpec };
        std::string client_name{};
        std::size_t byte_count{ 0 };
    };

    struct ProtocolRequest {
        ProtocolCommand command{ ProtocolCommand::Unknown };
        std::string session_id{};
        std::string job_id{};
        ArtifactKind upload_kind{ ArtifactKind::LayoutSpec };
        std::string upload_client_name{};
        std::string upload_content{};
        std::string layout_upload_id{};
        std::string logic_upload_id{};
        std::string data_upload_id{};
    };

    struct ProtocolResponse {
        ProtocolCommand command{ ProtocolCommand::Unknown };
        bool ok{ false };
        ProtocolErrorCode error_code{ ProtocolErrorCode::None };
        std::string message{};
        std::string session_id{};
        std::string job_id{};
        std::optional<UploadedArtifact> upload{};
        std::vector<CatalogEntry> catalog{};
        std::optional<JobStatus> job_status{};
        std::optional<JobResult> job_result{};
        bool close_connection{ false };
    };

    std::string_view protocol_command_name(ProtocolCommand command);
    std::string_view protocol_error_code_name(ProtocolErrorCode code);

    std::string serialize_protocol_request_json(const ProtocolRequest& request);
    std::string serialize_protocol_response_json(const ProtocolResponse& response);

    bool parse_protocol_request_json(std::string_view json, ProtocolRequest& out, std::string& err);
    bool parse_protocol_response_ok(std::string_view json, bool& out_ok, std::string& err);

    std::string frame_json_message(std::string_view json);
    FramedMessageParseStatus try_extract_framed_json(
        std::string& buffer,
        std::string& out_json,
        std::size_t max_message_bytes,
        std::string& err);

} // namespace sugar::service
