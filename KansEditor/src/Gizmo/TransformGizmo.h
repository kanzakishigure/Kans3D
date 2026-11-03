#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Renderer/GizmoDraw.h"

namespace Kans
{
    class EditorCamera;
}

// Kans3D's transform gizmo: hit testing, the drag state machine and the drag math, with no
// ImGui. The viewport feeds it the camera, the pointer and the pivot each frame and applies
// the delta it reports; the handles it describes are drawn by the TransformGizmo render feature.
//
// Handles keep a constant screen size (Settings::Size pixels of axis length, whatever the
// distance, projection or viewport height), point along the positive axes (they never flip
// toward the camera) and are picked within Settings::PickRadius pixels of their screen shape.
namespace Kans::Gizmo
{
    enum class Mode : uint8_t
    {
        Translate,
        Rotate,
        Scale,
        // Translate axes, planes and centre, rotation rings and scale cubes beyond the arrows.
        Combined
    };
    enum class Orientation : uint8_t
    {
        World,
        Local, // The active object's rotation.
        View   // Camera right, up and back.
    };
    enum class Handle : uint8_t
    {
        None,
        X,
        Y,
        Z,
        // Planes, named by the axes they move along.
        YZ,
        ZX,
        XY,
        // Screen-plane move (Translate, Combined) or uniform scale (Scale).
        Center,
        RingX,
        RingY,
        RingZ,
        RingView, // The outer ring, about the view direction.
        Trackball,
        // Combined mode's scale cubes.
        ScaleX,
        ScaleY,
        ScaleZ
    };
    enum class Operation : uint8_t
    {
        None,
        Translate,
        Rotate,
        Scale
    };
    enum class State : uint8_t
    {
        Idle,
        Hover,
        Dragging
    };
    enum class Event : uint8_t
    {
        None,
        Began,     // A press on a handle; the delta is the identity.
        Dragged,   // The pointer moved while dragging; the delta is from the start.
        Committed, // Released: apply the final delta (one undo step).
        Cancelled  // Esc or right click: restore the start state.
    };

    struct Ray
    {
        glm::vec3 Origin {0}, Direction {0, 0, -1};
    };

    // The camera and where its image is on the screen (pixels, y down).
    struct View
    {
        glm::mat4 ViewMatrix {1}, Projection {1};
        glm::vec2 Min {0}, Size {1};
        bool      Orthographic = false;

        glm::vec3 Position() const;
        glm::vec3 Forward() const;
        glm::vec3 Right() const;
        glm::vec3 Up() const;
        // Distance in front of the camera along its forward axis.
        float Depth(glm::vec3 point) const;
        // Screen position; empty behind the camera.
        std::optional<glm::vec2> ToScreen(glm::vec3 point) const;
        Ray                      RayAt(glm::vec2 screen) const;
        // The world length of one pixel at `point`'s depth.
        float UnitsPerPixel(glm::vec3 point) const;
        // The unit direction from the camera toward `point` (the forward axis when orthographic).
        glm::vec3 DirectionTo(glm::vec3 point) const;

        static View From(const EditorCamera& camera, glm::vec2 min, glm::vec2 size);
    };

    // Where the gizmo is: the pivot and its orthonormal axes (columns X, Y, Z).
    struct Pivot
    {
        glm::vec3 Position {0};
        glm::mat3 Axes {1};
    };

    struct Settings
    {
        float Size       = 110.0f; // Axis length in pixels.
        float PickRadius = 10.0f;  // Pixels around a handle's screen shape.
        // Axes and planes closer than this to edge-on cannot be picked; they fade in over twice it.
        float EdgeOnDegrees = 5.0f;
        bool  Snap          = false; // The toolbar setting; Input::InvertSnap flips it.
        float TranslateStep = 0.5f;  // World units.
        float RotateStep    = 15.0f; // Degrees.
        float ScaleStep     = 0.1f;  // Factor steps.
        // Exponential scale rate per pixel; 0 uses ln 2 / Size (one axis length doubles).
        float     ScaleRate = 0.0f;
        glm::vec4 AxisColors[3] {{0.92f, 0.25f, 0.27f, 1}, {0.45f, 0.80f, 0.18f, 1}, {0.24f, 0.52f, 0.96f, 1}};
        glm::vec4 ViewColor {0.86f, 0.86f, 0.86f, 1};
        glm::vec4 CenterColor {0.78f, 0.78f, 0.78f, 1};
        float     HoverBrighten = 0.45f; // Mix toward white.
        // Whether the drawing shows the negative half of each axis as a thin stub.
        bool NegativeStubs = true;
    };

    // The pointer this frame. `Allowed` is false when something else owns the pointer (another
    // widget, navigation); hovering and pressing then do nothing, but a drag continues.
    struct Input
    {
        glm::vec2 Mouse {0};
        bool      Down = false, Pressed = false, Released = false;
        bool      Cancel     = false; // Esc or a right click.
        bool      InvertSnap = false; // Ctrl.
        bool      Allowed    = true;
    };

