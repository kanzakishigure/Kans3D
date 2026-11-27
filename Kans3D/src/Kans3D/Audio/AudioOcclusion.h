#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <glm/vec3.hpp>

namespace YAML
{
    class Node;
}

namespace Kans
{
    enum class AudioOcclusionMode : uint8_t
    {
        Off,
        Raycast,
        Volumetric
    };
    enum class AudioTransmissionMode : uint8_t
    {
        Off,
        FrequencyIndependent,
        FrequencyDependent
    };

    inline constexpr uint32_t MaxAudioOcclusionSamples        = 32;
    inline constexpr uint32_t DefaultMaxOcclusionRaysPerFrame = 32;
    inline constexpr uint32_t DefaultOcclusionStaleFrames     = 8;
    inline constexpr size_t   MaxAudioTransmissionHits        = 8;

    struct AudioOcclusionSettings
    {
        AudioOcclusionMode    Mode              = AudioOcclusionMode::Off;
        AudioTransmissionMode Transmission      = AudioTransmissionMode::Off;
        float                 OcclusionStrength = 1, TransmissionStrength = 1;
        uint32_t              OcclusionLayerMask = UINT32_MAX;
        float                 SourceRadius       = 1;
        uint32_t              Samples            = 16;

        bool Enabled() const
        {
            return OcclusionLayerMask && ((Mode != AudioOcclusionMode::Off && OcclusionStrength > 0) ||
                                          (Transmission != AudioTransmissionMode::Off && TransmissionStrength > 0));
        }
        bool operator==(const AudioOcclusionSettings& other) const
        {
            return std::tie(Mode,
                            Transmission,
                            OcclusionStrength,
                            TransmissionStrength,
                            OcclusionLayerMask,
                            SourceRadius,
                            Samples) == std::tie(other.Mode,
                                                 other.Transmission,
                                                 other.OcclusionStrength,
                                                 other.TransmissionStrength,
                                                 other.OcclusionLayerMask,
                                                 other.SourceRadius,
                                                 other.Samples);
        }
        bool operator!=(const AudioOcclusionSettings& other) const { return !(*this == other); }
    };

    struct AudioOcclusionOverrides
    {
        std::optional<AudioOcclusionMode>    Mode;
        std::optional<AudioTransmissionMode> Transmission;
        std::optional<float>                 OcclusionStrength, TransmissionStrength;
        std::optional<uint32_t>              OcclusionLayerMask;
        std::optional<float>                 SourceRadius;
        std::optional<uint32_t>              Samples;

        bool operator==(const AudioOcclusionOverrides& other) const
        {
            return std::tie(Mode,
                            Transmission,
                            OcclusionStrength,
                            TransmissionStrength,
                            OcclusionLayerMask,
                            SourceRadius,
                            Samples) == std::tie(other.Mode,
                                                 other.Transmission,
                                                 other.OcclusionStrength,
                                                 other.TransmissionStrength,
                                                 other.OcclusionLayerMask,
                                                 other.SourceRadius,
                                                 other.Samples);
        }
        bool                    operator!=(const AudioOcclusionOverrides& other) const { return !(*this == other); }
        AudioOcclusionOverrides Over(const AudioOcclusionOverrides& base) const
        {
            return {Mode ? Mode : base.Mode,
                    Transmission ? Transmission : base.Transmission,
                    OcclusionStrength ? OcclusionStrength : base.OcclusionStrength,
                    TransmissionStrength ? TransmissionStrength : base.TransmissionStrength,
                    OcclusionLayerMask ? OcclusionLayerMask : base.OcclusionLayerMask,
                    SourceRadius ? SourceRadius : base.SourceRadius,
                    Samples ? Samples : base.Samples};
        }

        AudioOcclusionSettings Resolve(const AudioOcclusionSettings& base = {}) const
        {
            return {Mode.value_or(base.Mode),
                    Transmission.value_or(base.Transmission),
                    OcclusionStrength.value_or(base.OcclusionStrength),
                    TransmissionStrength.value_or(base.TransmissionStrength),
                    OcclusionLayerMask.value_or(base.OcclusionLayerMask),
                    SourceRadius.value_or(base.SourceRadius),
                    Samples.value_or(base.Samples)};
        }
    };

    bool                    IsValidAudioOcclusion(const AudioOcclusionSettings&);
    YAML::Node              EncodeAudioOcclusion(const AudioOcclusionOverrides&);
    AudioOcclusionOverrides DecodeAudioOcclusion(const YAML::Node&);

    struct AudioOcclusionResult
    {
        float                Occlusion = 0;          // Strength-scaled blocked fraction. Zero is clear.
        std::array<float, 3> Transmission {1, 1, 1}; // Effective low, mid and high gains before occlusion.
        bool                 operator==(const AudioOcclusionResult& other) const
        {
            return Occlusion == other.Occlusion && Transmission == other.Transmission;
        }
    };

    struct AudioOcclusionRay
    {
        glm::vec3 Start {0}, End {0};
        uint64_t  Emitter   = 0;
        uint32_t  LayerMask = UINT32_MAX;
    };

    struct AudioOcclusionHit
    {
        glm::vec3            Point {0};
        std::array<float, 3> Transmission {1, 1, 1};
    };

    // Begin runs once before the first cast in a frame. Trace writes at most capacity
    // hits, nearest first, with normalized transmission coefficients. The provider
    // and optional debug hook run on the control thread without mutating the voice roster.
    struct AudioOcclusionQuery
    {
        void* Context                                                                         = nullptr;
        void (*Begin)(void*)                                                                  = nullptr;
        size_t (*Trace)(void*, const AudioOcclusionRay&, AudioOcclusionHit*, size_t capacity) = nullptr;
    };
    using AudioOcclusionDebugDraw = void (*)(void*, const AudioOcclusionRay&, const AudioOcclusionHit*, size_t count);

    struct AudioOcclusionStats
    {
        uint32_t Rays = 0, VoicesServiced = 0, DeferredVoices = 0;
        uint64_t BudgetOverruns = 0; // Frames with deferred work. Rays never exceed the budget.
    };
} // namespace Kans
