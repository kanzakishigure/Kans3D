#pragma once
#include <glm/vec2.hpp>
#include "Kans3D/Renderer/Graph/RenderGraph.h"
namespace Kans
{
    // Vulkan mip extents round down. The last reduction footprint includes the odd edge (3 texels).
    std::vector<glm::uvec2> HiZMipSizes(glm::uvec2 size);
    struct HiZState
    {
        RenderHardware::Handle<RenderHardware::Texture>         Pyramid;
        RenderHardware::Handle<RenderHardware::BindGroupLayout> CopyLayout, ReduceLayout;
        RenderHardware::Handle<RenderHardware::ComputePipeline> Copy, Reduce;
        RenderHardware::Handle<RenderHardware::Sampler>         Point;
    };
    // Standard-Z min depth; reusable by any graph consumer. All mip views use GENERAL while reducing.
    GraphTexture BuildHiZ(RenderGraph&                                           graph,
                          RenderHardware::Device&                                device,
                          const RenderHardware::Handle<RenderHardware::Texture>& depth,
                          HiZState&                                              state);
} // namespace Kans
