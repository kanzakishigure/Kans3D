#pragma once

#include "Kans3D/Audio/AudioEngine.h"

namespace Kans
{
    class AssetManager;
    class ProjectConfig;
    struct ProductChangeSet;

    class AudioSystem
    {
    public:
        [[nodiscard]] static std::optional<std::string> Init(const AudioEngineConfig& config);
        static void Shutdown();
        static AudioEngine* Get();
        static void LoadProjectBusLayout(const ProjectConfig& config, AssetManager& assets);
        static void
        ReloadProjectBusLayout(const ProjectConfig& config, AssetManager& assets, const ProductChangeSet& changes);
    };
}
