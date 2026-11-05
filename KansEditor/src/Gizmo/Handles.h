#pragma once
#include <any>
#include <cstdint>
#include <optional>
#include <vector>
#include "Gizmo/TransformGizmo.h"

// Editor handles for authored values, declared each frame in immediate-mode style with stable
// IDs: a panel declares its handles between Begin and End with the current values and gets the
// edited values back. One HandleContext per viewport resolves which handle (or other
// participant, such as the transform gizmo) is under the pointer: the nearest in depth, then the
// highest priority, then the closest on the screen, within the gizmo's pick radius. Pressing
// the hot handle starts a drag that runs from the values and geometry it started with; release
// commits it (one undo step for the caller), Esc or a right click cancels it and writes the
// start value back exactly. ImGui-free; handles draw as GizmoDraw items (a stem line and a knob
// of constant screen size) through the TransformGizmo render feature.
namespace Kans::Gizmo
{
    using HandleId = uint64_t;
    // A stable ID from a scope (an owner, a component) and an index inside it.
    constexpr HandleId MakeHandleId(uint64_t scope, uint64_t index)
    {
        return (scope * 0x9E3779B97F4A7C15ull) ^ (index + 0x632BE59BD9B4E019ull + (scope << 6) + (scope >> 2));
    }

    struct HandleResult
    {
        bool Hovered   = false; // The hot handle (the press would grab it).
        bool Active    = false; // Being dragged.
        bool Started   = false; // The drag began this frame (the value is still the start value).
        bool Changed   = false; // The value was written this frame.
        bool Committed = false; // Released: the drag is one undo step.
        bool Cancelled = false; // The start value was written back.
        HandleResult& operator|=(const HandleResult& other);
    };

    enum class KnobShape : uint8_t
    {
        Sphere,
        Cube
    };
    struct HandleStyle
    {
        glm::vec4 Color {1.0f};
        KnobShape Knob = KnobShape::Sphere;
        float     KnobPixels = 5.0f; // Knob radius on the screen.
        bool      Stem       = true; // A line from the handle's origin (or StemFrom) to the knob.
        // False: drawn only, never hot (handles shown read-only).
        bool      Interactive = true;
    };

    // A point dragged freely on the view plane, along a line or on a plane.
    struct PointHandle
    {
        enum class Constraint : uint8_t
        {
            Screen, // The plane through the point facing the camera.
            Line,   // Along Axis.
            Plane   // On the plane whose normal is Axis.
        };
        Constraint  Mode = Constraint::Screen;
        glm::vec3   Axis {0, 0, 1};
        float       Snap   = 0.0f;  // World units, per constrained axis of the drag (relative).
        bool        SnapOn = false; // The toolbar setting (Ctrl inverts it).
        HandleStyle Style;
    };

    // A scalar along a direction from an origin: the knob sits at Origin + Direction * value, so
    // the value is in units of Direction's length. Radii, lengths, extents and falloffs.
    struct LinearHandle
    {
        glm::vec3                Origin {0}, Direction {1, 0, 0};
        std::optional<float>     Min, Max;
        float                    Snap = 0.0f; // Rounds the value to multiples (Ctrl inverts SnapOn).
        bool                     SnapOn = false;
        std::optional<glm::vec3> StemFrom; // Where the stem starts (Origin when empty).
        HandleStyle              Style;
    };

    // An angle in degrees about Axis, measured from Reference toward Axis x Reference; the knob
    // sits at Radius on that circle. Cone angles, swing and twist limits.
    struct AngleHandle
    {
        glm::vec3            Center {0}, Axis {0, 0, 1}, Reference {1, 0, 0};
        float                Radius = 1.0f;
        std::optional<float> Min, Max;
        float                Snap = 0.0f; // Degrees (Ctrl inverts SnapOn).
        bool                 SnapOn = false;
        HandleStyle          Style;
    };

    // Six face knobs of a box in Frame's space (Frame may scale): a face drags alone (the opposite
    // face stays) or, symmetric, both faces about the centre. Alt toggles between the two.
    struct BoxHandle
    {
        glm::mat4   Frame {1.0f};
        float       MinHalf   = 0.001f;
        bool        Symmetric = false; // The default; Alt flips it.
        bool        LockCenter = false; // Always symmetric (shapes centred on their entity).
        float       Snap   = 0.0f;  // Rounds the dragged face's travel (Ctrl inverts SnapOn).
        bool        SnapOn = false;
        HandleStyle Style;
    };
    // A radius knob on each side of each axis of Frame (LinearHandles from the centre).
    struct SphereHandle
    {
        glm::mat4   Frame {1.0f};
        float       MinRadius = 0.001f;
        float       Snap   = 0.0f; // Rounds the radius (Ctrl inverts SnapOn).
        bool        SnapOn = false;
        HandleStyle Style;
    };
    // A capsule along Frame's axis Axis: two cap knobs (each moves its end alone, or both with
    // Alt) and four radius knobs around it.
    struct CapsuleHandle
    {
        glm::mat4   Frame {1.0f};
        int         Axis      = 1;
        float       MinRadius = 0.001f;
        HandleStyle Style;
    };

