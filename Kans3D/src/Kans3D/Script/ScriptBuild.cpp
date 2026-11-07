#include "kspch.h"
#include "ScriptBuild.h"
#include <regex>
#if defined(_WIN32)
#include <Windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>
extern char** environ;
#endif
#ifndef KANS_DOTNET_EXECUTABLE
#define KANS_DOTNET_EXECUTABLE "dotnet"
#endif

namespace Kans
{
    namespace
    {
        // .NET tools localise their output; English keeps it ASCII for the editor console.
        constexpr std::pair<const char*, const char*> DotnetEnvironment[] = {{"DOTNET_CLI_UI_LANGUAGE", "en"},
                                                                             {"VSLANG", "1033"}};

#if defined(_WIN32)
        // The current environment block with DotnetEnvironment replacing any inherited values.
        std::wstring ChildEnvironment()
        {
            std::wstring block;
            if (auto* inherited = GetEnvironmentStringsW())
            {
                for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1)
                {
                    std::wstring_view variable(entry);
                    bool              overridden = false;
                    for (const auto& [name, value] : DotnetEnvironment)
                    {
                        const auto prefix = std::filesystem::u8path(std::string(name) + "=").wstring();
                        overridden |= variable.size() >= prefix.size() &&
                                      CompareStringOrdinal(variable.data(), int(prefix.size()), prefix.data(),
                                                           int(prefix.size()), TRUE) == CSTR_EQUAL;
                    }
                    if (!overridden)
                        block.append(variable).push_back(L'\0');
                }
                FreeEnvironmentStringsW(inherited);
            }
            for (const auto& [name, value] : DotnetEnvironment)
                block.append(std::filesystem::u8path(std::string(name) + "=" + value).wstring()).push_back(L'\0');
            block.push_back(L'\0');
            return block;
        }

