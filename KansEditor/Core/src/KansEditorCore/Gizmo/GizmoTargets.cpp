#include "KansEditorCore/Gizmo/GizmoTargets.h"
#include <cmath>
#include <limits>
#include <glm/gtc/constants.hpp>
#include "Kans3D/Scene/Scene.h"

namespace Kans::Gizmo
{
    namespace
    {
        // The rotation part of a matrix with its scale removed.
        glm::quat RotationOf(const glm::mat4& m)
        {
            return glm::normalize(glm::quat_cast(ResolveAxes(Orientation::Local, m, {})));
        }
        float Near(float angle, float reference)
        {
            const float tau = glm::two_pi<float>();
            return angle + tau * std::round((reference - angle) / tau);
        }
    } // namespace

    glm::vec3 ContinuousEuler(const glm::quat& rotation, const glm::vec3& reference)
    {
        const float     pi = glm::pi<float>();
        const glm::vec3 a  = glm::eulerAngles(rotation);
        const glm::vec3 b(a.x + pi, pi - a.y, a.z + pi);
        glm::vec3       best;
        float           bestDistance = std::numeric_limits<float>::max();
        for (const glm::vec3& candidate : {a, b})
        {
            // Only solutions that really give the rotation (the alternative depends on the order).
            if (std::abs(glm::dot(glm::quat(candidate), rotation)) < 1.0f - 1e-5f)
                continue;
            const glm::vec3 c(Near(candidate.x, reference.x), Near(candidate.y, reference.y), Near(candidate.z, reference.z));
            const float     distance = glm::length(c - reference);
            if (distance < bestDistance)
                best = c, bestDistance = distance;
        }
        return bestDistance < std::numeric_limits<float>::max() ? best : a;
    }

    Pivot SelectionPivot(const Scene& scene, const std::vector<Entity>& targets, Entity active, PivotMode mode,
                         Orientation orientation, const View& view)
    {
        Pivot pivot;
        if (targets.empty() && !active)
            return pivot;
        const Entity    primary = active ? active : targets.front();
        const glm::mat4 world   = scene.GetWorldTransform(primary);
        if (mode == PivotMode::Active || targets.empty())
            pivot.Position = glm::vec3(world[3]);
        else
        {
            glm::dvec3 sum(0.0);
            for (Entity e : targets)
                sum += glm::dvec3(scene.GetWorldTransform(e)[3]);
            pivot.Position = glm::vec3(sum / double(targets.size()));
        }
        pivot.Axes = ResolveAxes(orientation, world, view);
        return pivot;
    }

    void GizmoTargets::Begin(Scene& scene, const std::vector<Entity>& targets)
    {
        m_Targets.clear();
        for (Entity e : targets)
        {
            if (!e || !e.HasComponent<TransformComponent>())
                continue;
            const Entity parent = scene.GetParent(e);
            m_Targets.push_back({e, e.GetComponent<TransformComponent>(), scene.GetWorldTransform(e),
                                 parent ? scene.GetWorldTransform(parent) : glm::mat4(1.0f)});
        }
    }

    void GizmoTargets::Apply(const Delta& delta)
    {
        const glm::vec3 pivot = delta.Pivot.Position;
        const glm::mat3 axes  = delta.Pivot.Axes;
        for (auto& target : m_Targets)
        {
            const glm::vec3 start = glm::vec3(target.World[3]);
            glm::vec3       world = start;
            TransformComponent next = target.Start;
            switch (delta.Op)
            {
                case Operation::Translate:
                    world = start + delta.Translation;
                    break;
                case Operation::Rotate:
                {
                    world = pivot + delta.Rotation * (start - pivot);
                    // The world rotation turned, seen from the parent's frame.
                    const glm::quat parent = RotationOf(target.Parent);
                    const glm::quat local  = glm::inverse(parent) * delta.Rotation * parent * glm::quat(target.Start.Rotation);
                    next.Rotation          = ContinuousEuler(glm::normalize(local), target.Start.Rotation);
                    break;
                }
                case Operation::Scale:
                {
                    // Positions scale about the pivot along its axes; each target scales along its own.
                    const glm::vec3 offset = glm::transpose(axes) * (start - pivot);
                    world                  = pivot + axes * (offset * delta.Scale);
                    next.Scale             = target.Start.Scale * delta.Scale;
                    break;
                }
                case Operation::None:
                    continue;
            }
            if (world != start)
                next.Position = glm::vec3(glm::inverse(target.Parent) * glm::vec4(world, 1.0f));
            if (delta.Op == Operation::Rotate && delta.Rotation == glm::quat(1, 0, 0, 0))
                next = target.Start;
            target.Handle.PatchComponent<TransformComponent>([&](TransformComponent& tc) { tc = next; });
        }
    }

    void GizmoTargets::Restore()
    {
        for (auto& target : m_Targets)
            target.Handle.PatchComponent<TransformComponent>([&](TransformComponent& tc) { tc = target.Start; });
    }
} // namespace Kans::Gizmo
