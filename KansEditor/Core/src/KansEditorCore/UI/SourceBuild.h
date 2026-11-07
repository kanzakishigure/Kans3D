#pragma once
#include <filesystem>
#include <string>

namespace Kans
{
    class EditorAssetImportService;
    // Starts rebuilding the project source `file` after an editor saved it; returns the status to
    // show ("Saved; building...", or why no build started).
    std::string BuildSavedSource(EditorAssetImportService* service, const std::filesystem::path& file);
} // namespace Kans
