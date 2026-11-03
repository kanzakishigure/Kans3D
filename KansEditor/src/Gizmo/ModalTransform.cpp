#include "ModalTransform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <glm/gtc/constants.hpp>

namespace Kans::Gizmo
{
    namespace
    {
        constexpr float Pi = glm::pi<float>();
        // Pixels of middle-drag travel before an axis is picked.
        constexpr float MiddlePickPixels = 8.0f;
        // Trackball: radians per pixel of travel, as a fraction of the gizmo size (a quarter turn
        // over one gizmo length).
        constexpr float TrackballTurn = 0.5f * Pi;
        constexpr int   MaxDashes     = 24;

        float WrapAngle(float a)
        {
            while (a > Pi)
                a -= 2.0f * Pi;
            while (a <= -Pi)
                a += 2.0f * Pi;
            return a;
        }
        float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }
        glm::mat4 Segment(glm::vec3 a, glm::vec3 b)
        {
            const glm::vec3 d      = b - a;
            const float     length = glm::length(d);
            glm::vec3       z      = d / length;
            const glm::vec3 helper = std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 x      = glm::normalize(glm::cross(helper, z));
            glm::mat4       m(1.0f);
            m[0] = glm::vec4(x, 0.0f);
            m[1] = glm::vec4(glm::cross(z, x), 0.0f);
            m[2] = glm::vec4(z * length, 0.0f);
            m[3] = glm::vec4(a, 1.0f);
            return m;
        }
        const char* AxisName(int axis) { return axis == 0 ? "X" : axis == 1 ? "Y" : "Z"; }
        const char* PlaneName(int excluded) { return excluded == 0 ? "YZ" : excluded == 1 ? "ZX" : "XY"; }
    } // namespace

    float ModalTransform::Component::Value(float fallback) const
    {
        const float base = Text.empty() ? fallback : std::strtof(Text.c_str(), nullptr);
        return Negative ? -base : base;
    }

    bool ModalTransform::IsNumeric() const
    {
        return std::any_of(m_Numeric.begin(), m_Numeric.end(), [](const Component& c) { return c.Typed(); });
    }

    std::string ModalTransform::NumericText(int component) const
    {
        if (component < 0 || component >= 3 || !m_Numeric[size_t(component)].Typed())
            return {};
        const auto& c = m_Numeric[size_t(component)];
        return (c.Negative ? "-" : "") + c.Text;
    }

    const char* ModalTransform::SpaceName(int space) const
    {
        const Orientation first = m_Orientation;
        if (space == 0)
            return first == Orientation::World ? "Global" : first == Orientation::Local ? "Local" : "View";
        return first == Orientation::World ? "Local" : "Global";
    }

    glm::vec3 ModalTransform::TowardViewer() const { return -m_View.DirectionTo(m_Pivot.Position); }

    void ModalTransform::Start(Operation op, const View& view, const Pivot& pivot, Orientation orientation,
                               const glm::mat3& local, const Settings& settings, glm::vec2 mouse)
    {
        m_Active      = true;
        m_View        = view;
        m_Pivot       = pivot;
        m_Orientation = orientation;
        m_Primary     = pivot.Axes;
        // The second press switches to global axes, or to local ones when the first were global.
        m_Secondary = orientation == Orientation::World ? local : glm::mat3(1.0f);
        m_Settings  = settings;
        m_Snap      = settings.Snap;
        m_Trackball = false;
        m_Mouse     = mouse;
        Restart(op, mouse);
    }

    void ModalTransform::Restart(Operation op, glm::vec2 mouse)
    {
        m_Op         = op;
        m_StartMouse = m_LastMouse = m_Virtual = m_Mouse = mouse;
        m_Sweep = m_LastRaw = 0.0f;
        m_Constraint        = {};
        m_Numeric           = {};
        m_Field             = 0;
        m_MiddlePicking     = false;
        Compute();
    }

    int ModalTransform::ComponentCount() const
    {
        if (m_Op == Operation::Rotate)
            return m_Trackball ? 2 : 1;
        if (m_Constraint.IsFree())
            return 3;
        return m_Constraint.Plane ? 2 : 1;
    }

    void ModalTransform::LockAxis(int axis, bool plane)
    {
        if (m_Op == Operation::Rotate && m_Trackball)
            m_Trackball = false; // An axis turns the trackball back into a rotation about it.
        if (m_Constraint.Axis == axis && m_Constraint.Plane == plane)
        {
            if (m_Constraint.Space == 0)
                m_Constraint.Space = 1;
            else
                m_Constraint = {};
        }
        else
            m_Constraint = {axis, plane, 0};
        m_Field = std::min(m_Field, ComponentCount() - 1);
    }

    void ModalTransform::Key(const ModalKey& key)
    {
        auto& field = m_Numeric[size_t(m_Field)];
        switch (key.Key)
        {
            case 'X':
            case 'Y':
            case 'Z':
                LockAxis(key.Key - 'X', key.Shift);
                return;
            case 'G':
                if (m_Op != Operation::Translate)
                    Restart(Operation::Translate, m_Mouse);
                return;
            case 'R':
            {
                // R again: the trackball (and back).
                const bool trackball = m_Op == Operation::Rotate && !m_Trackball;
                Restart(Operation::Rotate, m_Mouse);
                m_Trackball = trackball;
                return;
            }
            case 'S':
                if (m_Op != Operation::Scale)
                    Restart(Operation::Scale, m_Mouse);
                return;
            case '\t':
                m_Field = (m_Field + 1) % ComponentCount();
                return;
            case '-':
                field.Negative = !field.Negative;
                return;
            case '\b':
                if (!field.Text.empty())
                    field.Text.pop_back();
                else if (field.Negative)
                    field.Negative = false;
                else if (m_Field > 0)
                    --m_Field;
                return;
            case '.':
                if (field.Text.find('.') == std::string::npos)
                    field.Text += field.Text.empty() ? "0." : ".";
                return;
            default:
                if (key.Key >= '0' && key.Key <= '9' && field.Text.size() < 16)
                    field.Text += key.Key;
                return;
        }
    }

    Event ModalTransform::Update(const View& view, const ModalInput& input)
    {
        if (!m_Active)
            return Event::None;
        m_View = view;
        m_Snap = m_Settings.Snap != input.Ctrl;
        if (input.Cancel)
        {
            m_Active = false;
            m_Delta  = {};
            return Event::Cancelled;
        }
        // The pointer first, so a key that restarts the operation starts from where it is now.
        m_Virtual += (input.Mouse - m_LastMouse) * (input.Shift ? 0.1f : 1.0f);
        m_LastMouse = m_Mouse = input.Mouse;
        for (const auto& key : input.Keys)
            Key(key);

        // Middle drag: the axis whose screen direction is nearest the pointer's motion.
        if (input.MiddlePressed)
        {
            m_MiddlePicking = true;
            m_MiddleStart   = input.Mouse;
        }
        else if (!input.MiddleDown)
            m_MiddlePicking = false;
        if (m_MiddlePicking && glm::length(input.Mouse - m_MiddleStart) >= MiddlePickPixels)
        {
            const glm::vec2 motion = glm::normalize(input.Mouse - m_MiddleStart);
            const auto      origin = m_View.ToScreen(m_Pivot.Position);
            int             best   = -1;
            float           score  = -1.0f;
            const float     reach  = TransformGizmo::WorldSize(m_View, m_Pivot.Position, m_Settings);
            for (int i = 0; i < 3 && origin; ++i)
            {
                const auto tip = m_View.ToScreen(m_Pivot.Position + m_Primary[i] * reach);
                if (!tip || glm::length(*tip - *origin) < 1.0f)
                    continue;
                const float s = std::abs(glm::dot(motion, glm::normalize(*tip - *origin)));
                if (s > score)
                    score = s, best = i;
            }
            if (best >= 0)
                m_Constraint = {best, false, 0}, m_Field = 0;
        }

        // Rotation sweeps the pointer's angle around the pivot on the screen, unwrapped.
        if (m_Op == Operation::Rotate && !m_Trackball)
            if (const auto c = m_View.ToScreen(m_Pivot.Position))
            {
                const glm::vec2 a = m_StartMouse - *c, b = m_Virtual - *c;
                if (glm::length(a) > 1e-3f && glm::length(b) > 1e-3f)
                {
                    const float raw = std::atan2(Cross(a, b), glm::dot(a, b));
                    // The screen's y points down: a positive raw angle turns clockwise as seen.
                    m_Sweep -= WrapAngle(raw - m_LastRaw);
                    m_LastRaw = raw;
                }
            }
        Compute();
        if (input.Confirm)
        {
            m_Active = false;
            return Event::Committed;
        }
        return Event::Dragged;
    }

    void ModalTransform::Compute()
    {
        const glm::vec3 p       = m_Pivot.Position;
        const bool      numeric = IsNumeric();
        const int       a       = m_Constraint.Axis;
        const int       u = (a + 1) % 3, v = (a + 2) % 3;
        const glm::mat3 axes    = m_Constraint.IsFree() ? m_Primary : ConstraintAxes();
        m_Delta                 = {};
        m_Delta.Op              = m_Op;
        m_Delta.Pivot           = {p, axes};
        m_Delta.Snapped         = m_Snap && !numeric;
        const Ray start = m_View.RayAt(m_StartMouse), now = m_View.RayAt(m_Virtual);
        switch (m_Op)
        {
            case Operation::Translate:
            {
                glm::vec3 d(0.0f);
                if (numeric)
                {
                    if (m_Constraint.IsFree())
                        d = axes * glm::vec3(m_Numeric[0].Value(0), m_Numeric[1].Value(0), m_Numeric[2].Value(0));
                    else if (!m_Constraint.Plane)
                        d = axes[a] * m_Numeric[0].Value(0);
                    else
                        d = axes[u] * m_Numeric[0].Value(0) + axes[v] * m_Numeric[1].Value(0);
                }
                else if (!m_Constraint.IsFree() && !m_Constraint.Plane)
                {
                    const auto s0 = ClosestLineParameter(start, p, axes[a]), s = ClosestLineParameter(now, p, axes[a]);
                    float      t  = s0 && s ? *s - *s0 : 0.0f;
                    if (m_Snap)
                        t = SnapValue(t, m_Settings.TranslateStep);
                    d = axes[a] * t;
                }
                else
                {
                    // The view plane through the pivot, or the locked plane unless it is seen edge-on.
                    const glm::vec3 facing = TowardViewer();
                    glm::vec3       normal = facing;
                    if (m_Constraint.Plane && std::abs(glm::dot(axes[a], facing)) > 0.05f)
                        normal = axes[a];
                    const auto h0 = IntersectPlane(start, p, normal), h = IntersectPlane(now, p, normal);
                    if (h0 && h)
                        d = *h - *h0;
                    glm::vec3 local = glm::transpose(axes) * d;
                    if (m_Constraint.Plane)
                        local[a] = 0.0f;
                    if (m_Snap)
                        for (int c = 0; c < 3; ++c)
                            local[c] = SnapValue(local[c], m_Settings.TranslateStep);
                    if (m_Constraint.Plane || m_Snap)
                        d = axes * local;
                }
                m_Delta.Translation = d;
                break;
            }
            case Operation::Rotate:
            {
                if (m_Trackball)
                {
                    const glm::vec3 up = m_View.Up(), right = m_View.Right();
                    if (numeric)
                    {
                        const float yaw = glm::radians(m_Numeric[0].Value(0)), pitch = glm::radians(m_Numeric[1].Value(0));
                        m_Delta.Rotation     = glm::normalize(glm::angleAxis(pitch, right) * glm::angleAxis(yaw, up));
                        m_Delta.RotationAxis = glm::axis(m_Delta.Rotation);
                        m_Delta.Angle        = glm::angle(m_Delta.Rotation);
                        break;
                    }
                    const glm::vec2 travel = m_Virtual - m_StartMouse;
                    const float     pixels = glm::length(travel);
                    if (pixels < 1e-4f)
                        break;
                    // Right turns the front toward the right (about up), down tips it down (about right).
                    const glm::vec3 axis  = glm::normalize(up * travel.x + right * travel.y);
                    float           angle = pixels * TrackballTurn / std::max(m_Settings.Size, 1.0f);
                    if (m_Snap)
                        angle = SnapValue(angle, glm::radians(m_Settings.RotateStep));
                    m_Delta.RotationAxis = axis;
                    m_Delta.Angle        = angle;
                    m_Delta.Rotation     = glm::angleAxis(angle, axis);
                    break;
                }
                const glm::vec3 facing = TowardViewer();
                const glm::vec3 axis   = m_Constraint.IsFree() ? facing : axes[a];
                float           angle  = 0.0f;
                if (numeric)
                    angle = glm::radians(m_Numeric[0].Value(0));
                else
                {
                    // Turning the pointer counterclockwise turns counterclockwise as seen, so about an
                    // axis pointing away from the viewer the angle's sign flips.
                    angle = glm::dot(axis, facing) >= 0.0f ? m_Sweep : -m_Sweep;
                    if (m_Snap)
                        angle = SnapValue(angle, glm::radians(m_Settings.RotateStep));
                }
                m_Delta.RotationAxis = axis;
                m_Delta.Angle        = angle;
                m_Delta.Rotation     = glm::angleAxis(angle, axis);
                break;
            }
            case Operation::Scale:
            {
                glm::vec3 scale(1.0f);
                if (numeric)
                {
                    if (m_Constraint.IsFree())
                        scale = !m_Numeric[1].Typed() && !m_Numeric[2].Typed() ?
                                    glm::vec3(m_Numeric[0].Value(1)) :
                                    glm::vec3(m_Numeric[0].Value(1), m_Numeric[1].Value(1), m_Numeric[2].Value(1));
                    else if (!m_Constraint.Plane)
                        scale[a] = m_Numeric[0].Value(1);
                    else
                    {
                        scale[u] = m_Numeric[0].Value(1);
                        scale[v] = m_Numeric[1].Typed() ? m_Numeric[1].Value(1) : scale[u];
                    }
                }
                else
                {
                    // The pointer's distance from the pivot over its distance at the start.
                    float      factor = 1.0f;
                    const auto c      = m_View.ToScreen(p);
                    if (c && glm::length(m_StartMouse - *c) >= 1.0f)
                        factor = std::max(glm::length(m_Virtual - *c) / glm::length(m_StartMouse - *c), 1e-6f);
                    else
                        factor = ExponentialScale(glm::dot(m_Virtual - m_StartMouse, glm::normalize(glm::vec2(1, -1))),
                                                  std::log(2.0f) / std::max(m_Settings.Size, 1.0f));
                    if (m_Snap)
                        factor = SnapFactor(factor, m_Settings.ScaleStep);
                    if (m_Constraint.IsFree())
                        scale = glm::vec3(factor);
                    else if (!m_Constraint.Plane)
                        scale[a] = factor;
                    else
                        scale[u] = scale[v] = factor;
                }
                m_Delta.Scale = scale;
                break;
            }
            case Operation::None:
                break;
        }
    }

    std::string ModalTransform::Readout() const
    {
        if (!m_Active)
            return {};
        char text[128] = {};
        switch (m_Op)
        {
            case Operation::Translate:
                if (!m_Constraint.IsFree() && !m_Constraint.Plane)
                    std::snprintf(text, sizeof(text), "%+.3f", glm::dot(m_Delta.Translation, ConstraintAxes()[m_Constraint.Axis]));
                else
                {
                    const glm::vec3 d = m_Delta.Translation;
                    std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f", d.x, d.y, d.z);
                }
                break;
            case Operation::Rotate:
                std::snprintf(text, sizeof(text), "%+.1f\xC2\xB0", glm::degrees(m_Delta.Angle));
                break;
            case Operation::Scale:
            {
                const glm::vec3 s = m_Delta.Scale;
                if (s.x == s.y && s.y == s.z)
                    std::snprintf(text, sizeof(text), "x%.3f", s.x);
                else
                    std::snprintf(text, sizeof(text), "x%.3f, %.3f, %.3f", s.x, s.y, s.z);
                break;
            }
            case Operation::None:
                break;
        }
        return text;
    }

    std::string ModalTransform::Status() const
    {
        if (!m_Active)
            return {};
        std::string status = m_Op == Operation::Translate ? "Move" :
                             m_Op == Operation::Scale     ? "Scale" :
                             m_Trackball                  ? "Trackball" :
                                                            "Rotate";
        const ModalConstraint& c = m_Constraint;
        if (m_Op == Operation::Rotate && !m_Trackball)
            status += c.IsFree() ? std::string("  about the view") :
                                   std::string("  about ") + AxisName(c.Axis) + " (" + SpaceName(c.Space) + ")";
        else if (!m_Trackball)
        {
            if (c.IsFree())
                status += m_Op == Operation::Translate ? "  in the view plane" : "  uniform";
            else if (c.Plane)
                status += std::string("  on ") + PlaneName(c.Axis) + " (" + SpaceName(c.Space) + ")";
            else
                status += std::string("  along ") + AxisName(c.Axis) + " (" + SpaceName(c.Space) + ")";
        }
        status += "    ";
        if (IsNumeric())
        {
            // The typed fields, the one Tab is on marked with a cursor.
            const int count = ComponentCount();
            for (int i = 0; i < count; ++i)
            {
                if (i)
                    status += ", ";
                std::string text = NumericText(i);
                if (text.empty())
                    text = m_Op == Operation::Scale ? "1" : "0";
                status += i == m_Field ? "[" + text + "|]" : text;
            }
            status += "  (typed)";
        }
        else
            status += Readout();
        if (m_Delta.Snapped)
            status += "  (snap)";
        status += "     Enter or click: confirm   Esc or right click: cancel";
        return status;
    }

    void ModalTransform::AppendDraws(std::vector<GizmoDraw>& out) const
    {
        if (!m_Active)
            return;
        const glm::vec3 p     = m_Pivot.Position;
        const float     s     = TransformGizmo::WorldSize(m_View, p, m_Settings);
        const float     guide = 1000.0f * s;
        const auto      line  = [&](glm::vec3 a, glm::vec3 b, glm::vec4 color) {
            if (glm::length(b - a) > 0.0f)
                out.push_back({GizmoShape::Line, Segment(a, b), color});
        };
        const ModalConstraint& c = m_Constraint;
        if (!c.IsFree() && !(m_Op == Operation::Rotate && m_Trackball))
        {
            const glm::mat3 axes = ConstraintAxes();
            if (c.Plane && m_Op != Operation::Rotate)
                for (int i : {(c.Axis + 1) % 3, (c.Axis + 2) % 3})
                    line(p - axes[i] * guide, p + axes[i] * guide, glm::vec4(glm::vec3(m_Settings.AxisColors[i]), 0.8f));
            else
                line(p - axes[c.Axis] * guide, p + axes[c.Axis] * guide,
                     glm::vec4(glm::vec3(m_Settings.AxisColors[c.Axis]), 0.8f));
        }
        if (m_Op == Operation::Translate)
            return;
        // A dashed line from the pivot to the pointer, on the view plane through the pivot.
        const auto centre = m_View.ToScreen(p);
        const auto cursor = IntersectPlane(m_View.RayAt(m_Mouse), p, TowardViewer());
        if (!centre || !cursor)
            return;
        const float pixels = glm::length(m_Mouse - *centre);
        const int   dashes = std::clamp(int(pixels / 12.0f), 1, MaxDashes);
        const glm::vec4 color(glm::vec3(m_Settings.ViewColor), 0.9f);
        for (int i = 0; i < dashes; ++i)
        {
            const float t0 = float(i) / dashes, t1 = (float(i) + 0.55f) / dashes;
            line(glm::mix(p, *cursor, t0), glm::mix(p, *cursor, t1), color);
        }
    }
} // namespace Kans::Gizmo
