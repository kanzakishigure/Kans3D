#pragma once
#include <cstdint>
#include "Kans3D/Project/Project.h"

namespace Kans
{
    struct ProjectFile
    {
        PhysicsSettings       Physics;
        std::string           Name;
        uint32_t              FormatVersion = 1;
        std::filesystem::path StartupScene;
        std::filesystem::path AssetDirectory = "assets";
        std::filesystem::path CacheDirectory = "Cache";
    };

    // Usable as a folder and file name on every platform (no separators or Windows-reserved characters).
    bool IsValidProjectName(const std::string& name);
    [[nodiscard]] std::variant<ProjectFile, AssetError> LoadProjectFile(const std::filesystem::path& kproj);
    [[nodiscard]] std::optional<AssetError>             SaveProjectFile(const std::filesystem::path& kproj,
                                                                        const ProjectFile&           project);
    [[nodiscard]] std::variant<std::filesystem::path, AssetError>
                  FindProjectFile(const std::filesystem::path& directory);
    ProjectConfig MakeProjectConfig(const std::filesystem::path& root, const ProjectFile& project);
    [[nodiscard]] std::variant<std::filesystem::path, AssetError>
    CreateProjectFromTemplate(const std::filesystem::path& templateDir,
                              const std::filesystem::path& parentDir,
                              const std::string&           name);
} // namespace Kans
