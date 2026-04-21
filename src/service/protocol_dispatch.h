#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "service/protocol_framing.h"
#include "service/server_runtime.h"

namespace sugar::service {

    struct ProtocolDispatcherConfig {
        RuntimeConfig runtime{};
        std::filesystem::path source_root{};
    };

    class ProtocolDispatcher {
    public:
        explicit ProtocolDispatcher(ProtocolDispatcherConfig config = {});

        bool handle_request(const ProtocolRequest& request, ProtocolResponse& response);
        bool handle_json_request(std::string_view request_json, std::string& response_json);

    private:
        struct StoredUpload {
            UploadedArtifact receipt{};
            std::string content{};
        };

        bool ensure_session_exists(const std::string& session_id, ProtocolResponse& response);
        bool handle_connect(ProtocolResponse& response);
        bool handle_list_catalog(
            const ProtocolRequest& request,
            const std::filesystem::path& root,
            const char* extension,
            ProtocolResponse& response);
        bool handle_upload_artifact(const ProtocolRequest& request, ProtocolResponse& response);
        bool handle_validate_or_submit(
            const ProtocolRequest& request,
            JobIntent intent,
            ProtocolResponse& response);
        bool handle_status(const ProtocolRequest& request, ProtocolResponse& response);
        bool handle_results(const ProtocolRequest& request, ProtocolResponse& response);
        bool handle_cancel(const ProtocolRequest& request, ProtocolResponse& response);
        bool handle_disconnect(const ProtocolRequest& request, ProtocolResponse& response);

        bool build_submit_request(const ProtocolRequest& request, SubmitRequest& out, ProtocolResponse& response) const;
        ProtocolErrorCode map_error_code(std::string_view detail) const;
        std::string next_upload_id_unlocked();

        ProtocolDispatcherConfig config_{};
        ServerRuntime runtime_{};
        std::unordered_map<std::string, std::unordered_map<std::string, StoredUpload>> uploads_by_session_{};
        std::size_t next_upload_seq_{ 1 };
        mutable std::mutex uploads_mutex_{};
    };

} // namespace sugar::service
