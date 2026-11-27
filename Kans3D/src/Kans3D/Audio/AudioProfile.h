#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include "Kans3D/Asset/Asset.h"
#include "AudioOcclusion.h"
#include "AudioSpatializer.h"

namespace Kans
{
    using AudioSoundID = uint64_t;

    struct AudioVoiceHandle
    {
        uint32_t Index = 0, Generation = 0;
        bool     IsValid() const { return Generation != 0; }
    };

    enum class AudioVoiceOwnerKind
    {
        Direct,
        Entity,
        Event,
        OneShot,
        Music,
        Timeline,
        Generator
    };

    struct AudioVoiceOwner
    {
        AudioVoiceOwnerKind Kind   = AudioVoiceOwnerKind::Direct;
        uint64_t            Entity = 0;
        AssetID             Asset; // Event or Timeline asset, when applicable.
    };

    enum class AudioVoiceState
    {
        Stopped,
        Scheduled,
        Playing,
        Paused,
        HeldByGamePause,
        Virtual,
        Stopping
    };
    enum class AudioVoiceReason
    {
        None,
        Priority,
        ConcurrencyLimit,
        InstanceLimit,
        Cooldown,
        Inaudible,
        PausedBus,
        MissingClip,
        InvalidSource
    };
    enum class AudioVoiceStorage
    {
        Decoded,
        Streaming,
        Generator
    };

    struct AudioStreamProfile
    {
        uint64_t BufferedFrames = 0, CapacityFrames = 0;
        uint64_t DecodeNanoseconds = 0, DecodePasses = 0;
        uint64_t Underruns = 0, UnderrunFrames = 0;
    };

    struct AudioVoiceInfo
    {
        AudioVoiceHandle   Voice;
        AudioVoiceOwner    Owner;
        AudioSoundID       Sound = 0;
        AssetID            Clip;
        std::string_view   Bus, ConcurrencyGroup;
        AudioVoiceState    State      = AudioVoiceState::Stopped;
        AudioVoiceReason   Reason     = AudioVoiceReason::None;
        uint8_t            Priority   = 0;
        float              Audibility = 0, FinalGainDb = -std::numeric_limits<float>::infinity();
        float              Distance        = 0;
        int32_t            Listener        = -1;
        double             PositionSeconds = 0, LengthSeconds = 0;
        AudioVoiceStorage  Storage = AudioVoiceStorage::Decoded;
        AudioStreamProfile Stream;
        AudioSpatializer   Spatializer  = AudioSpatializer::Panning;
        float              SpatialBlend = 1;
        AudioHRTFFallback  HRTFFallback = AudioHRTFFallback::None;
        AudioOcclusionResult    Acoustics;
        std::optional<uint64_t> OcclusionAgeFrames;
        bool                    OcclusionStale = false;
    };

    enum class AudioProfileEventType
    {
        Started,
        Stopped,
        Stolen,
        Rejected,
        Virtualised,
        Revived,
        Underrun,
        DeviceChanged
    };
    struct AudioProfileEvent
    {
        uint64_t              Frame   = 0;
        double                Seconds = 0;
        AudioProfileEventType Type    = AudioProfileEventType::Started;
        AudioVoiceReason      Reason  = AudioVoiceReason::None;
        AudioVoiceHandle      Voice;
        AudioVoiceOwner       Owner;
        AssetID               Clip;
        AudioSoundID          Sound = 0;
        uint64_t Count = 0, Frames = 0; // Underrun deltas, or the output revision in Count for DeviceChanged.
    };

    struct AudioProfileEventCursor
    {
        uint64_t Next = 0, Dropped = 0;
    };

    struct AudioProfileWindow
    {
        uint64_t Samples = 0;
        double   Minimum = 0, Average = 0, Peak = 0;
    };

    struct AudioProfileCallback
    {
        uint64_t Frame = 0, Frames = 0;
        uint64_t WallNanoseconds   = 0;
        double   BudgetNanoseconds = 0;
        uint64_t SteamAudioNanoseconds = 0;
        double   LoadPercent() const { return BudgetNanoseconds > 0 ? 100 * WallNanoseconds / BudgetNanoseconds : 0; }
    };

    struct AudioProfileNode
    {
        size_t                BusIndex = 0;
        std::optional<size_t> EffectSlot;
        uint64_t              CurrentNanoseconds = 0;
        AudioProfileWindow    LastSecond, LastTenSeconds; // Nanoseconds.
    };

    struct AudioProfileStats
    {
        AudioProfileCallback Current;
        AudioProfileWindow   LastSecond, LastTenSeconds; // Load percent.
        uint64_t             Callbacks = 0, Overruns = 0, DroppedTimings = 0, OverwrittenEvents = 0;
        uint64_t             Underruns = 0, UnderrunFrames = 0;
        uint64_t             DecodedBytes = 0, StreamingBufferBytes = 0, GeneratorBufferBytes = 0;
        uint32_t             RealVoices = 0, VirtualVoices = 0;
        size_t               UnprofiledNodes = 0;
        AudioOcclusionStats  Occlusion;
    };
} // namespace Kans
