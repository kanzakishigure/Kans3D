#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace Kans::Benchmark
{
    struct ComplexOptions
    {
        uint32_t Instances      = 4096;
        uint32_t Lights         = 128;
        uint32_t ShadowedLights = 6;
        uint32_t Seed           = 42;
    };

    struct Options
    {
        std::string           Scene;
        uint32_t              Frames = 360, Warmup = 120;
        uint32_t              Width = 2560, Height = 1440;
        std::filesystem::path Output           = "benchmark.json", Baseline;
        std::filesystem::path Progress; // --benchmark-progress: see BenchmarkProgress.h.
        double                ThresholdPercent = 5;
        ComplexOptions        Complex;
    };

    inline constexpr const char* ComplexScene     = "builtin:complex";
    inline constexpr double      FixedStepSeconds = 1.0 / 60.0;
} // namespace Kans::Benchmark
