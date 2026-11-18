#include "PluginSettings.h"
#include "Kans3D/Project/ProjectFile.h"
namespace Kans
{
    std::optional<AssetError> PluginSettings::Load(std::filesystem::path file)
    {
        m_File.clear();
        m_Resolution = {};
        if (file.empty())
            return {};
        auto loaded = LoadProjectFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        m_Overrides  = std::get<ProjectFile>(loaded).Plugins;
        m_File       = file;
        m_Resolution = DiscoverPlugins(EnginePluginDirectory(), file.parent_path() / "Plugins", m_Overrides);
        return {};
    }
    bool PluginSettings::Requested(const std::string& name) const
    {
        const auto override = m_Overrides.find(name);
        const auto found    = m_Resolution.Plugins.find(name);
        return override != m_Overrides.end() ?
                   override->second :
                   found != m_Resolution.Plugins.end() && found->second.Manifest.EnabledByDefault;
    }
    std::optional<AssetError> PluginSettings::SetEnabled(const std::string& name, bool enabled)
    {
        if (m_File.empty() || !m_Resolution.Plugins.count(name))
            return AssetError {AssetErrorCode::NotFound, "Unknown plugin " + name};
        // Re-read to preserve settings edited by other panels since this window opened.
        auto loaded = LoadProjectFile(m_File);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        auto project          = std::get<ProjectFile>(std::move(loaded));
        project.Plugins[name] = enabled;
        if (auto error = SaveProjectFile(m_File, project))
            return error;
        return Load(m_File);
    }
} // namespace Kans
