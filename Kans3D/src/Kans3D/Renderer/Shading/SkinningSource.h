#pragma once

namespace Kans
{
    // Shared by the SPIR-V skinning patch and both palette evaluations in object velocity.
    inline constexpr const char* SkinningMorphVectorGLSL = R"(
vec3 kans_MorphVector(uint offset) {
    return vec3(uintBitsToFloat(kans_Deltas[offset]),uintBitsToFloat(kans_Deltas[offset+1]),
                uintBitsToFloat(kans_Deltas[offset+2]));
}
)";
} // namespace Kans
