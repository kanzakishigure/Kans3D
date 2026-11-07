#pragma once
#include "Panels/VfxSystemEditing.h"

// A colour gradient editor drawn at the cursor in the available width: a preview bar over a
// checkerboard, alpha stops above it and colour stops below it (drag to move, click to select,
// double-click the bar to add a stop where it was clicked, right-click a stop to remove it), the
// blend mode, and the selected stop's time and value. Returns true while the gradient changes;
// ImGui::IsAnyItemActive() stays true during a drag, so owners commit one undo step once it ends.
namespace Kans::GradientEditor
{
    using Gradient = VfxSystemEditing::Gradient;
    // Draws only the bar (a mini preview of `width` x `height` pixels at the cursor).
    void Preview(const Gradient& gradient, float width, float height);
    bool Draw(const char* id, Gradient& gradient);
} // namespace Kans::GradientEditor
