#include "Handles.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtc/constants.hpp>

namespace Kans::Gizmo
{
    namespace
    {
        constexpr float Pi = glm::pi<float>();
        // Lines closer than this to the line of sight drag by screen travel instead of the
        // closest point, which is unstable there; rings likewise (about 10 degrees).
        constexpr float EdgeOnCosine = 0.996f;
        constexpr float RingEdgeOnSine = 0.17f;

        float WrapAngle(float a)
        {
            while (a > Pi)
                a -= 2.0f * Pi;
            while (a <= -Pi)
                a += 2.0f * Pi;
            return a;
        }
        glm::mat4 Segment(glm::vec3 a, glm::vec3 b)
        {
            const glm::vec3 d      = b - a;
            const float     length = glm::length(d);
            const glm::vec3 z      = d / length;
            const glm::vec3 helper = std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 x      = glm::normalize(glm::cross(helper, z));
            glm::mat4       m(1.0f);
            m[0] = glm::vec4(x, 0.0f);
            m[1] = glm::vec4(glm::cross(z, x), 0.0f);
            m[2] = glm::vec4(z * length, 0.0f);
            m[3] = glm::vec4(a, 1.0f);
            return m;
        }
        glm::vec3 Unit(int axis, float sign)
        {
            glm::vec3 u(0.0f);
            u[axis] = sign;
            return u;
        }
        // The signed angle of `v` from `reference` about `axis`.
        float AngleAbout(glm::vec3 reference, glm::vec3 v, glm::vec3 axis)
        {
            return std::atan2(glm::dot(glm::cross(reference, v), axis), glm::dot(reference, v));
        }
    } // namespace

    HandleResult& HandleResult::operator|=(const HandleResult& o)
    {
        Hovered |= o.Hovered;
        Active |= o.Active;
        Started |= o.Started;
        Changed |= o.Changed;
        Committed |= o.Committed;
        Cancelled |= o.Cancelled;
        return *this;
    }

    void HandleContext::Begin(const View& view, const Input& input, const Settings& settings)
    {
        m_View     = view;
        m_Input    = input;
        m_Settings = settings;
        m_InFrame  = true;
        m_ActiveSeen = false;
        m_Candidates.clear();
        m_Draws.clear();
        m_KnobDraws.clear();
    }

    void HandleContext::Reset()
    {
        m_Active = m_Hot = 0;
        m_Stash.reset();
    }

    float HandleContext::KnobRadius(glm::vec3 at, float pixels) const { return pixels * m_View.UnitsPerPixel(at); }

    void HandleContext::Knob(HandleId id, glm::vec3 at, std::optional<glm::vec3> from, const HandleStyle& style)
    {
        const size_t first = m_Draws.size();
        const auto   screen = m_View.ToScreen(at);
        if (!screen)
            return;
        if (style.Stem && from && glm::length(at - *from) > 0.0f)
            m_Draws.push_back({GizmoShape::Line, Segment(*from, at), glm::vec4(glm::vec3(style.Color), 0.7f * style.Color.a)});
        const float r = KnobRadius(at, style.KnobPixels);
        glm::mat4   knob(1.0f);
        knob[0][0] = knob[1][1] = knob[2][2] = r;
        knob[3]                              = glm::vec4(at, 1.0f);
        m_Draws.push_back({style.Knob == KnobShape::Cube ? GizmoShape::Cube : GizmoShape::Sphere, knob, style.Color});
        m_KnobDraws.push_back({id, first, m_Draws.size() - first});
        // Hover: within the pick radius of the knob's centre (or of its drawn size, if larger).
        const float distance = glm::length(m_Input.Mouse - *screen);
        if (m_Input.Allowed && m_Active == 0 && distance <= std::max(m_Settings.PickRadius, style.KnobPixels + 2.0f))
            m_Candidates.push_back({id, m_View.Depth(at), distance, KnobPriority});
    }

    void HandleContext::Offer(HandleId id, float depth, float distance, int priority)
    {
        if (m_Input.Allowed && m_Active == 0 && id != 0)
            m_Candidates.push_back({id, depth, distance, priority});
    }