    // The change since the drag began, about Pivot (the pivot at the start).
    struct Delta
    {
        Operation     Op     = Operation::None;
        Gizmo::Handle Handle = Gizmo::Handle::None;
        Gizmo::Pivot  Pivot;
        glm::vec3 Translation {0};          // World.
        glm::quat Rotation {1, 0, 0, 0};    // World, about Pivot.Position.
        float     Angle = 0.0f;             // Radians, signed about RotationAxis.
        glm::vec3 RotationAxis {0, 0, 1};
        glm::vec3 Scale {1};                // Along Pivot.Axes.
        bool      Snapped = false;
        bool      IsIdentity() const;
        // The world-space change as one matrix (rotation and scale about the pivot).
        glm::mat4 Matrix() const;
    };

    Operation OperationOf(Handle handle, Mode mode);
    // The pivot axes for an orientation: the active object's world transform (Local), the
    // camera (View) or the world axes. Scale in the result is removed.
    glm::mat3 ResolveAxes(Orientation orientation, const glm::mat4& activeWorld, const View& view);
    // Snapping relative to the drag's own start: the accumulated value rounded to steps.
    float SnapValue(float value, float step);
    // A scale factor snapped to steps from 1, keeping it positive and symmetric (1/x snaps like x).
    float SnapFactor(float factor, float step);
    // exp(rate * pixels): never zero or negative (the exponent is kept within +-80), and dragging
    // out and back returns 1.
    float ExponentialScale(float pixels, float rate);
    // The closest point on the line (origin, direction) to the ray: its parameter along the line.
    // Empty when they are parallel.
    std::optional<float> ClosestLineParameter(const Ray& ray, glm::vec3 origin, glm::vec3 direction);
    std::optional<glm::vec3> IntersectPlane(const Ray& ray, glm::vec3 point, glm::vec3 normal);

    class TransformGizmo
    {
    public:
        // One frame: hover, press, drag, release or cancel. The pivot is read while idle; a
        // drag keeps the pivot it started with.
        Event Update(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, const Input& input);
        // The handle under `mouse` (no state change).
        Handle HitTest(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, glm::vec2 mouse) const;
        // Starts a drag on `handle` at `mouse` without a hit test (G/R/S-style tools).
        void Begin(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, Handle handle, glm::vec2 mouse);
        // Drops a drag without an event (the selection went away).
        void Reset();

        State        GetState() const { return m_State; }
        bool         IsDragging() const { return m_State == State::Dragging; }
        bool         IsHovered() const { return m_State == State::Hover; }
        // Whether a left press now would start a drag (hovered or dragging).
        bool         WantsPointer() const { return m_State != State::Idle; }
        Handle       GetHovered() const { return m_Hovered; }
        Handle       GetActive() const { return m_Active; }
        const Delta& GetDelta() const { return m_Delta; }

        // The handles as of the last Update: all of them, or while dragging only the active
        // one with its guide lines. Empty before the first Update.
        void AppendDraws(std::vector<GizmoDraw>& out) const;
        // The drag's value ("1.25", "45.0°", "x1.50"); empty when not dragging.
        std::string Readout() const;
        glm::vec2   ReadoutPosition() const { return m_Mouse + glm::vec2(18.0f, 14.0f); }

        // The gizmo's world size (axis length) at a point.
        static float WorldSize(const View& view, glm::vec3 point, const Settings& settings);

    private:
        void Drag(glm::vec2 mouse);

        State    m_State   = State::Idle;
        Handle   m_Hovered = Handle::None, m_Active = Handle::None;
        Delta    m_Delta;
        // The last frame's inputs, for drawing.
        View     m_View;
        Pivot    m_Pivot;
        Mode     m_Mode = Mode::Translate;
        Settings m_Settings;
        bool     m_Snap = false, m_HasFrame = false;
        glm::vec2 m_Mouse {0};

        // The drag's start: pointer, world size, and the grab on the handle.
        glm::vec2 m_StartMouse {0};
        float     m_StartSize = 1.0f;
        glm::vec3 m_StartPoint {0}; // Plane hit, or the closest point on the axis.
        float     m_StartParameter = 0.0f;
        glm::vec3 m_Axis {1, 0, 0}, m_PlaneNormal {0, 0, 1};
        // Rotation: the grab direction in the ring plane, the angle so far (unwrapped), and the
        // screen-space fallback for rings seen edge-on.
        glm::vec3 m_StartVector {1, 0, 0};
        float     m_Angle = 0.0f, m_LastRaw = 0.0f;
        bool      m_ScreenTangent = false;
        glm::vec2 m_Tangent {1, 0};
        float     m_RadiusPixels = 100.0f;
        glm::vec3 m_ArcStart {0, 0, 1}; // Trackball, view space.
        glm::vec2 m_ScreenAxis {1, 0};  // Scale: the dragged direction on the screen.
    };
} // namespace Kans::Gizmo

namespace Kans
{
    using Gizmo::TransformGizmo;
}
