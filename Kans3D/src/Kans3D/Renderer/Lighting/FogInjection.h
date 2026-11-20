#pragma once
#include "Kans3D/Renderer/Graph/RenderGraph.h"
#include "Fog.h"

namespace Kans
{
    // Candidate budget, including dead/unused reservations. Stable UUID/emitter/renderer order, then
    // ascending particle slot; no GPU count readback or race-dependent selection.
    constexpr uint32_t FogParticleInjectionLimit = 4096;
    struct FogInjectionSource
    {
        UUID      EntityID     = 0;
        uint32_t  EmitterIndex = 0, RendererIndex = 0;
        uint32_t  Capacity = 0, Count = 0, PositionOffset = 0, SizeOffset = 0, ColorOffset = 0;
        bool      LocalSpace = false;
        glm::mat4 Owner {1};
        float     Density = 1, RadiusScale = 1;
        glm::vec3 Albedo {1};
        RenderHardware::Handle<RenderHardware::Buffer> Attributes, Flags;
        // Grid attributes are SoA vec4 words, matching GpuEmitter::GridStorage. An explicitly named
        // scalar density is required; grids without one can expose it later through this interface.
        glm::uvec3 Resolution {0};
        glm::vec3  WorldBounds {1};
        uint32_t   DensityOffset = 0;
    };
    std::vector<FogInjectionSource> SelectFogInjectionSources(std::vector<FogInjectionSource> sources);
    struct FogInjectionState
    {
        RenderHardware::Handle<RenderHardware::BindGroupLayout> Layout, BoundsLayout, ScheduleLayout, SplatLayout,
            DiagnosticLayout;
        RenderHardware::Handle<RenderHardware::ComputePipeline> Bounds, Schedule, Splat, DiagnosticSplat, Grid, Resolve;
    };
    struct FogInjectionDiagnostics
    {
        GraphBuffer Sums;
        GraphBuffer Counters; // uint32_t candidate cells, cells reaching atomics, max candidate cells per lane.
    };
    std::vector<FogInjectionSource> CollectFogInjectionSources(RenderFrame&);
    // Returns the injected-only texture for beam marching, or -1 without allocating/recording work.
    uint32_t AddFogInjection(RenderGraph&,
                             RenderHardware::Device&,
                             const FogFrameResources&,
                             const std::vector<FogInjectionSource>&,
                             std::shared_ptr<FogInjectionState>&,
                             FogInjectionDiagnostics* diagnostics = nullptr);
} // namespace Kans
