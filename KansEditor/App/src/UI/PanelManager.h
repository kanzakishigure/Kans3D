#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include "IniSection.h"

namespace Kans
{
    // Where a panel goes in the default dock layout.
    enum class PanelDock
    {
        Left,
        Right,
        Bottom,
        Center,
        Floating, // Not docked: opens as its own window (and can leave the main window).
    };

    // Groups panels in the Window menu.
    enum class PanelCategory
    {
        Scene,
        Assets,
        Debug,
    };

    struct PanelSpec
    {
        // Stable identity: persists the open state and the docking position, so the
        // title can change freely.
        std::string      ID;
        std::string      Title;
        const char*      Icon     = "";
        PanelCategory    Category = PanelCategory::Scene;
        PanelDock        Dock     = PanelDock::Right;
        bool             DefaultOpen = true;
        bool             Closable    = true;
        bool             NoPadding   = false;
        ImGuiWindowFlags Flags       = 0;
        const char*      Shortcut    = nullptr; // Shown in the Window menu only.
    };

    // Owns the editor's dockable windows. Each panel registers a function that
    // draws its contents; the manager opens the window around it, keeps the
    // open state (saved in imgui.ini), builds the default dock layout and lists
    // panels in the Window menu.
    class PanelManager
    {
    public:
        void Register(PanelSpec spec, std::function<void()> draw);

        void Open(const std::string& id, bool focus = true);
        void Close(const std::string& id);
        void Toggle(const std::string& id);
        bool IsOpen(const std::string& id) const;

        // Call inside the dock host window before ImGui::DockSpace. Builds the
        // default layout on first use or after ResetLayout().
        void PrepareDockSpace(ImGuiID dockspace, ImVec2 size);
        void ResetLayout() { m_ResetLayout = true; }

        void Draw();
        void DrawWindowMenu();

        // Saves which panels are open in imgui.ini. Install before the first
        // frame; detach before the panels are destroyed.
        void InstallSettingsHandler();
        void UninstallSettingsHandler() { m_Settings.Detach(); }

    private:
        struct Panel
        {
            PanelSpec             Spec;
            std::function<void()> Draw;
            bool                  Open         = true;
            bool                  FocusRequest = false;
        };
        Panel*       Find(const std::string& id);
        const Panel* Find(const std::string& id) const;
        std::string  WindowName(const Panel& panel) const;

        std::vector<Panel>                    m_Panels;
        std::unordered_map<std::string, bool> m_SavedOpen; // From imgui.ini, applied on registration.
        bool                                  m_ResetLayout = false;
        IniSection                            m_Settings;
    };
} // namespace Kans
