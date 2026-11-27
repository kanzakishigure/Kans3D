#include "MusicSetEditorPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Components/ComponentEditors.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/AudioWaveform.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace MusicSetEditing;
    using EditorTheme::Px;

    namespace
    {
        constexpr char  AssetPayload[] = "asset_payload"; // A source file dragged from the Content Browser.
        constexpr float KeyGap         = 1e-3f;           // Curve keys stay this far apart (parameter units).

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

        std::string Stem(const std::string& name) { return std::filesystem::u8path(name).stem().u8string(); }

        // A matrix cell: the sync point, then the fades and bridge on a second line.
        std::string CellText(const MusicTransition& rule)
        {
            const auto summary = RuleSummary(rule);
            const auto split   = summary.find(" \xC2\xB7 ");
            return split == std::string::npos ? summary : summary.substr(0, split) + "\n" + summary.substr(split + 4);
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

        // A button lit while `on`.
        bool Toggle(const char* label, bool on, const char* tooltip, bool enabled = true)
        {
            const auto& p = EditorTheme::Colors();
            if (on)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, p.Selection);
                ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
            }
            const bool clicked = ToolButton(label, tooltip, enabled);
            if (on)
                ImGui::PopStyleColor(2);
            return clicked;
        }

        // Puts the next item on this line when `width` still fits, else on the next.
        void SameLineIfFits(float width)
        {
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + width > right)
                ImGui::NewLine();
        }

        ImU32 Tone(const ImVec4& color, float alpha = 1.0f)
        {
            return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
        }

        constexpr const char* TabNames[MusicSetEditorPanel::TabCount] = {"Segments",
                                                                         "Parameters",
                                                                         "States",
                                                                         "Transitions",
                                                                         "Stingers"};

        bool TabHasIssues(const std::vector<Issue>& issues, int tab)
        {
            return std::any_of(issues.begin(), issues.end(), [&](const Issue& issue) {
                switch (issue.Kind)
                {
                    case Where::Segment:
                    case Where::Layer:
                    case Where::Cue:
                        return tab == 0;
                    case Where::Parameter:
                        return tab == 1;
                    case Where::State:
                    case Where::PlaylistItem:
                        return tab == 2;
                    case Where::Transition:
                        return tab == 3;
                    case Where::Stinger:
                        return tab == 4;
                    case Where::Set:
                        return false;
                }
                return false;
            });
        }

        // The custom cue names of every segment, once each (for rule sync and entry cues).
        std::vector<std::string> AllCueNames(const MusicSetData& data)
        {
            std::vector<std::string> names;
            for (const auto& segment : data.Segments)
                for (const auto& cue : segment.Cues)
                    if (std::find(names.begin(), names.end(), cue.Name) == names.end())
                        names.push_back(cue.Name);
            return names;
        }

        // A dropdown of `names`; `none` (when given) is the empty choice. True when `value` changed.
        bool NameCombo(const char* id, std::string& value, const std::vector<std::string>& names, const char* none)
        {
            const std::string preview = value.empty() && none ? std::string(none) : value;
            bool              changed = false;
            if (ImGui::BeginCombo(id, preview.c_str()))
            {
                if (none && ImGui::Selectable(none, value.empty()) && !value.empty())
                    value.clear(), changed = true;
                for (const auto& name : names)
                    if (ImGui::Selectable(name.c_str(), name == value) && name != value)
                        value = name, changed = true;
                ImGui::EndCombo();
            }
            return changed;
        }

        // A text field that commits once, when the user leaves it after typing; returns the text then.
        struct NameEdit
        {
            ImGuiID     ID = 0;
            std::string Text;
        };
        NameEdit& ActiveName()
        {
            static NameEdit edit;
            return edit;
        }
        std::optional<std::string> NameField(const char* id, const std::string& value, float width)
        {
            auto&         edit = ActiveName();
            const ImGuiID self = ImGui::GetID(id);
            const bool    mine = edit.ID == self;
            char          buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s", mine ? edit.Text.c_str() : value.c_str());
            ImGui::SetNextItemWidth(width);
            ImGui::InputText(id, buffer, sizeof(buffer));
            if (ImGui::IsItemActive())
            {
                edit.ID   = self;
                edit.Text = buffer;
                return std::nullopt;
            }
            if (mine)
                edit.ID = 0;
            if (ImGui::IsItemDeactivatedAfterEdit() && std::string(buffer) != value)
                return std::string(buffer);
            return std::nullopt;
        }
    } // namespace

    // ------------------------------------------------------------------ binding and files

    void MusicSetEditorPanel::Bind(EditorAssetImportService* service, AudioEngine* engine, AssetManager* assets)
    {
        if (service != m_Service)
            m_PeakCache.Clear();
        m_Service = service;
        m_Engine  = engine;
        m_Preview.Bind(engine, assets);
    }

    void MusicSetEditorPanel::SetAssetChoices(const std::vector<EditorAssetChoice>& choices)
    {
        m_Clips.clear();
        for (const auto& choice : choices)
            if (choice.Type == AssetType::AudioClip)
                m_Clips.push_back(choice);
    }

    bool MusicSetEditorPanel::Open(const std::filesystem::path& file)
    {
        auto loaded = LoadMusicSetFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            m_Status = file.filename().u8string() + ": " + error->Message;
            return false;
        }
        const bool other = m_File.empty() || Canonical(m_File) != Canonical(file);
        // Switching files keeps valid edits instead of dropping them.
        if (other && m_Opened && m_Document.IsDirty() && !m_File.empty() && CollectIssues(m_Document.Get()).empty())
            Save();
        if (other)
        {
            m_Preview.Stop();
            m_Segment = m_State = 0;
            m_Curve             = {0, 0};
            m_Cell              = {AnyState, AnyState};
            m_Muted.clear();
            m_Soloed.clear();
        }
        m_Opened = true;
        m_File   = file;
        m_Pending.reset();
        m_Document.Reset(std::get<MusicSetData>(std::move(loaded)));
        m_SeenRevision = UINT64_MAX;
        m_Status.clear();
        return true;
    }

    bool MusicSetEditorPanel::Save()
    {
        if (!m_Opened || m_File.empty())
            return false;
        MusicClipDuration duration;
        if (m_Durations || m_Service)
            duration = [this](AssetID clip) { return DurationOf(clip); };
        const auto issues = CollectIssues(m_Document.Get(), duration);
        if (!issues.empty())
        {
            m_Status = "Not saved: " + issues.front().Message;
            return false;
        }
        if (m_Service && m_Service->IsReadOnly(m_File))
        {
            m_Status = "Plugin content is read-only.";
            return false;
        }
        if (auto error = SaveMusicSetFile(m_File, m_Document.Get()))
        {
            m_Status = error->Message;
            return false;
        }
        m_Document.MarkSaved();
        m_Status = BuildSavedSource(m_Service, m_File);
        return true;
    }

    bool MusicSetEditorPanel::Undo()
    {
        m_Pending.reset();
        return m_Document.Undo();
    }

    bool MusicSetEditorPanel::Redo()
    {
        m_Pending.reset();
        return m_Document.Redo();
    }

    // ------------------------------------------------------------------ clips

    const EditorAssetChoice* MusicSetEditorPanel::FindClip(AssetID clip) const
    {
        for (const auto& choice : m_Clips)
            if (choice.ID == clip)
                return &choice;
        return nullptr;
    }

    AssetID MusicSetEditorPanel::ClipAt(const std::filesystem::path& file) const
    {
        const auto path = Canonical(file);
        for (const auto& choice : m_Clips)
            if (Canonical(choice.SourcePath) == path)
                return choice.ID;
        return {};
    }

    const std::vector<uint8_t>* MusicSetEditorPanel::PeaksOf(AssetID clip)
    {
        if (m_Waveforms)
            return m_Waveforms(clip);
        if (!m_Service || !clip.IsValid())
            return nullptr;
        const auto* summary = m_PeakCache.Find(
            m_Service->GetRuntimeAssetRegistry(), m_Service->GetProductDirectory(), clip.GetSourceID());
        return summary ? &summary->Data.Waveform : nullptr;
    }

    std::optional<double> MusicSetEditorPanel::DurationOf(AssetID clip)
    {
        if (m_Durations)
            return m_Durations(clip);
        if (!m_Service || !clip.IsValid())
            return std::nullopt;
        const auto* summary = m_PeakCache.Find(
            m_Service->GetRuntimeAssetRegistry(), m_Service->GetProductDirectory(), clip.GetSourceID());
        if (!summary || summary->Seconds() <= 0)
            return std::nullopt;
        return summary->Seconds();
    }

    bool MusicSetEditorPanel::ClipCombo(const char* id, AssetID& clip, const char* none)
    {
        const auto* current = FindClip(clip);
        std::string preview = current        ? Stem(current->Name) :
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
                if (ImGui::Selectable(Stem(choice.Name).c_str(), choice.ID == clip))
                {
                    changed = choice.ID != clip;
                    clip    = choice.ID;
                }
                m_Boxes.ClipItems.emplace_back(choice.ID, LastItem());
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

    bool MusicSetEditorPanel::BusCombo(const char* id, std::string& bus)
    {
        return ComponentEditors::AudioBusCombo(id, bus, m_Engine);
    }

    // ------------------------------------------------------------------ edits

    void MusicSetEditorPanel::Edit(const std::function<void(MusicSetData&)>& change)
    {
        auto next = m_Pending ? *m_Pending : m_Document.Get();
        change(next);
        m_PendingContinuous = (m_Pending ? m_PendingContinuous : true) && ImGui::IsAnyItemActive();
        m_Pending           = std::move(next);
    }

    void MusicSetEditorPanel::CommitPending()
    {
        if (!m_Pending)
            return;
        auto data = std::move(*m_Pending);
        m_Pending.reset();
        m_Document.Commit(std::move(data), m_PendingContinuous);
    }

    void MusicSetEditorPanel::RefreshPreviewData()
    {
        // A drag reloads the audition once, when it ends.
        if (ImGui::IsAnyItemActive() && m_Preview.IsPlaying())
            return;
        if (m_Document.GetRevision() == m_SeenRevision && !m_MuteChanged)
            return;
        m_SeenRevision = m_Document.GetRevision();
        m_MuteChanged  = false;
        ++m_PreviewRevision;
        m_PreviewData = m_Document.Get();
        for (size_t s = 0; s < m_PreviewData.Segments.size(); ++s)
        {
            auto&      layers = m_PreviewData.Segments[s].Layers;
            const bool solo   = std::any_of(
                m_Soloed.begin(), m_Soloed.end(), [s](const std::pair<size_t, size_t>& key) { return key.first == s; });
            for (size_t l = 0; l < layers.size(); ++l)
                if (m_Muted.count({s, l}) || (solo && !m_Soloed.count({s, l})))
                    layers[l].VolumeDb = -std::numeric_limits<float>::infinity();
        }
    }

    bool MusicSetEditorPanel::Auditioning() const { return m_Preview.IsPlaying(); }

    void MusicSetEditorPanel::IssueLine(const Issue* issue)
    {
        if (!issue)
            return;
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Danger);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", issue->Message.c_str());
        ImGui::PopStyleColor();
        m_Boxes.ShownIssues.push_back(issue->Message);
    }

    // ------------------------------------------------------------------ drawing

    void MusicSetEditorPanel::Draw()
    {
        m_Focused          = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_Boxes            = {};
        const bool playing = m_PlayMode && m_PlayMode();
        if (playing && m_Preview.IsPlaying())
            m_Preview.Stop();
        if (!m_Opened)
        {
            ImGui::TextDisabled(
                "%s", m_Status.empty() ? "Open a .musicset file from the Content Browser." : m_Status.c_str());
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
        MusicClipDuration duration;
        if (m_Durations || m_Service)
            duration = [this](AssetID clip) { return DurationOf(clip); };
        m_Issues = CollectIssues(m_Document.Get(), duration);
        RefreshPreviewData();
        m_Preview.Update(m_PreviewData, m_PreviewRevision);

        DrawHeader();
        DrawAudition();
        if (ImGui::BeginTabBar("##musicset-tabs"))
        {
            for (int tab = 0; tab < TabCount; ++tab)
            {
                char label[64];
                std::snprintf(label,
                              sizeof(label),
                              "%s%s###%s",
                              TabNames[tab],
                              TabHasIssues(m_Issues, tab) ? " " ICON_FA_TRIANGLE_EXCLAMATION : "",
                              TabNames[tab]);
                const ImGuiTabItemFlags flags =
                    m_SelectTab && int(m_Tab) == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                const bool open   = ImGui::BeginTabItem(label, nullptr, flags);
                m_Boxes.Tabs[tab] = LastItem();
                if (!open)
                    continue;
                if (!m_SelectTab)
                    m_Tab = Tab(tab);
                switch (Tab(tab))
                {
                    case Tab::Segments:
                        DrawSegments();
                        break;
                    case Tab::Parameters:
                        DrawParameters();
                        break;
                    case Tab::States:
                        DrawStates();
                        break;
                    case Tab::Transitions:
                        DrawTransitions();
                        break;
                    case Tab::Stingers:
                        DrawStingers();
                        break;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_SelectTab = false;
        CommitPending();
        if (!ImGui::IsAnyItemActive())
            m_Document.EndGesture();
    }

    void MusicSetEditorPanel::DrawHeader()
    {
        const auto& p    = EditorTheme::Colors();
        std::string name = m_File.filename().u8string();
        if (m_Document.IsDirty())
            name += " *";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.TypeAudio, ICON_FA_MUSIC);
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_File.u8string().c_str());
        ImGui::SameLine();
        if (ToolButton(ICON_FA_FLOPPY_DISK " Save",
                       m_Issues.empty() ? "Write the file and rebuild it (Ctrl+S)" : "Fix the problems shown to save",
                       m_Document.IsDirty() && m_Issues.empty()))
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
        const auto&  data  = m_Document.Get();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (!m_Issues.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            if (m_Issues.size() == 1)
                ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", m_Issues.front().Message.c_str());
            else
                ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %zu problems, first: %s",
                                   m_Issues.size(),
                                   m_Issues.front().Message.c_str());
            ImGui::PopStyleColor();
            m_Boxes.Validation = {start, ImGui::GetItemRectMax()};
        }
        else if (data.Segments.empty())
            ImGui::TextDisabled("An empty music set: add a segment with stems, then a state that plays it.");
    }

    void MusicSetEditorPanel::DrawAudition()
    {
        const auto& p         = EditorTheme::Colors();
        const auto& data      = m_Document.Get();
        const bool  scene     = m_PlayMode && m_PlayMode();
        const bool  available = m_Preview.IsAvailable() && !scene;
        const bool  playable  = available && m_Issues.empty() && !data.States.empty();
        const char* why       = scene                    ? "The scene's audio runs in Play mode" :
                                !m_Preview.IsAvailable() ? "The editor has no audio engine" :
                                !m_Issues.empty()        ? "Fix the problems shown to audition the set" :
                                data.States.empty()      ? "Add a state to play" :
                                                           "Play the set from its first state (or the selected one)";
        if (ToolButton(ICON_FA_PLAY " Play", why, playable))
        {
            std::string state;
            if (m_Tab == Tab::States && m_State < data.States.size())
                state = data.States[m_State].Name;
            if (!m_Preview.Play(m_PreviewData, state))
                m_Status = m_Preview.GetError();
        }
        m_Boxes.Play = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_STOP "##stop", "Stop the audition", m_Preview.IsPlaying()))
            m_Preview.Stop();
        m_Boxes.Stop = LastItem();

        const auto position = m_Preview.GetPosition();
        const auto pending  = m_Preview.GetPendingTransition();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (position)
        {
            m_Boxes.Position = BarBeatLabel(position->Bar, position->BeatInBar) + "  " + std::string(position->State) +
                               " / " + std::string(position->Segment);
            ImGui::TextColored(p.Accent, "%s", m_Boxes.Position.c_str());
            if (pending)
            {
                std::string at = "?";
                for (const auto& segment : m_PreviewData.Segments)
                    if (segment.Name == position->Segment)
                    {
                        const double beat = std::max(
                            0.0, (pending->Seconds - segment.FirstBeatSeconds) / std::max(1e-9, BeatSeconds(segment)));
                        at = BarBeatLabel(uint64_t(beat / std::max(1u, segment.BeatsPerBar)),
                                          std::fmod(beat, double(std::max(1u, segment.BeatsPerBar))));
                    }
                m_Boxes.Pending = ICON_FA_ARROW_RIGHT " " + std::string(pending->State) + " at " + at;
                ImGui::SameLine();
                ImGui::TextColored(p.Warning, "%s", m_Boxes.Pending.c_str());
            }
        }
        else if (!m_Preview.GetError().empty() && !scene)
            ImGui::TextColored(p.Warning, "%s", m_Preview.GetError().c_str());
        else
            ImGui::TextDisabled("%s", scene ? "Play mode" : "Stopped");

        // States, stingers and parameters drive the audition.
        if (!data.States.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("States");
            for (const auto& state : data.States)
            {
                const bool current = position && position->State == state.Name;
                SameLineIfFits(ImGui::CalcTextSize(state.Name.c_str()).x + Px(16.0f));
                ImGui::PushID(state.Name.c_str());
                if (Toggle(state.Name.c_str(),
                           current,
                           "Go to this state through the transition rules",
                           m_Preview.IsPlaying() && !scene))
                    m_Preview.SetState(state.Name);
                m_Boxes.StateButtons.emplace_back(state.Name, LastItem());
                ImGui::PopID();
            }
        }
        if (!data.Stingers.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Stingers");
            for (const auto& stinger : data.Stingers)
            {
                SameLineIfFits(ImGui::CalcTextSize(stinger.Name.c_str()).x + Px(30.0f));
                ImGui::PushID(stinger.Name.c_str());
                const std::string label = ICON_FA_BOLT " " + stinger.Name;
                if (ToolButton(label.c_str(), "Play the stinger at its sync point", m_Preview.IsPlaying() && !scene))
                    m_Preview.TriggerStinger(stinger.Name);
                m_Boxes.StingerButtons.emplace_back(stinger.Name, LastItem());
                ImGui::PopID();
            }
        }
        if (!data.Parameters.empty())
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Parameters");
            for (const auto& parameter : data.Parameters)
            {
                SameLineIfFits(Px(150.0f));
                ImGui::PushID(parameter.Name.c_str());
                float value = m_Preview.GetParameter(parameter.Name).value_or(parameter.Default);
                ImGui::SetNextItemWidth(Px(150.0f));
                const std::string format = parameter.Name + " %.2f";
                ImGui::BeginDisabled(!available);
                if (ImGui::SliderFloat("##live",
                                       &value,
                                       parameter.Min,
                                       std::max(parameter.Max, parameter.Min + 1e-3f),
                                       format.c_str(),
                                       ImGuiSliderFlags_AlwaysClamp))
                    m_Preview.SetParameter(parameter.Name, value);
                ImGui::EndDisabled();
                m_Boxes.ParameterSliders.emplace_back(parameter.Name, LastItem());
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
    }

    // ------------------------------------------------------------------ segments

    void MusicSetEditorPanel::DrawSegments()
    {
        const auto& data   = m_Document.Get();
        const float height = Px(292.0f);
        if (ImGui::BeginChild("##segment-list", {Px(180.0f), height}, ImGuiChildFlags_Borders))
        {
            for (size_t i = 0; i < data.Segments.size(); ++i)
            {
                ImGui::PushID(int(i));
                const bool  broken = std::any_of(m_Issues.begin(), m_Issues.end(), [i](const Issue& issue) {
                    return (issue.Kind == Where::Segment || issue.Kind == Where::Layer || issue.Kind == Where::Cue) &&
                           issue.Index == i;
                });
                std::string label  = data.Segments[i].Name.empty() ? std::string("(unnamed)") : data.Segments[i].Name;
                if (broken)
                    label = ICON_FA_TRIANGLE_EXCLAMATION " " + label;
                if (broken)
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Danger);
                if (ImGui::Selectable(label.c_str(), m_Segment == i))
                    m_Segment = i;
                if (broken)
                    ImGui::PopStyleColor();
                m_Boxes.SegmentRows.push_back(LastItem());
                if (ImGui::BeginPopupContextItem("##segment-menu"))
                {
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove segment"))
                        Edit([i](MusicSetData& next) { RemoveSegment(next, i); });
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (data.Segments.empty())
                ImGui::TextDisabled("No segments");
            if (ImGui::Button(ICON_FA_PLUS " Segment", {-FLT_MIN, 0}))
            {
                m_Segment = data.Segments.size();
                Edit([](MusicSetData& next) { AddSegment(next); });
            }
            m_Boxes.AddSegment = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A new segment (120 BPM, 4/4); add its stems in the timeline");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##segment-fields", {0, height}))
            DrawSegmentFields(m_Segment);
        ImGui::EndChild();
        if (m_Segment < data.Segments.size())
        {
            DrawTimeline(m_Segment);
            DrawCueList(m_Segment);
        }
    }

    void MusicSetEditorPanel::DrawSegmentFields(size_t index)
    {
        const auto& data = m_Document.Get();
        if (index >= data.Segments.size())
        {
            ImGui::TextDisabled("Select or add a segment.");
            return;
        }
        static const MusicSegment defaults;
        const auto&               segment = data.Segments[index];
        if (!PropertyGrid::BeginGroup("Segment"))
            return;
        if (PropertyGrid::Row("Name", "Renaming updates the states, playlists and bridges that play it"))
        {
            if (auto name = NameField("##name", segment.Name, -FLT_MIN))
            {
                MusicSetData probe = data;
                if (RenameSegment(probe, index, *name))
                    Edit([index, name = *name](MusicSetData& next) { RenameSegment(next, index, name); });
                else
                    m_Status = "\"" + *name + "\" is empty or already a segment";
            }
            m_Boxes.Name = LastItem();
        }
        const bool bpmModified = segment.BPM != defaults.BPM;
        if (PropertyGrid::Row("Tempo", "Beats per minute; the ruler, beat and bar syncs follow it", bpmModified))
        {
            ImGui::PushID("BPM");
            float bpm = float(segment.BPM);
            if (ImGui::DragFloat("##value", &bpm, 0.1f, 1.0f, 1000.0f, "%.2f BPM", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, bpm](MusicSetData& next) { next.Segments[index].BPM = bpm; });
            m_Boxes.BPM = LastItem();
            if (PropertyGrid::RowReset(bpmModified))
                Edit([index](MusicSetData& next) { next.Segments[index].BPM = MusicSegment {}.BPM; });
            ImGui::PopID();
        }
        const bool meterModified = segment.BeatsPerBar != defaults.BeatsPerBar;
        if (PropertyGrid::Row("Beats per bar", "The meter: beats in one bar", meterModified))
        {
            ImGui::PushID("Meter");
            int beats = int(segment.BeatsPerBar);
            if (ImGui::DragInt("##value", &beats, 0.05f, 1, 64, "%d / bar", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, beats](MusicSetData& next) { next.Segments[index].BeatsPerBar = uint32_t(beats); });
            m_Boxes.Meter = LastItem();
            if (PropertyGrid::RowReset(meterModified))
                Edit([index](MusicSetData& next) { next.Segments[index].BeatsPerBar = 4; });
            ImGui::PopID();
        }
        const bool firstModified = segment.FirstBeatSeconds != 0;
        if (PropertyGrid::Row("First beat", "Where beat 1 of bar 1 is in the clips (seconds)", firstModified))
        {
            ImGui::PushID("FirstBeat");
            float first = float(segment.FirstBeatSeconds);
            if (ImGui::DragFloat("##value", &first, 0.001f, 0.0f, 600.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, first](MusicSetData& next) { next.Segments[index].FirstBeatSeconds = first; });
            m_Boxes.FirstBeat = LastItem();
            if (PropertyGrid::RowReset(firstModified))
                Edit([index](MusicSetData& next) { next.Segments[index].FirstBeatSeconds = 0; });
            ImGui::PopID();
        }
        const auto flag = [&](const char* label, const char* tooltip, bool value, ImRect& box, auto apply) {
            if (!PropertyGrid::Row(label, tooltip, value))
                return;
            ImGui::PushID(label);
            bool on = value;
            if (ImGui::Checkbox("##value", &on))
                Edit([index, on, apply](MusicSetData& next) { apply(next.Segments[index], on); });
            box = LastItem();
            if (PropertyGrid::RowReset(value))
                Edit([index, apply](MusicSetData& next) { apply(next.Segments[index], false); });
            ImGui::PopID();
        };
        flag("Loop",
             "Loop between the entry and exit cues while the state stays",
             segment.Loop,
             m_Boxes.Loop,
             [](MusicSegment& s, bool on) { s.Loop = on; });
        flag("Pre-entry",
             "Play the pickup before the entry cue, so the entry lands on the sync point",
             segment.PreEntry,
             m_Boxes.PreEntry,
             [](MusicSegment& s, bool on) { s.PreEntry = on; });
        flag("Post-exit",
             "Let the tail after the exit cue ring out over the next segment",
             segment.PostExit,
             m_Boxes.PostExit,
             [](MusicSegment& s, bool on) { s.PostExit = on; });
        if (PropertyGrid::Row("Snap", "Cue drags snap to this grid (hold Alt to move freely)"))
        {
            const char* snaps[] = {"Off", "Beat", "Bar"};
            int         snap    = int(m_Snap);
            if (ImGui::Combo("##snap", &snap, snaps, 3))
                m_Snap = Snap(snap);
            m_Boxes.Snap = LastItem();
        }
        for (const auto& issue : m_Issues)
            if (issue.Kind == Where::Segment && issue.Index == index && PropertyGrid::Row(""))
                IssueLine(&issue);
        PropertyGrid::EndGroup();
    }

    void MusicSetEditorPanel::DrawTimeline(size_t index)
    {
        const auto&       p       = EditorTheme::Colors();
        const auto&       data    = m_Document.Get();
        const auto&       segment = data.Segments[index];
        MusicClipDuration duration;
        if (m_Durations || m_Service)
            duration = [this](AssetID clip) { return DurationOf(clip); };
        // The drawn length: the shortest stem, or (without one) past the cues and at least four bars.
        double length = SegmentLength(segment, duration).value_or(0.0);
        if (length <= 0)
        {
            length = std::max(4.0 * BarSeconds(segment), 1.0);
            length = std::max(length, CueSeconds(segment, {CueKind::Entry}, 0));
            if (segment.Exit)
                length = std::max(length, segment.SecondsAt(*segment.Exit));
            for (const auto& cue : segment.Cues)
                length = std::max(length, segment.SecondsAt(cue.Time));
            if (!std::isfinite(length))
                length = 1.0;
        }

        ImGui::Spacing();
        auto*        draw    = ImGui::GetWindowDrawList();
        const ImVec2 origin  = ImGui::GetCursorScreenPos();
        const float  width   = std::max(ImGui::GetContentRegionAvail().x, Px(400.0f));
        const float  header  = Px(260.0f);
        const float  x0      = origin.x + header;
        const float  x1      = std::max(x0 + Px(80.0f), origin.x + width - Px(6.0f));
        const float  rulerH  = Px(26.0f);
        const float  laneH   = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y + Px(6.0f);
        const size_t lanes   = segment.Layers.size();
        const float  top     = origin.y;
        const float  bottom  = top + rulerH + laneH * float(std::max<size_t>(lanes, 1));
        const double pps     = double(x1 - x0) / length;
        const auto   X       = [&](double seconds) { return x0 + float(std::clamp(seconds, 0.0, length) * pps); };
        const auto   Seconds = [&](float x) { return double(x - x0) / pps; };
        m_Boxes.Ruler        = {{x0, top}, {x1, top + rulerH}};
        m_Boxes.Timeline     = {{x0, top}, {x1, bottom}};

        // Background, ruler and grid.
        draw->AddRectFilled({x0, top}, {x1, bottom}, Tone(p.Inset), Px(3.0f));
        draw->AddRectFilled({x0, top}, {x1, top + rulerH}, Tone(p.Raised));
        for (const auto& tick : RulerTicks(segment, length, pps, Px(9.0f)))
        {
            const float x = X(tick.Seconds);
            if (tick.IsBar)
            {
                draw->AddLine({x, top}, {x, bottom}, Tone(p.TextDim, 0.35f));
                char label[16];
                std::snprintf(label, sizeof(label), "%u", unsigned(tick.Bar + 1));
                draw->AddText({x + Px(3.0f), top + Px(2.0f)}, Tone(p.TextDim), label);
            }
            else
            {
                draw->AddLine({x, top + rulerH * 0.6f}, {x, top + rulerH}, Tone(p.TextDim, 0.6f));
                draw->AddLine({x, top + rulerH}, {x, bottom}, Tone(p.TextDim, 0.12f));
            }
        }
        draw->AddText({origin.x, top + Px(4.0f)},
                      Tone(p.TextDim),
                      (std::to_string(int(std::lround(segment.BPM))) + " BPM  " + std::to_string(segment.BeatsPerBar) +
                       " per bar")
                          .c_str());

        // The tails outside the entry and exit cues: shaded (tinted while they play).
        const double entry = CueSeconds(segment, {CueKind::Entry}, length);
        const double exit  = CueSeconds(segment, {CueKind::Exit}, length);
        if (entry > 0)
        {
            m_Boxes.PreEntryShade = {{x0, top + rulerH}, {X(entry), bottom}};
            draw->AddRectFilled(m_Boxes.PreEntryShade.Min,
                                m_Boxes.PreEntryShade.Max,
                                segment.PreEntry ? Tone(p.Accent, 0.14f) : Tone(ImVec4(0, 0, 0, 1), 0.35f));
            if (m_Boxes.PreEntryShade.GetWidth() > Px(60.0f))
                draw->AddText({x0 + Px(4.0f), bottom - ImGui::GetFontSize() - Px(3.0f)},
                              Tone(p.TextDim),
                              segment.PreEntry ? "pre-entry" : "not played");
        }
        if (exit < length)
        {
            m_Boxes.PostExitShade = {{X(exit), top + rulerH}, {x1, bottom}};
            draw->AddRectFilled(m_Boxes.PostExitShade.Min,
                                m_Boxes.PostExitShade.Max,
                                segment.PostExit ? Tone(p.Accent, 0.14f) : Tone(ImVec4(0, 0, 0, 1), 0.35f));
            if (m_Boxes.PostExitShade.GetWidth() > Px(60.0f))
                draw->AddText({X(exit) + Px(4.0f), bottom - ImGui::GetFontSize() - Px(3.0f)},
                              Tone(p.TextDim),
                              segment.PostExit ? "post-exit" : "not played");
        }

        // The stems: controls on the left, waveform lanes on the right.
        m_Boxes.Lanes.resize(lanes);
        const float frame = ImGui::GetFrameHeight();
        for (size_t l = 0; l < lanes; ++l)
        {
            const auto& layer = segment.Layers[l];
            auto&       box   = m_Boxes.Lanes[l];
            const float y     = top + rulerH + laneH * float(l);
            box.Lane          = {{origin.x, y}, {x1, y + laneH}};
            ImGui::PushID(int(l));
            ImGui::SetCursorScreenPos({origin.x, y + Px(3.0f)});
            ImGui::SetNextItemWidth(header - Px(70.0f));
            AssetID clip = layer.Clip;
            if (ClipCombo("##clip", clip, "(no clip)"))
                Edit([index, l, clip](MusicSetData& next) { next.Segments[index].Layers[l].Clip = clip; });
            box.Clip = LastItem();
            ImGui::SameLine(0.0f, Px(3.0f));
            const bool muted = IsMuted(index, l), soloed = IsSoloed(index, l);
            if (Toggle("M", muted, "Mute this stem in the audition (not saved)"))
            {
                muted ? (void)m_Muted.erase({index, l}) : (void)m_Muted.insert({index, l});
                m_MuteChanged = true;
            }
            box.Mute = LastItem();
            ImGui::SameLine(0.0f, Px(3.0f));
            if (Toggle("S", soloed, "Solo this stem in the audition (not saved)"))
            {
                soloed ? (void)m_Soloed.erase({index, l}) : (void)m_Soloed.insert({index, l});
                m_MuteChanged = true;
            }
            box.Solo = LastItem();
            ImGui::SetCursorScreenPos({origin.x, y + Px(3.0f) + frame + ImGui::GetStyle().ItemSpacing.y});
            ImGui::SetNextItemWidth(Px(110.0f));
            std::string bus = layer.Bus;
            if (BusCombo("##bus", bus))
                Edit([index, l, bus](MusicSetData& next) { next.Segments[index].Layers[l].Bus = bus; });
            box.Bus = LastItem();
            ImGui::SameLine(0.0f, Px(3.0f));
            ImGui::SetNextItemWidth(Px(84.0f));
            float volume = layer.VolumeDb;
            if (ImGui::DragFloat("##volume", &volume, 0.1f, -80.0f, 24.0f, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, l, volume](MusicSetData& next) { next.Segments[index].Layers[l].VolumeDb = volume; });
            box.Volume = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The stem's base level (a parameter curve adds to it)");
            ImGui::SameLine(0.0f, Px(3.0f));
            if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                Edit([index, l](MusicSetData& next) { RemoveLayer(next.Segments[index], l); });
            box.Remove = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove this stem");

            // The waveform over the clip's own length.
            const auto  clipLength = layer.Clip.IsValid() ? DurationOf(layer.Clip) : std::nullopt;
            const float waveEnd    = clipLength ? X(*clipLength) : x1;
            box.Wave               = {{x0 + 1.0f, y + Px(3.0f)}, {std::max(x0 + 2.0f, waveEnd), y + laneH - Px(3.0f)}};
            if (const auto* peaks = PeaksOf(layer.Clip); peaks && !peaks->empty())
            {
                AudioWaveform::Style style;
                style.Background = false;
                AudioWaveform::Draw(*peaks, box.Wave.Min, box.Wave.Max, style);
            }
            if (muted || (!soloed && std::any_of(m_Soloed.begin(), m_Soloed.end(), [index](const auto& key) {
                    return key.first == index;
                })))
                draw->AddRectFilled(box.Wave.Min, box.Wave.Max, Tone(ImVec4(0, 0, 0, 1), 0.45f));
            if (layer.Parameter.size())
                draw->AddText({x1 - ImGui::CalcTextSize(layer.Parameter.c_str()).x - Px(6.0f), y + Px(3.0f)},
                              Tone(p.TextDim),
                              layer.Parameter.c_str());
            if (const auto* issue = FindIssue(m_Issues, Where::Layer, index, l))
            {
                box.Issue = issue->Message;
                m_Boxes.ShownIssues.push_back(issue->Message);
                const std::string text = ICON_FA_TRIANGLE_EXCLAMATION " " + issue->Message;
                draw->AddText(
                    {x0 + Px(6.0f), y + laneH * 0.5f - ImGui::GetFontSize() * 0.5f}, Tone(p.Danger), text.c_str());
            }
            draw->AddLine({origin.x, y + laneH}, {x1, y + laneH}, Tone(p.Line, 0.6f));
            ImGui::PopID();
        }
        if (lanes == 0)
            draw->AddText({x0 + Px(8.0f), top + rulerH + Px(10.0f)},
                          Tone(p.TextDim),
                          "No stems: add one below or drop audio files here");

        // Drops of audio files anywhere on the timeline add stems.
        if (ImGui::BeginDragDropTargetCustom(ImRect({origin.x, top}, {x1, bottom}), ImGui::GetID("##stem-drop")))
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(AssetPayload))
            {
                if (const AssetID dropped = ClipAt(PayloadPath(*payload)); dropped.IsValid())
                    Edit([index, dropped](MusicSetData& next) { AddLayer(next.Segments[index], dropped); });
                else
                    m_Status = "Only imported audio files can be stems.";
            }
            ImGui::EndDragDropTarget();
        }

        // Cues: a line through the lanes and a handle in the ruler, dragged along the grid.
        std::vector<CueRef> cues {{CueKind::Entry}, {CueKind::Exit}};
        for (size_t c = 0; c < segment.Cues.size(); ++c)
            cues.push_back({CueKind::Custom, c});
        const auto& io = ImGui::GetIO();
        for (const auto& cue : cues)
        {
            const double seconds = CueSeconds(segment, cue, length);
            const float  x       = X(seconds);
            const ImVec4 color   = cue.Kind == CueKind::Entry ? p.Success :
                                   cue.Kind == CueKind::Exit  ? p.Danger :
                                                                p.Accent;
            const char*  name    = cue.Kind == CueKind::Entry ? "Entry" :
                                   cue.Kind == CueKind::Exit  ? "Exit" :
                                                                segment.Cues[cue.Index].Name.c_str();
            const bool   issue   = cue.Kind == CueKind::Custom && FindIssue(m_Issues, Where::Cue, index, cue.Index);
            draw->AddLine(
                {x, top}, {x, bottom}, Tone(issue ? p.Danger : color), cue.Kind == CueKind::Custom ? 1.0f : 2.0f);
            const ImRect handle({x - Px(6.0f), top + rulerH * 0.45f}, {x + Px(6.0f), top + rulerH});
            draw->AddTriangleFilled({handle.Min.x, handle.Min.y},
                                    {handle.Max.x, handle.Min.y},
                                    {x, handle.Max.y},
                                    Tone(issue ? p.Danger : color));
            // The label right of the line, or left of it near the end of the timeline.
            const float labelWidth = ImGui::CalcTextSize(name).x;
            const float labelX     = x + Px(7.0f) + labelWidth > x1 ? x - Px(7.0f) - labelWidth : x + Px(7.0f);
            draw->AddText({labelX, top + rulerH * 0.45f - Px(2.0f)}, Tone(color), name);
            ImGui::SetCursorScreenPos(handle.Min);
            const std::string id = cue.Kind == CueKind::Entry ? std::string("##cue-entry") :
                                   cue.Kind == CueKind::Exit  ? std::string("##cue-exit") :
                                                                "##cue-" + std::to_string(cue.Index);
            ImGui::InvisibleButton(id.c_str(), handle.GetSize());
            m_Boxes.Cues.push_back({cue, handle, x});
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
            {
                const double to   = Seconds(io.MousePos.x);
                const Snap   snap = io.KeyAlt ? Snap::Off : m_Snap;
                Edit([index, cue, to, snap, length](MusicSetData& next) {
                    MoveCue(next.Segments[index], cue, to, snap, length);
                });
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            {
                const auto& now  = Current().Segments[index];
                MusicTime   time = cue.Kind == CueKind::Entry ? now.Entry :
                                   cue.Kind == CueKind::Exit  ? now.Exit.value_or(MusicTime {length, {}, 0}) :
                                                                now.Cues[cue.Index].Time;
                ImGui::SetTooltip("%s: %s%s",
                                  name,
                                  TimeLabel(now, time).c_str(),
                                  cue.Kind == CueKind::Custom ? "\nRight-click to remove" : "");
            }
            if (cue.Kind == CueKind::Custom && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                Edit([index, cue](MusicSetData& next) { RemoveCue(next.Segments[index], cue.Index); });
        }
        // Double-click the ruler to add a cue there (on the grid).
        if (ImGui::IsMouseHoveringRect(m_Boxes.Ruler.Min, m_Boxes.Ruler.Max) && !ImGui::IsAnyItemHovered() &&
            ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const auto time = SnapTime(segment, Seconds(io.MousePos.x), io.KeyAlt ? Snap::Off : m_Snap);
            Edit([index, time](MusicSetData& next) { AddCue(next.Segments[index], time); });
        }

        // The audition's playhead and the transition waiting for its sync point.
        if (const auto position = m_Preview.GetPosition(); position && position->Segment == segment.Name)
        {
            const float x = X(position->Seconds);
            draw->AddLine({x, top}, {x, bottom}, Tone(p.Text), 2.0f);
            m_Boxes.Playhead      = x;
            m_Boxes.PlayheadLabel = BarBeatLabel(position->Bar, position->BeatInBar);
            draw->AddRectFilled({x - Px(1.0f), top},
                                {x + ImGui::CalcTextSize(m_Boxes.PlayheadLabel.c_str()).x + Px(6.0f),
                                 top + ImGui::GetFontSize() + Px(2.0f)},
                                Tone(p.Text));
            draw->AddText({x + Px(2.0f), top + Px(1.0f)}, Tone(p.Panel), m_Boxes.PlayheadLabel.c_str());
            if (const auto pending = m_Preview.GetPendingTransition())
            {
                const float marker = X(pending->Seconds);
                for (float y = top; y < bottom; y += Px(8.0f))
                    draw->AddLine({marker, y}, {marker, std::min(bottom, y + Px(4.0f))}, Tone(p.Warning), 2.0f);
                m_Boxes.PendingMarker = marker;
                m_Boxes.PendingLabel  = ICON_FA_ARROW_RIGHT " " + std::string(pending->State);
                draw->AddText({marker + Px(4.0f), bottom - ImGui::GetFontSize() * 2.0f - Px(4.0f)},
                              Tone(p.Warning),
                              m_Boxes.PendingLabel.c_str());
            }
        }

        // Below the lanes: add a stem.
        ImGui::SetCursorScreenPos({origin.x, bottom + Px(4.0f)});
        AssetID added;
        ImGui::SetNextItemWidth(header - Px(8.0f));
        if (ClipCombo("##add-stem", added, ICON_FA_PLUS " Add stem") && added.IsValid())
            Edit([index, added](MusicSetData& next) { AddLayer(next.Segments[index], added); });
        m_Boxes.AddStem = LastItem();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Drag cues along the ruler; double-click it to add one. Length %.2f s", length);
        ImGui::Spacing();
    }

    void MusicSetEditorPanel::DrawCueList(size_t index)
    {
        const auto& data    = m_Document.Get();
        const auto& segment = data.Segments[index];
        if (!PropertyGrid::BeginGroup("Cues"))
            return;
        const auto timeRow = [&](const char* label, const char* tooltip, const MusicTime& time, auto apply) {
            if (!PropertyGrid::Row(label, tooltip))
                return;
            ImGui::PushID(label);
            float seconds = float(segment.SecondsAt(time));
            ImGui::SetNextItemWidth(Px(110.0f));
            if (ImGui::DragFloat("##seconds", &seconds, 0.005f, 0.0f, 3600.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, seconds, apply](MusicSetData& next) { apply(next.Segments[index], seconds); });
            if (time.Bar)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", TimeLabel(segment, time).c_str());
            }
            ImGui::PopID();
        };
        timeRow("Entry cue",
                "Where the segment's music starts (the sync point lands here)",
                segment.Entry,
                [](MusicSegment& s, float seconds) { s.Entry = {seconds, std::nullopt, 0}; });
        if (segment.Exit)
            timeRow("Exit cue",
                    "Where the segment hands over (the loop end when it loops)",
                    *segment.Exit,
                    [](MusicSegment& s, float seconds) { s.Exit = MusicTime {seconds, std::nullopt, 0}; });
        else if (PropertyGrid::Row("Exit cue", "Without one, the segment ends with its shortest stem"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("End of the shortest stem");
            ImGui::SameLine();
            if (ImGui::SmallButton("Set##exit"))
            {
                MusicClipDuration duration;
                if (m_Durations || m_Service)
                    duration = [this](AssetID clip) { return DurationOf(clip); };
                const double end = SegmentLength(segment, duration).value_or(4.0 * BarSeconds(segment));
                Edit(
                    [index, end](MusicSetData& next) { next.Segments[index].Exit = MusicTime {end, std::nullopt, 0}; });
            }
        }
        for (size_t c = 0; c < segment.Cues.size(); ++c)
        {
            const auto& cue = segment.Cues[c];
            ImGui::PushID(int(c));
            if (PropertyGrid::Row("Cue", "A custom cue: rules and stingers can sync to it"))
            {
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (auto name = NameField("##name", cue.Name, Px(120.0f)))
                    Edit([index, c, name = *name](MusicSetData& next) { next.Segments[index].Cues[c].Name = name; });
                ImGui::SameLine();
                float seconds = float(segment.SecondsAt(cue.Time));
                ImGui::SetNextItemWidth(Px(100.0f));
                if (ImGui::DragFloat(
                        "##seconds", &seconds, 0.005f, 0.0f, 3600.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp))
                    Edit([index, c, seconds](MusicSetData& next) {
                        next.Segments[index].Cues[c].Time = {seconds, std::nullopt, 0};
                    });
                if (cue.Time.Bar)
                {
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", TimeLabel(segment, cue.Time).c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    Edit([index, c](MusicSetData& next) { RemoveCue(next.Segments[index], c); });
                m_Boxes.CueRows.push_back({rowStart, ImGui::GetItemRectMax()});
            }
            if (const auto* issue = FindIssue(m_Issues, Where::Cue, index, c); issue && PropertyGrid::Row(""))
                IssueLine(issue);
            ImGui::PopID();
        }
        if (PropertyGrid::Row(""))
        {
            if (ImGui::Button(ICON_FA_PLUS " Cue"))
            {
                // One bar after the last cue (or the entry), on the grid.
                double at = segment.SecondsAt(segment.Entry);
                for (const auto& cue : segment.Cues)
                    at = std::max(at, segment.SecondsAt(cue.Time));
                const auto time = SnapTime(segment, at + BarSeconds(segment), m_Snap);
                Edit([index, time](MusicSetData& next) { AddCue(next.Segments[index], time); });
            }
            m_Boxes.AddCue = LastItem();
        }
        PropertyGrid::EndGroup();
    }

    // ------------------------------------------------------------------ parameters

    void MusicSetEditorPanel::DrawParameters()
    {
        const auto& data  = m_Document.Get();
        const auto  flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##parameters", 7, flags))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
            ImGui::TableSetupColumn("Ramp", ImGuiTableColumnFlags_WidthFixed, Px(76.0f));
            ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, Px(56.0f));
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, Px(26.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < data.Parameters.size(); ++i)
            {
                const auto& parameter = data.Parameters[i];
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (auto name = NameField("##name", parameter.Name, -FLT_MIN))
                {
                    MusicSetData probe = data;
                    if (RenameParameter(probe, i, *name))
                        Edit([i, name = *name](MusicSetData& next) { RenameParameter(next, i, name); });
                    else
                        m_Status = "\"" + *name + "\" is empty or already a parameter";
                }
                const auto field = [&](const char* id, float value, float speed, const char* format, auto apply) {
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat(id, &value, speed, -100000.0f, 100000.0f, format))
                        Edit([i, value, apply](MusicSetData& next) { apply(next.Parameters[i], value); });
                };
                field("##min", parameter.Min, 0.01f, "%.2f", [](MusicParameter& q, float v) { q.Min = v; });
                field("##max", parameter.Max, 0.01f, "%.2f", [](MusicParameter& q, float v) { q.Max = v; });
                field("##default", parameter.Default, 0.01f, "%.2f", [](MusicParameter& q, float v) { q.Default = v; });
                field("##ramp", parameter.RampSeconds, 0.001f, "%.3f s", [](MusicParameter& q, float v) {
                    q.RampSeconds = v;
                });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How long a change takes to reach the stems (1 ms to 1 s)");
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (const auto now = m_Preview.GetParameter(parameter.Name); now && Auditioning())
                    ImGui::Text("%.2f", double(*now));
                else
                    ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    Edit([i](MusicSetData& next) { RemoveParameter(next, i); });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove the parameter (the stems it drove lose their curves)");
                m_Boxes.ParameterRows.push_back({rowStart, ImGui::GetItemRectMax()});
                if (const auto* issue = FindIssue(m_Issues, Where::Parameter, i))
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    IssueLine(issue);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (data.Parameters.empty())
            ImGui::TextDisabled("No parameters: add one to shape stem levels from gameplay.");
        if (ImGui::Button(ICON_FA_PLUS " Parameter"))
            Edit([](MusicSetData& next) { AddParameter(next); });
        m_Boxes.AddParameter = LastItem();
        ImGui::Spacing();
        DrawCurve();
    }

    void MusicSetEditorPanel::DrawCurve()
    {
        const auto& p    = EditorTheme::Colors();
        const auto& data = m_Document.Get();
        if (!PropertyGrid::BeginGroup("Stem volume curve"))
            return;
        std::vector<std::pair<size_t, size_t>> stems;
        for (size_t s = 0; s < data.Segments.size(); ++s)
            for (size_t l = 0; l < data.Segments[s].Layers.size(); ++l)
                stems.emplace_back(s, l);
        const auto stemName = [&](size_t s, size_t l) {
            const auto* clip = FindClip(data.Segments[s].Layers[l].Clip);
            return data.Segments[s].Name + " / " + std::to_string(l + 1) +
                   (clip ? " \xC2\xB7 " + Stem(clip->Name) : std::string());
        };
        if (stems.empty())
        {
            if (PropertyGrid::Row(""))
                ImGui::TextDisabled("Add stems to segments to shape them here.");
            PropertyGrid::EndGroup();
            return;
        }
        if (std::find(stems.begin(), stems.end(), m_Curve) == stems.end())
            m_Curve = stems.front(), m_CurveKey.reset();
        const auto [s, l] = m_Curve;
        const auto& layer = data.Segments[s].Layers[l];
        if (PropertyGrid::Row("Stem", "The stem whose level the curve shapes"))
        {
            if (ImGui::BeginCombo("##stem", stemName(s, l).c_str()))
            {
                for (const auto& stem : stems)
                    if (ImGui::Selectable(stemName(stem.first, stem.second).c_str(), stem == m_Curve))
                        m_Curve = stem, m_CurveKey.reset();
                ImGui::EndCombo();
            }
            m_Boxes.CurveStem = LastItem();
        }
        if (PropertyGrid::Row("Driven by", "The parameter whose value moves along the curve", !layer.Parameter.empty()))
        {
            const std::string preview = layer.Parameter.empty() ? std::string("None") : layer.Parameter;
            if (ImGui::BeginCombo("##driver", preview.c_str()))
            {
                if (ImGui::Selectable("None", layer.Parameter.empty()) && !layer.Parameter.empty())
                    Edit([s = s, l = l](MusicSetData& next) {
                        SetLayerParameter(next, next.Segments[s].Layers[l], {});
                    });
                m_Boxes.DriverItems.emplace_back(-1, LastItem());
                for (size_t i = 0; i < data.Parameters.size(); ++i)
                {
                    const auto& name = data.Parameters[i].Name;
                    if (ImGui::Selectable(name.c_str(), name == layer.Parameter) && name != layer.Parameter)
                        Edit([s = s, l = l, name](MusicSetData& next) {
                            SetLayerParameter(next, next.Segments[s].Layers[l], name);
                        });
                    m_Boxes.DriverItems.emplace_back(int(i), LastItem());
                }
                ImGui::EndCombo();
            }
            m_Boxes.CurveDriver = LastItem();
            if (PropertyGrid::RowReset(!layer.Parameter.empty()))
                Edit([s = s, l = l](MusicSetData& next) { SetLayerParameter(next, next.Segments[s].Layers[l], {}); });
        }
        const MusicParameter* parameter = nullptr;
        for (const auto& candidate : data.Parameters)
            if (candidate.Name == layer.Parameter)
                parameter = &candidate;
        if (!parameter || layer.Curve.empty())
        {
            if (PropertyGrid::Row(""))
                ImGui::TextDisabled("Pick a parameter to shape this stem's level with it.");
            PropertyGrid::EndGroup();
            return;
        }
        PropertyGrid::BeginFree();
        // The graph: the parameter across, the level offset up (-48 to +12 dB).
        constexpr float Low = -48.0f, High = 12.0f;
        const float     lower  = parameter->Min,
                        upper  = parameter->Max > parameter->Min ? parameter->Max : parameter->Min + 1;
        const ImVec2    origin = ImGui::GetCursorScreenPos();
        const ImVec2    size {std::max(Px(200.0f), ImGui::GetContentRegionAvail().x), Px(170.0f)};
        const auto      ToScreen = [&](float input, float db) {
            return ImVec2(origin.x + (input - lower) / (upper - lower) * size.x,
                          origin.y + (High - std::clamp(db, Low, High)) / (High - Low) * size.y);
        };
        const auto FromScreen = [&](ImVec2 at) {
            return ImVec2(lower + (at.x - origin.x) / size.x * (upper - lower),
                          High - (at.y - origin.y) / size.y * (High - Low));
        };
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, Tone(p.Inset), Px(3.0f));
        for (float db = High; db >= Low; db -= 12.0f)
        {
            const float y = ToScreen(lower, db).y;
            draw->AddLine({origin.x, y}, {origin.x + size.x, y}, Tone(p.TextDim, db == 0 ? 0.45f : 0.15f));
            char label[16];
            std::snprintf(label, sizeof(label), "%+.0f dB", double(db));
            const float labelY = db <= Low ? y - ImGui::GetFontSize() - 1.0f : y + 1.0f;
            draw->AddText({origin.x + Px(3.0f), labelY}, Tone(p.TextDim, 0.8f), label);
        }
        // The parameter's range along the bottom.
        {
            char low[64], high[64];
            std::snprintf(low, sizeof(low), "%s %.2f", parameter->Name.c_str(), double(lower));
            std::snprintf(high, sizeof(high), "%.2f", double(upper));
            const float y = origin.y + size.y - ImGui::GetFontSize() - Px(2.0f);
            draw->AddText({origin.x + Px(60.0f), y}, Tone(p.TextDim), low);
            draw->AddText({origin.x + size.x - ImGui::CalcTextSize(high).x - Px(4.0f), y}, Tone(p.TextDim), high);
        }
        constexpr int Points = 96;
        ImVec2        line[Points];
        for (int i = 0; i < Points; ++i)
        {
            const float input = lower + (upper - lower) * float(i) / float(Points - 1);
            line[i]           = ToScreen(input, CurveDb(layer, input));
        }
        draw->AddPolyline(line, Points, Tone(p.TypeAudio), ImDrawFlags_None, 2.0f);
        // The audition's value of the parameter.
        if (const auto now = m_Preview.GetParameter(parameter->Name); now && Auditioning())
        {
            const float x = ToScreen(*now, 0).x;
            draw->AddLine({x, origin.y}, {x, origin.y + size.y}, Tone(p.Warning), 1.5f);
        }
        // The empty plot adds a key on click; keys submitted after it win the pointer.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##plot", size);
        m_Boxes.CurvePlot       = LastItem();
        const bool  plotClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const auto& io          = ImGui::GetIO();
        bool        onKey       = false;
        for (size_t k = 0; k < layer.Curve.size(); ++k)
        {
            const auto&  key    = layer.Curve[k];
            const ImVec2 at     = ToScreen(key.Input, key.VolumeDb);
            const float  radius = Px(5.0f);
            m_Boxes.CurveKeys.push_back(at);
            ImGui::SetCursorScreenPos({at.x - radius, at.y - radius});
            ImGui::PushID(int(k));
            ImGui::InvisibleButton("##key", {radius * 2, radius * 2});
            const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
            onKey |= hovered;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                m_CurveKey = k;
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
            {
                const ImVec2 value = FromScreen(io.MousePos);
                const float  input = std::clamp(value.x, lower, upper);
                const float  db    = std::clamp(value.y, Low, High);
                Edit([s = s, l = l, k, input, db](MusicSetData& next) {
                    MoveKey(next.Segments[s].Layers[l].Curve, k, input, db, KeyGap);
                });
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && layer.Curve.size() > 1)
            {
                Edit([s = s, l = l, k](MusicSetData& next) { RemoveKey(next.Segments[s].Layers[l].Curve, k); });
                m_CurveKey.reset();
            }
            if (hovered && !active)
                ImGui::SetTooltip(
                    "%.2f: %+.1f dB\nDrag to move, right-click to remove", double(key.Input), double(key.VolumeDb));
            const bool selected = m_CurveKey == k;
            draw->AddCircleFilled(at, radius, Tone(selected || hovered ? p.Text : p.TypeAudio));
            ImGui::PopID();
        }
        if (plotClicked && !onKey)
        {
            const ImVec2         value = FromScreen(io.MousePos);
            const MusicVolumeKey key {std::clamp(value.x, lower, upper), std::clamp(value.y, Low, High)};
            Edit([s = s, l = l, key](MusicSetData& next) { InsertKey(next.Segments[s].Layers[l].Curve, key, KeyGap); });
        }
        ImGui::SetCursorScreenPos({origin.x, origin.y + size.y + Px(4.0f)});
        ImGui::Dummy({size.x, 0.0f});
        PropertyGrid::EndFree();
        if (m_CurveKey && *m_CurveKey < layer.Curve.size() && PropertyGrid::Row("Selected key"))
        {
            const size_t k     = *m_CurveKey;
            const auto&  key   = layer.Curve[k];
            float        input = key.Input, db = key.VolumeDb;
            const float  half = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            if (ImGui::DragFloat("##input", &input, 0.01f, lower, upper, "at %.2f"))
                Edit([s = s, l = l, k, input, db](MusicSetData& next) {
                    MoveKey(next.Segments[s].Layers[l].Curve, k, input, db, KeyGap);
                });
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            if (ImGui::DragFloat("##db", &db, 0.1f, Low, High, "%+.1f dB"))
                Edit([s = s, l = l, k, input, db](MusicSetData& next) {
                    MoveKey(next.Segments[s].Layers[l].Curve, k, input, db, KeyGap);
                });
        }
        if (m_CurveKey && *m_CurveKey < layer.Curve.size() &&
            PropertyGrid::Row("Shape", "How the level moves to the next key"))
        {
            const size_t k      = *m_CurveKey;
            const bool   smooth = layer.Curve[k].Interpolation == AudioCurveInterpolation::Smooth;
            if (Toggle("Linear", !smooth, nullptr))
                Edit([s = s, l = l, k](MusicSetData& next) {
                    next.Segments[s].Layers[l].Curve[k].Interpolation = AudioCurveInterpolation::Linear;
                });
            ImGui::SameLine();
            if (Toggle("Smooth", smooth, nullptr))
                Edit([s = s, l = l, k](MusicSetData& next) {
                    next.Segments[s].Layers[l].Curve[k].Interpolation = AudioCurveInterpolation::Smooth;
                });
        }
        if (const auto* issue = FindIssue(m_Issues, Where::Layer, s, l); issue && PropertyGrid::Row(""))
            IssueLine(issue);
        PropertyGrid::EndGroup();
    }

    // ------------------------------------------------------------------ states

    void MusicSetEditorPanel::DrawStates()
    {
        const auto& data     = m_Document.Get();
        const auto  segments = [&] {
            std::vector<std::string> names;
            for (const auto& segment : data.Segments)
                names.push_back(segment.Name);
            return names;
        }();
        const auto flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##states", 6, flags))
        {
            ImGui::TableSetupColumn("##select", ImGuiTableColumnFlags_WidthFixed, Px(22.0f));
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthFixed, Px(96.0f));
            ImGui::TableSetupColumn("Segment / playlist", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("##play", ImGuiTableColumnFlags_WidthFixed, Px(26.0f));
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, Px(26.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < data.States.size(); ++i)
            {
                const auto& state = data.States[i];
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (ImGui::RadioButton("##select", m_State == i))
                    m_State = i;
                ImGui::TableNextColumn();
                if (auto name = NameField("##name", state.Name, -FLT_MIN))
                {
                    MusicSetData probe = data;
                    if (RenameState(probe, i, *name))
                        Edit([i, name = *name](MusicSetData& next) { RenameState(next, i, name); });
                    else
                        m_Status = "\"" + *name + "\" is empty, reserved or already a state";
                }
                if (ImGui::IsItemActivated())
                    m_State = i;
                ImGui::TableNextColumn();
                const bool  playlist = !state.Playlist.empty();
                const char* modes[]  = {"Segment", "Playlist"};
                int         mode     = playlist ? 1 : 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##mode", &mode, modes, 2) && (mode == 1) != playlist)
                {
                    m_State = i;
                    Edit([i, mode](MusicSetData& next) {
                        auto& edited = next.States[i];
                        if (mode == 1)
                        {
                            const auto first = !edited.Segment.empty() ? edited.Segment :
                                               next.Segments.empty()   ? std::string() :
                                                                         next.Segments.front().Name;
                            edited.Playlist  = {{first, 1}};
                            edited.Segment.clear();
                        }
                        else
                        {
                            edited.Segment = edited.Playlist.empty() ? std::string() : edited.Playlist.front().Segment;
                            edited.Playlist.clear();
                        }
                    });
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (!playlist)
                {
                    std::string segment = state.Segment;
                    if (NameCombo("##segment", segment, segments, "(none)"))
                        Edit([i, segment](MusicSetData& next) { next.States[i].Segment = segment; });
                }
                else
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%zu segments, %s%s",
                                        state.Playlist.size(),
                                        OrderName(state.Order),
                                        state.NoImmediateRepeat ? ", no repeats" : "");
                }
                ImGui::TableNextColumn();
                const bool canPlay = m_Preview.IsAvailable() && !(m_PlayMode && m_PlayMode()) && m_Issues.empty();
                ImGui::BeginDisabled(!canPlay);
                if (ImGui::SmallButton(ICON_FA_PLAY "##play"))
                {
                    if (m_Preview.IsPlaying())
                        m_Preview.SetState(state.Name);
                    else if (!m_Preview.Play(m_PreviewData, state.Name))
                        m_Status = m_Preview.GetError();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Audition this state (a transition when the set already plays)");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    Edit([i](MusicSetData& next) { RemoveState(next, i); });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove the state and the transition rules from or to it");
                m_Boxes.StateRows.push_back({rowStart, ImGui::GetItemRectMax()});
                if (const auto* issue = FindIssue(m_Issues, Where::State, i))
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    IssueLine(issue);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (data.States.empty())
            ImGui::TextDisabled("No states: a state plays a segment or a playlist; gameplay switches between them.");
        if (ImGui::Button(ICON_FA_PLUS " State"))
        {
            m_State = data.States.size();
            Edit([](MusicSetData& next) { AddState(next); });
        }
        m_Boxes.AddState = LastItem();

        // The selected state's playlist.
        if (m_State >= data.States.size() || data.States[m_State].Playlist.empty())
            return;
        const auto&       state = data.States[m_State];
        const size_t      index = m_State;
        const std::string title = "Playlist \xC2\xB7 " + state.Name;
        ImGui::Spacing();
        if (!PropertyGrid::BeginGroup(title.c_str()))
            return;
        const char* orders[] = {"Ordered", "Shuffled", "Weighted"};
        int         order = int(state.Order), orderReset = 0;
        if (PropertyGrid::Combo("Order", orders, 3, order, &orderReset))
            Edit([index, order](MusicSetData& next) { next.States[index].Order = MusicPlaylistOrder(order); });
        bool       noRepeat = state.NoImmediateRepeat;
        const bool off      = false;
        if (PropertyGrid::Checkbox("No immediate repeat", noRepeat, &off))
            Edit([index, noRepeat](MusicSetData& next) { next.States[index].NoImmediateRepeat = noRepeat; });
        for (size_t k = 0; k < state.Playlist.size(); ++k)
        {
            const auto& item = state.Playlist[k];
            ImGui::PushID(int(k));
            if (PropertyGrid::Row(("Item " + std::to_string(k + 1)).c_str()))
            {
                const float width = ImGui::CalcItemWidth();
                ImGui::SetNextItemWidth(width * 0.55f);
                std::string segment = item.Segment;
                if (NameCombo("##segment", segment, segments, nullptr))
                    Edit([index, k, segment](MusicSetData& next) { next.States[index].Playlist[k].Segment = segment; });
                ImGui::SameLine();
                ImGui::SetNextItemWidth(width * 0.3f);
                float weight = item.Weight;
                ImGui::BeginDisabled(state.Order != MusicPlaylistOrder::Weighted);
                if (ImGui::DragFloat(
                        "##weight", &weight, 0.01f, 0.0f, 1000.0f, "weight %.2f", ImGuiSliderFlags_AlwaysClamp))
                    Edit([index, k, weight](MusicSetData& next) { next.States[index].Playlist[k].Weight = weight; });
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove") && state.Playlist.size() > 1)
                    Edit([index, k](MusicSetData& next) {
                        auto& list = next.States[index].Playlist;
                        list.erase(list.begin() + std::ptrdiff_t(k));
                    });
            }
            if (const auto* issue = FindIssue(m_Issues, Where::PlaylistItem, index, k); issue && PropertyGrid::Row(""))
                IssueLine(issue);
            ImGui::PopID();
        }
        if (PropertyGrid::Row(""))
        {
            if (ImGui::Button(ICON_FA_PLUS " Item"))
                Edit([index](MusicSetData& next) {
                    next.States[index].Playlist.push_back(
                        {next.Segments.empty() ? std::string() : next.Segments.front().Name, 1});
                });
            m_Boxes.AddPlaylistItem = LastItem();
        }
        PropertyGrid::EndGroup();
    }

    // ------------------------------------------------------------------ transitions

    void MusicSetEditorPanel::DrawTransitions()
    {
        const auto& p      = EditorTheme::Colors();
        const auto& data   = m_Document.Get();
        const auto  states = MatrixStates(data);
        if (std::find(states.begin(), states.end(), m_Cell.first) == states.end() ||
            std::find(states.begin(), states.end(), m_Cell.second) == states.end())
            m_Cell = {AnyState, AnyState};
        if (const auto winner = WinningRule(data, m_Cell.first, m_Cell.second))
            m_Boxes.Winner = Pair {data.Transitions[*winner].From, data.Transitions[*winner].To};
        ImGui::TextDisabled("Rows go from, columns go to. Select a pair: the rule the player uses for it is lit.");
        const int   columns    = int(states.size()) + 1;
        const float cellWidth  = Px(140.0f);
        const float cellHeight = ImGui::GetTextLineHeight() * 2.0f + Px(8.0f);
        // Columns share the width while they fit; past that they keep theirs and the matrix scrolls.
        const bool   scroll = cellWidth * float(states.size()) + Px(110.0f) > ImGui::GetContentRegionAvail().x;
        const auto   flags = ImGuiTableFlags_Borders | (scroll ? ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY : 0);
        const float  rowsHeight = (cellHeight + ImGui::GetStyle().CellPadding.y * 2.0f + 1.0f) * float(states.size()) +
                                  ImGui::GetFrameHeight() + Px(4.0f);
        const ImVec2 size {0.0f, scroll ? std::min(rowsHeight, ImGui::GetContentRegionAvail().y * 0.55f) : 0.0f};
        if (ImGui::BeginTable("##matrix", columns, flags, size))
        {
            if (scroll)
                ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("From \\ To", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
            for (const auto& to : states)
                ImGui::TableSetupColumn(to.c_str(),
                                        scroll ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch,
                                        cellWidth);
            ImGui::TableHeadersRow();
            for (const auto& from : states)
            {
                ImGui::PushID(from.c_str());
                ImGui::TableNextRow(ImGuiTableRowFlags_None, cellHeight);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (from == AnyState)
                    ImGui::TextColored(p.TextDim, "%s", from.c_str());
                else
                    ImGui::TextUnformatted(from.c_str());
                for (const auto& to : states)
                {
                    ImGui::TableNextColumn();
                    ImGui::PushID(to.c_str());
                    const Pair pair {from, to};
                    if (from == to && from != AnyState)
                    {
                        ImGui::TextDisabled("\xE2\x80\x94");
                        ImGui::PopID();
                        continue;
                    }
                    const auto rule     = RuleAt(data, from, to);
                    const bool selected = m_Cell == pair, wins = m_Boxes.Winner == pair;
                    if (wins)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, Tone(p.Accent, 0.35f));
                    const bool  broken = rule && FindIssue(m_Issues, Where::Transition, *rule);
                    std::string text   = rule ? CellText(data.Transitions[*rule]) : std::string("\xC2\xB7");
                    if (broken)
                        text = ICON_FA_TRIANGLE_EXCLAMATION " " + text;
                    if (broken)
                        ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
                    else if (!rule)
                        ImGui::PushStyleColor(ImGuiCol_Text, p.TextOff);
                    if (ImGui::Selectable(
                            (text + "##cell").c_str(), selected, ImGuiSelectableFlags_None, {0, cellHeight - Px(4.0f)}))
                        m_Cell = pair;
                    if (broken || !rule)
                        ImGui::PopStyleColor();
                    m_Boxes.Cells[pair] = LastItem();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s \xE2\x86\x92 %s%s%s",
                                          from.c_str(),
                                          to.c_str(),
                                          rule ? "\n" : "\nNo rule here: select it to add one",
                                          rule ? RuleSummary(data.Transitions[*rule]).c_str() : "");
                    if (wins)
                        ImGui::GetWindowDrawList()->AddRect(
                            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Tone(p.Accent), Px(2.0f), 0, 2.0f);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // Rules naming a state the set does not have have no cell: listed here with their reason.
        for (size_t i = 0; i < data.Transitions.size(); ++i)
        {
            const auto& rule = data.Transitions[i];
            if (std::find(states.begin(), states.end(), rule.From) != states.end() &&
                std::find(states.begin(), states.end(), rule.To) != states.end())
                continue;
            ImGui::PushID(int(i));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(p.Danger, "%s \xE2\x86\x92 %s", rule.From.c_str(), rule.To.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK "##remove-orphan"))
                Edit([i](MusicSetData& next) { RemoveTransition(next, i); });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove this rule");
            IssueLine(FindIssue(m_Issues, Where::Transition, i));
            ImGui::PopID();
        }
        // Which rule the selected pair uses.
        ImGui::AlignTextToFramePadding();
        if (m_Boxes.Winner)
            ImGui::TextColored(p.Accent,
                               "%s \xE2\x86\x92 %s uses the rule %s \xE2\x86\x92 %s",
                               m_Cell.first.c_str(),
                               m_Cell.second.c_str(),
                               m_Boxes.Winner->first.c_str(),
                               m_Boxes.Winner->second.c_str());
        else
            ImGui::TextDisabled("%s \xE2\x86\x92 %s: no rule applies, the state changes at once without fades",
                                m_Cell.first.c_str(),
                                m_Cell.second.c_str());
        DrawRuleInspector();
    }

    void MusicSetEditorPanel::DrawRuleInspector()
    {
        const auto&       data  = m_Document.Get();
        const auto        rule  = RuleAt(data, m_Cell.first, m_Cell.second);
        const std::string title = "Rule " + m_Cell.first + " \xE2\x86\x92 " + m_Cell.second;
        if (!PropertyGrid::BeginGroup(title.c_str()))
            return;
        if (!rule)
        {
            if (PropertyGrid::Row(""))
            {
                const bool same = m_Cell.first == m_Cell.second && m_Cell.first != AnyState;
                ImGui::BeginDisabled(same);
                if (ImGui::Button(ICON_FA_PLUS " Add rule"))
                    Edit([from = m_Cell.first, to = m_Cell.second](MusicSetData& next) {
                        AddTransition(next, from, to);
                    });
                ImGui::EndDisabled();
                m_Boxes.AddRule = LastItem();
            }
            PropertyGrid::EndGroup();
            return;
        }
        const size_t                 index = *rule;
        const auto&                  r     = data.Transitions[index];
        static const MusicTransition defaults;
        if (PropertyGrid::Row(
                "Sync", "When the change lands in the playing segment", r.Sync.Sync != defaults.Sync.Sync))
        {
            if (ImGui::BeginCombo("##sync", SyncName(r.Sync.Sync)))
            {
                for (int s = 0; s <= int(MusicSync::EndOfSegment); ++s)
                {
                    if (ImGui::Selectable(SyncName(MusicSync(s)), int(r.Sync.Sync) == s) && int(r.Sync.Sync) != s)
                        Edit([index, s](MusicSetData& next) { next.Transitions[index].Sync.Sync = MusicSync(s); });
                    m_Boxes.SyncItems.emplace_back(s, LastItem());
                }
                ImGui::EndCombo();
            }
            m_Boxes.Sync = LastItem();
            if (PropertyGrid::RowReset(r.Sync.Sync != defaults.Sync.Sync))
                Edit([index](MusicSetData& next) { next.Transitions[index].Sync.Sync = MusicSync::Immediate; });
        }
        if (r.Sync.Sync == MusicSync::NextNBar &&
            PropertyGrid::Row("Bars", "Land on the next multiple of this many bars"))
        {
            int bars = int(r.Sync.Bars);
            if (ImGui::DragInt("##bars", &bars, 0.05f, 1, 64, "every %d bars", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, bars](MusicSetData& next) { next.Transitions[index].Sync.Bars = uint32_t(bars); });
            m_Boxes.Bars = LastItem();
        }
        if (r.Sync.Sync == MusicSync::NextCue &&
            PropertyGrid::Row("Cue", "The custom cue to land on (any: the next one)"))
        {
            std::string cue = r.Sync.Cue;
            if (NameCombo("##sync-cue", cue, AllCueNames(data), "(any cue)"))
                Edit([index, cue](MusicSetData& next) { next.Transitions[index].Sync.Cue = cue; });
            m_Boxes.SyncCue = LastItem();
        }
        const char* curves[] = {"Linear", "Equal power", "S-curve"};
        const auto  fadeRow  = [&](const char*      label,
                                   const char*      tooltip,
                                   const MusicFade& fade,
                                   ImRect&          secondsBox,
                                   ImRect&          curveBox,
                                   bool             out) {
            if (!PropertyGrid::Row(label, tooltip, fade.Seconds > 0))
                return;
            ImGui::PushID(label);
            const float half    = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            float       seconds = float(fade.Seconds);
            ImGui::SetNextItemWidth(half);
            if (ImGui::DragFloat("##seconds", &seconds, 0.005f, 0.0f, 30.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
                Edit([index, seconds, out](MusicSetData& next) {
                    (out ? next.Transitions[index].FadeOut : next.Transitions[index].FadeIn).Seconds = seconds;
                });
            secondsBox = LastItem();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            int curve = int(fade.Curve);
            if (ImGui::Combo("##curve", &curve, curves, 3))
                Edit([index, curve, out](MusicSetData& next) {
                    (out ? next.Transitions[index].FadeOut : next.Transitions[index].FadeIn).Curve =
                        AudioFadeCurve(curve);
                });
            curveBox = LastItem();
            if (PropertyGrid::RowReset(fade.Seconds > 0))
                Edit([index, out](MusicSetData& next) {
                    (out ? next.Transitions[index].FadeOut : next.Transitions[index].FadeIn) = MusicFade {};
                });
            ImGui::PopID();
        };
        fadeRow("Fade out",
                "How the playing segment fades from the sync point",
                r.FadeOut,
                m_Boxes.FadeOut,
                m_Boxes.FadeOutCurve,
                true);
        fadeRow("Fade in",
                "How the next segment fades in from the sync point",
                r.FadeIn,
                m_Boxes.FadeIn,
                m_Boxes.FadeInCurve,
                false);
        if (PropertyGrid::Row("Destination entry", "Where the next segment starts playing", r.Entry != defaults.Entry))
        {
            const char* entries[] = {"Entry cue", "Same position", "Named cue"};
            int         entry     = int(r.Entry);
            if (ImGui::Combo("##entry", &entry, entries, 3))
                Edit([index, entry](MusicSetData& next) { next.Transitions[index].Entry = MusicEntry(entry); });
            m_Boxes.Entry = LastItem();
            if (PropertyGrid::RowReset(r.Entry != defaults.Entry))
                Edit([index](MusicSetData& next) { next.Transitions[index].Entry = MusicEntry::EntryCue; });
        }
        if (r.Entry == MusicEntry::NamedCue &&
            PropertyGrid::Row("Entry cue", "The destination's custom cue to start from"))
        {
            std::string cue = r.Cue;
            if (NameCombo("##entry-cue", cue, AllCueNames(data), "(pick a cue)"))
                Edit([index, cue](MusicSetData& next) { next.Transitions[index].Cue = cue; });
            m_Boxes.EntryCue = LastItem();
        }
        if (PropertyGrid::Row(
                "Transition segment", "A bridge played between the two (none: straight across)", !r.Via.empty()))
        {
            std::vector<std::string> segments;
            for (const auto& segment : data.Segments)
                segments.push_back(segment.Name);
            std::string via = r.Via;
            if (NameCombo("##via", via, segments, "None"))
                Edit([index, via](MusicSetData& next) { next.Transitions[index].Via = via; });
            m_Boxes.Via = LastItem();
            if (PropertyGrid::RowReset(!r.Via.empty()))
                Edit([index](MusicSetData& next) { next.Transitions[index].Via.clear(); });
        }
        if (const auto* issue = FindIssue(m_Issues, Where::Transition, index); issue && PropertyGrid::Row(""))
            IssueLine(issue);
        if (PropertyGrid::Row(""))
        {
            if (ImGui::Button(ICON_FA_TRASH " Remove rule"))
                Edit([index](MusicSetData& next) { RemoveTransition(next, index); });
            m_Boxes.RemoveRule = LastItem();
        }
        PropertyGrid::EndGroup();
    }

    // ------------------------------------------------------------------ stingers

    void MusicSetEditorPanel::DrawStingers()
    {
        const auto& data  = m_Document.Get();
        const auto  flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##stingers", 7, flags))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Bus", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
            ImGui::TableSetupColumn("Sync", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
            ImGui::TableSetupColumn("Min interval", ImGuiTableColumnFlags_WidthFixed, Px(90.0f));
            ImGui::TableSetupColumn("##play", ImGuiTableColumnFlags_WidthFixed, Px(26.0f));
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, Px(26.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < data.Stingers.size(); ++i)
            {
                const auto& stinger = data.Stingers[i];
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (auto name = NameField("##name", stinger.Name, -FLT_MIN))
                    Edit([i, name = *name](MusicSetData& next) { next.Stingers[i].Name = name; });
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                AssetID clip = stinger.Clip;
                if (ClipCombo("##clip", clip, "(no clip)"))
                    Edit([i, clip](MusicSetData& next) { next.Stingers[i].Clip = clip; });
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                std::string bus = stinger.Bus;
                if (BusCombo("##bus", bus))
                    Edit([i, bus](MusicSetData& next) { next.Stingers[i].Bus = bus; });
                ImGui::TableNextColumn();
                const bool bars = stinger.Sync.Sync == MusicSync::NextNBar;
                ImGui::SetNextItemWidth(bars ? Px(100.0f) : -FLT_MIN);
                if (ImGui::BeginCombo("##sync", SyncName(stinger.Sync.Sync)))
                {
                    for (int s = 0; s <= int(MusicSync::EndOfSegment); ++s)
                        if (ImGui::Selectable(SyncName(MusicSync(s)), int(stinger.Sync.Sync) == s))
                            Edit([i, s](MusicSetData& next) { next.Stingers[i].Sync.Sync = MusicSync(s); });
                    ImGui::EndCombo();
                }
                if (bars)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    int count = int(stinger.Sync.Bars);
                    if (ImGui::DragInt("##bars", &count, 0.05f, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp))
                        Edit([i, count](MusicSetData& next) { next.Stingers[i].Sync.Bars = uint32_t(count); });
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                float interval = float(stinger.MinimumInterval);
                if (ImGui::DragFloat(
                        "##interval", &interval, 0.01f, 0.0f, 600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
                    Edit([i, interval](MusicSetData& next) { next.Stingers[i].MinimumInterval = interval; });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A trigger sooner than this after the last one is ignored");
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!m_Preview.IsPlaying());
                if (ImGui::SmallButton(ICON_FA_BOLT "##play"))
                    m_Preview.TriggerStinger(stinger.Name);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Trigger it in the audition (play the set first)");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    Edit([i](MusicSetData& next) { RemoveStinger(next, i); });
                m_Boxes.StingerRows.push_back({rowStart, ImGui::GetItemRectMax()});
                if (const auto* issue = FindIssue(m_Issues, Where::Stinger, i))
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    IssueLine(issue);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (data.Stingers.empty())
            ImGui::TextDisabled("No stingers: short accents played on the beat over the music.");
        AssetID added;
        ImGui::SetNextItemWidth(Px(220.0f));
        if (ClipCombo("##add-stinger", added, ICON_FA_PLUS " Add stinger") && added.IsValid())
            Edit([added](MusicSetData& next) { AddStinger(next, added); });
        m_Boxes.AddStinger = LastItem();
    }
} // namespace Kans
