#pragma once
#include <memory>
#include "KansEditorCore/Profiling/BenchmarkReport.h"

namespace Kans
{
    class Project;
    namespace RenderHardware
    {
        class Device;
    }
} // namespace Kans

namespace Kans::Benchmark
{
    YAML::Node RunComplex(const Options& options, const std::shared_ptr<RenderHardware::Device>& device);
    YAML::Node RunProjectScene(Project&                                       project,
                               const std::filesystem::path&                   sceneFile,
                               const Options&                                 options,
                               const std::shared_ptr<RenderHardware::Device>& device);

    // Writes the report even when comparison flags a regression. IO/schema failures throw.
    int Finish(const Options& options, YAML::Node report);
} // namespace Kans::Benchmark
