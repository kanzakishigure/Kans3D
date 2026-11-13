#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Kans::EditorProcess
{
    // Command-line arguments as UTF-8 (on Windows from the wide command line, not the ANSI argv).
    std::vector<std::string> Arguments(int argc, char** argv);
    // Starts another editor process for `project`; returns an error message on failure.
    std::optional<std::string> LaunchWithProject(const std::filesystem::path& project);
    // True when a project chosen in the Hub should open in this process instead of a new one:
    // a debugger is attached (it would not follow a child process), or KANS_EDITOR_IN_PROCESS=1.
    bool OpensInProcess();
} // namespace Kans::EditorProcess
