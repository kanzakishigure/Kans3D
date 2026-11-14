#pragma once

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace Kans::FatalError
{
    bool IsDebuggerAttached() noexcept;
    // Call after enabling the log file, before creating the application. No-op under a debugger.
    void Install();
    void Shutdown() noexcept;
    void SetShowDialog(bool show) noexcept;

    std::string       FormatReport(const std::string&              type,
                                   const std::string&              message,
                                   const std::filesystem::path&    logPath,
                                   const std::string&              dumpPath,
                                   const std::vector<std::string>& frames);
    [[noreturn]] void ReportException(std::exception_ptr exception, const char* origin = "Kans::Main") noexcept;
} // namespace Kans::FatalError
