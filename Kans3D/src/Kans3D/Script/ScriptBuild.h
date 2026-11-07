#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Kans
{
    class ScriptBuild
    {
    public:
        enum class Step
        {
            Restoring,
            Compiling,
            LinkingCopying,
            Done
        };
        // Pure English dotnet/MSBuild output classifier. Unrecognized lines retain previous.
        static Step StepForLine(std::string_view line, Step previous);
        using LineCallback = std::function<void(std::string_view)>;
        enum class Config
        {
            Debug,
            Release
        };
        struct Diagnostic
        {
            std::filesystem::path File;
            int                   Line = 0, Column = 0;
            std::string           Severity, Code, Message;
        };
        struct Result
        {
            bool                    Success  = false;
            int                     ExitCode = -1;
            unsigned                ErrorCount = 0;
            std::string             Output;
            std::vector<Diagnostic> Diagnostics;
        };
        // Includes the executable; an empty ScriptCore directory preserves the csproj default.
        static std::vector<std::string>
        Arguments(const std::filesystem::path& csproj, Config config, const std::filesystem::path& scriptCoreDir = {});
        // Executes Arguments directly, with combined stdout/stderr capture.
        // Optional callback runs on the calling/build thread, once per line (without
        // CR/LF), including a final unterminated line. It must not throw; views are transient.
        static Result Build(const std::filesystem::path& csproj,
                            Config                       config,
                            const std::filesystem::path& scriptCoreDir = {},
                            LineCallback                 onLine        = {});
    };
} // namespace Kans
