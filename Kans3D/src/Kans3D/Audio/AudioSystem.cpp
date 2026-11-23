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
        if (config.AudioBusLayout.IsValid())
        {
            auto loaded = assets.Load<AudioBusLayoutAsset>(config.AudioBusLayout);
            if (auto* error = std::get_if<AssetError>(&loaded))
                LogLayoutError(config.AudioBusLayout.ToString() + ": " + error->Message);
            else
            {
                const auto& asset = std::get<Ref<const AudioBusLayoutAsset>>(loaded);
                if (auto error = s_Engine->ApplyBusLayout(asset->GetData()))
                    LogLayoutError(*error);
                else
                    return;
            }
        }
        // A project without a layout reference uses the default layout; that is not an error.
        if (auto error = s_Engine->ApplyBusLayout(AudioBusLayout::Default()))
            LogLayoutError(*error);
    }

    void AudioSystem::ReloadProjectBusLayout(const ProjectConfig&    config,
                                             AssetManager&           assets,
                                             const ProductChangeSet& changes)
    {
        const auto contains = [&](const std::vector<AssetID>& ids) {
            return std::find(ids.begin(), ids.end(), config.AudioBusLayout) != ids.end();
        };
        if (contains(changes.Added) || contains(changes.Modified) || contains(changes.Removed))
            LoadProjectBusLayout(config, assets);
    }
}
