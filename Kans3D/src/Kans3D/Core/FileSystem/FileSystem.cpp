#include "kspch.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Project/Project.h"
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <Windows.h>
#endif
namespace Kans
{

    int SkipBOM(std::istream& in)
    {
        char test[4] = {0};
        in.seekg(0, std::ios::beg);
        in.read(test, 3);

        if (strcmp(test, "\xEF\xBB\xBF") == 0)
        {
            in.seekg(3, std::ios::beg);
            return 3;
        }

        in.seekg(0, std::ios::beg);
        return 0;
    }

    void KansFileSystem::InitFileCache() {}

    void KansFileSystem::ResetFileCache() {}

    FileSystemSpecification* KansFileSystem::s_Specification = nullptr;
    PhysicsSettings          KansFileSystem::s_PhysicsSettings;
    const PhysicsSettings&   KansFileSystem::GetProjectPhysicsSettings() { return s_PhysicsSettings; }
    std::filesystem::path KansFileSystem::s_ProjectRoot, KansFileSystem::s_AssetFolder;

    void KansFileSystem::SetProjectPhysicsSettings(const PhysicsSettings& settings)
    {
        auto physics = settings;
        physics.Validate();
        s_PhysicsSettings = std::move(physics);
    }
    void KansFileSystem::SetProject(const ProjectConfig& config)
    {
        SetProjectPhysicsSettings(config.Physics);
        s_ProjectRoot = std::filesystem::absolute(config.RootDirectory).lexically_normal();
        s_AssetFolder = (s_ProjectRoot / config.AssetDirectory).lexically_normal();
    }
    void                     KansFileSystem::Init(const std::filesystem::path& configPath)
    {

        CORE_ASSERT(!configPath.empty(), "FileSystm init fail");
        ShutDown();
        s_Specification = new FileSystemSpecification(configPath);
        s_Specification->ReadConfig();
    }

    bool KansFileSystem::IsEditorRoot(const std::filesystem::path& dir)
    {
        if (dir.empty())
            return false;

        std::error_code error;
        return std::filesystem::is_directory(dir / "Resources", error) &&
               std::filesystem::is_regular_file(dir / "KansEditor.ini", error);
    }

    std::optional<std::filesystem::path> KansFileSystem::FindEditorRoot(const std::filesystem::path& executableDir,
                                                                    const std::filesystem::path& currentDir)
    {
        if (IsEditorRoot(executableDir))
            return executableDir;

        for (auto dir = currentDir; !dir.empty(); dir = dir.parent_path())
        {
            if (IsEditorRoot(dir))
                return dir;
            if (dir == dir.parent_path())
                break;
        }
        return std::nullopt;
    }

    bool KansFileSystem::Exists(const std::string& filepath) { return std::filesystem::exists(filepath); }

    bool KansFileSystem::Exists(const std::filesystem::path& filepath) { return std::filesystem::exists(filepath); }

    bool KansFileSystem::CreateDirectories(std::string directory)
    {
        return std::filesystem::create_directories(std::filesystem::path(directory));
    }

    bool KansFileSystem::CreateDirectories(const std::filesystem::path& directory)
    {
        return std::filesystem::create_directories(directory);
    }

    Kans::Buffer KansFileSystem::ReadBytes(const std::filesystem::path& path)
    {
        Buffer        buffer;
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        CORE_ASSERT(stream, "Load file Byte stream fail : " + path.string());

        std::streampos end = stream.tellg();
        stream.seekg(0, std::ios::beg);
        size_t size = end - stream.tellg();

        buffer.Allocate(size);
        stream.read((char*)buffer.Data, size);
        stream.close();

        return buffer;
    }

    std::string KansFileSystem::ReadFileSkipBOM(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::in | std::ios::binary);

        CORE_ASSERT(stream, "Load file Byte stream fail : " + path.string());

        stream.seekg(0, std::ios::end);
        auto filesize = stream.tellg();

        std::string result;
        result.resize(filesize);
        stream.seekg(0, std::ios::beg);
        stream.read(result.data(), filesize);
        stream.close();
        return result;
    }

    std::filesystem::path KansFileSystem::GetScriptAssemblyPath()
    {
        return s_Specification ? s_Specification->m_ScriptAssemblyPath :
                                 GetExecutableDirectory() / "Resources/Scripts/ScriptCore.dll";
    }

    std::filesystem::path KansFileSystem::GetExecutablePath()
    {
#if defined(_WIN32)
        std::vector<wchar_t> executable(32768);
        const auto length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
        if (length == 0)
            throw std::system_error(GetLastError(), std::system_category(), "GetModuleFileNameW failed");
        if (length >= executable.size())
            throw std::system_error(ERROR_INSUFFICIENT_BUFFER, std::system_category(), "Executable path is too long");
        return std::filesystem::path(std::wstring(executable.data(), length));
#else
        return std::filesystem::read_symlink("/proc/self/exe");
#endif
    }

    void KansFileSystem::ShutDown()
    {
        delete s_Specification;
        s_Specification = nullptr;
        s_ProjectRoot.clear();
        s_AssetFolder.clear();
    }

    FileSystemSpecification::FileSystemSpecification(const std::filesystem::path& configPath) : m_ConfigPath(std::filesystem::absolute(configPath).lexically_normal())
    {}

    void FileSystemSpecification::ReadConfig()
    {
        std::ifstream config_file(m_ConfigPath);
        std::string   config_line;

        while (std::getline(config_file, config_line))
        {
            size_t spos = config_line.find_first_of('=');
            if (spos != config_line.npos)
            {
                std::string key   = config_line.substr(0, spos);
                std::string value = config_line.substr(spos + 1, config_line.length());
                auto        renderKey = key;
                if (renderKey.compare(0, 3, "\xef\xbb\xbf") == 0)
                    renderKey.erase(0, 3);
                auto first = renderKey.find_first_not_of(" \t");
                if (first != std::string::npos)
                    renderKey = renderKey.substr(first, renderKey.find_last_not_of(" \t") - first + 1);
                if ("RenderBackend" == renderKey)
                    m_RenderBackend = value;
                else if ("ResoucesDir" == key)
                {
                    m_ResoucesDir = value;
                }
                else if ("FontsDir" == key)
                {
                    m_FontsPath = value;
                }
                else if ("ScriptAssemblyPath" == key)
                {
                    m_ScriptAssemblyPath = value;
                }
                else if ("ShaderCacheDir" == key)
                {
                    m_ShaderCachePath = value;
                }
            }
        }
        const auto root = m_ConfigPath.parent_path();
        auto resolve = [&](const std::filesystem::path& path) { return (root / path).lexically_normal(); };
        m_ResoucesDir = resolve(m_ResoucesDir.empty() ? "Resources" : m_ResoucesDir);
        m_FontsPath = resolve(m_FontsPath.empty() ? "Resources/Fonts" : m_FontsPath);
        m_ShaderCachePath = resolve(m_ShaderCachePath.empty() ? "Resources/Cache/Shader" : m_ShaderCachePath);
        m_ScriptAssemblyPath = resolve(m_ScriptAssemblyPath.empty() ? "Resources/Scripts/ScriptCore.dll" : m_ScriptAssemblyPath);
    }

} // namespace Kans
