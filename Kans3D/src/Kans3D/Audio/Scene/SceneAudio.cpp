#include "kspch.h"
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/Audio/MusicSetPlayer.h"
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

    struct MusicSetPlayerComponent::Runtime
    {
        explicit Runtime(AudioEngine& engine, const MusicSetPlayerSettings& settings) :
            Settings(settings), Player(engine)
        {}
        MusicSetPlayerSettings                       Settings;
        AudioClipRegistry                            Clips;
        MusicSetPlayer                               Player;
        Ref<const MusicSetAsset>                     Source;
        std::map<AssetID, Ref<const AudioClipAsset>> RegisteredClips;

        void Listen(bool listen)
        {
            Read = Size = 0;
            if (!listen)
                Player.SetEventCallback({});
            else
                Player.SetEventCallback([this](const MusicSetEvent& event) {
                    if (Size == Events.size())
                        return;
                    Events[(Read + Size++) % Events.size()] = event;
                });
        }
        std::array<MusicSetEvent, 256> Events;
        size_t                         Read = 0, Size = 0;
    };

    MusicSetPlayerComponent::MusicSetPlayerComponent()  = default;
    MusicSetPlayerComponent::~MusicSetPlayerComponent() = default;
    MusicSetPlayerComponent::MusicSetPlayerComponent(const MusicSetPlayerComponent& other) :
        MusicSetPlayerSettings(other)
    {}
    MusicSetPlayerComponent::MusicSetPlayerComponent(MusicSetPlayerComponent&&) noexcept            = default;
    MusicSetPlayerComponent& MusicSetPlayerComponent::operator=(MusicSetPlayerComponent&&) noexcept = default;
    MusicSetPlayerComponent& MusicSetPlayerComponent::operator=(const MusicSetPlayerComponent& other)
    {
        if (this != &other)
        {
            MusicSetPlayerSettings::operator=(other);
            m_Runtime.reset();
            m_PlayRequested = m_Listen = false;
        }
        return *this;
    }
    bool MusicSetPlayerComponent::operator==(const MusicSetPlayerComponent& other) const
    {
        return MusicSetPlayerSettings::operator==(other);
    }

    bool Scene::PrepareMusicSet(Entity entity, bool restart, bool reload, double fadeSeconds)
    {
        if (!m_AudioRunning || !m_AudioEngine || !m_PrefabAssets || !entity ||
            !entity.HasComponent<MusicSetPlayerComponent>())
            return false;
        auto& component = entity.GetComponent<MusicSetPlayerComponent>();
        if (!component.Asset.IsValid())
        {
            component.m_Runtime.reset();
            return false;
        }
        auto* current = component.m_Runtime.get();
        if (!reload && current && current->Settings.Asset == component.Asset &&
            current->Settings.BusOverride == component.BusOverride)
        {
            if (!current->Player.GetData())
                return false;
            if (current->Settings != static_cast<const MusicSetPlayerSettings&>(component))
            {
                const auto& parameters = current->Player.GetData()->Parameters;
                for (const auto& parameter : parameters)
                    if (current->Settings.Parameters.count(parameter.Name) &&
                        !component.Parameters.count(parameter.Name))
                        current->Player.SetParameter(parameter.Name, parameter.Default);
                for (const auto& [name, value] : component.Parameters)
                {
                    const auto old = current->Settings.Parameters.find(name);
                    if (old == current->Settings.Parameters.end() || old->second != value)
                        current->Player.SetParameter(name, value);
                }
                current->Settings = component;
            }
            return !restart || current->Player.Play(component.InitialState, fadeSeconds);
        }
        auto       next = std::make_unique<MusicSetPlayerComponent::Runtime>(*m_AudioEngine, component);
        const auto fail = [&](const AssetError& error) {
            WarnAudioOnce("Music set " + component.Asset.ToString() + " " + error.Message);
            if (!component.m_Runtime ||
                static_cast<const MusicSetPlayerSettings&>(component) != component.m_Runtime->Settings)
                component.m_Runtime = std::move(next);
            return false;
        };
        const auto asset = m_PrefabAssets->Load<MusicSetAsset>(component.Asset);
        if (const auto* error = std::get_if<AssetError>(&asset))
            return fail(*error);
        const auto resolve = [&](AssetID id) -> MusicSetClip {
            const auto audio = m_PrefabAssets->Load<AudioClipAsset>(id);
            if (const auto* clip = std::get_if<Ref<const AudioClipAsset>>(&audio))
                if (const auto sound = next->Clips.Acquire(*m_AudioEngine, *clip))
                {
                    const MusicSetClip result {*sound, (*clip)->GetData().Volume};
                    next->RegisteredClips.emplace(id, *clip);
                    return result;
                }
            return {};
        };
        next->Source = std::get<Ref<const MusicSetAsset>>(asset);
        if (auto error = next->Player.Load(next->Source, resolve, component.BusOverride, uint64_t(entity.GetUUID())))
            return fail(*error);
        for (const auto& [name, value] : component.Parameters)
            next->Player.SetParameter(name, value);
        next->Listen(component.m_Listen);
        const auto position = !restart && current ? current->Player.GetPosition() : std::nullopt;
        if (restart || component.m_PlayRequested || position)
        {
            const auto& states    = next->Player.GetData()->States;
            const auto  keepState = position && std::any_of(states.begin(), states.end(), [&](const auto& state) {
                                       return state.Name == position->State;
                                    });
            if (!next->Player.Play(keepState ? position->State : std::string_view(component.InitialState), fadeSeconds))
                return fail({AssetErrorCode::InvalidData, "Could not start its initial state"});
            if (keepState)
                if (const auto newPosition = next->Player.GetPosition();
                    newPosition && newPosition->Segment == position->Segment)
                    next->Player.SeekSeconds(position->Seconds);
        }
        component.m_Runtime = std::move(next);
        return true;
    }

    bool Scene::PlayMusicSet(Entity entity, double fadeSeconds)
    {
        if (!entity || entity.m_Scene != this || !entity.HasComponent<MusicSetPlayerComponent>())
            return false;
        auto& component           = entity.GetComponent<MusicSetPlayerComponent>();
        component.m_PlayRequested = true;
        return PrepareMusicSet(entity, true, false, fadeSeconds);
    }

    void Scene::StopMusicSet(Entity entity, double fadeSeconds)
    {
        if (!entity || entity.m_Scene != this || !entity.HasComponent<MusicSetPlayerComponent>())
            return;
        entity.GetComponent<MusicSetPlayerComponent>().m_PlayRequested = false;
        if (auto* player = GetMusicSetPlayer(entity))
            player->Stop(fadeSeconds);
    }

    MusicSetPlayer* Scene::GetMusicSetPlayer(Entity entity) const
    {
        if (!entity || entity.m_Scene != this || !entity.HasComponent<MusicSetPlayerComponent>())
            return nullptr;
        const auto& runtime = entity.GetComponent<MusicSetPlayerComponent>().m_Runtime;
        return runtime && runtime->Player.GetData() ? &runtime->Player : nullptr;
    }

    void Scene::ListenToMusicSet(Entity entity, bool listen)
    {
        if (!entity || entity.m_Scene != this || !entity.HasComponent<MusicSetPlayerComponent>())
            return;
        auto& component    = entity.GetComponent<MusicSetPlayerComponent>();
        component.m_Listen = listen;
        if (auto* runtime = component.m_Runtime.get())
            runtime->Listen(listen);
    }

    bool Scene::ReadMusicSetEvent(Entity entity, MusicSetEvent& event)
    {
        if (!entity || entity.m_Scene != this || !entity.HasComponent<MusicSetPlayerComponent>())
            return false;
        auto* runtime = entity.GetComponent<MusicSetPlayerComponent>().m_Runtime.get();
        if (!runtime || !runtime->Size)
            return false;
        event         = runtime->Events[runtime->Read];
        runtime->Read = (runtime->Read + 1) % runtime->Events.size();
        --runtime->Size;
        return true;
    }

    void Scene::UpdateMusicSets()
    {
        if (!m_AudioEngine || !m_AudioRunning)
            return;
        const auto changes = m_MusicSetChanges ? m_MusicSetChanges->Drain() : std::vector<AssetID>();
        for (auto handle : m_Registry.view<MusicSetPlayerComponent>())
        {
            auto& component = m_Registry.get<MusicSetPlayerComponent>(handle);
            auto* runtime   = component.m_Runtime.get();
            if (!runtime && !component.m_PlayRequested)
                component.m_PlayRequested = component.PlayOnStart;
            const auto reload = std::any_of(changes.begin(), changes.end(), [&](AssetID changed) {
                if (!m_PrefabAssets)
                    return false;
                if (changed == component.Asset)
                    return !runtime || !runtime->Source ||
                           m_PrefabAssets->GetLoaded<MusicSetAsset>(changed) != runtime->Source;
                if (runtime)
                    if (auto clip = runtime->RegisteredClips.find(changed); clip != runtime->RegisteredClips.end())
                        return m_PrefabAssets->GetLoaded<AudioClipAsset>(changed) != clip->second;
                return false;
            });
            if (reload || (runtime && static_cast<const MusicSetPlayerSettings&>(component) != runtime->Settings) ||
                (!runtime && component.Asset.IsValid()))
                PrepareMusicSet({handle, this}, false, reload);
            if (component.m_Runtime)
                component.m_Runtime->Player.Update();
        }
    }

    bool Scene::OwnsAudioSource(Entity entity) const
    {
        return entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle) &&
               m_Registry.all_of<AudioSourceComponent>(entity.m_EntityHandle);
    }
    void Scene::SetTimeScale(float scale)
    {
        if (std::isfinite(scale) && scale >= 0)
        {
            const bool changedHold = (m_TimeScale == 0) != (scale == 0);
            m_TimeScale = scale;
            if (m_AudioEngine && m_AudioRunning)
            {
                m_AudioEngine->SetTimeScale(scale);
                if (changedHold)
                    UpdateMusicSets();
            }
        }
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
        m_AudioEngine->SetTimeScale(m_TimeScale);
        m_AudioEngine->SetGamePaused(m_AudioPaused);
        if (m_PhysicsWorld)
        {
            m_AudioEngine->SetOcclusionQuery(
                {this,
                 [](void* context) {
                     if (auto* world = static_cast<Scene*>(context)->GetPhysicsWorld())
                         world->SynchronizeChanges();
                 },
                 [](void* context, const AudioOcclusionRay& ray, AudioOcclusionHit* output, size_t capacity) -> size_t {
                     auto* world = static_cast<Scene*>(context)->GetPhysicsWorld();
                     if (!world)
                         return 0;
                     QueryFilter filter;
                     filter.LayerMask    = ray.LayerMask;
                     filter.IgnoreEntity = ray.Emitter;
                     filter.Triggers     = QueryTriggerInteraction::Ignore;
                     std::array<BoundedRaycastHit, MaxAudioTransmissionHits> hits;
                     const auto                                              delta = ray.End - ray.Start;
                     const size_t                                            count =
                         world->RaycastBounded(ray.Start, delta, glm::length(delta), filter, hits.data(), capacity);
                     for (size_t hit = 0; hit < count; ++hit)
                         output[hit] = {hits[hit].Point, hits[hit].Acoustics.Transmission};
                     return count;
                 }});
        }
        m_AudioZones.reserve(m_Registry.view<AudioReverbZoneComponent>().size());
        UpdateAudioListeners(0);
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            if (m_Registry.get<AudioSourceComponent>(handle).PlayOnStart)
                PlayAudioSource({handle, this});
        UpdateMusicSets();
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
        for (auto handle : m_Registry.view<MusicSetPlayerComponent>())
        {
            auto& component = m_Registry.get<MusicSetPlayerComponent>(handle);
            component.m_Runtime.reset();
            component.m_PlayRequested = component.m_Listen = false;
        }
        m_EntityAudioEvents.clear();
        m_AudioClips.Clear();
        m_AudioZones.clear();
        if (m_AudioEngine && m_AudioRunning)
        {
            m_AudioEngine->ClearOcclusionQuery(this);
            m_AudioEngine->SetGamePaused(false);
            m_AudioEngine->SetTimeScale(1);
        }
        m_AudioRunning = m_AudioPaused                             = false;
        m_AudioCamera                                              = 0;
        m_HasCameraAudioPosition                                   = false;
    }
    void Scene::PlayAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return;
        auto& c = entity.GetComponent<AudioSourceComponent>();
        if (c.m_Runtime && !c.m_Runtime->MatchesSource(c))
            c.m_Runtime.reset();
        if (c.GetSourceKind() == AudioSourceKind::Event)
        {
            if (!c.m_Runtime)
                c.m_Runtime = std::make_unique<AudioSourceComponent::Runtime>();
            auto& r          = *c.m_Runtime;
            r.SetSource(c);
            r.Engine         = m_AudioEngine;
            r.PendingStart   = true;
            r.LastError.reset();
            PrepareAudioSource(entity);
            return;
        }
        if (c.m_Runtime && m_AudioEngine->IsVoiceValid(c.m_Runtime->Voice) &&
            !m_AudioEngine->HasEnded(c.m_Runtime->Voice))
        {
            c.m_Runtime->HasPosition = false;
            PushAudioSource(entity, 0);
            m_AudioEngine->Play(c.m_Runtime->Voice);
            return;
        }
        const auto seek = c.m_Runtime ? c.m_Runtime->PendingSeek : std::nullopt;
        c.m_Runtime     = std::make_unique<AudioSourceComponent::Runtime>();
        auto& r         = *c.m_Runtime;
        r.SetSource(c);
        r.Engine        = m_AudioEngine;
        r.Registry      = &m_AudioClips;
        r.PendingStart  = true;
        r.PendingSeek   = seek;
        PrepareAudioSource(entity);
    }
    void Scene::StopAudioSource(Entity entity)
    {
        if (m_AudioEngine && m_AudioRunning && OwnsAudioSource(entity))
            entity.GetComponent<AudioSourceComponent>().m_Runtime.reset();
    }

    AudioEventTriggerResult Scene::PlayEvent(AssetID event, const AudioEventParams& params)
    {
        if (!m_AudioEngine || !m_AudioRunning || !m_PrefabAssets)
            return {};
        return m_AudioEngine->GetEventPlayer().Trigger(*m_PrefabAssets, m_AudioClips, event, params);
    }

    AudioEventTriggerResult Scene::PlayEvent(Entity emitter, AssetID event, AudioEventParams params)
    {
        return PlayEventAtEntity(emitter, event, std::move(params), false);
    }

    AudioEventTriggerResult
    Scene::PlayEventAtEntity(Entity emitter, AssetID event, AudioEventParams params, bool followPoint)
    {
        if (emitter.m_Scene != this || !m_Registry.valid(emitter.m_EntityHandle))
            return {};
        params.OwnerEntity     = emitter.GetUUID();
        params.EntityTransform = GetWorldTransform(emitter);
        params.Position        = glm::vec3((*params.EntityTransform)[3]);
        if (emitter.HasComponent<AudioSourceComponent>())
        {
            const auto& source = emitter.GetComponent<AudioSourceComponent>();
            params.EntityShape = source.Shape;
            params.Occlusion   = source.Occlusion.Over(params.Occlusion);
            params.ReverbZoneSend &= source.ReverbZoneSend;
            params.PinToCenter |= source.PinToCenter;
            if (source.Spatializer)
                params.Spatializer = source.Spatializer;
            if (source.SpatialBlend)
                params.SpatialBlend = source.SpatialBlend;
            if (source.LfeSendDb != AudioSendOffDb && !params.LfeSendDb)
                params.LfeSendDb = source.LfeSendDb;
            if (source.AttenuationAsset.IsValid())
                params.AttenuationAsset = source.AttenuationAsset;
        }
        const auto result = PlayEvent(event, params);
        if (result.Voice.IsValid())
            m_EntityAudioEvents.push_back({result.Voice, emitter.m_EntityHandle, *params.Position, followPoint});
        return result;
    }

    AudioEventTriggerResult Scene::PlayEvent(glm::vec3 position, AssetID event, AudioEventParams params)
    {
        params.Position = position;
        return PlayEvent(event, params);
    }

    AudioVoiceHandle Scene::PlayOneShot(AssetID clip, const AudioOneShotParams& params)
    {
        if (!m_AudioEngine || !m_AudioRunning || !m_PrefabAssets || !clip.IsValid())
            return {};
        auto loaded = m_PrefabAssets->Request<AudioClipAsset>(clip);
        if (loaded.IsReady())
            return m_AudioEngine->PlayOneShot(m_AudioClips, loaded.Asset, params);
        if (loaded.State == AssetLoadState::Failed)
            m_AudioEngine->ProfileRejection(
                AudioVoiceReason::MissingClip, {AudioVoiceOwnerKind::OneShot, params.OwnerEntity, {}}, clip);
        return {};
    }

    AudioVoiceHandle Scene::PlayOneShot(Entity entity, AssetID clip, AudioOneShotParams params)
    {
        if (entity.m_Scene != this || !m_Registry.valid(entity.m_EntityHandle))
            return {};
        params.OwnerEntity = entity.GetUUID();
        if (OwnsAudioSource(entity))
            params.Occlusion = entity.GetComponent<AudioSourceComponent>().Occlusion.Over(params.Occlusion);
        return PlayOneShot(glm::vec3(GetWorldTransform(entity)[3]), clip, std::move(params));
    }

    AudioVoiceHandle Scene::PlayOneShot(glm::vec3 position, AssetID clip, AudioOneShotParams params)
    {
        params.Position = position;
        return PlayOneShot(clip, params);
    }
    AssetType Scene::GetAudioAssetType(AssetID id) const
    {
        if (m_PrefabAssets && id.IsValid())
            if (const auto registry = m_PrefabAssets->GetRegistry())
                if (const auto asset = registry->Find(id, m_PrefabAssets->GetTarget()))
                    return asset->Metadata.Type;
        return AssetType::None;
    }

    void Scene::WarnInvalidAudioAsset(const std::string& asset) { WarnAudioOnce("cannot play asset " + asset); }

    void Scene::WarnAudioOnce(const std::string& message)
    {
        if (m_WarnedAudioMessages.insert(message).second)
            Warn(message);
    }

    AudioEventTriggerResult Scene::PlayAudioAsset(AssetID asset, const AudioEventParams& params, Entity emitter)
    {
        if (!m_AudioEngine || !m_AudioRunning)
            return {};
        if (emitter && (emitter.m_Scene != this || !m_Registry.valid(emitter.m_EntityHandle)))
            return {};
        switch (GetAudioAssetType(asset))
        {
            case AssetType::AudioEvent: {
                const auto result =
                    emitter ? PlayEventAtEntity(emitter, asset, params, true) : PlayEvent(asset, params);
                if (result.Status == AudioEventTriggerStatus::Invalid ||
                    result.Status == AudioEventTriggerStatus::MissingClip)
                    WarnInvalidAudioAsset(asset.ToString());
                return result;
            }
            case AssetType::AudioClip: {
                AudioOneShotParams clipParams;
                clipParams.OwnerEntity = emitter ? uint64_t(emitter.GetUUID()) : 0;
                clipParams.Position =
                    emitter ? std::optional<glm::vec3>(glm::vec3(GetWorldTransform(emitter)[3])) : params.Position;
                clipParams.Volume = params.Volume;
                clipParams.Pitch  = params.Pitch;
                clipParams.Spatializer  = params.Spatializer;
                clipParams.SpatialBlend = params.SpatialBlend.value_or(1.f);
                clipParams.Occlusion    = params.Occlusion;
                if (emitter && OwnsAudioSource(emitter))
                {
                    const auto& source          = emitter.GetComponent<AudioSourceComponent>();
                    clipParams.Bus              = source.Bus;
                    clipParams.Spatial          = source.Spatial;
                    clipParams.Occlusion        = source.Occlusion.Over(clipParams.Occlusion);
                    clipParams.Priority         = source.Priority;
                    clipParams.ConcurrencyGroup = source.ConcurrencyGroup;
                    if (source.Spatializer)
                        clipParams.Spatializer = source.Spatializer;
                    if (source.SpatialBlend)
                        clipParams.SpatialBlend = *source.SpatialBlend;
                }
                if (params.Bus)
                    clipParams.Bus = *params.Bus;
                if (m_AudioEngine->IsBusPaused(clipParams.Bus))
                {
                    m_AudioEngine->ProfileRejection(
                        AudioVoiceReason::PausedBus, {AudioVoiceOwnerKind::OneShot, clipParams.OwnerEntity, {}}, asset);
                    return {{}, AudioEventTriggerStatus::Paused};
                }
                auto loaded = m_PrefabAssets->Load<AudioClipAsset>(asset);
                if (const auto* clip = std::get_if<Ref<const AudioClipAsset>>(&loaded))
                {
                    const auto voice = m_AudioEngine->PlayOneShot(m_AudioClips, *clip, clipParams);
                    if (voice.IsValid())
                    {
                        if (emitter)
                            m_EntityAudioEvents.push_back({voice, emitter.m_EntityHandle, *clipParams.Position, true});
                        return {voice, AudioEventTriggerStatus::Started};
                    }
                    return {};
                }
                m_AudioEngine->ProfileRejection(
                    AudioVoiceReason::MissingClip, {AudioVoiceOwnerKind::OneShot, clipParams.OwnerEntity, {}}, asset);
                WarnInvalidAudioAsset(asset.ToString());
                return {{}, AudioEventTriggerStatus::MissingClip};
            }
            default:
                WarnInvalidAudioAsset(asset.ToString());
                return {};
        }
    }

    void Scene::PauseAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !m_AudioRunning || !OwnsAudioSource(entity))
            return;
        auto& c = entity.GetComponent<AudioSourceComponent>();
        if (c.m_Runtime)
        {
            m_AudioEngine->Pause(c.m_Runtime->Voice);
            c.m_Runtime->PendingStart = false;
        }
    }
    bool Scene::UnPauseAudioSource(Entity entity)
    {
        return m_AudioEngine && m_AudioRunning && OwnsAudioSource(entity) &&
               m_AudioEngine->UnPause(entity.GetComponent<AudioSourceComponent>().GetVoiceHandle());
    }

    void Scene::SetAudioSourceTime(Entity entity, double seconds)
    {
        if (!OwnsAudioSource(entity) || !std::isfinite(seconds) || seconds < 0)
            return;
        auto& source = entity.GetComponent<AudioSourceComponent>();
        if (source.GetSourceKind() == AudioSourceKind::Generator)
            return;
        if (m_AudioEngine && m_AudioEngine->IsVoiceValid(source.GetVoiceHandle()))
            m_AudioEngine->SeekSeconds(source.GetVoiceHandle(), seconds);
        else
        {
            if (!source.m_Runtime)
                source.m_Runtime = std::make_unique<AudioSourceComponent::Runtime>();
            source.m_Runtime->SetSource(source);
            source.m_Runtime->PendingSeek    = seconds;
        }
    }

    double Scene::GetAudioSourceTime(Entity entity) const
    {
        if (!OwnsAudioSource(entity))
            return 0;
        const auto& source = entity.GetComponent<AudioSourceComponent>();
        if (source.m_Runtime && source.m_Runtime->PendingSeek)
            return *source.m_Runtime->PendingSeek;
        return m_AudioEngine ? m_AudioEngine->GetVoicePositionSeconds(source.GetVoiceHandle()) : 0;
    }

    void Scene::SyncAudioSource(Entity entity)
    {
        if (!m_AudioEngine || !OwnsAudioSource(entity))
            return;
        auto& source  = entity.GetComponent<AudioSourceComponent>();
        auto* runtime = source.m_Runtime.get();
        if (!runtime || !m_AudioEngine->IsVoiceValid(runtime->Voice))
            return;
        if (runtime->EventAsset)
        {
            const auto& event = runtime->EventAsset->GetData();
            m_AudioEngine->SetVoiceOcclusion(runtime->Voice, source.Occlusion.Over(event.Occlusion));
            m_AudioEngine->SetVoiceSpatializer(runtime->Voice,
                                               source.Spatializer ? source.Spatializer : event.Spatializer,
                                               source.SpatialBlend.value_or(event.SpatialBlend));
            m_AudioEngine->SetVolume(runtime->Voice, source.Volume * runtime->EventVolume);
            m_AudioEngine->SetPitch(runtime->Voice, source.Pitch * runtime->EventPitch);
            m_AudioEngine->SetLooping(runtime->Voice, source.Loop || event.Loop);
            m_AudioEngine->SetVoiceBus(runtime->Voice, source.Bus == MasterAudioBus ? event.Bus : source.Bus);
        }
        else
            PushAudioSource(entity, 0);
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
        const auto kind = c.GetSourceKind();
        if (!r.PendingStart || r.LastError || (kind == AudioSourceKind::Clip && m_AudioEngine->IsBusPaused(c.Bus)))
            return;
        auto fail = [&](AssetError error) {
            r.LastError    = std::move(error);
            r.PendingStart = false;
            if (kind != AudioSourceKind::Generator)
                WarnInvalidAudioAsset((kind == AudioSourceKind::Event ? c.Event : c.Clip).ToString());
        };
        if (kind == AudioSourceKind::Event)
        {
            // Event playback snapshots gain, pitch, routing and loop settings at Play.
            // Component volume and pitch multiply. Master and default priority inherit.
            // The component can disable spatial playback or enable looping. When spatial,
            // the component supplies its A1 attenuation and follows the entity transform.
            AudioEventParams params;
            params.OwnerEntity = entity.GetUUID();
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
            params.Spatializer              = c.Spatializer;
            params.SpatialBlend             = c.SpatialBlend;
            params.Occlusion                = c.Occlusion;
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
                    r.HasPosition = false;
                    r.EventAsset  = result.Asset;
                    r.EventVolume = result.ClipVolume * std::pow(10.f, result.VolumeDb / 20);
                    r.EventPitch  = std::exp2(result.PitchSemitones / 12);
                    if (r.PendingSeek)
                        m_AudioEngine->SeekSeconds(r.Voice, *std::exchange(r.PendingSeek, std::nullopt));
                    return;
                case AudioEventTriggerStatus::Paused:
                    r.LastError = AssetError {AssetErrorCode::InvalidArgument, "Audio event bus is paused"};
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
        if (kind == AudioSourceKind::Clip)
        {
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
        }
        AudioVoiceDesc desc;
        desc.Owner            = {AudioVoiceOwnerKind::Entity, entity.GetUUID(), {}};
        desc.Bus     = c.Bus;
        desc.Priority         = c.Priority;
        desc.ConcurrencyGroup = c.ConcurrencyGroup;
        desc.Volume  = c.Volume;
        desc.Pitch   = c.Pitch;
        desc.Pan     = c.Spatial ? 0 : c.Pan;
        desc.Loop    = c.Loop;
        desc.Spatial = c.Spatial;
        desc.Spatializer      = c.Spatializer;
        desc.SpatialBlend     = c.SpatialBlend.value_or(1.f);
        desc.Occlusion        = c.Occlusion;
        desc.ReverbZoneSend   = c.ReverbZoneSend;
        desc.LfeSendDb        = c.LfeSendDb;
        desc.PinToCenter      = c.PinToCenter;
        r.Spatial    = c.Spatial;
        if (kind == AudioSourceKind::Generator)
        {
            desc.Generator = c.Generator;
            r.Voice        = m_AudioEngine->CreateVoice(desc);
        }
        else
            r.Voice = m_AudioClips.CreateVoice(*m_AudioEngine, r.Source, desc);
        if (!m_AudioEngine->IsVoiceValid(r.Voice))
        {
            if (r.Acquired)
                m_AudioClips.Release(r.Source->assetID);
            r.Acquired = false;
            r.Source.reset();
            fail({AssetErrorCode::InvalidData, "Audio voice creation failed"});
            return;
        }
        r.PendingStart = false;
        if (r.PendingSeek)
            m_AudioEngine->SeekSeconds(r.Voice, *std::exchange(r.PendingSeek, std::nullopt));
        PushAudioSource(entity, 0);
        if (!m_AudioEngine->Play(r.Voice))
        {
            m_AudioEngine->DestroyVoice(r.Voice);
            r.Voice = {};
            if (r.Acquired)
                m_AudioClips.Release(r.Source->assetID);
            r.Acquired = false;
            r.Source.reset();
        }
    }
    void Scene::PushAudioSource(Entity entity, double seconds)
    {
        auto& c = entity.GetComponent<AudioSourceComponent>();
        auto& r = *c.m_Runtime;
        const auto* event = r.EventAsset ? &r.EventAsset->GetData() : nullptr;
        m_AudioEngine->SetVoiceOcclusion(r.Voice, event ? c.Occlusion.Over(event->Occlusion) : c.Occlusion);
        m_AudioEngine->SetVoiceSpatializer(r.Voice,
                                           c.Spatializer ? c.Spatializer : (event ? event->Spatializer : std::nullopt),
                                           c.SpatialBlend.value_or(event ? event->SpatialBlend : 1.f));
        if (!r.EventAsset)
        {
            m_AudioEngine->SetVoiceBus(r.Voice, c.Bus);
            m_AudioEngine->SetVoicePriority(r.Voice, c.Priority);
            m_AudioEngine->SetVolume(r.Voice, c.Volume * (r.Source ? r.Source->GetData().Volume : 1.f));
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
            if (c.m_Runtime && !c.m_Runtime->MatchesSource(c))
            {
                const bool pending = c.m_Runtime->PendingStart;
                c.m_Runtime.reset();
                if (pending)
                    PlayAudioSource({handle, this});
            }
        }
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
                m_AudioEngine->SetVoiceEmitter(instance.Voice, spatial, instance.FollowPoint);
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
                     (c.GetSourceKind() != AudioSourceKind::Clip && c.m_Runtime->Voice.IsValid() &&
                      !c.m_Runtime->LastError && !c.m_Runtime->PendingStart))
                c.m_Runtime.reset();
        }
    }
    void Scene::OnRuntimePause()
    {
        m_PhysicsPaused = true;
        if (!m_AudioRunning || m_AudioPaused)
            return;
        m_AudioPaused = true;
        if (m_AudioEngine)
        {
            m_AudioEngine->SetGamePaused(true);
            UpdateMusicSets();
            for (auto handle : m_Registry.view<TimelinePlayerComponent>())
                if (const auto& player = m_Registry.get<TimelinePlayerComponent>(handle).Player;
                    player && player->m_Audio)
                    player->m_Audio->SetGamePaused(true);
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
        m_AudioCamera = 0;
        for (auto handle : m_Registry.view<TimelinePlayerComponent>())
            if (const auto& player = m_Registry.get<TimelinePlayerComponent>(handle).Player; player && player->m_Audio)
                player->m_Audio->SetGamePaused(false);
        m_AudioEngine->SetGamePaused(false);
        UpdateMusicSets();
        UpdateAudio(0);
    }
} // namespace Kans
