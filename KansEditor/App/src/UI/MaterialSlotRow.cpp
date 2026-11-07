#include "MaterialSlotRow.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Resource/Texture.h"
#include "EditorIcons.h"
#include "Widgets.h"

namespace Kans::Widgets
{
    bool MaterialSlotRow(ComponentUI&                 ui,
                         const std::string&           title,
                         const std::string&           detail,
                         const SlotParts&             parts,
                         AssetID                      imported,
                         AssetID                      current,
                         AssetID&                     chosen,
                         const std::function<void()>& remove)
    {
        const auto&   style    = ImGui::GetStyle();
        const auto&   colors   = EditorTheme::Colors();
        const float   tool     = EditorTheme::Px(26.0f);
        const float   side     = tool + style.ItemSpacing.y + ImGui::GetFrameHeight();
        const bool    isolated = ui.IsSlotIsolated(parts), highlighted = ui.IsSlotHighlighted(parts);
        const AssetID browser = ui.BrowserMaterial();
        bool          picked  = false;
        const float   pad     = EditorTheme::Px(3.0f);
        ImDrawList*   draw    = ImGui::GetWindowDrawList();
        // The gap comes first, so the row itself is the last item (for tooltips and drops).
        ImGui::Dummy({0, pad});
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::BeginGroup();

        const ImVec2 at = ImGui::GetCursorScreenPos(), end {at.x + side, at.y + side};
        ImGui::Dummy({side, side});
        draw->AddRectFilled(at, end, ImGui::GetColorU32(colors.Inset), style.FrameRounding);
        if (const auto texture = ui.MaterialThumbnail(current))
            draw->AddImageRounded(static_cast<ImTextureID>(uintptr_t(texture->GetImGuiTextureID())),
                                  at,
                                  end,
                                  {0, 1},
                                  {1, 0},
                                  IM_COL32_WHITE,
                                  style.FrameRounding);
        else
            CenterGlyph(ICON_FA_CIRCLE_DOT, at, end, ImGui::GetColorU32(colors.TextOff));
        ImGui::SameLine();

        ImGui::BeginGroup();
        // The four tools keep the right end; the title and detail share what is left, the title
        // giving way first, both cut with an ellipsis and shown in full in the tooltip.
        const float  tools = 4 * EditorTheme::Px(28.0f) + 3 * style.ItemSpacing.x;
        const float  text  = std::max(0.0f, ImGui::GetContentRegionAvail().x - tools - style.ItemSpacing.x);
        const ImVec2 line  = ImGui::GetCursorScreenPos();
        const float  textY = line.y + style.FramePadding.y, bottom = textY + ImGui::GetTextLineHeight();
        const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str()), detailSize = ImGui::CalcTextSize(detail.c_str());
        const float  titleWidth =
            std::min(titleSize.x, std::max(text - detailSize.x - style.ItemSpacing.x, text * 0.5f));
        const float detailX = line.x + titleWidth + style.ItemSpacing.x, right = line.x + text;
        ImGui::RenderTextEllipsis(draw, {line.x, textY}, {line.x + titleWidth, bottom}, line.x + titleWidth,
                                  title.c_str(), nullptr, &titleSize);
        if (detailX < right)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.TextDim);
            ImGui::RenderTextEllipsis(draw, {detailX, textY}, {right, bottom}, right, detail.c_str(), nullptr,
                                      &detailSize);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({text, ImGui::GetFrameHeight()});
        if (ImGui::IsItemHovered() && titleSize.x + style.ItemSpacing.x + detailSize.x > text)
            ImGui::SetTooltip("%s  %s", title.c_str(), detail.c_str());
        ImGui::SameLine();
        if (ToolButton(ICON_FA_EYE, "Highlight these parts in the viewport", highlighted))
            ui.ToggleSlotHighlight(parts);
        ImGui::SameLine();
        if (ToolButton(ICON_FA_CROSSHAIRS, isolated ? "Show the whole scene" : "Show only these parts", isolated))
            ui.ToggleSlotIsolate(parts);
        ImGui::SameLine();
        if (ToolButton(ICON_FA_ARROW_LEFT,
                       browser.IsValid() ? ("Use " + ui.MaterialName(browser)).c_str() :
                                           "Use the material selected in the Content Browser",
                       false,
                       browser.IsValid() && browser != current))
        {
            chosen = browser;
            picked = true;
        }
        ImGui::SameLine();
        if (ToolButton(
                ICON_FA_ROTATE_LEFT, ("Restore " + ui.MaterialName(imported)).c_str(), false, current != imported))
        {
            chosen = {};
            picked = true;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        AssetID combo;
        if (ui.MaterialCombo(imported, current, combo))
        {
            chosen = combo;
            picked = true;
        }
        ImGui::EndGroup();
        ImGui::EndGroup();

        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered)
            ui.HoverSlot(parts);
        // Double-clicking the preview opens the material; the row's menu holds the rest.
        if (ImGui::IsMouseHoveringRect(at, end) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            ui.CanOpenMaterial(current))
            ui.OpenMaterial(current);
        if (ImGui::BeginPopupContextItem("##slotMenu"))
        {
            const bool file = ui.CanOpenMaterial(current);
            if (ImGui::MenuItem(ICON_FA_PEN "  Open in Material Editor", nullptr, false, file))
                ui.OpenMaterial(current);
            if (!file && current.IsValid())
                ImGui::SetItemTooltip("Inside its model; create an instance to edit it");
            if (ImGui::MenuItem(ICON_FA_FOLDER "  Show in Content Browser", nullptr, false, current.IsValid()))
                ui.RevealAsset(current);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CLONE "  Create Material Instance", nullptr, false, current.IsValid()))
                ui.CreateMaterialInstance(current, parts);
            ImGui::SetItemTooltip("A new material with %s as its parent, used by this slot",
                                  ui.MaterialName(current).c_str());
            if (remove)
            {
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove Slot"))
                    remove();
            }
            ImGui::EndPopup();
        }
        if (const AssetID dropped = ui.AcceptMaterialDrop(); dropped.IsValid())
        {
            chosen = dropped;
            picked = true;
        }
        draw->ChannelsSetCurrent(0);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        // Hover fills the row; a pinned or isolated row carries a strip in its viewport colour,
        // which keeps the tools' own on state readable.
        if (hovered)
            draw->AddRectFilled({min.x - pad, min.y - pad},
                                {max.x + pad, max.y + pad},
                                ImGui::GetColorU32(colors.Hover),
                                style.FrameRounding);
        if (highlighted || isolated)
        {
            const glm::vec4 strip = highlighted ? SlotPinnedColor : SlotIsolatedColor;
            draw->AddRectFilled({min.x - pad, min.y - pad},
                                {min.x - pad + EditorTheme::Px(3.0f), max.y + pad},
                                ImGui::GetColorU32(ImVec4(strip.r, strip.g, strip.b, 1.0f)),
                                style.FrameRounding);
        }
        draw->ChannelsMerge();
        return picked;
    }
} // namespace Kans::Widgets
