#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "PluginManifest.h"
namespace Kans
{
    std::filesystem::path EnginePluginDirectory()
    {
        const auto installed = KansFileSystem::GetExecutableDirectory() / "Plugins";
        return std::filesystem::is_directory(installed) ? installed : std::filesystem::u8path(KANS_ENGINE_PLUGIN_DIR);
    }
} // namespace Kans
