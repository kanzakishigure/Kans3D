#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include "Kans3D/Core/ByteBuffer.h"

namespace Kans
{
    class ProjectConfig;
    struct PhysicsSettings;

    class FileSystemSpecification
    {
    public:
        FileSystemSpecification(const std::filesystem::path& configPath);

    private:
        void ReadConfig();

    private:
        std::filesystem::path m_ResoucesDir;
        std::filesystem::path m_FontsPath;
        std::filesystem::path m_ScriptAssemblyPath;
        std::filesystem::path m_ShaderCachePath;
        std::string           m_RenderBackend;

        std::filesystem::path m_ConfigPath;

        friend class KansFileSystem;
    };

    class KansFileSystem
    {
    public:
        static void Init(const std::filesystem::path& configPath);
        static void ShutDown();

        static std::filesystem::path GetExecutablePath();
        static std::filesystem::path GetExecutableDirectory() { return GetExecutablePath().parent_path(); }
        static bool IsEditorRoot(const std::filesystem::path& dir);
        static std::optional<std::filesystem::path> FindEditorRoot(const std::filesystem::path& executableDir,
                                                                 const std::filesystem::path& currentDir);

        // scope cache
        static void BenginCacheFile() { InitFileCache(); }
        static void EndCacheFile() { ResetFileCache(); }

        static bool Exists(const std::string& filepath);
        static bool Exists(const std::filesystem::path& filepath);

        static bool CreateDirectories(std::string directory);
        static bool CreateDirectories(const std::filesystem::path& directory);

        static Buffer      ReadBytes(const std::filesystem::path& path);
        static std::string ReadFileSkipBOM(const std::filesystem::path& path);

    public:
        static const std::filesystem::path& GetShaderCacheFolder() { return s_Specification->m_ShaderCachePath; }

        static void SetProject(const ProjectConfig& config);
        static const PhysicsSettings&       GetProjectPhysicsSettings();
        // Replaces the project's physics settings (validated; throws std::invalid_argument and keeps
        // the current ones when invalid). Worlds created afterwards use them.
        static void                         SetProjectPhysicsSettings(const PhysicsSettings& settings);
        static const std::filesystem::path& GetProjectRoot() { return s_ProjectRoot; }
        static const std::filesystem::path& GetAssetFolder() { return s_AssetFolder; }
        static const std::filesystem::path& GetFontsFolder() { return s_Specification->m_FontsPath; }
        static std::filesystem::path        GetScriptAssemblyPath();

        static const std::filesystem::path& GetResoucesFolder() { return s_Specification->m_ResoucesDir; }

    private:
        static void InitFileCache();
        static void ResetFileCache();

    private:
        static PhysicsSettings          s_PhysicsSettings;
        static FileSystemSpecification* s_Specification;
        static std::filesystem::path s_ProjectRoot, s_AssetFolder;
    };

    // in scope we will load file from fileCache
    // but ScopeFileCacher is not thread-safe
    // because the KansFileSystem is not thread-safe
    class ScopeFileCacher
    {
        ScopeFileCacher() { KansFileSystem::BenginCacheFile(); }
        ~ScopeFileCacher() { KansFileSystem::EndCacheFile(); }
    };
} // namespace Kans
