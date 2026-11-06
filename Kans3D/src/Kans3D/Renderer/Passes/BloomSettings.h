#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Asset/Asset.h"

namespace Kans
{
    struct BloomSettings
    {
        bool      Enabled   = false;
        float     Threshold = 1, Knee = 0.5f, Clamp = 65472, Intensity = 0.8f, Scatter = 0.7f;
        glm::vec3 Tint {1};
        bool      HighQualityFiltering = false;
        enum Composite
        {
            Additive,
            EnergyConserving
        };
        Composite Mode = Additive;
        AssetID   DirtTexture;
        float     DirtIntensity = 0;

        // Non-finite runtime inputs fall back to defaults; persistence rejects them.
        BloomSettings Validated() const;
        bool          operator==(const BloomSettings& other) const;
    };
} // namespace Kans
