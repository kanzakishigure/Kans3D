#pragma once
#include <vector>

namespace Kans
{
    struct WindowSize
    {
        int Width, Height;
    };
    struct WindowPosition
    {
        int X, Y;
    };
    struct WindowPlacement
    {
        int  X = 0, Y = 0, Width = 1920, Height = 1080;
        bool Maximized = false;
    };
    struct MonitorWorkArea
    {
        int X, Y, Width, Height;
    };

    // Keeps a draggable 32 x 32 strip at the top reachable, shrinks oversized rectangles,
    // preserves negative desktop coordinates and the requested maximized state.
    // Invalid work areas are ignored; with none, only positive dimensions are enforced.
    WindowPlacement ClampWindowPlacement(WindowPlacement placement, const std::vector<MonitorWorkArea>& workAreas);

    enum class TitlebarMode
    {
        Native,
        Custom
    };
    enum class TitlebarHit
    {
        Client,
        Caption,
        Minimize,
        Maximize,
        Close
    };
    struct TitlebarButtonState
    {
        bool MaximizeHovered = false;
        bool MaximizePressed = false;
    };

    // Distances from the pointer to the outer rectangle's edges, in the same units
    // as borderX/Y. Returns documented Win32 HT* numeric values without Win32 headers.
    // Out-of-window points are client; maximized windows have no resize borders.
    int TitlebarHitTestCode(TitlebarHit hit,
                            int         left,
                            int         top,
                            int         right,
                            int         bottom,
                            int         borderX,
                            int         borderY,
                            bool        maximized);
} // namespace Kans
