#include "KansEditorCore/Launcher/BenchmarkSession.h"
#include <ctime>
#include <deque>
#include <fstream>
#include <thread>
#include "Kans3D/Project/ProjectFile.h"

namespace Kans::Launcher
{
    namespace fs = std::filesystem;
    using Clock  = std::chrono::steady_clock;

    namespace
    {
        auto LockHistory(const fs::path& directory)
        {
            const auto file = directory / Benchmark::HistoryLockFile;
            auto       lock = ProductStoreLock::Acquire(file);
            for (int attempt = 0; attempt < 10 && std::holds_alternative<AssetError>(lock); ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                lock = ProductStoreLock::Acquire(file);
            }
            return lock;
        }
    } // namespace

    std::vector<std::string> ReadLogTail(const fs::path& log, size_t count)
    {
        std::ifstream           input(log, std::ios::binary);
        std::deque<std::string> lines;
        for (std::string line; std::getline(input, line);)
        {
            std::string clean;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[')
                {
                    i += 2;
                    while (i < line.size() && !(line[i] >= '@' && line[i] <= '~'))
                        ++i;
                    continue;
                }
                if (line[i] != '\r')
                    clean += line[i];
            }
            if (clean.find_first_not_of(" \t") == std::string::npos)
                continue;
            lines.push_back(std::move(clean));
            if (lines.size() > count)
                lines.pop_front();
        }
        return {lines.begin(), lines.end()};
    }

    BenchmarkSession::~BenchmarkSession()
    {
        if (IsRunning())
            Cancel();
    }

    std::optional<std::string> BenchmarkSession::Start(const Request& request)
    {
        if (IsRunning())
            return "A benchmark is already running.";
        m_State         = State::Idle;
        const auto root = request.ProjectFile.parent_path();
        if (auto problem = Benchmark::SettingsProblem(request.Settings, root))
            return problem;
        auto loaded = LoadProjectFile(request.ProjectFile);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return error->Message;
        const auto& project = std::get<ProjectFile>(loaded);

        m_Request = request;
        m_Error.clear();
        m_LogTail.clear();
        m_Progress.reset();
        m_Child.reset();
        m_CreatedUnix = int64_t(std::time(nullptr));
        const auto      directory = Benchmark::BenchmarksDirectory(root);
        std::error_code error;
        fs::create_directories(directory, error);
        if (error)
            return "Cannot create " + directory.u8string() + ": " + error.message();
        {
            // Publish the folder and its live-run lock together with respect to history sweeps.
            auto historyLock = LockHistory(directory);
            if (const auto* failed = std::get_if<AssetError>(&historyLock))
                return failed->Message;
            const auto base =
                directory / fs::u8path(Benchmark::ResultFolderName(m_CreatedUnix, request.Settings.Scene));
            m_Folder = base;
            for (int suffix = 2; !fs::create_directory(m_Folder, error); ++suffix)
            {
                if (error)
                    return "Cannot create " + m_Folder.u8string() + ": " + error.message();
                m_Folder = fs::path(base.native() + fs::u8path("-" + std::to_string(suffix)).native());
            }
            auto runLock = ProductStoreLock::Acquire(m_Folder / Benchmark::LiveRunLockFile);
            if (const auto* failed = std::get_if<AssetError>(&runLock))
            {
                fs::remove_all(m_Folder, error);
                return failed->Message;
            }
            m_RunLock = std::move(std::get<Scope<ProductStoreLock>>(runLock));
        }
        m_WorkDirectory = (root / project.CacheDirectory / "Editor" / "Benchmark").lexically_normal();
        fs::create_directories(m_WorkDirectory, error);
        if (error)
        {
            Discard();
            return "Cannot create " + m_WorkDirectory.u8string() + ": " + error.message();
        }
        m_Started = Clock::now();
        if (auto failed = StartRun(1))
        {
            Discard();
            return failed;
        }
        m_State = State::Running;
        return std::nullopt;
    }

    std::optional<std::string> BenchmarkSession::StartRun(uint32_t run)
    {
        m_Run          = run;
        m_ProgressFile = m_WorkDirectory / "progress.txt";
        m_LogFile      = m_WorkDirectory / ("run-" + std::to_string(run) + ".log");
        std::error_code error;
        fs::remove(m_ProgressFile, error);
        m_Progress.reset();
        const auto output = m_Folder / ("run-" + std::to_string(run) + ".json");
        auto started = EditorProcess::ChildProcess::Start(m_Request.Program,
                                                          Benchmark::RunArguments(fs::absolute(m_Request.ProjectFile),
                                                                                  m_Request.Settings,
                                                                                  fs::absolute(output),
                                                                                  fs::absolute(m_ProgressFile)),
                                                          m_LogFile);
        if (auto* message = std::get_if<std::string>(&started))
            return *message;
        m_Child = std::move(std::get<std::unique_ptr<EditorProcess::ChildProcess>>(started));
        return std::nullopt;
    }

