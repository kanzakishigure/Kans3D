#include "kspch.h"
#include "Kans3D/Renderer/Passes/BloomSettings.h"
namespace Kans
{
    BloomSettings BloomSettings::Validated() const
    {
        auto result = *this;
        auto finite = [](float value, float fallback, float maximum) {
            return std::isfinite(value) ? std::clamp(value, 0.0f, maximum) : fallback;
        };
        result.Threshold = finite(Threshold, 1, 65472);
        result.Knee      = finite(Knee, .5f, 1);
        result.Clamp     = finite(Clamp, 65472, 65472);
        result.Mode      = Mode == EnergyConserving ? EnergyConserving : Additive;
        result.Intensity = finite(Intensity, .8f, result.Mode == EnergyConserving ? 1 : 128);
        result.Scatter   = finite(Scatter, .7f, 1);
        for (unsigned i = 0; i < 3; ++i)
            result.Tint[i] = finite(Tint[i], 1, 128);
        result.DirtIntensity = finite(DirtIntensity, 0, 128);
        return result;
    }
    bool BloomSettings::operator==(const BloomSettings& b) const
    {
        return Enabled == b.Enabled && Threshold == b.Threshold && Knee == b.Knee && Clamp == b.Clamp &&
               Intensity == b.Intensity && Scatter == b.Scatter && Tint == b.Tint &&
               HighQualityFiltering == b.HighQualityFiltering && Mode == b.Mode && DirtTexture == b.DirtTexture &&
               DirtIntensity == b.DirtIntensity;
    }
} // namespace Kans
