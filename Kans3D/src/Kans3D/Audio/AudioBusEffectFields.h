#pragma once

#include <array>
#include <string_view>
#include <type_traits>
#include "AudioBusEffect.h"

namespace Kans
{
    enum class AudioEffectType
    {
        LowPass,
        HighPass,
        PeakingEQ,
        Delay,
        Reverb,
        Compressor,
        Limiter,
        Ducking,
        Custom
    };

    inline AudioEffectType GetEffectType(const AudioBusEffectParameters& params)
    {
        // The type order follows AudioBusEffectParameters and the on-disk names below.
        return static_cast<AudioEffectType>(params.index());
    }

    inline constexpr std::array<std::string_view, std::variant_size_v<AudioBusEffectParameters>> AudioEffectTypes {
        "LowPass",
        "HighPass",
        "PeakingEQ",
        "Delay",
        "Reverb",
        "Compressor",
        "Limiter",
        "Ducking",
        "Custom"};

    // Keep parameter names, units and ranges shared by the asset and the runtime editor API.
    template<class T, class Visit>
    void VisitAudioEffectFields(T& params, Visit&& visit)
    {
        using P = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<P, AudioLowPass> || std::is_same_v<P, AudioHighPass>)
        {
            visit("CutoffHz", params.CutoffHz, 10.f, 24000.f);
            visit("Order", params.Order, 1u, 8u);
        }
        else if constexpr (std::is_same_v<P, AudioPeakingEQ>)
        {
            visit("FrequencyHz", params.FrequencyHz, 10.f, 24000.f);
            visit("GainDb", params.GainDb, -24.f, 24.f);
            visit("Q", params.Q, .1f, 20.f);
        }
        else if constexpr (std::is_same_v<P, AudioDelay>)
        {
            visit("TimeMs", params.TimeMs, 1.f, 2000.f);
            visit("Feedback", params.Feedback, 0.f, .99f);
            visit("Wet", params.Wet, 0.f, 1.f);
        }
        else if constexpr (std::is_same_v<P, AudioReverb>)
        {
            visit("RoomSize", params.RoomSize, 0.f, 1.f);
            visit("Damping", params.Damping, 0.f, 1.f);
            visit("Width", params.Width, 0.f, 1.f);
            visit("Wet", params.Wet, 0.f, 1.f);
            visit("PreDelayMs", params.PreDelayMs, 0.f, 250.f);
        }
        else if constexpr (std::is_same_v<P, AudioCompressor>)
        {
            visit("ThresholdDb", params.ThresholdDb, -80.f, 0.f);
            visit("Ratio", params.Ratio, 1.f, 100.f);
            visit("AttackMs", params.AttackMs, .1f, 500.f);
            visit("ReleaseMs", params.ReleaseMs, 1.f, 5000.f);
            visit("MakeupDb", params.MakeupDb, -24.f, 24.f);
            visit("KneeDb", params.KneeDb, 0.f, 24.f);
        }
        else if constexpr (std::is_same_v<P, AudioLimiter>)
        {
            visit("CeilingDb", params.CeilingDb, -60.f, 0.f);
            visit("ReleaseMs", params.ReleaseMs, 1.f, 5000.f);
        }
        else if constexpr (std::is_same_v<P, AudioDucking>)
        {
            visit("SourceBus", params.SourceBus);
            visit("ThresholdDb", params.ThresholdDb, -80.f, 0.f);
            visit("DepthDb", params.DepthDb, 0.f, 80.f);
            visit("AttackMs", params.AttackMs, .1f, 500.f);
            visit("ReleaseMs", params.ReleaseMs, 1.f, 5000.f);
        }
        else if constexpr (std::is_same_v<P, AudioCustomDSP>)
            visit("Name", params.Name);
    }
} // namespace Kans
