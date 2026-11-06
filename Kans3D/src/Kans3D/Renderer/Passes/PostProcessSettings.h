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
    struct PostProcessSettings
    {
        float               Exposure = 0.8f;
        BloomSettings       Bloom;
        std::vector<PostProcessMaterial> Materials;
        ToneMappingMode     ToneMapping = ToneMappingMode::ACES;
        float FilterSize = 0; // Radial Blackman-Harris radius in output pixels; zero preserves legacy output.
        PostProcessSettings Validated() const
        {
            auto result     = *this;
            result.Exposure = std::isfinite(Exposure) ? std::clamp(Exposure, 0.001f, 128.f) : .8f;
            result.Bloom    = Bloom.Validated();
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
            return Exposure == other.Exposure && Bloom == other.Bloom && ToneMapping == other.ToneMapping &&
                   FilterSize == other.FilterSize && Materials == other.Materials;
        }
        bool operator!=(const PostProcessSettings& other) const { return !(*this == other); }
    };
} // namespace Kans
