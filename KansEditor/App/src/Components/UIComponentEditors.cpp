#include <imgui.h>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/UI/UIComponents.h"
#include "ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans::ComponentEditors
{
    void DrawUIScreen(Entity entity, ComponentUI& ui)
    {
        static const UIScreenComponent defaults;
        auto                           c       = entity.GetComponent<UIScreenComponent>();
        bool                           changed = false;
        if (PropertyGrid::BeginGroup("Screen UI"))
        {
            changed |= ui.AssetPicker("UI", AssetType::UI, c.UI);
            changed |= PropertyGrid::Checkbox("Visible", c.Visible, &defaults.Visible);
            changed |= PropertyGrid::Int("Sort order", c.SortOrder, -1000, 1000, &defaults.SortOrder);
            PropertyGrid::EndGroup();
        }
        if (changed)
            entity.SetComponent<UIScreenComponent>(c);
    }

    void DrawUIWorld(Entity entity, ComponentUI& ui)
    {
        static const UIWorldComponent defaults;
        auto                          c       = entity.GetComponent<UIWorldComponent>();
        bool                          changed = false;
        if (PropertyGrid::BeginGroup("World UI"))
        {
            changed |= ui.AssetPicker("UI", AssetType::UI, c.UI);
            changed |= PropertyGrid::Checkbox("Visible", c.Visible, &defaults.Visible);
            changed |= PropertyGrid::Int("Sort order", c.SortOrder, -1000, 1000, &defaults.SortOrder);
            // The canvas's reference resolution is stretched over this size in metres.
            if (PropertyGrid::Row("Size", nullptr, c.WorldSize != defaults.WorldSize))
            {
                ImGui::SetNextItemWidth(c.WorldSize != defaults.WorldSize ?
                                            ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4 :
                                            -FLT_MIN);
                changed |= ImGui::DragFloat2("##size", &c.WorldSize.x, 0.01f, 0.01f, 1000.0f, "%.2f m");
                if (PropertyGrid::RowReset(c.WorldSize != defaults.WorldSize))
                    c.WorldSize = defaults.WorldSize, changed = true;
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Rendering", nullptr, nullptr, c.Billboard != defaults.Billboard || c.AlwaysOnTop))
        {
            const char* modes[] {"None", "Face camera", "Upright"};
            int         mode        = int(c.Billboard);
            const int   defaultMode = int(defaults.Billboard);
            if (PropertyGrid::Combo("Billboard", modes, 3, mode, &defaultMode))
                c.Billboard = TextBillboard(mode), changed = true;
            changed |= PropertyGrid::Checkbox("Always on top", c.AlwaysOnTop, &defaults.AlwaysOnTop);
            changed |= PropertyGrid::Checkbox("Double sided", c.DoubleSided, &defaults.DoubleSided);
            PropertyGrid::EndGroup();
        }
        if (changed)
            entity.SetComponent<UIWorldComponent>(c);
    }

    void DrawSprite(Entity entity, ComponentUI& ui)
    {
        static const SpriteRendererComponent defaults;
        auto                                 c       = entity.GetComponent<SpriteRendererComponent>();
        bool                                 changed = false;
        if (PropertyGrid::BeginGroup("Sprite"))
        {
            changed |= ui.AssetPicker("Texture", AssetType::Texture, c.Texture);
            changed |= PropertyGrid::Color4("Color", c.Color, &defaults.Color);
            if (PropertyGrid::Row("Flip"))
            {
                changed |= ImGui::Checkbox("X", &c.FlipX);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Y", &c.FlipY);
            }
            changed |= PropertyGrid::Int("Sorting order", c.SortingOrder, -1000, 1000, &defaults.SortingOrder);
            PropertyGrid::EndGroup();
        }
        // The part of the texture shown (atlas cells) and how often it repeats.
        if (PropertyGrid::BeginGroup("Region", nullptr, nullptr, c.UVRect != defaults.UVRect || c.TilingFactor != 1))
        {
            const bool modified = c.UVRect != defaults.UVRect;
            if (PropertyGrid::Row("UV rect", "Min U, min V, max U, max V", modified))
            {
                ImGui::SetNextItemWidth(modified ? ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4 :
                                                   -FLT_MIN);
                changed |= ImGui::DragFloat4("##uv", &c.UVRect.x, 0.001f, 0.0f, 1.0f, "%.3f");
                if (PropertyGrid::RowReset(modified))
                    c.UVRect = defaults.UVRect, changed = true;
            }
            const float tiling = 1;
            changed |= PropertyGrid::Float("Tiling", c.TilingFactor, 0.01f, 0.0f, 100.0f, "%.2f", &tiling);
            PropertyGrid::EndGroup();
        }
        if (changed)
            entity.SetComponent<SpriteRendererComponent>(c);
    }
} // namespace Kans::ComponentEditors
