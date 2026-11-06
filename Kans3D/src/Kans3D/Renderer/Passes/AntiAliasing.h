#pragma once
#include <cstdint>
#include <stdexcept>
namespace Kans
{
    enum class AntiAliasing : uint8_t
    {
        None,
        FXAA,
        MSAA2,
        MSAA4,
        MSAA8
    };
    inline uint32_t AntiAliasingSamples(AntiAliasing mode)
    {
        switch (mode)
        {
            case AntiAliasing::None:
            case AntiAliasing::FXAA:
                return 1;
            case AntiAliasing::MSAA2:
                return 2;
            case AntiAliasing::MSAA4:
                return 4;
            case AntiAliasing::MSAA8:
                return 8;
        }
        throw std::invalid_argument("Invalid anti-aliasing mode");
    }
    inline AntiAliasing EffectiveAntiAliasing(AntiAliasing requested, uint32_t supported)
    {
        const auto samples = AntiAliasingSamples(requested);
        if (samples == 1)
            return requested;
        if (supported & samples)
            return requested;
        const auto mode = [](uint32_t n) {
            return n == 8 ? AntiAliasing::MSAA8 : n == 4 ? AntiAliasing::MSAA4 : AntiAliasing::MSAA2;
        };
        // The nearest supported count above the request (closest quality and cost),
        // otherwise the highest one below it.
        for (uint32_t n = samples * 2; n <= 8; n *= 2)
            if (supported & n)
                return mode(n);
        for (uint32_t n = samples / 2; n > 1; n /= 2)
            if (supported & n)
                return mode(n);
        return AntiAliasing::None;
    }
} // namespace Kans