    HandleResult HandleContext::Track(HandleId id)
    {
        HandleResult r;
        if (m_Active == id)
        {
            m_ActiveSeen = true;
            if (m_Input.Cancel)
            {
                m_Active    = 0;
                r.Cancelled = r.Changed = true;
                return r;
            }
            if (m_Input.Released || !m_Input.Down)
            {
                m_Active    = 0;
                r.Committed = true;
                return r;
            }
            r.Active = true;
            return r;
        }
        if (m_Active == 0 && m_Hot == id && m_Input.Allowed)
        {
            r.Hovered = true;
            if (m_Input.Pressed && !m_Input.Cancel)
            {
                m_Active     = id;
                m_ActiveSeen = true;
                m_Drag       = {};
                m_Drag.StartMouse = m_Input.Mouse;
                m_Stash.reset();
                r.Started = r.Active = true;
            }
        }
        return r;
    }

    float HandleContext::LinearValue(const Drag& drag) const
    {
        if (drag.ScreenTangent)
            return glm::dot(m_Input.Mouse - drag.StartMouse, drag.Tangent) / std::max(drag.PixelsPerUnit, 1e-6f);
        const auto t = ClosestLineParameter(m_View.RayAt(m_Input.Mouse), drag.Origin, drag.Direction);
        return t ? *t - drag.Grab : 0.0f;
    }

    HandleResult HandleContext::Linear(HandleId id, const LinearHandle& h, float& value)
    {
        const glm::vec3 knob = h.Origin + h.Direction * value;
        Knob(id, knob, h.StemFrom ? h.StemFrom : std::optional<glm::vec3>(h.Origin), h.Style);
        HandleResult r = Track(id);
        if (r.Started)
        {
            m_Drag.Origin    = h.Origin;
            m_Drag.Direction = h.Direction;
            m_Drag.Value     = value;
            const float length = glm::length(h.Direction);
            const auto  t      = ClosestLineParameter(m_View.RayAt(m_Input.Mouse), h.Origin, h.Direction);
            // Seen nearly end-on, the closest point is unstable: follow the screen instead.
            m_Drag.ScreenTangent =
                !t || length < 1e-9f || std::abs(glm::dot(h.Direction / length, m_View.DirectionTo(knob))) > EdgeOnCosine;
            m_Drag.Grab = t.value_or(0.0f);
            const auto a = m_View.ToScreen(knob), b = m_View.ToScreen(knob + h.Direction);
            const glm::vec2 screen = a && b ? *b - *a : glm::vec2(0.0f);
            m_Drag.PixelsPerUnit   = glm::length(screen);
            m_Drag.Tangent         = m_Drag.PixelsPerUnit > 1e-6f ? screen / m_Drag.PixelsPerUnit : glm::vec2(1, 0);
        }
        if ((r.Active && !r.Started) || r.Committed)
        {
            float v = m_Drag.Value + LinearValue(m_Drag);
            if (SnapsNow(h.SnapOn) && h.Snap > 0.0f)
                v = SnapValue(v, h.Snap);
            if (h.Min)
                v = std::max(v, *h.Min);
            if (h.Max)
                v = std::min(v, *h.Max);
            value     = v;
            r.Changed = true;
        }
        if (r.Cancelled)
            value = m_Drag.Value;
        return r;
    }

