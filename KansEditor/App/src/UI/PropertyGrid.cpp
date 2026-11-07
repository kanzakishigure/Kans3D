#include "PropertyGrid.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/ImGui/EditorTheme.h"
#include "EditorIcons.h"

namespace Kans::PropertyGrid
{
    namespace
    {
        using EditorTheme::Px;

        struct State
        {
            // Shared label column: a fraction of the grid width, clamped while never dragged.
            float LabelFraction = 0.38f;
            bool  Dragged       = false;
            // Filter.
            std::string Filter;
            bool        ModifiedOnly = false;
            bool        LastRowShown = true;
            // Current grid.
            int    Depth      = 0;
            float  GridX      = 0.0f;
            float  GridWidth  = 0.0f;
            float  LabelWidth = 0.0f;
            ImVec2 GridStart;
            // Value cell of the current row (window-local x).
            float ValueRight = 0.0f;
            // Counts for the open group and card, stored when they end.
            ImGuiID GroupID = 0, CardID = 0;
            int     GroupModified = 0, GroupShown = 0, CardShown = 0;
            bool    CardBody      = false;
            ImVec2  CardMin;
            bool    CardOverridden = false;
            // The outer groups' counts while a nested one is open.
            int RowIndex = 0; // Rows so far in the current group or grid, for separators.
            struct Frame
            {
                ImGuiID ID       = 0;
                int     Modified = 0, Shown = 0, RowIndex = 0;
                float   Indent = 0, GuideX = 0, BodyTop = 0;
            };
            std::vector<Frame> Groups;
            // The last group heading drawn, and room kept free at the next heading's right end.
            ImVec2 HeadingMin, HeadingMax;
            float  NextTrailer = 0.0f;
        };
        State g;

