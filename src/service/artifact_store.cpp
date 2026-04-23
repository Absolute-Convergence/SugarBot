#include "service/artifact_store.h"

#include <fstream>

namespace sugar::service {

    namespace {
        bool path_has_prefix(const std::filesystem::path& path, const std::filesystem::path& prefix) {
            auto pit = path.begin();
            auto fit = prefix.begin();
            for (; fit != prefix.end(); ++fit, ++pit) {
                if (pit == path.end() || *pit != *fit) {
                    return false;
                }
            }
            return true;
        }

        std::string artifact_extension_for_kind(ArtifactKind kind) {
            switch (kind) {
            case ArtifactKind::LayoutSpec:
                return ".layout.json";
            case ArtifactKind::LogicSpec:
                return ".logic.json";
            case ArtifactKind::DataCsv:
                return ".data.csv";
            case ArtifactKind::SummaryText:
                return ".summary.txt";
            }
            return ".bin";
        }

        bool canonical_path_has_prefix(
            const std::filesystem::path& path,
            const std::filesystem::path& prefix) {
            return path_has_prefix(path.lexically_normal(), prefix.lexically_normal());
        }
    } // namespace

    ArtifactStore::ArtifactStore(std::filesystem::path root, const QuotaConfig& quotas)
        : root_(std::move(root)),
          quotas_(quotas) {}

    bool ArtifactStore::initialize(std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        return initialize_unlocked(err);
    }

    bool ArtifactStore::create_job_workspace(
        const std::string& job_id,
        std::filesystem::path& out_workspace,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        return create_job_workspace_unlocked(job_id, out_workspace, err);
    }

    bool ArtifactStore::resolve_job_relative_path(
        const std::string& job_id,
        std::string_view relative_name,
        std::filesystem::path& out_path,
        std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return resolve_job_relative_path_unlocked(job_id, relative_name, out_path, err);
    }

    bool ArtifactStore::stage_input_artifact(
        const std::string& job_id,
        ArtifactKind kind,
        std::string_view content,
        ArtifactRecord& out,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (content.size() > quotas_.max_upload_size_bytes) {
            err = "artifact exceeds max upload size";
            return false;
        }
        const std::string artifact_id = next_artifact_id_unlocked();
        const std::string relative_name = artifact_id + artifact_extension_for_kind(kind);
        return write_text_artifact_unlocked(job_id, kind, relative_name, content, out, err);
    }

    bool ArtifactStore::write_output_artifact(
        const std::string& job_id,
        std::string_view stem,
        std::string_view content,
        ArtifactRecord& out,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string artifact_id = next_artifact_id_unlocked();
        const std::string base = stem.empty() ? "result" : std::string(stem);
        const std::string relative_name = artifact_id + "." + base + ".txt";
        return write_text_artifact_unlocked(job_id, ArtifactKind::SummaryText, relative_name, content, out, err);
    }

    bool ArtifactStore::cleanup_job_workspace(const std::string& job_id, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            err.clear();
            return true;
        }
        std::error_code ec;
        std::filesystem::remove_all(job_workspace_path(job_id), ec);
        if (ec) {
            err = "failed to cleanup job workspace";
            return false;
        }
        err.clear();
        return true;
    }

    bool ArtifactStore::initialize_unlocked(std::string& err) {
        if (initialized_) {
            err.clear();
            return true;
        }
        if (root_.empty()) {
            root_ = std::filesystem::temp_directory_path() / "sugar_service_core";
        }
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) {
            err = "failed to create service storage root";
            return false;
        }
        root_ = std::filesystem::weakly_canonical(root_, ec);
        if (ec) {
            err = "failed to canonicalize service storage root";
            return false;
        }
        initialized_ = true;
        err.clear();
        return true;
    }

    bool ArtifactStore::create_job_workspace_unlocked(
        const std::string& job_id,
        std::filesystem::path& out_workspace,
        std::string& err) {
        if (!initialize_unlocked(err)) {
            return false;
        }
        const auto workspace = job_workspace_path(job_id);
        std::error_code ec;
        std::filesystem::create_directories(workspace, ec);
        if (ec) {
            err = "failed to create job workspace";
            return false;
        }
        const auto canonical_workspace = std::filesystem::weakly_canonical(workspace, ec);
        if (ec) {
            err = "failed to canonicalize job workspace";
            return false;
        }
        if (!canonical_path_has_prefix(canonical_workspace, root_)) {
            err = "job workspace escapes the service storage root";
            return false;
        }
        out_workspace = workspace;
        err.clear();
        return true;
    }

    bool ArtifactStore::resolve_job_relative_path_unlocked(
        const std::string& job_id,
        std::string_view relative_name,
        std::filesystem::path& out_path,
        std::string& err) const {
        if (!initialized_) {
            err = "artifact store is not initialized";
            return false;
        }
        if (relative_name.empty()) {
            err = "artifact relative path is empty";
            return false;
        }

        std::error_code ec;
        const auto workspace = std::filesystem::weakly_canonical(job_workspace_path(job_id), ec);
        if (ec) {
            err = "failed to canonicalize job workspace";
            return false;
        }
        const std::filesystem::path relative(relative_name);
        if (relative.is_absolute()) {
            err = "artifact relative path must not be absolute";
            return false;
        }

        const auto candidate = (workspace / relative).lexically_normal();
        const auto canonical_parent = std::filesystem::weakly_canonical(candidate.parent_path(), ec);
        if (ec) {
            err = "failed to canonicalize artifact parent path";
            return false;
        }
        const auto canonical_candidate = (canonical_parent / candidate.filename()).lexically_normal();
        if (!canonical_path_has_prefix(canonical_candidate, workspace)) {
            err = "artifact path escapes the job workspace";
            return false;
        }
        out_path = canonical_candidate;
        err.clear();
        return true;
    }

    std::filesystem::path ArtifactStore::job_workspace_path(const std::string& job_id) const {
        return root_ / "jobs" / job_id;
    }

    std::string ArtifactStore::next_artifact_id_unlocked() {
        return make_counter_id("art", next_artifact_seq_++);
    }

    bool ArtifactStore::write_text_artifact_unlocked(
        const std::string& job_id,
        ArtifactKind kind,
        std::string_view relative_name,
        std::string_view content,
        ArtifactRecord& out,
        std::string& err) {
        std::filesystem::path ignored_workspace{};
        if (!create_job_workspace_unlocked(job_id, ignored_workspace, err)) {
            return false;
        }

        std::filesystem::path full_path{};
        if (!resolve_job_relative_path_unlocked(job_id, relative_name, full_path, err)) {
            return false;
        }

        std::ofstream file(full_path, std::ios::binary);
        if (!file) {
            err = "failed to open artifact path for writing";
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file) {
            err = "failed to write artifact content";
            return false;
        }

        out = ArtifactRecord{};
        out.artifact_id = std::filesystem::path(relative_name).stem().stem().string();
        out.kind = kind;
        out.owner_job_id = job_id;
        out.storage_path = full_path;
        out.byte_count = content.size();
        err.clear();
        return true;
    }

} // namespace sugar::service