    HandleResult HandleContext::Angle(HandleId id, const AngleHandle& h, float& degrees)
    {
        const glm::vec3 axis = glm::normalize(h.Axis);
        glm::vec3       ref  = h.Reference - axis * glm::dot(h.Reference, axis);
        ref                  = glm::length(ref) > 1e-9f ? glm::normalize(ref) : glm::normalize(glm::cross(axis, glm::vec3(0.3f, 0.5f, 0.8f)));
        const glm::vec3 knob = h.Center + glm::angleAxis(glm::radians(degrees), axis) * ref * h.Radius;
        Knob(id, knob, h.Center, h.Style);
        HandleResult r = Track(id);
        if (r.Started)
        {
            m_Drag.Origin    = h.Center;
            m_Drag.Axis      = axis;
            m_Drag.Reference = ref;
            m_Drag.Value     = degrees;
            m_Drag.Radius    = h.Radius;
            const auto hit   = IntersectPlane(m_View.RayAt(m_Input.Mouse), h.Center, axis);
            m_Drag.ScreenTangent = !hit || glm::length(*hit - h.Center) < 1e-6f * std::max(h.Radius, 1e-6f) ||
                                   std::abs(glm::dot(axis, m_View.DirectionTo(h.Center))) < RingEdgeOnSine;
            if (!m_Drag.ScreenTangent)
                m_Drag.LastRaw = AngleAbout(ref, glm::normalize(*hit - h.Center), axis);
            else
            {
                // Edge-on: the knob's screen tangent, one radian per radius of travel.
                const glm::vec3 tangent = glm::cross(axis, knob - h.Center);
                const auto      a = m_View.ToScreen(knob), b = m_View.ToScreen(knob + tangent);
                const glm::vec2 t = a && b ? *b - *a : glm::vec2(0.0f);
                m_Drag.PixelsPerUnit = glm::length(t);
                m_Drag.Tangent       = m_Drag.PixelsPerUnit > 1e-6f ? t / m_Drag.PixelsPerUnit : glm::vec2(1, 0);
            }
        }
        if ((r.Active && !r.Started) || r.Committed)
        {
            if (m_Drag.ScreenTangent)
                m_Drag.Sweep = glm::dot(m_Input.Mouse - m_Drag.StartMouse, m_Drag.Tangent) / std::max(m_Drag.PixelsPerUnit, 1.0f);
            else if (const auto hit = IntersectPlane(m_View.RayAt(m_Input.Mouse), m_Drag.Origin, m_Drag.Axis);
                     hit && glm::length(*hit - m_Drag.Origin) > 1e-6f * std::max(m_Drag.Radius, 1e-6f))
            {
                const float raw = AngleAbout(m_Drag.Reference, glm::normalize(*hit - m_Drag.Origin), m_Drag.Axis);
                m_Drag.Sweep += WrapAngle(raw - m_Drag.LastRaw);
                m_Drag.LastRaw = raw;
            }
            float v = m_Drag.Value + glm::degrees(m_Drag.Sweep);
            if (SnapsNow(h.SnapOn) && h.Snap > 0.0f)
                v = SnapValue(v, h.Snap);
            if (h.Min)
                v = std::max(v, *h.Min);
            if (h.Max)
                v = std::min(v, *h.Max);
            degrees   = v;
            r.Changed = true;
        }
        if (r.Cancelled)
            degrees = m_Drag.Value;
        return r;
    }

    HandleResult HandleContext::Point(HandleId id, const PointHandle& h, glm::vec3& position)
    {
        Knob(id, position, std::nullopt, h.Style);
        HandleResult r = Track(id);
        const auto   normal = [&] {
            return h.Mode == PointHandle::Constraint::Plane ? glm::normalize(h.Axis) : -m_View.DirectionTo(m_Drag.Point);
        };
        if (r.Started)
        {
            m_Drag.Point     = position;
            m_Drag.Direction = glm::normalize(h.Axis);
            const Ray ray    = m_View.RayAt(m_Input.Mouse);
            if (h.Mode == PointHandle::Constraint::Line)
            {
                m_Drag.Origin = position;
                m_Drag.Grab   = ClosestLineParameter(ray, position, m_Drag.Direction).value_or(0.0f);
            }
            else
            {
                m_Drag.Axis   = normal();
                m_Drag.Origin = IntersectPlane(ray, position, m_Drag.Axis).value_or(position);
            }
        }
        if ((r.Active && !r.Started) || r.Committed)
        {
            const Ray ray  = m_View.RayAt(m_Input.Mouse);
            const bool snap = SnapsNow(false) && h.Snap > 0.0f;
            if (h.Mode == PointHandle::Constraint::Line)
            {
                if (const auto t = ClosestLineParameter(ray, m_Drag.Origin, m_Drag.Direction))
                {
                    float d  = *t - m_Drag.Grab;
                    position = m_Drag.Point + m_Drag.Direction * (snap ? SnapValue(d, h.Snap) : d);
                }
            }
            else if (const auto hit = IntersectPlane(ray, m_Drag.Point, m_Drag.Axis);
                     hit && glm::dot(*hit - ray.Origin, ray.Direction) > 0.0f)
            {
                glm::vec3 d = *hit - m_Drag.Origin;
                if (snap)
                    for (int c = 0; c < 3; ++c)
                        d[c] = SnapValue(d[c], h.Snap);
                position = m_Drag.Point + d;
            }
            r.Changed = true;
        }
        if (r.Cancelled)
            position = m_Drag.Point;
        return r;
    }

