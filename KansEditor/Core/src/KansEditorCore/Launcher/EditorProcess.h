#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Kans::EditorProcess
{
    // Command-line arguments as UTF-8 (on Windows from the wide command line, not the ANSI argv).
    std::vector<std::string> Arguments(int argc, char** argv);
    // Starts another editor process for `project`; returns an error message on failure.
    std::optional<std::string> LaunchWithProject(const std::filesystem::path& project);
    // Starts another editor process without a project, which is the project Hub.
    std::optional<std::string> LaunchHub();
    // True when a project chosen in the Hub should open in this process instead of a new one:
    // a debugger is attached (it would not follow a child process), or KANS_EDITOR_IN_PROCESS=1.
    bool OpensInProcess();

    // A process this one starts and owns (a benchmark run): stdout and stderr go to `log`, it has
    // no console window and no other handle of ours, and it is killed when the owner goes away
    // (on Windows also when this process dies, through a kill-on-close job).
    class ChildProcess
    {
    public:
        ~ChildProcess();
        ChildProcess(const ChildProcess&)            = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;
        // `arguments` exclude the program; UTF-8. Returns an error message on failure.
        static std::variant<std::unique_ptr<ChildProcess>, std::string> Start(const std::filesystem::path&    program,
                                                                              const std::vector<std::string>& arguments,
                                                                              const std::filesystem::path&    log);
        // The exit code once the process has ended (128 + signal when killed by one), else nothing.
        std::optional<int> Poll();
        // Ends the process and waits until it is gone. No-op once it has exited.
        void    Kill();
        int64_t Id() const { return m_Id; }

    private:
        ChildProcess()          = default;
        int64_t            m_Id = 0;
        std::optional<int> m_Exit;
#if defined(_WIN32)
        void* m_Process = nullptr;
        void* m_Job     = nullptr;
#endif
    };
} // namespace Kans::EditorProcess
