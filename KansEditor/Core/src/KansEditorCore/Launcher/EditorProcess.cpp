#include "KansEditorCore/Launcher/EditorProcess.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
extern char** environ;
#endif

namespace Kans::EditorProcess
{
    std::vector<std::string> Arguments(int argc, char** argv)
    {
#if defined(_WIN32)
        int   count = 0;
        auto* wide  = CommandLineToArgvW(GetCommandLineW(), &count);
        if (wide)
        {
            std::vector<std::string> arguments;
            for (int i = 0; i < count; ++i)
                arguments.push_back(std::filesystem::path(wide[i]).u8string());
            LocalFree(wide);
            return arguments;
        }
#endif
        return {argv, argv + argc};
    }

    bool OpensInProcess()
    {
        if (const char* forced = std::getenv("KANS_EDITOR_IN_PROCESS"); forced && std::strcmp(forced, "1") == 0)
            return true;
#if defined(_WIN32)
        return IsDebuggerPresent() != FALSE;
#else
        // A traced process (gdb, lldb) reports its tracer's PID; 0 means none.
        std::ifstream status("/proc/self/status");
        std::string   line;
        while (std::getline(status, line))
            if (line.rfind("TracerPid:", 0) == 0)
                return std::strtol(line.c_str() + 10, nullptr, 10) != 0;
        return false;
#endif
    }

    namespace
    {
        // Another editor process, with `--project <project>` when one is given.
        std::optional<std::string> Spawn(const std::optional<std::filesystem::path>& project)
        {
            std::filesystem::path executable;
            try
            {
                executable = KansFileSystem::GetExecutablePath();
            }
            catch (const std::exception& error)
            {
                return "Cannot find the editor executable: " + std::string(error.what());
            }
#if defined(_WIN32)
        // Quote per CommandLineToArgvW rules; paths cannot contain '"' on Windows.
        auto quote = [](const std::wstring& text) {
            std::wstring quoted = L"\"";
            for (size_t i = 0; i < text.size(); ++i)
            {
                quoted += text[i];
                if (text[i] == L'\\' && i + 1 == text.size())
                    quoted += L'\\'; // A trailing backslash would escape the closing quote.
            }
            return quoted + L"\"";
        };
        std::wstring command = quote(executable.wstring());
        if (project)
            command += L" --project " + quote(project->wstring());
        STARTUPINFOW        startup {sizeof(startup)};
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            executable.parent_path().c_str(), &startup, &process))
            return "Cannot start the editor (error " + std::to_string(GetLastError()) + ")";
        // This process closes right after: let the new window come to the front.
        AllowSetForegroundWindow(process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        const auto  program = executable.string();
        const auto  path    = project ? project->string() : std::string();
        const char* argv[]  = {program.c_str(), project ? "--project" : nullptr, path.c_str(), nullptr};
        pid_t       child   = 0;
        if (int status = posix_spawn(&child, program.c_str(), nullptr, nullptr, const_cast<char**>(argv), environ))
            return "Cannot start the editor: " + std::string(std::strerror(status));
#endif
        return std::nullopt;
        }
    } // namespace

    std::optional<std::string> LaunchWithProject(const std::filesystem::path& requested)
    {
        // The new process starts in another folder (the executable's on Windows).
        std::error_code ec;
        auto            project = std::filesystem::absolute(requested, ec);
        if (ec)
            project = requested;
        return Spawn(project);
    }

    std::optional<std::string> LaunchHub() { return Spawn(std::nullopt); }
} // namespace Kans::EditorProcess
