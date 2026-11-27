#include "AudioProfilerPanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "Kans3D/Audio/AudioBusEffectFields.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/Audio/AudioSpatialEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace AudioProfiler;
    using EditorTheme::Px;

    namespace
    {
        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        // A toolbar toggle, filled with `on` while active (the mixer's strip buttons).
        bool ToggleButton(const char* label, const char* tooltip, bool active, ImVec4 on)
        {
            const auto& p = EditorTheme::Colors();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? on : p.Inset);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? Mix(on, p.Text, 0.15f) : p.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? Mix(on, p.Text, 0.25f) : p.Pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.06f, 0.06f, 0.07f, 1.0f) : p.TextDim);
            const bool clicked = ImGui::Button(label);
            ImGui::PopStyleColor(4);
            if (tooltip && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        bool SameLayout(const std::vector<AudioBusDefinition>& a, const std::vector<AudioBusDefinition>& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].Name != b[i].Name || a[i].Parent != b[i].Parent || a[i].Effects.size() != b[i].Effects.size())
                    return false;
                for (size_t e = 0; e < a[i].Effects.size(); ++e)
                {
                    const auto& x = a[i].Effects[e];
                    const auto& y = b[i].Effects[e];
                    if (x.Parameters.index() != y.Parameters.index() || x.Enabled != y.Enabled)
                        return false;
                }
            }
            return true;
        }

        ImVec4 StateColor(AudioVoiceState state)
        {
            const auto& p = EditorTheme::Colors();
            switch (state)
            {
                case AudioVoiceState::Playing:
                    return p.Text;
                case AudioVoiceState::Stopping:
                    return p.Warning;
                case AudioVoiceState::Stopped:
                    return p.TextOff;
                default:
                    return p.TextDim;
            }
        }

        ImVec4 EventColor(AudioProfileEventType type)
        {
            const auto& p = EditorTheme::Colors();
            switch (type)
            {
                case AudioProfileEventType::Started:
                case AudioProfileEventType::Revived:
                    return p.Success;
                case AudioProfileEventType::Stolen:
                case AudioProfileEventType::Rejected:
                case AudioProfileEventType::Virtualised:
                    return p.Warning;
                case AudioProfileEventType::Underrun:
                    return p.Danger;
                default:
                    return p.TextDim;
            }
        }

        void TextCell(const char* text, const ImVec4* color = nullptr)
        {
            if (color)
                ImGui::PushStyleColor(ImGuiCol_Text, *color);
            ImGui::TextUnformatted(text);
            if (color)
                ImGui::PopStyleColor();
        }

        // Percent of the budget: green below half, amber up to the budget, red over it.
        ImVec4 LoadColor(double percent)
        {
            const auto& p = EditorTheme::Colors();
            return percent > 100.0 ? p.Danger : percent > 50.0 ? p.Warning : p.Success;
        }

        constexpr const char* TabLabels[]   = {"Voices", "Buses", "Streams & memory", "Event log"};
        constexpr const char* StateLabels[] = {"Real", "Virtual", "Stopping", "Stopped"};
        constexpr const char* StateTips[]   = {"Playing, scheduled, paused or held by the game pause",
                                               "Tracked but not mixed (over the real-voice limit or inaudible)",
                                               "Fading out after a stop or a steal",
                                               "Created but not playing (a rejected start)"};
    } // namespace

    // ------------------------------------------------------------------ binding

    void AudioProfilerPanel::Bind(AudioEngine* engine)
    {
        if (engine == m_Engine)
            return;
        if (m_Engine && m_Recorder.IsRecording())
            m_Recorder.Stop(*m_Engine);
        m_Engine   = engine;
        m_Instance = 0;
        Clear();
        m_Log.Reset();
        m_Voices.clear();
        m_Order.clear();
        m_Nodes.clear();
        m_BusRows.clear();
        m_Buses.clear();
        m_Stats = {};
    }

    void AudioProfilerPanel::SetAssetChoices(const std::vector<EditorAssetChoice>& choices)
    {
        m_AssetNames.clear();
        for (const auto& choice : choices)
            if (choice.Type == AssetType::AudioClip || choice.Type == AssetType::AudioEvent ||
                choice.Type == AssetType::Timeline)
                m_AssetNames[choice.ID] = std::filesystem::u8path(choice.Name).stem().u8string();
    }

    void AudioProfilerPanel::SetRecording(bool record)
    {
        if (!m_Engine || !m_Engine->IsInitialized())
            return;
        if (record)
            m_Recorder.Start(*m_Engine);
        else
            m_Recorder.Stop(*m_Engine);
    }

    void AudioProfilerPanel::Clear()
    {
        m_History.Clear();
        m_Log.Clear();
    }

    // ------------------------------------------------------------------ reading the engine

    void AudioProfilerPanel::Poll()
    {
        auto& engine = *m_Engine;
        m_Now        = ImGui::GetTime();
        if (engine.GetInstanceID() != m_Instance)
        {
            // Init again (a new run): the history belongs to the old one, recording carries over.
            m_Instance = engine.GetInstanceID();
            m_History.Clear();
            m_LastCallbacks = 0;
            m_Recorder.Rebind(engine);
        }
        m_Stats = engine.GetProfileStats();
        if (m_Stats.Callbacks != m_LastCallbacks)
        {
            m_LastCallbacks = m_Stats.Callbacks;
            m_History.Add(m_Now, float(m_Stats.Current.LoadPercent()));
        }
        const auto& voiceStats = engine.GetVoiceStats();
        m_AllocatedVoices      = voiceStats.AllocatedVoices;
        m_StolenThisFrame      = voiceStats.StolenThisFrame;
        m_TotalStolen          = voiceStats.TotalStolen;
        m_Rejected             = voiceStats.RejectedStarts;

        engine.GetProfileNodeSnapshot(m_Nodes);
        if (!SameLayout(m_Buses, engine.GetBuses()))
            m_Buses = engine.GetBuses(); // Only on a layout change: names stay valid while paused.

        engine.GetVoiceSnapshot(m_Snapshot);
        CopyVoices(m_Snapshot, m_Voices);
        for (auto& row : m_Voices)
            ResolveNames(row);

        if (m_Log.Poll(engine) > 0 && m_AutoScroll)
            m_ScrollToNewest = true;
    }

    void AudioProfilerPanel::OwnerText(const AudioVoiceOwner& owner, char* out, size_t size) const
    {
        if (owner.Entity != 0)
        {
            if (Scene* scene = m_Scene ? m_Scene() : nullptr)
                if (Entity entity = scene->GetEntityByUUID(UUID(owner.Entity)))
                {
                    std::snprintf(out, size, "%s", entity.GetComponent<TagComponent>().Tag.c_str());
                    return;
                }
            std::snprintf(out, size, "Entity %" PRIu64, owner.Entity);
            return;
        }
        std::snprintf(out, size, "%s", OwnerKindLabel(owner.Kind));
    }

    void AudioProfilerPanel::AssetText(AssetID clip, AssetID owner, AudioSoundID sound, char* out, size_t size) const
    {
        const auto name = [&](AssetID id, char* buffer, size_t n) {
            const auto found = m_AssetNames.find(id);
            if (found != m_AssetNames.end())
                std::snprintf(buffer, n, "%s", found->second.c_str());
            else
                std::snprintf(buffer, n, "Asset %08" PRIx64, id.GetSourceID().GetUUID() & 0xFFFFFFFFu);
        };
        char first[64], second[64];
        if (owner.IsValid() && clip.IsValid() && owner != clip)
        {
            name(owner, first, sizeof(first));
            name(clip, second, sizeof(second));
            std::snprintf(out, size, "%s / %s", first, second);
        }
        else if (clip.IsValid() || owner.IsValid())
            name(clip.IsValid() ? clip : owner, out, size);
        else if (sound != 0)
            std::snprintf(out, size, "Sound %" PRIu64, sound);
        else
            std::snprintf(out, size, "-");
    }

    void AudioProfilerPanel::ResolveNames(VoiceRow& row) const
    {
        OwnerText(row.Info.Owner, row.Owner, sizeof(row.Owner));
        if (row.Info.Storage == AudioVoiceStorage::Generator && !row.Info.Clip.IsValid() && !row.Info.Sound)
            std::snprintf(row.Asset, sizeof(row.Asset), "Generator");
        else
            AssetText(row.Info.Clip, row.Info.Owner.Asset, row.Info.Sound, row.Asset, sizeof(row.Asset));
        FormatSpatializer(row.Info, row.Spatializer, sizeof(row.Spatializer));
        std::snprintf(row.Occlusion,
                      sizeof(row.Occlusion),
                      row.Info.OcclusionStale ? "%.2f (stale)" : "%.2f",
                      double(row.Info.Acoustics.Occlusion));
        AudioSpatialEditing::FormatBands(row.Info.Acoustics.Transmission, row.Transmission, sizeof(row.Transmission));
    }

    // ------------------------------------------------------------------ drawing

    void AudioProfilerPanel::Draw()
    {
        m_Boxes.Voices.clear();
        m_Boxes.Buses.clear();
        m_Boxes.Streams.clear();
        m_Boxes.Generators.clear();
        m_Boxes.Events.clear();
        m_Boxes.Unavailable = !m_Engine || !m_Engine->IsInitialized();
        if (!m_Boxes.Unavailable && !m_Paused)
            Poll();

        DrawToolbar();
        if (m_Boxes.Unavailable)
        {
            ImGui::TextDisabled("Audio is unavailable: there is no engine to profile.");
            return;
        }
        BuildBusRows(m_Buses, m_Nodes, m_Window, m_BusSort, m_BusAscending, m_BusRows, m_BusScratch);
        FilterAndSort(m_Voices, m_FilterText, m_States, m_VoiceSort, m_VoiceAscending, m_Order);
        DrawLoad();

        if (ImGui::BeginTabBar("##profilerTabs"))
        {
            for (size_t t = 0; t < size_t(Tab::Count); ++t)
            {
                char label[64];
                if (Tab(t) == Tab::Voices)
                    std::snprintf(label, sizeof(label), "Voices (%zu)###voices", m_Voices.size());
                else
                    std::snprintf(label, sizeof(label), "%s###tab%zu", TabLabels[t], t);
                const auto flags = m_ShowTab == Tab(t) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                const bool open  = ImGui::BeginTabItem(label, nullptr, flags);
                m_Boxes.Tabs[t]  = LastItem();
                if (!open)
                    continue;
                m_Tab = Tab(t);
                switch (m_Tab)
                {
                    case Tab::Voices:
                        DrawVoices();
                        break;
                    case Tab::Buses:
                        DrawBuses();
                        break;
                    case Tab::Streams:
                        DrawStreams();
                        break;
                    case Tab::Events:
                        DrawEvents();
                        break;
                    case Tab::Count:
                        break;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_ShowTab = Tab::Count;
    }

    void AudioProfilerPanel::DrawToolbar()
    {
        const auto& p = EditorTheme::Colors();
        ImGui::BeginDisabled(m_Boxes.Unavailable);
        const bool recording = m_Recorder.IsRecording();
        if (ToggleButton(ICON_FA_CIRCLE " Record##record",
                         "Times the mixer callback, every bus and every effect slot while on; stopping restores the "
                         "profiling state it found",
                         recording,
                         p.Danger))
            SetRecording(!recording);
        m_Boxes.Record = LastItem();
        ImGui::SameLine();
        if (ToggleButton(ICON_FA_PAUSE " Pause view##pause",
                         "Freezes what the panel shows; the engine keeps running",
                         m_Paused,
                         p.AccentHover))
            m_Paused = !m_Paused;
        m_Boxes.Pause = LastItem();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH_CAN " Clear##clear"))
            Clear();
        m_Boxes.Clear = LastItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Empties the load graph and the event log");
        ImGui::SameLine(0.0f, Px(16.0f));
        if (ToggleButton("1 s##window1",
                         "Minimum, average and peak over the last second",
                         m_Window == TimeWindow::OneSecond,
                         p.AccentHover))
            m_Window = TimeWindow::OneSecond;
        m_Boxes.OneSecond = LastItem();
        ImGui::SameLine(0.0f, Px(2.0f));
        if (ToggleButton("10 s##window10",
                         "Minimum, average and peak over the last ten seconds",
                         m_Window == TimeWindow::TenSeconds,
                         p.AccentHover))
            m_Window = TimeWindow::TenSeconds;
        m_Boxes.TenSeconds = LastItem();
        ImGui::EndDisabled();

        if (m_Boxes.Unavailable)
            return;
        ImGui::SameLine(0.0f, Px(16.0f));
        ImGui::AlignTextToFramePadding();
        const char* mode = m_Engine->GetMode() == AudioOutputMode::Offline ? "Offline" : "Device";
        if (recording)
            ImGui::TextColored(p.Danger, ICON_FA_CIRCLE_DOT " Recording");
        else if (m_Engine->IsProfilingEnabled())
            ImGui::TextColored(p.Warning, "Profiling on (enabled outside this panel)");
        else
            ImGui::TextDisabled("Profiling off: Record times the mixer, buses and effects");
        ImGui::SameLine(0.0f, Px(16.0f));
        ImGui::TextDisabled("%u Hz, %u channels, %s%s",
                            m_Engine->GetSampleRate(),
                            m_Engine->GetChannels(),
                            mode,
                            m_Paused ? ", view paused" : "");
    }

    void AudioProfilerPanel::DrawLoad()
    {
        const auto& p      = EditorTheme::Colors();
        const auto& window = Pick(m_Stats, m_Window);

        // Numbers: now, then the window's minimum, average and peak, then the overruns.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Mixer load");
        ImGui::SameLine();
        const double now = m_Stats.Callbacks ? m_Stats.Current.LoadPercent() : 0.0;
        ImGui::TextColored(LoadColor(now), "%.1f%%", now);
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::TextDisabled("min");
        ImGui::SameLine();
        ImGui::Text("%.1f%%", window.Minimum);
        ImGui::SameLine();
        ImGui::TextDisabled("avg");
        ImGui::SameLine();
        ImGui::TextColored(LoadColor(window.Average), "%.1f%%", window.Average);
        ImGui::SameLine();
        ImGui::TextDisabled("peak");
        ImGui::SameLine();
        ImGui::TextColored(LoadColor(window.Peak), "%.1f%%", window.Peak);
        ImGui::SameLine(0.0f, Px(14.0f));
        if (m_Stats.Overruns)
            ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION " %" PRIu64 " overruns", m_Stats.Overruns);
        else
            ImGui::TextDisabled("0 overruns");
        m_Boxes.Overruns = LastItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Callbacks that took longer than the audio they produced (audible as dropouts)");
        ImGui::SameLine(0.0f, Px(14.0f));
        char budget[32];
        FormatNanoseconds(m_Stats.Current.BudgetNanoseconds, budget, sizeof(budget));
        ImGui::TextDisabled("%" PRIu64 " callbacks, %" PRIu64 " frames each, budget %s",
                            m_Stats.Callbacks,
                            m_Stats.Current.Frames,
                            budget);
        if (m_Stats.DroppedTimings)
        {
            ImGui::SameLine();
            ImGui::TextColored(p.Warning, "%" PRIu64 " timings dropped", m_Stats.DroppedTimings);
        }

        // Steam Audio's share of the last callback (HRTF and the direct effect), then occlusion.
        {
            char time[32];
            FormatNanoseconds(double(m_Stats.Current.SteamAudioNanoseconds), time, sizeof(time));
            const double share =
                m_Stats.Current.WallNanoseconds ?
                    100.0 * double(m_Stats.Current.SteamAudioNanoseconds) / double(m_Stats.Current.WallNanoseconds) :
                    0.0;
            std::snprintf(m_Boxes.SteamAudioText,
                          sizeof(m_Boxes.SteamAudioText),
                          "Steam Audio %s (%.0f%% of the callback)",
                          time,
                          share);
            ImGui::TextDisabled("%s", m_Boxes.SteamAudioText);
            m_Boxes.SteamAudio = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Time the last callback spent in Steam Audio (binaural HRTF and the occlusion "
                                  "and transmission filters)");
            const auto& o      = m_Stats.Occlusion;
            const auto  budget = m_Engine->GetMaxOcclusionRaysPerFrame();
            std::snprintf(m_Boxes.OcclusionText,
                          sizeof(m_Boxes.OcclusionText),
                          "Occlusion %u / %u rays, %u voices serviced, %u deferred, %" PRIu64 " overruns",
                          o.Rays,
                          budget,
                          o.VoicesServiced,
                          o.DeferredVoices,
                          o.BudgetOverruns);
            ImGui::SameLine(0.0f, Px(14.0f));
            if (o.BudgetOverruns)
                ImGui::TextColored(p.Warning, "%s", m_Boxes.OcclusionText);
            else
                ImGui::TextDisabled("%s", m_Boxes.OcclusionText);
            m_Boxes.Occlusion = LastItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rays cast this frame against Max Occlusion Rays Per Frame, the voices whose "
                                  "occlusion was refreshed, those left for a later frame, and the frames that "
                                  "had to defer work");
        }

        // The graph: the last 1 s or 10 s of callbacks, the budget line at 100%.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  width  = std::max(Px(100.0f), ImGui::GetContentRegionAvail().x);
        const ImRect graph(origin, {origin.x + width, origin.y + Px(96.0f)});
        ImGui::ItemSize(graph);
        ImGui::ItemAdd(graph, ImGui::GetID("##loadGraph"));
        m_Boxes.Graph = graph;
        auto* draw    = ImGui::GetWindowDrawList();
        draw->AddRectFilled(graph.Min, graph.Max, ImGui::GetColorU32(p.Inset), Px(3.0f));

        const double span  = WindowSeconds(m_Window);
        const size_t first = m_History.FirstAtOrAfter(m_Now - span);
        // The scale follows the window's peak (a quiet mixer still fills the graph); the budget line shows once
        // the peak comes near it.
        double top = 0.0;
        for (size_t i = first; i < m_History.Size(); ++i)
            top = std::max(top, double(m_History.At(i).Load));
        top             = std::clamp(top * 1.25, 10.0, 1000.0);
        const float pad = Px(4.0f);
        const auto  yOf = [&](double load) {
            const double t = std::clamp(load / top, 0.0, 1.0);
            return float(graph.Max.y - pad - t * (graph.GetHeight() - 2 * pad));
        };
        for (double line : {0.25, 0.5, 0.75})
            draw->AddLine({graph.Min.x, yOf(top * line)}, {graph.Max.x, yOf(top * line)}, ImGui::GetColorU32(p.Panel));
        char scale[32];
        std::snprintf(scale, sizeof(scale), "%.0f%%", top);
        draw->AddText({graph.Max.x - ImGui::CalcTextSize(scale).x - pad, graph.Min.y + 1.0f},
                      ImGui::GetColorU32(p.TextOff),
                      scale);
        if (top >= 100.0)
        {
            const float budgetY = yOf(100.0);
            draw->AddLine({graph.Min.x, budgetY}, {graph.Max.x, budgetY}, ImGui::GetColorU32(p.Danger), 1.0f);
            const float labelY = budgetY - ImGui::GetFontSize() - 1.0f < graph.Min.y ?
                                     budgetY + 1.0f :
                                     budgetY - ImGui::GetFontSize() - 1.0f;
            draw->AddText({graph.Min.x + pad, labelY}, ImGui::GetColorU32(p.TextDim), "Budget 100%");
        }

        size_t points = 0;
        for (size_t i = first; i < m_History.Size() && points < m_Points.size(); ++i)
        {
            const auto&  sample = m_History.At(i);
            const double age    = m_Now - sample.Time;
            m_Points[points++]  = {float(graph.Max.x - age / span * graph.GetWidth()), yOf(sample.Load)};
        }
        if (points >= 2)
            draw->AddPolyline(m_Points.data(), int(points), ImGui::GetColorU32(p.Accent), ImDrawFlags_None, 1.5f);
        for (size_t i = 0; i < points; ++i)
            if (m_History.At(first + i).Load > 100.0f)
                draw->AddCircleFilled(m_Points[i], Px(2.5f), ImGui::GetColorU32(p.Danger));
        if (points == 0)
        {
            const char*  hint = m_Stats.Callbacks              ? "No callbacks in this window" :
                                m_Engine->IsProfilingEnabled() ? "Waiting for the mixer" :
                                                                 "Record to time the mixer";
            const ImVec2 size = ImGui::CalcTextSize(hint);
            draw->AddText({graph.GetCenter().x - size.x * 0.5f, graph.GetCenter().y - size.y * 0.5f},
                          ImGui::GetColorU32(p.TextDim),
                          hint);
        }
        ImGui::Spacing();
    }

    void AudioProfilerPanel::DrawVoices()
    {
        const auto& p = EditorTheme::Colors();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%u real", m_Stats.RealVoices);
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::Text("%u virtual", m_Stats.VirtualVoices);
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::Text("%u allocated", m_AllocatedVoices);
        ImGui::SameLine(0.0f, Px(14.0f));
        if (m_TotalStolen)
            ImGui::TextColored(
                p.Warning, "%" PRIu64 " stolen (%" PRIu64 " this frame)", m_TotalStolen, m_StolenThisFrame);
        else
            ImGui::TextDisabled("0 stolen");
        ImGui::SameLine();
        if (m_Rejected)
            ImGui::TextColored(p.Warning, "%" PRIu64 " rejected", m_Rejected);
        else
            ImGui::TextDisabled("0 rejected");

        ImGui::SetNextItemWidth(Px(260.0f));
        ImGui::InputTextWithHint(
            "##voiceFilter", ICON_FA_FILTER " Owner, asset, bus or group", m_FilterText, sizeof(m_FilterText));
        m_Boxes.Filter = LastItem();
        for (size_t s = 0; s < 4; ++s)
        {
            ImGui::SameLine(0.0f, s == 0 ? Px(12.0f) : Px(2.0f));
            const auto bit = Bit(StateClass(s));
            char       label[32];
            std::snprintf(label, sizeof(label), "%s##state%zu", StateLabels[s], s);
            if (ToggleButton(label, StateTips[s], (m_States & bit) != 0, p.AccentHover))
                m_States = StateMask(m_States ^ bit);
            m_Boxes.States[s] = LastItem();
        }
        FilterAndSort(m_Voices, m_FilterText, m_States, m_VoiceSort, m_VoiceAscending, m_Order);

        const auto flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                           ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
        if (!ImGui::BeginTable("##voices", int(VoiceColumn::Count), flags))
            return;
        const auto column = [](const char* name, VoiceColumn id, ImGuiTableColumnFlags extra = 0, float w = 0.0f) {
            ImGui::TableSetupColumn(name, extra, w, ImGuiID(id));
        };
        column("Owner", VoiceColumn::Owner, ImGuiTableColumnFlags_WidthStretch, 1.0f);
        column("Asset", VoiceColumn::Asset, ImGuiTableColumnFlags_WidthStretch, 1.3f);
        column("Bus", VoiceColumn::Bus);
        column("State", VoiceColumn::State);
        column("Priority", VoiceColumn::Priority, ImGuiTableColumnFlags_PreferSortDescending);
        column("Group", VoiceColumn::Group);
        column("Audibility",
               VoiceColumn::Audibility,
               ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        column("Gain", VoiceColumn::Gain, ImGuiTableColumnFlags_PreferSortDescending);
        column("Distance", VoiceColumn::Distance);
        column("Listener", VoiceColumn::Listener);
        column("Storage", VoiceColumn::Storage);
        column("Reason", VoiceColumn::Reason);
        column("Spatializer", VoiceColumn::Spatializer);
        column("HRTF fallback", VoiceColumn::Fallback);
        column("Occlusion", VoiceColumn::Occlusion, ImGuiTableColumnFlags_PreferSortDescending);
        column("Transmission (L/M/H)", VoiceColumn::Transmission);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
            if (specs->SpecsDirty)
            {
                if (specs->SpecsCount > 0)
                {
                    m_VoiceSort      = VoiceColumn(specs->Specs[0].ColumnUserID);
                    m_VoiceAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                }
                specs->SpecsDirty = false;
                FilterAndSort(m_Voices, m_FilterText, m_States, m_VoiceSort, m_VoiceAscending, m_Order);
            }

        Scene*           scene = m_Scene ? m_Scene() : nullptr;
        ImGuiListClipper clipper;
        clipper.Begin(int(m_Order.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& row  = m_Voices[m_Order[size_t(i)]];
                const auto& info = row.Info;
                ImGui::PushID(int(info.Voice.Index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected =
                    m_SelectedVoice.Index == info.Voice.Index && m_SelectedVoice.Generation == info.Voice.Generation;
                if (ImGui::Selectable(
                        row.Owner, selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    m_SelectedVoice = info.Voice;
                    if (info.Owner.Entity != 0 && scene && m_Select)
                        if (Entity entity = scene->GetEntityByUUID(UUID(info.Owner.Entity)))
                            m_Select(entity);
                }
                m_Boxes.Voices.push_back({m_Order[size_t(i)], LastItem(), ImGui::GetItemRectMin().x, {}});
                if (ImGui::IsItemHovered())
                {
                    const char* detail = ReasonDetail(info.State, info.Reason);
                    ImGui::SetTooltip("%s voice #%u, %s\n%.2f / %.2f s%s%s",
                                      OwnerKindLabel(info.Owner.Kind),
                                      info.Voice.Index,
                                      StateLabel(info.State),
                                      info.PositionSeconds,
                                      info.LengthSeconds,
                                      detail[0] ? "\n" : "",
                                      detail);
                }
                char text[32];
                ImGui::TableNextColumn();
                TextCell(row.Asset);
                ImGui::TableNextColumn();
                TextCell(row.Bus[0] ? row.Bus : "-");
                ImGui::TableNextColumn();
                const ImVec4 stateColor = StateColor(info.State);
                TextCell(StateLabel(info.State), &stateColor);
                ImGui::TableNextColumn();
                std::snprintf(text, sizeof(text), "%u", unsigned(info.Priority));
                TextCell(text);
                ImGui::TableNextColumn();
                TextCell(row.Group[0] ? row.Group : "-", row.Group[0] ? nullptr : &p.TextOff);
                ImGui::TableNextColumn();
                std::snprintf(text, sizeof(text), "%.3f", double(info.Audibility));
                TextCell(text);
                ImGui::TableNextColumn();
                FormatDb(info.FinalGainDb, text, sizeof(text));
                TextCell(text);
                ImGui::TableNextColumn();
                if (info.Listener >= 0)
                    std::snprintf(text, sizeof(text), "%.1f m", double(info.Distance));
                TextCell(info.Listener >= 0 ? text : "-");
                ImGui::TableNextColumn();
                if (info.Listener >= 0)
                    std::snprintf(text, sizeof(text), "%d", int(info.Listener));
                TextCell(info.Listener >= 0 ? text : "-");
                ImGui::TableNextColumn();
                TextCell(StorageLabel(info.Storage));
                ImGui::TableNextColumn();
                TextCell(ReasonLabel(info.Reason), &p.Warning);
                auto& cells = m_Boxes.Voices.back().Spatial;
                ImGui::TableNextColumn();
                TextCell(row.Spatializer);
                cells[0] = LastItem();
                ImGui::TableNextColumn();
                TextCell(FallbackLabel(info.HRTFFallback), &p.Warning);
                cells[1] = LastItem();
                if (info.HRTFFallback != AudioHRTFFallback::None && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", AudioSpatialEditing::FallbackText(info.HRTFFallback));
                ImGui::TableNextColumn();
                TextCell(row.Occlusion, info.Acoustics.Occlusion > 0 ? &p.Warning : nullptr);
                cells[2] = LastItem();
                ImGui::TableNextColumn();
                TextCell(row.Transmission);
                cells[3] = LastItem();
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void AudioProfilerPanel::DrawBuses()
    {
        const auto& p = EditorTheme::Colors();
        if (!m_Engine->IsProfilingEnabled() && m_Nodes.empty())
            ImGui::TextDisabled("Profiling is off: Record times every bus and effect slot.");
        if (m_Stats.UnprofiledNodes)
            ImGui::TextColored(p.Warning,
                               "%zu buses or effects are past the profiler's node limit and not timed.",
                               m_Stats.UnprofiledNodes);
        const auto flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
        if (!ImGui::BeginTable("##buses", 4, flags))
            return;
        ImGui::TableSetupColumn("Bus / effect", ImGuiTableColumnFlags_WidthStretch, 1.0f, ImGuiID(BusSort::Name));
        ImGui::TableSetupColumn("Current",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                Px(90.0f),
                                ImGuiID(BusSort::Current));
        ImGui::TableSetupColumn(m_Window == TimeWindow::OneSecond ? "Average (1 s)###avg" : "Average (10 s)###avg",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                Px(110.0f),
                                ImGuiID(BusSort::Average));
        ImGui::TableSetupColumn(m_Window == TimeWindow::OneSecond ? "Peak (1 s)###peak" : "Peak (10 s)###peak",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                Px(110.0f),
                                ImGuiID(BusSort::Peak));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
            if (specs->SpecsDirty)
            {
                m_BusSort         = specs->SpecsCount ? BusSort(specs->Specs[0].ColumnUserID) : BusSort::Tree;
                m_BusAscending    = specs->SpecsCount && specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                specs->SpecsDirty = false;
                BuildBusRows(m_Buses, m_Nodes, m_Window, m_BusSort, m_BusAscending, m_BusRows, m_BusScratch);
            }

        for (size_t i = 0; i < m_BusRows.size(); ++i)
        {
            const auto& row = m_BusRows[i];
            if (row.Bus >= m_Buses.size())
                continue;
            const auto& bus = m_Buses[row.Bus];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14.0f) * float(row.Depth));
            const float textX = ImGui::GetCursorScreenPos().x;
            if (row.IsEffect() && row.Slot < bus.Effects.size())
            {
                const auto& effect = bus.Effects[row.Slot];
                const auto* custom = std::get_if<AudioCustomDSP>(&effect.Parameters);
                const auto  type   = AudioEffectTypes[effect.Parameters.index()];
                ImGui::TextColored(effect.Enabled ? p.TextDim : p.TextOff,
                                   "%zu. %.*s%s%s%s",
                                   row.Slot + 1,
                                   int(type.size()),
                                   type.data(),
                                   custom ? " " : "",
                                   custom ? custom->Name.c_str() : "",
                                   effect.Enabled ? "" : " (off)");
            }
            else
                ImGui::TextUnformatted(bus.Name.c_str());
            m_Boxes.Buses.push_back({i, LastItem(), textX});
            char       text[32];
            const auto time = [&](double nanoseconds, bool load) {
                ImGui::TableNextColumn();
                if (!row.Timed)
                {
                    ImGui::TextDisabled("-");
                    return;
                }
                FormatNanoseconds(nanoseconds, text, sizeof(text));
                // Share of the callback budget: a bus near it is what makes the mixer overrun.
                const double budget = m_Stats.Current.BudgetNanoseconds;
                if (load && budget > 0)
                    ImGui::TextColored(LoadColor(100.0 * nanoseconds / budget), "%s", text);
                else
                    ImGui::TextUnformatted(text);
            };
            time(double(row.CurrentNanoseconds), false);
            time(row.Window.Average, true);
            time(row.Window.Peak, true);
        }
        ImGui::EndTable();
    }

    void AudioProfilerPanel::DrawStreams()
    {
        const auto& p = EditorTheme::Colors();
        PropertyGrid::Begin("##audioMemory", Px(150.0f));
        if (PropertyGrid::BeginGroup("Memory"))
        {
            char       text[48];
            const auto row = [&](const char* label, uint64_t bytes, const char* tip) {
                if (!PropertyGrid::Row(label, tip))
                    return;
                FormatBytes(bytes, text, sizeof(text));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(text);
            };
            row("Decoded PCM", m_Stats.DecodedBytes, "Clips decompressed into memory when loaded");
            row("Stream buffers", m_Stats.StreamingBufferBytes, "Ring buffers of streaming voices");
            row("Generator buffers", m_Stats.GeneratorBufferBytes, "Rings that generator sources push PCM into");
            row("Total",
                m_Stats.DecodedBytes + m_Stats.StreamingBufferBytes + m_Stats.GeneratorBufferBytes,
                "Audio sample memory the engine holds");
            if (PropertyGrid::Row("Underruns", "Times a stream or generator had no audio ready (silence was mixed)"))
            {
                ImGui::AlignTextToFramePadding();
                if (m_Stats.Underruns)
                    ImGui::TextColored(
                        p.Danger, "%" PRIu64 " (%" PRIu64 " frames)", m_Stats.Underruns, m_Stats.UnderrunFrames);
                else
                    ImGui::TextDisabled("None");
            }
            PropertyGrid::EndGroup();
        }
        PropertyGrid::End();

        const auto flags   = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX |
                             ImGuiTableFlags_SizingFixedFit;
        size_t     streams = 0, generators = 0;
        for (const auto& row : m_Voices)
        {
            streams += row.Info.Storage == AudioVoiceStorage::Streaming;
            generators += row.Info.Storage == AudioVoiceStorage::Generator;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Streaming voices");
        if (!streams)
            ImGui::TextDisabled("None: every playing clip is decoded in memory.");
        else if (ImGui::BeginTable("##streams", 5, flags))
        {
            ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Buffer", ImGuiTableColumnFlags_WidthFixed, Px(200.0f));
            ImGui::TableSetupColumn("Decode", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
            ImGui::TableSetupColumn("Starvation", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < m_Voices.size(); ++i)
            {
                const auto& row = m_Voices[i];
                if (row.Info.Storage != AudioVoiceStorage::Streaming)
                    continue;
                const auto& s = row.Info.Stream;
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding(); // Level with the fill bar.
                ImGui::TextUnformatted(row.Owner);
                const float textX = ImGui::GetItemRectMin().x;
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(row.Asset);
                ImGui::TableNextColumn();
                const float fill = s.CapacityFrames ? float(double(s.BufferedFrames) / double(s.CapacityFrames)) : 0.0f;
                char        text[64];
                std::snprintf(text, sizeof(text), "%.0f%% of %" PRIu64 " frames", fill * 100.0, s.CapacityFrames);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fill < 0.25f ? p.Danger : p.Accent);
                ImGui::ProgressBar(fill, {-1.0f, 0.0f}, text);
                ImGui::PopStyleColor();
                m_Boxes.Streams.push_back({i, LastItem(), textX});
                ImGui::TableNextColumn();
                char decode[32];
                FormatNanoseconds(s.DecodePasses ? double(s.DecodeNanoseconds) / double(s.DecodePasses) : 0.0,
                                  decode,
                                  sizeof(decode));
                ImGui::AlignTextToFramePadding();
                if (s.DecodePasses)
                    ImGui::Text("%s x %" PRIu64, decode, s.DecodePasses);
                else
                    ImGui::TextDisabled("Not refilled yet");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Average decode time per refill, times the refills so far");
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (s.Underruns)
                    ImGui::TextColored(p.Danger, "%" PRIu64 " (%" PRIu64 " frames)", s.Underruns, s.UnderrunFrames);
                else
                    ImGui::TextDisabled("None");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Generator sources");
        if (!generators)
            ImGui::TextDisabled("None playing.");
        else if (ImGui::BeginTable("##generators", 4, flags))
        {
            ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Buffered", ImGuiTableColumnFlags_WidthFixed, Px(200.0f));
            ImGui::TableSetupColumn("Underruns", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
            ImGui::TableSetupColumn("Bus", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < m_Voices.size(); ++i)
            {
                const auto& row = m_Voices[i];
                if (row.Info.Storage != AudioVoiceStorage::Generator)
                    continue;
                const auto& s = row.Info.Stream;
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding(); // Level with the fill bar.
                ImGui::TextUnformatted(row.Owner);
                m_Boxes.Generators.push_back({i, LastItem(), ImGui::GetItemRectMin().x});
                ImGui::TableNextColumn();
                const float fill = s.CapacityFrames ? float(double(s.BufferedFrames) / double(s.CapacityFrames)) : 0.0f;
                char        text[64];
                std::snprintf(
                    text, sizeof(text), "%" PRIu64 " / %" PRIu64 " frames", s.BufferedFrames, s.CapacityFrames);
                ImGui::ProgressBar(fill, {-1.0f, 0.0f}, text);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (s.Underruns)
                    ImGui::TextColored(p.Danger, "%" PRIu64 " (%" PRIu64 " frames)", s.Underruns, s.UnderrunFrames);
                else
                    ImGui::TextDisabled("None");
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(row.Bus[0] ? row.Bus : "-");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void AudioProfilerPanel::DrawEvents()
    {
        const auto& p = EditorTheme::Colors();
        for (size_t k = 0; k < EventKinds; ++k)
        {
            if (k)
                ImGui::SameLine(0.0f, Px(2.0f));
            const auto bit = Bit(AudioProfileEventType(k));
            char       label[40];
            std::snprintf(label, sizeof(label), "%s##kind%zu", EventLabel(AudioProfileEventType(k)), k);
            if (ToggleButton(label, nullptr, (m_Kinds & bit) != 0, p.AccentHover))
                m_Kinds = EventMask(m_Kinds ^ bit);
            m_Boxes.Kinds[k] = LastItem();
        }
        ImGui::SameLine(0.0f, Px(12.0f));
        if (ToggleButton(
                "Auto-scroll##autoscroll", "Jump to the newest entry when one arrives", m_AutoScroll, p.AccentHover))
            m_AutoScroll = !m_AutoScroll;
        m_Boxes.AutoScroll = LastItem();
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::AlignTextToFramePadding();
        if (m_Log.TotalDropped())
            ImGui::TextColored(p.Warning, "%" PRIu64 " dropped this run", m_Log.TotalDropped());
        else
            ImGui::TextDisabled("%zu entries", m_Log.Size());

        m_EventOrder.clear();
        for (size_t i = 0; i < m_Log.Size(); ++i)
        {
            const auto& entry = m_Log.Newest(i);
            if (entry.Kind != LogKind::Event || (m_Kinds & Bit(entry.Event.Type)))
                m_EventOrder.push_back(uint32_t(i));
        }

        const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##events", 6, flags))
            return;
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Px(80.0f));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Px(100.0f));
        ImGui::TableSetupColumn("Voice", ImGuiTableColumnFlags_WidthFixed, Px(60.0f));
        ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (m_ScrollToNewest)
        {
            ImGui::SetScrollY(0.0f); // Newest first: the top is the newest entry.
            m_ScrollToNewest = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(m_EventOrder.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const size_t index = m_EventOrder[size_t(i)];
                const auto&  entry = m_Log.Newest(index);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (entry.Kind != LogKind::Event)
                {
                    // A gap: the rest of the row says what is missing here.
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           ImGui::GetColorU32(Mix(p.Panel, p.Warning, 0.18f)));
                    ImGui::TextColored(p.Warning, ICON_FA_TRIANGLE_EXCLAMATION);
                    m_Boxes.Events.push_back({index, LastItem(), ImGui::GetItemRectMin().x});
                    ImGui::TableNextColumn();
                    ImGui::TextColored(p.Warning, entry.Kind == LogKind::Dropped ? "Dropped" : "Restart");
                    ImGui::TableSetColumnIndex(3); // The wide columns carry the explanation.
                    if (entry.Kind == LogKind::Dropped)
                    {
                        ImGui::TextColored(p.Warning, "%" PRIu64 " entries dropped here", entry.Dropped);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("The engine keeps its last 256 entries; more happened between two "
                                              "reads (for example while the view was paused).");
                    }
                    else
                        ImGui::TextColored(p.Warning, "Audio engine restarted: a new log starts here");
                    continue;
                }
                const auto& event = entry.Event;
                char        text[96];
                std::snprintf(text, sizeof(text), "%.3f s", event.Seconds);
                ImGui::TextUnformatted(text);
                m_Boxes.Events.push_back({index, LastItem(), ImGui::GetItemRectMin().x});
                ImGui::TableNextColumn();
                const ImVec4 kind = EventColor(event.Type);
                TextCell(EventLabel(event.Type), &kind);
                ImGui::TableNextColumn();
                if (event.Voice.IsValid())
                    std::snprintf(text, sizeof(text), "#%u", event.Voice.Index);
                TextCell(event.Voice.IsValid() ? text : "-");
                ImGui::TableNextColumn();
                OwnerText(event.Owner, text, sizeof(text));
                TextCell(text);
                ImGui::TableNextColumn();
                AssetText(event.Clip, event.Owner.Asset, event.Sound, text, sizeof(text));
                TextCell(text);
                ImGui::TableNextColumn();
                if (event.Type == AudioProfileEventType::Underrun)
                    std::snprintf(
                        text, sizeof(text), "%" PRIu64 " underruns, %" PRIu64 " frames", event.Count, event.Frames);
                else if (event.Type == AudioProfileEventType::DeviceChanged)
                    std::snprintf(text, sizeof(text), "Output revision %" PRIu64, event.Count);
                else
                    std::snprintf(text, sizeof(text), "%s", ReasonLabel(event.Reason));
                TextCell(text);
            }
        ImGui::EndTable();
    }
} // namespace Kans
