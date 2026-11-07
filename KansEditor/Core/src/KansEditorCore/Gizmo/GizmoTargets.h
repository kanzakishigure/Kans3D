#pragma once
#include <vector>
#include "KansEditorCore/Gizmo/TransformGizmo.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    class Scene;
}

namespace Kans::Gizmo
{
    enum class PivotMode : uint8_t
    {
        Median, // The mean of the targets' world positions.
        Active  // The active (primary) entity.
    };

    // The pivot for a selection: its position by `mode`, oriented by `orientation` from the
    // active entity (or the first target).
    Pivot SelectionPivot(const Scene& scene, const std::vector<Entity>& targets, Entity active, PivotMode mode,
                         Orientation orientation, const View& view);

    // Applies a gizmo delta to entities from the transforms they had when the drag began.
    // Translation moves every target; rotation and scale turn and scale their positions about
    // the pivot, and rotate or scale each target in its own frame, written back to its local
    // transform under its parent. Targets should not include each other's ancestors (children
    // follow their parents); see HierarchyPanel::TopMostSelected.
    class GizmoTargets
    {
    public:
        void Begin(Scene& scene, const std::vector<Entity>& targets);
        void Apply(const Delta& delta);
        // Every target back to its start transform, bit for bit.
        void Restore();
        void Clear() { m_Targets.clear(); }
        bool IsActive() const { return !m_Targets.empty(); }

    private:
        struct Target
        {
            Entity             Handle;
            TransformComponent Start;
            glm::mat4          World {1}, Parent {1};
        };
        std::vector<Target> m_Targets;
    };

    // Euler angles (TransformComponent's convention) of `rotation`, picked from the two
    // equivalent solutions and 2*pi turns to be nearest `reference`, so editing never jumps.
    glm::vec3 ContinuousEuler(const glm::quat& rotation, const glm::vec3& reference);
} // namespace Kans::Gizmo
