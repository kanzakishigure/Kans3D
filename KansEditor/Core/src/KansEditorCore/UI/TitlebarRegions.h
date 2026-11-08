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

    // The project button of the editor's titlebar: the project name and a chevron with
    // `padding` on both sides and `gap` between them, `height` tall and centred in the band,
    // starting at `x`. Its rectangle is a control (Client); the band around it still drags.
    struct TitlebarProjectButton
    {
        TitlebarRect Rect;
        float        NameX = 0, ChevronX = 0;
    };
    TitlebarProjectButton LayoutProjectButton(float               x,
                                              const TitlebarRect& band,
                                              float               height,
                                              float               nameWidth,
                                              float               chevronWidth,
                                              float               padding,
                                              float               gap);

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
} // namespace Kans
