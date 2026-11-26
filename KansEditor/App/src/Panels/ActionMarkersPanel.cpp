#include "ActionMarkersPanel.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace ActionMarkerEditing;
    using EditorTheme::Px;

    namespace
    {
        constexpr char AssetPayload[] = "asset_payload"; // A source file dragged from the Content Browser.

        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path : result;
        }

        bool ToolButton(const char* label, const char* tooltip, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Button(label);
            ImGui::EndDisabled();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        std::string Stem(const EditorAssetChoice& choice)
        {
            return std::filesystem::u8path(choice.Name).stem().u8string();
        }
    } // namespace

    bool ActionMarkersPanel::Open(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            m_Status = "Cannot read " + file.filename().u8string();
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        auto              parsed = ParseAction(text);
        if (const auto* error = std::get_if<std::string>(&parsed))
        {
            m_Status = file.filename().u8string() + ": " + *error;
            return false;
        }
        // Switching files keeps valid edits instead of dropping them.
        if (m_Opened && !m_File.empty() && Canonical(m_File) != Canonical(file) && m_Document.IsDirty() &&
            !ValidationError(m_Document.Get()))
            Save();
        m_Opened = true;
        m_File   = file;
        m_Pending.reset();
        m_Document.Reset(std::get<ActionData>(std::move(parsed)));
        m_Status.clear();
        return true;
    }

    bool ActionMarkersPanel::Save()
    {
        if (!m_Opened || m_File.empty())
            return false;
        if (auto error = ValidationError(m_Document.Get()))
        {
            m_Status = "Not saved: " + *error;
            return false;
        }
        if (m_Service && m_Service->IsReadOnly(m_File))
        {
            m_Status = "Plugin content is read-only.";
            return false;
        }
        const auto    text = ToText(m_Document.Get());
        std::ofstream stream(m_File, std::ios::binary | std::ios::trunc);
        if (text.empty() || !(stream << text) || !stream.flush())
        {
            m_Status = "Cannot write " + m_File.u8string();
            return false;
        }
        stream.close();
        m_Document.MarkSaved();
        m_Status = BuildSavedSource(m_Service, m_File);
        return true;
    }

    void ActionMarkersPanel::Edit(const std::function<void(ActionData&)>& change)
    {
        auto next = m_Pending ? *m_Pending : m_Document.Get();
        change(next);
        m_PendingContinuous = (m_Pending ? m_PendingContinuous : true) && ImGui::IsAnyItemActive();
        m_Pending           = std::move(next);
    }

    void ActionMarkersPanel::CommitPending()
    {
        if (!m_Pending)
            return;
        auto next = std::move(*m_Pending);
        m_Pending.reset();
        m_Document.Commit(std::move(next), m_PendingContinuous);
    }

    void ActionMarkersPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_Boxes   = {};
        if (!m_Opened)
        {
            ImGui::TextDisabled("%s",
                                m_Status.empty() ? "Open a .action file from the Content Browser." : m_Status.c_str());
            return;
        }
        const auto& io = ImGui::GetIO();
        if (m_Focused && io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false) && m_Document.IsDirty())
                Save();
        }
        DrawHeader();
        const auto& action = m_Document.Get();
        m_Boxes.Markers.resize(action.Events.size());
        if (action.Events.empty())
            ImGui::TextDisabled("No markers. Add one: a marker named Audio plays a clip or an event at the entity.");
        for (size_t i = 0; i < action.Events.size(); ++i)
            DrawMarker(i, m_Boxes.Markers[i]);
        CommitPending();
        if (!ImGui::IsAnyItemActive())
            m_Document.EndGesture();
    }

    void ActionMarkersPanel::DrawHeader()
    {
        const auto& p    = EditorTheme::Colors();
        std::string name = m_File.filename().u8string();
        if (m_Document.IsDirty())
            name += " *";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.TypeAction, ICON_FA_FLAG);
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_File.u8string().c_str());
        const auto error = ValidationError(m_Document.Get());
        ImGui::SameLine();
        if (ToolButton(ICON_FA_FLOPPY_DISK " Save",
                       error ? "Fix the problem below to save" : "Write the file and rebuild it (Ctrl+S)",
                       m_Document.IsDirty() && !error))
            Save();
        m_Boxes.Save = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_ROTATE_LEFT "##undo", "Undo (Ctrl+Z)", m_Document.CanUndo()))
            Undo();
        m_Boxes.Undo = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_ROTATE_RIGHT "##redo", "Redo (Ctrl+Y)", m_Document.CanRedo()))
            Redo();
        m_Boxes.Redo = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_PLUS " Marker", "Add a marker at the last one's time (or 0)"))
        {
            const auto& events = m_Document.Get().Events;
            const auto  last   = std::max_element(
                events.begin(), events.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
            const double time = last == events.end() ? 0.0 : last->Time;
            Edit([time](ActionData& next) { AddMarker(next, time, AudioMarker); });
        }
        m_Boxes.Add = LastItem();
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
        if (error)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted((ICON_FA_TRIANGLE_EXCLAMATION " " + *error).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            m_Boxes.Validation = LastItem();
        }
        // The clip and slot the markers belong to, for orientation (edited in the .action text).
        std::string clip = m_Document.Get().Clip.ToString();
        for (const auto& choice : m_Choices)
            if (choice.ID == m_Document.Get().Clip)
                clip = Stem(choice);
        ImGui::TextDisabled(
            "Clip %s%s%s", clip.c_str(), m_Document.Get().Slot.empty() ? "" : ", slot ", m_Document.Get().Slot.c_str());
    }

    void ActionMarkersPanel::DrawMarker(size_t index, MarkerBoxes& boxes)
    {
        const auto& p      = EditorTheme::Colors();
        const auto& marker = m_Document.Get().Events[index];
        ImGui::PushID(int(index));
        char title[160];
        // The title is the group's ID too, so it leaves out the time (a time drag keeps its field).
        std::snprintf(title, sizeof(title), "%d  %s", int(index + 1), marker.Name.c_str());
        PropertyGrid::SetNextGroupTrailer(Px(24.0f));
        const bool open = PropertyGrid::BeginGroup(title);
        ImVec2     bmin, bmax;
        if (PropertyGrid::LastGroupHeading(bmin, bmax))
        {
            // Remove, at the heading's right end.
            const ImRect  remove({bmax.x - Px(22.0f), bmin.y}, {bmax.x, bmax.y});
            const ImGuiID id = ImGui::GetID("##remove");
            if (ImGui::ItemAdd(remove, id))
            {
                bool hovered = false, held = false;
                if (ImGui::ButtonBehavior(remove, id, &hovered, &held))
                    Edit([index](ActionData& next) { RemoveMarker(next, index); });
                const float textY = bmin.y + (bmax.y - bmin.y - ImGui::GetFontSize()) * 0.5f;
                ImGui::GetWindowDrawList()->AddText({remove.Min.x + Px(5.0f), textY},
                                                    ImGui::GetColorU32(hovered ? p.Danger : p.TextDim),
                                                    ICON_FA_XMARK);
                if (hovered)
                    ImGui::SetTooltip("Remove this marker");
            }
            boxes.Remove = remove;
        }
        if (!open)
        {
            ImGui::PopID();
            return;
        }
        if (PropertyGrid::Row("Time", "Seconds into the action's clip"))
        {
            float time = float(marker.Time);
            if (ImGui::DragFloat("##time", &time, 0.005f, 0.0f, 100000.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, time](ActionData& next) { next.Events[index].Time = double(time); });
            boxes.Time = LastItem();
        }
        if (PropertyGrid::Row("Name", "Audio plays the payload's clip or event; other names reach scripts"))
        {
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
            std::string name = marker.Name;
            ImGui::InputText("##name", &name);
            boxes.Name = LastItem();
            if (ImGui::IsItemDeactivatedAfterEdit() && name != marker.Name)
                Edit([index, name](ActionData& next) { next.Events[index].Name = name; });
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CARET_DOWN "##names", {button, 0}))
                ImGui::OpenPopup("##quicknames");
            boxes.Names = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pick a name: Audio, or one this action already uses");
            if (ImGui::BeginPopup("##quicknames"))
            {
                for (const auto& quick : QuickNames(m_Document.Get()))
                {
                    if (ImGui::MenuItem(quick.c_str(), nullptr, quick == marker.Name) && quick != marker.Name)
                        Edit([index, quick](ActionData& next) { next.Events[index].Name = quick; });
                    m_Boxes.Menu.emplace_back(quick, LastItem());
                    if (quick == AudioMarker && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Plays an AudioClip or AudioEvent at the entity");
                }
                ImGui::EndPopup();
            }
        }
        if (marker.Name == AudioMarker)
        {
            std::string payload = marker.Payload;
            if (PropertyGrid::Row("Sound", "The AudioClip or AudioEvent this marker plays (drop one here)") &&
                AudioPayloadCombo(payload, boxes))
                Edit([index, payload](ActionData& next) { next.Events[index].Payload = payload; });
            if (const char* warning = AudioPayloadWarning(CheckAudioPayload(marker.Payload, m_Choices)))
            {
                boxes.Warning = warning;
                if (PropertyGrid::Row(""))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted((std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + warning).c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            }
        }
        else if (PropertyGrid::Row("Payload", "Text passed along with the marker"))
        {
            std::string payload = marker.Payload;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##payload", "string passed along", &payload);
            boxes.Payload = LastItem();
            if (ImGui::IsItemDeactivatedAfterEdit() && payload != marker.Payload)
                Edit([index, payload](ActionData& next) { next.Events[index].Payload = payload; });
        }
        PropertyGrid::EndGroup();
        ImGui::PopID();
    }

    bool ActionMarkersPanel::AudioPayloadCombo(std::string& payload, MarkerBoxes& boxes)
    {
        const EditorAssetChoice* current = nullptr;
        const auto               status  = CheckAudioPayload(payload, m_Choices, &current);
        std::string              preview;
        if (status == AudioPayload::Valid)
            preview =
                std::string(current->Type == AssetType::AudioEvent ? ICON_FA_LAYER_GROUP " " : ICON_FA_MUSIC " ") +
                Stem(*current);
        else if (status == AudioPayload::Empty)
            preview = "(none)";
        else
            preview = ICON_FA_TRIANGLE_EXCLAMATION " " +
                      (status == AudioPayload::WrongType ? Stem(*current) + " (not audio)" : payload + " (missing)");
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (status != AudioPayload::Valid && status != AudioPayload::Empty)
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Warning);
        const bool open = ImGui::BeginCombo("##sound", preview.c_str(), ImGuiComboFlags_HeightLarge);
        if (status != AudioPayload::Valid && status != AudioPayload::Empty)
            ImGui::PopStyleColor();
        boxes.Payload = LastItem();
        if (open)
        {
            bool any = false;
            for (const AssetType type : {AssetType::AudioEvent, AssetType::AudioClip})
            {
                ImGui::TextDisabled("%s", type == AssetType::AudioEvent ? "Audio events" : "Audio clips");
                for (const auto& choice : m_Choices)
                {
                    if (choice.Type != type)
                        continue;
                    any = true;
                    ImGui::PushID(choice.ID.ToString().c_str());
                    const std::string id = choice.ID.ToString();
                    if (ImGui::Selectable(Stem(choice).c_str(), id == payload) && id != payload)
                        payload = id, changed = true;
                    m_Boxes.Menu.emplace_back(Stem(choice), LastItem());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", choice.Name.c_str());
                    ImGui::PopID();
                }
            }
            if (!any)
                ImGui::TextDisabled("No imported AudioClips or AudioEvents");
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !open)
            ImGui::SetTooltip("%s\nDrop an audio file or .audioevent here.", current ? current->Name.c_str() : "");
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* drop = ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto file = Canonical(
                    std::filesystem::u8path(std::string(static_cast<const char*>(drop->Data), drop->DataSize)));
                bool found = false;
                for (const auto& choice : m_Choices)
                    if (IsAudioChoice(choice) && Canonical(choice.SourcePath) == file)
                    {
                        found = true;
                        if (choice.ID.ToString() != payload)
                            payload = choice.ID.ToString(), changed = true;
                        break;
                    }
                if (!found)
                    m_Status = "Only imported AudioClips and AudioEvents can be dropped here.";
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }
} // namespace Kans
