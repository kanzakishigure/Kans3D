#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/vec2.hpp>
namespace Kans
{
    // Scene lighting, rather than display post-processing. No AO for missing prepass normals.
    struct GtaoSettings
    {
        bool         Enabled = false, FullResolution = false;
        uint32_t     Slices = 3, Steps = 6;
        float        Radius = 1, Intensity = 1, Power = 1, Falloff = .5f, Thickness = .1f;
        GtaoSettings Validated() const
        {
            auto s      = *this;
            s.Slices    = std::clamp(s.Slices, 1u, 16u);
            s.Steps     = std::clamp(s.Steps, 1u, 32u);
            auto finite = [](float x, float fallback, float lo, float hi) {
                return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
            };
            s.Radius    = finite(s.Radius, 1, .01f, 100);
            s.Intensity = finite(s.Intensity, 1, 0, 4);
            s.Power     = finite(s.Power, 1, .01f, 8);
            s.Falloff   = finite(s.Falloff, .5f, .01f, 1);
            s.Thickness = finite(s.Thickness, .1f, 0, 1);
            return s;
        }
    };
    inline bool operator==(const GtaoSettings& a, const GtaoSettings& b)
    {
        return a.Enabled == b.Enabled && a.FullResolution == b.FullResolution && a.Slices == b.Slices &&
               a.Steps == b.Steps && a.Radius == b.Radius && a.Intensity == b.Intensity && a.Power == b.Power &&
               a.Falloff == b.Falloff && a.Thickness == b.Thickness;
    }
    inline bool operator!=(const GtaoSettings& a, const GtaoSettings& b) { return !(a == b); }
    // Integer phase avoids precision loss at long runtimes. Shared CPU/GPU pattern constants.
    inline glm::vec2 GtaoSample(uint64_t frame, uint32_t slice, uint32_t step, const GtaoSettings& input)
    {
        auto  s        = input.Validated();
        float rotation = float(frame % 64) * .61803398875f;
        rotation -= std::floor(rotation);
        return {3.14159265359f * (float(slice) + rotation) / float(s.Slices), (float(step) + .5f) / float(s.Steps)};
    }
} // namespace Kans