    HandleResult HandleContext::Sphere(HandleId id, const SphereHandle& h, float& radius)
    {
        HandleResult    result;
        const glm::vec3 centre = glm::vec3(h.Frame[3]);
        for (int part = 0; part < 6; ++part)
        {
            LinearHandle linear;
            linear.Origin    = centre;
            linear.Direction = glm::vec3(h.Frame * glm::vec4(Unit(part / 2, part % 2 ? 1.0f : -1.0f), 0.0f));
            linear.Min       = h.MinRadius;
            linear.Style     = h.Style;
            result |= Linear(MakeHandleId(id, uint64_t(part) + 1), linear, radius);
        }
        return result;
    }

    HandleResult HandleContext::Box(HandleId id, const BoxHandle& h, glm::vec3& center, glm::vec3& halfExtents)
    {
        HandleResult    result;
        const glm::vec3 centre = glm::vec3(h.Frame * glm::vec4(center, 1.0f));
        for (int part = 0; part < 6; ++part)
        {
            const int   axis = part / 2;
            const float sign = part % 2 ? 1.0f : -1.0f;
            LinearHandle linear;
            linear.Origin    = centre;
            linear.Direction = glm::vec3(h.Frame * glm::vec4(Unit(axis, sign), 0.0f));
            linear.Style     = h.Style;
            float             reach = std::abs(halfExtents[axis]);
            const HandleResult r    = Linear(MakeHandleId(id, uint64_t(part) + 1), linear, reach);
            if (r.Started)
            {
                m_Drag.Center = center;
                m_Drag.Half   = glm::abs(halfExtents);
                m_Drag.Part   = part;
            }
            if (r.Changed && !r.Cancelled)
            {
                // How far the dragged face moved out; one face alone moves the centre half as far.
                const float moved     = reach - m_Drag.Half[axis];
                const bool  symmetric = h.LockCenter || (h.Symmetric != m_Input.Alt);
                glm::vec3   half      = m_Drag.Half;
                glm::vec3   c         = m_Drag.Center;
                if (symmetric)
                    half[axis] = std::max(h.MinHalf, m_Drag.Half[axis] + moved);
                else
                {
                    half[axis] = std::max(h.MinHalf, m_Drag.Half[axis] + moved * 0.5f);
                    c[axis]    = m_Drag.Center[axis] + sign * (half[axis] - m_Drag.Half[axis]);
                }
                center      = c;
                halfExtents = half;
            }
            if (r.Cancelled)
            {
                center      = m_Drag.Center;
                halfExtents = m_Drag.Half;
            }
            result |= r;
        }
        return result;
    }

