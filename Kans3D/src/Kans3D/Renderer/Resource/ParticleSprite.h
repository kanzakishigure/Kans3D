#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/RHI/RHI.h"
namespace Kans
{
    struct ParticleSpriteUniforms
    {
        glm::mat4  Owner {1}, View {1}, Projection {1}, InverseView {1}, InverseProjection {1};
        glm::uvec4 Attributes0 {}, Attributes1 {};
        glm::vec4  PivotGrid {.5f, .5f, 1, 1};
        glm::vec4  Facing {0, 0, 1, 0}, Alignment {0, 1, 0, 0};
        glm::vec4  MeshScale {1}, MeshPivot {0}, LockedAxis {0, 0, 1, 0};
        glm::vec4  Options {0}; // facing mode, depth fade, frame blending, local space
    };
    std::variant<ShaderData, AssetError> MakeParticleSpriteShader(const ShaderData&, bool mesh = false);
    RenderHardware::Handle<RenderHardware::BindGroupLayout> ParticleSpriteLayout(RenderHardware::Device&);
} // namespace Kans
