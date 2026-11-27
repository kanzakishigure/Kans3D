#include "kspch.h"
#include "Kans3D/Audio/AudioSystem.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Project/Project.h"

namespace Kans
{
    namespace
    {
        std::unique_ptr<AudioEngine> s_Engine;

        void LogLayoutError(const std::string& message)
        {
            if (Log::GetCoreLogger())
                CORE_ERROR("Audio bus layout: {}", message);
            else
                std::fprintf(stderr, "Audio bus layout: %s\n", message.c_str());
        }

        void SelectProjectHRTF(AudioEngine& engine, AssetManager& assets, AssetID id)
        {
            if (!engine.IsHRTFAvailable())
                return;
            if (auto error = engine.SetActiveHRTF(&assets, id))
            {
                if (Log::GetCoreLogger())
                    CORE_WARN("Audio HRTF {}: {}", id.ToString(), *error);
                else
                    std::fprintf(stderr, "Audio HRTF %s: %s\n", id.ToString().c_str(), error->c_str());
                (void)engine.SetActiveHRTF(nullptr, {});
            }
        }

        void LoadBusLayout(AudioEngine& engine, AssetManager& assets, AssetID id)
        {
            if (id.IsValid())
            {
                auto loaded = assets.Load<AudioBusLayoutAsset>(id);
                if (auto* error = std::get_if<AssetError>(&loaded))
                    LogLayoutError(id.ToString() + ": " + error->Message);
                else if (auto error =
                             engine.ApplyBusLayout(std::get<Ref<const AudioBusLayoutAsset>>(loaded)->GetData()))
                    LogLayoutError(*error);
                else
                    return;
            }
            if (auto error = engine.ApplyBusLayout(AudioBusLayout::Default()))
                LogLayoutError(*error);
        }
    }

    std::optional<std::string> AudioSystem::Init(const AudioEngineConfig& config)
    {
        Shutdown();
        auto engine = std::make_unique<AudioEngine>();
        if (auto error = engine->Init(config))
            return error;
        s_Engine = std::move(engine);
        return std::nullopt;
    }

    void AudioSystem::Shutdown() { s_Engine.reset(); }
    AudioEngine* AudioSystem::Get() { return s_Engine.get(); }

    void AudioSystem::LoadProjectBusLayout(const ProjectConfig& config, AssetManager& assets)
    {
        if (!s_Engine)
            return;
        SelectProjectHRTF(*s_Engine, assets, config.AudioHRTF);
        s_Engine->SetDefaultSpatializer(config.DefaultAudioSpatializer);
        s_Engine->SetOcclusionDefaults(
            config.DefaultAudioOcclusion, config.MaxOcclusionRaysPerFrame, config.OcclusionStaleFrames);
        LoadBusLayout(*s_Engine, assets, config.AudioBusLayout);
    }

    void AudioSystem::ReloadProjectBusLayout(const ProjectConfig&    config,
                                             AssetManager&           assets,
                                             const ProductChangeSet& changes)
    {
        if (!s_Engine)
            return;
        const auto changed = [&](AssetID id) {
            const auto contains = [id](const std::vector<AssetID>& ids) {
                return std::find(ids.begin(), ids.end(), id) != ids.end();
            };
            return id.IsValid() && (contains(changes.Added) || contains(changes.Modified) || contains(changes.Removed));
        };
        if (changed(config.AudioBusLayout))
            LoadBusLayout(*s_Engine, assets, config.AudioBusLayout);
        const auto active   = s_Engine->GetActiveHRTF();
        const auto selected = active.IsValid() ? active : config.AudioHRTF;
        if (changed(selected))
            SelectProjectHRTF(*s_Engine, assets, selected);
    }
}
