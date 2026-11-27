#pragma once
#include <map>
#include <memory>
#include <optional>
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    class Scene;
    class AudioClipRegistry;
    class AudioEventAsset;

    enum class AudioSourceKind
    {
        Clip,
        Event,
        Generator
    };

    struct AudioSourceComponent
    {
        AssetID               Clip, Event, AttenuationAsset;
        // Legacy scenes and callers infer the asset kind from Event when Kind is unset.
        std::optional<AudioSourceKind> Kind;
        AudioGeneratorFormat           Generator;
        AudioSourceKind                GetSourceKind() const
        {
            return Kind.value_or(Event.IsValid() ? AudioSourceKind::Event : AudioSourceKind::Clip);
        }
        AudioEmitterShape     Shape;
        std::string           Bus    = MasterAudioBus;
        uint8_t               Priority = DefaultAudioVoicePriority;
        std::string           ConcurrencyGroup;
        float                 Volume = 1, Pitch = 1;
        bool                  PlayOnStart = true, Loop = false, Spatial = true;
        std::optional<AudioSpatializer> Spatializer;
        std::optional<float>            SpatialBlend;
        AudioOcclusionOverrides         Occlusion;
        float                 Pan         = 0;
        bool                  ReverbZoneSend = true, PinToCenter = false;
        float                 LfeSendDb   = AudioSendOffDb;
        AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
        float                 MinDistance = 1, MaxDistance = 100, Rolloff = 1, DopplerFactor = 1;
        float                 ConeInnerAngle = 360, ConeOuterAngle = 360, ConeOuterGain = 0;

        AudioSourceComponent();
        ~AudioSourceComponent();
        AudioSourceComponent(const AudioSourceComponent&);
        AudioSourceComponent& operator=(const AudioSourceComponent&);
        AudioSourceComponent(AudioSourceComponent&&) noexcept;
        AudioSourceComponent&            operator=(AudioSourceComponent&&) noexcept;
        AudioVoiceHandle                 GetVoiceHandle() const;
        const std::optional<AssetError>& GetLastError() const;

    private:
        friend class Scene;
        struct Runtime
        {
            ~Runtime();
            Ref<const AudioClipAsset> Source;
            AssetID                   RequestedClip, RequestedEvent;
            AudioSourceKind                     RequestedKind = AudioSourceKind::Clip;
            std::optional<AudioGeneratorFormat> RequestedGenerator;
            void                                SetSource(const AudioSourceComponent& source);
            bool                                MatchesSource(const AudioSourceComponent& source) const;
            AudioEngine*              Engine   = nullptr;
            AudioClipRegistry*        Registry = nullptr;
            AudioVoiceHandle          Voice;
            glm::vec3                 PreviousPosition {0};
            bool                      Acquired = false, HasPosition = false, PendingStart = false;
            bool                       Spatial = false;
            std::optional<double>      PendingSeek;
            Ref<const AudioEventAsset> EventAsset;
            float                      EventVolume = 1, EventPitch = 1;
            std::optional<AssetError> LastError;
        };
        std::unique_ptr<Runtime> m_Runtime;
    };
    bool operator==(const AudioSourceComponent&, const AudioSourceComponent&);

    class MusicSetPlayer;
    struct MusicSetEvent;

    struct MusicSetPlayerSettings
    {
        AssetID                                   Asset;
        std::string                               BusOverride, InitialState;
        bool                                      PlayOnStart = true;
        std::map<std::string, float, std::less<>> Parameters;
        bool                                      operator==(const MusicSetPlayerSettings& other) const
        {
            return Asset == other.Asset && BusOverride == other.BusOverride && InitialState == other.InitialState &&
                   PlayOnStart == other.PlayOnStart && Parameters == other.Parameters;
        }
        bool operator!=(const MusicSetPlayerSettings& other) const { return !(*this == other); }
    };

    struct MusicSetPlayerComponent : MusicSetPlayerSettings
    {
        MusicSetPlayerComponent();
        ~MusicSetPlayerComponent();
        MusicSetPlayerComponent(const MusicSetPlayerComponent& other);
        MusicSetPlayerComponent& operator=(const MusicSetPlayerComponent& other);
        MusicSetPlayerComponent(MusicSetPlayerComponent&&) noexcept;
        MusicSetPlayerComponent& operator=(MusicSetPlayerComponent&&) noexcept;
        bool                     operator==(const MusicSetPlayerComponent& other) const;
        bool                     operator!=(const MusicSetPlayerComponent& other) const { return !(*this == other); }

    private:
        friend class Scene;
        struct Runtime;
        std::unique_ptr<Runtime> m_Runtime;
        bool                     m_PlayRequested = false, m_Listen = false;
    };

    enum class AudioReverbZoneShape
    {
        Sphere,
        Box
    };

    struct AudioReverbZoneComponent
    {
        AudioReverbZoneShape Shape  = AudioReverbZoneShape::Sphere;
        float                Radius = 1, FadeDistance = 1;
        glm::vec3            HalfExtents {1};
        std::string          Bus      = "Reverb";
        float                SendDb   = 0;
        int                  Priority = 0;

        bool operator==(const AudioReverbZoneComponent& other) const
        {
            return Shape == other.Shape && Radius == other.Radius && HalfExtents == other.HalfExtents &&
                   FadeDistance == other.FadeDistance && Bus == other.Bus && SendDb == other.SendDb &&
                   Priority == other.Priority;
        }
        bool operator!=(const AudioReverbZoneComponent& other) const { return !(*this == other); }
    };

    float AudioReverbZoneWeight(const AudioReverbZoneComponent& zone, const glm::mat4& world, glm::vec3 listener);

    struct AudioListenerComponent
    {
        bool Active              = true;
        AudioListenerComponent() = default;
        AudioListenerComponent(const AudioListenerComponent& other) : Active(other.Active) {}
        AudioListenerComponent(AudioListenerComponent&&) noexcept            = default;
        AudioListenerComponent& operator=(AudioListenerComponent&&) noexcept = default;
        AudioListenerComponent& operator=(const AudioListenerComponent& other)
        {
            Active             = other.Active;
            m_HasPosition      = false;
            m_PreviousPosition = {};
            return *this;
        }

    private:
        friend class Scene;
        glm::vec3 m_PreviousPosition {0};
        bool      m_HasPosition = false;
    };
    inline bool operator==(const AudioListenerComponent& a, const AudioListenerComponent& b)
    {
        return a.Active == b.Active;
    }
} // namespace Kans
