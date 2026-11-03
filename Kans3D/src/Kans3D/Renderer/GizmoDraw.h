#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace Kans
{
    // Unit shapes of editor handles (the TransformGizmo render feature builds each once).
    // Solids are in their own space: Cylinder and Cone run from z = 0 to z = 1 with radius 1
    // (the cone's apex at z = 1), Cube and Sphere fill [-1, 1], Quad and Disc lie in the XY
    // plane with radius 1 and both faces, and Torus circles the Z axis with radius 1.
    // Line runs from the origin to (0, 0, 1); Square outlines the Quad; Circle outlines the Disc.
    enum class GizmoShape : uint8_t
    {
        Cylinder,
        Cone,
        Cube,
        Quad,
        Torus,
        Sphere,
        Disc,
        Line,
        Square,
        Circle
    };
    inline constexpr uint32_t GizmoShapeCount = 10;
    inline constexpr bool     IsGizmoLineShape(GizmoShape shape)
    {
        return shape == GizmoShape::Line || shape == GizmoShape::Square || shape == GizmoShape::Circle;
    }
    // One handle part: a unit shape placed in the world, in display-space colour (alpha blends).
    struct GizmoDraw
    {
        GizmoShape Shape = GizmoShape::Cube;
        glm::mat4  Transform {1};
        glm::vec4  Color {1};
    };
} // namespace Kans
