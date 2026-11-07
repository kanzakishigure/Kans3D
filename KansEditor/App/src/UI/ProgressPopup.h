#pragma once
#include <functional>
#include <optional>
#include <set>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>
#include "KansEditorCore/UI/ProgressPopupModel.h"

namespace Kans
{
    // Where the popup sits: a window's rectangle (above its status bar) and its viewport.
    struct ProgressPopupHost
    {
        ImRect  Rect;
        ImGuiID Viewport = 0;
    };

    // Long-running editor tasks (imports, script builds) in a card at the bottom right of the
    // Content Browser, or of the main viewport when that panel is not shown. It opens while a
    // task runs, closes a few seconds after all succeeded, and stays while a failure is not
    // dismissed. It never takes keyboard focus; clicks outside its card reach the windows below.
    class ProgressPopup
    {
    public:
        using Clock = std::chrono::steady_clock;

        // Draw after the panels, once a frame.
        void Draw(EditorProgress& progress, const std::optional<ProgressPopupHost>& host, Clock::time_point now = Clock::now());
        void SetShowConsole(std::function<void()> show) { m_ShowConsole = std::move(show); }

        const ProgressUI::Model& GetModel() const { return m_Model; }
        void                     ToggleCollapsed() { m_Model.ToggleCollapsed(); }
        // The card (or pill) drawn last frame; empty while closed.
        std::optional<ImRect> GetRect() const { return m_Rect; }

    private:
        float RowHeight() const;
        void  DrawRow(EditorProgress& progress, const EditorProgressTask& task, ImVec2 min, float width, Clock::time_point now);
        void  DrawCard(EditorProgress&                               progress,
                       const ImRect&                                 card,
                       const std::vector<const EditorProgressTask*>& rows,
                       size_t                                        hidden,
                       Clock::time_point                             now);
        void  DrawPill(const ImRect& pill);

        ProgressUI::Model          m_Model;
        std::function<void()>      m_ShowConsole;
        std::optional<ImRect>      m_Rect;
        std::set<EditorProgressID> m_Logged;
    };
} // namespace Kans
