#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "KansEditorCore/Launcher/EditorProcess.h"
#include "KansEditorCore/Profiling/BenchmarkHub.h"

namespace Kans::Launcher
{
    // The launcher's benchmark runs for one project: each repeat is a separate editor process
    // running the --benchmark CLI, one after another, writing
    // <project>/Benchmarks/<timestamp>-<scene>/run-<k>.json; after the last one the session
    // writes summary.json there (CombineRuns, compared with the baseline when one is set).
    // A failed run or Cancel kills the process and removes the folder: no partial result stays.
    class BenchmarkSession
    {
    public:
        enum class State
        {
            Idle,
            Running,
            Succeeded,
            Failed,
            Cancelled
        };
        struct Request
        {
            std::filesystem::path  Program; // The KansEditor executable.
            std::filesystem::path  ProjectFile;
            Benchmark::HubSettings Settings;
        };

        BenchmarkSession() = default;
        ~BenchmarkSession();
        BenchmarkSession(const BenchmarkSession&)            = delete;
        BenchmarkSession& operator=(const BenchmarkSession&) = delete;

        // Starts run 1; returns an error message (and stays Idle) when it cannot.
        std::optional<std::string> Start(const Request& request);
        // Reads the progress, notices a finished run and starts the next or writes the summary.
        // True when something shown changed. Cheap: call it on every wake-up while Running.
        bool Poll();
        void Cancel();

        State                                     GetState() const { return m_State; }
        bool                                      IsRunning() const { return m_State == State::Running; }
        uint32_t                                  CurrentRun() const { return m_Run; }
        uint32_t                                  Runs() const { return m_Request.Settings.Repeats; }
        const std::optional<Benchmark::Progress>& GetProgress() const { return m_Progress; }
        double                                    ElapsedSeconds() const;
        double                                    CompletedFraction() const;
        std::optional<double>                     RemainingSeconds() const;
        const std::filesystem::path&              ResultFolder() const { return m_Folder; }
        std::filesystem::path                     SummaryFile() const { return m_Folder / "summary.json"; }
        const std::string&                        GetError() const { return m_Error; }
        const std::vector<std::string>&           LogTail() const { return m_LogTail; }
        // The running child's process ID, or 0.
        int64_t ChildId() const { return m_Child ? m_Child->Id() : 0; }

        static constexpr size_t LogTailLines = 24;

    private:
        std::optional<std::string> StartRun(uint32_t run);
        void                       Fail(std::string error);
        void                       Finish();
        void                       Discard();

        Request                                      m_Request;
        State                                        m_State = State::Idle;
        uint32_t                                     m_Run   = 0;
        std::filesystem::path                        m_Folder, m_WorkDirectory, m_ProgressFile, m_LogFile;
        std::unique_ptr<EditorProcess::ChildProcess> m_Child;
        Scope<ProductStoreLock>                      m_RunLock;
        std::optional<Benchmark::Progress>           m_Progress;
        std::chrono::steady_clock::time_point        m_Started {}, m_Ended {};
        int64_t                                      m_CreatedUnix = 0;
        std::string                                  m_Error;
        std::vector<std::string>                     m_LogTail;
    };

    // The last `count` non-empty lines of a log, without terminal color codes.
    std::vector<std::string> ReadLogTail(const std::filesystem::path& log, size_t count);
} // namespace Kans::Launcher
