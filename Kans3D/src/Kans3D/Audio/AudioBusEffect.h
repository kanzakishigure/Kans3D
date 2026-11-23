#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace Kans
{
    struct AudioLowPass
    {
        float    CutoffHz = 1000;
        uint32_t Order    = 2;
    };

    struct AudioHighPass
    {
        float    CutoffHz = 100;
        uint32_t Order    = 2;
    };

    // One peaking band per slot. Stack bands for a multi-band EQ.
    struct AudioPeakingEQ
    {
        float FrequencyHz = 1000, GainDb = 0, Q = 1;
    };

    struct AudioDelay
    {
        float TimeMs = 250, Feedback = .35f, Wet = .3f;
    };

    struct AudioReverb
    {
        float RoomSize = .5f, Damping = .5f, Width = 1, Wet = .3f, PreDelayMs = 0;
    };

    struct AudioCompressor
    {
        float ThresholdDb = -18, Ratio = 4, AttackMs = 10, ReleaseMs = 100, MakeupDb = 0, KneeDb = 6;
    };

    // Sample-peak ceiling. Look-ahead adds max(1, sampleRate / 1000) frames while enabled.
    struct AudioLimiter
    {
        float CeilingDb = -1, ReleaseMs = 50;
    };

    struct AudioDucking
    {
        std::string SourceBus;
        float       ThresholdDb = -24, DepthDb = 12, AttackMs = 10, ReleaseMs = 150;
    };

    struct AudioCustomDSP
    {
        std::string Name;
    };

    using AudioBusEffectParameters = std::variant<AudioLowPass,
                                                  AudioHighPass,
                                                  AudioPeakingEQ,
                                                  AudioDelay,
                                                  AudioReverb,
                                                  AudioCompressor,
                                                  AudioLimiter,
                                                  AudioDucking,
                                                  AudioCustomDSP>;

    struct AudioBusEffect
    {
        AudioBusEffectParameters Parameters;
        bool                     Enabled = true;
    };

    // The callback runs on the audio thread. It must not allocate, lock, block, throw or call AudioEngine.
    // The interleaved buffer is valid only during the call and may be read or modified in place.
    using AudioDSPCallback =
        void (*)(float* interleaved, uint32_t frames, uint32_t channels, uint32_t sampleRate, void* user);
} // namespace Kans
