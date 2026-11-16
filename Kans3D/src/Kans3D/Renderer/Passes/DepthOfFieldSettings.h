#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
namespace Kans
{
    class Scene;
    enum class DepthOfFieldMode : uint32_t
    {
        Off,
        Physical,
        Manual
    };
    struct DepthOfFieldSettings
    {
        DepthOfFieldMode Mode          = DepthOfFieldMode::Off;
        float            FocusDistance = 5;
        uint64_t         FocusTarget   = 0;
        float            NearFocus = 3, FarFocus = 7, NearBlend = 2, FarBlend = 5;
        // Radius, not diameter, in percent of display height.
        float                MaxCoCPercent = 2, BokehStrength = .25f;
        uint32_t             Samples = 32;
        DepthOfFieldSettings Validated() const
        {
            auto r = *this;
            auto f = [](float x, float lo, float hi, float fallback) {
                return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
            };
            if (uint32_t(r.Mode) > 2)
                r.Mode = DepthOfFieldMode::Off;
            r.FocusDistance = f(r.FocusDistance, .001f, 1e6f, 5);
            r.NearFocus     = f(r.NearFocus, 0, 1e6f, 3);
            r.FarFocus      = f(r.FarFocus, r.NearFocus, 1e6f, std::max(7.f, r.NearFocus));
            r.NearBlend     = f(r.NearBlend, .001f, 1e6f, 2);
            r.FarBlend      = f(r.FarBlend, .001f, 1e6f, 5);
            r.MaxCoCPercent = f(r.MaxCoCPercent, 0, 10, 2);
            r.BokehStrength = f(r.BokehStrength, 0, 4, .25f);
            r.Samples       = r.Samples > 32 ? 64 : 32;
            return r;
        }
        bool Active() const { return Mode != DepthOfFieldMode::Off && MaxCoCPercent > 0; }
        // Thin lens; all scene distances in metres, lens/sensor dimensions in mm.
        // Signed radius: negative in front of focus, positive behind it.
        static float PhysicalCoCPixels(float depth,
                                       float focus,
                                       float focalMm,
                                       float aperture,
                                       float sensorHeightMm,
                                       float displayHeight);
        float        ResolveFocusDistance(Scene& scene, const glm::mat4& view) const;
        bool         operator==(const DepthOfFieldSettings& o) const
        {
            return Mode == o.Mode && FocusDistance == o.FocusDistance && FocusTarget == o.FocusTarget &&
                   NearFocus == o.NearFocus && FarFocus == o.FarFocus && NearBlend == o.NearBlend &&
                   FarBlend == o.FarBlend && MaxCoCPercent == o.MaxCoCPercent && BokehStrength == o.BokehStrength &&
                   Samples == o.Samples;
        }
    };
} // namespace Kans
