#include "kspch.h"
#include "AudioComponents.h"
#include <tuple>
#include <glm/geometric.hpp>
#include "AudioClipRegistry.h"

namespace Kans
{
    namespace
    {
        template<class T>
        auto Fields(T& c)
        {
            return std::tie(c.Clip,
                            c.Event,
                            c.Kind,
                            c.Generator,
                            c.AttenuationAsset,
                            c.Shape,
                            c.Bus,
                            c.Priority,
                            c.ConcurrencyGroup,
                            c.Volume,
                            c.Pitch,
                            c.PlayOnStart,
                            c.Loop,
                            c.Spatial,
                            c.Spatializer,
                            c.SpatialBlend,
                            c.Occlusion,
                            c.Pan,
                            c.ReverbZoneSend,
                            c.PinToCenter,
                            c.LfeSendDb,
                            c.Attenuation,
                            c.MinDistance,
                            c.MaxDistance,
                            c.Rolloff,
                            c.DopplerFactor,
                            c.ConeInnerAngle,
                            c.ConeOuterAngle,
                            c.ConeOuterGain);
        }
    } // namespace
    float AudioReverbZoneWeight(const AudioReverbZoneComponent& zone, const glm::mat4& world, glm::vec3 listener)
    {
        AudioEmitterShape shape;
        shape.Type        = zone.Shape == AudioReverbZoneShape::Box ? AudioSourceShape::Box : AudioSourceShape::Sphere;
        shape.Radius      = zone.Radius;
        shape.HalfExtents = zone.HalfExtents;
        const float distance = glm::length(ClosestAudioEmitterPoint(shape, world, listener) - listener);
        if (distance == 0)
            return 1;
        if (!(distance < zone.FadeDistance))
            return 0;
        const float t = 1 - distance / zone.FadeDistance;
        return t * t * (3 - 2 * t);
    }

    void AudioSourceComponent::Runtime::SetSource(const AudioSourceComponent& source)
    {
        RequestedKind      = source.GetSourceKind();
        RequestedClip      = source.Clip;
        RequestedEvent     = source.Event;
        RequestedGenerator = RequestedKind == AudioSourceKind::Generator ?
                                 std::optional<AudioGeneratorFormat>(source.Generator) :
                                 std::nullopt;
    }

    bool AudioSourceComponent::Runtime::MatchesSource(const AudioSourceComponent& source) const
    {
        if (RequestedKind != source.GetSourceKind())
            return false;
        switch (RequestedKind)
        {
            case AudioSourceKind::Generator:
                return RequestedGenerator == source.Generator;
            case AudioSourceKind::Event:
                return RequestedEvent == source.Event;
            case AudioSourceKind::Clip:
                return RequestedClip == source.Clip;
        }
        return false;
    }

    AudioSourceComponent::Runtime::~Runtime()
    {
        if (Engine)
            Engine->DestroyVoice(Voice);
        if (Acquired && Registry && Source)
            Registry->Release(Source->assetID);
    }
    AudioSourceComponent::AudioSourceComponent()  = default;
    AudioSourceComponent::~AudioSourceComponent() = default;
    AudioSourceComponent::AudioSourceComponent(const AudioSourceComponent& other) { Fields(*this) = Fields(other); }
    AudioSourceComponent& AudioSourceComponent::operator=(const AudioSourceComponent& other)
    {
        if (this != &other)
        {
            m_Runtime.reset();
            Fields(*this) = Fields(other);
        }
        return *this;
    }
    AudioSourceComponent::AudioSourceComponent(AudioSourceComponent&&) noexcept            = default;
    AudioSourceComponent& AudioSourceComponent::operator=(AudioSourceComponent&&) noexcept = default;
    AudioVoiceHandle      AudioSourceComponent::GetVoiceHandle() const
    {
        return m_Runtime ? m_Runtime->Voice : AudioVoiceHandle {};
    }
    const std::optional<AssetError>& AudioSourceComponent::GetLastError() const
    {
        static const std::optional<AssetError> empty;
        return m_Runtime ? m_Runtime->LastError : empty;
    }
    bool operator==(const AudioSourceComponent& a, const AudioSourceComponent& b) { return Fields(a) == Fields(b); }
} // namespace Kans
