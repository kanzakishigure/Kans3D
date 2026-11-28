#pragma once
#include <memory>
#include <vector>
#include "Kans3D/Core/Math/Bounds.h"
#include "Kans3D/Scene/Entity.h"
#include "KansEditorCore/Profiling/BenchmarkOptions.h"

namespace Kans
{
    class AssetManager;
}

namespace Kans::Benchmark
{
    struct ComplexSceneData
    {
        Ref<Scene> World;
        struct Motion
        {
            Entity    Target;
            glm::vec3 Position, Rotation;
            float     Phase;
            bool      Head;
        };
        std::vector<Motion> Animated;
        void                Advance(double time) const;
    };
    ComplexSceneData              BuildComplex(const ComplexOptions& options);
    std::unique_ptr<AssetManager> PrepareComplexAssets();
    glm::vec3                     OrbitPosition(const Bounds& bounds, double phase);
} // namespace Kans::Benchmark
