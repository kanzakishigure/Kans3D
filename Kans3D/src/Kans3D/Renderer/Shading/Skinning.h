#pragma once
#include "Kans3D/Animation/AnimationData.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Core/Math/Bounds.h"
#include "Kans3D/RHI/RHI.h"
namespace Kans
{
    struct SkinningPose
    {
        std::vector<glm::mat4> Palette;
        uint64_t               Version = 0;
        std::vector<ShapeKey>  ShapeKeys;
        std::vector<float>     MorphWeights;
    };
    // Material vertex inputs at locations 0/1/4/7 are transformed before the original main.
    // The static shader's bytecode is untouched. Set 2/binding 0 is a readonly mat4 palette SSBO;
    // Binding 1 stores sparse integer indices and packed float position/optional normal/tangent streams. A tagged
    // final palette matrix describes the active (weight, target) records; morphs run before skinning.
    // skin stream locations 5/6 are uvec4 joints/vec4 weights. There is no fixed palette length.
    std::variant<ShaderData, AssetError> MakeSkinnedShader(const ShaderData&, bool smoothNormalFallback = false);
    RenderHardware::Handle<RenderHardware::BindGroupLayout> SkinningLayout(RenderHardware::Device&);
    void   BindSkinning(RenderHardware::Device&,
                        RenderHardware::CommandList&,
                        const RenderHardware::Handle<RenderHardware::BindGroupLayout>&,
                        const RenderHardware::Handle<RenderHardware::Buffer>&,
                        const RenderHardware::Handle<RenderHardware::Buffer>& morphDeltas = {});
    Bounds DeformedBounds(const MeshData&, const SkinningPose* pose = nullptr);
    Bounds SkinnedBounds(const std::vector<Bounds>& jointBounds, const SkinningPose&);
} // namespace Kans
