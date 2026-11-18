#include "PluginsPanel.h"
#include <imgui.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/PropertyGrid.h"
namespace Kans
{
    void PluginsPanel::Bind(const std::filesystem::path& file)
    {
        m_Status.clear();
        if (auto error = m_Settings.Load(file))
            m_Status = error->Message;
    }
    void PluginsPanel::Draw()
    {
        if (!m_Status.empty())
            ImGui::TextWrapped("%s", m_Status.c_str());
        std::optional<std::pair<std::string, bool>> toggle;
        for (const auto& diagnostic : m_Settings.Get().Diagnostics)
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", diagnostic.c_str());
        if (m_Settings.Get().Plugins.empty())
            ImGui::TextDisabled("No plugins discovered.");
        for (const auto& [name, plugin] : m_Settings.Get().Plugins)
        {
            const auto& manifest = plugin.Manifest;
            const auto  title    = manifest.FriendlyName.empty() ? name : manifest.FriendlyName;
            if (PropertyGrid::BeginCard(name.c_str(), "", title.c_str()))
            {
                if (PropertyGrid::BeginGroup("Details"))
                {
                    bool enabled = m_Settings.Requested(name);
                    if (PropertyGrid::Checkbox("Enabled", enabled))
                        toggle = {{name, enabled}};
                    PropertyGrid::Text("Name", name);
                    PropertyGrid::Text("Version", manifest.Version);
                    PropertyGrid::Text("Description", manifest.Description);
                    PropertyGrid::Text("Source", plugin.ProjectOwned ? "Project" : "Engine (read-only content)");
                    PropertyGrid::Text("Type", manifest.Type == PluginType::Editor ? "Editor" : "Runtime");
                    PropertyGrid::Text("Resolved", plugin.Enabled ? "Enabled" : "Disabled");
                    for (const auto& [dependency, range] : manifest.Dependencies)
                        PropertyGrid::Text(dependency.c_str(), range);
                    PropertyGrid::BeginFree();
                    for (const auto& error : plugin.Errors)
                        ImGui::TextColored(EditorTheme::Colors().Danger, "%s", error.c_str());
                    PropertyGrid::EndFree();
                    PropertyGrid::EndGroup();
                }
            }
            PropertyGrid::EndCard();
        }
        if (toggle)
        {
            if (auto error = m_Settings.SetEnabled(toggle->first, toggle->second))
                m_Status = error->Message;
            else
                m_Status = "Saved. Restart or rebuild for C++ modules. Reload the project to apply shader and content "
                           "changes.";
        }
    }
} // namespace Kans
