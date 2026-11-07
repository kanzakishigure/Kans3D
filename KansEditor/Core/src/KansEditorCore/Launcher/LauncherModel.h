#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Asset/AssetError.h"

// Project launcher logic without UI, shared by the launcher layer and the editor's File menu.
namespace Kans::Launcher
{
    struct ProjectTemplate
    {
        std::string           Name;
        std::filesystem::path Directory;
    };

    // A folder that holds no .kproj yet; the launcher offers to create one.
    struct MissingProjectFile
    {
        std::filesystem::path Folder;
    };

    using Selection = std::variant<std::filesystem::path /* .kproj */, MissingProjectFile, AssetError>;

    // Resolves a path picked by the user: a valid .kproj, or a folder holding exactly one.
    Selection ResolveSelection(const std::filesystem::path& path);
    // Creates <Folder>.kproj in a folder without one (and its asset folder) and returns it.
    std::variant<std::filesystem::path, AssetError> CreateProjectFile(const std::filesystem::path& folder);

    // Subfolders of `templatesDir` holding a .kproj, sorted with Blank first.
    std::vector<ProjectTemplate> ListTemplates(const std::filesystem::path& templatesDir);
    // Why `location / name` cannot become a new project, or nothing when it can.
    std::optional<std::string> NewProjectProblem(const std::filesystem::path& location, const std::string& name);
    // A free "<base>", "<base>2", ... name inside `location`.
    std::string UniqueProjectName(const std::filesystem::path& location, const std::string& base);
    // Where new projects go: next to the most recent one, else a folder in the user's home.
    std::filesystem::path DefaultLocation(const std::vector<std::filesystem::path>& recentProjectFiles,
                                          const std::filesystem::path&              home);
    std::filesystem::path HomeDirectory();

    // "just now", "5 minutes ago", "yesterday", "3 days ago", or the date.
    std::string FormatLastOpened(int64_t now, int64_t then);

    struct LaunchArguments
    {
        std::filesystem::path Project; // Empty: show the launcher.
        std::string           Error;
    };
    LaunchArguments ParseLaunchArguments(int argc, const char* const* argv);
    // Panel layout and per-project editor state: kept with the project's derived data.
    std::filesystem::path LayoutFile(const std::filesystem::path& projectRoot, const std::filesystem::path& cacheDirectory);
} // namespace Kans::Launcher
