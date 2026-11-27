#pragma once

#include <array>
#include <cstdint>

namespace Kans
{
    enum class AcousticMaterialPreset : uint8_t
    {
        Generic,
        Brick,
        Concrete,
        Ceramic,
        Gravel,
        Carpet,
        Glass,
        Plaster,
        Wood,
        Metal,
        Rock
    };

    struct AcousticMaterial
    {
        std::array<float, 3> Absorption {.10f, .20f, .30f};
        std::array<float, 3> Transmission {.100f, .050f, .030f};
        float                Scattering = .05f;
        bool                 operator==(const AcousticMaterial& other) const
        {
            return Absorption == other.Absorption && Transmission == other.Transmission &&
                   Scattering == other.Scattering;
        }
    };

    const char*      AcousticMaterialPresetName(AcousticMaterialPreset preset);
    AcousticMaterial GetAcousticMaterialPreset(AcousticMaterialPreset preset);
} // namespace Kans
