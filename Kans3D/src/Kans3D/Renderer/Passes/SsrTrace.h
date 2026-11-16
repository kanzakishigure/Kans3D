#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Graph/RenderGraph.h"
#include "ScreenSpaceReflections.h"
namespace Kans
{
    const std::string& SsrTraceShaderSource();
    const std::string& SsrResolveShaderSource();
    struct TemporalState;
    struct SsrTraceInputs
    {
        GraphTexture                  HiZ, Normal, Color, Surface;
        glm::mat4                     Projection, View;
        glm::uvec2                    Viewport;
        ScreenSpaceReflectionSettings Settings;
        GraphTexture Velocity = uint32_t(-1), PreviousDepth = uint32_t(-1), PreviousNormal = uint32_t(-1);
        glm::mat4    PreviousClipFromView {1}, PreviousInverseProjection {1};
        glm::vec2    JitterDelta {0};
        bool         HistoryValid = true;
    };
    // Radiance in RGB, confidence in A; ray endpoint and sampling PDF are retained for neighbour reuse.
    GraphTexture BuildSsrTrace(RenderGraph&            graph,
                               RenderHardware::Device& device,
                               TemporalState&          state,
                               const SsrTraceInputs&   inputs);
} // namespace Kans
