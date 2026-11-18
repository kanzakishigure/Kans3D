#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    inline constexpr const char* PluginEngineVersion = "1.0.0";
    enum class PluginType
    {
        Runtime,
        Editor
    };
    struct PluginManifest
    {
        std::string                        Name, FriendlyName, Version, Description, EngineVersion;
        std::map<std::string, std::string> Dependencies;
        PluginType                         Type             = PluginType::Runtime;
        bool                               EnabledByDefault = false;
        std::vector<std::string>           Modules;
        std::filesystem::path              Shaders = "Shaders", Content = "Content";
    };
    // Ranges: *, exact semver, whitespace-separated comparisons, ^, ~ and ||.
    bool                                     IsPluginVersion(const std::string& version);
    bool                                     IsPluginVersionRange(const std::string& range);
    bool                                     PluginVersionMatches(const std::string& version, const std::string& range);
    bool                                     IsPluginName(const std::string& name);
    std::variant<PluginManifest, AssetError> LoadPluginManifest(const std::filesystem::path& file,
                                                                const std::string& engineVersion = PluginEngineVersion);

    struct DiscoveredPlugin
    {
        PluginManifest           Manifest;
        std::filesystem::path    Directory;
        bool                     ProjectOwned = false, Enabled = false;
        std::vector<std::string> Errors;
    };
    struct PluginResolution
    {
        // Name order, including invalid/disabled plugins for the editor.
        std::map<std::string, DiscoveredPlugin> Plugins;
        std::vector<std::string>                Order, Diagnostics;
    };
    std::filesystem::path EnginePluginDirectory();
    PluginResolution      DiscoverPlugins(const std::filesystem::path&       engineRoot,
                                          const std::filesystem::path&       projectRoot,
                                          const std::map<std::string, bool>& overrides     = {},
                                          const std::string&                 engineVersion = PluginEngineVersion);
    void                  ResolvePlugins(PluginResolution& plugins, const std::map<std::string, bool>& overrides = {});
} // namespace Kans
