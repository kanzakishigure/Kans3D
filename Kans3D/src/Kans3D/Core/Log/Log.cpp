#include "kspch.h"
#include "Log.h"
#include "Kans3D/Project/RecentProjects.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Kans
{
    namespace
    {
        std::filesystem::path s_FilePath;

        bool IsRunLog(const std::filesystem::path& path)
        {
            const auto name = path.filename().string();
            if (name.size() != 26 || name.compare(0, 7, "Kans3D-") || name[15] != '-' || name.compare(22, 4, ".log"))
                return false;
            for (size_t i = 7; i < 22; ++i)
                if (i != 15 && (name[i] < '0' || name[i] > '9'))
                    return false;
            return true;
        }

        std::filesystem::path ReserveLog(const std::filesystem::path& directory)
        {
            std::filesystem::create_directories(directory);
            std::string newestName;
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                if (entry.is_regular_file() && IsRunLog(entry.path()))
                    newestName = std::max(newestName, entry.path().filename().string());
            auto now = std::time(nullptr);
            // Do not overwrite another run started in the same second (including another process).
            for (int attempt = 0; attempt < 3600; ++attempt, ++now)
            {
                std::tm time {};
#ifdef _WIN32
                localtime_s(&time, &now);
#else
                localtime_r(&now, &time);
#endif
                std::ostringstream name;
                name << "Kans3D-" << std::put_time(&time, "%Y%m%d-%H%M%S") << ".log";
                // Rotation can free an earlier name. Reusing it would make the current run the oldest log.
                if (name.str() <= newestName)
                    continue;
                auto path = std::filesystem::absolute(directory / name.str());
#ifdef _WIN32
                const auto handle = CreateFileW(path.c_str(),
                                                GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                nullptr,
                                                CREATE_NEW,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(handle);
                    return path;
                }
                if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                    break;
#else
                const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (fd >= 0)
                {
                    close(fd);
                    return path;
                }
                if (errno != EEXIST)
                    break;
#endif
            }
            throw std::runtime_error("Cannot create a Kans3D log in " + directory.u8string());
        }
    } // namespace

    std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
    std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

    void Log::Init()
    {
        if (s_CoreLogger && s_ClientLogger)
            return;
        spdlog::set_pattern("%^ [%T] %n: %v %$");
        s_CoreLogger = spdlog::stdout_color_mt("Kans3D");
        s_ClientLogger = spdlog::stdout_color_mt("KansEditor");
        for (auto& logger : {s_CoreLogger, s_ClientLogger})
        {
            logger->set_level(spdlog::level::trace);
            logger->flush_on(spdlog::level::warn);
        }
    }

    std::vector<std::filesystem::path> Log::SelectFilesToDelete(std::vector<std::filesystem::path> files, size_t keep)
    {
        files.erase(std::remove_if(files.begin(), files.end(), [](const auto& path) { return !IsRunLog(path); }),
                    files.end());
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
        files.erase(std::unique(files.begin(), files.end()), files.end());
        files.resize(files.size() > keep ? files.size() - keep : 0);
        return files;
    }

    bool Log::StartFileLogging(const std::filesystem::path& directory)
    {
        Init();
        if (!s_FilePath.empty())
            return true;
        std::vector<std::filesystem::path> directories;
        if (!directory.empty())
            directories.push_back(directory);
        else
        {
#ifdef _WIN32
            // Match the placement-store convention without converting Unicode user names through the ANSI code page.
            if (const auto* appData = _wgetenv(L"APPDATA"); appData && *appData)
                directories.emplace_back(std::filesystem::path(appData) / L"Kans3D/Logs");
            else if (const auto* userHome = _wgetenv(L"HOME"); userHome && *userHome)
                directories.emplace_back(std::filesystem::path(userHome) / L".config/Kans3D/Logs");
#else
            directories.push_back(RecentProjects::DefaultStorageFile().parent_path() / "Logs");
#endif
        }
        directories.emplace_back("Logs");
        for (const auto& candidate : directories)
        {
            std::filesystem::path path;
            try
            {
                path      = ReserveLog(candidate);
                auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
#ifdef _WIN32
                    path.wstring(),
#else
                    path.string(),
#endif
                    false);
                sink->set_pattern("[%Y-%m-%d %T] [%n] [%l] %v");
                s_CoreLogger->sinks().push_back(sink);
                s_ClientLogger->sinks().push_back(sink);
                s_FilePath = path;
                spdlog::flush_every(std::chrono::seconds(3));
                std::vector<std::filesystem::path> files;
                std::error_code                    ec;
                for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec))
                    if (entry.is_regular_file(ec))
                        files.push_back(entry.path());
                for (const auto& old : SelectFilesToDelete(std::move(files)))
                    std::filesystem::remove(old, ec);
                CORE_INFO("Log file: {}", path.u8string());
                Flush();
                return true;
            }
            catch (const std::exception& error)
            {
                if (!s_FilePath.empty())
                    return true; // Retention failure must not discard an already-open sink.
                if (!path.empty())
                {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                }
                CORE_WARN("File logging unavailable in {}: {}", candidate.u8string(), error.what());
            }
        }
        return false;
    }

    const std::filesystem::path& Log::GetFilePath() { return s_FilePath; }

    void Log::Flush()
    {
        if (s_CoreLogger)
            s_CoreLogger->flush();
        if (s_ClientLogger)
            s_ClientLogger->flush();
    }

    void Log::ShutDown()
    {
        Flush();
        spdlog::shutdown(); // Also stops the periodic flush thread.
        s_CoreLogger.reset();
        s_ClientLogger.reset();
        s_FilePath.clear();
    }
} // namespace Kans