    class HandleContext
    {
    public:
        // Starts a frame. While `input.Allowed` is false nothing becomes hot or starts, but a
        // drag in progress continues. `settings` gives the pick radius, the gizmo size (for the
        // depth band) and Snap.
        void Begin(const View& view, const Input& input, const Settings& settings);
        HandleResult Point(HandleId id, const PointHandle& handle, glm::vec3& position);
        HandleResult Linear(HandleId id, const LinearHandle& handle, float& value);
        HandleResult Angle(HandleId id, const AngleHandle& handle, float& degrees);
        HandleResult Box(HandleId id, const BoxHandle& handle, glm::vec3& center, glm::vec3& halfExtents);
        HandleResult Sphere(HandleId id, const SphereHandle& handle, float& radius);
        HandleResult Capsule(HandleId id, const CapsuleHandle& handle, glm::vec3& center, float& radius, float& halfHeight);
        // Another participant under the pointer (the transform gizmo): it is hot when it wins.
        void Offer(HandleId id, float depth, float distance, int priority);
        // Resolves this frame's hover; the result holds until the next End.
        void End();

        HandleId Hot() const { return m_Hot; }
        HandleId GetActive() const { return m_Active; }
        bool     IsHot(HandleId id) const { return id != 0 && m_Hot == id; }
        bool     IsDragging() const { return m_Active != 0; }
        // Whether a press now goes to a handle (or the transform gizmo, when it was offered).
        bool     WantsPointer() const { return m_Hot != 0 || m_Active != 0; }
        // Drops a drag without writing anything (the edited object went away).
        void     Reset();

        // A copy of what a drag edits, kept from its start (for a bit-exact cancel).
        template<typename T>
        void Stash(const T& value)
        {
            m_Stash = value;
        }
        template<typename T>
        const T* Stashed() const
        {
            return std::any_cast<T>(&m_Stash);
        }

        // This frame's knobs and stems (the hot one brightened).
        void AppendDraws(std::vector<GizmoDraw>& out) const;
        // The knob's world radius at a point (constant on the screen).
        float KnobRadius(glm::vec3 at, float pixels) const;

        // Priorities at equal depth: handle knobs win over the transform gizmo's parts.
        static constexpr int KnobPriority = 5;

    private:
        struct Drag
        {
            // Geometry and values at the start: an origin and a direction (or an axis and a
            // reference for angles), the grab parameter, and up to six values for composites.
            glm::vec3 Origin {0}, Direction {0}, Axis {0}, Reference {0};
            float     Grab = 0.0f, Value = 0.0f, Radius = 0.0f;
            glm::vec3 Point {0};
            glm::mat4 Frame {1.0f};
            glm::vec3 Center {0}, Half {0};
            int       Part = -1;
            // Angle drags: the swept angle so far and the last raw one; edge-on rings use the screen.
            float     Sweep = 0.0f, LastRaw = 0.0f;
            bool      ScreenTangent = false;
            glm::vec2 Tangent {1, 0};
            glm::vec2 StartMouse {0};
            float     PixelsPerUnit = 1.0f;
        };
        struct Candidate
        {
            HandleId Id       = 0;
            float    Depth    = 0.0f;
            float    Distance = 0.0f;
            int      Priority = 0;
        };
        // Registers a knob at `at` for hover, and draws it (and its stem from `from`).
        void Knob(HandleId id, glm::vec3 at, std::optional<glm::vec3> from, const HandleStyle& style);
        // Starts, continues, ends or cancels the drag of `id`; returns the frame's flags.
        HandleResult Track(HandleId id);
        bool         SnapsNow(bool snapOn) const { return snapOn != m_Input.InvertSnap; }
        // Linear drag of a part: the value from the stored origin and direction.
        float LinearValue(const Drag& drag) const;

        View     m_View;
        Input    m_Input;
        Settings m_Settings;
        bool     m_InFrame = false;
        HandleId m_Hot = 0, m_Active = 0;
        Drag     m_Drag;
        std::any m_Stash;
        bool     m_Cancelling = false, m_ActiveSeen = false;
        std::vector<Candidate> m_Candidates;
        std::vector<GizmoDraw> m_Draws;
        struct KnobDraw
        {
            HandleId  Id;
            size_t    First, Count;
        };
        std::vector<KnobDraw> m_KnobDraws;
    };
} // namespace Kans::Gizmo
