#include "WindowGeometry.h"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace Kans
{
    WindowPlacement ClampWindowPlacement(WindowPlacement p, const std::vector<MonitorWorkArea>& areas)
    {
        p.Width                             = std::max(1, p.Width);
        p.Height                            = std::max(1, p.Height);
        const MonitorWorkArea* best         = nullptr;
        int64_t                bestOverlap  = -1;
        double                 bestDistance = std::numeric_limits<double>::max();
        for (const auto& area : areas)
        {
            if (area.Width <= 0 || area.Height <= 0)
                continue;
            const int64_t overlapX = std::max<int64_t>(
                0, std::min<int64_t>(int64_t(p.X) + p.Width, int64_t(area.X) + area.Width) - std::max(p.X, area.X));
            const int64_t overlapY = std::max<int64_t>(
                0, std::min<int64_t>(int64_t(p.Y) + p.Height, int64_t(area.Y) + area.Height) - std::max(p.Y, area.Y));
            const double dx      = double(p.X) + p.Width / 2.0 -
                                   std::clamp(double(p.X) + p.Width / 2.0, double(area.X), double(area.X) + area.Width);
            const double dy      = double(p.Y) - std::clamp(double(p.Y), double(area.Y), double(area.Y) + area.Height);
            const auto   overlap = overlapX * overlapY;
            const double distance = dx * dx + dy * dy;
            if (overlap > bestOverlap || (overlap == bestOverlap && distance < bestDistance))
            {
                best         = &area;
                bestOverlap  = overlap;
                bestDistance = distance;
            }
        }
        if (best)
        {
            p.Width          = std::min(p.Width, best->Width);
            p.Height         = std::min(p.Height, best->Height);
            const int stripX = std::min(32, p.Width), stripY = std::min(32, p.Height);
            // Shrunk windows fit entirely; otherwise preserve intentional partial placement.
            p.X = int(
                std::clamp<int64_t>(p.X, int64_t(best->X) - p.Width + stripX, int64_t(best->X) + best->Width - stripX));
            p.Y = int(std::clamp<int64_t>(p.Y, best->Y, int64_t(best->Y) + best->Height - stripY));
            if (p.Width == best->Width)
                p.X = best->X;
            if (p.Height == best->Height)
                p.Y = best->Y;
        }
        return p;
    }

    int TitlebarHitTestCode(TitlebarHit hit,
                            int         left,
                            int         top,
                            int         right,
                            int         bottom,
                            int         borderX,
                            int         borderY,
                            bool        maximized)
    {
        if (left < 0 || top < 0 || right < 0 || bottom < 0)
            return 1; // HTCLIENT
        if (!maximized)
        {
            const bool l = left < borderX, r = right < borderX;
            const bool t = top < borderY, b = bottom < borderY;
            if (t && l)
                return 13; // HTTOPLEFT
            if (t && r)
                return 14; // HTTOPRIGHT
            if (b && l)
                return 16; // HTBOTTOMLEFT
            if (b && r)
                return 17; // HTBOTTOMRIGHT
            if (l)
                return 10; // HTLEFT
            if (r)
                return 11; // HTRIGHT
            if (t)
                return 12; // HTTOP
            if (b)
                return 15; // HTBOTTOM
        }
        if (hit == TitlebarHit::Caption)
            return 2; // HTCAPTION
        if (hit == TitlebarHit::Maximize)
            return 9; // HTMAXBUTTON (Snap Layouts)
        return 1;     // The UI clicks Minimize and Close itself.
    }
} // namespace Kans
