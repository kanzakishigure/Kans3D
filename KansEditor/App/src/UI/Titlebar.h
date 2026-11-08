#pragma once
#include <imgui.h>
#include <functional>
#include "Kans3D/Core/Base/Base.h"
#include "Kans3D/Core/WindowGeometry.h"
#include "KansEditorCore/UI/TitlebarRegions.h"

namespace Kans
{
    class Window;
    class Texture2D;

    // The window's own titlebar where the platform allows one (Windows): one band with the
    // Kans3D icon at its start and the window buttons at its end, the caller's content in
    // between. While drawing, it records which parts are buttons and which drag the window,
    // and hands that to the window procedure through the hit test.
    class Titlebar
    {
    public:
        Titlebar();
        ~Titlebar();
        Titlebar(const Titlebar&)            = delete;
        Titlebar& operator=(const Titlebar&) = delete;

        // Takes over `window`'s titlebar hit test when the window draws its own titlebar.
        // False (and nothing registered) when it keeps the native one.
        bool Attach(Window& window);
        void Detach();
        bool IsCustom() const { return m_Window != nullptr; }

        // Starts a frame's band, in screen coordinates. `origin` is the main viewport's
        // position, the window's client origin.
        void Begin(ImVec2 min, ImVec2 max, ImVec2 origin);
        // The Kans3D icon at `x`, centred in the band; returns its right edge.
        float Icon(float x);
        // Marks a part of the band the UI handles (menus, buttons) rather than a drag area.
        void Client(ImVec2 min, ImVec2 max);
        void ClientItem() { Client(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }

        enum class Button
        {
            None,
            Minimize,
            Close
        };
        // Minimize, maximize (restore while `maximized`) and close at the band's right end.
        // Windows owns the maximize button (Snap Layouts), so it is drawn from `state` and
        // its clicks never reach here. Returns the button clicked.
        Button WindowButtons(bool maximized, TitlebarButtonState state);
        // Publishes the frame's regions to the hit test.
        void End();

        // Left edge of the window buttons in the current band.
        float ButtonsLeft() const { return m_Max.x - ButtonsWidth(); }
        static float ButtonsWidth();
        static ImVec2 ButtonSize(); // 46 x 32 at scale 1, as on Windows 11.

        const TitlebarRegions& Regions() const { return m_Regions; }

    private:
        TitlebarRect Local(ImVec2 min, ImVec2 max) const;

        Window*         m_Window = nullptr;
        TitlebarRegions m_Regions;
        ImVec2          m_Min {}, m_Max {}, m_Origin {};
        Ref<Texture2D>  m_Icon;
        bool            m_IconLoaded = false;
    };

    // The launcher's titlebar at the current window's top: icon, "Kans3D — Projects" and
    // the window buttons on an App-toned band; advances the cursor below it.
    Titlebar::Button DrawLauncherTitlebar(Titlebar& titlebar, bool maximized, TitlebarButtonState state);

    // Carries out a window button on the application's main window: Minimize minimizes,
    // Close asks to close the way the native close button does (unsaved changes first).
    void RunTitlebarButton(Titlebar::Button button);
} // namespace Kans