    HandleResult HandleContext::Capsule(HandleId id, const CapsuleHandle& h, glm::vec3& center, float& radius, float& halfHeight)
    {
        HandleResult    result;
        const int       a      = std::clamp(h.Axis, 0, 2);
        const glm::vec3 centre = glm::vec3(h.Frame * glm::vec4(center, 1.0f));
        // The caps: each end moves alone (the other stays), or both with Alt.
        for (int part = 0; part < 2; ++part)
        {
            const float  sign = part ? 1.0f : -1.0f;
            LinearHandle linear;
            linear.Origin    = centre;
            linear.Direction = glm::vec3(h.Frame * glm::vec4(Unit(a, sign), 0.0f));
            linear.Style     = h.Style;
            float              reach = std::abs(halfHeight) + std::abs(radius);
            const HandleResult r     = Linear(MakeHandleId(id, uint64_t(part) + 1), linear, reach);
            if (r.Started)
            {
                m_Drag.Center = center;
                m_Drag.Half   = glm::vec3(std::abs(halfHeight), std::abs(radius), 0.0f);
                m_Drag.Part   = part;
            }
            if (r.Changed && !r.Cancelled)
            {
                const float moved = reach - (m_Drag.Half.x + m_Drag.Half.y);
                glm::vec3   c     = m_Drag.Center;
                float       half  = m_Drag.Half.x;
                if (m_Input.Alt)
                    half = std::max(0.0f, m_Drag.Half.x + moved);
                else
                {
                    half = std::max(0.0f, m_Drag.Half.x + moved * 0.5f);
                    c[a] = m_Drag.Center[a] + sign * (half - m_Drag.Half.x);
                }
                center     = c;
                halfHeight = half;
            }
            if (r.Cancelled)
            {
                center     = m_Drag.Center;
                halfHeight = m_Drag.Half.x;
            }
            result |= r;
        }
        // The radius, around the axis.
        int part = 2;
        for (int side : {(a + 1) % 3, (a + 2) % 3})
            for (float sign : {-1.0f, 1.0f})
            {
                LinearHandle linear;
                linear.Origin    = centre;
                linear.Direction = glm::vec3(h.Frame * glm::vec4(Unit(side, sign), 0.0f));
                linear.Min       = h.MinRadius;
                linear.Style     = h.Style;
                result |= Linear(MakeHandleId(id, uint64_t(part++) + 1), linear, radius);
            }
        return result;
    }

    void HandleContext::End()
    {
        m_InFrame = false;
        if (m_Active != 0 && !m_ActiveSeen)
            m_Active = 0; // The dragged handle was not declared: its owner went away.
        if (m_Active != 0)
        {
            m_Hot = m_Active;
            return;
        }
        m_Hot = 0;
        if (!m_Input.Allowed || m_Candidates.empty())
            return;
        // The nearest depth wins; within a band of 5% of the gizmo's size there, priority and
        // then the screen distance decide.
        const auto band = [&](float depth) {
            const float scale = std::abs(m_View.Projection[1][1]);
            const float upp   = (m_View.Orthographic ? 1.0f : std::max(depth, 0.0f)) * 2.0f /
                              (std::max(scale, 1e-12f) * std::max(m_View.Size.y, 1.0f));
            return 0.05f * m_Settings.Size * upp;
        };
        float nearest = std::numeric_limits<float>::max();
        for (const auto& c : m_Candidates)
            nearest = std::min(nearest, c.Depth);
        const float limit = nearest + band(nearest);
        const Candidate* best = nullptr;
        for (const auto& c : m_Candidates)
        {
            if (c.Depth > limit)
                continue;
            if (!best || c.Priority > best->Priority || (c.Priority == best->Priority && c.Distance < best->Distance))
                best = &c;
        }
        m_Hot = best ? best->Id : 0;
    }

    void HandleContext::AppendDraws(std::vector<GizmoDraw>& out) const
    {
        for (const auto& k : m_KnobDraws)
        {
            const bool lit = k.Id == m_Hot || k.Id == m_Active;
            for (size_t i = k.First; i < k.First + k.Count; ++i)
            {
                GizmoDraw draw = m_Draws[i];
                if (lit)
                    draw.Color = glm::vec4(glm::mix(glm::vec3(draw.Color), glm::vec3(1.0f), m_Settings.HoverBrighten), draw.Color.a);
                out.push_back(draw);
            }
        }
    }
} // namespace Kans::Gizmo
