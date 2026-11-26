#pragma once

#include <atomic>
#include <cstdint>

namespace Kans
{
    struct AudioGainRamp
    {
        std::atomic<float> Target {1};
        std::atomic<float> Published {1};
        double             Current = 1, Step = 0;
        float              Destination = 1;
        uint32_t           Remaining   = 0;

        void Initialize(float gain)
        {
            Destination = gain;
            Current     = gain;
            Published.store(gain, std::memory_order_relaxed);
        }
        void Prepare(uint32_t frames) { Prepare(frames, Target.load(std::memory_order_relaxed)); }
        void Prepare(uint32_t frames, float target)
        {
            if (target != Destination)
            {
                Destination = target;
                Remaining   = frames;
                Step        = (target - Current) / frames;
            }
        }
        float Next()
        {
            if (Remaining > 0)
            {
                Current += Step;
                if (--Remaining == 0)
                    Current = Destination;
            }
            return static_cast<float>(Current);
        }
        void Publish() { Published.store(static_cast<float>(Current), std::memory_order_relaxed); }
    };
} // namespace Kans
