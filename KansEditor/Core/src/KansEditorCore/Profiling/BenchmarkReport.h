#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "KansEditorCore/Profiling/BenchmarkOptions.h"

namespace Kans::FrameProfiling
{
    class FrameHistory;
}

namespace Kans::Benchmark
{
    inline constexpr uint32_t SchemaVersion   = 2;
    inline constexpr double   AbsoluteFloorMs = 0.05;

    struct Distribution
    {
        size_t Samples;
        double Mean, Median, P95, P99, Maximum;
    };
    // Sorts the scratch samples in place. Median, P95 and P99 use nearest rank.
    std::optional<Distribution> Describe(std::vector<double>& samples);

    struct CpuMetric
    {
        const char* Key;
        const char* Label;
    };
    inline constexpr CpuMetric CpuMetrics[] {
        {"scene_update_ms", "Scene update"},
        {"world_sync_extract_ms", "World sync / extract"},
        {"graph_build_ms", "Graph build"},
        {"graph_compile_ms", "Graph compile"},
        {"pass_record_ms", "Pass recording"},
        {"execute_submit_ms", "Execute / submit"},
        {"fence_wait_ms", "Fence wait"},
        {"other_ms", "Other CPU work"},
    };

    struct FrameTiming
    {
        double SceneUpdateMs = 0, FenceWaitMs = 0;
    };
    struct CompileNames
    {
        std::vector<std::string> Shaders, Pipelines;
        size_t                   Omitted = 0;
    };

    YAML::Node JsonString(std::string_view value);
    YAML::Node MakeReport(const FrameProfiling::FrameHistory&     history,
                          const YAML::Node&                       machine,
                          const YAML::Node&                       settings,
                          const std::vector<FrameTiming>&         frameTimings = {},
                          const std::map<uint64_t, CompileNames>& compileNames = {});
    YAML::Node ReadReport(const std::filesystem::path& file);
    void       WriteReport(const std::filesystem::path& file, const YAML::Node& report);

    struct ComparisonRow
    {
        std::string           Name, Metric, Status;
        std::optional<double> BaselineMs, CurrentMs, DeltaMs, DeltaPercent;
    };

    struct Comparison
    {
        std::vector<std::string>   Mismatches;
        std::vector<ComparisonRow> Rows;
        bool                       Regressed = false;
        int                        ExitCode() const { return Regressed ? 2 : 0; }
    };

    Comparison Compare(const YAML::Node& baseline, const YAML::Node& current, double thresholdPercent);
    YAML::Node ComparisonJson(const Comparison& comparison, double thresholdPercent);
} // namespace Kans::Benchmark
