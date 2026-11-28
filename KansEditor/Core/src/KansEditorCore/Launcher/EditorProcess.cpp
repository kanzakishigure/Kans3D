#include "KansEditorCore/Launcher/EditorProcess.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>
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

    namespace
    {
#if defined(_WIN32)
        // One argument for CommandLineToArgvW and the CRT: quoted when needed, with the
        // backslashes before a quote (or the closing quote) doubled.
        std::wstring QuoteArgument(const std::wstring& argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return argument;
            std::wstring quoted = L"\"";
            for (size_t i = 0;; ++i)
            {
                size_t backslashes = 0;
                while (i < argument.size() && argument[i] == L'\\')
                {
                    ++i;
                    ++backslashes;
                }
                if (i == argument.size())
                {
                    quoted.append(backslashes * 2, L'\\');
                    break;
                }
                quoted.append(argument[i] == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
                quoted.push_back(argument[i]);
            }
            quoted.push_back(L'"');
            return quoted;
        }
#else
        std::optional<int> ExitCode(int status)
        {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return std::nullopt;
        }
#endif
    } // namespace

    std::variant<std::unique_ptr<ChildProcess>, std::string>
    ChildProcess::Start(const std::filesystem::path&    program,
                        const std::vector<std::string>& arguments,
                        const std::filesystem::path&    log)
    {
        std::unique_ptr<ChildProcess> child(new ChildProcess());
#if defined(_WIN32)
        SECURITY_ATTRIBUTES inheritable {sizeof(inheritable), nullptr, TRUE};
        HANDLE              output = CreateFileW(log.c_str(),
                                                 GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 &inheritable,
                                                 CREATE_ALWAYS,
                                                 FILE_ATTRIBUTE_NORMAL,
                                                 nullptr);
        if (output == INVALID_HANDLE_VALUE)
            return "Cannot create " + log.u8string() + " (error " + std::to_string(GetLastError()) + ")";
        HANDLE input = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
        // Only these two handles reach the child: none of this process's other inheritable ones.
        HANDLE handles[2] = {output, input};
        SIZE_T size       = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> storage(size);
        auto*             attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        const bool        initialized =
            input != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE;
        bool ok =
            initialized &&
            UpdateProcThreadAttribute(
                attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) != FALSE;
        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb         = sizeof(startup);
        startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput  = input;
        startup.StartupInfo.hStdOutput = output;
        startup.StartupInfo.hStdError  = output;
        startup.lpAttributeList        = attributes;
        std::wstring command           = QuoteArgument(program.wstring());
        for (const auto& argument : arguments)
            command += L" " + QuoteArgument(std::filesystem::u8path(argument).wstring());
        PROCESS_INFORMATION process {};
        ok               = ok && CreateProcessW(program.c_str(),
                                                command.data(),
                                                nullptr,
                                                nullptr,
                                                TRUE,
                                                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                                nullptr,
                                                program.parent_path().c_str(),
                                                &startup.StartupInfo,
                                                &process) != FALSE;
        const auto error = GetLastError();
        if (initialized)
            DeleteProcThreadAttributeList(attributes);
        CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
        if (!ok)
            return "Cannot start " + program.u8string() + " (error " + std::to_string(error) + ")";
        // The job ends the child with this process, even when it crashes.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job, process.hProcess))
            {
                CloseHandle(job);
                job = nullptr;
            }
        }
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        child->m_Process = process.hProcess;
        child->m_Job     = job;
        child->m_Id      = int64_t(process.dwProcessId);
#else
        const auto logFile = log.string();
        // Created here first, so a bad log path is reported instead of a silent child failure.
        const int probe = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (probe < 0)
            return "Cannot create " + log.u8string() + ": " + std::strerror(errno);
        close(probe);
        const auto         path = program.string();
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(path.c_str()));
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, logFile.c_str(), O_WRONLY | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        pid_t     pid    = 0;
        const int status = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (status != 0)
            return "Cannot start " + program.u8string() + ": " + std::strerror(status);
        child->m_Id = int64_t(pid);
#endif
        return child;
    }

    std::optional<int> ChildProcess::Poll()
    {
        if (m_Exit)
            return m_Exit;
#if defined(_WIN32)
        DWORD code = 0;
        if (m_Process && WaitForSingleObject(m_Process, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(m_Process, &code))
            m_Exit = int(code);
#else
        int status = 0;
        if (m_Id > 0 && waitpid(pid_t(m_Id), &status, WNOHANG) == pid_t(m_Id))
            m_Exit = ExitCode(status).value_or(1);
#endif
        return m_Exit;
    }

    void ChildProcess::Kill()
    {
        if (Poll())
            return;
#if defined(_WIN32)
        if (!m_Process)
            return;
        TerminateProcess(m_Process, 1);
        WaitForSingleObject(m_Process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(m_Process, &code);
        m_Exit = int(code);
#else
        if (m_Id <= 0)
            return;
        kill(pid_t(m_Id), SIGKILL);
        int status = 0;
        while (waitpid(pid_t(m_Id), &status, 0) < 0 && errno == EINTR)
        {
        }
        m_Exit = ExitCode(status).value_or(128 + SIGKILL);
#endif
    }

    ChildProcess::~ChildProcess()
    {
        Kill();
#if defined(_WIN32)
        if (m_Process)
            CloseHandle(m_Process);
        if (m_Job)
            CloseHandle(m_Job);
#endif
    }
} // namespace Kans::EditorProcess
