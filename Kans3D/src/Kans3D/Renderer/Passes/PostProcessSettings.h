#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Renderer/Passes/BloomSettings.h"
namespace Kans
{
    enum class ToneMappingMode : uint32_t
    {
        ACES,
        Standard
    };
    struct PostProcessMaterial
    {
        AssetID Material;
        bool    Enabled = true;
        bool    operator==(const PostProcessMaterial& other) const
        {
            return Material == other.Material && Enabled == other.Enabled;
        }
    };
    struct TemporalReconstructionSettings
    {
        float Sharpness = .3f;
        // SDK GenerateReactive sample defaults. Flags: tonemap | threshold | components max.
        float ReactiveScale = 1.f, ReactiveThreshold = .2f, ReactiveBinaryValue = .9f;
        bool  operator==(const TemporalReconstructionSettings& other) const
        {
            return Sharpness == other.Sharpness && ReactiveScale == other.ReactiveScale &&
                   ReactiveThreshold == other.ReactiveThreshold && ReactiveBinaryValue == other.ReactiveBinaryValue;
        }
        TemporalReconstructionSettings Validated() const
        {
            const auto finite = [](float value, float fallback, float maximum) {
                return std::isfinite(value) ? std::clamp(value, 0.f, maximum) : fallback;
            };
            return {finite(Sharpness, .3f, 1),
                    finite(ReactiveScale, 1, 16),
                    finite(ReactiveThreshold, .2f, 1),
                    finite(ReactiveBinaryValue, .9f, 1)};
        }
    };
    struct PostProcessSettings
    {
        float               Exposure = 0.8f;
        BloomSettings       Bloom;
        TemporalReconstructionSettings   Temporal;
        std::vector<PostProcessMaterial> Materials;
        ToneMappingMode     ToneMapping = ToneMappingMode::ACES;
        float FilterSize = 0; // Radial Blackman-Harris radius in output pixels; zero preserves legacy output.
        PostProcessSettings Validated() const
        {
            auto result     = *this;
            result.Exposure = std::isfinite(Exposure) ? std::clamp(Exposure, 0.001f, 128.f) : .8f;
            result.Bloom    = Bloom.Validated();
            result.Temporal = Temporal.Validated();
            result.Materials.erase(std::remove_if(result.Materials.begin(),
                                                  result.Materials.end(),
                                                  [](const auto& entry) { return !entry.Material.IsValid(); }),
                                   result.Materials.end());
            result.FilterSize = std::isfinite(FilterSize) ? std::clamp(FilterSize, 0.f, 4.f) : 0.f;
            if (uint32_t(ToneMapping) > uint32_t(ToneMappingMode::Standard))
                result.ToneMapping = ToneMappingMode::ACES;
            return result;
        }
        bool operator==(const PostProcessSettings& other) const
        {
            return Exposure == other.Exposure && Temporal == other.Temporal && Bloom == other.Bloom &&
                   ToneMapping == other.ToneMapping && FilterSize == other.FilterSize && Materials == other.Materials;
        }
        bool operator!=(const PostProcessSettings& other) const { return !(*this == other); }
    };
} // namespace Kans
