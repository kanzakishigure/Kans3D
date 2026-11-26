#include "AudioOutputSettingsPanel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/Audio/AudioMixerEditing.h"
#include "KansEditorCore/Audio/AudioOutputEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace AudioOutputEditing;
    using EditorTheme::Px;

    namespace
    {
        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        constexpr float PlayerMinDb = -80.0f, PlayerMaxDb = 12.0f;
    } // namespace

    void AudioOutputSettingsPanel::Bind(AudioEngine* engine)
    {
        m_Engine         = engine;
        m_DeviceRevision = UINT64_MAX; // Lists the devices on the next draw.
        m_Devices.clear();
        m_Status.clear();
    }

    void AudioOutputSettingsPanel::RefreshDevices()
    {
        m_Devices        = m_Engine && m_Engine->IsInitialized() ? m_Engine->EnumerateOutputDevices() :
                                                                   std::vector<AudioOutputDevice> {};
        m_DeviceRevision = m_Engine ? m_Engine->GetLastDeviceChange().Revision : 0;
    }

    void AudioOutputSettingsPanel::Draw()
    {
        m_Boxes = {};
        if (!m_Engine || !m_Engine->IsInitialized())
        {
            m_Boxes.Hint = NoDevicesHint(nullptr, {});
            ImGui::TextDisabled("%s", m_Boxes.Hint.c_str());
            return;
        }
        if (m_DeviceRevision != m_Engine->GetLastDeviceChange().Revision)
            RefreshDevices();
        PropertyGrid::Begin("##audioOutput", 110.0f);
        DrawOutput();
        DrawPlayerVolume();
        PropertyGrid::End();

        // Saved per user (never in the project), so a team shares the project but not the speakers.
        const auto& file = m_Engine->GetUserSettingsFile();
        m_Boxes.File     = file.empty() ? std::string("Storage is off for this engine") : file.u8string();
        ImGui::BeginDisabled(file.empty());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save"))
        {
            if (auto error = m_Engine->SaveUserSettings())
                m_Status = "Not saved: " + *error;
            else
                m_Status = "Saved.";
        }
        ImGui::EndDisabled();
        m_Boxes.Save = LastItem();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Write the device, speaker mode and player volume to the per-user settings file");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", m_Status.empty() ? "Per-user settings" : m_Status.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", m_Boxes.File.c_str());
        ImGui::PopTextWrapPos();
    }

    void AudioOutputSettingsPanel::DrawOutput()
    {
        const auto& p = EditorTheme::Colors();
        if (!PropertyGrid::BeginGroup("Output"))
            return;
        const std::string selected = m_Engine->GetUserSettings().OutputDevice;
        if (PropertyGrid::Row("Device", "Where the editor plays: the system default follows the system's choice"))
        {
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
            const bool open = ImGui::BeginCombo("##device", SelectedDeviceLabel(m_Devices, selected).c_str());
            m_Boxes.Device  = LastItem();
            if (open)
            {
                if (ImGui::IsWindowAppearing())
                    RefreshDevices();
                for (const auto& choice : DeviceChoices(m_Devices, selected))
                {
                    ImGui::PushID(choice.Id.c_str());
                    if (choice.Missing)
                        ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                    if (ImGui::Selectable(choice.Label.c_str(), choice.Selected) && !choice.Selected && !choice.Missing)
                        m_Status =
                            m_Engine->SetOutputDevice(choice.Id) ? std::string() : "Cannot open " + choice.Label + ".";
                    if (choice.Missing)
                        ImGui::PopStyleColor();
                    m_Boxes.Menu.emplace_back(choice.Label, LastItem());
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ROTATE "##refresh", {button, 0}))
                RefreshDevices();
            m_Boxes.Refresh = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("List the devices again");
        }
        m_Boxes.Hint     = NoDevicesHint(m_Engine, m_Devices);
        m_Boxes.Fallback = FallbackText(m_Engine->GetLastDeviceChange(), m_Devices);
        if (!m_Boxes.Hint.empty() && PropertyGrid::Row(""))
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", m_Boxes.Hint.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!m_Boxes.Fallback.empty() && PropertyGrid::Row(""))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted((ICON_FA_TRIANGLE_EXCLAMATION " " + m_Boxes.Fallback).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        const uint32_t resolved =
            m_Engine->GetOutputFormat().Channels ? m_Engine->GetOutputFormat().Channels : m_Engine->GetChannels();
        const auto mode = m_Engine->GetSpeakerMode();
        if (PropertyGrid::Row("Speakers",
                              "Auto follows the device's channels; the others fold or spread the mix to that layout",
                              mode != SpeakerMode::Auto))
        {
            const bool open  = ImGui::BeginCombo("##speakers", SpeakerModeLabel(mode, resolved).c_str());
            m_Boxes.Speakers = LastItem();
            if (open)
            {
                for (int i = 0; i < SpeakerModeCount; ++i)
                {
                    const auto choice = SpeakerMode(i);
                    const auto label  = SpeakerModeLabel(choice, choice == SpeakerMode::Auto ? resolved : 0);
                    if (ImGui::Selectable(label.c_str(), choice == mode) && choice != mode)
                        m_Status = m_Engine->SetSpeakerMode(choice) ? std::string() : "Cannot use " + label + ".";
                    m_Boxes.Menu.emplace_back(label, LastItem());
                }
                ImGui::EndCombo();
            }
            if (PropertyGrid::RowReset(mode != SpeakerMode::Auto))
                m_Engine->SetSpeakerMode(SpeakerMode::Auto);
        }
        char format[64];
        std::snprintf(
            format, sizeof(format), "%u Hz, %u channels mixed", m_Engine->GetSampleRate(), m_Engine->GetChannels());
        PropertyGrid::Text("Format", format, true);
        PropertyGrid::EndGroup();
    }

    void AudioOutputSettingsPanel::DrawPlayerVolume()
    {
        const auto& p = EditorTheme::Colors();
        if (!PropertyGrid::BeginGroup("Player volume"))
            return;
        PropertyGrid::BeginFree();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The player's own levels (a game's options menu): added after each bus's authored "
                            "fader, which they never change.");
        ImGui::PopTextWrapPos();
        PropertyGrid::EndFree();
        const auto& buses = m_Engine->GetBuses();
        for (const auto& entry : AudioMixerEditing::TreeOrder(AudioBusLayout {buses}))
        {
            const auto& bus      = buses[entry.Index].Name;
            const auto  settings = m_Engine->GetUserBusSettings(bus).value_or(AudioUserBusSettings {});
            const bool  modified = settings.VolumeDb != 0.0f || settings.Mute;
            auto&       boxes    = m_Boxes.Buses.emplace_back();
            boxes.Bus            = bus;
            char label[96];
            std::snprintf(label, sizeof(label), "%*s%s", entry.Depth * 2, "", bus.c_str());
            if (!PropertyGrid::Row(label, "Player level for this bus, on top of its authored fader", modified))
                continue;
            ImGui::PushID(bus.c_str());
            const float button = ImGui::GetFrameHeight();
            // CalcItemWidth leaves room for the row's reset button.
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemSpacing.x);
            float db = settings.VolumeDb;
            if (ImGui::SliderFloat(
                    "##player", &db, PlayerMinDb, PlayerMaxDb, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp) &&
                db != settings.VolumeDb)
                m_Engine->SetUserBusVolumeDb(bus, db);
            boxes.Volume = LastItem();
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("Player level (-80 to +12 dB); the authored fader stays in the strip");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, settings.Mute ? p.Danger : p.Inset);
            ImGui::PushStyleColor(ImGuiCol_Text, settings.Mute ? ImVec4(0.06f, 0.06f, 0.07f, 1.0f) : p.TextDim);
            if (ImGui::Button("M##playerMute", {button, 0}))
                m_Engine->SetUserBusMute(bus, !settings.Mute);
            ImGui::PopStyleColor(2);
            boxes.Mute = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Player mute (the authored Mute is separate)");
            if (PropertyGrid::RowReset(modified))
            {
                m_Engine->SetUserBusVolumeDb(bus, 0.0f);
                m_Engine->SetUserBusMute(bus, false);
            }
            boxes.Reset = modified ? LastItem() : ImRect {};
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT " Reset player volume"))
            for (const auto& bus : buses)
            {
                m_Engine->SetUserBusVolumeDb(bus.Name, 0.0f);
                m_Engine->SetUserBusMute(bus.Name, false);
            }
        m_Boxes.ResetAll = LastItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every bus back to 0 dB and unmuted (the player layer only)");
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }
} // namespace Kans
