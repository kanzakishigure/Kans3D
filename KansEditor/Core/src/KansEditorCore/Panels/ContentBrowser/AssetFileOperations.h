#pragma once
#include <optional>
#include <variant>
#include <vector>
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans::Browser
{
    namespace fs = std::filesystem;

    // What deleting a set of files and folders affects.
    struct DeletionPreview
    {
        size_t Files   = 0; // Files to delete, including those inside folders.
        size_t Folders = 0;
        // Sources outside the deletion whose Products reference a deleted source.
        std::vector<SourceAssetMetadata> Dependents;
    };

    // File operations in the project's asset folder that keep asset identity:
    // sources move with their sidecars through the project, so references to
    // them stay valid. Copies are new sources (new identity) with the original
    // import settings. Paths are absolute. The service must be idle.
    class AssetFileOperations
    {
    public:
        explicit AssetFileOperations(EditorAssetImportService* service = nullptr) : m_Service(service) {}
        void SetService(EditorAssetImportService* service) { m_Service = service; }
        bool IsAvailable() const { return m_Service != nullptr; }

        // Absolute, normalized asset folder.
        fs::path AssetRoot() const;
        // Source registered for a file, if any.
        std::optional<SourceAssetMetadata> FindSource(const fs::path& file) const;
        std::vector<SourceAssetMetadata>   SourcesWithin(const fs::path& directory) const;

        std::variant<fs::path, AssetError> CreateFolder(const fs::path& parent, const std::string& name = "New Folder");
        // Renames in place; returns the new path.
        std::variant<fs::path, AssetError> Rename(const fs::path& item, const std::string& name);
        // Moves into `directory`, keeping names; returns the new paths.
        std::variant<std::vector<fs::path>, AssetError> Move(const std::vector<fs::path>& items,
                                                             const fs::path&              directory);
        // Copies into `directory`, renaming on conflict; returns the new paths.
        std::variant<std::vector<fs::path>, AssetError> Copy(const std::vector<fs::path>& items,
                                                             const fs::path&              directory);
        std::variant<DeletionPreview, AssetError>       PlanDelete(const std::vector<fs::path>& items) const;
        std::variant<std::monostate, AssetError>        Delete(const std::vector<fs::path>& items);

    private:
        std::optional<AssetError>                RequireIdle() const;
        std::optional<AssetError>                CheckInside(const fs::path& path) const;
        std::variant<std::monostate, AssetError> MoveOne(const fs::path& from, const fs::path& to);
        std::variant<std::monostate, AssetError> Refresh();

        EditorAssetImportService* m_Service = nullptr;
    };
} // namespace Kans::Browser
