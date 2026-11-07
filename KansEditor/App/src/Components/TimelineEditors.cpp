#include <imgui.h>
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/Timeline.h"
#include "ComponentEditors.h"
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
        if (changed)
            entity.PatchComponent<TimelinePlayerComponent>([&](TimelinePlayerComponent& player) {
                player.Timeline    = timeline;
                player.PlayOnStart = playOnStart;
                player.Loop        = loop;
                player.Speed       = std::max(0.0f, speed);
                player.WrapMode    = TimelineWrapMode(wrap);
                player.Bindings    = bindings;
            });
    }
} // namespace Kans::ComponentEditors
