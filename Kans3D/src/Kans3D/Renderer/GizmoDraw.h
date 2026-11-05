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
    //
    // Arc, Sector and Ticks are flat, unlit and shaped by GizmoDraw::Params, about the Z axis of
    // their transform with radius 1 (angles in radians from +X toward +Y):
    // - Arc: a band of constant screen thickness along the circle from Params.y over Params.z,
    //   Params.w pixels to each side of the circle;
    // - Sector: a filled pie slice from Params.y over Params.z;
    // - Ticks: Params.w short radial marks just outside the circle, from Params.y every Params.z.
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
        Circle,
        Arc,
        Sector,
        Ticks
    };
    inline constexpr uint32_t GizmoShapeCount = 13;
    // The most marks one Ticks draw shows.
    inline constexpr uint32_t GizmoMaxTicks = 360;
    inline constexpr bool     IsGizmoLineShape(GizmoShape shape)
    {
        return shape == GizmoShape::Line || shape == GizmoShape::Square || shape == GizmoShape::Circle;
    }
    inline constexpr bool IsGizmoFlatShape(GizmoShape shape)
    {
        return shape == GizmoShape::Arc || shape == GizmoShape::Sector || shape == GizmoShape::Ticks;
    }
    // One handle part: a unit shape placed in the world, in display-space colour (alpha blends).
    // Solid shapes are lightly shaded with a thin dark silhouette around them, unless Flat (drawn
    // in their plain colour, with no outline); Arc and Ticks get the silhouette too.
    struct GizmoDraw
    {
        GizmoShape Shape = GizmoShape::Cube;
        glm::mat4  Transform {1};
        glm::vec4  Color {1};
        glm::vec4  Params {0}; // Arc, Sector and Ticks (see GizmoShape).
        bool       Flat = false;
    };
} // namespace Kans
