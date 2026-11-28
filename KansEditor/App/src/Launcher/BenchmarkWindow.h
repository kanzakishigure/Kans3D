#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "KansEditorCore/Launcher/BenchmarkSession.h"
#include "UI/FolderBrowser.h"

namespace Kans
{
    // The project Hub's Benchmark window, a modal over the launcher for one project: setup
    // (scene, resolution, frames, repeats, baseline and threshold, remembered per project), the
    // run in progress, the combined result with its comparison, and the project's history.
    // The runs are separate editor processes (BenchmarkSession); this window only watches them.
    class BenchmarkWindow
    {
    public:
        struct RunStatus
        {
            uint32_t                           Run = 0, Runs = 0;
            std::optional<Benchmark::Progress> Progress;
            uint32_t                           Warmup = 0, Frames = 0;
            double                             Fraction = 0, ElapsedSeconds = 0;
            std::optional<double>              RemainingSeconds;
        };

        // `program` is the editor executable the runs start.
        explicit BenchmarkWindow(std::filesystem::path program);
        void Open(const std::filesystem::path& projectFile);
        bool IsOpen() const { return m_Open; }
        // Call every frame from the launcher's window while it is open.
        void Draw();
        bool IsRunning() const { return m_Session.IsRunning(); }
        // Polls the run between frames; true when the window needs a new frame.
        bool Poll() { return m_Session.Poll(); }
        // Shows a summary.json (or any report) as the result.
        void ShowResult(const std::filesystem::path& summary);
        // Draws `status` as the run in progress instead of the session's (layout tests).
        void PreviewRunning(std::optional<RunStatus> status) { m_Preview = std::move(status); }

        Launcher::BenchmarkSession&                 GetSession() { return m_Session; }
        Benchmark::HubSettings&                     GetSettings() { return m_Settings; }
        const std::vector<Benchmark::HistoryEntry>& GetHistory() const { return m_History; }
        const std::optional<Benchmark::ResultView>& GetResult() const { return m_Result; }
        // Starts the runs with the current settings (the Run button).
        void Start();

    private:
        void        RefreshHistory();
        void        SaveSettings();
        void        DrawSetup(float height);
        void        DrawHistory();
        void        DrawStatus();
        void        DrawRunning(const RunStatus& status);
        void        DrawFailure();
        void        DrawResult();
        void        DrawResultPasses();
        void        DrawComparison();
        void        DrawDialogs();
        std::string BaselineLabel() const;

        enum class PassSort
        {
            Name,
            Cpu,
            Gpu
        };

        std::filesystem::path                m_Program, m_ProjectFile, m_ProjectRoot, m_SettingsFile;
        std::string                          m_ProjectName, m_Error;
        bool                                 m_Open = false, m_OpenRequested = false;
        Launcher::BenchmarkSession           m_Session;
        Launcher::BenchmarkSession::State    m_SeenState = Launcher::BenchmarkSession::State::Idle;
        Benchmark::HubSettings               m_Settings;
        std::vector<std::string>             m_Scenes;
        std::vector<Benchmark::HistoryEntry> m_History;
        std::optional<Benchmark::ResultView> m_Result;
        std::filesystem::path                m_ResultFile;
        std::optional<RunStatus>             m_Preview;
        PassSort                             m_Sort      = PassSort::Gpu;
        bool                                 m_Ascending = false, m_CustomResolution = false;
        std::filesystem::path                m_PendingDelete;
        bool                                 m_AskDelete = false;
        FolderBrowser                        m_Browser;
    };
} // namespace Kans
