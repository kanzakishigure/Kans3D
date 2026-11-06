#include "EditorProcess.h"
#include <cstring>
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

    std::optional<std::string> LaunchWithProject(const std::filesystem::path& project)
    {
        const auto executable = KansFileSystem::GetExecutablePath();
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
        std::wstring command = quote(executable.wstring()) + L" --project " + quote(project.wstring());
        STARTUPINFOW        startup {sizeof(startup)};
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            executable.parent_path().c_str(), &startup, &process))
            return "Cannot start the editor (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        const auto  program = executable.string();
        const auto  path    = project.string();
        const char* argv[]  = {program.c_str(), "--project", path.c_str(), nullptr};
        pid_t       child   = 0;
        if (int status = posix_spawn(&child, program.c_str(), nullptr, nullptr, const_cast<char**>(argv), environ))
            return "Cannot start the editor: " + std::string(std::strerror(status));
#endif
        return std::nullopt;
    }
} // namespace Kans::EditorProcess