        bool Contains(const char* text, const std::string& needle)
        {
            const std::string haystack(text);
            return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                   }) != haystack.end();
        }

        ImVec4 Faded(ImVec4 color, float alpha) { return {color.x, color.y, color.z, color.w * alpha}; }

        ImFont* Bold()
        {
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            return fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
        }

        // Right-aligned reset button of a modified row.
        bool ResetButton(bool modified)
        {
            if (!modified)
                return false;
            const float size = ImGui::GetFrameHeight();
            ImGui::SameLine(g.ValueRight - size);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
            const bool clicked = ImGui::Button(ICON_FA_ROTATE_LEFT "##reset", {size, size});
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset to default");
            return clicked;
        }

        float ResetReserve(bool modified) { return modified ? ImGui::GetFrameHeight() + Px(4.0f) : 0.0f; }

        ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        // Opens a group: a nested one steps in, with its guide line on the left.
        void OpenFrame(bool decorated)
        {
            g.Groups.emplace_back();
            auto& f    = g.Groups.back();
            f.ID       = g.GroupID;
            f.Modified = g.GroupModified;
            f.Shown    = g.GroupShown;
            f.RowIndex = g.RowIndex;
            if (decorated && g.Groups.size() > 1)
            {
                f.Indent = Px(12.0f);
                f.GuideX = ImGui::GetCursorScreenPos().x + Px(5.0f);
                ImGui::Indent(f.Indent);
            }
        }

        void CloseFrame()
        {
            auto&       f      = g.Groups.back();
            const float bottom = ImGui::GetCursorScreenPos().y;
            if (f.Indent > 0)
            {
                ImGui::Unindent(f.Indent);
                if (bottom > f.BodyTop)
                    ImGui::GetWindowDrawList()->AddLine({f.GuideX, f.BodyTop},
                                                        {f.GuideX, bottom - Px(2.0f)},
                                                        ImGui::GetColorU32(EditorTheme::Colors().Pressed),
                                                        Px(2.0f));
            }
            g.GroupID       = f.ID;
            g.GroupModified = f.Modified;
            g.GroupShown    = f.Shown;
            g.RowIndex      = f.RowIndex;
            g.Groups.pop_back();
        }
    } // namespace

    // ---------------------------------------------------------------------- filter

    void SetFilter(const std::string& text, bool modifiedOnly)
    {
        g.Filter       = text;
        g.ModifiedOnly = modifiedOnly;
    }
    void ClearFilter() { SetFilter({}, false); }
    bool IsFiltering() { return !g.Filter.empty() || g.ModifiedOnly; }

    // ---------------------------------------------------------------------- cards

    bool BeginCard(const char* id, const char* icon, const char* title, const CardOptions& options)
    {
        ImGui::PushID(id);
        auto*         storage   = ImGui::GetStateStorage();
        const ImGuiID openID    = ImGui::GetID("##open");
        const ImGuiID shownID   = ImGui::GetID("##shown");
        bool          open      = storage->GetBool(openID, options.DefaultOpen);
        // While filtering, a card whose rows all failed the filter last frame keeps only its header.
        const bool    empty     = IsFiltering() && storage->GetInt(shownID, 1) == 0;
        g.CardID                = shownID;
        g.CardShown             = 0;
        g.CardOverridden        = options.Overridden;
        // The gap between cards goes before the card, so after EndCard() the card is the last item.
        if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1.0f)
            ImGui::Dummy({0, Px(4.0f)});
        g.CardMin = ImGui::GetCursorScreenPos();

        const float headerHeight = ImGui::GetFrameHeight() + Px(6.0f);
        const std::string label = std::string(icon) + "  " + title;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0)); // The outline is drawn behind the child.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {Px(10.0f), Px(4.0f)});
        ImGui::BeginChild("##card", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Header: clicking it folds the card; the menu button sits at its right end.
        const ImVec2 top   = ImGui::GetCursorScreenPos();
        const float  width = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("##header", {width, headerHeight - Px(4.0f)});
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::GetWindowDrawList()->AddRectFilled({top.x - Px(6.0f), top.y - Px(2.0f)},
                                                      {top.x + width + Px(6.0f), top.y + headerHeight - Px(4.0f)},
                                                      ImGui::GetColorU32(Faded(EditorTheme::Colors().Hover, 0.6f)), Px(5.0f));
        if (clicked && !empty)
        {
            open = !open;
            storage->SetBool(openID, open);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && options.Menu)
            ImGui::OpenPopup("##CardMenu");
        auto*        draw  = ImGui::GetWindowDrawList();
        const auto&  p     = EditorTheme::Colors();
        const float  textY = top.y + (headerHeight - Px(4.0f) - ImGui::GetFontSize()) * 0.5f;
        const ImU32  dim   = ImGui::GetColorU32(p.TextDim);
        const ImU32  text  = ImGui::GetColorU32(empty ? p.TextOff : p.Text);
        float        x     = top.x;
        draw->AddText({x, textY}, dim, open && !empty ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        x += ImGui::GetFontSize() + Px(6.0f);
        ImGui::PushFont(Bold(), 0.0f);
        draw->AddText({x, textY}, text, label.c_str());
        x += ImGui::CalcTextSize(label.c_str()).x;
        ImGui::PopFont();
        if (options.Note)
            draw->AddText({x + Px(8.0f), textY}, ImGui::GetColorU32(options.Overridden ? p.AccentHover : p.TextDim), options.Note);

        if (options.Menu)
        {
            const float size = ImGui::GetFrameHeight() - Px(4.0f);
            ImGui::SetCursorScreenPos({top.x + width - size, top.y + (headerHeight - Px(4.0f) - size) * 0.5f});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, p.TextDim);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});
            if (ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL "##menu", {size, size}))
                ImGui::OpenPopup("##CardMenu");
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::BeginPopup("##CardMenu"))
            {
                options.Menu();
                ImGui::EndPopup();
            }
        }
        ImGui::SetCursorScreenPos({top.x, top.y + headerHeight});
        ImGui::Dummy({0, 0});

        g.CardBody = open && !empty;
        return g.CardBody;
    }

    void EndCard()
    {
        auto* storage = ImGui::GetStateStorage();
        if (g.CardBody)
        {
            storage->SetInt(g.CardID, g.CardShown);
            ImGui::Dummy({0, Px(2.0f)});
        }
        else if (!IsFiltering())
            storage->SetInt(g.CardID, 1); // Recount once filtering starts again.
        ImGui::EndChild();
        // The child is drawn above its parent, so its outline goes on the parent's list.
        const ImVec2 min  = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        auto*        draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(EditorTheme::Colors().Raised), Px(6.0f));
        if (g.CardOverridden)
            draw->AddRectFilled({min.x, min.y + Px(6.0f)}, {min.x + Px(3.0f), max.y - Px(6.0f)},
                                ImGui::GetColorU32(EditorTheme::Colors().Accent), Px(1.5f));
        ImGui::PopID();
    }

    // ---------------------------------------------------------------------- groups

    bool BeginGroup(const char* title, bool* enabled, bool* toggled, bool defaultOpen)
    {
        ImGui::PushID(title);
        auto*         storage    = ImGui::GetStateStorage();
        const ImGuiID openID     = ImGui::GetID("##open");
        const ImGuiID modifiedID = ImGui::GetID("##modified");
        const ImGuiID shownID    = ImGui::GetID("##shown");
        bool          open       = storage->GetBool(openID, defaultOpen);
        if (toggled)
            *toggled = false;
        // While filtering every group opens, and one that matched nothing last frame keeps
        // only its rows (no heading), so it can show up again when they match.
        const bool filtering = IsFiltering();
        const bool heading   = !filtering || storage->GetInt(shownID, 1) > 0;
        if (filtering)
            open = true;
        if (!heading)
        {
            g.HeadingMax  = g.HeadingMin; // No heading this time.
            g.NextTrailer = 0.0f;
            if (enabled && !*enabled)
            {
                ImGui::PopID();
                return false;
            }
            OpenFrame(false);
            g.RowIndex      = 0;
            g.GroupModified = 0;
            g.GroupShown    = 0;
            Begin(title);
            g.GroupID = modifiedID;
            return true;
        }

        OpenFrame(true);
        const float  trailer = g.NextTrailer;
        g.NextTrailer        = 0.0f;
        const auto&  p      = EditorTheme::Colors();
        const float  height = ImGui::GetFrameHeight();
        const ImVec2 top    = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy({0, Px(2.0f)});
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##heading", {width, height}))
        {
            open = !open;
            storage->SetBool(openID, open);
        }
        const bool hovered = ImGui::IsItemHovered();
        g.HeadingMin       = at;
        g.HeadingMax       = {at.x + width, at.y + height};
        auto*      draw    = ImGui::GetWindowDrawList();
        {
            // A band one step lighter than the card; nested bands step back towards it.
            const int    level = int(g.Groups.size()) - 1;
            const ImVec4 band  = hovered ? p.Pressed : Mix(p.Hover, p.Raised, std::min(0.6f, 0.3f * level));
            draw->AddRectFilled(
                {at.x - Px(4.0f), at.y}, {at.x + width + Px(4.0f), at.y + height}, ImGui::GetColorU32(band), Px(3.0f));
        }
        const float textY = at.y + (height - ImGui::GetFontSize()) * 0.5f;
        float       x     = at.x + Px(2.0f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.75f);
        draw->AddText({x, at.y + (height - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(p.TextDim),
                      open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        ImGui::PopFont();
        x += Px(16.0f);
        if (enabled)
        {
            // The feature toggle sits on the heading, before the title.
            // A box 6 px shorter than the heading, centred in it.
            ImGui::SetCursorScreenPos({x, at.y + Px(3.0f)});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {Px(3.0f), (height - ImGui::GetFontSize() - Px(6.0f)) * 0.5f});
            if (ImGui::Checkbox("##enabled", enabled) && toggled)
                *toggled = true;
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(*enabled ? "Turn %s off" : "Turn %s on", title);
            x = ImGui::GetItemRectMax().x + Px(6.0f);
        }
        ImGui::PushFont(Bold(), 0.0f);
        draw->AddText({x, textY}, ImGui::GetColorU32(enabled && !*enabled ? p.TextDim : p.Text), title);
        ImGui::PopFont();
        // "N modified" from last frame's rows, at the right end.
        if (const int modified = storage->GetInt(modifiedID, 0))
        {
            char note[32];
            std::snprintf(note, sizeof(note), "%d modified", modified);
            const float w = ImGui::CalcTextSize(note).x;
            draw->AddText({at.x + width - w - Px(4.0f) - trailer, textY}, ImGui::GetColorU32(p.AccentHover), note);
        }
        ImGui::SetCursorScreenPos({top.x, at.y + height});
        ImGui::Dummy({0, Px(2.0f)});
        g.Groups.back().BodyTop = ImGui::GetCursorScreenPos().y;

        if (!open || (enabled && !*enabled))
        {
            CloseFrame();
            ImGui::PopID();
            return false;
        }
        g.RowIndex      = 0;
        g.GroupID       = 0;
        g.GroupModified = 0;
        g.GroupShown    = 0;
        Begin(title);
        g.GroupID = modifiedID;
        return true;
    }

    void SetNextGroupTrailer(float width) { g.NextTrailer = width; }
    bool LastGroupHeading(ImVec2& min, ImVec2& max)
    {
        min = g.HeadingMin;
        max = g.HeadingMax;
        return max.x > min.x;
    }

    void EndGroup()
    {
        End();
        if (g.GroupID)
        {
            auto* storage = ImGui::GetStateStorage();
            storage->SetInt(g.GroupID, g.GroupModified);
            storage->SetInt(ImGui::GetID("##shown"), g.GroupShown);
        }
        // A nested group's rows also count for the group around it.
        const int shown = g.GroupShown, modified = g.GroupModified;
        CloseFrame();
        g.GroupShown += shown;
        g.GroupModified += modified;
        ImGui::PopID();
    }

    // ---------------------------------------------------------------------- grids

    bool Begin(const char* id, float labelWidth)
    {
        ImGui::PushID(id);
        if (g.Depth++ > 0)
            return true; // Nested grids share the outer one's columns.
        g.GridStart = ImGui::GetCursorScreenPos();
        g.GridX     = g.GridStart.x;
        g.RowIndex  = 0;
        g.GridWidth = ImGui::GetContentRegionAvail().x;
        if (labelWidth > 0.0f)
            g.LabelWidth = Px(labelWidth);
        else if (g.Dragged)
            g.LabelWidth = std::clamp(g.LabelFraction * g.GridWidth, Px(48.0f), g.GridWidth * 0.7f);
        else
            g.LabelWidth = std::clamp(g.LabelFraction * g.GridWidth, Px(80.0f), Px(160.0f));
        g.LastRowShown = true;
        return true;
    }

    void End()
    {
        if (--g.Depth > 0)
        {
            ImGui::PopID();
            return;
        }
        // The shared column edge: drag it in any grid to resize every grid.
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const float  x      = g.GridX + g.LabelWidth - ImGui::GetStyle().ItemSpacing.x * 0.5f;
        const ImRect edge({x - Px(3.0f), g.GridStart.y}, {x + Px(3.0f), window->DC.CursorPos.y});
        const ImGuiID id = ImGui::GetID("##labelEdge");
        if (edge.GetHeight() > 0.0f && ImGui::ItemAdd(edge, id, nullptr, ImGuiItemFlags_NoNav))
        {
            bool hovered = false, held = false;
            ImGui::ButtonBehavior(edge, id, &hovered, &held);
            if (hovered || held)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                window->DrawList->AddLine({x, edge.Min.y}, {x, edge.Max.y},
                                          ImGui::GetColorU32(EditorTheme::Colors().Accent), Px(1.0f));
            }
            if (held && g.GridWidth > 0.0f)
            {
                const float width = ImGui::GetIO().MousePos.x - g.GridX + ImGui::GetStyle().ItemSpacing.x * 0.5f;
                g.LabelFraction   = std::clamp(width / g.GridWidth, 0.1f, 0.7f);
                g.Dragged         = true;
            }
        }
        ImGui::PopID();
    }

    void BeginFree() { End(); }

    void EndFree() { Begin("##free"); }

    bool Row(const char* label, const char* tooltip, bool modified)
    {
        // Continuation rows (empty label) follow the row they belong to.
        bool shown = true;
        if (IsFiltering())
            shown = !label[0] ? g.LastRowShown :
                                (g.Filter.empty() || Contains(label, g.Filter)) && (!g.ModifiedOnly || modified);
        g.LastRowShown = shown;
        if (!shown)
            return false;
        g.CardShown++;
        g.GroupShown++;
        g.GroupModified += modified;

        const auto&  p      = EditorTheme::Colors();
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const float  height = ImGui::GetFrameHeight();
        // Nested groups step in, so their rows end at the narrower region.
        const float  width  = g.GridWidth > 0.0f ? std::min(g.GridWidth, ImGui::GetContentRegionAvail().x) :
                                                   ImGui::GetContentRegionAvail().x;
        auto*        draw   = ImGui::GetWindowDrawList();
        // Hovered row highlight (behind the row's items, which are drawn after it).
        const ImVec2 rowMin {start.x - Px(4.0f), start.y - 1.0f}, rowMax {start.x + width + Px(4.0f), start.y + height + 1.0f};
        // A hairline between the rows of a group or grid.
        if (g.RowIndex++ > 0)
        {
            const float y = start.y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            draw->AddLine({rowMin.x, y}, {rowMax.x, y}, ImGui::GetColorU32(Mix(p.Panel, p.App, 0.5f)), 1.0f);
        }
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(rowMin, rowMax, false))
            draw->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(Faded(p.Hover, 0.45f)), Px(3.0f));

        // Label, cut with an ellipsis; the tooltip shows it in full.
        const float labelWidth = std::max(0.0f, g.LabelWidth - ImGui::GetStyle().ItemSpacing.x);
        const float textY      = start.y + ImGui::GetStyle().FramePadding.y;
        const ImVec2 size      = ImGui::CalcTextSize(label);
        ImGui::PushStyleColor(ImGuiCol_Text, modified ? p.Text : p.TextDim);
        ImGui::RenderTextEllipsis(draw, {start.x, textY}, {start.x + labelWidth, textY + size.y}, start.x + labelWidth,
                                  label, nullptr, &size);
        ImGui::PopStyleColor();
        ImGui::Dummy({labelWidth, height});
        if (ImGui::IsItemHovered() && (tooltip || size.x > labelWidth))
            ImGui::SetTooltip("%s", tooltip ? tooltip : label);
        ImGui::SameLine();
        g.ValueRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(modified ? ImGui::GetContentRegionAvail().x - ResetReserve(true) : -FLT_MIN);
        return true;
    }

    bool RowReset(bool modified) { return ResetButton(modified); }

    // ---------------------------------------------------------------------- widgets

    void Text(const char* label, const std::string& value, bool disabled)
    {
        if (!Row(label))
            return;
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.0f);
        if (disabled)
            ImGui::TextDisabled("%s", value.c_str());
        else
            ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    }

    bool Int(const char* label, int& value, int min, int max, const int* reset)
    {
        const bool modified = reset && value != *reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = ImGui::DragInt("##value", &value, 0.2f, min, max);
        if (ResetButton(modified))
        {
            value   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    bool Float(const char* label, float& value, float speed, float min, float max, const char* format, const float* reset)
    {
        const bool modified = reset && std::abs(value - *reset) > 1e-6f;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = ImGui::DragFloat("##value", &value, speed, min, max, format);
        if (ResetButton(modified))
        {
            value   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    bool Checkbox(const char* label, bool& value, const bool* reset)
    {
        const bool modified = reset && value != *reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = ImGui::Checkbox("##value", &value);
        if (ResetButton(modified))
        {
            value   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        // Compact swatch with the components beside it; clicking opens a picker.
        bool ColorField(float* rgba, int components, bool modified)
        {
            const float  height = ImGui::GetFrameHeight();
            const float  avail  = ImGui::GetContentRegionAvail().x - ResetReserve(modified);
            const ImVec4 color {rgba[0], rgba[1], rgba[2], components == 4 ? rgba[3] : 1.0f};
            const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                              (components == 4 ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha);
            if (ImGui::ColorButton("##swatch", color, flags | ImGuiColorEditFlags_AlphaPreviewHalf,
                                   {std::min(Px(44.0f), avail), height}))
                ImGui::OpenPopup("##picker");
            // The values, dim and clipped to the cell.
            char text[64];
            if (components == 4)
                std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f  %.2f", rgba[0], rgba[1], rgba[2], rgba[3]);
            else
                std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f", rgba[0], rgba[1], rgba[2]);
            const ImVec2 at = {ImGui::GetItemRectMax().x + Px(8.0f), ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y};
            const float  right = ImGui::GetItemRectMin().x + avail;
            if (right > at.x)
            {
                const ImVec2 size = ImGui::CalcTextSize(text);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, {right, at.y + size.y}, right, text, nullptr, &size);
                ImGui::PopStyleColor();
            }
            bool changed = false;
            if (ImGui::BeginPopup("##picker"))
            {
                changed = components == 4 ? ImGui::ColorPicker4("##picker", rgba, flags) :
                                            ImGui::ColorPicker3("##picker", rgba, flags);
                ImGui::EndPopup();
            }
            return changed;
        }
    } // namespace

    bool Color3(const char* label, glm::vec3& value, const glm::vec3* reset)
    {
        const bool modified = reset && value != *reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = ColorField(glm::value_ptr(value), 3, modified);
        if (ResetButton(modified))
        {
            value   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    bool Color4(const char* label, glm::vec4& value, const glm::vec4* reset)
    {
        const bool modified = reset && value != *reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = ColorField(glm::value_ptr(value), 4, modified);
        if (ResetButton(modified))
        {
            value   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    bool InputText(const char* label, std::string& value)
    {
        if (!Row(label))
            return false;
        char buffer[512] = {};
        value.copy(buffer, sizeof(buffer) - 1);
        ImGui::PushID(label);
        const bool changed = ImGui::InputText("##value", buffer, sizeof(buffer));
        ImGui::PopID();
        if (changed)
            value = buffer;
        return changed;
    }

    bool Combo(const char* label, const char* const items[], int count, int& index, const int* reset)
    {
        const bool modified = reset && index != *reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        bool changed = false;
        if (ImGui::BeginCombo("##value", index >= 0 && index < count ? items[index] : ""))
        {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], i == index) && i != index)
                {
                    index   = i;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        if (ResetButton(modified))
        {
            index   = *reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }

    bool Vec3(const char* label, glm::vec3& value, float reset, float speed, bool* linked)
    {
        return Vec3(label, value, glm::vec3(reset), speed, linked);
    }

    bool Vec3(const char* label, glm::vec3& value, const glm::vec3& reset, float speed, bool* linked)
    {
        const bool modified = value != reset;
        if (!Row(label, nullptr, modified))
            return false;
        ImGui::PushID(label);
        const ImU32 axisColors[3] = {IM_COL32(222, 82, 74, 255), IM_COL32(104, 186, 92, 255), IM_COL32(72, 132, 232, 255)};
        const float height  = ImGui::GetFrameHeight();
        const float spacing = Px(4.0f);
        const float link    = linked ? height + spacing : 0.0f;
        const float avail   = ImGui::GetContentRegionAvail().x - ResetReserve(modified) - link;
        const float field   = std::max(Px(24.0f), (avail - 2.0f * spacing) / 3.0f);
        bool        changed = false;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (axis)
                ImGui::SameLine(0.0f, spacing);
            ImGui::PushID(axis);
            ImGui::SetNextItemWidth(field);
            const float before = value[axis];
            if (ImGui::DragFloat("##axis", &value[axis], speed, 0.0f, 0.0f, "%.3f"))
            {
                changed = true;
                if (linked && *linked)
                    KeepRatio(value, axis, before);
            }
            // Axis colour as a thin bar at the field's left edge.
            const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled({min.x, min.y + Px(3.0f)}, {min.x + Px(2.0f), max.y - Px(3.0f)},
                                                      axisColors[axis], Px(1.0f));
            ImGui::PopID();
        }
        if (linked)
        {
            ImGui::SameLine(0.0f, spacing);
            ImGui::PushStyleColor(ImGuiCol_Button, *linked ? EditorTheme::Colors().Selection : ImVec4(0, 0, 0, 0));
            if (ImGui::Button(*linked ? ICON_FA_LINK "##link" : ICON_FA_LINK_SLASH "##link", {height, height}))
                *linked = !*linked;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(*linked ? "Uniform: the axes keep their ratio" : "Each axis on its own");
        }
        if (ResetButton(modified))
        {
            value   = reset;
            changed = true;
        }
        ImGui::PopID();
        return changed;
    }
    void KeepRatio(glm::vec3& value, int axis, float before)
    {
        for (int other = 0; other < 3; ++other)
            if (other != axis)
                value[other] = std::abs(before) > 1e-6f ? value[other] * (value[axis] / before) : value[axis];
    }
} // namespace Kans::PropertyGrid
