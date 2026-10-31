#pragma once
#include "Kans3D/Timeline/Timeline.h"

// A compact editor for a 0..1 curve (TimelineCurve keys), drawn at the current cursor in the
// available width: the graph shows the first `components` channels; drag a key to move it,
// double-click to add one, right-click an inner key to remove it. A key list below edits the exact
// times and values. Returns true when the curve changed.
namespace Kans::CurveEditor
{
    // `start` and `end` label the graph's two ends (a chain's root and tip by default).
    bool Draw(const char* id, TimelineCurve& curve, int components, glm::vec4 fallback, float speed = 0.01f,
              const char* start = "Root", const char* end = "Tip");
} // namespace Kans::CurveEditor
