#pragma once
#include <vector>
#include "Kans3D/Core/WindowGeometry.h"

namespace Kans
{
    // A rectangle in window client coordinates; Min is inside, Max is not.
    struct TitlebarRect
    {
        float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
        bool  Contains(float x, float y) const { return x >= MinX && x < MaxX && y >= MinY && y < MaxY; }
    };

    // What each part of a custom titlebar is to the window manager. A frame records the
    // band, its interactive items and the maximize button while it draws them, then
    // publishes; the window procedure hit-tests the last published layout, so it never
    // sees a half-built frame and never calls into the UI.
    class TitlebarRegions
    {
    public:
        // Starts recording a frame's band.
        void Begin(const TitlebarRect& band);
        // Menus, the icon, the minimize and close buttons and other items the UI handles.
        void AddClient(const TitlebarRect& rect);
        // The maximize button: Windows handles its hover and click (Snap Layouts).
        void SetMaximize(const TitlebarRect& rect);
        // Makes the recorded frame the one HitTest answers from.
        void Publish();
        // Forgets everything: no band, so every point is client.
        void Clear();

        // Outside the band: client. Over the maximize button: maximize. Over an
        // interactive item: client. Any other point of the band drags the window.
        TitlebarHit HitTest(float x, float y) const;

    private:
        struct Layout
        {
            bool                      HasBand = false;
            TitlebarRect              Band;
            bool                      HasMaximize = false;
            TitlebarRect              Maximize;
            std::vector<TitlebarRect> Clients;
        };
        Layout m_Recording, m_Published;
    };

    // Whether a window that draws its own titlebar outlines its edge with a hairline: only while
    // restored. A maximized window meets the screen edges and a minimized one has no client area.
    bool ShowsWindowEdge(bool customTitlebar, bool maximized, float width, float height);
} // namespace Kans
