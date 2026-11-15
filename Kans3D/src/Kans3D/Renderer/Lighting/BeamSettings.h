#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Kans
{
    struct BeamSettings
    {
        bool         Enabled   = false;
        float        Intensity = 1;
        float        Length    = 0;              // Zero follows the spot Range, including animated range changes.
        float        StartFade = 0, EndFade = 0; // Fade distances in metres from either end.
        bool         UseShadow = true, UseCookie = true;
        uint32_t     Samples = 32;
        float        EffectiveLength(float range) const { return Length > 0 ? Length : range; }
        BeamSettings Validated() const
        {
            auto s      = *this;
            auto finite = [](float v) { return std::isfinite(v) ? std::max(0.f, v) : 0.f; };
            s.Intensity = finite(s.Intensity);
            s.Length    = finite(s.Length);
            s.StartFade = finite(s.StartFade);
            s.EndFade   = finite(s.EndFade);
            s.Samples   = std::clamp(s.Samples, 16u, 64u);
            return s;
        }
        bool operator==(const BeamSettings& b) const
        {
            return Enabled == b.Enabled && Intensity == b.Intensity && Length == b.Length && StartFade == b.StartFade &&
                   EndFade == b.EndFade && UseShadow == b.UseShadow && UseCookie == b.UseCookie && Samples == b.Samples;
        }
    };
} // namespace Kans
