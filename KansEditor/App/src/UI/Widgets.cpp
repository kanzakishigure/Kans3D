#include "Widgets.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include <imgui_internal.h>
#include <string>
#include "EditorIcons.h"

namespace Kans::Widgets
{
    void CenterGlyph(const char* icon, ImVec2 min, ImVec2 max, ImU32 color)
    {
        // Centre the glyph's drawn pixels, not the font's line box: icon glyphs sit at
        // different heights within the line, so box-centred icons look misaligned.
        unsigned int codepoint = 0;
        ImTextCharFromUtf8(&codepoint, icon, nullptr);
        ImFontBaked*       baked = ImGui::GetFontBaked();
        const ImFontGlyph* glyph = baked ? baked->FindGlyphNoFallback(ImWchar(codepoint)) : nullptr;
        const ImVec2       center {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
        ImVec2             at;
        if (glyph)
            at = {center.x - (glyph->X0 + glyph->X1) * 0.5f, center.y - (glyph->Y0 + glyph->Y1) * 0.5f};
        else
        {
            const ImVec2 size = ImGui::CalcTextSize(icon);
            at                = {center.x - size.x * 0.5f, center.y - size.y * 0.5f};
        }
        ImGui::GetWindowDrawList()->AddText({IM_ROUND(at.x), IM_ROUND(at.y)}, color, icon);
    }

    bool ToolButton(const char* icon, const char* tooltip, bool active, bool enabled)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, active ? EditorTheme::Colors().Selection : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().Hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::BeginDisabled(!enabled);
        ImGui::PushID(icon);
        const bool clicked = ImGui::Button("##tool", {EditorTheme::Px(28.0f), EditorTheme::Px(26.0f)});
        ImGui::PopID();
        CenterGlyph(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }

    void MenuSection(const char* title)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
        ImGui::SeparatorText(title);
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }

