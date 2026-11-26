#pragma once
#include <memory>
#include <optional>
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    class Scene;
    class AudioClipRegistry;

    struct AudioSourceComponent
    {
        AssetID               Clip, Event, AttenuationAsset;
        AudioEmitterShape     Shape;
        std::string           Bus    = MasterAudioBus;
        uint8_t               Priority = DefaultAudioVoicePriority;
        std::string           ConcurrencyGroup;
        float                 Volume = 1, Pitch = 1;
        bool                  PlayOnStart = true, Loop = false, Spatial = true;
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
            AudioEngine*              Engine   = nullptr;
            AudioClipRegistry*        Registry = nullptr;
            AudioVoiceHandle          Voice;
            glm::vec3                 PreviousPosition {0};
            bool                      Acquired = false, HasPosition = false, PendingStart = false;
            bool                      ResumeOnUnpause = false, Spatial = false;
            std::optional<AssetError> LastError;
        };
        std::unique_ptr<Runtime> m_Runtime;
    };
    bool operator==(const AudioSourceComponent&, const AudioSourceComponent&);

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
