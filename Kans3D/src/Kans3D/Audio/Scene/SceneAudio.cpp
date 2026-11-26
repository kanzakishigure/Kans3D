#include "kspch.h"
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/TimelineAudioPlayer.h"

namespace Kans
{
    namespace
    {
        void Warn(const std::string& message)
        {
            if (Log::GetCoreLogger())
                CORE_WARN("Audio: {}", message);
            else
                std::fprintf(stderr, "Audio: %s\n", message.c_str());
        }
        glm::vec3 Direction(const glm::mat4& world, const glm::vec3& axis)
        {
            const auto v      = glm::vec3(world * glm::vec4(axis, 0));
            const auto length = glm::dot(v, v);
            return std::isfinite(length) && length > 0 ? v / std::sqrt(length) : axis;
        }
        glm::vec3 Velocity(glm::vec3 position, glm::vec3 previous, bool hasPrevious, double seconds)
        {
            return hasPrevious && std::isfinite(seconds) && seconds > 0 ? (position - previous) / float(seconds) :
                                                                          glm::vec3(0);
        }
    } // namespace

    bool Scene::OwnsAudioSource(Entity entity) const
    {
        return entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle) &&
               m_Registry.all_of<AudioSourceComponent>(entity.m_EntityHandle);
    }
    void Scene::SetAudioEngine(AudioEngine* engine)
    {
        if (m_AudioEngine == engine)
            return;
        const bool running = m_AudioRunning, paused = m_AudioPaused;
        StopAudio();
        m_AudioEngine = engine;
        if (running)
        {
            m_AudioPaused = paused;
            StartAudio();
        }
    }
    void Scene::StartAudio()
    {
        m_AudioRunning = true;
        if (!m_AudioEngine)
            return;
        m_AudioZones.reserve(m_Registry.view<AudioReverbZoneComponent>().size());
        UpdateAudioListeners(0);
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            if (m_Registry.get<AudioSourceComponent>(handle).PlayOnStart)
                PlayAudioSource({handle, this});
    }
    void Scene::StopAudio()
    {
        for (auto handle : m_Registry.view<TimelinePlayerComponent>())
            if (auto& player = m_Registry.get<TimelinePlayerComponent>(handle).Player)
                player->m_Audio.reset();
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            m_Registry.get<AudioSourceComponent>(handle).m_Runtime.reset();
        for (auto handle : m_Registry.view<AudioListenerComponent>())
            m_Registry.get<AudioListenerComponent>(handle).m_HasPosition = false;
        m_EntityAudioEvents.clear();
        m_AudioClips.Clear();
        m_AudioZones.clear();
        m_AudioRunning = m_AudioPaused                             = false;
        m_AudioCamera                                              = 0;
        m_HasCameraAudioPosition                                   = false;
    }
    void Scene::PlayAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return;
        auto& c = entity.GetComponent<AudioSourceComponent>();
        if (c.m_Runtime &&
            (c.m_Runtime->RequestedEvent != c.Event || (!c.Event.IsValid() && c.m_Runtime->RequestedClip != c.Clip)))
            c.m_Runtime.reset();
        if (c.Event.IsValid())
        {
            if (!c.m_Runtime)
                c.m_Runtime = std::make_unique<AudioSourceComponent::Runtime>();
            auto& r          = *c.m_Runtime;
            r.RequestedEvent = c.Event;
            r.Engine         = m_AudioEngine;
            r.PendingStart   = true;
            r.LastError.reset();
            if (!m_AudioPaused)
                PrepareAudioSource(entity);
            return;
        }
        if (c.m_Runtime && m_AudioEngine->IsVoiceValid(c.m_Runtime->Voice) &&
            !m_AudioEngine->HasEnded(c.m_Runtime->Voice))
        {
            c.m_Runtime->HasPosition = false;
            PushAudioSource(entity, 0);
            if (m_AudioPaused)
                c.m_Runtime->ResumeOnUnpause = true;
            else
                m_AudioEngine->Play(c.m_Runtime->Voice);
            return;
        }
        c.m_Runtime     = std::make_unique<AudioSourceComponent::Runtime>();
        auto& r         = *c.m_Runtime;
        r.RequestedClip = c.Clip;
        r.Engine        = m_AudioEngine;
        r.Registry      = &m_AudioClips;
        r.PendingStart  = true;
        if (!m_AudioPaused)
            PrepareAudioSource(entity);
    }
    void Scene::StopAudioSource(Entity entity)
    {
        if (m_AudioEngine && m_AudioRunning && OwnsAudioSource(entity))
            entity.GetComponent<AudioSourceComponent>().m_Runtime.reset();
    }

    AudioEventTriggerResult Scene::PlayEvent(AssetID event, const AudioEventParams& params)
    {
        if (!m_AudioEngine || !m_AudioRunning || m_AudioPaused || !m_PrefabAssets)
            return {};
        return m_AudioEngine->GetEventPlayer().Trigger(*m_PrefabAssets, m_AudioClips, event, params);
    }

    AudioEventTriggerResult Scene::PlayEvent(Entity emitter, AssetID event, AudioEventParams params)
    {
        if (emitter.m_Scene != this || !m_Registry.valid(emitter.m_EntityHandle))
            return {};
        params.EntityTransform = GetWorldTransform(emitter);
        params.Position        = glm::vec3((*params.EntityTransform)[3]);
        if (emitter.HasComponent<AudioSourceComponent>())
        {
            const auto& source = emitter.GetComponent<AudioSourceComponent>();
            params.EntityShape = source.Shape;
            params.ReverbZoneSend &= source.ReverbZoneSend;
            params.PinToCenter |= source.PinToCenter;
            if (source.LfeSendDb != AudioSendOffDb && !params.LfeSendDb)
                params.LfeSendDb = source.LfeSendDb;
            if (source.AttenuationAsset.IsValid())
                params.AttenuationAsset = source.AttenuationAsset;
        }
        const auto result = PlayEvent(event, params);
        if (result.Voice.IsValid())
            m_EntityAudioEvents.push_back({result.Voice, emitter.m_EntityHandle, *params.Position});
        return result;
    }

    AudioEventTriggerResult Scene::PlayEvent(glm::vec3 position, AssetID event, AudioEventParams params)
    {
        params.Position = position;
        return PlayEvent(event, params);
    }

    AudioVoiceHandle Scene::PlayOneShot(AssetID clip, const AudioOneShotParams& params)
    {
        if (!m_AudioEngine || !m_AudioRunning || m_AudioPaused || !m_PrefabAssets || !clip.IsValid())
            return {};
        auto loaded = m_PrefabAssets->Request<AudioClipAsset>(clip);
        return loaded.IsReady() ? m_AudioEngine->PlayOneShot(m_AudioClips, loaded.Asset, params) : AudioVoiceHandle {};
    }

    AudioVoiceHandle Scene::PlayOneShot(Entity entity, AssetID clip, AudioOneShotParams params)
    {
        if (entity.m_Scene != this || !m_Registry.valid(entity.m_EntityHandle))
            return {};
        return PlayOneShot(glm::vec3(GetWorldTransform(entity)[3]), clip, std::move(params));
    }

    AudioVoiceHandle Scene::PlayOneShot(glm::vec3 position, AssetID clip, AudioOneShotParams params)
    {
        params.Position = position;
        return PlayOneShot(clip, params);
    }
    void Scene::PauseAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return;
        auto& c = entity.GetComponent<AudioSourceComponent>();
        if (c.m_Runtime)
        {
            m_AudioEngine->Pause(c.m_Runtime->Voice);
            c.m_Runtime->PendingStart    = false;
            c.m_Runtime->ResumeOnUnpause = false;
        }
    }
    bool Scene::IsAudioSourcePlaying(Entity entity) const
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return false;
        return m_AudioEngine->IsPlaying(entity.GetComponent<AudioSourceComponent>().GetVoiceHandle());
    }
    void Scene::PrepareAudioSource(Entity entity)
    {
        auto& c = entity.GetComponent<AudioSourceComponent>();
        auto& r = *c.m_Runtime;
        if (!r.PendingStart || r.LastError || m_AudioPaused)
            return;
        auto fail = [&](AssetError error) {
            r.LastError    = std::move(error);
            r.PendingStart = false;
            Warn("source " + std::to_string(uint64_t(entity.GetUUID())) + ": " + r.LastError->Message);
        };
        if (c.Event.IsValid())
        {
            // Event playback snapshots gain, pitch, routing and loop settings at Play.
            // Component volume and pitch multiply. Master and default priority inherit.
            // The component can disable spatial playback or enable looping. When spatial,
            // the component supplies its A1 attenuation and follows the entity transform.
            AudioEventParams params;
            params.Volume = c.Volume;
            params.Pitch  = c.Pitch;
            if (!c.Bus.empty() && c.Bus != MasterAudioBus)
                params.Bus = c.Bus;
            if (c.Priority != DefaultAudioVoicePriority)
                params.Priority = c.Priority;
            if (!c.Spatial)
                params.Spatial = false;
            if (c.Loop)
                params.Loop = true;
            params.Pan                      = c.Pan;
            params.ReverbZoneSend           = c.ReverbZoneSend;
            params.PinToCenter              = c.PinToCenter;
            if (c.LfeSendDb != AudioSendOffDb)
                params.LfeSendDb = c.LfeSendDb;
            params.Emitter                  = GetAudioSpatialParams(entity);
            params.AttenuationAsset         = c.AttenuationAsset;
            params.FallbackConcurrencyGroup = c.ConcurrencyGroup;
            const auto result               = PlayEvent(c.Event, params);
            r.PendingStart                  = false;
            switch (result.Status)
            {
                case AudioEventTriggerStatus::Started:
                    // A stolen voice owns its clip through the fade, even after this source restarts.
                    if (!m_AudioEngine->IsVoiceReleasing(r.Voice))
                        m_AudioEngine->DestroyVoice(r.Voice);
                    r.Voice       = result.Voice;
                    r.HasPosition = r.ResumeOnUnpause = false;
                    return;
                case AudioEventTriggerStatus::Cooldown:
                    r.LastError = AssetError {AssetErrorCode::InvalidArgument, "Audio event is on cooldown"};
                    return;
                case AudioEventTriggerStatus::InstanceLimit:
                    r.LastError = AssetError {AssetErrorCode::InvalidArgument, "Audio event instance limit reached"};
                    return;
                case AudioEventTriggerStatus::MissingClip:
                    fail({AssetErrorCode::NotFound, "Audio event clip load failed"});
                    return;
                case AudioEventTriggerStatus::Invalid:
                    fail({AssetErrorCode::InvalidData, "Audio event playback failed"});
                    return;
            }
        }
        if (!c.Clip.IsValid() || !m_PrefabAssets)
        {
            fail({AssetErrorCode::InvalidData, "Audio clip or scene AssetManager is missing"});
            return;
        }
        auto loaded = m_PrefabAssets->Request<AudioClipAsset>(c.Clip);
        if (loaded.State == AssetLoadState::Failed)
        {
            fail(loaded.Error.value_or(AssetError {AssetErrorCode::InvalidData, "Audio clip load failed"}));
            return;
        }
        if (!loaded.IsReady())
            return;
        r.Source = loaded.Asset;
        if (!m_AudioClips.Acquire(*m_AudioEngine, r.Source))
        {
            fail({AssetErrorCode::InvalidData, "Audio clip registration failed"});
            r.Source.reset();
            return;
        }
        r.Acquired = true;
        AudioVoiceDesc desc;
        desc.Bus     = c.Bus;
        desc.Priority         = c.Priority;
        desc.ConcurrencyGroup = c.ConcurrencyGroup;
        desc.Volume  = c.Volume;
        desc.Pitch   = c.Pitch;
        desc.Pan     = c.Spatial ? 0 : c.Pan;
        desc.Loop    = c.Loop;
        desc.Spatial = c.Spatial;
        desc.ReverbZoneSend   = c.ReverbZoneSend;
        desc.LfeSendDb        = c.LfeSendDb;
        desc.PinToCenter      = c.PinToCenter;
        r.Spatial    = c.Spatial;
        r.Voice      = m_AudioClips.CreateVoice(*m_AudioEngine, r.Source, desc);
        if (!m_AudioEngine->IsVoiceValid(r.Voice))
        {
            m_AudioClips.Release(r.Source->assetID);
            r.Acquired = false;
            r.Source.reset();
            fail({AssetErrorCode::InvalidData, "Audio voice creation failed"});
            return;
        }
        r.PendingStart = false;
        PushAudioSource(entity, 0);
        if (!m_AudioEngine->Play(r.Voice))
        {
            m_AudioEngine->DestroyVoice(r.Voice);
            r.Voice = {};
            m_AudioClips.Release(r.Source->assetID);
            r.Acquired = false;
            r.Source.reset();
        }
    }
    void Scene::PushAudioSource(Entity entity, double seconds)
    {
        auto& c = entity.GetComponent<AudioSourceComponent>();
        auto& r = *c.m_Runtime;
        if (!r.RequestedEvent.IsValid())
        {
            m_AudioEngine->SetVoiceBus(r.Voice, c.Bus);
            m_AudioEngine->SetVoicePriority(r.Voice, c.Priority);
            m_AudioEngine->SetVolume(r.Voice, c.Volume * r.Source->GetData().Volume);
            m_AudioEngine->SetPitch(r.Voice, c.Pitch);
            m_AudioEngine->SetPan(r.Voice, r.Spatial ? 0 : c.Pan);
            m_AudioEngine->SetLooping(r.Voice, c.Loop);
            m_AudioEngine->SetVoiceRouting(r.Voice, c.ReverbZoneSend, c.LfeSendDb, c.PinToCenter);
            if (!r.Spatial)
            {
                m_AudioEngine->SetVoiceReverbPosition(r.Voice, glm::vec3(GetWorldTransform(entity)[3]));
                return;
            }
        }
        auto params           = GetAudioSpatialParams(entity);
        params.Velocity       = Velocity(params.Position, r.PreviousPosition, r.HasPosition, seconds);
        r.PreviousPosition    = params.Position;
        r.HasPosition         = true;
        m_AudioEngine->SetVoiceReverbPosition(r.Voice, params.Position);
        m_AudioEngine->SetSpatialParams(r.Voice, params);
        m_AudioEngine->SetVoiceAttenuationAsset(r.Voice, m_PrefabAssets, c.AttenuationAsset);
    }
    AudioSpatialParams Scene::GetAudioSpatialParams(Entity entity) const
    {
        const auto         world = GetWorldTransform(entity);
        AudioSpatialParams params;
        params.Position  = glm::vec3(world[3]);
        params.Direction = Direction(world, {0, 0, -1});
        if (entity.HasComponent<AudioSourceComponent>())
        {
            const auto& c         = entity.GetComponent<AudioSourceComponent>();
            params.Shape          = c.Shape;
            if (c.Shape.Type != AudioSourceShape::Point)
                params.ShapeTransform = world;
            params.Attenuation    = c.Attenuation;
            params.MinDistance    = c.MinDistance;
            params.MaxDistance    = c.MaxDistance;
            params.Rolloff        = c.Rolloff;
            params.DopplerFactor  = c.DopplerFactor;
            params.ConeInnerAngle = c.ConeInnerAngle;
            params.ConeOuterAngle = c.ConeOuterAngle;
            params.ConeOuterGain  = c.ConeOuterGain;
        }
        return params;
    }

    void Scene::UpdateAudioListeners(double seconds)
    {
        std::array<Entity, MaxAudioListeners> selected;
        size_t                                count    = 0;
        const size_t                          capacity = m_AudioEngine->GetMaxListeners();
        for (auto handle : m_Registry.view<AudioListenerComponent, IDComponent>())
        {
            auto& component = m_Registry.get<AudioListenerComponent>(handle);
            if (!component.Active)
            {
                component.m_HasPosition = false;
                continue;
            }
            Entity entity {handle, this};
            size_t index = 0;
            while (index < count && selected[index].GetUUID() < entity.GetUUID())
                ++index;
            if (index == capacity)
            {
                component.m_HasPosition = false;
                continue;
            }
            if (count == capacity)
                selected[count - 1].GetComponent<AudioListenerComponent>().m_HasPosition = false;
            else
                ++count;
            std::move_backward(selected.begin() + index, selected.begin() + count - 1, selected.begin() + count);
            selected[index] = entity;
        }

        std::array<AudioListenerState, MaxAudioListeners> states;
        for (size_t i = 0; i < count; ++i)
        {
            const auto world     = GetWorldTransform(selected[i]);
            auto&      state     = states[i];
            auto&      component = selected[i].GetComponent<AudioListenerComponent>();
            state.Identity       = selected[i].GetUUID();
            state.Position       = glm::vec3(world[3]);
            state.Forward        = Direction(world, {0, 0, -1});
            state.Up             = Direction(world, {0, 1, 0});
            state.Velocity = Velocity(state.Position, component.m_PreviousPosition, component.m_HasPosition, seconds);
            component.m_PreviousPosition = state.Position;
            component.m_HasPosition      = true;
        }
        if (count == 0)
        {
            count             = 1;
            const auto camera = GetCameraEntity();
            if (camera)
            {
                auto&      state         = states[0];
                const auto world         = GetWorldTransform(camera);
                state.Identity           = camera.GetUUID();
                state.Position           = glm::vec3(world[3]);
                state.Forward            = Direction(world, {0, 0, -1});
                state.Up                 = Direction(world, {0, 1, 0});
                state.Velocity           = Velocity(state.Position,
                                                    m_CameraAudioPosition,
                                                    m_HasCameraAudioPosition && m_AudioCamera == camera.GetUUID(),
                                                    seconds);
                m_CameraAudioPosition    = state.Position;
                m_HasCameraAudioPosition = true;
                m_AudioCamera            = camera.GetUUID();
            }
            else
                m_HasCameraAudioPosition = false;
        }
        else
            m_HasCameraAudioPosition = false;
        UpdateAudioZones(states.data(), count);
        m_AudioEngine->SetListeners(states.data(), count);
    }

    uint32_t Scene::GetAudioListenerCapacity() const
    {
        return m_AudioEngine ? std::min(m_AudioEngine->GetMaxListeners(), MaxAudioListeners) : MaxAudioListeners;
    }

    std::optional<uint32_t> Scene::GetAudioListenerSlot(Entity entity) const
    {
        if (!m_Registry.valid(entity) || !m_Registry.all_of<AudioListenerComponent, IDComponent>(entity))
            return std::nullopt;
        if (!m_Registry.get<AudioListenerComponent>(entity).Active)
            return std::nullopt;
        // UpdateAudioListeners' order: the active listeners with a lower UUID come first.
        const uint64_t id   = m_Registry.get<IDComponent>(entity).ID;
        uint32_t       slot = 0;
        for (auto handle : m_Registry.view<AudioListenerComponent, IDComponent>())
            if (m_Registry.get<AudioListenerComponent>(handle).Active &&
                uint64_t(m_Registry.get<IDComponent>(handle).ID) < id)
                ++slot;
        return slot < GetAudioListenerCapacity() ? std::optional<uint32_t>(slot) : std::nullopt;
    }

    void Scene::UpdateAudioZones(AudioListenerState* listeners, size_t listenerCount)
    {
        m_AudioZones.clear();
        for (auto handle : m_Registry.view<AudioReverbZoneComponent, IDComponent>())
        {
            const auto& zone = m_Registry.get<AudioReverbZoneComponent>(handle);
            const auto  bus  = m_AudioEngine->ResolveReverbBus(zone.Bus);
            if (bus && IsValidAudioBusLevel(zone.SendDb))
            {
                Entity entity {handle, this};
                m_AudioZones.push_back({&zone, GetWorldTransform(entity), entity.GetUUID(), *bus});
            }
        }
        std::sort(m_AudioZones.begin(), m_AudioZones.end(), [](const auto& a, const auto& b) {
            return a.Zone->Priority != b.Zone->Priority ? a.Zone->Priority > b.Zone->Priority : a.ID < b.ID;
        });

        // Composite high priority first. An interior fills the remaining coverage.
        std::array<float, MaxAudioListeners> remaining;
        remaining.fill(1);
        for (auto& entry : m_AudioZones)
        {
            const float gain = std::pow(10.f, entry.Zone->SendDb / 20.f);
            for (size_t i = 0; i < listenerCount; ++i)
            {
                if (remaining[i] == 0)
                    continue;
                const float weight = AudioReverbZoneWeight(*entry.Zone, entry.World, listeners[i].Position);
                entry.Gains[i]     = remaining[i] * weight * gain;
                remaining[i] *= 1 - weight;
            }
        }

        // Combine zones sharing a bus before selecting the two strongest bus sends.
        std::sort(m_AudioZones.begin(), m_AudioZones.end(), [](const auto& a, const auto& b) {
            return a.Bus != b.Bus ? a.Bus < b.Bus : a.ID < b.ID;
        });
        for (size_t begin = 0; begin < m_AudioZones.size();)
        {
            const auto                           bus = m_AudioZones[begin].Bus;
            std::array<float, MaxAudioListeners> gains {};
            do
            {
                for (size_t i = 0; i < listenerCount; ++i)
                    gains[i] += m_AudioZones[begin].Gains[i];
                ++begin;
            } while (begin < m_AudioZones.size() && m_AudioZones[begin].Bus == bus);

            for (size_t i = 0; i < listenerCount; ++i)
            {
                auto& sends = listeners[i].ReverbSends;
                for (auto it = sends.begin(); it != sends.end(); ++it)
                    if (gains[i] > it->Gain)
                    {
                        std::move_backward(it, sends.end() - 1, sends.end());
                        *it = {bus, gains[i]};
                        break;
                    }
            }
        }
    }

    void Scene::UpdateAudio(double seconds)
    {
        if (!m_AudioEngine || !m_AudioRunning)
            return;
        // Clip replacement also cancels paused voices; no stale registrations survive a Step.
        for (auto handle : m_Registry.view<AudioSourceComponent>())
        {
            auto& c = m_Registry.get<AudioSourceComponent>(handle);
            if (c.m_Runtime && (c.m_Runtime->RequestedEvent != c.Event ||
                                (!c.Event.IsValid() && c.m_Runtime->RequestedClip != c.Clip)))
            {
                const bool pending = c.m_Runtime->PendingStart;
                c.m_Runtime.reset();
                if (pending)
                    PlayAudioSource({handle, this});
            }
        }
        if (m_AudioPaused)
            return;
        UpdateAudioListeners(seconds);
        const auto expired =
            std::remove_if(m_EntityAudioEvents.begin(), m_EntityAudioEvents.end(), [&](EntityAudioEvent& instance) {
                if (!m_AudioEngine->IsVoiceValid(instance.Voice) || !m_Registry.valid(instance.Emitter))
                    return true;
                Entity emitter {instance.Emitter, this};
                auto   spatial            = GetAudioSpatialParams(emitter);
                spatial.Velocity          = Velocity(spatial.Position, instance.PreviousPosition, true, seconds);
                instance.PreviousPosition = spatial.Position;
                m_AudioEngine->SetVoiceReverbPosition(instance.Voice, spatial.Position);
                m_AudioEngine->SetVoiceEmitter(instance.Voice, spatial);
                return false;
            });
        m_EntityAudioEvents.erase(expired, m_EntityAudioEvents.end());
        for (auto handle : m_Registry.view<AudioSourceComponent>())
        {
            auto& c = m_Registry.get<AudioSourceComponent>(handle);
            if (!c.m_Runtime)
                continue;
            Entity e {handle, this};
            PrepareAudioSource(e);
            if (m_AudioEngine->IsVoiceValid(c.m_Runtime->Voice))
            {
                PushAudioSource(e, seconds);
                if (m_AudioEngine->HasEnded(c.m_Runtime->Voice))
                    c.m_Runtime.reset();
            }
            else if (c.m_Runtime->Acquired ||
                     (c.Event.IsValid() && !c.m_Runtime->LastError && !c.m_Runtime->PendingStart))
                c.m_Runtime.reset();
        }
    }
    void Scene::OnRuntimePause()
    {
        m_PhysicsPaused = true;
        if (!m_AudioRunning || m_AudioPaused)
            return;
        m_AudioPaused = true;
        for (auto handle : m_Registry.view<TimelinePlayerComponent>())
            if (auto& player = m_Registry.get<TimelinePlayerComponent>(handle).Player)
                player->SetAudioPaused(true);
        if (!m_AudioEngine)
            return;
        for (auto handle : m_Registry.view<AudioSourceComponent>())
        {
            auto& c = m_Registry.get<AudioSourceComponent>(handle);
            if (c.m_Runtime)
            {
                c.m_Runtime->ResumeOnUnpause = m_AudioEngine->IsPlaying(c.m_Runtime->Voice);
                m_AudioEngine->Pause(c.m_Runtime->Voice);
            }
        }
    }
    void Scene::OnRuntimeResume()
    {
        m_PhysicsPaused = false;
        if (!m_AudioRunning || !m_AudioPaused)
            return;
        m_AudioPaused = false;
        for (auto handle : m_Registry.view<TimelinePlayerComponent>())
            if (auto& player = m_Registry.get<TimelinePlayerComponent>(handle).Player)
                player->SetAudioPaused(false);
        if (!m_AudioEngine)
            return;
        m_AudioCamera = 0;
        UpdateAudio(0);
        for (auto handle : m_Registry.view<AudioSourceComponent>())
        {
            auto& c = m_Registry.get<AudioSourceComponent>(handle);
            if (c.m_Runtime)
            {
                c.m_Runtime->HasPosition = false;
                if (m_AudioEngine->IsVoiceValid(c.m_Runtime->Voice))
                {
                    PushAudioSource({handle, this}, 0);
                    if (c.m_Runtime->ResumeOnUnpause)
                        m_AudioEngine->Play(c.m_Runtime->Voice);
                }
                c.m_Runtime->ResumeOnUnpause = false;
            }
        }
    }
} // namespace Kans
