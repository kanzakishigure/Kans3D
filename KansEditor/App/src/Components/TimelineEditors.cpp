#include <imgui.h>
#include <cmath>
#include <cstdio>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/Timeline.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Panels/TimelineAudioEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    void DrawTimelinePlayer(Entity entity, ComponentUI& ui)
    {
        const auto& c           = entity.GetComponent<TimelinePlayerComponent>();
        auto        timeline    = c.Timeline;
        bool        playOnStart = c.PlayOnStart, loop = c.Loop, changed = false;
        float       speed    = float(c.Speed);
        int         wrap     = int(c.WrapMode);
        auto        bindings = c.Bindings;
        int         clock    = int(c.ClockSource);
        size_t      track    = c.AudioClockTrack;
        double      offset   = c.AudioOffset;
        if (PropertyGrid::BeginGroup("Timeline"))
        {
            changed |= ui.AssetPicker("Asset", AssetType::Timeline, timeline);
            changed |= PropertyGrid::Checkbox("Play on start", playOnStart);
            changed |= PropertyGrid::Checkbox("Loop", loop);
            changed |= PropertyGrid::Float("Speed", speed, 0.01f, 0.0f, 10.0f, "%.2fx");
            const char* modes[] {"Hold last frame", "Reset"};
            changed |= PropertyGrid::Combo("At the end", modes, 2, wrap);
            PropertyGrid::BeginFree();
            if (ImGui::Button(ICON_FA_FILM "  Open in Timeline", {-FLT_MIN, 0}))
                ui.OpenTimeline(entity);
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        // Which scene entity each of the timeline's bindings drives (this player only).
        if (PropertyGrid::BeginGroup(
                ("Bindings (" + std::to_string(bindings.size()) + ")").c_str(), nullptr, nullptr, !bindings.empty()))
        {
            for (auto& [id, target] : bindings)
            {
                uint64_t value = uint64_t(target);
                if (EntityField(id.c_str(), value, ui.GetScene()))
                    target = UUID(value), changed = true;
            }
            if (bindings.empty())
                PropertyGrid::Text("Bindings", "Added in the Timeline window", true);
            PropertyGrid::EndGroup();
        }
        // Audio as the clock (the timeline follows a track's playback) and the sync offset.
        Ref<const TimelineAsset> source = c.Source;
        if (!source && timeline.IsValid() && ui.GetAssets())
            source = ui.GetAssets()->GetLoaded<TimelineAsset>(timeline);
        const TimelineData* data = source ? &source->GetData() : nullptr;
        const double        fps  = data && data->FrameRate > 0 ? data->FrameRate : 30.0;
        if (PropertyGrid::BeginGroup("Clock", nullptr, nullptr, true))
        {
            const char* clocks[] {"Timeline", "Audio"};
            changed |= PropertyGrid::Combo("Clock source", clocks, 2, clock);
            ImGui::BeginDisabled(clock == int(TimelineClockSource::Timeline));
            if (PropertyGrid::Row("Audio clock track", "The audio track whose playback drives the timeline"))
            {
                const auto name = [&](size_t index) {
                    std::string label = "Track " + std::to_string(index + 1);
                    if (data && index < data->Audio.size())
                    {
                        const auto& t = data->Audio[index];
                        label += "  " + (t.Bus.empty() ? std::string(MasterAudioBus) : t.Bus);
                        label += t.Binding.empty() ? "  (2D)" : "  (" + t.Binding + ")";
                    }
                    else if (data)
                        label += "  (missing)";
                    return label;
                };
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##clockTrack", name(track).c_str()))
                {
                    if (data)
                        for (size_t i = 0; i < data->Audio.size(); ++i)
                            if (ImGui::Selectable(name(i).c_str(), i == track) && i != track)
                                track = i, changed = true;
                    if (!data || data->Audio.empty())
                        ImGui::TextDisabled("The timeline has no audio tracks.");
                    ImGui::EndCombo();
                }
            }
            ImGui::EndDisabled();
            // Milliseconds, and whole frames at the timeline's rate (MMD releases give the offset in frames).
            float milliseconds = float(TimelineAudioEditing::Milliseconds(offset));
            if (PropertyGrid::Float("Audio offset", milliseconds, 0.5f, -10000.0f, 10000.0f, "%.1f ms"))
                offset = double(milliseconds) / 1000.0, changed = true;
            char rate[48];
            std::snprintf(rate, sizeof(rate), "Offset in frames at %g fps; positive plays the audio later", fps);
            if (PropertyGrid::Row("Offset (frames)", rate))
            {
                int frames = int(TimelineAudioEditing::OffsetFrames(offset, fps, true));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##offsetFrames", &frames, 1, 10))
                    offset = TimelineAudioEditing::FramesToSeconds(frames, fps), changed = true;
            }
            char readout[64];
            std::snprintf(readout,
                          sizeof(readout),
                          "%.2f frames at %g fps",
                          TimelineAudioEditing::OffsetFrames(offset, fps),
                          fps);
            PropertyGrid::Text("Exact", readout, true);
            // How far the heard audio is from the timeline, while it plays.
            if (c.Player && c.Player->IsPlaying())
            {
                const auto stats = c.Player->GetAudioStats();
                char       drift[64];
                std::snprintf(drift,
                              sizeof(drift),
                              "%+.1f ms  (%zu voices)",
                              TimelineAudioEditing::Milliseconds(stats.DriftSeconds),
                              stats.ActiveVoices);
                PropertyGrid::Text("Drift", drift);
            }
            else
                PropertyGrid::Text("Drift", "Shown while playing", true);
            PropertyGrid::EndGroup();
        }
        if (changed)
            entity.PatchComponent<TimelinePlayerComponent>([&](TimelinePlayerComponent& player) {
                player.Timeline    = timeline;
                player.PlayOnStart = playOnStart;
                player.Loop        = loop;
                player.Speed       = std::max(0.0f, speed);
                player.WrapMode    = TimelineWrapMode(wrap);
                player.Bindings    = bindings;
                player.ClockSource     = TimelineClockSource(clock);
                player.AudioClockTrack = track;
                player.AudioOffset     = offset;
            });
    }
} // namespace Kans::ComponentEditors
