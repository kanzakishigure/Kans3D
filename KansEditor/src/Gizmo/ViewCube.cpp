#include "ViewCube.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace Kans::Gizmo
{
    namespace
    {
        // The eye's distance from the cube's middle, in half-widths (the perspective's strength).
        constexpr float EyeDistance = 8.0f;

        glm::vec3 FaceNormal(int face)
        {
            glm::vec3 n(0.0f);
            n[face % 3] = face < 3 ? 1.0f : -1.0f;
            return n;
        }
        float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }
        bool  Inside(const std::array<glm::vec2, 4>& quad, glm::vec2 point)
        {
            float sign = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                const float side = Cross(quad[(i + 1) % 4] - quad[i], point - quad[i]);
                if (side == 0.0f)
                    continue;
                if (sign == 0.0f)
                    sign = side;
                else if (side * sign < 0.0f)
                    return false;
            }
            return true;
        }
        // The eye in the cube's space (the view's rotation only).
        glm::vec3 Eye(const glm::mat4& view) { return glm::transpose(glm::mat3(view)) * glm::vec3(0, 0, EyeDistance); }
    } // namespace

    glm::vec2 ViewCubeProject(const glm::mat4& view, glm::vec2 centre, float half, glm::vec3 point)
    {
        const glm::vec3 q = glm::mat3(view) * point; // x right, y up, z toward the camera.
        const float     k = EyeDistance / (EyeDistance - q.z);
        return centre + glm::vec2(q.x, -q.y) * (half * k);
    }

    std::vector<ViewCubeCell> ViewCubeCells(const glm::mat4& view, glm::vec2 centre, float half)
    {
        std::vector<ViewCubeCell> cells;
        const glm::vec3           eye = Eye(view);
        const float               cuts[4] = {-1.0f, -ViewCubeFaceExtent, ViewCubeFaceExtent, 1.0f};
        for (int face = 0; face < 6; ++face)
        {
            const glm::vec3 n = FaceNormal(face);
            // Turned toward the eye: the eye is outside the face's plane.
            if (glm::dot(n, eye) <= 1.0f)
                continue;
            const int axis = face % 3;
            glm::vec3 u(0.0f), v(0.0f);
            u[(axis + 1) % 3] = 1.0f;
            v[(axis + 2) % 3] = 1.0f;
            const float facing = glm::clamp((glm::mat3(view) * n).z, 0.0f, 1.0f);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    ViewCubeCell cell;
                    cell.Face             = face;
                    cell.Facing           = facing;
                    const glm::vec3 c[4]  = {n + u * cuts[i] + v * cuts[j], n + u * cuts[i + 1] + v * cuts[j],
                                             n + u * cuts[i + 1] + v * cuts[j + 1], n + u * cuts[i] + v * cuts[j + 1]};
                    float           depth = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        cell.Screen[size_t(k)] = ViewCubeProject(view, centre, half, c[k]);
                        depth += EyeDistance - (glm::mat3(view) * c[k]).z;
                    }
                    cell.Depth     = depth * 0.25f;
                    cell.Direction = glm::ivec3(n) + glm::ivec3(u) * (i - 1) + glm::ivec3(v) * (j - 1);
                    cells.push_back(cell);
                }
        }
        std::stable_sort(cells.begin(), cells.end(), [](const ViewCubeCell& a, const ViewCubeCell& b) { return a.Depth > b.Depth; });
        return cells;
    }

    std::vector<ViewCubeLabel> ViewCubeLabels(const glm::mat4& view, glm::vec2 centre, float half)
    {
        std::vector<ViewCubeLabel> labels;
        const glm::vec3            eye = Eye(view);
        for (int face = 0; face < 6; ++face)
        {
            const glm::vec3 n = FaceNormal(face);
            if (glm::dot(n, eye) <= 1.0f)
                continue;
            labels.push_back({ViewCubeProject(view, centre, half, n), glm::clamp((glm::mat3(view) * n).z, 0.0f, 1.0f), face,
                              ViewCubeFaceName(face)});
        }
        std::stable_sort(labels.begin(), labels.end(), [](const ViewCubeLabel& a, const ViewCubeLabel& b) { return a.Facing < b.Facing; });
        return labels;
    }

    std::optional<CubeDirection> PickViewCube(const std::vector<ViewCubeCell>& cells, glm::vec2 mouse)
    {
        // Back to front: the last hit is the front-most.
        std::optional<CubeDirection> hit;
        for (const auto& cell : cells)
            if (Inside(cell.Screen, mouse))
                hit = cell.Direction;
        return hit;
    }

    const char* ViewCubeFaceName(int face)
    {
        static const char* names[6] = {"Right", "Top", "Front", "Left", "Bottom", "Back"};
        return names[std::clamp(face, 0, 5)];
    }

    std::optional<EditorCamera::View> FaceView(CubeDirection d)
    {
        using View = EditorCamera::View;
        if (std::abs(d.x) + std::abs(d.y) + std::abs(d.z) != 1)
            return std::nullopt;
        if (d.x)
            return d.x > 0 ? View::Right : View::Left;
        if (d.y)
            return d.y > 0 ? View::Top : View::Bottom;
        return d.z > 0 ? View::Front : View::Back;
    }

    std::pair<float, float> ViewAnglesFrom(CubeDirection direction)
    {
        if (const auto face = FaceView(direction))
            return EditorCamera::ViewAngles(*face);
        // The camera looks along -direction: yaw about +Y (0 looks along -Z), then pitch.
        const glm::vec3 forward = -glm::normalize(glm::vec3(direction));
        return {std::atan2(-forward.x, -forward.z), std::asin(glm::clamp(forward.y, -1.0f, 1.0f))};
    }

    bool IsLookingFrom(const EditorCamera& camera, glm::vec3 direction)
    {
        // Looking from +d means looking along -d.
        return glm::dot(camera.Forward(), -glm::normalize(direction)) > std::cos(glm::radians(1.0f));
    }

    std::pair<float, float> InterpolateView(std::pair<float, float> from, std::pair<float, float> to, float t)
    {
        const float tau = glm::two_pi<float>();
        t               = glm::clamp(t, 0.0f, 1.0f);
        t               = t * t * (3.0f - 2.0f * t);
        float yaw       = std::fmod(to.first - from.first, tau);
        if (yaw > glm::pi<float>())
            yaw -= tau;
        else if (yaw < -glm::pi<float>())
            yaw += tau;
        return {from.first + yaw * t, from.second + (to.second - from.second) * t};
    }

    // ---------------------------------------------------------------------- widget

    void ViewCube::Align(const EditorCamera& camera, CubeDirection direction)
    {
        if (direction == CubeDirection(0))
            return;
        if (IsLookingFrom(camera, glm::vec3(direction)))
            direction = -direction;
        const glm::ivec3 d = direction;
        const int        face = d.x > 0 ? 0 : d.y > 0 ? 1 : d.z > 0 ? 2 : d.x < 0 ? 3 : d.y < 0 ? 4 : 5;
        m_Animation = Animation {{camera.GetYaw(), camera.GetPitch()}, ViewAnglesFrom(direction), 0.0f, FaceView(direction), face};
    }

    void ViewCube::Animate(EditorCamera& camera, float deltaTime)
    {
        if (!m_Animation)
            return;
        m_Animation->Time += deltaTime / AnimationSeconds;
        if (m_Animation->Time < 1.0f)
        {
            const auto [yaw, pitch] = InterpolateView(m_Animation->From, m_Animation->To, m_Animation->Time);
            camera.SetRotation(yaw, pitch);
            return;
        }
        // Faces end in their orthographic view; edges and corners in perspective.
        if (m_Animation->Face)
        {
            camera.SetView(*m_Animation->Face);
            m_Finished = ViewCubeFaceName(m_Animation->FaceIndex);
        }
        else
        {
            camera.SetRotation(m_Animation->To.first, m_Animation->To.second);
            camera.SetOrthographic(false);
            m_Finished = "Perspective";
        }
        m_Animation.reset();
    }

    const char* ViewCube::TakeFinishedViewName()
    {
        const char* name = m_Finished;
        m_Finished       = nullptr;
        return name;
    }

    void ViewCube::Update(EditorCamera& camera, glm::vec2 centre, float half, const ViewCubeInput& input)
    {
        Animate(camera, input.DeltaTime);
        m_Cells   = ViewCubeCells(camera.GetViewMatrix(), centre, half);
        m_Labels  = ViewCubeLabels(camera.GetViewMatrix(), centre, half);
        m_Hovered = input.Over && !m_Orbiting ? PickViewCube(m_Cells, input.Mouse) : std::nullopt;

        // A press on a cell, or on the cube's area around it (which only orbits).
        if (input.Over && input.Pressed && !m_Pressed)
        {
            m_Pressed   = true;
            m_Orbiting  = false;
            m_PressedOn = m_Hovered;
            m_PressAt = m_Last = input.Mouse;
            return;
        }
        if (!m_Pressed)
            return;
        if (input.Down)
        {
            if (!m_Orbiting && glm::length(input.Mouse - m_PressAt) > DragPixels)
            {
                m_Orbiting = true;
                m_Animation.reset();
            }
            if (m_Orbiting)
            {
                camera.Orbit(input.Mouse - m_Last);
                m_Cells   = ViewCubeCells(camera.GetViewMatrix(), centre, half);
                m_Labels  = ViewCubeLabels(camera.GetViewMatrix(), centre, half);
                m_Hovered = std::nullopt;
                // From the press on, so the slop before the drag counts too.
                m_Last = input.Mouse;
            }
            return;
        }
        // Released: a click (no drag) on the cell it began on aligns the view.
        if (!m_Orbiting && m_PressedOn && m_Hovered == m_PressedOn)
            Align(camera, *m_PressedOn);
        m_Pressed  = m_Orbiting = false;
        m_PressedOn = std::nullopt;
    }
} // namespace Kans::Gizmo
