#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "KansEditorCore/Profiling/BenchmarkProgress.h"
#include "KansEditorCore/Profiling/BenchmarkReport.h"

// The launcher's Benchmark window without UI: the remembered setup, the run arguments, how the
// repeats are combined into summary.json, the result history and what a result shows.
namespace Kans::Benchmark
{
    inline constexpr uint32_t MaxRepeats = 20;
    // A run is unstable when the run-to-run spread of its frame CPU or GPU mean (max - min) is
    // over this share of their median and also over AbsoluteFloorMs.
    inline constexpr double UnstableSpreadPercent = 3.0;

    struct HubSettings
    {
        std::string           Scene = ComplexScene;
        uint32_t              Width = 2560, Height = 1440;
        uint32_t              Frames = 360, Warmup = 120, Repeats = 3;
        double                ThresholdPercent = 5;
        ComplexOptions        Complex;
        std::filesystem::path Baseline; // A summary.json or a CLI report; empty: no comparison.
    };

    // <project>/<cache>/Editor/Benchmark.yaml: the last choices of the project's Benchmark window.
    std::filesystem::path HubSettingsFile(const std::filesystem::path& projectRoot,
                                          const std::filesystem::path& cacheDirectory);
    // Missing or out-of-range fields (or an unreadable file) keep their defaults.
    HubSettings                LoadHubSettings(const std::filesystem::path& file);
    std::optional<std::string> SaveHubSettings(const std::filesystem::path& file, const HubSettings& settings);
    // Why a run cannot start with these settings, or nothing.
    std::optional<std::string> SettingsProblem(const HubSettings& settings, const std::filesystem::path& projectRoot);

    // The project's .kans scenes under its asset folder, as project-relative generic paths, sorted.
    std::vector<std::string> ListScenes(const std::filesystem::path& projectRoot,
                                        const std::filesystem::path& assetDirectory);
    // One run's command line after the program: the project, then the --benchmark flags.
    std::vector<std::string> RunArguments(const std::filesystem::path& projectFile,
                                          const HubSettings&           settings,
                                          const std::filesystem::path& output,
                                          const std::filesystem::path& progress);

    std::filesystem::path BenchmarksDirectory(const std::filesystem::path& projectRoot);
    // "<yyyymmdd-hhmmss>-<scene>" in local time; the scene's file stem, with characters that
    // cannot be in a folder name replaced by '-'.
    std::string ResultFolderName(int64_t unixTime, std::string_view scene);

    // The ordinary median (the mean of the middle two for an even count); 0 when empty.
    double Median(std::vector<double> values);
    // (max - min) as a percentage of the median; nothing for fewer than one value or a zero median.
    std::optional<double> SpreadPercent(const std::vector<double>& values);
    bool                  IsUnstable(const std::vector<double>& runMeans);

    // summary.json for `runs` (reports in run order, their file names in `files`): the report
    // schema, where every frame and pass statistic is the median of the runs' values, sample
    // counts and compiles are summed, and a `runs` section lists each run's frame means, the
    // spread and the stability verdict. Throws when the runs differ in machine or settings.
    YAML::Node
    CombineRuns(const std::vector<YAML::Node>& runs, const std::vector<std::string>& files, int64_t createdUnix);
    Comparison ComparisonFromJson(const YAML::Node& node);

    struct HistoryEntry
    {
        std::filesystem::path Folder, Summary;
        int64_t               Created = 0;
        std::string           Scene, Gpu;
        uint32_t              Width = 0, Height = 0, Runs = 0;
        std::optional<double> GpuMedianMs;
        bool                  Stable = true;
    };
    // Result folders with a readable summary.json, newest first; anything else is skipped.
    std::vector<HistoryEntry> ListHistory(const std::filesystem::path& benchmarksDirectory);

    // Share of the whole session done: run `run` (1-based) of `runs` at `progress`.
    double CompletedFraction(uint32_t run, uint32_t runs, const Progress& progress, uint32_t warmup, uint32_t frames);
    std::optional<double> RemainingSeconds(double elapsedSeconds, double fraction);

    struct PassResult
    {
        std::string           Name;
        std::optional<double> CpuMs, GpuMs;
    };
    struct RunResult
    {
        std::string           File;
        std::optional<double> CpuMs, GpuMs;
    };
    struct ResultView
    {
        std::string                 Scene, Gpu, Bottleneck;
        std::optional<std::string>  TopPass;
        std::optional<double>       TopPassMs;
        uint32_t                    Width = 0, Height = 0;
        int64_t                     Created = 0;
        std::optional<Distribution> CpuFrame, GpuFrame;
        std::vector<PassResult>     Passes; // Report order (by name).
        uint64_t                    ShaderCompiles = 0, PipelineCompiles = 0;
        std::vector<RunResult>      Runs;
        bool                        Stable = true;
        std::optional<double>       CpuSpreadPercent, GpuSpreadPercent;
        std::optional<Comparison>   Compared;
        double                      ThresholdPercent = 0;
        std::string                 BaselineFile, ComparisonError;
    };
    ResultView MakeResultView(const YAML::Node& summary);
} // namespace Kans::Benchmark
