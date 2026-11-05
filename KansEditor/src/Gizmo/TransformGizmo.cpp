#include "TransformGizmo.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Editor/EditorCamera.h"

namespace Kans::Gizmo
{
    namespace
    {
        constexpr float Pi = glm::pi<float>();

        // Handle layout, in fractions of the axis length.
        // Arrows: a short shaft and a long tip, as in UE5 and Blender.
        constexpr float AxisStart = 0.12f, ArrowShaftEnd = 0.72f, ScaleShaftEnd = 0.86f;
        constexpr float ShaftRadius = 0.016f, ConeRadius = 0.07f;
        constexpr float ScaleCube = 0.92f, ScaleCubeHalf = 0.065f;
        constexpr float CombinedCube = 1.2f, CombinedCubeHalf = 0.05f;
        // Plane squares (Blender's): about a fifth of the axis on a side, clear of the centre and
        // the arrow roots; the scale ones further out, so the uniform centre stays free.
        constexpr float PlaneLow = 0.28f, PlaneHigh = 0.46f;
        constexpr float ScalePlaneLow = 0.40f, ScalePlaneHigh = 0.58f;
        constexpr float PlaneFillAlpha = 0.35f, PlaneHoverAlpha = 0.85f;
        constexpr float CenterRadius = 0.065f, ScaleCenterRadius = 0.1f;
        constexpr float RingRadius = 0.9f, CombinedRingRadius = 0.72f, ViewRingRadius = 1.1f;
        constexpr int   RingSamples = 96;
        // Rings closer than this to edge-on rotate by screen-space travel along their tangent.
        constexpr float RingEdgeOnSine = 0.17f; // About 10 degrees.

        int AxisOf(Handle h)
        {
            switch (h)
            {
                case Handle::X:
                case Handle::YZ:
                case Handle::RingX:
                case Handle::ScaleX:
                    return 0;
                case Handle::Y:
                case Handle::ZX:
                case Handle::RingY:
                case Handle::ScaleY:
                    return 1;
                case Handle::Z:
                case Handle::XY:
                case Handle::RingZ:
                case Handle::ScaleZ:
                    return 2;
                default:
                    return -1;
            }
        }
        bool IsAxis(Handle h) { return h == Handle::X || h == Handle::Y || h == Handle::Z; }
        bool IsPlane(Handle h) { return h == Handle::YZ || h == Handle::ZX || h == Handle::XY; }
        bool IsRing(Handle h) { return h == Handle::RingX || h == Handle::RingY || h == Handle::RingZ; }
        bool IsScaleCube(Handle h) { return h == Handle::ScaleX || h == Handle::ScaleY || h == Handle::ScaleZ; }

        // Tie-break order when handles are at the same depth: centre > planes > axes > rings.
        int Priority(Handle h)
        {
            if (h == Handle::Center)
                return 4;
            if (IsPlane(h))
                return 3;
            if (IsAxis(h) || IsScaleCube(h))
                return 2;
            if (IsRing(h) || h == Handle::RingView)
                return 1;
            return 0;
        }

