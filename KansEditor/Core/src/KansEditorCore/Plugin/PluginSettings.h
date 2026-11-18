#pragma once
#include "Kans3D/Plugin/PluginManifest.h"
namespace Kans
{
    class PluginSettings
    {
    public:
        std::optional<AssetError> Load(std::filesystem::path file);
        std::optional<AssetError> SetEnabled(const std::string& name, bool enabled);
        const PluginResolution&   Get() const { return m_Resolution; }
        bool                      Requested(const std::string& name) const;

    private:
        std::filesystem::path       m_File;
        std::map<std::string, bool> m_Overrides;
        PluginResolution            m_Resolution;
    };
} // namespace Kans
