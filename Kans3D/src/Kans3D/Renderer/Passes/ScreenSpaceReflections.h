#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Kans
{
    struct ScreenSpaceReflectionSettings
    {
        bool                          Enabled = false, HalfResolution = true;
        uint32_t                      MaxSteps     = 128;
        float                         MaxRoughness = .6f, Thickness = .15f, Intensity = 1;
        ScreenSpaceReflectionSettings Validated() const
        {
            auto s      = *this;
            s.MaxSteps  = std::clamp(s.MaxSteps, 8u, 512u);
            auto finite = [](float x, float fallback, float lo, float hi) {
                return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
            };
            s.MaxRoughness = finite(s.MaxRoughness, .6f, 0, 1);
            s.Thickness    = finite(s.Thickness, .15f, .001f, 10);
            s.Intensity    = finite(s.Intensity, 1, 0, 4);
            return s;
        }
        bool operator==(const ScreenSpaceReflectionSettings& b) const
        {
            return Enabled == b.Enabled && HalfResolution == b.HalfResolution && MaxSteps == b.MaxSteps &&
                   MaxRoughness == b.MaxRoughness && Thickness == b.Thickness && Intensity == b.Intensity;
        }
        bool operator!=(const ScreenSpaceReflectionSettings& b) const { return !(*this == b); }
    };
} // namespace Kans