        float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t)
        {
            const glm::vec2 ab = b - a;
            const float     l2 = glm::dot(ab, ab);
            t                  = l2 > 1e-12f ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + ab * t));
        }

        float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

        // A frame whose third column is `z`.
        glm::mat3 FrameAlong(glm::vec3 z)
        {
            z                = glm::normalize(z);
            const glm::vec3 helper = std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 x      = glm::normalize(glm::cross(helper, z));
            return glm::mat3(x, glm::cross(z, x), z);
        }
        glm::mat4 Place(glm::vec3 position, const glm::mat3& frame, glm::vec3 scale)
        {
            glm::mat4 m(1.0f);
            for (int i = 0; i < 3; ++i)
                m[i] = glm::vec4(frame[i] * scale[i], 0.0f);
            m[3] = glm::vec4(position, 1.0f);
            return m;
        }
        float WrapAngle(float a)
        {
            while (a > Pi)
                a -= 2.0f * Pi;
            while (a <= -Pi)
                a += 2.0f * Pi;
            return a;
        }

        float RingRadiusOf(Mode mode) { return mode == Mode::Combined ? CombinedRingRadius : RingRadius; }
        void  PlaneRange(Mode mode, float& low, float& high)
        {
            low  = mode == Mode::Scale ? ScalePlaneLow : PlaneLow;
            high = mode == Mode::Scale ? ScalePlaneHigh : PlaneHigh;
        }

        bool HasTranslate(Mode m) { return m == Mode::Translate || m == Mode::Combined; }
        bool HasPlanes(Mode m) { return m != Mode::Rotate; }

        // Visibility of an axis (1 = full) from the angle between it and the line of sight.
        float AxisVisibility(glm::vec3 axis, glm::vec3 sight, float edgeOnDegrees)
        {
            const float angle = glm::degrees(std::acos(glm::clamp(std::abs(glm::dot(axis, sight)), 0.0f, 1.0f)));
            return glm::clamp((angle - edgeOnDegrees) / std::max(edgeOnDegrees, 1e-3f), 0.0f, 1.0f);
        }
        // Visibility of a plane from the angle between it and the line of sight.
        float PlaneVisibility(glm::vec3 normal, glm::vec3 sight, float edgeOnDegrees)
        {
            const float angle = glm::degrees(std::asin(glm::clamp(std::abs(glm::dot(normal, sight)), 0.0f, 1.0f)));
            return glm::clamp((angle - edgeOnDegrees) / std::max(edgeOnDegrees, 1e-3f), 0.0f, 1.0f);
        }
        void PlaneAxes(int normalAxis, int& u, int& v)
        {
            u = (normalAxis + 1) % 3;
            v = (normalAxis + 2) % 3;
        }
        Handle PlaneHandle(int normalAxis) { return Handle(int(Handle::YZ) + normalAxis); }
        Handle AxisHandle(int axis) { return Handle(int(Handle::X) + axis); }
        Handle RingHandle(int axis) { return Handle(int(Handle::RingX) + axis); }
        Handle CubeHandle(int axis) { return Handle(int(Handle::ScaleX) + axis); }

        struct Candidate
        {
            Handle Id       = Handle::None;
            float  Depth    = 0.0f;
            float  Distance = 0.0f;
            int    Rank     = 0;     // The priority it competes with.
            bool   Margin   = false; // Only near the handle: it counts where nothing else is.
        };

        // The closest screen point of a ring's arc to `mouse`, its distance and its world point.
        float RingDistance(const View& view, glm::vec3 centre, const RingArc& arc, float radius, glm::vec2 mouse,
                           glm::vec3& closest)
        {
            float                    best = std::numeric_limits<float>::max();
            std::optional<glm::vec2> previous;
            glm::vec3                previousWorld {0};
            const int                samples = std::max(8, int(std::ceil(RingSamples * arc.HalfAngle / Pi)));
            for (int i = 0; i <= samples; ++i)
            {
                const float     a     = arc.HalfAngle * (2.0f * float(i) / samples - 1.0f);
                const glm::vec3 world = centre + (arc.Middle * std::cos(a) + arc.Side * std::sin(a)) * radius;
                const auto      at    = view.ToScreen(world);
                if (at && previous)
                {
                    float       t = 0;
                    const float d = SegmentDistance(mouse, *previous, *at, t);
                    if (d < best)
                        best = d, closest = glm::mix(previousWorld, world, t);
                }
                previous      = at;
                previousWorld = world;
            }
            return best;
        }
    } // namespace

    // ---------------------------------------------------------------------- rings

    bool RingArc::Contains(glm::vec3 radial) const
    {
        const float length = glm::length(radial);
        if (length < 1e-12f)
            return true;
        const float c = glm::dot(radial / length, Middle);
        const float s = glm::dot(radial / length, Side);
        return std::abs(std::atan2(s, c)) <= HalfAngle + 1e-5f;
    }

    RingArc FacingArc(glm::vec3 normal, glm::vec3 toCamera)
    {
        normal = glm::normalize(normal);
        // The camera direction within the ring's plane: its length b says how far the ring is
        // turned from facing the camera. Shown where b cos(angle from it) >= -tolerance.
        const glm::vec3 inPlane = toCamera - normal * glm::dot(toCamera, normal);
        const float     b       = glm::length(inPlane);
        RingArc         arc;
        if (b < 1e-6f)
        {
            const glm::mat3 frame = FrameAlong(normal);
            arc.Middle            = frame[0];
            arc.Side              = frame[1];
            arc.HalfAngle         = Pi;
            return arc;
        }
        arc.Middle    = inPlane / b;
        arc.Side      = glm::cross(normal, arc.Middle);
        arc.HalfAngle = std::acos(glm::clamp(-RingBackTolerance / b, -1.0f, 1.0f));
        return arc;
    }

    // ---------------------------------------------------------------------- camera

    glm::vec3 View::Position() const { return glm::vec3(glm::inverse(ViewMatrix)[3]); }
    glm::vec3 View::Forward() const { return -glm::normalize(glm::vec3(glm::inverse(ViewMatrix)[2])); }
    glm::vec3 View::Right() const { return glm::normalize(glm::vec3(glm::inverse(ViewMatrix)[0])); }
    glm::vec3 View::Up() const { return glm::normalize(glm::vec3(glm::inverse(ViewMatrix)[1])); }
    float     View::Depth(glm::vec3 point) const { return -(ViewMatrix * glm::vec4(point, 1.0f)).z; }

    std::optional<glm::vec2> View::ToScreen(glm::vec3 point) const
    {
        const glm::vec4 clip = Projection * ViewMatrix * glm::vec4(point, 1.0f);
        if (clip.w <= 1e-6f)
            return std::nullopt;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        return Min + glm::vec2((ndc.x * 0.5f + 0.5f) * Size.x, (0.5f - ndc.y * 0.5f) * Size.y);
    }

    Ray View::RayAt(glm::vec2 screen) const
    {
        const glm::vec2 ndc((screen.x - Min.x) / Size.x * 2.0f - 1.0f, 1.0f - (screen.y - Min.y) / Size.y * 2.0f);
        const glm::mat4 inverse = glm::inverse(Projection * ViewMatrix);
        // Two depths inside the frustum for both depth conventions (-1..1 and 0..1).
        glm::vec4 a = inverse * glm::vec4(ndc, 0.2f, 1.0f), b = inverse * glm::vec4(ndc, 0.8f, 1.0f);
        a /= a.w;
        b /= b.w;
        Ray ray;
        ray.Direction = glm::normalize(glm::vec3(b - a));
        if (Orthographic)
            // On the camera plane, so every point in front of the camera is ahead of the origin.
            ray.Origin = glm::vec3(a) - ray.Direction * glm::dot(glm::vec3(a) - Position(), ray.Direction);
        else
            ray.Origin = Position();
        return ray;
    }

    float View::UnitsPerPixel(glm::vec3 point) const
    {
        // Clip w is the depth in perspective and 1 when orthographic; P[1][1] maps view
        // height to clip height.
        const glm::vec4 clip  = Projection * ViewMatrix * glm::vec4(point, 1.0f);
        const float     scale = std::abs(Projection[1][1]);
        return std::abs(clip.w) * 2.0f / (std::max(scale, 1e-12f) * std::max(Size.y, 1.0f));
    }

    glm::vec3 View::DirectionTo(glm::vec3 point) const
    {
        if (Orthographic)
            return Forward();
        const glm::vec3 d = point - Position();
        return glm::length(d) > 1e-9f ? glm::normalize(d) : Forward();
    }

    View View::From(const EditorCamera& camera, glm::vec2 min, glm::vec2 size)
    {
        View view;
        view.ViewMatrix   = camera.GetViewMatrix();
        view.Projection   = camera.GetProjectionMatrix();
        view.Min          = min;
        view.Size         = glm::max(size, glm::vec2(1.0f));
        view.Orthographic = camera.IsOrthographic();
        return view;
    }

    // ---------------------------------------------------------------------- helpers

    glm::mat4 Delta::Matrix() const
    {
        const glm::mat4 toPivot = glm::translate(glm::mat4(1.0f), Pivot.Position);
        const glm::mat4 back    = glm::translate(glm::mat4(1.0f), -Pivot.Position);
        switch (Op)
        {
            case Operation::Translate:
                return glm::translate(glm::mat4(1.0f), Translation);
            case Operation::Rotate:
                return toPivot * glm::mat4_cast(Rotation) * back;
            case Operation::Scale:
            {
                const glm::mat4 axes(Pivot.Axes);
                return toPivot * axes * glm::scale(glm::mat4(1.0f), Scale) * glm::transpose(axes) * back;
            }
            case Operation::None:
                break;
        }
        return glm::mat4(1.0f);
    }

    bool Delta::IsIdentity() const
    {
        return Translation == glm::vec3(0) && Rotation == glm::quat(1, 0, 0, 0) && Scale == glm::vec3(1);
    }

    Operation OperationOf(Handle h, Mode mode)
    {
        if (h == Handle::None)
            return Operation::None;
        if (IsRing(h) || h == Handle::RingView || h == Handle::Trackball)
            return Operation::Rotate;
        if (IsScaleCube(h))
            return Operation::Scale;
        return mode == Mode::Scale ? Operation::Scale : Operation::Translate;
    }

    glm::mat3 ResolveAxes(Orientation orientation, const glm::mat4& activeWorld, const View& view)
    {
        switch (orientation)
        {
            case Orientation::World:
                return glm::mat3(1.0f);
            case Orientation::View:
                return glm::mat3(view.Right(), view.Up(), -view.Forward());
            case Orientation::Local:
            {
                // Gram-Schmidt, so a sheared or mirrored matrix still gives a right-handed frame.
                glm::vec3 x(activeWorld[0]), y(activeWorld[1]);
                if (glm::length(x) < 1e-9f || glm::length(y) < 1e-9f)
                    return glm::mat3(1.0f);
                x           = glm::normalize(x);
                y           = y - x * glm::dot(x, y);
                if (glm::length(y) < 1e-9f)
                    return glm::mat3(1.0f);
                y = glm::normalize(y);
                return glm::mat3(x, y, glm::cross(x, y));
            }
        }
        return glm::mat3(1.0f);
    }

    float SnapValue(float value, float step) { return step > 0.0f ? std::round(value / step) * step : value; }

    float SnapFactor(float factor, float step)
    {
        if (step <= 0.0f || factor <= 0.0f)
            return factor;
        if (factor >= 1.0f)
            return 1.0f + std::round((factor - 1.0f) / step) * step;
        return 1.0f / (1.0f + std::round((1.0f / factor - 1.0f) / step) * step);
    }

    float ExponentialScale(float pixels, float rate) { return std::exp(glm::clamp(rate * pixels, -80.0f, 80.0f)); }

    std::optional<float> ClosestLineParameter(const Ray& ray, glm::vec3 origin, glm::vec3 direction)
    {
        // Minimise |ray(s) - line(t)|: the 2x2 normal equations.
        const glm::vec3 w = ray.Origin - origin;
        const float     a = glm::dot(ray.Direction, ray.Direction), b = glm::dot(ray.Direction, direction),
                    c = glm::dot(direction, direction), d = glm::dot(ray.Direction, w), e = glm::dot(direction, w);
        const float denominator = a * c - b * b;
        if (std::abs(denominator) < 1e-10f * a * c)
            return std::nullopt;
        return (a * e - b * d) / denominator;
    }

    std::optional<glm::vec3> IntersectPlane(const Ray& ray, glm::vec3 point, glm::vec3 normal)
    {
        const float facing = glm::dot(ray.Direction, normal);
        if (std::abs(facing) < 1e-7f)
            return std::nullopt;
        const float t = glm::dot(point - ray.Origin, normal) / facing;
        return ray.Origin + ray.Direction * t;
    }

    // ---------------------------------------------------------------------- hit testing

    float TransformGizmo::WorldSize(const View& view, glm::vec3 point, const Settings& settings)
    {
        return settings.Size * view.UnitsPerPixel(point);
    }

    Handle TransformGizmo::HitTest(const View& view, const Pivot& pivot, Mode mode, const Settings& settings,
                                   glm::vec2 mouse) const
    {
        return Pick(view, pivot, mode, settings, mouse).Id;
    }

    int HandlePriority(Handle handle) { return Priority(handle); }

    Hit TransformGizmo::Pick(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, glm::vec2 mouse) const
    {
        const glm::vec3 p      = pivot.Position;
        const auto      centre = view.ToScreen(p);
        if (!centre)
            return {};
        const float     s     = WorldSize(view, p, settings);
        const glm::vec3 sight = view.DirectionTo(p);
        const float     pick  = settings.PickRadius;
        const float fromCentre = glm::length(mouse - *centre);
        // The uniform scale centre takes its whole zone, before the plane squares and axis roots.
        if (mode == Mode::Scale && fromCentre <= std::max(pick, settings.CenterPickRadius))
            return {Handle::Center, view.Depth(p), fromCentre};
        std::vector<Candidate> hits;
        const auto      add = [&](Handle id, glm::vec3 at, float distance, bool margin = false) {
            hits.push_back({id, view.Depth(at), distance, Priority(id), margin});
        };
        // A world segment on the screen: the distance to it and the world point there.
        const auto segment = [&](glm::vec3 a, glm::vec3 b, float& distance, glm::vec3& at) {
            const auto sa = view.ToScreen(a), sb = view.ToScreen(b);
            if (!sa || !sb)
                return false;
            float t  = 0;
            distance = SegmentDistance(mouse, *sa, *sb, t);
            at       = glm::mix(a, b, t);
            return true;
        };

        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 axis = pivot.Axes[i];
            if (mode != Mode::Rotate && AxisVisibility(axis, sight, settings.EdgeOnDegrees) > 0.0f)
            {
                const float end = mode == Mode::Scale ? ScaleCube + ScaleCubeHalf : 1.0f;
                float       d   = 0;
                glm::vec3   at;
                if (segment(p + axis * (AxisStart * s), p + axis * (end * s), d, at) && d <= pick)
                    add(AxisHandle(i), at, d);
                if (mode == Mode::Combined)
                {
                    const glm::vec3 cube = p + axis * (CombinedCube * s);
                    if (const auto sc = view.ToScreen(cube))
                    {
                        const float cubePixels = CombinedCubeHalf * settings.Size;
                        const float dc         = glm::length(mouse - *sc);
                        if (dc <= pick + cubePixels)
                            add(CubeHandle(i), cube, dc);
                    }
                }
            }
            if (HasPlanes(mode) && PlaneVisibility(axis, sight, settings.EdgeOnDegrees) > 0.0f)
            {
                int u, v;
                PlaneAxes(i, u, v);
                float low, high;
                PlaneRange(mode, low, high);
                const glm::vec3 du = pivot.Axes[u] * s, dv = pivot.Axes[v] * s;
                const glm::vec3 corners[4] = {p + du * low + dv * low, p + du * high + dv * low, p + du * high + dv * high,
                                              p + du * low + dv * high};
                glm::vec2 screen[4];
                bool      valid = true;
                for (int c = 0; c < 4; ++c)
                {
                    const auto at = view.ToScreen(corners[c]);
                    valid &= bool(at);
                    if (at)
                        screen[c] = *at;
                }
                if (valid)
                {
                    // Inside the convex square (either winding), or within the pick radius of it
                    // where no other handle is.
                    float sign = 0, edge = std::numeric_limits<float>::max();
                    bool  inside = true;
                    for (int c = 0; c < 4; ++c)
                    {
                        const glm::vec2 a = screen[c], b = screen[(c + 1) % 4];
                        const float     side = Cross(b - a, mouse - a);
                        if (sign == 0)
                            sign = side;
                        else if (side * sign < 0)
                            inside = false;
                        float t = 0;
                        edge    = std::min(edge, SegmentDistance(mouse, a, b, t));
                    }
                    const glm::vec3 middle = p + (du + dv) * ((low + high) * 0.5f);
                    if (inside)
                        add(PlaneHandle(i), middle, 0.0f);
                    else if (edge <= pick)
                        add(PlaneHandle(i), middle, edge, true);
                }
            }
            if (mode == Mode::Rotate || mode == Mode::Combined)
            {
                // Only the camera-facing arc: the back half is neither drawn nor picked.
                glm::vec3   at;
                const float d = RingDistance(view, p, FacingArc(axis, -sight), RingRadiusOf(mode) * s, mouse, at);
                if (d <= pick)
                    add(RingHandle(i), at, d);
            }
        }
        if (mode != Mode::Rotate && mode != Mode::Scale && fromCentre <= std::max(pick, CenterRadius * settings.Size))
            add(Handle::Center, p, fromCentre);
        if (mode == Mode::Rotate || mode == Mode::Combined)
        {
            const float ring = ViewRingRadius * settings.Size;
            if (std::abs(fromCentre - ring) <= pick)
                add(Handle::RingView, p, std::abs(fromCentre - ring));
        }
        if (hits.empty())
        {
            if (mode == Mode::Rotate && fromCentre <= RingRadius * settings.Size)
                return {Handle::Trackball, view.Depth(p), fromCentre};
            return {};
        }
        // Margins only fill empty space.
        if (std::any_of(hits.begin(), hits.end(), [](const Candidate& h) { return !h.Margin; }))
            hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Candidate& h) { return h.Margin; }), hits.end());
        // The nearest depth wins; within a small depth band, priority and then distance.
        float nearest = std::numeric_limits<float>::max();
        for (const auto& h : hits)
            nearest = std::min(nearest, h.Depth);
        const float band = 0.05f * s;
        Candidate   best;
        int         bestPriority = -1;
        for (const auto& h : hits)
        {
            if (h.Depth > nearest + band)
                continue;
            const int priority = h.Rank;
            if (priority > bestPriority || (priority == bestPriority && h.Distance < best.Distance))
                best = h, bestPriority = priority;
        }
        return {best.Id, best.Depth, best.Distance};
    }

    // ---------------------------------------------------------------------- state machine

    void TransformGizmo::Reset()
    {
        m_State   = State::Idle;
        m_Hovered = m_Active = Handle::None;
        m_Delta   = {};
    }

    Event TransformGizmo::Update(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, const Input& input)
    {
        m_View     = view;
        m_Settings = settings;
        m_Mouse    = input.Mouse;
        m_Snap     = settings.Snap != input.InvertSnap;
        m_HasFrame = true;
        if (m_State == State::Dragging)
        {
            if (input.Cancel)
            {
                m_State = State::Idle;
                m_Delta = {};
                m_Active = m_Hovered = Handle::None;
                return Event::Cancelled;
            }
            Drag(input.Mouse);
            if (input.Released || !input.Down)
            {
                m_State = State::Idle;
                m_Pivot.Position += m_Delta.Translation;
                m_Active = m_Hovered = Handle::None;
                return Event::Committed;
            }
            return Event::Dragged;
        }
        m_Pivot   = pivot;
        m_Mode    = mode;
        m_Delta   = {};
        m_Hovered = input.Allowed ? HitTest(view, pivot, mode, settings, input.Mouse) : Handle::None;
        if (m_Hovered != Handle::None && input.Pressed && !input.Cancel)
        {
            Begin(view, pivot, mode, settings, m_Hovered, input.Mouse);
            return Event::Began;
        }
        m_State = m_Hovered != Handle::None ? State::Hover : State::Idle;
        return Event::None;
    }

    void TransformGizmo::Begin(const View& view, const Pivot& pivot, Mode mode, const Settings& settings, Handle handle,
                               glm::vec2 mouse)
    {
        m_View       = view;
        m_Pivot      = pivot;
        m_Mode       = mode;
        m_Settings   = settings;
        m_State      = State::Dragging;
        m_Active     = m_Hovered = handle;
        m_StartMouse = m_Mouse = mouse;
        m_StartSize  = WorldSize(view, pivot.Position, settings);
        m_Angle = m_LastRaw = 0.0f;
        m_Delta        = {};
        m_Delta.Op     = OperationOf(handle, mode);
        m_Delta.Handle = handle;
        m_Delta.Pivot  = pivot;

        const glm::vec3 p   = pivot.Position;
        const Ray       ray = view.RayAt(mouse);
        const int       i   = AxisOf(handle);
        const auto screenDirection = [&](glm::vec3 direction) {
            const auto a = view.ToScreen(p), b = view.ToScreen(p + direction * m_StartSize);
            if (!a || !b || glm::length(*b - *a) < 1e-4f)
                return glm::vec2(1, 0);
            return glm::normalize(*b - *a);
        };
        switch (m_Delta.Op)
        {
            case Operation::Translate:
                if (IsAxis(handle))
                {
                    m_Axis           = pivot.Axes[i];
                    m_StartParameter = ClosestLineParameter(ray, p, m_Axis).value_or(0.0f);
                }
                else
                {
                    m_PlaneNormal = IsPlane(handle) ? pivot.Axes[i] : -view.Forward();
                    m_StartPoint  = IntersectPlane(ray, p, m_PlaneNormal).value_or(p);
                }
                break;
            case Operation::Scale:
                if (IsAxis(handle) || IsScaleCube(handle))
                    m_ScreenAxis = screenDirection(pivot.Axes[i]);
                else if (IsPlane(handle))
                {
                    int u, v;
                    PlaneAxes(i, u, v);
                    m_ScreenAxis = screenDirection(glm::normalize(pivot.Axes[u] + pivot.Axes[v]));
                }
                else
                    m_ScreenAxis = glm::normalize(glm::vec2(1, -1)); // Toward the top right grows.
                break;
            case Operation::Rotate:
            {
                m_RadiusPixels = (handle == Handle::RingView ? ViewRingRadius : RingRadiusOf(mode)) * settings.Size;
                if (handle == Handle::Trackball)
                {
                    const auto c = view.ToScreen(p).value_or(mouse);
                    glm::vec3  a((mouse.x - c.x) / m_RadiusPixels, -(mouse.y - c.y) / m_RadiusPixels, 0.0f);
                    const float d2 = a.x * a.x + a.y * a.y;
                    a.z            = d2 < 1.0f ? std::sqrt(1.0f - d2) : 0.0f;
                    m_ArcStart     = glm::normalize(a);
                    break;
                }
                m_Axis          = handle == Handle::RingView ? -view.DirectionTo(p) : pivot.Axes[i];
                m_ScreenTangent = handle != Handle::RingView &&
                                  std::abs(glm::dot(m_Axis, view.DirectionTo(p))) < RingEdgeOnSine;
                const auto hit  = IntersectPlane(ray, p, m_Axis);
                if (!m_ScreenTangent && hit && glm::length(*hit - p) > 1e-6f * m_StartSize)
                    m_StartVector = glm::normalize(*hit - p);
                else
                {
                    m_ScreenTangent = true;
                    // The grab point on the ring and the screen direction rotation moves it.
                    glm::vec3 grab = p;
                    if (handle == Handle::RingView)
                        grab = p + view.Up() * (ViewRingRadius * m_StartSize);
                    else
                        RingDistance(view, p, FacingArc(m_Axis, -view.DirectionTo(p)), RingRadiusOf(mode) * m_StartSize, mouse,
                                     grab);
                    const glm::vec3 radial = grab - p;
                    m_StartVector = glm::length(radial) > 0 ? glm::normalize(radial) : FrameAlong(m_Axis)[0];
                    const glm::vec3 tangent = glm::cross(m_Axis, m_StartVector);
                    const auto      a = view.ToScreen(grab), b = view.ToScreen(grab + tangent * (0.05f * m_StartSize));
                    glm::vec2       t = a && b ? *b - *a : glm::vec2(0);
                    if (glm::length(t) < 1e-3f)
                    {
                        // The tangent points at the camera: use the ring's screen line instead.
                        const glm::vec3 line = glm::cross(m_Axis, view.DirectionTo(p));
                        const auto      c = view.ToScreen(p), d = view.ToScreen(p + line * m_StartSize);
                        t                  = c && d ? *d - *c : glm::vec2(1, 0);
                    }
                    m_Tangent = glm::length(t) > 1e-6f ? glm::normalize(t) : glm::vec2(1, 0);
                }
                m_Delta.RotationAxis = m_Axis;
                break;
            }
            case Operation::None:
                break;
        }
    }

    void TransformGizmo::Drag(glm::vec2 mouse)
    {
        m_Mouse           = mouse;
        const glm::vec3 p = m_Pivot.Position;
        const Ray       ray = m_View.RayAt(mouse);
        const int       i   = AxisOf(m_Active);
        const glm::mat3& axes = m_Pivot.Axes;
        m_Delta.Snapped = m_Snap;
        switch (m_Delta.Op)
        {
            case Operation::Translate:
            {
                if (IsAxis(m_Active))
                {
                    if (const auto t = ClosestLineParameter(ray, p, m_Axis))
                    {
                        float d = *t - m_StartParameter;
                        if (m_Snap)
                            d = SnapValue(d, m_Settings.TranslateStep);
                        m_Delta.Translation = m_Axis * d;
                    }
                    break;
                }
                const auto hit = IntersectPlane(ray, p, m_PlaneNormal);
                // A plane turned away from the camera keeps the last delta.
                if (!hit || glm::dot(*hit - ray.Origin, ray.Direction) <= 0.0f)
                    break;
                const glm::vec3 d = *hit - m_StartPoint;
                glm::vec3       local(glm::dot(d, axes[0]), glm::dot(d, axes[1]), glm::dot(d, axes[2]));
                if (IsPlane(m_Active))
                    local[i] = 0.0f;
                if (m_Snap)
                    for (int c = 0; c < 3; ++c)
                        local[c] = SnapValue(local[c], m_Settings.TranslateStep);
                m_Delta.Translation = IsPlane(m_Active) || m_Snap ? axes * local : d;
                break;
            }
            case Operation::Scale:
            {
                const float rate   = m_Settings.ScaleRate > 0.0f ? m_Settings.ScaleRate : std::log(2.0f) / m_Settings.Size;
                const float pixels = glm::dot(mouse - m_StartMouse, m_ScreenAxis);
                float       factor = ExponentialScale(pixels, rate);
                if (m_Snap)
                    factor = SnapFactor(factor, m_Settings.ScaleStep);
                glm::vec3 scale(1.0f);
                if (IsAxis(m_Active) || IsScaleCube(m_Active))
                    scale[i] = factor;
                else if (IsPlane(m_Active))
                {
                    int u, v;
                    PlaneAxes(i, u, v);
                    scale[u] = scale[v] = factor;
                }
                else
                    scale = glm::vec3(factor);
                m_Delta.Scale = scale;
                break;
            }
            case Operation::Rotate:
            {
                if (m_Active == Handle::Trackball)
                {
                    const auto c = m_View.ToScreen(p).value_or(m_StartMouse);
                    glm::vec3  b((mouse.x - c.x) / m_RadiusPixels, -(mouse.y - c.y) / m_RadiusPixels, 0.0f);
                    const float d2 = b.x * b.x + b.y * b.y;
                    b.z            = d2 < 1.0f ? std::sqrt(1.0f - d2) : 0.0f;
                    b              = glm::normalize(b);
                    const glm::vec3 axis = glm::cross(m_ArcStart, b);
                    float           angle = std::atan2(glm::length(axis), glm::dot(m_ArcStart, b));
                    if (glm::length(axis) < 1e-7f)
                    {
                        m_Delta.Rotation = glm::quat(1, 0, 0, 0);
                        m_Delta.Angle    = 0.0f;
                        break;
                    }
                    // View space to world: the camera's right, up and back.
                    const glm::mat3 camera(m_View.Right(), m_View.Up(), -m_View.Forward());
                    const glm::vec3 world = glm::normalize(camera * glm::normalize(axis));
                    if (m_Snap)
                        angle = SnapValue(angle, glm::radians(m_Settings.RotateStep));
                    m_Delta.RotationAxis = world;
                    m_Delta.Angle        = angle;
                    m_Delta.Rotation     = glm::angleAxis(angle, world);
                    break;
                }
                if (m_ScreenTangent)
                    m_Angle = glm::dot(mouse - m_StartMouse, m_Tangent) / std::max(m_RadiusPixels, 1.0f);
                else if (const auto hit = IntersectPlane(ray, p, m_Axis); hit && glm::length(*hit - p) > 1e-6f * m_StartSize)
                {
                    const glm::vec3 now = glm::normalize(*hit - p);
                    const float     raw =
                        std::atan2(glm::dot(glm::cross(m_StartVector, now), m_Axis), glm::dot(m_StartVector, now));
                    // Unwrapped, so turns past half a circle keep counting.
                    m_Angle += WrapAngle(raw - m_LastRaw);
                    m_LastRaw = raw;
                }
                float angle = m_Angle;
                if (m_Snap)
                    angle = SnapValue(angle, glm::radians(m_Settings.RotateStep));
                m_Delta.Angle    = angle;
                m_Delta.Rotation = glm::angleAxis(angle, m_Axis);
                break;
            }
            case Operation::None:
                break;
        }
    }

    // ---------------------------------------------------------------------- drawing

    void TransformGizmo::AppendDraws(std::vector<GizmoDraw>& out) const
    {
        if (!m_HasFrame || !m_View.ToScreen(m_Pivot.Position))
            return;
        const Settings& settings = m_Settings;
        const bool      dragging = m_State == State::Dragging;
        const glm::vec3 start    = m_Pivot.Position;
        const glm::vec3 p        = dragging ? start + m_Delta.Translation : start;
        const float     s        = dragging ? m_StartSize : WorldSize(m_View, p, settings);
        const glm::vec3 sight    = m_View.DirectionTo(p);
        const glm::mat3& axes    = m_Pivot.Axes;
        const glm::mat3 facing(m_View.Right(), m_View.Up(), -m_View.Forward());
        const auto      shows    = [&](Handle h) { return !dragging || h == m_Active; };
        const auto      hot      = [&](Handle h) { return h == m_Hovered || (dragging && h == m_Active); };
        const auto      tint     = [&](Handle h, glm::vec4 color) {
            if (hot(h))
                color = glm::vec4(glm::mix(glm::vec3(color), glm::vec3(1.0f), settings.HoverBrighten), color.a);
            return color;
        };
        const auto      add = [&](GizmoShape shape, const glm::mat4& transform, glm::vec4 color, glm::vec4 params = glm::vec4(0),
                             bool flat = false) {
            if (color.a > 0.0f)
                out.push_back({shape, transform, color, params, flat});
        };
        const auto      line = [&](glm::vec3 a, glm::vec3 b, glm::vec4 color) {
            const glm::vec3 d = b - a;
            if (glm::length(d) > 0.0f)
                add(GizmoShape::Line, Place(a, FrameAlong(d), glm::vec3(1, 1, glm::length(d))), color);
        };
        // A flat band along a circle about `normal` from `from`, `span` radians toward
        // normal x from; `thickness` pixels across.
        const auto      band = [&](glm::vec3 normal, glm::vec3 from, float radius, float first, float span, float thickness,
                              glm::vec4 color) {
            const glm::mat3 frame(from, glm::cross(normal, from), normal);
            add(GizmoShape::Arc, Place(p, frame, glm::vec3(radius)), color, {0, first, span, thickness * 0.5f});
        };
        const float guide = 1000.0f * s;
        const Operation op = dragging ? m_Delta.Op : Operation::None;
        float planeLow, planeHigh;
        PlaneRange(m_Mode, planeLow, planeHigh);

        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 axis  = axes[i];
            const glm::vec4 base  = settings.AxisColors[i];
            if (m_Mode != Mode::Rotate)
            {
                const Handle    h      = AxisHandle(i);
                const float     fade   = AxisVisibility(axis, sight, settings.EdgeOnDegrees);
                const glm::mat3 frame  = FrameAlong(axis);
                float           length = 1.0f;
                if (dragging && op == Operation::Scale && h == m_Active)
                    length = m_Delta.Scale[i];
                if (shows(h) && (fade > 0.0f || (dragging && h == m_Active)))
                {
                    glm::vec4 color = tint(h, base);
                    color.a *= dragging ? 1.0f : fade;
                    if (m_Mode == Mode::Scale)
                    {
                        const float end = ScaleShaftEnd * length;
                        add(GizmoShape::Cylinder, Place(p + axis * (AxisStart * s), frame,
                                                        {ShaftRadius * s, ShaftRadius * s, (end - AxisStart) * s}), color);
                        add(GizmoShape::Cube, Place(p + axis * (ScaleCube * length * s), glm::mat3(axes), glm::vec3(ScaleCubeHalf * s)),
                            color);
                    }
                    else
                    {
                        add(GizmoShape::Cylinder, Place(p + axis * (AxisStart * s), frame,
                                                        {ShaftRadius * s, ShaftRadius * s, (ArrowShaftEnd - AxisStart) * s}), color);
                        add(GizmoShape::Cone, Place(p + axis * (ArrowShaftEnd * s), frame,
                                                    {ConeRadius * s, ConeRadius * s, (1.0f - ArrowShaftEnd) * s}), color);
                    }
                    if (!dragging && settings.NegativeStubs)
                        line(p, p - axis * (0.3f * s), glm::vec4(glm::vec3(base), 0.55f * fade));
                }
                if (dragging && h == m_Active)
                    line(start - axis * guide, start + axis * guide, glm::vec4(glm::vec3(base), 0.8f));
                if (m_Mode == Mode::Combined)
                {
                    const Handle c = CubeHandle(i);
                    if (shows(c) && (fade > 0.0f || dragging))
                    {
                        const float scale = dragging && c == m_Active ? m_Delta.Scale[i] : 1.0f;
                        glm::vec4   color = tint(c, base);
                        color.a *= dragging ? 1.0f : fade;
                        add(GizmoShape::Cube, Place(p + axis * (CombinedCube * scale * s), glm::mat3(axes),
                                                    glm::vec3(CombinedCubeHalf * s)), color);
                        if (dragging)
                            line(start - axis * guide, start + axis * guide, glm::vec4(glm::vec3(base), 0.8f));
                    }
                }
            }
            if (HasPlanes(m_Mode))
            {
                const Handle h    = PlaneHandle(i);
                const float  fade = PlaneVisibility(axis, sight, settings.EdgeOnDegrees);
                if (shows(h) && (fade > 0.0f || dragging))
                {
                    // A flat square in the normal axis' colour: a see-through fill with a solid
                    // edge, filled in when hot.
                    int u, v;
                    PlaneAxes(i, u, v);
                    const float half   = (planeHigh - planeLow) * 0.5f;
                    glm::vec2   scale(1.0f);
                    if (dragging && op == Operation::Scale)
                        scale = {m_Delta.Scale[u], m_Delta.Scale[v]};
                    const float     mid = (planeLow + planeHigh) * 0.5f;
                    const glm::vec3 c   = p + axes[u] * (mid * scale.x * s) + axes[v] * (mid * scale.y * s);
                    const glm::mat3 frame(axes[u], axes[v], axis);
                    const float     alpha = dragging ? 1.0f : fade;
                    const glm::vec4 color = tint(h, base);
                    add(GizmoShape::Quad, Place(c, frame, {half * s, half * s, 1.0f}),
                        glm::vec4(glm::vec3(color), (hot(h) ? PlaneHoverAlpha : PlaneFillAlpha) * alpha), {}, true);
                    add(GizmoShape::Square, Place(c, frame, {half * s, half * s, 1.0f}), glm::vec4(glm::vec3(color), alpha));
                    if (dragging)
                    {
                        // The constrained plane: both axes as guide lines and an outline around the handle.
                        line(start - axes[u] * guide, start + axes[u] * guide, glm::vec4(glm::vec3(settings.AxisColors[u]), 0.7f));
                        line(start - axes[v] * guide, start + axes[v] * guide, glm::vec4(glm::vec3(settings.AxisColors[v]), 0.7f));
                        add(GizmoShape::Square, Place(p + (axes[u] + axes[v]) * (planeHigh * 0.5f * s), frame,
                                                      {planeHigh * 0.5f * s, planeHigh * 0.5f * s, 1.0f}),
                            glm::vec4(glm::vec3(base), 0.9f));
                    }
                }
            }
            if (m_Mode == Mode::Rotate || m_Mode == Mode::Combined)
            {
                // Idle: the camera-facing arc only. Dragging: the whole active ring.
                const Handle h = RingHandle(i);
                if (shows(h))
                {
                    const float     r         = RingRadiusOf(m_Mode) * s;
                    const float     thickness = settings.RingThickness * (hot(h) ? 1.35f : 1.0f);
                    const glm::vec4 color     = tint(h, base);
                    if (dragging)
                        band(m_Axis, m_StartVector, r, 0.0f, 2.0f * Pi, thickness, color);
                    else
                    {
                        const RingArc arc = FacingArc(axis, -sight);
                        band(axis, arc.Middle, r, -arc.HalfAngle, 2.0f * arc.HalfAngle, thickness, color);
                    }
                }
            }
        }
        if (m_Mode != Mode::Rotate && shows(Handle::Center))
        {
            const glm::vec4 color = tint(Handle::Center, settings.CenterColor);
            if (m_Mode == Mode::Scale)
            {
                // Drawn after the axes and clear of every plane square, so nothing covers it.
                const float f = dragging ? m_Delta.Scale.x : 1.0f;
                add(GizmoShape::Sphere, Place(p, facing, glm::vec3(ScaleCenterRadius * f * s)), color);
            }
            else
            {
                add(GizmoShape::Disc, Place(p, facing, glm::vec3(CenterRadius * s)), glm::vec4(glm::vec3(color), 0.85f));
                add(GizmoShape::Circle, Place(p, facing, glm::vec3(CenterRadius * 1.5f * s)), color);
                if (dragging)
                    line(start, p, glm::vec4(glm::vec3(settings.ViewColor), 0.8f));
            }
        }
        // The outer view ring: thin and light. The trackball inside the rings has no drawing.
        if ((m_Mode == Mode::Rotate || m_Mode == Mode::Combined) && shows(Handle::RingView))
        {
            const float thickness = settings.ViewRingThickness * (hot(Handle::RingView) ? 1.5f : 1.0f);
            const float alpha     = hot(Handle::RingView) ? 0.9f : 0.5f;
            const glm::vec3 axis  = dragging ? m_Axis : -sight;
            const glm::vec3 from  = dragging ? m_StartVector : glm::normalize(facing[0] - axis * glm::dot(facing[0], axis));
            band(axis, from, ViewRingRadius * s, 0.0f, 2.0f * Pi, thickness,
                 glm::vec4(glm::vec3(tint(Handle::RingView, settings.ViewColor)), alpha));
        }
        if (dragging && op == Operation::Rotate && m_Active != Handle::Trackball)
        {
            // UE5's swept angle: a see-through pie slice from the grab to now, its two spokes
            // and, while snapping, a tick at every step around the ring.
            const int       i     = AxisOf(m_Active);
            const float     r     = (m_Active == Handle::RingView ? ViewRingRadius : RingRadiusOf(m_Mode)) * s;
            const glm::vec3 base  = i >= 0 ? glm::vec3(settings.AxisColors[i]) : glm::vec3(settings.ViewColor);
            const glm::mat3 frame(m_StartVector, glm::cross(m_Axis, m_StartVector), m_Axis);
            const float     swept = glm::clamp(m_Delta.Angle, -2.0f * Pi, 2.0f * Pi);
            add(GizmoShape::Sector, Place(p, frame, glm::vec3(r)), glm::vec4(base, 0.3f), {0, 0, swept, 0});
            const glm::vec3 from  = m_StartVector * r;
            const glm::vec3 to    = glm::angleAxis(m_Delta.Angle, m_Axis) * from;
            line(p, p + from, glm::vec4(base, 0.9f));
            line(p, p + to, glm::vec4(glm::vec3(settings.ViewColor), 0.9f));
            if (m_Snap && settings.RotateStep > 0.0f)
            {
                const float step  = glm::radians(settings.RotateStep);
                const float count = std::min(float(GizmoMaxTicks), std::ceil(2.0f * Pi / step - 1e-3f));
                add(GizmoShape::Ticks, Place(p, frame, glm::vec3(r)), glm::vec4(glm::vec3(settings.ViewColor), 0.9f),
                    {0, 0, step, count});
            }
        }
    }

    std::string TransformGizmo::Readout() const
    {
        if (m_State != State::Dragging)
            return {};
        char text[96] = {};
        switch (m_Delta.Op)
        {
            case Operation::Translate:
                if (IsAxis(m_Active))
                    std::snprintf(text, sizeof(text), "%+.3f", glm::dot(m_Delta.Translation, m_Axis));
                else
                {
                    const glm::vec3 d = m_Delta.Translation;
                    std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f  (%.3f)", d.x, d.y, d.z, glm::length(d));
                }
                break;
            case Operation::Rotate:
                std::snprintf(text, sizeof(text), "%+.1f\xC2\xB0", glm::degrees(m_Delta.Angle));
                break;
            case Operation::Scale:
            {
                const int   i = AxisOf(m_Active);
                const float f = i >= 0 && !IsPlane(m_Active) ? m_Delta.Scale[i] : IsPlane(m_Active) ? m_Delta.Scale[(i + 1) % 3] :
                                                                                                    m_Delta.Scale.x;
                std::snprintf(text, sizeof(text), "x%.3f", f);
                break;
            }
            case Operation::None:
                break;
        }
        return text;
    }
} // namespace Kans::Gizmo
