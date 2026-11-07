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
} // namespace Kans::EditorProcess
