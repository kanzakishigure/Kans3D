#pragma once
#include "Kans3D/Asset/Runtime/ShaderData.h"

namespace Kans
{
    // Decorate Surface position outputs in-place; retain every original instruction.
    void MakeSurfacePositionInvariant(ShaderData& data);
    // Only the fragment stage is replaced; the supplied vertex product is copied verbatim.
    std::variant<ShaderData, AssetError> MakeStandardDepthNormalsShader(const ShaderData& surface);
    // Preserve an authored DepthOnly vertex stage, discard and
    // gl_FragDepth, replacing its optional color with the reconstruct-normal flag.
    std::variant<ShaderData, AssetError> MakeDepthOnlyPrepassShader(const ShaderProgramCode& code);
    // Main draws without prepass coverage invalidate the normal behind their visible pixels.
    std::variant<ShaderData, AssetError> MakePrepassInvalidationShader(const ShaderProgramCode& code,
                                                                       uint32_t                 location);
} // namespace Kans
