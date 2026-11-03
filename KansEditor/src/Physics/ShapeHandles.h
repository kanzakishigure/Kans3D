#pragma once
#include <glm/glm.hpp>
#include "Gizmo/Handles.h"
#include "Kans3D/Physics/Shapes.h"

// The resize handles of the physics primitives on the generic handle API: box faces (Alt resizes
// both sides), sphere radii and capsule caps and radii, placed by `world` times the shape's
// offset. While a handle is dragged `shape` receives the resized copy; a cancel writes back the
// shape the drag started from, bit for bit. Colliders, motion colliders and ragdoll bodies share
// them.
namespace Kans::ShapeHandles
{
    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world, Physics::Box& shape,
                             glm::vec4 color);
    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world,
                             Physics::Sphere& shape, glm::vec4 color);
    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world,
                             Physics::Capsule& shape, glm::vec4 color);
    // The shape's frame without its offset's position: the offset's position is then a point in
    // it, so the box and capsule centres move inside one fixed frame.
    glm::mat4 RotationScaleFrame(const glm::mat4& world, const Physics::ShapeOffset& offset);
} // namespace Kans::ShapeHandles
