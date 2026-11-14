#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <glm/glm.hpp>

namespace Kans
{
    enum class UpscalingMode : uint8_t
    {
        Native,
        Quality,
        Balanced,
        Performance,
        UltraPerformance,
        Custom
    };
    struct UpscalingSettings
    {
        UpscalingMode Mode        = UpscalingMode::Native;
        float         CustomRatio = 1.f;
        float         Ratio() const;
        bool          operator==(const UpscalingSettings& other) const
        {
            return Mode == other.Mode && CustomRatio == other.CustomRatio;
        }
    };
    glm::uvec2 UpscalingRenderSize(glm::uvec2 display, float ratio);
    float      UpscalingMipBias(uint32_t renderWidth, uint32_t displayWidth);
} // namespace Kans
