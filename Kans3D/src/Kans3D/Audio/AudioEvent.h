#pragma once

#include "AudioEngine.h"

namespace Kans
{
    enum class AudioEventSelection
    {
        Random,
        Shuffle,
        Sequential
    };

    struct AudioEventEntry
    {
        AssetID Clip;
        float   Weight = 1, VolumeDb = 0, PitchSemitones = 0;
        bool    operator==(const AudioEventEntry& other) const
        {
            return Clip == other.Clip && Weight == other.Weight && VolumeDb == other.VolumeDb &&
                   PitchSemitones == other.PitchSemitones;
        }
    };

    struct AudioEventRange
    {
        float Min = 0, Max = 0;
    };

    struct AudioEventData
    {
        std::vector<AudioEventEntry> Entries;
        AudioEventSelection          Selection          = AudioEventSelection::Random;
        uint32_t                     AvoidRepeatingLast = 0;
        float                        VolumeDb = 0, PitchSemitones = 0;
        AudioEventRange              VolumeRangeDb, PitchRangeSemitones, StartOffsetSeconds;
        double                       CooldownSeconds = 0;
        AudioConcurrencyGroup        InstanceLimit {std::numeric_limits<uint32_t>::max()};
        std::string                  ConcurrencyGroup;
        std::string                  Bus      = MasterAudioBus;
        uint8_t                      Priority = DefaultAudioVoicePriority;
        bool                         Loop = false, Spatial = false;
        std::optional<AudioSpatializer> Spatializer;
        float                           SpatialBlend   = 1;
        AudioOcclusionOverrides         Occlusion;
        bool                         ReverbZoneSend = true;
        float                        LfeSendDb      = AudioSendOffDb;
        AssetID                      AttenuationAsset;
        AudioSpatialParams           Attenuation; // Only attenuation, cone and Doppler settings are authored.
    };

    class AudioEventAsset final : public Asset
    {
    public:
        explicit AudioEventAsset(AudioEventData data) : m_Data(std::move(data)) {}
        static AssetType      GetStaticType() { return AssetType::AudioEvent; }
        AssetType             GetAssetType() const override { return GetStaticType(); }
        const AudioEventData& GetData() const { return m_Data; }

    private:
        AudioEventData m_Data;
    };

    struct AudioEventParams
    {
        std::optional<glm::vec3>   Position;
        float                      Volume = 1, Pitch = 1; // Linear multipliers.
        std::optional<uint8_t>     Priority;
        std::optional<std::string> Bus;
        // Used by component playback. Position, when supplied, takes precedence over Emitter.Position.
        std::optional<AudioSpatialParams> Emitter;
        AssetID                           AttenuationAsset; // Per-trigger override.
        std::optional<glm::mat4>          EntityTransform;
        AudioEmitterShape                 EntityShape;
        std::optional<bool>               Spatial, Loop;
        std::optional<AudioSpatializer>   Spatializer;
        std::optional<float>              SpatialBlend;
        AudioOcclusionOverrides           Occlusion;
        bool                              ReverbZoneSend = true, PinToCenter = false;
        std::optional<float>              LfeSendDb;
        float                             Pan = 0;
        std::string                       FallbackConcurrencyGroup;
        uint64_t                          OwnerEntity = 0;
    };

    enum class AudioEventTriggerStatus
    {
        Started,
        Cooldown,
        InstanceLimit,
        MissingClip,
        Invalid,
        Paused
    };

    struct AudioEventTriggerResult
    {
        AudioVoiceHandle        Voice;
        AudioEventTriggerStatus Status = AudioEventTriggerStatus::Invalid;
        // What a started trigger played: the entry's index and the variation applied to it (base, entry
        // offset and random draw summed; the params' linear multipliers are not included).
        size_t Entry    = SIZE_MAX;
        float  VolumeDb = 0, PitchSemitones = 0;
        double StartOffsetSeconds = 0;
        float                      ClipVolume         = 1;
        Ref<const AudioEventAsset> Asset;
    };

    struct AudioEventStats
    {
        uint32_t ActiveInstances = 0;
        uint64_t TotalTriggers = 0, CooldownRejections = 0, LimitRejections = 0;
    };

    std::optional<AssetError>                ValidateAudioEvent(const AudioEventData& data);
    std::variant<AudioEventData, AssetError> ParseAudioEvent(const std::string& text);
    std::variant<std::string, AssetError>    SerializeAudioEvent(const AudioEventData& data);
} // namespace Kans
