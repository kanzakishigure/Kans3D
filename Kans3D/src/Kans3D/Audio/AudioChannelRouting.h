#pragma once

#include <miniaudio.h>
#include <algorithm>
#include <cstdint>

namespace Kans
{
    inline constexpr float AudioMinus3Db = 0.7071067811865475244f;

    inline int AudioChannelIndex(const ma_channel* map, uint32_t channels, ma_channel channel)
    {
        const auto found = std::find(map, map + channels, channel);
        return found == map + channels ? -1 : static_cast<int>(found - map);
    }

    inline void RouteAudioCenter(float* frame, unsigned channels, int center, float mono)
    {
        if (center >= 0)
        {
            std::fill_n(frame, channels, 0.f);
            frame[center] = mono;
        }
        else if (channels > 1)
        {
            std::fill_n(frame, channels, 0.f);
            frame[0] = frame[1] = mono * AudioMinus3Db;
        }
    }
} // namespace Kans
