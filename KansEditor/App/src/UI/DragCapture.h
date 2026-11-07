#pragma once
#include <imgui.h>

namespace Kans
{
    // The one "input captured by drag" gate the scene viewport and the editor's shortcuts both
    // respect. While a transform runs (a gizmo or handle drag, or a modal G/R/S) every key belongs
    // to it: X, Y and Z constrain it, Ctrl inverts snapping, Shift and Esc steer it, and no shortcut
    // fires until it ends. While a point handle that takes an axis key is under the cursor, X, Y and
    // Z (without Ctrl) are held for the drag that is about to start.
    struct DragCapture
    {
        bool Dragging      = false; // A gizmo or handle drag.
        bool Modal         = false; // A modal G/R/S transform.
        bool AxisHandleHot = false; // A point handle (axis keys pick its constraint) under the cursor.

        static bool IsAxisKey(ImGuiKey key) { return key == ImGuiKey_X || key == ImGuiKey_Y || key == ImGuiKey_Z; }
        bool        Active() const { return Dragging || Modal; }
        // Whether a press of `key` (with Ctrl held or not) goes to the drag instead of a shortcut.
        bool Captures(ImGuiKey key, bool ctrl) const
        {
            if (Active())
                return true;
            return AxisHandleHot && !ctrl && IsAxisKey(key);
        }
    };
} // namespace Kans
