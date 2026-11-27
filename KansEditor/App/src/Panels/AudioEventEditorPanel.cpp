#include "AudioEventEditorPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Components/ComponentEditors.h"
#include "KansEditorCore/Audio/AudioSpatialEditing.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/AudioWaveform.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace AudioEventEditing;
    using EditorTheme::Px;

    namespace
    {
        constexpr char EntryPayload[] = "KANS_EVENT_ENTRY";
        constexpr char AssetPayload[] = "asset_payload"; // A source file dragged from the Content Browser.

        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path : result;
        }

        std::filesystem::path PayloadPath(const ImGuiPayload& payload)
        {
            return std::filesystem::u8path(std::string(static_cast<const char*>(payload.Data), payload.DataSize));
        }

        // A button of the header and audition rows; `id` keeps its ID while its label changes.
        bool ToolButton(const char* label, const char* tooltip, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Button(label);
            ImGui::EndDisabled();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        constexpr const char* RangeNames[3] = {"Volume range", "Pitch range", "Start offset"};
    } // namespace

    // ------------------------------------------------------------------ binding and files

    void AudioEventEditorPanel::Bind(EditorAssetImportService* service, AudioEngine* engine, AssetManager* assets)
    {
        if (service != m_Service)
            m_PeakCache.Clear();
        m_Service = service;
        m_Engine  = engine;
        m_Preview.Bind(engine, assets);
    }

    void AudioEventEditorPanel::SetAssetChoices(const std::vector<EditorAssetChoice>& choices)
    {
        m_Clips.clear();
        m_Attenuations.clear();
        for (const auto& choice : choices)
            if (choice.Type == AssetType::AudioClip)
                m_Clips.push_back(choice);
            else if (choice.Type == AssetType::AudioAttenuation)
                m_Attenuations.push_back(choice);
    }

    bool AudioEventEditorPanel::Open(const std::filesystem::path& file)
    {
        auto loaded = LoadEventFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            m_Status = file.filename().u8string() + ": " + error->Message;
            return false;
        }
        const bool other = m_File.empty() || Canonical(m_File) != Canonical(file);
        // Switching files keeps valid edits instead of dropping them.
        if (other && m_Document.IsDirty() && !m_File.empty() && !m_Document.GetError())
            Save();
        if (other)
            m_Preview.Reset(); // Another asset: its audition, selection state and statistics start over.
        m_Opened = true;
        m_File   = file;
        m_NewDirectory.clear();
        m_Pending.reset();
        m_Document.Reset(std::get<AudioEventData>(std::move(loaded)));
        m_Status.clear();
        return true;
    }

    void AudioEventEditorPanel::OpenNew(const std::filesystem::path& directory)
    {
        m_Preview.Reset();
        m_Opened = true;
        m_File.clear();
        m_NewDirectory = directory;
        m_Pending.reset();
        m_Document.Reset({}, false);
        m_Status = "Add a clip, then Save to write the file.";
    }

    bool AudioEventEditorPanel::Save()
    {
        if (!m_Opened)
            return false;
        if (auto error = m_Document.GetError())
        {
            m_Status = "Not saved: " + *error;
            return false;
        }
        auto file = m_File.empty() ? UniqueEventFile(m_NewDirectory, "NewAudioEvent") : m_File;
        if (m_Service && m_Service->IsReadOnly(file))
        {
            m_Status = "Plugin content is read-only.";
            return false;
        }
        if (auto error = SaveEventFile(file, m_Document.Get()))
        {
            m_Status = error->Message;
            return false;
        }
        m_File = file;
        m_NewDirectory.clear();
        m_Document.MarkSaved();
        m_Status = BuildSavedSource(m_Service, m_File);
        return true;
    }

    // ------------------------------------------------------------------ clips

    const EditorAssetChoice* AudioEventEditorPanel::FindClip(AssetID clip) const
    {
        for (const auto& choice : m_Clips)
            if (choice.ID == clip)
                return &choice;
        return nullptr;
    }

    AssetID AudioEventEditorPanel::ClipAt(const std::filesystem::path& file) const
    {
        const auto path = Canonical(file);
        for (const auto& choice : m_Clips)
            if (Canonical(choice.SourcePath) == path)
                return choice.ID;
        return {};
    }

    const std::vector<uint8_t>* AudioEventEditorPanel::PeaksOf(AssetID clip)
    {
        if (m_Waveforms)
            return m_Waveforms(clip);
        if (!m_Service || !clip.IsValid())
            return nullptr;
        const auto* summary = m_PeakCache.Find(
            m_Service->GetRuntimeAssetRegistry(), m_Service->GetProductDirectory(), clip.GetSourceID());
        return summary ? &summary->Data.Waveform : nullptr;
    }

    bool AudioEventEditorPanel::ClipCombo(const char* id, AssetID& clip, const char* none)
    {
        const auto* current = FindClip(clip);
        std::string preview = current        ? std::filesystem::u8path(current->Name).stem().u8string() :
                              clip.IsValid() ? std::string(ICON_FA_TRIANGLE_EXCLAMATION " (missing)") :
                                               std::string(none);
        bool        changed = false;
        if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (m_Clips.empty())
                ImGui::TextDisabled("No imported AudioClips");
            for (const auto& choice : m_Clips)
            {
                ImGui::PushID(choice.ID.ToString().c_str());
                if (ImGui::Selectable(std::filesystem::u8path(choice.Name).stem().u8string().c_str(),
                                      choice.ID == clip))
                {
                    changed = choice.ID != clip;
                    clip    = choice.ID;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", choice.Name.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (current && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nDrop an audio file here to replace it.", current->Name.c_str());
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayload))
                if (const AssetID dropped = ClipAt(PayloadPath(*payload)); dropped.IsValid())
                {
                    changed = dropped != clip;
                    clip    = dropped;
                }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    bool AudioEventEditorPanel::AttenuationCombo(const char* id, AssetID& asset)
    {
        const EditorAssetChoice* current = nullptr;
        for (const auto& choice : m_Attenuations)
            if (choice.ID == asset)
                current = &choice;
        const std::string preview = current         ? std::filesystem::u8path(current->Name).stem().u8string() :
                                    asset.IsValid() ? std::string(ICON_FA_TRIANGLE_EXCLAMATION " (missing)") :
                                                      std::string("Built-in model");
        bool              changed = false;
        if (ImGui::BeginCombo(id, preview.c_str()))
        {
            if (ImGui::Selectable("Built-in model", !asset.IsValid()) && asset.IsValid())
                asset = {}, changed = true;
            for (const auto& choice : m_Attenuations)
            {
                ImGui::PushID(choice.ID.ToString().c_str());
                if (ImGui::Selectable(std::filesystem::u8path(choice.Name).stem().u8string().c_str(),
                                      choice.ID == asset) &&
                    choice.ID != asset)
                    asset = choice.ID, changed = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", choice.Name.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A shared distance curve; replaces the built-in model below.\nDrop a "
                              ".audioattenuation file here.");
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto file = Canonical(PayloadPath(*payload));
                for (const auto& choice : m_Attenuations)
                    if (Canonical(choice.SourcePath) == file && choice.ID != asset)
                        asset = choice.ID, changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    // ------------------------------------------------------------------ edits

    void AudioEventEditorPanel::Edit(const std::function<void(AudioEventData&)>& change)
    {
        auto next = m_Pending ? *m_Pending : m_Document.Get();
        change(next);
        m_PendingContinuous = (m_Pending ? m_PendingContinuous : true) && ImGui::IsAnyItemActive();
        m_Pending           = std::move(next);
    }

    void AudioEventEditorPanel::CommitPending()
    {
        if (!m_Pending)
            return;
        auto data = std::move(*m_Pending);
        m_Pending.reset();
        m_Document.Commit(std::move(data), m_PendingContinuous);
    }

    // ------------------------------------------------------------------ drawing

    void AudioEventEditorPanel::Draw()
    {
        m_Focused          = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_Boxes            = {};
        ComponentEditors::LastAudioSpatialBoxes() = {};
        const bool playing = m_PlayMode && m_PlayMode();
        if (playing)
            m_Preview.Stop();
        m_Preview.Update(ImGui::GetIO().DeltaTime, m_Document.Get());
        if (!m_Opened)
        {
            ImGui::TextDisabled(
                "%s", m_Status.empty() ? "Open a .audioevent file from the Content Browser." : m_Status.c_str());
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
        DrawAudition();
        DrawEntries();
        DrawSelection();
        DrawVariation();
        DrawLimits();
        DrawOutput();
        DrawSpatializer();
        CommitPending();
        if (!ImGui::IsAnyItemActive())
            m_Document.EndGesture();
    }

    void AudioEventEditorPanel::DrawHeader()
    {
        const auto& p    = EditorTheme::Colors();
        std::string name = m_File.empty() ? std::string("New audio event (unsaved)") : m_File.filename().u8string();
        if (m_Document.IsDirty() && !m_File.empty())
            name += " *";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.TypeAudio, ICON_FA_LAYER_GROUP);
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered() && !m_File.empty())
            ImGui::SetTooltip("%s", m_File.u8string().c_str());
        const auto error = m_Document.GetError();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_FLOPPY_DISK " Save",
                       error ? "Fix the problem above to save" : "Write the file and rebuild it (Ctrl+S)",
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
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
        // Why the event cannot be saved (or played).
        if (error)
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", error->c_str());
            ImGui::PopStyleColor();
            m_Boxes.Validation = {start, ImGui::GetItemRectMax()};
        }
    }

    void AudioEventEditorPanel::DrawAudition()
    {
        const auto& p         = EditorTheme::Colors();
        const bool  playing   = m_PlayMode && m_PlayMode();
        const bool  available = m_Preview.IsAvailable() && !playing;
        const char* why       = playing                  ? "The scene's audio runs in Play mode" :
                                !m_Preview.IsAvailable() ? "The editor has no audio engine" :
                                                           "Play the event once with its selection, variation, "
                                                           "cooldown and instance limit";
        if (ToolButton(ICON_FA_BOLT " Trigger", why, available))
            m_Preview.Trigger(m_Document.Get());
        m_Boxes.Trigger = LastItem();
        ImGui::SameLine();
        bool automatic = m_Preview.IsAutoTriggering();
        ImGui::BeginDisabled(!available);
        if (ImGui::Checkbox("Auto##trigger", &automatic))
            m_Preview.SetAutoTrigger(automatic);
        ImGui::EndDisabled();
        m_Boxes.Auto = LastItem();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Trigger repeatedly at the interval, to hear the variation");
        ImGui::SameLine();
        float interval = m_Preview.GetInterval();
        ImGui::SetNextItemWidth(Px(80.0f));
        if (ImGui::DragFloat("##interval",
                             &interval,
                             0.01f,
                             AudioEventPreview::MinInterval,
                             AudioEventPreview::MaxInterval,
                             "every %.2f s",
                             ImGuiSliderFlags_AlwaysClamp))
            m_Preview.SetInterval(interval);
        m_Boxes.Interval = LastItem();
        if (m_Preview.GetActiveVoices())
        {
            ImGui::SameLine();
            if (ToolButton(ICON_FA_STOP "##stop", "Stop the audition"))
                m_Preview.Stop();
        }

        // The last pick and the preview's statistics.
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (const auto& last = m_Preview.GetLast())
        {
            const bool started = last->Status == AudioEventTriggerStatus::Started;
            ImGui::PushStyleColor(ImGuiCol_Text, started ? p.Success : p.Warning);
            ImGui::TextUnformatted(StatusName(last->Status));
            ImGui::PopStyleColor();
            if (started)
            {
                const auto& entries = m_Document.Get().Entries;
                std::string clip    = "#" + std::to_string(last->Entry + 1);
                if (last->Entry < entries.size())
                    if (const auto* choice = FindClip(entries[last->Entry].Clip))
                        clip += " " + std::filesystem::u8path(choice->Name).stem().u8string();
                char text[192];
                std::snprintf(text,
                              sizeof(text),
                              "%s   %+.1f dB   %+.2f st   %.2f s",
                              clip.c_str(),
                              last->VolumeDb,
                              last->PitchSemitones,
                              last->StartOffsetSeconds);
                ImGui::SameLine();
                ImGui::TextUnformatted(text);
            }
        }
        else
            ImGui::TextDisabled("Not triggered yet");
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", EventStatsText(m_Preview.GetStats()).c_str());
        m_Boxes.LastPick = {start, {start.x + ImGui::GetContentRegionAvail().x, ImGui::GetItemRectMax().y}};
    }

    void AudioEventEditorPanel::DrawEntries()
    {
        const auto& p       = EditorTheme::Colors();
        const auto& data    = m_Document.Get();
        const bool  random  = data.Selection == AudioEventSelection::Random;
        const auto  shares  = SelectionShares(data);
        const float height  = ImGui::GetFrameHeight();
        const auto  columns = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        m_Boxes.Rows.resize(data.Entries.size());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (ImGui::BeginTable("##entries", 8, columns))
        {
            ImGui::TableSetupColumn("##grip", ImGuiTableColumnFlags_WidthFixed, Px(14.0f));
            ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
            ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, Px(58.0f));
            ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Pitch", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, Px(46.0f));
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, Px(50.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < data.Entries.size(); ++i)
            {
                const auto& entry = data.Entries[i];
                auto&       box   = m_Boxes.Rows[i];
                ImGui::PushID(int(i));
                ImGui::TableNextRow(ImGuiTableRowFlags_None, height);

                // The grip: drag onto another row to move this entry there (order matters for Sequential).
                ImGui::TableNextColumn();
                const float   rowTop  = ImGui::GetCursorScreenPos().y;
                const ImVec2  gripMin = ImGui::GetCursorScreenPos();
                const ImRect  grip(gripMin, {gripMin.x + Px(14.0f), gripMin.y + height});
                const ImGuiID gripID = ImGui::GetID("##grip");
                ImGui::ItemSize(grip);
                if (ImGui::ItemAdd(grip, gripID))
                {
                    bool hovered = false, held = false;
                    ImGui::ButtonBehavior(grip, gripID, &hovered, &held);
                    ImGui::GetWindowDrawList()->AddText(
                        {grip.Min.x + Px(4.0f), grip.Min.y + ImGui::GetStyle().FramePadding.y},
                        ImGui::GetColorU32(hovered || held ? p.Text : p.TextDim),
                        ICON_FA_ELLIPSIS_VERTICAL);
                    if (hovered && !held)
                        ImGui::SetTooltip("Drag onto another entry to reorder");
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        ImGui::SetDragDropPayload(EntryPayload, &i, sizeof(i));
                        ImGui::Text("Entry %zu", i + 1);
                        ImGui::EndDragDropSource();
                    }
                }
                box.Grip = grip;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                AssetID clip = entry.Clip;
                if (ClipCombo("##clip", clip, "(no clip)"))
                    Edit([i, clip](AudioEventData& next) { next.Entries[i].Clip = clip; });
                box.Clip = LastItem();

                ImGui::TableNextColumn();
                const ImVec2 waveMin = ImGui::GetCursorScreenPos();
                const ImVec2 waveMax {waveMin.x + ImGui::GetContentRegionAvail().x, waveMin.y + height};
                ImGui::Dummy({waveMax.x - waveMin.x, height});
                box.Waveform = {waveMin, waveMax};
                if (const auto* peaks = PeaksOf(entry.Clip); peaks && !peaks->empty())
                {
                    AudioWaveform::Draw(*peaks, waveMin, waveMax);
                    box.WaveformBars = size_t(std::max(0.0f, waveMax.x - waveMin.x));
                }
                else
                    ImGui::GetWindowDrawList()->AddRectFilled(waveMin, waveMax, ImGui::GetColorU32(p.Inset), Px(2.0f));

                ImGui::TableNextColumn();
                float weight = entry.Weight;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##weight", &weight, 0.01f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                    Edit([i, weight](AudioEventData& next) { next.Entries[i].Weight = weight; });
                box.Weight = LastItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Weight: how often Random picks this entry (relative to the others)");

                ImGui::TableNextColumn();
                float volume = entry.VolumeDb;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat(
                        "##volume", &volume, 0.1f, -80.0f, 24.0f, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp))
                    Edit([i, volume](AudioEventData& next) { next.Entries[i].VolumeDb = volume; });
                box.Volume = LastItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Volume offset of this entry, added to the event's");

                ImGui::TableNextColumn();
                float pitch = entry.PitchSemitones;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##pitch", &pitch, 0.05f, -48.0f, 48.0f, "%+.2f st", ImGuiSliderFlags_AlwaysClamp))
                    Edit([i, pitch](AudioEventData& next) { next.Entries[i].PitchSemitones = pitch; });
                box.Pitch = LastItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Pitch offset of this entry in semitones, added to the event's");

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (random)
                {
                    ImGui::Text("%.0f%%", shares[i]);
                    box.ShownShare = shares[i];
                }
                else
                    ImGui::TextDisabled("-");
                box.Share = LastItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(random ? "This entry's share of the picks (weight / sum of weights)" :
                                               "Only Random picks by weight");

                ImGui::TableNextColumn();
                const bool canPlay = m_Preview.IsAvailable() && !(m_PlayMode && m_PlayMode()) && entry.Clip.IsValid();
                ImGui::BeginDisabled(!canPlay);
                if (ImGui::SmallButton(ICON_FA_PLAY "##play"))
                    m_Preview.PlayEntry(data, i);
                ImGui::EndDisabled();
                box.Play = LastItem();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Play this entry with the event's base volume and pitch");
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    Edit([i](AudioEventData& next) { RemoveEntry(next, i); });
                box.Remove = LastItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove this entry");

                // The whole row takes a dragged entry (not only the cell drawn last, whose clip rect is current).
                const ImGuiTable* table = ImGui::GetCurrentTable();
                box.Row                 = {{table->OuterRect.Min.x, rowTop}, {table->OuterRect.Max.x, rowTop + height}};
                ImGui::PushClipRect(box.Row.Min, box.Row.Max, false);
                const bool target = ImGui::BeginDragDropTargetCustom(box.Row, ImGui::GetID("##drop"));
                ImGui::PopClipRect();
                if (target)
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(EntryPayload))
                    {
                        size_t from = 0;
                        std::memcpy(&from, payload->Data, sizeof(from));
                        Edit([from, i](AudioEventData& next) { MoveEntry(next, from, i); });
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (data.Entries.empty())
            ImGui::TextDisabled("No entries: add a clip below or drop audio files here.");

        // Adds an entry: pick a clip, or drop audio files anywhere on the list.
        AssetID added;
        ImGui::SetNextItemWidth(Px(220.0f));
        if (ClipCombo("##add", added, ICON_FA_PLUS " Add clip") && added.IsValid())
            Edit([added](AudioEventData& next) { AddEntry(next, added); });
        m_Boxes.Add     = LastItem();
        m_Boxes.Entries = {start, {start.x + ImGui::GetContentRegionAvail().x, ImGui::GetItemRectMax().y}};
        if (ImGui::BeginDragDropTargetCustom(m_Boxes.Entries, ImGui::GetID("##entries-drop")))
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayload))
            {
                if (const AssetID dropped = ClipAt(PayloadPath(*payload)); dropped.IsValid())
                    Edit([dropped](AudioEventData& next) { AddEntry(next, dropped); });
                else
                    m_Status = "Only imported audio files can be added.";
            }
            ImGui::EndDragDropTarget();
        }
    }

    void AudioEventEditorPanel::DrawSelection()
    {
        static const AudioEventData defaults;
        if (!PropertyGrid::BeginGroup("Selection"))
            return;
        const auto& data = m_Document.Get();
        const char* modes[] {"Random", "Shuffle", "Sequential"};
        int         mode = int(data.Selection), modeReset = int(defaults.Selection);
        if (PropertyGrid::Combo("Mode", modes, 3, mode, &modeReset))
            Edit([mode](AudioEventData& next) { next.Selection = AudioEventSelection(mode); });
        if (data.Selection == AudioEventSelection::Random)
        {
            const uint32_t limit = MaxAvoidRepeatingLast(data);
            char           hint[200];
            std::snprintf(hint,
                          sizeof(hint),
                          "Skip the entries picked in the last N triggers. At most %u with %zu entries (one must "
                          "stay pickable); a larger value plays as %u.",
                          limit,
                          data.Entries.size(),
                          limit);
            const bool modified = data.AvoidRepeatingLast != 0;
            if (PropertyGrid::Row("Avoid repeating last", hint, modified))
            {
                ImGui::PushID("AvoidRepeatingLast");
                int value = int(std::min(data.AvoidRepeatingLast, limit));
                if (ImGui::SliderInt("##value", &value, 0, int(limit), "%d", ImGuiSliderFlags_AlwaysClamp))
                    Edit([value](AudioEventData& next) { next.AvoidRepeatingLast = uint32_t(value); });
                if (PropertyGrid::RowReset(modified))
                    Edit([](AudioEventData& next) { next.AvoidRepeatingLast = 0; });
                ImGui::PopID();
            }
        }
        PropertyGrid::EndGroup();
    }

    bool AudioEventEditorPanel::RangeRow(const char*     label,
                                         size_t          index,
                                         AudioEventRange stored,
                                         const char*     format,
                                         float           speed)
    {
        const auto& p        = EditorTheme::Colors();
        auto&       field    = m_Ranges[index];
        const auto  kind     = index == 2 ? RangeKind::Nonnegative : RangeKind::Signed;
        const bool  modified = stored.Min != 0.0f || stored.Max != 0.0f;
        const char* hints[3] = {"A random offset in dB between min and max, drawn on every trigger",
                                "A random offset in semitones between min and max, drawn on every trigger",
                                "Start this many seconds into the clip, drawn between min and max"};
        if (!PropertyGrid::Row(label, hints[index], modified))
            return false;
        ImGui::PushID(label);
        const ImGuiID minID = ImGui::GetID("##min"), maxID = ImGui::GetID("##max");
        const ImGuiID active = ImGui::GetActiveID();
        field.Sync(stored, active == minID || active == maxID);
        auto        shown   = field.GetShown();
        const bool  error   = !field.GetError().empty();
        const float width   = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        bool        changed = false;
        if (error)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(p.Danger.x, p.Danger.y, p.Danger.z, 0.35f));
        const float lower = kind == RangeKind::Nonnegative ? 0.0f : -96.0f;
        for (int side = 0; side < 2; ++side)
        {
            if (side)
                ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            float value = side ? shown.Max : shown.Min;
            if (ImGui::DragFloat(side ? "##max" : "##min", &value, speed, lower, 96.0f, format))
                if (const auto accepted = field.Edit(side == 1, value, kind))
                {
                    Edit([index, range = *accepted](AudioEventData& next) {
                        (index == 0 ? next.VolumeRangeDb :
                         index == 1 ? next.PitchRangeSemitones :
                                      next.StartOffsetSeconds) = range;
                    });
                    changed = true;
                }
            m_Boxes.Ranges[index][side] = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(side ? "Max" : "Min");
        }
        if (error)
            ImGui::PopStyleColor();
        if (PropertyGrid::RowReset(modified))
            Edit([index](AudioEventData& next) {
                (index == 0 ? next.VolumeRangeDb :
                 index == 1 ? next.PitchRangeSemitones :
                              next.StartOffsetSeconds) = AudioEventRange {};
            });
        ImGui::PopID();
        // The refusal, under the fields.
        if (!field.GetError().empty() && PropertyGrid::Row(""))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::TextWrapped("%s (not applied)", field.GetError().c_str());
            ImGui::PopStyleColor();
        }
        return changed;
    }

    void AudioEventEditorPanel::DrawVariation()
    {
        static const AudioEventData defaults;
        if (!PropertyGrid::BeginGroup("Variation"))
            return;
        const auto& data   = m_Document.Get();
        float       volume = data.VolumeDb, pitch = data.PitchSemitones;
        if (PropertyGrid::Float("Volume", volume, 0.1f, -80.0f, 24.0f, "%+.1f dB", &defaults.VolumeDb))
            Edit([volume](AudioEventData& next) { next.VolumeDb = volume; });
        if (PropertyGrid::Float("Pitch", pitch, 0.05f, -48.0f, 48.0f, "%+.2f st", &defaults.PitchSemitones))
            Edit([pitch](AudioEventData& next) { next.PitchSemitones = pitch; });
        RangeRow(RangeNames[0], 0, data.VolumeRangeDb, "%+.1f dB", 0.1f);
        RangeRow(RangeNames[1], 1, data.PitchRangeSemitones, "%+.2f st", 0.05f);
        RangeRow(RangeNames[2], 2, data.StartOffsetSeconds, "%.3f s", 0.005f);
        PropertyGrid::EndGroup();
    }

    void AudioEventEditorPanel::DrawLimits()
    {
        static const AudioEventData defaults;
        if (!PropertyGrid::BeginGroup("Limits"))
            return;
        const auto& data     = m_Document.Get();
        float       cooldown = float(data.CooldownSeconds);
        const float reset    = 0.0f;
        if (PropertyGrid::Float("Cooldown", cooldown, 0.005f, 0.0f, 60.0f, "%.3f s", &reset))
            Edit([cooldown](AudioEventData& next) { next.CooldownSeconds = cooldown; });
        // 0 shows "Unlimited" (stored as the largest count).
        const bool unlimited = data.InstanceLimit.MaxCount == std::numeric_limits<uint32_t>::max();
        const bool modified  = !unlimited;
        if (PropertyGrid::Row(
                "Max instances", "How many instances of this event may play at once; 0 is unlimited", modified))
        {
            ImGui::PushID("MaxInstances");
            int count = unlimited ? 0 : int(std::min<uint32_t>(data.InstanceLimit.MaxCount, 1024));
            if (ImGui::DragInt(
                    "##value", &count, 0.1f, 0, 1024, count == 0 ? "Unlimited" : "%d", ImGuiSliderFlags_AlwaysClamp))
                Edit([count](AudioEventData& next) {
                    next.InstanceLimit.MaxCount = count == 0 ? std::numeric_limits<uint32_t>::max() : uint32_t(count);
                });
            if (PropertyGrid::RowReset(modified))
                Edit([](AudioEventData& next) { next.InstanceLimit.MaxCount = std::numeric_limits<uint32_t>::max(); });
            ImGui::PopID();
        }
        if (!unlimited)
        {
            const char* rules[] {"Stop oldest", "Stop quietest", "Stop lowest priority", "Reject new"};
            int         rule = int(data.InstanceLimit.Rule), ruleReset = int(defaults.InstanceLimit.Rule);
            if (PropertyGrid::Combo("Rule", rules, 4, rule, &ruleReset))
                Edit([rule](AudioEventData& next) { next.InstanceLimit.Rule = AudioConcurrencyRule(rule); });
            float release = data.InstanceLimit.ReleaseSeconds;
            if (PropertyGrid::Float(
                    "Release", release, 0.001f, 0.0f, 5.0f, "%.3f s", &defaults.InstanceLimit.ReleaseSeconds))
                Edit([release](AudioEventData& next) { next.InstanceLimit.ReleaseSeconds = release; });
        }
        std::string group = data.ConcurrencyGroup;
        if (PropertyGrid::InputText("Shared group", group))
            Edit([group](AudioEventData& next) { next.ConcurrencyGroup = group; });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A concurrency group shared with other sounds (empty: none)");
        PropertyGrid::EndGroup();
    }

    void AudioEventEditorPanel::DrawOutput()
    {
        static const AudioEventData defaults;
        if (!PropertyGrid::BeginGroup("Output"))
            return;
        const auto& data = m_Document.Get();
        std::string bus  = data.Bus;
        if (ComponentEditors::AudioBusRow("Bus", bus, m_Engine, nullptr))
            Edit([bus](AudioEventData& next) { next.Bus = bus; });
        int       priority      = data.Priority;
        const int priorityReset = defaults.Priority;
        if (PropertyGrid::Int("Priority", priority, 0, 255, &priorityReset))
            Edit([priority](AudioEventData& next) { next.Priority = uint8_t(priority); });
        bool loop = data.Loop, spatial = data.Spatial;
        if (PropertyGrid::Checkbox("Loop", loop, &defaults.Loop))
            Edit([loop](AudioEventData& next) { next.Loop = loop; });
        if (PropertyGrid::Checkbox("Spatial", spatial, &defaults.Spatial))
            Edit([spatial](AudioEventData& next) { next.Spatial = spatial; });
        bool send = data.ReverbZoneSend;
        if (PropertyGrid::Row("Reverb zone send",
                              "Reverb zones around the listener send this event to their buses",
                              send != defaults.ReverbZoneSend))
        {
            ImGui::PushID("ReverbZoneSend");
            if (ImGui::Checkbox("##value", &send))
                Edit([send](AudioEventData& next) { next.ReverbZoneSend = send; });
            m_Boxes.ReverbZoneSend = LastItem();
            if (PropertyGrid::RowReset(send != defaults.ReverbZoneSend))
                Edit([](AudioEventData& next) { next.ReverbZoneSend = true; });
            ImGui::PopID();
        }
        float lfe = data.LfeSendDb;
        if (ComponentEditors::AudioSendRow(
                "LFE send", lfe, "Also feed the subwoofer channel (surround outputs) at this level", &m_Boxes.LfeSend))
            Edit([lfe](AudioEventData& next) { next.LfeSendDb = lfe; });
        if (data.Spatial)
        {
            const bool modified = data.AttenuationAsset.IsValid();
            if (PropertyGrid::Row("Attenuation asset",
                                  "A shared .audioattenuation; it replaces the built-in model and, with a cone, the "
                                  "cone below (a source's own asset replaces this one)",
                                  modified))
            {
                ImGui::PushID("AttenuationAsset");
                AssetID asset = data.AttenuationAsset;
                if (AttenuationCombo("##value", asset))
                    Edit([asset](AudioEventData& next) { next.AttenuationAsset = asset; });
                m_Boxes.AttenuationAsset = LastItem();
                if (PropertyGrid::RowReset(modified))
                    Edit([](AudioEventData& next) { next.AttenuationAsset = {}; });
                ImGui::PopID();
            }
            if (modified)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
            const auto& d = defaults.Attenuation;
            auto        a = data.Attenuation;
            const char* models[] {"None", "Inverse", "Linear", "Exponential"};
            int         model = int(a.Attenuation), modelReset = int(d.Attenuation);
            bool        changed = false;
            if (PropertyGrid::Combo("Attenuation", models, 4, model, &modelReset))
                a.Attenuation = AudioAttenuationModel(model), changed = true;
            changed |=
                PropertyGrid::Float("Min distance", a.MinDistance, 0.05f, 0.001f, 100000.0f, "%.2f m", &d.MinDistance);
            changed |=
                PropertyGrid::Float("Max distance", a.MaxDistance, 0.5f, 0.001f, 100000.0f, "%.2f m", &d.MaxDistance);
            if (a.Attenuation != AudioAttenuationModel::None)
                changed |= PropertyGrid::Float("Rolloff", a.Rolloff, 0.01f, 0.0f, 100.0f, "%.2f", &d.Rolloff);
            changed |= PropertyGrid::Float("Doppler", a.DopplerFactor, 0.01f, 0.0f, 10.0f, "%.2f", &d.DopplerFactor);
            changed |=
                PropertyGrid::Float("Cone inner", a.ConeInnerAngle, 0.5f, 0.0f, 360.0f, "%.1f°", &d.ConeInnerAngle);
            changed |=
                PropertyGrid::Float("Cone outer", a.ConeOuterAngle, 0.5f, 0.0f, 360.0f, "%.1f°", &d.ConeOuterAngle);
            changed |=
                PropertyGrid::Float("Cone outer gain", a.ConeOuterGain, 0.01f, 0.0f, 1.0f, "%.2f", &d.ConeOuterGain);
            if (changed)
                Edit([a](AudioEventData& next) { next.Attenuation = a; });
            if (modified)
                ImGui::PopStyleVar();
        }
        PropertyGrid::EndGroup();
    }

    void AudioEventEditorPanel::DrawSpatializer()
    {
        static const AudioEventData defaults;
        const auto&                 data = m_Document.Get();
        // A 2D event has no direction to spatialize and no path to occlude.
        if (!data.Spatial)
            return;
        auto&       boxes   = ComponentEditors::LastAudioSpatialBoxes();
        const bool  live    = m_Engine && m_Engine->IsInitialized();
        const auto  project = live ? m_Engine->GetDefaultSpatializer() : AudioSpatializer::Panning;
        const char* from    = "the project default (Project Settings > Audio)";
        if (PropertyGrid::BeginGroup("Spatializer"))
        {
            auto spatializer = data.Spatializer;
            if (ComponentEditors::AudioSpatializerRow(spatializer, project, from))
                Edit([spatializer](AudioEventData& next) { next.Spatializer = spatializer; });
            const auto resolved = spatializer.value_or(project);
            if (resolved == AudioSpatializer::HRTF)
            {
                float      blend    = data.SpatialBlend;
                const bool modified = blend != defaults.SpatialBlend;
                if (PropertyGrid::Row(
                        "Spatial blend", "0 pans, 1 is fully binaural; values between mix the two", modified))
                {
                    ImGui::PushID("SpatialBlend");
                    if (ImGui::SliderFloat("##value", &blend, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                        Edit([blend](AudioEventData& next) { next.SpatialBlend = std::clamp(blend, 0.0f, 1.0f); });
                    boxes.Blend = LastItem();
                    if (PropertyGrid::RowReset(modified))
                        Edit([](AudioEventData& next) { next.SpatialBlend = 1.0f; });
                    ImGui::PopID();
                }
            }
            if (const char* hint = AudioSpatialEditing::StereoHint(resolved, m_Engine); hint && PropertyGrid::Row(""))
            {
                boxes.StereoHint = hint;
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(EditorTheme::Colors().Warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", hint);
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Occlusion"))
        {
            auto occlusion = data.Occlusion;
            if (ComponentEditors::AudioOcclusionRows(
                    occlusion, nullptr, live ? m_Engine->GetOcclusionDefaults() : AudioOcclusionSettings {}, from))
                Edit([occlusion](AudioEventData& next) { next.Occlusion = occlusion; });
            PropertyGrid::EndGroup();
        }
    }
} // namespace Kans
