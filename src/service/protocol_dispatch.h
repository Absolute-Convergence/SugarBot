#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "service/protocol_framing.h"
#include "service/server_runtime.h"

namespace sugar::service {

    using SecurityLogFn = std::function<void(std::string_view)>;

    struct ProtocolDispatcherConfig {
        RuntimeConfig runtime{};
        std::filesystem::path source_root{};
        SecurityLogFn security_log{};
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

        struct SessionAbuseState {
            std::size_t consecutive_expensive_rejects{ 0 };
            std::size_t expensive_reject_pair_count{ 0 };
            std::size_t total_expensive_rejects{ 0 };
            bool blocked{ false };
            std::string blocked_message{};
        };

        bool ensure_session_exists(const std::string& session_id, ProtocolResponse& response, bool allow_blocked = false);
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
        void emit_security_log(std::string line) const;
        std::optional<std::string> blocked_session_message(const std::string& session_id) const;
        void reset_expensive_reject_streak(const std::string& session_id);
        bool apply_expensive_reject_policy(
            const std::string& session_id,
            std::string_view detail,
            ProtocolResponse& response);
        void clear_session_state(const std::string& session_id);
        void note_expensive_reject_success(const std::string& session_id);

        ProtocolDispatcherConfig config_{};
        ServerRuntime runtime_{};
        std::unordered_map<std::string, std::unordered_map<std::string, StoredUpload>> uploads_by_session_{};
        std::unordered_map<std::string, SessionAbuseState> abuse_by_session_{};
        std::size_t next_upload_seq_{ 1 };
        mutable std::mutex uploads_mutex_{};
        mutable std::mutex abuse_mutex_{};
    };

} // namespace sugar::service
