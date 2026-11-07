#include "ProgressPopup.h"
#include <algorithm>
#include <cmath>
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/Launcher/ProjectGrid.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace
    {
        using EditorTheme::Px;
        constexpr float CardWidth = 360.0f, HeaderHeight = 32.0f, PillHeight = 28.0f, Rounding = 8.0f;

        ImU32 Color(const ImVec4& color, float alpha = 1.0f)
        {
            return ImGui::GetColorU32(ImVec4 {color.x, color.y, color.z, color.w * alpha});
        }
        ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
        }
        ImFont* BoldFont()
        {
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            return fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
        }
        const char* CategoryIcon(EditorProgressCategory category)
        {
            return category == EditorProgressCategory::Scripts ? ICON_FA_CODE : ICON_FA_FILE_IMPORT;
        }
        ImVec4 CategoryColor(EditorProgressCategory category)
        {
            const auto& p = EditorTheme::Colors();
            return category == EditorProgressCategory::Scripts ? p.TypeScript : p.Accent;
        }

        // A soft shadow under a floating card, outside the window's own rectangle.
        void Shadow(ImDrawList* draw, const ImRect& r, float rounding)
        {
            draw->PushClipRectFullScreen();
            for (int i = 6; i >= 1; --i)
            {
                const float s = Px(1.5f * float(i));
                draw->AddRectFilled({r.Min.x - s, r.Min.y - s + Px(3.0f)},
                                    {r.Max.x + s, r.Max.y + s + Px(3.0f)},
                                    IM_COL32(0, 0, 0, 14),
                                    rounding + s);
            }
            draw->PopClipRect();
        }
        // The card itself: raised tone, 1px hairline.
        void Card(ImDrawList* draw, const ImRect& r, float rounding, bool hovered)
        {
            Shadow(draw, r, rounding);
            draw->AddRectFilled(r.Min, r.Max, Color(EditorTheme::Colors().Raised), rounding);
            draw->AddRect(r.Min, r.Max, IM_COL32(255, 255, 255, hovered ? 56 : 26), rounding, 0, 1.0f);
        }
        // Three quarters of a circle, turning.
        void Spinner(ImDrawList* draw, ImVec2 center, float radius, ImU32 color)
        {
            const float start = float(std::fmod(ImGui::GetTime() * 6.0, 2.0 * IM_PI));
            draw->PathArcTo(center, radius, start, start + IM_PI * 1.5f, 16);
            draw->PathStroke(color, 0, Px(2.0f));
        }
        // A flat icon button that never takes focus (the item flag is pushed by Draw).
        bool IconButton(const char* id, const char* icon, ImVec2 at, float size, const char* tooltip, ImU32 color)
        {
            ImGui::SetCursorScreenPos(at);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 {0, 0, 0, 0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Colors().Pressed);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(4.0f));
            const bool clicked = ImGui::Button(id, {size, size});
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            Widgets::CenterGlyph(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), color);
            if (ImGui::IsItemHovered() && tooltip)
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }
        void Ellipsized(ImDrawList* draw, ImVec2 at, float maxX, const std::string& text, const ImVec4& color)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            const float line = ImGui::GetTextLineHeight();
            ImGui::RenderTextEllipsis(draw, at, {maxX, at.y + line}, maxX, text.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
    } // namespace

    float ProgressPopup::RowHeight() const
    {
        const float line = ImGui::GetTextLineHeight();
        return Px(EditorTheme::Space2) * 2.0f + line * 3.0f + Px(2.0f) + Px(EditorTheme::Space1);
    }

    void ProgressPopup::Draw(EditorProgress& progress, const std::optional<ProgressPopupHost>& host, Clock::time_point now)
    {
        m_Rect.reset();
        // Failures also go to the console, where "Show in Console" leads.
        for (const auto& [id, task] : progress.Tasks())
            if (task.State == EditorProgressState::Failed && m_Logged.insert(id).second)
                CORE_ERROR("{} failed: {}", task.Title, task.Error.empty() ? std::string("no details") : task.Error);
        for (auto it = m_Logged.begin(); it != m_Logged.end();)
            it = progress.Tasks().count(*it) ? std::next(it) : m_Logged.erase(it);

        m_Model.Update(progress.Tasks(), now);
        if (!m_Model.Visible())
            return;

        // Running first, then failures, then the finished ones; oldest first within each.
        std::vector<const EditorProgressTask*> rows;
        for (auto state : {EditorProgressState::Running, EditorProgressState::Failed})
            for (const auto& [id, task] : progress.Tasks())
                if (task.State == state)
                    rows.push_back(&task);
        for (const auto& [id, task] : progress.Tasks())
            if (task.State == EditorProgressState::Succeeded || task.State == EditorProgressState::Cancelled)
                rows.push_back(&task);

        const ImGuiViewport* main = ImGui::GetMainViewport();
        const ProgressUI::Rect viewport {main->WorkPos.x, main->WorkPos.y, main->WorkSize.x, main->WorkSize.y};
        std::optional<ProgressUI::Rect> panel;
        if (host)
            panel = ProgressUI::Rect {host->Rect.Min.x, host->Rect.Min.y, host->Rect.GetWidth(), host->Rect.GetHeight()};
        const float margin = Px(EditorTheme::Space2);
        const float avail  = (panel && panel->Width > 0 && panel->Height > 0 ? panel->Height : viewport.Height) - margin * 2.0f;

        float  width = 0.0f, height = 0.0f;
        size_t shown = rows.size();
        if (m_Model.Collapsed())
        {
            const std::string text = m_Model.PillText();
            width  = Px(EditorTheme::Space3) * 2.0f + ImGui::GetFontSize() * 2.0f + Px(EditorTheme::Space2) * 2.0f +
                    ImGui::CalcTextSize(text.c_str()).x;
            height = Px(PillHeight);
        }
        else
        {
            // As many rows as fit; the rest are counted in the footer.
            const float row    = RowHeight();
            const float footer = ImGui::GetTextLineHeight() + Px(EditorTheme::Space2);
            while (shown > 1 && Px(HeaderHeight) + row * float(shown) + (shown < rows.size() ? footer : 0.0f) > avail)
                --shown;
            width  = Px(CardWidth);
            height = Px(HeaderHeight) + row * float(shown) + (shown < rows.size() ? footer : 0.0f);
        }
        const auto r = ProgressUI::Anchor(panel, viewport, width, height, margin);
        if (r.Width <= 0.0f || r.Height <= 0.0f)
            return;
        const ImRect rect {{r.X, r.Y}, {r.Right(), r.Bottom()}};

        ImGui::SetNextWindowPos(rect.Min);
        ImGui::SetNextWindowSize(rect.GetSize());
        ImGui::SetNextWindowViewport(host && host->Viewport ? host->Viewport : main->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(Rounding));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
        ImGui::Begin("##EditorProgress", nullptr, flags);
        ImGui::PopStyleVar(3);
        // Above the panels it sits on, also after one of them was clicked.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true); // Clicks act without focusing the popup.
        if (m_Model.Collapsed())
            DrawPill(rect);
        else
            DrawCard(progress, rect, {rows.begin(), rows.begin() + long(shown)}, rows.size() - shown, now);
        // Clicks on the card's empty parts land here rather than focusing the window.
        ImGui::SetCursorScreenPos(rect.Min);
        ImGui::InvisibleButton("##Background", rect.GetSize());
        ImGui::PopItemFlag();
        ImGui::End();
        m_Rect = rect;
    }

    void ProgressPopup::DrawPill(const ImRect& pill)
    {
        const auto& p       = EditorTheme::Colors();
        auto*       draw    = ImGui::GetWindowDrawList();
        const bool  hovered = ImGui::IsMouseHoveringRect(pill.Min, pill.Max) && ImGui::IsWindowHovered();
        Card(draw, pill, pill.GetHeight() * 0.5f, hovered);
        const float  pad = Px(EditorTheme::Space3), icon = ImGui::GetFontSize();
        const float  midY = (pill.Min.y + pill.Max.y) * 0.5f;
        const ImVec2 iconMin {pill.Min.x + pad, midY - icon * 0.5f};
        if (m_Model.Running())
            Spinner(draw, {iconMin.x + icon * 0.5f, midY}, icon * 0.4f, Color(p.Accent));
        else
            Widgets::CenterGlyph(m_Model.Failed() ? ICON_FA_CIRCLE_XMARK : ICON_FA_CIRCLE_CHECK,
                                 iconMin,
                                 {iconMin.x + icon, iconMin.y + icon},
                                 Color(m_Model.Failed() ? p.Danger : p.Success));
        const std::string text = m_Model.PillText();
        const float       x    = iconMin.x + icon + Px(EditorTheme::Space2);
        draw->AddText({x, midY - ImGui::GetTextLineHeight() * 0.5f}, Color(p.Text), text.c_str());
        const float chevron = x + ImGui::CalcTextSize(text.c_str()).x + Px(EditorTheme::Space2);
        Widgets::CenterGlyph(ICON_FA_CHEVRON_UP, {chevron, iconMin.y}, {chevron + icon, iconMin.y + icon}, Color(p.TextDim));
        ImGui::SetCursorScreenPos(pill.Min);
        if (ImGui::InvisibleButton("##Expand", pill.GetSize()))
            m_Model.ToggleCollapsed();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the tasks");
    }

    void ProgressPopup::DrawCard(EditorProgress&                               progress,
                                 const ImRect&                                 card,
                                 const std::vector<const EditorProgressTask*>& rows,
                                 size_t                                        hidden,
                                 Clock::time_point                             now)
    {
        const auto& p       = EditorTheme::Colors();
        auto*       draw    = ImGui::GetWindowDrawList();
        const bool  hovered = ImGui::IsMouseHoveringRect(card.Min, card.Max) && ImGui::IsWindowHovered();
        const float rounding = Px(Rounding), pad = Px(EditorTheme::Space3);
        Card(draw, card, rounding, hovered);
        draw->PushClipRect(card.Min, card.Max, true);

        // Header: what the card lists, and the collapse button.
        const float header = Px(HeaderHeight);
        const float line   = ImGui::GetTextLineHeight();
        std::string title  = "Background tasks";
        if (m_Model.Running())
            title += "  \xc2\xb7  " + std::to_string(m_Model.Running()) + " running";
        ImGui::PushFont(BoldFont(), 0.0f);
        draw->AddText({card.Min.x + pad, card.Min.y + (header - line) * 0.5f}, Color(p.Text), title.c_str());
        ImGui::PopFont();
        const float button = Px(22.0f);
        if (IconButton("##Collapse",
                       ICON_FA_CHEVRON_DOWN,
                       {card.Max.x - pad + Px(4.0f) - button, card.Min.y + (header - button) * 0.5f},
                       button,
                       "Collapse to a pill",
                       Color(p.TextDim)))
            m_Model.ToggleCollapsed();
        draw->AddLine({card.Min.x, card.Min.y + header - 0.5f}, {card.Max.x, card.Min.y + header - 0.5f}, IM_COL32(255, 255, 255, 18));

        float y = card.Min.y + header;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            ImGui::PushID(int(rows[i]->ID));
            if (i > 0)
                draw->AddLine({card.Min.x + pad, y}, {card.Max.x - pad, y}, IM_COL32(255, 255, 255, 14));
            DrawRow(progress, *rows[i], {card.Min.x, y}, card.GetWidth(), now);
            ImGui::PopID();
            y += RowHeight();
        }
        if (hidden)
        {
            const std::string more = "+" + std::to_string(hidden) + (hidden == 1 ? " more task" : " more tasks");
            draw->AddText({card.Min.x + pad, y + Px(EditorTheme::Space1)}, Color(p.TextDim), more.c_str());
        }
        draw->PopClipRect();
    }

    void ProgressPopup::DrawRow(EditorProgress& progress, const EditorProgressTask& task, ImVec2 min, float width, Clock::time_point now)
    {
        const auto& p      = EditorTheme::Colors();
        auto*       draw   = ImGui::GetWindowDrawList();
        const auto  text   = ProgressUI::FormatRow(task, now);
        const float pad    = Px(EditorTheme::Space3), gap = Px(EditorTheme::Space2);
        const float line   = ImGui::GetTextLineHeight();
        const float right  = min.x + width - pad;
        const float button = Px(22.0f);
        const bool  failed = task.State == EditorProgressState::Failed;
        if (failed)
            draw->AddRectFilled(min, {min.x + width, min.y + RowHeight()}, Color(p.Danger, 0.08f));

        // Title line: category icon and title; elapsed time and the row's action on the right.
        float       y    = min.y + gap;
        const float icon = ImGui::GetFontSize();
        Widgets::CenterGlyph(CategoryIcon(task.Category),
                             {min.x + pad, y},
                             {min.x + pad + icon, y + line},
                             Color(CategoryColor(task.Category)));
        float actionX = right + Px(4.0f) - button;
        if (task.State == EditorProgressState::Running && task.CanCancel)
        {
            if (IconButton("##Cancel", ICON_FA_XMARK, {actionX, y + (line - button) * 0.5f}, button, "Cancel", Color(p.TextDim)))
                progress.Cancel(task.ID);
        }
        else if (failed)
        {
            if (IconButton("##Dismiss", ICON_FA_XMARK, {actionX, y + (line - button) * 0.5f}, button, "Dismiss", Color(p.TextDim)))
                progress.Dismiss(task.ID); // The row is gone from the next frame on.
        }
        else if (task.State == EditorProgressState::Succeeded)
            Widgets::CenterGlyph(ICON_FA_CIRCLE_CHECK, {actionX, y}, {actionX + button, y + line}, Color(p.Success));
        else if (task.State == EditorProgressState::Running)
            Spinner(draw, {actionX + button * 0.5f, y + line * 0.5f}, icon * 0.4f, Color(p.Accent));
        else
            actionX = right + gap; // Cancelled: no icon, the time moves right.
        const std::string elapsed = task.State == EditorProgressState::Cancelled ? "Cancelled  " + text.Elapsed : text.Elapsed;
        const float       elapsedW = ImGui::CalcTextSize(elapsed.c_str()).x;
        draw->AddText({actionX - gap - elapsedW, y}, Color(p.TextDim), elapsed.c_str());
        const float titleX = min.x + pad + icon + gap;
        ImGui::PushFont(BoldFont(), 0.0f);
        Ellipsized(draw, {titleX, y}, actionX - gap * 2.0f - elapsedW, task.Title, p.Text);
        ImGui::PopFont();

        // Detail line: the current file and step, and "3 of 12".
        y += line + Px(2.0f);
        const float countW = text.Count.empty() ? 0.0f : ImGui::CalcTextSize(text.Count.c_str()).x;
        if (!text.Count.empty())
            draw->AddText({right - countW, y}, Color(p.TextDim), text.Count.c_str());
        const float detailMax = right - (countW > 0 ? countW + gap : 0.0f);
        // Without a detail, the line says how the task ended.
        const std::string what = !task.Detail.empty()                          ? task.Detail :
                                 task.State == EditorProgressState::Failed    ? std::string("Failed") :
                                 task.State == EditorProgressState::Succeeded ? std::string("Done") :
                                                                                std::string();
        const auto        detail = Launcher::EllipsizeMiddle(what, detailMax - titleX, [](std::string_view s) {
            return ImGui::CalcTextSize(s.data(), s.data() + s.size()).x;
        });
        draw->AddText({titleX, y}, Color(p.TextDim), detail.c_str());

        // Bar, or the error and a way to the console.
        y += line + Px(EditorTheme::Space1);
        if (failed)
        {
            const char* show  = ICON_FA_TERMINAL "  Show in Console";
            const float showW = ImGui::CalcTextSize(show).x + gap;
            Ellipsized(draw, {titleX, y}, right - showW - gap, task.Error.empty() ? "Failed" : task.Error, p.Danger);
            ImGui::SetCursorScreenPos({right - showW, y - Px(1.0f)});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 {0, 0, 0, 0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.Pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, p.Accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {gap * 0.5f, Px(1.0f)});
            if (ImGui::Button(show) && m_ShowConsole)
                m_ShowConsole();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", task.Error.c_str());
            return;
        }
        const float  barH = Px(4.0f);
        const ImVec2 a {titleX, y + (line - barH) * 0.5f}, b {right, a.y + barH};
        draw->AddRectFilled(a, b, Color(p.Inset), barH * 0.5f);
        const ImVec4 fill = task.State == EditorProgressState::Succeeded ? p.Success :
                            task.State == EditorProgressState::Cancelled ? p.TextOff :
                                                                           p.Accent;
        if (text.Fraction)
        {
            if (*text.Fraction > 0.0f)
                draw->AddRectFilled(a, {a.x + (b.x - a.x) * *text.Fraction, b.y}, Color(fill), barH * 0.5f);
        }
        else if (task.State == EditorProgressState::Running)
        {
            // Indeterminate: a segment sliding across.
            const float phase = float(std::fmod(ImGui::GetTime() * 0.8, 1.0)) * 1.4f - 0.4f;
            const float from = std::clamp(phase, 0.0f, 1.0f), to = std::clamp(phase + 0.4f, 0.0f, 1.0f);
            if (to > from)
                draw->AddRectFilled({a.x + (b.x - a.x) * from, a.y}, {a.x + (b.x - a.x) * to, b.y}, Color(fill), barH * 0.5f);
        }
    }
} // namespace Kans
