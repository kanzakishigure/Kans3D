#include "PhysicsMaterialPanel.h"
#include <imgui.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Physics/PhysicsEquality.h"
#include "Components/ComponentEditors.h"
#include "KansEditorCore/Physics/PhysicsEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "KansEditorCore/UI/SourceBuild.h"

namespace Kans
{
    bool PhysicsMaterialPanel::Open(const std::filesystem::path& file)
    {
        auto loaded = PhysicsEditing::LoadPhysicsMaterialFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            m_Status = error->Message;
            return false;
        }
        m_File   = file;
        m_Values = m_Saved = std::get<PhysicsMaterialValues>(loaded);
        m_Dirty            = false;
        m_Status.clear();
        return true;
    }

    void PhysicsMaterialPanel::Save()
    {
        if (m_File.empty())
            return;
        if (auto error = PhysicsEditing::SavePhysicsMaterialFile(m_File, m_Values))
        {
            m_Status = error->Message;
            return;
        }
        m_Saved  = m_Values;
        m_Dirty  = false;
        m_Status = BuildSavedSource(m_Service, m_File);
    }

    void PhysicsMaterialPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (m_File.empty())
        {
            ImGui::TextDisabled("%s",
                                m_Status.empty() ? "Open a .physmat file from the Content Browser." : m_Status.c_str());
            return;
        }
        ImGui::TextUnformatted(ICON_FA_ATOM);
        ImGui::SameLine();
        ImGui::TextUnformatted((m_File.filename().u8string() + (m_Dirty ? "  *" : "")).c_str());
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
            Save();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Revert"))
        {
            m_Values = m_Saved;
            m_Dirty  = false;
        }
        ImGui::EndDisabled();
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
        if (PropertyGrid::BeginGroup("Surface"))
        {
            if (ComponentEditors::PhysicsMaterialRows(m_Values))
                m_Dirty = !(m_Values == m_Saved);
            PropertyGrid::EndGroup();
        }
        ImGui::TextDisabled("Combine: how two touching surfaces' values mix (the higher mode of the pair wins).");
        if (m_Dirty && m_Focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
    }
} // namespace Kans
