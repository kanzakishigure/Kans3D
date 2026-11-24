#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Resource/Texture.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/Timeline.h"
#include "ComponentEditors.h"
#include "KansEditorCore/Video/VideoEditing.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    namespace
    {
        // Target files are looked up by source path; the card re-reads one only when it changes on disk.
        VideoEditing::RenderTextureIndex& Targets()
        {
            static VideoEditing::RenderTextureIndex index;
            return index;
        }

        void WarningRow(const char* label, const std::string& text)
        {
            if (!PropertyGrid::Row(label))
                return;
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Warning);
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted((ICON_FA_TRIANGLE_EXCLAMATION " " + text).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // The target's image, as wide as the card and in the texture's aspect.
        void Thumbnail(ComponentUI& ui, AssetID target)
        {
            const auto texture = ui.TexturePreview(target);
            PropertyGrid::BeginFree();
            if (texture && texture->GetWidth() && texture->GetHeight())
            {
                const float width  = ImGui::GetContentRegionAvail().x;
                const float aspect = float(texture->GetHeight()) / float(texture->GetWidth());
                const float height = std::min(width * aspect, EditorTheme::Px(220.0f));
                const float shown  = height / aspect;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - shown) * 0.5f);
                ImGui::Image(ImTextureID(texture->GetImGuiTextureID()), {shown, height});
            }
            else
                ImGui::TextDisabled(target.IsValid() ? "The target has no image yet." : "Pick or create a target.");
            PropertyGrid::EndFree();
        }

        // Edit mode: a thumbnail, the preview time over the clip and its transport. The preview drives the target,
        // so the viewport's materials show the frame too.
        void DrawPreview(Entity entity, const VideoPlayerComponent& c, const VideoClipInfo* info, ComponentUI& ui)
        {
            Scene* scene   = ui.GetScene();
            auto*  preview = ui.GetVideoPreview();
            Thumbnail(ui, c.Target);
            if (!preview || !scene)
            {
                PropertyGrid::Text("Preview", "Unavailable", true);
                return;
            }
            const bool ready  = info && c.Clip.IsValid() && c.Target.IsValid() && VideoEditing::DecodingAvailable();
            const bool active = preview->IsPreviewing(entity);
            if (PropertyGrid::Row("Time", "Scrub the clip; the target and the viewport follow."))
            {
                ImGui::BeginDisabled(!ready);
                float        time     = float(active ? preview->Time() : 0.0);
                const float  duration = info ? float(info->Duration) : 0.0f;
                const double fps      = VideoEditing::OffsetRate(info);
                char         label[64];
                std::snprintf(label,
                              sizeof(label),
                              "%s  (frame %lld)",
                              VideoEditing::FormatClock(time).c_str(),
                              static_cast<long long>(std::floor(double(time) * fps + 1e-6)));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##previewTime", &time, 0.0f, std::max(duration, 0.001f), label) && ready)
                {
                    if (!active)
                        preview->Begin(*scene, entity, time);
                    else
                        preview->SetTime(time);
                }
                ImGui::EndDisabled();
            }
            if (PropertyGrid::Row("Preview", "Plays the clip in edit mode until stopped, deselected or Play starts."))
            {
                ImGui::BeginDisabled(!ready);
                const bool playing = active && preview->IsPlaying();
                if (ImGui::Button(playing ? ICON_FA_PAUSE " Pause##videoPreview" : ICON_FA_PLAY " Play##videoPreview"))
                {
                    if (playing)
                        preview->Pause();
                    else
                    {
                        if (!active)
                            preview->Begin(*scene, entity, 0.0);
                        preview->Play();
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!active);
                if (ImGui::Button(ICON_FA_STOP " Stop##videoPreview"))
                    preview->End();
                ImGui::EndDisabled();
            }
        }

        // Play mode: where the player is and what it dropped (read-only).
        void DrawRuntime(Entity entity, const VideoClipInfo* info, Scene& scene)
        {
            const auto time = scene.GetVideoFrameTime(entity);
            if (time)
            {
                char text[64];
                std::snprintf(text,
                              sizeof(text),
                              "%s  (frame %lld)",
                              VideoEditing::FormatClock(*time).c_str(),
                              static_cast<long long>(std::floor(*time * VideoEditing::OffsetRate(info) + 1e-6)));
                PropertyGrid::Text("Time", text);
            }
            else
                PropertyGrid::Text("Time", "No frame shown", true);
            PropertyGrid::Text("Dropped frames", std::to_string(scene.GetVideoPlayerStats(entity).DroppedFrames));
        }
    } // namespace

    void DrawVideoPlayer(Entity entity, ComponentUI& ui)
    {
        static const VideoPlayerComponent defaults;
        const auto&                       c       = entity.GetComponent<VideoPlayerComponent>();
        Scene*                            scene   = ui.GetScene();
        auto                              edited  = c;
        bool                              changed = false;
        // The clip's metadata (its size, rate and length) once loaded; the player loads it for playback anyway.
        Ref<const VideoClipAsset> clip;
        std::string               clipStatus;
        if (c.Clip.IsValid() && ui.GetAssets())
        {
            const auto status = ui.GetAssets()->Request<VideoClipAsset>(c.Clip);
            if (status.IsReady())
                clip = status.Asset;
            else if (status.State == AssetLoadState::Failed)
                clipStatus = status.Error ? status.Error->Message : "The clip cannot be loaded";
            else
                clipStatus = "Loading...";
        }
        const VideoClipInfo* info = clip ? &clip->GetData().Info : nullptr;

        if (PropertyGrid::BeginGroup("Video"))
        {
            changed |= ui.AssetPicker("Clip", AssetType::VideoClip, edited.Clip);
            if (info)
                PropertyGrid::Text("Format", VideoEditing::MetadataLine(*info));
            else if (c.Clip.IsValid())
                PropertyGrid::Text("Format", clipStatus.empty() ? "Not loaded" : clipStatus, true);
            changed |= ui.FilteredAssetPicker("Target", AssetType::Texture, edited.Target, [](const auto& source) {
                return Targets().IsVideoTarget(source);
            });
            if (c.Target.IsValid())
            {
                const auto source = ui.AssetSource(c.Target);
                if (!source.empty() && !Targets().IsVideoTarget(source))
                    WarningRow("", "Not a video render texture: set its Update mode to Video, or create a target.");
            }
            PropertyGrid::BeginFree();
            const bool  editMode = !scene || !scene->GetPhysicsWorld();
            const float arrow    = ImGui::GetFrameHeight();
            ImGui::BeginDisabled(!info || !editMode);
            if (ImGui::Button(ICON_FA_PLUS "  Create target##videoTarget",
                              {ImGui::GetContentRegionAvail().x - arrow - ImGui::GetStyle().ItemSpacing.x, 0}))
                ui.CreateVideoTarget(entity, false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A Video render texture the size of the clip, beside the clip, assigned here.");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CHEVRON_DOWN "##videoTargetMenu", {arrow, 0}))
                ImGui::OpenPopup("##videoTargetWhere");
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("##videoTargetWhere"))
            {
                if (ImGui::MenuItem("Beside the clip"))
                    ui.CreateVideoTarget(entity, false);
                if (ImGui::MenuItem("In the Content Browser folder"))
                    ui.CreateVideoTarget(entity, true);
                ImGui::EndPopup();
            }
            PropertyGrid::EndFree();
            if (const auto error = VideoEditing::PlayerError(scene, entity); !error.empty())
                WarningRow("Error", error);
            PropertyGrid::EndGroup();
        }

        if (PropertyGrid::BeginGroup("Playback"))
        {
            changed |= PropertyGrid::Checkbox("Loop", edited.Loop, &defaults.Loop);
            float       speed = float(edited.Speed);
            const float reset = float(defaults.Speed);
            if (PropertyGrid::Float("Speed", speed, 0.01f, 0.0f, 10.0f, "%.2fx", &reset))
                edited.Speed = std::max(0.0f, speed), changed = true;
            changed |= PropertyGrid::Checkbox("Play on start", edited.PlayOnStart, &defaults.PlayOnStart);
            const char* ends[] {"Hold last frame", "Clear"};
            int         end = int(edited.EndBehavior), endReset = int(defaults.EndBehavior);
            if (PropertyGrid::Combo("At the end", ends, 2, end, &endReset))
                edited.EndBehavior = VideoEndBehavior(end), changed = true;
            PropertyGrid::EndGroup();
        }

        // What the video's time follows; folded while the game clock drives it.
        if (PropertyGrid::BeginGroup("Clock", nullptr, nullptr, c.ClockSource != VideoClockSource::Game))
        {
            const char* clocks[] {"Game", "Timeline", "Audio Source"};
            int         clock = int(edited.ClockSource);
            if (PropertyGrid::Combo("Clock source", clocks, 3, clock))
                edited.ClockSource = VideoClockSource(clock), changed = true;
            const bool timeline = edited.ClockSource == VideoClockSource::Timeline;
            const bool audio    = edited.ClockSource == VideoClockSource::AudioSource;
            const auto accept   = [&](Entity e) {
                return timeline ? e.HasComponent<TimelinePlayerComponent>() :
                       audio    ? e.HasComponent<AudioSourceComponent>() :
                                  true;
            };
            ImGui::BeginDisabled(!timeline && !audio);
            uint64_t id = uint64_t(edited.ClockEntity);
            if (EntityField("Clock entity", id, scene, accept, "(this entity)"))
                edited.ClockEntity = UUID(id), changed = true;
            ImGui::EndDisabled();
            if (timeline || audio)
            {
                Entity source = edited.ClockEntity == UUID(0) ? entity :
                                scene                         ? scene->GetEntityByUUID(edited.ClockEntity) :
                                                                Entity {};
                if (!source || !accept(source))
                    WarningRow("Status",
                               timeline ? "No Timeline Player there: the video holds its frame" :
                                          "No Audio Source there: the video holds its frame");
            }
            // Milliseconds, and frames at the clip's rate.
            const double fps          = VideoEditing::OffsetRate(info);
            float        milliseconds = float(edited.Offset * 1000.0);
            if (PropertyGrid::Float("Offset", milliseconds, 0.5f, -600000.0f, 600000.0f, "%.1f ms"))
                edited.Offset = double(milliseconds) / 1000.0, changed = true;
            char rate[96];
            std::snprintf(rate,
                          sizeof(rate),
                          "Offset in whole frames at %s%s; positive shows the video later",
                          VideoEditing::FormatRate(fps).c_str(),
                          info ? "" : " (the clip is not loaded)");
            if (PropertyGrid::Row("Offset (frames)", rate))
            {
                int frames = int(VideoEditing::OffsetFrames(edited.Offset, fps, true));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##offsetFrames", &frames, 1, 10))
                    edited.Offset = VideoEditing::FramesToSeconds(frames, fps), changed = true;
            }
            char exact[64];
            std::snprintf(exact,
                          sizeof(exact),
                          "%.2f frames at %s",
                          VideoEditing::OffsetFrames(edited.Offset, fps),
                          VideoEditing::FormatRate(fps).c_str());
            PropertyGrid::Text("Exact", exact, true);
            PropertyGrid::EndGroup();
        }

        if (scene && scene->GetPhysicsWorld())
        {
            if (PropertyGrid::BeginGroup("Playing"))
            {
                DrawRuntime(entity, info, *scene);
                PropertyGrid::EndGroup();
            }
        }
        else if (PropertyGrid::BeginGroup("Preview"))
        {
            DrawPreview(entity, c, info, ui);
            PropertyGrid::EndGroup();
        }

        if (changed)
            entity.PatchComponent<VideoPlayerComponent>([&](VideoPlayerComponent& player) { player = edited; });
    }

    void DrawVideoStats(const VideoPlayerStats& stats)
    {
        PropertyGrid::CardOptions options;
        options.DefaultOpen = true;
        if (PropertyGrid::BeginCard("Video", ICON_FA_CLAPPERBOARD, "Video", options))
        {
            PropertyGrid::Begin("Video");
            const bool idle = stats.ActivePlayers == 0;
            for (const auto& [label, value] : VideoEditing::StatsRows(stats))
                PropertyGrid::Text(label.c_str(), value, idle);
            PropertyGrid::End();
        }
        PropertyGrid::EndCard();
    }
} // namespace Kans::ComponentEditors
