#pragma once
#include <array>
#include <utility>
#include <glm/glm.hpp>
#include "Kans3D/Editor/EditorCamera.h"

// The view-axis widget in the viewport corner: a ball per world axis direction, turning with
// the camera. Clicking a ball looks along it (again: from the opposite side); dragging orbits.
namespace Kans::Gizmo
{
    struct AxisBall
    {
        int       Axis     = 0;
        bool      Negative = false;
        glm::vec2 Screen {0};
        float     Depth = 0.0f; // Larger is farther from the camera.
    };
    // The balls for a view matrix, `radius` pixels around `centre`, sorted back to front.
    std::array<AxisBall, 6> ViewAxisBalls(const glm::mat4& view, glm::vec2 centre, float radius);
    // The front-most ball within `ballRadius` of `mouse`: its index in `balls`, or -1.
    int PickViewAxisBall(const std::array<AxisBall, 6>& balls, glm::vec2 mouse, float ballRadius);
    // The view that looks at the focus from the ball's side (+Y: from above, looking down).
    EditorCamera::View BallView(int axis, bool negative);
    // Whether the camera already looks from that side (within about a degree).
    bool IsLookingFrom(const EditorCamera& camera, int axis, bool negative);
    // Yaw and pitch between two orientations at t (0..1, eased), turning the short way in yaw.
    std::pair<float, float> InterpolateView(std::pair<float, float> from, std::pair<float, float> to, float t);
} // namespace Kans::Gizmo
