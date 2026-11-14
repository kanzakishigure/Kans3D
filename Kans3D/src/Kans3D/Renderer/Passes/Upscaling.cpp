#include "kspch.h"
#include "Upscaling.h"

namespace Kans
{
    float UpscalingSettings::Ratio() const
    {
        switch (Mode)
        {
            case UpscalingMode::Native:
                return 1.f;
            case UpscalingMode::Quality:
                return 1.5f;
            case UpscalingMode::Balanced:
                return 1.7f;
            case UpscalingMode::Performance:
                return 2.f;
            case UpscalingMode::UltraPerformance:
                return 3.f;
            case UpscalingMode::Custom:
                return std::isfinite(CustomRatio) ? std::clamp(CustomRatio, 1.f, 3.f) : 1.f;
        }
        throw std::invalid_argument("Invalid upscaling mode");
    }
    glm::uvec2 UpscalingRenderSize(glm::uvec2 display, float ratio)
    {
        ratio = std::isfinite(ratio) ? std::clamp(ratio, 1.f, 3.f) : 1.f;
        return {std::max(1u, uint32_t(std::round(double(display.x) / ratio))),
                std::max(1u, uint32_t(std::round(double(display.y) / ratio)))};
    }
    float UpscalingMipBias(uint32_t renderWidth, uint32_t displayWidth)
    {
        return renderWidth < displayWidth ? std::log2(float(renderWidth) / displayWidth) - 1.f : 0.f;
    }
} // namespace Kans
