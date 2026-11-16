#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Graph/RenderGraph.h"
#include "ScreenSpaceReflections.h"
namespace Kans
{
    const std::string& SsrTraceShaderSource();
    struct TemporalState;
    struct SsrTraceInputs
    {
        GraphTexture                  HiZ, Normal, Color, Surface;
        glm::mat4                     Projection, View;
        glm::uvec2                    Viewport;
        ScreenSpaceReflectionSettings Settings;
    };
    // Radiance in RGB, confidence in A. SSR-2 resolve/denoise consumes this graph output before composite.
    GraphTexture BuildSsrTrace(RenderGraph&            graph,
                               RenderHardware::Device& device,
                               TemporalState&          state,
                               const SsrTraceInputs&   inputs);
} // namespace Kans
