#pragma once
#include "Kans3D/Core/Base/Base.h"

#include <glm/gtx/string_cast.hpp>

#include <spdlog/spdlog.h>
#include <filesystem>
#include <vector>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>

namespace Kans
{

    class Log
    {
    public:
        enum class Level : uint8_t
        {
            Trace = 0,
            Info,
            Warn,
            Error,
            Critical
        };
        enum class Type : uint8_t
        {
            Core = 0,
            Client
        };

        static void                                    Init();
        static void                                    ShutDown();
        // Init stays console-only for library users/tests. Applications opt in at startup.
        static bool                         StartFileLogging(const std::filesystem::path& directory = {});
        static const std::filesystem::path& GetFilePath();
        static void                         Flush();
        // Timestamped names sort chronologically; unrelated files are never selected.
        static std::vector<std::filesystem::path>      SelectFilesToDelete(std::vector<std::filesystem::path> files,
                                                                           size_t                             keep = 10);
        inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
        inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }

        template<typename... Args>
        static void PrintMessage(Log::Type type, Log::Level level, std::string_view tag, Args&&... args);

        template<typename... Args>
        static void PrintAssertMessage(Log::Type type, std::string string, Args&&... args);

        // Invoked as AssertPrinter{type}(__VA_ARGS__) so an empty argument list
        // needs no comma elision (__VA_OPT__ is C++20-only on MSVC).
        struct AssertPrinter
        {
            Log::Type Type;

            template<typename... Args>
            void operator()(Args&&... args) const
            {
                PrintAssertMessage(Type, "Assertion Failed", std::forward<Args>(args)...);
            }
        };

    private:
        static std::shared_ptr<spdlog::logger> s_CoreLogger;
        static std::shared_ptr<spdlog::logger> s_ClientLogger;
    };

} // namespace Kans

template<typename OStream, glm::length_t L, typename T, glm::qualifier Q>
inline OStream& operator<<(OStream& os, const glm::vec<L, T, Q>& vector)
{
    return os << glm::to_string(vector);
}

// core logging marcos
#define CORE_TRACE(...) ::Kans::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define CORE_INFO(...) ::Kans::Log::GetCoreLogger()->info(__VA_ARGS__)
#define CORE_WARN(...) ::Kans::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define CORE_ERROR(...) ::Kans::Log::GetCoreLogger()->error(__VA_ARGS__)
#define CORE_CRITICAL(...) ::Kans::Log::GetCoreLogger()->critical(__VA_ARGS__)

// client logging marcos
#define CLIENT_TRACE(...) ::Kans::Log::GetClientLogger()->trace(__VA_ARGS__)
#define CLIENT_INFO(...) ::Kans::Log::GetClientLogger()->info(__VA_ARGS__)
#define CLIENT_WARN(...) ::Kans::Log::GetClientLogger()->warn(__VA_ARGS__)
#define CLIENT_ERROR(...) ::Kans::Log::GetClientLogger()->error(__VA_ARGS__)
#define CLIENT_CRITICAL(...) ::Kans::Log::GetClientLogger()->critical(__VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tagged logs
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define CORE_TRACE_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Core, ::Kans::Log::Level::Trace, tag, __VA_ARGS__)
#define CORE_INFO_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Core, ::Kans::Log::Level::Info, tag, __VA_ARGS__)
#define CORE_WARN_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Core, ::Kans::Log::Level::Warn, tag, __VA_ARGS__)
#define CORE_ERROR_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Core, ::Kans::Log::Level::Error, tag, __VA_ARGS__)
#define CORE_CRITICAL_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Core, ::Kans::Log::Level::Critical, tag, __VA_ARGS__)

#define CLIENT_TRACE_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Client, ::Kans::Log::Level::Trace, tag, __VA_ARGS__)
#define CLIENT_INFO_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Client, ::Kans::Log::Level::Info, tag, __VA_ARGS__)
#define CLIENT_WARN_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Client, ::Kans::Log::Level::Warn, tag, __VA_ARGS__)
#define CLIENT_ERROR_TAG(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Client, ::Kans::Log::Level::Error, tag, __VA_ARGS__)
#define CLIENT_CRITICA_TAGL(tag, ...) \
    ::Kans::Log::PrintMessage(::Kans::Log::Type::Client, ::Kans::Log::Level::Critical, tag, __VA_ARGS__)
namespace Kans
{
    template<typename... Args>
    void Log::PrintMessage(Log::Type type, Log::Level level, std::string_view tag, Args&&... args)
    {
        auto logger = type == Log::Type::Core ? Log::GetCoreLogger() : Log::GetClientLogger();

        std::string logStringFMT = tag.empty() ? "{0}{1}" : "[{0}] {1}";
        switch (level)
        {
            case Kans::Log::Level::Trace:
                logger->trace(logStringFMT, tag, fmt::format(std::forward<Args>(args)...));
                break;
            case Kans::Log::Level::Info:
                logger->info(logStringFMT, tag, fmt::format(std::forward<Args>(args)...));
                break;
            case Kans::Log::Level::Warn:
                logger->warn(logStringFMT, tag, fmt::format(std::forward<Args>(args)...));
                break;
            case Kans::Log::Level::Error:
                logger->error(logStringFMT, tag, fmt::format(std::forward<Args>(args)...));
                break;
            case Kans::Log::Level::Critical:
                logger->critical(logStringFMT, tag, fmt::format(std::forward<Args>(args)...));
                break;
        }
    }

    template<typename... Args>
    void Log::PrintAssertMessage(Log::Type type, std::string string, Args&&... args)
    {
        auto logger = type == Log::Type::Core ? Log::GetCoreLogger() : Log::GetClientLogger();
        logger->error("{0}: {1}", string, fmt::format(std::forward<Args>(args)...));
    }

    template<>
    inline void Log::PrintAssertMessage(Log::Type type, std::string string)
    {
        auto logger = type == Log::Type::Core ? Log::GetCoreLogger() : Log::GetClientLogger();
        logger->error("{0}", string);
    }

} // namespace Kans