    bool BenchmarkSession::Poll()
    {
        if (!IsRunning() || !m_Child)
            return false;
        bool changed = false;
        if (auto progress = Benchmark::ReadProgress(m_ProgressFile); progress && progress != m_Progress)
        {
            m_Progress = progress;
            changed    = true;
        }
        const auto exit = m_Child->Poll();
        if (!exit)
            return changed;
        m_Child.reset();
        // The last lines a run writes (writing, done) may land between the read above and its exit.
        if (auto final = Benchmark::ReadProgress(m_ProgressFile); final && final != m_Progress)
            m_Progress = final;
        const auto      report = m_Folder / ("run-" + std::to_string(m_Run) + ".json");
        std::error_code error;
        // 2 is a regression against a baseline the CLI was given; the launcher compares itself.
        if (*exit != 0 && *exit != 2)
            Fail("Run " + std::to_string(m_Run) + " of " + std::to_string(Runs()) + " failed (exit code " +
                 std::to_string(*exit) + ").");
        else if (!fs::is_regular_file(report, error))
            Fail("Run " + std::to_string(m_Run) + " wrote no report.");
        else if (m_Run < Runs())
        {
            if (auto failed = StartRun(m_Run + 1))
                Fail(*failed);
        }
        else
            Finish();
        return true;
    }

    void BenchmarkSession::Finish()
    {
        try
        {
            std::vector<YAML::Node>  runs;
            std::vector<std::string> files;
            for (uint32_t run = 1; run <= Runs(); ++run)
            {
                files.push_back("run-" + std::to_string(run) + ".json");
                runs.push_back(Benchmark::ReadReport(m_Folder / files.back()));
            }
            auto summary = Benchmark::CombineRuns(runs, files, m_CreatedUnix);
            if (const auto& baseline = m_Request.Settings.Baseline; !baseline.empty())
            {
                summary["runs"]["baseline"] = Benchmark::JsonString(baseline.u8string());
                try
                {
                    const auto threshold  = m_Request.Settings.ThresholdPercent;
                    const auto comparison = Benchmark::Compare(Benchmark::ReadReport(baseline), summary, threshold);
                    summary["comparison"] = Benchmark::ComparisonJson(comparison, threshold);
                }
                catch (const std::exception& error)
                {
                    // The result stands without it; the window says why there is no comparison.
                    summary["runs"]["comparison_error"] = Benchmark::JsonString(error.what());
                }
            }
            Benchmark::WriteReport(SummaryFile(), summary);
        }
        catch (const std::exception& error)
        {
            Fail(std::string("Cannot combine the runs: ") + error.what());
            return;
        }
        const auto historyLock = LockHistory(m_Folder.parent_path());
        m_RunLock.reset();
        if (std::holds_alternative<Scope<ProductStoreLock>>(historyLock))
        {
            std::error_code ignored;
            fs::remove(m_Folder / Benchmark::LiveRunLockFile, ignored);
        }
        m_Ended = Clock::now();
        m_State = State::Succeeded;
    }

    void BenchmarkSession::Fail(std::string error)
    {
        m_LogTail = ReadLogTail(m_LogFile, LogTailLines);
        m_Error   = std::move(error);
        Discard();
        m_State = State::Failed;
    }

    void BenchmarkSession::Cancel()
    {
        if (!IsRunning())
            return;
        Discard();
        m_State = State::Cancelled;
    }

    void BenchmarkSession::Discard()
    {
        if (m_Child)
            m_Child->Kill();
        m_Child.reset();
        m_Ended = Clock::now();
        std::error_code error;
        if (m_RunLock)
        {
            // Keep the live marker until the child has exited. The directory lock
            // keeps history from removing the folder during the Windows handle close.
            const auto historyLock = LockHistory(m_Folder.parent_path());
            m_RunLock.reset();
            if (std::holds_alternative<Scope<ProductStoreLock>>(historyLock) &&
                fs::symlink_status(SummaryFile(), error).type() == fs::file_type::not_found)
                fs::remove_all(m_Folder, error);
        }
        fs::remove(m_ProgressFile, error);
    }

    double BenchmarkSession::ElapsedSeconds() const
    {
        if (m_State == State::Idle)
            return 0;
        const auto end = IsRunning() ? Clock::now() : m_Ended;
        return std::chrono::duration<double>(end - m_Started).count();
    }

    double BenchmarkSession::CompletedFraction() const
    {
        if (m_State == State::Succeeded)
            return 1;
        if (!m_Progress)
            return Benchmark::CompletedFraction(
                m_Run, Runs(), {}, m_Request.Settings.Warmup, m_Request.Settings.Frames);
        return Benchmark::CompletedFraction(
            m_Run, Runs(), *m_Progress, m_Request.Settings.Warmup, m_Request.Settings.Frames);
    }

    std::optional<double> BenchmarkSession::RemainingSeconds() const
    {
        if (!IsRunning())
            return std::nullopt;
        return Benchmark::RemainingSeconds(ElapsedSeconds(), CompletedFraction());
    }
} // namespace Kans::Launcher