        // CommandLineToArgvW/CRT escaping, including quotes and trailing backslashes.
        std::wstring Quote(const std::wstring& argument)
        {
            std::wstring result  = L"\"";
            size_t       slashes = 0;
            for (wchar_t ch : argument)
            {
                if (ch == L'\\')
                {
                    ++slashes;
                    continue;
                }
                result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
                slashes = 0;
                if (ch == L'"')
                    result += L'\\';
                result += ch;
            }
            result.append(slashes * 2, L'\\');
            return result + L'"';
        }
#else
        std::vector<std::string> ChildEnvironment()
        {
            std::vector<std::string> variables;
            for (char** entry = environ; *entry; ++entry)
            {
                std::string_view variable(*entry);
                bool             overridden = false;
                for (const auto& [name, value] : DotnetEnvironment)
                    overridden |= variable.rfind(std::string(name) + "=", 0) == 0;
                if (!overridden)
                    variables.emplace_back(variable);
            }
            for (const auto& [name, value] : DotnetEnvironment)
                variables.push_back(std::string(name) + "=" + value);
            return variables;
        }
#endif
    } // namespace
    std::vector<std::string> ScriptBuild::Arguments(const std::filesystem::path& csproj,
                                                    Config                       config,
                                                    const std::filesystem::path& scriptCoreDir)
    {
        std::vector<std::string> args {KANS_DOTNET_EXECUTABLE,
                                       "build",
                                       std::filesystem::absolute(csproj).u8string(),
                                       "-c",
                                       config == Config::Debug ? "Debug" : "Release",
                                       "-m:1",
                                       "-nr:false",
                                       "--nologo",
                                       "-p:UseSharedCompilation=false"};
        if (!scriptCoreDir.empty())
            args.push_back("-p:KansScriptCoreDir=" + std::filesystem::absolute(scriptCoreDir).generic_u8string());
        return args;
    }

    ScriptBuild::Step ScriptBuild::StepForLine(std::string_view line, Step previous)
    {
        if (previous == Step::Done)
            return previous;
        if (line.find("Build succeeded.") != line.npos || line.find("Build FAILED.") != line.npos)
            return Step::Done;
        if (line.find("Determining projects to restore") != line.npos || line.find("Restored ") != line.npos ||
            line.find("for restore.") != line.npos || line.find("Restoring packages for ") != line.npos)
            return Step::Restoring;
        if (line.find("Copying file from ") != line.npos || line.find(" -> ") != line.npos ||
            line.find("CopyFilesToOutputDirectory:") != line.npos || line.find("Linking ") != line.npos)
            return Step::LinkingCopying;
        if (line.find("CoreCompile:") != line.npos || line.find("csc.dll") != line.npos ||
            line.find(": warning CS") != line.npos || line.find(": error CS") != line.npos)
            return Step::Compiling;
        return previous;
    }

    ScriptBuild::Result ScriptBuild::Build(const std::filesystem::path& csproj,
                                           Config                       config,
                                           const std::filesystem::path& scriptCoreDir,
                                           LineCallback                 onLine)
    {
        Result result;
        auto   args = Arguments(csproj, config, scriptCoreDir);
        std::string pending;
        const auto  capture = [&](const char* data, size_t size) {
            result.Output.append(data, size);
            if (!onLine)
                return;
            pending.append(data, size);
            size_t start = 0, end;
            while ((end = pending.find('\n', start)) != std::string::npos)
            {
                size_t length = end - start;
                if (length && pending[end - 1] == '\r')
                    --length;
                onLine(std::string_view(pending).substr(start, length));
                start = end + 1;
            }
            pending.erase(0, start);
        };
#if defined(_WIN32)
        SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE              read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 0))
            return result;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        std::wstring command;
        for (const auto& argument : args)
        {
            if (!command.empty())
                command += L' ';
            command += Quote(std::filesystem::u8path(argument).wstring());
        }
        STARTUPINFOW startup {};
        startup.cb         = sizeof(startup);
        startup.dwFlags    = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError  = write;
        startup.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process {};
        auto                executable = std::filesystem::u8path(args[0]).wstring();
        auto                environment = ChildEnvironment();
        bool                started    = CreateProcessW(executable.c_str(),
                                                        command.data(),
                                                        nullptr,
                                                        nullptr,
                                                        TRUE,
                                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                                        environment.data(),
                                                        nullptr,
                                                        &startup,
                                                        &process);
        CloseHandle(write);
        if (!started)
        {
            CloseHandle(read);
            result.Output = "CreateProcessW failed: " + std::to_string(GetLastError());
            return result;
        }
        char  buffer[4096];
        DWORD count;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count)
            capture(buffer, count);
        CloseHandle(read);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code;
        GetExitCodeProcess(process.hProcess, &code);
        result.ExitCode = int(code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        int pipeFD[2];
        if (pipe(pipeFD) != 0)
            return result;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipeFD[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipeFD[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipeFD[0]);
        posix_spawn_file_actions_addclose(&actions, pipeFD[1]);
        std::vector<char*> argv;
        for (auto& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        auto               variables = ChildEnvironment();
        std::vector<char*> envp;
        for (auto& variable : variables)
            envp.push_back(variable.data());
        envp.push_back(nullptr);
        pid_t process;
        int   status = posix_spawnp(&process, argv[0], &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        close(pipeFD[1]);
        if (status != 0)
        {
            close(pipeFD[0]);
            result.Output = std::strerror(status);
            return result;
        }
        char buffer[4096];
        for (;;)
        {
            auto count = read(pipeFD[0], buffer, sizeof(buffer));
            if (count > 0)
                capture(buffer, size_t(count));
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        close(pipeFD[0]);
        int code;
        while (waitpid(process, &code, 0) < 0)
        {
            if (errno != EINTR)
                return result;
        }
        result.ExitCode = WIFEXITED(code) ? WEXITSTATUS(code) : -1;
#endif
        if (onLine && !pending.empty())
        {
            if (pending.back() == '\r')
                pending.pop_back();
            onLine(pending);
        }
        result.Success = result.ExitCode == 0;
        const std::regex   pattern(R"(^(.+)\((\d+),(\d+)(?:,\d+,\d+)?\): (error|warning) ([A-Z]+\d+): (.*)$)");
        std::istringstream lines(result.Output);
        std::string        line;
        std::unordered_set<std::string> seen, errors;
        const std::regex                errorLine(R"(:\s*error [A-Z]+\d+:)");
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::smatch match;
            if (std::regex_search(line, errorLine))
                errors.insert(line.substr(line.find_first_not_of(" \t")));
            static const std::regex errorSummary(R"(^\s*(\d+) Error\(s\)\s*$)");
            if (std::regex_match(line, match, errorSummary))
                result.ErrorCount = unsigned(std::stoul(match[1]));
            if (!std::regex_match(line, match, pattern) || !seen.insert(line).second)
                continue;
            result.Diagnostics.push_back({std::filesystem::u8path(match[1].str()),
                                          std::stoi(match[2]),
                                          std::stoi(match[3]),
                                          match[4],
                                          match[5],
                                          match[6]});
        }
        result.ErrorCount = std::max(result.ErrorCount, unsigned(errors.size()));
        return result;
    }
} // namespace Kans
