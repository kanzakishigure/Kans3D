#include "kspch.h"
#include <atomic>
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Entity.h"
#include "VideoDecoder.h"

namespace Kans
{
    namespace
    {
        std::atomic<uint64_t> NextVideoIdentity {1};
    }

    void Scene::OnVideoPlayerRemoved(entt::registry& registry, entt::entity entity)
    {
        if (const auto* id = registry.try_get<IDComponent>(entity))
            m_VideoPlayers.erase(id->ID);
    }

    bool Scene::PlayVideo(Entity entity)
    {
        if (!m_PhysicsWorld || !entity || entity.m_Scene != this || !entity.HasComponent<VideoPlayerComponent>())
            return false;
        auto& playback = m_VideoPlayers[entity.GetUUID()];
        if (!playback.Owner)
            playback.Owner = NextVideoIdentity++;
        playback.Playing = true;
        return true;
    }

    void Scene::PauseVideo(Entity entity)
    {
        if (entity && entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle))
            if (const auto found = m_VideoPlayers.find(entity.GetUUID()); found != m_VideoPlayers.end())
                found->second.Playing = false;
    }

    void Scene::StopVideo(Entity entity)
    {
        if (entity && entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle))
            m_VideoPlayers.erase(entity.GetUUID());
    }

    void Scene::SeekVideo(Entity entity, double seconds)
    {
        if (!entity || entity.m_Scene != this || !m_Registry.valid(entity.m_EntityHandle) || !std::isfinite(seconds))
            return;
        const auto found = m_VideoPlayers.find(entity.GetUUID());
        if (found == m_VideoPlayers.end() || !entity.HasComponent<VideoPlayerComponent>() ||
            entity.GetComponent<VideoPlayerComponent>().ClockSource != VideoClockSource::Game)
            return;
        found->second.GameTime = seconds;
        found->second.Seek     = true;
    }

    bool Scene::PrepareVideo(Entity entity, VideoPlayback& playback)
    {
        const auto& component = entity.GetComponent<VideoPlayerComponent>();
        auto*       assets    = GetAssetManager();
        if (playback.RequestedClip != component.Clip)
        {
            playback.RequestedClip = component.Clip;
            playback.Decoder.reset();
            playback.Clip.reset();
            playback.Frame = nullptr;
            playback.Error.clear();
        }
        if (!IsVideoDecodingAvailable())
        {
            if (playback.Error.empty())
                playback.Error = VideoDecodingUnavailable;
            return false;
        }
        if (!assets || !component.Clip.IsValid())
            return false;
        const auto status = assets->Request<VideoClipAsset>(component.Clip);
        if (status.State == AssetLoadState::Failed)
        {
            if (status.Error && playback.Error != status.Error->Message)
                playback.Error = status.Error->Message;
            return false;
        }
        if (status.Asset && status.Asset != playback.Clip)
        {
            playback.Clip    = status.Asset;
            playback.Decoder = CreateRef<VideoDecoder>(status.Asset);
            playback.Stream  = NextVideoIdentity++;
            playback.HasTime = false;
            playback.Seek    = false;
            playback.Frame   = nullptr;
            playback.Error.clear();
        }
        return bool(playback.Decoder);
    }

    bool Scene::PreviewVideo(Entity entity, double seconds)
    {
        if (m_PhysicsWorld || !entity || entity.m_Scene != this || !entity.HasComponent<VideoPlayerComponent>() ||
            !std::isfinite(seconds))
            return false;
        auto& playback   = m_VideoPlayers[entity.GetUUID()];
        playback.Preview = true;
        if (!playback.Owner)
            playback.Owner = NextVideoIdentity++;
        if (!PrepareVideo(entity, playback))
            return false;
        const auto time = std::clamp(seconds, 0.0, playback.Clip->GetData().Info.Duration);
        playback.Seek |= !playback.HasTime || playback.Time != time;
        playback.HasTime = true;
        playback.Time    = time;
        return true;
    }

    void Scene::EndVideoPreview(Entity entity)
    {
        if (!m_PhysicsWorld)
            StopVideo(entity);
    }

    void Scene::UpdateVideoPlayers(double seconds)
    {
        for (auto it = m_VideoPlayers.begin(); it != m_VideoPlayers.end();)
        {
            auto entity = GetEntityByUUID(it->first);
            if (!entity || !entity.HasComponent<VideoPlayerComponent>())
            {
                it = m_VideoPlayers.erase(it);
                continue;
            }
            auto& playback = (it++)->second;
            if (!IsEntityActive(entity))
                continue;
            const auto& component = entity.GetComponent<VideoPlayerComponent>();
            if (playback.Playing && component.ClockSource == VideoClockSource::Game)
                if (const auto next = playback.GameTime + seconds * component.Speed; std::isfinite(next))
                    playback.GameTime = next;
            if (!PrepareVideo(entity, playback))
                continue;
            if (!playback.Playing && !playback.Seek && playback.HasTime)
            {
                playback.Frame = playback.Decoder->Sample(playback.Time);
                continue;
            }

            std::optional<double> clock = playback.GameTime;
            if (component.ClockSource != VideoClockSource::Game)
            {
                clock.reset();
                const auto source = component.ClockEntity == UUID(0) ? entity : GetEntityByUUID(component.ClockEntity);
                if (source && component.ClockSource == VideoClockSource::Timeline &&
                    source.HasComponent<TimelinePlayerComponent>())
                {
                    if (const auto& player = source.GetComponent<TimelinePlayerComponent>().Player)
                        clock = player->Time();
                }
                if (source && component.ClockSource == VideoClockSource::AudioSource && m_AudioEngine &&
                    source.HasComponent<AudioSourceComponent>())
                {
                    const auto voice = source.GetComponent<AudioSourceComponent>().GetVoiceHandle();
                    if (m_AudioEngine->IsVoiceValid(voice))
                        clock = m_AudioEngine->GetVoicePositionSeconds(voice);
                }
            }
            if (!clock || !std::isfinite(*clock))
                continue;
            const auto mapped = MapVideoTime(component, *clock, playback.Clip->GetData().Info.Duration);
            const auto jump   = mapped.Seconds - playback.Time;
            const bool seek   = playback.Seek || (playback.HasTime && (jump < -1e-8 || jump > .5));
            playback.HasTime  = true;
            playback.Time     = mapped.Seconds;
            playback.Clear    = mapped.Ended && component.EndBehavior == VideoEndBehavior::Clear;
            playback.Frame    = playback.Decoder->Sample(playback.Time, seek);
            playback.Seek     = false;
        }
    }

    void Scene::RenderVideoTextures(RenderAssetCache& cache)
    {
        cache.BeginVideoTextures();
        for (auto& [id, playback] : m_VideoPlayers)
        {
            const auto entity = GetEntityByUUID(id);
            if (!entity || !entity.HasComponent<VideoPlayerComponent>() || !IsEntityActive(entity))
                continue;
            if (playback.Preview && playback.Decoder && playback.HasTime)
            {
                playback.Frame = playback.Decoder->Sample(playback.Time, playback.Seek);
                playback.Seek  = false;
            }
            const auto& component = entity.GetComponent<VideoPlayerComponent>();
            if (component.Target.IsValid())
                cache.PresentVideoTexture(component.Target,
                                          playback.Owner,
                                          playback.Stream,
                                          playback.Clear,
                                          playback.Clip ? &playback.Clip->GetData().Info : nullptr,
                                          playback.Frame,
                                          playback.UploadMilliseconds);
        }
        cache.EndVideoTextures();
    }

    namespace
    {
        template<typename Playback>
        void AddVideoStats(VideoPlayerStats& result, const Playback& playback)
        {
            if (!playback.Decoder)
                return;
            const auto stats = playback.Decoder->Stats();
            ++result.ActivePlayers;
            result.RingFill += stats.RingFill;
            result.RingCapacity += VideoDecoder::RingSize;
            result.DecodedFPS += stats.DecodedFPS;
            result.DroppedFrames += stats.Dropped;
            result.UploadMilliseconds += playback.UploadMilliseconds;
        }
    } // namespace

    VideoPlayerStats Scene::GetVideoPlayerStats() const
    {
        VideoPlayerStats result;
        for (const auto& [id, playback] : m_VideoPlayers)
            AddVideoStats(result, playback);
        return result;
    }

    VideoPlayerStats Scene::GetVideoPlayerStats(Entity entity) const
    {
        VideoPlayerStats result;
        if (entity && entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle))
            if (const auto found = m_VideoPlayers.find(entity.GetUUID()); found != m_VideoPlayers.end())
                AddVideoStats(result, found->second);
        return result;
    }

    std::optional<double> Scene::GetVideoFrameTime(Entity entity) const
    {
        if (entity && entity.m_Scene == this && m_Registry.valid(entity.m_EntityHandle))
            if (const auto found = m_VideoPlayers.find(entity.GetUUID());
                found != m_VideoPlayers.end() && found->second.Frame && !found->second.Clear)
                return found->second.Frame->Time;
        return {};
    }

    std::string Scene::GetVideoPlayerError(Entity entity) const
    {
        if (!entity || entity.m_Scene != this || !m_Registry.valid(entity.m_EntityHandle))
            return {};
        const auto found = m_VideoPlayers.find(entity.GetUUID());
        if (found == m_VideoPlayers.end())
            return {};
        const auto& playback = found->second;
        if (playback.Decoder)
            if (auto error = playback.Decoder->Error(); !error.empty())
                return error;
        return playback.Error;
    }

    void RenderWorld::PrepareVideoTextures(RenderAssetCache& cache)
    {
        if (m_Scene)
            m_Scene->RenderVideoTextures(cache);
    }
} // namespace Kans
