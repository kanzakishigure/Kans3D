#include "ShapeHandles.h"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Physics/PhysicsEditing.h"

namespace Kans::ShapeHandles
{
    namespace
    {
        Gizmo::HandleStyle Style(glm::vec4 color)
        {
            Gizmo::HandleStyle style;
            style.Color = color;
            style.Knob  = Gizmo::KnobShape::Cube;
            return style;
        }
        // The offset's position as a point of RotationScaleFrame, and back.
        glm::vec3 CentreOf(const Physics::ShapeOffset& offset)
        {
            return (glm::inverse(offset.Rotation) * offset.Position) / offset.Scale;
        }
        glm::vec3 PositionOf(const Physics::ShapeOffset& offset, glm::vec3 centre)
        {
            return offset.Rotation * (offset.Scale * centre);
        }
        // Keeps the start for a bit-exact cancel and writes it back on one.
        template<typename T>
        bool Restore(Gizmo::HandleContext& context, const Gizmo::HandleResult& r, const T& current, T& shape)
        {
            if (r.Started)
                context.Stash(current);
            if (r.Cancelled)
            {
                if (const T* start = context.Stashed<T>())
                    shape = *start;
                return true;
            }
            return false;
        }
    } // namespace

    glm::mat4 RotationScaleFrame(const glm::mat4& world, const Physics::ShapeOffset& offset)
    {
        return world * glm::mat4_cast(offset.Rotation) * glm::scale(glm::mat4(1.0f), offset.Scale);
    }

    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world, Physics::Box& shape,
                             glm::vec4 color)
    {
        const Physics::Box current = shape;
        Gizmo::BoxHandle   handle;
        handle.Frame   = RotationScaleFrame(world, shape.Offset);
        handle.MinHalf = PhysicsEditing::MinimumSize;
        handle.Style   = Style(color);
        glm::vec3  centre = CentreOf(shape.Offset), half = glm::abs(shape.HalfExtents);
        const auto r      = context.Box(id, handle, centre, half);
        if (Restore(context, r, current, shape) || !r.Changed)
            return r;
        shape.HalfExtents     = half;
        shape.Offset.Position = PositionOf(shape.Offset, centre);
        return r;
    }

    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world,
                             Physics::Sphere& shape, glm::vec4 color)
    {
        const Physics::Sphere current = shape;
        Gizmo::SphereHandle   handle;
        handle.Frame     = world * shape.Offset.Matrix();
        handle.MinRadius = PhysicsEditing::MinimumSize;
        handle.Style     = Style(color);
        float      radius = std::abs(shape.Radius);
        const auto r      = context.Sphere(id, handle, radius);
        if (Restore(context, r, current, shape) || !r.Changed)
            return r;
        shape.Radius = radius;
        return r;
    }

    Gizmo::HandleResult Edit(Gizmo::HandleContext& context, Gizmo::HandleId id, const glm::mat4& world,
                             Physics::Capsule& shape, glm::vec4 color)
    {
        const Physics::Capsule current = shape;
        Gizmo::CapsuleHandle   handle;
        handle.Frame     = RotationScaleFrame(world, shape.Offset);
        handle.Axis      = int(shape.Direction);
        handle.MinRadius = PhysicsEditing::MinimumSize;
        handle.Style     = Style(color);
        glm::vec3  centre = CentreOf(shape.Offset);
        float      radius = std::abs(shape.Radius), half = std::abs(shape.HalfHeight);
        const auto r      = context.Capsule(id, handle, centre, radius, half);
        if (Restore(context, r, current, shape) || !r.Changed)
            return r;
        shape.Radius          = radius;
        shape.HalfHeight      = half;
        shape.Offset.Position = PositionOf(shape.Offset, centre);
        return r;
    }
} // namespace Kans::ShapeHandles
