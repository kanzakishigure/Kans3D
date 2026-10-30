#include "kspch.h"
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Entity.h"
#include "Scene.h"

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
        UpdateAudioListener(0);
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            if (m_Registry.get<AudioSourceComponent>(handle).PlayOnStart)
                PlayAudioSource({handle, this});
    }
    void Scene::StopAudio()
    {
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            m_Registry.get<AudioSourceComponent>(handle).m_Runtime.reset();
        for (auto handle : m_Registry.view<AudioListenerComponent>())
            m_Registry.get<AudioListenerComponent>(handle).m_HasPosition = false;
        m_AudioClips.Clear();
        m_AudioRunning = m_AudioPaused = m_WarnedMultipleListeners = false;
        m_AudioListener                                            = 0;
        m_HasCameraAudioPosition                                   = false;
    }
    void Scene::PlayAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return;
        auto& c = entity.GetComponent<AudioSourceComponent>();
        if (c.m_Runtime && c.m_Runtime->RequestedClip != c.Clip)
            c.m_Runtime.reset();
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
        desc.Volume  = c.Volume;
        desc.Pitch   = c.Pitch;
        desc.Pan     = c.Spatial ? 0 : c.Pan;
        desc.Loop    = c.Loop;
        desc.Spatial = c.Spatial;
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
        m_AudioEngine->Play(r.Voice);
    }
    void Scene::PushAudioSource(Entity entity, double seconds)
    {
        auto& c = entity.GetComponent<AudioSourceComponent>();
        auto& r = *c.m_Runtime;
        m_AudioEngine->SetVolume(r.Voice, c.Volume * r.Source->GetData().Volume);
        m_AudioEngine->SetPitch(r.Voice, c.Pitch);
        m_AudioEngine->SetPan(r.Voice, r.Spatial ? 0 : c.Pan);
        m_AudioEngine->SetLooping(r.Voice, c.Loop);
        if (!r.Spatial)
            return;
        const auto         world = GetWorldTransform(entity);
        AudioSpatialParams params;
        params.Position       = glm::vec3(world[3]);
        params.Velocity       = Velocity(params.Position, r.PreviousPosition, r.HasPosition, seconds);
        params.Direction      = Direction(world, {0, 0, -1});
        params.Attenuation    = c.Attenuation;
        params.MinDistance    = c.MinDistance;
        params.MaxDistance    = c.MaxDistance;
        params.Rolloff        = c.Rolloff;
        params.DopplerFactor  = c.DopplerFactor;
        params.ConeInnerAngle = c.ConeInnerAngle;
        params.ConeOuterAngle = c.ConeOuterAngle;
        params.ConeOuterGain  = c.ConeOuterGain;
        r.PreviousPosition    = params.Position;
        r.HasPosition         = true;
        m_AudioEngine->SetSpatialParams(r.Voice, params);
    }
    void Scene::UpdateAudioListener(double seconds)
    {
        Entity   selected;
        unsigned active = 0;
        for (auto handle : m_Registry.view<AudioListenerComponent, IDComponent>())
            if (m_Registry.get<AudioListenerComponent>(handle).Active)
            {
                ++active;
                Entity e {handle, this};
                if (!selected || e.GetUUID() < selected.GetUUID())
                    selected = e;
            }
        if (active > 1 && !m_WarnedMultipleListeners)
        {
            Warn("Multiple active audio listeners; using the first by UUID");
            m_WarnedMultipleListeners = true;
        }
        if (!selected)
            selected = GetCameraEntity();
        AudioListenerState listener;
        const UUID         id = selected ? selected.GetUUID() : UUID(0);
        if (selected)
        {
            const auto world  = GetWorldTransform(selected);
            listener.Position = glm::vec3(world[3]);
            listener.Forward  = Direction(world, {0, 0, -1});
            listener.Up       = Direction(world, {0, 1, 0});
            if (selected.HasComponent<AudioListenerComponent>() &&
                selected.GetComponent<AudioListenerComponent>().Active)
            {
                auto& c           = selected.GetComponent<AudioListenerComponent>();
                listener.Velocity = Velocity(
                    listener.Position, c.m_PreviousPosition, c.m_HasPosition && m_AudioListener == id, seconds);
                c.m_PreviousPosition = listener.Position;
                c.m_HasPosition      = true;
            }
            else
            {
                listener.Velocity        = Velocity(listener.Position,
                                                    m_CameraAudioPosition,
                                                    m_HasCameraAudioPosition && m_AudioListener == id,
                                                    seconds);
                m_CameraAudioPosition    = listener.Position;
                m_HasCameraAudioPosition = true;
            }
        }
        m_AudioListener = id;
        m_AudioEngine->SetListener(0, listener);
    }
    void Scene::UpdateAudio(double seconds)
    {
        if (!m_AudioEngine || !m_AudioRunning)
            return;
        // Clip replacement also cancels paused voices; no stale registrations survive a Step.
        for (auto handle : m_Registry.view<AudioSourceComponent>())
        {
            auto& c = m_Registry.get<AudioSourceComponent>(handle);
            if (c.m_Runtime && c.m_Runtime->RequestedClip != c.Clip)
            {
                const bool pending = c.m_Runtime->PendingStart;
                c.m_Runtime.reset();
                if (pending)
                    PlayAudioSource({handle, this});
            }
        }
        if (m_AudioPaused)
            return;
        UpdateAudioListener(seconds);
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
            else if (c.m_Runtime->Acquired)
                c.m_Runtime.reset();
        }
    }
    void Scene::OnRuntimePause()
    {
        m_PhysicsPaused = true;
        if (!m_AudioRunning || m_AudioPaused)
            return;
        m_AudioPaused = true;
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
        if (!m_AudioEngine)
            return;
        m_AudioListener = 0;
        UpdateAudioListener(0);
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
