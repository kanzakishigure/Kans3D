#pragma once
#include <map>
#include <tuple>
#include "RenderView.h"

namespace Kans
{
    // FSR phase count: ceil(8 * (displayWidth/renderWidth)^2). Invalid sizes throw.
    uint32_t  JitterPhaseCount(uint32_t displayWidth, uint32_t renderWidth);
    glm::vec2 JitterOffset(uint64_t frameIndex, uint32_t phaseCount);
    glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 clipOffset);

    // Debug velocity: hue = atan2(y,x)/(2*pi), value = clamp(length(velocity)*32,0,1).
    // Normal debug: world normal * .5 + .5; upper-right quarter shows roughness in grayscale.
    // Uncovered custom normals (validity != 1) are black. Custom masked/deformed vertices
    // have no reusable material surface function: velocity does not reproduce their clipping/deformation.
    // Per-target history and cached temporal GPU state. Never shared between viewports.
    struct TemporalState
    {
        struct Object
        {
            glm::mat4                                      Transform {1};
            bool                                           Deformed = false;
            uint64_t                                       LastSeen = 0, Identity = 0;
            std::weak_ptr<RenderHardware::Buffer>          MeshVertices;
            std::vector<glm::mat4>                         Palette;
            RenderHardware::Handle<RenderHardware::Buffer> PreviousPalette, CurrentPalette;
        };
        using Key = std::tuple<uint64_t, uint64_t, MeshKey, uint32_t>;
        std::map<Key, Object>                                   Objects;
        const void*                                             World = nullptr;
        glm::mat4                                               Projection {1}, Current {1}, Previous {1};
        glm::vec4                                               Jitter {0};
        uint64_t                                                Frame = 0;
        bool                                                    Valid = false, Reset = true;
        RenderHardware::Handle<RenderHardware::BindGroupLayout> FullscreenLayout, ObjectLayout, SkinLayout,
            EmptyMaterialLayout;
        RenderHardware::Handle<RenderHardware::Sampler> PointSampler;
    };
} // namespace Kans
