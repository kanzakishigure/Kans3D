#pragma once
#include <array>
#include <string>
#include <vector>
#include "KansEditorCore/Gizmo/TransformGizmo.h"

// Keyboard-driven transforms in the manner of Blender 4.x: G, R or S starts moving, rotating
// or scaling the selection from where the pointer is (no click, no hit test); X, Y or Z then
// locks an axis (again: the other orientation, a third time: free), Shift+X/Y/Z locks the plane
// of the other two, a middle drag picks the axis nearest the pointer's motion, typed numbers
// replace the pointer, Shift slows the pointer to a tenth and Ctrl inverts snapping. A left
// click or Enter confirms, a right click or Esc cancels. ImGui-free: the viewport feeds it the
// pointer and the keys of each frame and applies the delta it reports.
namespace Kans::Gizmo
{
    // A key pressed while modal. Keys are the uppercase letters G, R, S, X, Y, Z and E, the digits,
    // '-', '.', '\t' (Tab) and '\b' (Backspace).
    struct ModalKey
    {
        char Key   = 0;
        bool Shift = false;
    };

    struct ModalInput
    {
        glm::vec2             Mouse {0};
        bool                  Shift = false; // Held: precision (a tenth of the pointer's travel).
        bool                  Ctrl  = false; // Held: inverts the snap setting.
        bool                  Confirm = false; // Left press or Enter.
        bool                  Cancel  = false; // Right press or Esc.
        bool                  MiddlePressed = false, MiddleDown = false;
        std::vector<ModalKey> Keys; // In the order typed.
    };

    // The axis lock: one axis, or the plane excluding it, in the first space (the toolbar's
    // orientation) or the second (global when the toolbar is not, otherwise local).
    struct ModalConstraint
    {
        int  Axis  = -1; // -1: free.
        bool Plane = false;
        int  Space = 0; // 0: the toolbar orientation; 1: the other one.
        bool IsFree() const { return Axis < 0; }
    };

    class ModalTransform
    {
    public:
        // Starts `op` from `mouse`. `pivot` holds the position and the toolbar orientation's axes;
        // `orientation` names that orientation and `local` is the active object's (or the
        // selected elements') frame, the alternative a second key press switches to.
        void Start(Operation op, const View& view, const Pivot& pivot, Orientation orientation, const glm::mat3& local,
                   const Settings& settings, glm::vec2 mouse);
        // One frame: keys, the pointer, confirm or cancel. Dragged while it goes on (the delta is
        // from the start), Committed once with the final delta, Cancelled with the identity.
        Event Update(const View& view, const ModalInput& input);
        // Drops the operation without an event.
        void Reset() { m_Active = false; }

        bool                   IsActive() const { return m_Active; }
        Operation              GetOperation() const { return m_Op; }
        bool                   IsTrackball() const { return m_Trackball; }
        const ModalConstraint& GetConstraint() const { return m_Constraint; }
        bool                   IsNumeric() const;
        bool                   IsSnapping() const { return m_Snap; }
        const Delta&           GetDelta() const { return m_Delta; }
        // The constrained axes in a space (0: the toolbar orientation, 1: the other one).
        glm::mat3 SpaceAxes(int space) const { return space == 0 ? m_Primary : m_Secondary; }
        const char* SpaceName(int space) const;
        // The typed value of a component as text ("-2.5"); empty when nothing was typed.
        std::string NumericText(int component) const;

        // The status line, like Blender's header: the operation, the lock, the value, and whether
        // the value is typed.
        std::string Status() const;
        // The value near the cursor ("+2.500", "90.0°", "x2.000").
        std::string Readout() const;
        glm::vec2   ReadoutPosition() const { return m_Mouse + glm::vec2(18.0f, 14.0f); }
        // Guide lines for the lock, and a dashed line from the pivot to the cursor (rotate and scale).
        void AppendDraws(std::vector<GizmoDraw>& out) const;

    private:
        struct Component
        {
            std::string Text;
            bool        Negative = false;
            bool        Typed() const { return !Text.empty() || Negative; }
            float       Value(float fallback) const;
        };
        void Restart(Operation op, glm::vec2 mouse);
        void Key(const ModalKey& key);
        void LockAxis(int axis, bool plane);
        int  ComponentCount() const;
        void Compute();
        // The constrained directions in world space: the axis, or the plane's two.
        glm::mat3 ConstraintAxes() const { return SpaceAxes(m_Constraint.Space); }
        glm::vec3 TowardViewer() const;

        bool        m_Active = false, m_Trackball = false, m_Snap = false;
        Operation   m_Op = Operation::None;
        View        m_View;
        Pivot       m_Pivot;
        Orientation m_Orientation = Orientation::World;
        glm::mat3   m_Primary {1}, m_Secondary {1};
        Settings    m_Settings;
        ModalConstraint m_Constraint;

        // The pointer: where the operation started, the last real position and the virtual one
        // (the start plus the travel, a tenth of it while Shift is held).
        glm::vec2 m_StartMouse {0}, m_LastMouse {0}, m_Virtual {0}, m_Mouse {0};
        // Rotation: the swept screen angle (unwrapped) and the last raw angle.
        float m_Sweep = 0.0f, m_LastRaw = 0.0f;
        // Middle drag: where it started and the lock before it.
        bool      m_MiddlePicking = false;
        glm::vec2 m_MiddleStart {0};

        std::array<Component, 3> m_Numeric {};
        int                      m_Field = 0; // The component Tab moved to.
        Delta                    m_Delta;
    };
} // namespace Kans::Gizmo