    void ToolSeparator()
    {
        ImGui::SameLine(0, 8.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            {at.x, at.y + 4.0f}, {at.x, at.y + 22.0f}, ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy({1.0f, 26.0f});
        ImGui::SameLine(0, 8.0f);
    }

    float Pill(ImVec2 position, const std::function<void()>& contents)
    {
        const float pad  = EditorTheme::Px(3.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::SetCursorScreenPos({position.x + pad, position.y});
        // Start a fresh line at `position`. In a menu bar the items before are still on the
        // current line: its text baseline would push the buttons down, and SameLine() inside
        // the pill would snap the following buttons back to that line's top.
        ImGuiWindow* window                 = ImGui::GetCurrentWindow();
        window->DC.CurrLineTextBaseOffset   = 0.0f;
        window->DC.IsSameLine               = false;
        window->DC.CurrLineSize.y           = 0.0f;
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {EditorTheme::Px(2.0f), 0.0f});
        contents();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        draw->ChannelsSetCurrent(0);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        draw->AddRectFilled({min.x - pad, min.y - pad},
                            {max.x + pad, max.y + pad},
                            ImGui::GetColorU32(EditorTheme::Colors().Raised),
                            EditorTheme::Px(6.0f));
        draw->ChannelsMerge();
        return max.x + pad;
    }

    namespace
    {
        enum class Shape
        {
            Play,
            Stop,
            Pause,
            Step
        };

        // Shapes rather than font glyphs: glyphs are centred by the font's line box,
        // so a triangle and two bars end up at different heights.
        void DrawShape(ImDrawList* draw, Shape shape, ImVec2 center, float size, ImU32 color)
        {
            const float h = size * 0.5f;
            switch (shape)
            {
                case Shape::Play:
                {
                    // Box centre a little right of the segment centre, toward the triangle's
                    // centroid, so it does not look pushed to the right.
                    const float left = center.x - h * 0.7f;
                    draw->AddTriangleFilled({left, center.y - h}, {left, center.y + h}, {left + h * 1.6f, center.y}, color);
                    break;
                }
                case Shape::Stop:
                    draw->AddRectFilled({center.x - h * 0.8f, center.y - h * 0.8f}, {center.x + h * 0.8f, center.y + h * 0.8f},
                                        color, EditorTheme::Px(1.5f));
                    break;
                case Shape::Pause:
                    draw->AddRectFilled({center.x - h * 0.8f, center.y - h}, {center.x - h * 0.25f, center.y + h}, color,
                                        EditorTheme::Px(1.0f));
                    draw->AddRectFilled({center.x + h * 0.25f, center.y - h}, {center.x + h * 0.8f, center.y + h}, color,
                                        EditorTheme::Px(1.0f));
                    break;
                case Shape::Step:
                {
                    const float left = center.x - h * 0.85f;
                    draw->AddTriangleFilled({left, center.y - h * 0.85f}, {left, center.y + h * 0.85f}, {left + h * 1.3f, center.y},
                                            color);
                    draw->AddRectFilled({center.x + h * 0.55f, center.y - h * 0.85f}, {center.x + h * 0.85f, center.y + h * 0.85f},
                                        color, EditorTheme::Px(1.0f));
                    break;
                }
            }
        }

        ImVec4 Tint(ImVec4 color, float alpha) { return {color.x, color.y, color.z, alpha}; }
    } // namespace

    float TransportWidth() { return 3.0f * EditorTheme::Px(36.0f) + 2.0f * EditorTheme::Px(1.0f); }
    // Tool buttons (26 px) plus the pill's padding on both sides: every group in a toolbar
    // band has this height, so their edges line up.
    float BandGroupHeight() { return EditorTheme::Px(26.0f) + 2.0f * EditorTheme::Px(3.0f); }

    Transport TransportControls(ImVec2 position, bool playing, bool paused, float* right)
    {
        using EditorTheme::Px;
        const auto&  p       = EditorTheme::Colors();
        const float  segment = Px(36.0f), height = BandGroupHeight(), gap = Px(1.0f);
        auto*        draw    = ImGui::GetWindowDrawList();
        Transport    clicked = Transport::None;
        const struct
        {
            Transport   Action;
            Shape       Icon;
            bool        Enabled;
            const char* Tooltip;
        } segments[3] = {
            {Transport::PlayStop, playing ? Shape::Stop : Shape::Play, true,
             playing ? "Stop and restore the scene (Ctrl+P)" : "Play (Ctrl+P)"},
            {Transport::Pause, Shape::Pause, playing, paused ? "Resume (Ctrl+Shift+P)" : "Pause (Ctrl+Shift+P)"},
            {Transport::Step, Shape::Step, paused, "Advance one frame"},
        };
        for (int i = 0; i < 3; ++i)
        {
            const auto&  s   = segments[i];
            const ImVec2 min {position.x + i * (segment + gap), position.y}, max {min.x + segment, position.y + height};
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(i);
            ImGui::BeginDisabled(!s.Enabled);
            if (ImGui::InvisibleButton("##transport", {segment, height}))
                clicked = s.Action;
            ImGui::EndDisabled();
            ImGui::PopID();
            const bool hovered = s.Enabled && ImGui::IsItemHovered();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", s.Tooltip);

            ImVec4 fill = hovered ? p.Hover : p.Raised;
            ImVec4 icon = s.Enabled ? p.Text : p.TextOff;
            if (i == 0)
            {
                const ImVec4 tone = playing ? p.Danger : p.Success;
                fill              = Tint(tone, hovered ? 0.32f : 0.2f);
                icon              = tone;
            }
            else if (i == 1 && paused)
            {
                fill = p.Selection;
                icon = p.AccentHover;
            }
            const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft :
                                        i == 2 ? ImDrawFlags_RoundCornersRight :
                                                 ImDrawFlags_RoundCornersNone;
            draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), Px(6.0f), corners);
            DrawShape(draw, s.Icon, {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}, Px(11.0f), ImGui::GetColorU32(icon));
        }
        if (right)
            *right = position.x + TransportWidth();
        return clicked;
    }

    bool TextButton(const char* icon, const char* text, const char* tooltip, bool active, bool dropdown)
    {
        std::string label = std::string(icon) + "  " + text;
        if (dropdown)
            label += "  " ICON_FA_CHEVRON_DOWN;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? EditorTheme::Colors().Selection : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().Hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {EditorTheme::Px(8.0f), ImGui::GetStyle().FramePadding.y});
        const bool clicked = ImGui::Button(label.c_str(), {0.0f, EditorTheme::Px(26.0f)});
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }

    float OverlayGroup(ImVec2 position, const std::function<void()>& contents)
    {
        const float pad  = EditorTheme::Px(4.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::SetCursorScreenPos({position.x + pad, position.y + pad});
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {EditorTheme::Px(2.0f), EditorTheme::Px(4.0f)});
        contents();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        draw->ChannelsSetCurrent(0);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        // Panel tone, mostly opaque so the scene shows through without hurting legibility.
        auto background = EditorTheme::Colors().Panel;
        background.w    = 0.82f;
        draw->AddRectFilled({min.x - pad, min.y - pad}, {max.x + pad, max.y + pad}, ImGui::GetColorU32(background),
                            EditorTheme::Px(8.0f));
        draw->ChannelsMerge();
        return max.x + pad;
    }
} // namespace Kans::Widgets
