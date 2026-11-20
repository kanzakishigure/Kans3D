#include "MaterialParameterCollectionPanel.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    void MaterialParameterCollectionPanel::Bind(EditorAssetImportService* service)
    {
        if (m_Service != service)
        {
            m_File.clear();
            m_Status.clear();
            m_Dirty = m_Focused = false;
            m_Service           = service;
        }
    }

    bool MaterialParameterCollectionPanel::Open(const std::filesystem::path& file)
    {
        if (file == m_File)
            return true;
        if (m_Dirty && !Save())
            return false;
        std::ifstream input(file, std::ios::binary);
        if (!input)
        {
            m_Status = "Cannot open collection";
            return false;
        }
        std::ostringstream text;
        text << input.rdbuf();
        auto parsed = ParseMaterialParameterCollection(text.str());
        if (const auto* error = std::get_if<AssetError>(&parsed))
        {
            m_Status = error->Message;
            return false;
        }
        m_File = file;
        m_Data = m_Saved = std::get<MaterialParameterCollection>(std::move(parsed));
        m_Dirty          = false;
        m_Status.clear();
        return true;
    }

    bool MaterialParameterCollectionPanel::Save()
    {
        if (m_File.empty() || (m_Service && m_Service->IsReadOnly(m_File)))
            return false;
        auto encoded = SerializeMaterialParameterCollection(m_Data);
        if (const auto* error = std::get_if<AssetError>(&encoded))
        {
            m_Status = error->Message;
            return false;
        }
        std::ofstream output(m_File, std::ios::binary | std::ios::trunc);
        output << std::get<std::string>(encoded) << '\n';
        output.close();
        if (!output)
        {
            m_Status = "Cannot save collection";
            return false;
        }
        m_Dirty  = false;
        m_Saved  = m_Data;
        m_Status = BuildSavedSource(m_Service, m_File, true);
        return true;
    }

    void MaterialParameterCollectionPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (!m_Status.empty())
            ImGui::TextColored(EditorTheme::Colors().TextDim, "%s", m_Status.c_str());
        if (m_File.empty())
            return;
        const bool readOnly = m_Service && m_Service->IsReadOnly(m_File);
        ImGui::TextUnformatted(m_File.filename().u8string().c_str());
        ImGui::BeginDisabled(readOnly);
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            m_Data  = m_Saved;
            m_Dirty = false;
            m_Status.clear();
        }
        ImGui::EndDisabled();
        if (PropertyGrid::BeginCard("Collection", ICON_FA_SLIDERS, "Collection"))
        {
            if (PropertyGrid::BeginGroup("Settings"))
            {
                m_Dirty |= PropertyGrid::InputText("Name", m_Data.Name);
                PropertyGrid::EndGroup();
            }
            ImGui::BeginDisabled(m_Data.Parameters.size() >= MaterialParameterLimit);
            if (ImGui::Button("Add parameter"))
            {
                std::string name = "Parameter";
                for (unsigned i = 1; std::any_of(m_Data.Parameters.begin(),
                                                 m_Data.Parameters.end(),
                                                 [&](const auto& p) { return p.Name == name; });
                     ++i)
                    name = "Parameter" + std::to_string(i);
                m_Data.Parameters.push_back({std::move(name)});
                m_Dirty = true;
            }
            ImGui::EndDisabled();
            size_t removed = m_Data.Parameters.size();
            for (size_t i = 0; i < m_Data.Parameters.size(); ++i)
            {
                ImGui::PushID(int(i));
                auto& p = m_Data.Parameters[i];
                if (PropertyGrid::BeginGroup((p.Name + "###parameter").c_str()))
                {
                    m_Dirty |= PropertyGrid::InputText("Name", p.Name);
                    const char* types[] = {"Scalar", "Vec4"};
                    int         type    = int(p.Type);
                    if (PropertyGrid::Combo("Type", types, 2, type))
                    {
                        p.Type = MaterialParameterType(type);
                        if (p.Type == MaterialParameterType::Scalar)
                            p.Default = glm::vec4(p.Default.x, 0, 0, 0);
                        m_Dirty = true;
                    }
                    if (p.Type == MaterialParameterType::Scalar)
                        m_Dirty |= PropertyGrid::Float("Default", p.Default.x);
                    else if (PropertyGrid::Row("Default"))
                        m_Dirty |= ImGui::DragFloat4("##default", &p.Default.x, 0.1f);
                    bool range = p.Range.has_value();
                    if (PropertyGrid::Checkbox("Range hint", range))
                    {
                        p.Range = range ? std::optional<glm::vec2>(glm::vec2(0, 1)) : std::nullopt;
                        m_Dirty = true;
                    }
                    if (p.Range)
                    {
                        m_Dirty |= PropertyGrid::Float("Minimum", p.Range->x);
                        m_Dirty |= PropertyGrid::Float("Maximum", p.Range->y);
                    }
                    if (PropertyGrid::Row("Parameter") && ImGui::SmallButton("Remove"))
                        removed = i;
                    PropertyGrid::EndGroup();
                }
                ImGui::PopID();
            }
            if (removed < m_Data.Parameters.size())
            {
                m_Data.Parameters.erase(m_Data.Parameters.begin() + removed);
                m_Dirty = true;
            }
        }
        PropertyGrid::EndCard();
        ImGui::EndDisabled();
        if (!readOnly && m_Focused && m_Dirty && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
    }
} // namespace Kans
