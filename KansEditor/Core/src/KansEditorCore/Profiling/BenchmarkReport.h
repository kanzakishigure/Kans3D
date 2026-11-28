#pragma once
#include <cstdint>
#include <filesystem>
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
    inline constexpr uint32_t SchemaVersion   = 1;
    inline constexpr double   AbsoluteFloorMs = 0.05;

    struct Distribution
    {
        size_t Samples;
        double Mean, Median, P95, P99, Maximum;
    };
    // Sorts the scratch samples in place. Median, P95 and P99 use nearest rank.
    std::optional<Distribution> Describe(std::vector<double>& samples);

    YAML::Node JsonString(std::string_view value);
    YAML::Node
    MakeReport(const FrameProfiling::FrameHistory& history, const YAML::Node& machine, const YAML::Node& settings);
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
