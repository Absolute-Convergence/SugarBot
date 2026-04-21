#pragma once

#include <mutex>
#include <filesystem>
#include <string>
#include <string_view>

#include "service/service_types.h"

namespace sugar::service {

    class ArtifactStore {
    public:
        ArtifactStore(std::filesystem::path root, const QuotaConfig& quotas = {});

        bool initialize(std::string& err);
        bool create_job_workspace(const std::string& job_id, std::filesystem::path& out_workspace, std::string& err);
        bool resolve_job_relative_path(
            const std::string& job_id,
            std::string_view relative_name,
            std::filesystem::path& out_path,
            std::string& err) const;
        bool stage_input_artifact(
            const std::string& job_id,
            ArtifactKind kind,
            std::string_view content,
            ArtifactRecord& out,
            std::string& err);
        bool write_output_artifact(
            const std::string& job_id,
            std::string_view stem,
            std::string_view content,
            ArtifactRecord& out,
            std::string& err);
        bool cleanup_job_workspace(const std::string& job_id, std::string& err);

        [[nodiscard]] const std::filesystem::path& root_path() const noexcept {
            return root_;
        }

    private:
        bool initialize_unlocked(std::string& err);
        bool create_job_workspace_unlocked(
            const std::string& job_id,
            std::filesystem::path& out_workspace,
            std::string& err);
        bool resolve_job_relative_path_unlocked(
            const std::string& job_id,
            std::string_view relative_name,
            std::filesystem::path& out_path,
            std::string& err) const;
        std::filesystem::path job_workspace_path(const std::string& job_id) const;
        std::string next_artifact_id_unlocked();
        bool write_text_artifact_unlocked(
            const std::string& job_id,
            ArtifactKind kind,
            std::string_view relative_name,
            std::string_view content,
            ArtifactRecord& out,
            std::string& err);

        std::filesystem::path root_{};
        QuotaConfig quotas_{};
        std::size_t next_artifact_seq_{ 1 };
        bool initialized_{ false };
        mutable std::mutex mutex_{};
    };

} // namespace sugar::service
