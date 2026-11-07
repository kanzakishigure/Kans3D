#include "kspch.h"
#include "KansEditorCore/EditorCamera.h"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <tuple>

namespace Kans
{
    namespace
    {
        constexpr float RotateSpeed = 0.005f; // Radians per pixel.
        constexpr float MinDistance = 0.05f;
        constexpr float PitchLimit  = glm::half_pi<float>() - 0.001f;
    } // namespace

    EditorCamera::EditorCamera(float verticalFovDegrees, float nearClip, float farClip) :
        m_Fov(glm::radians(verticalFovDegrees)), m_Near(nearClip), m_Far(farClip)
    {
        SetView(View::Perspective);
    }

    void EditorCamera::SetViewportSize(uint32_t width, uint32_t height)
    {
        if (!width || !height || (width == m_Width && height == m_Height))
            return;
        m_Width  = width;
        m_Height = height;
        UpdateProjection();
    }

    glm::vec3 EditorCamera::Forward() const
    {
        return glm::angleAxis(m_Yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(m_Pitch, glm::vec3(1, 0, 0)) *
               glm::vec3(0, 0, -1);
    }

    glm::vec3 EditorCamera::Right() const { return glm::angleAxis(m_Yaw, glm::vec3(0, 1, 0)) * glm::vec3(1, 0, 0); }

    glm::vec3 EditorCamera::Up() const { return glm::normalize(glm::cross(Right(), Forward())); }

    glm::vec3 EditorCamera::GetPosition() const { return m_Focus - Forward() * m_Distance; }

    void EditorCamera::Orbit(glm::vec2 delta)
    {
        m_Yaw -= delta.x * RotateSpeed;
        m_Pitch = glm::clamp(m_Pitch - delta.y * RotateSpeed, -PitchLimit, PitchLimit);
        UpdateView();
    }

    void EditorCamera::Look(glm::vec2 delta)
    {
        const glm::vec3 position = GetPosition();
        m_Yaw -= delta.x * RotateSpeed;
        m_Pitch = glm::clamp(m_Pitch - delta.y * RotateSpeed, -PitchLimit, PitchLimit);
        m_Focus = position + Forward() * m_Distance;
        UpdateView();
    }

    void EditorCamera::Pan(glm::vec2 delta)
    {
        // World units per pixel at the focus distance.
        const float unitsPerPixel = 2.0f * m_Distance * std::tan(m_Fov * 0.5f) / float(std::max(m_Height, 1u));
        m_Focus += (-Right() * delta.x + Up() * delta.y) * unitsPerPixel;
        UpdateView();
    }

    void EditorCamera::Dolly(float steps)
    {
        const float distance = m_Distance * std::pow(0.85f, steps);
        if (distance < MinDistance)
        {
            // Keep a minimum distance by moving the focus forward instead.
            m_Focus += Forward() * (m_Distance - distance);
            m_Distance = MinDistance;
        }
        else
            m_Distance = std::min(distance, m_Far * 0.5f);
        UpdateProjection();
        UpdateView();
    }

    void EditorCamera::Fly(const glm::vec3& direction, float seconds)
    {
        m_Focus += (Right() * direction.x + glm::vec3(0, 1, 0) * direction.y + Forward() * direction.z) *
                   m_FlySpeed * seconds;
        UpdateView();
    }

    void EditorCamera::Focus(const glm::vec3& center, float radius)
    {
        m_Focus    = center;
        m_Distance = std::max(MinDistance, std::max(radius, 0.01f) / std::sin(m_Fov * 0.5f) * 1.1f);
        UpdateProjection();
        UpdateView();
    }

    std::pair<float, float> EditorCamera::ViewAngles(View view)
    {
        constexpr float pi = glm::pi<float>();
        switch (view)
        {
            case View::Perspective:
                return {pi * 0.25f, -pi / 6.0f};
            case View::Top:
                return {0.0f, -PitchLimit};
            case View::Bottom:
                return {0.0f, PitchLimit};
            case View::Front:
                return {0.0f, 0.0f};
            case View::Back:
                return {pi, 0.0f};
            case View::Right:
                return {pi * 0.5f, 0.0f};
            case View::Left:
                return {-pi * 0.5f, 0.0f};
        }
        return {0.0f, 0.0f};
    }

    void EditorCamera::SetView(View view)
    {
        std::tie(m_Yaw, m_Pitch) = ViewAngles(view);
        m_Orthographic = view != View::Perspective;
        UpdateProjection();
        UpdateView();
    }

    void EditorCamera::SetRotation(float yaw, float pitch)
    {
        m_Yaw   = yaw;
        m_Pitch = glm::clamp(pitch, -PitchLimit, PitchLimit);
        UpdateView();
    }

    void EditorCamera::SetOrthographic(bool orthographic)
    {
        m_Orthographic = orthographic;
        UpdateProjection();
    }

    void EditorCamera::SetViewMatrix(const glm::mat4& view)
    {
        const glm::mat4 world    = glm::inverse(view);
        const glm::vec3 position = glm::vec3(world[3]);
        const glm::vec3 forward  = -glm::normalize(glm::vec3(world[2]));
        m_Pitch                  = glm::clamp(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)), -PitchLimit, PitchLimit);
        m_Yaw                    = std::atan2(-forward.x, -forward.z);
        m_Focus                  = position + Forward() * m_Distance;
        UpdateView();
    }

    void EditorCamera::SetFlySpeed(float speed) { m_FlySpeed = glm::clamp(speed, MinFlySpeed, MaxFlySpeed); }

    std::pair<glm::vec3, glm::vec3> EditorCamera::ScreenRay(glm::vec2 pixel) const
    {
        const glm::vec2 ndc(pixel.x / float(m_Width) * 2.0f - 1.0f, 1.0f - pixel.y / float(m_Height) * 2.0f);
        const glm::mat4 inverse = glm::inverse(GetViewProjection());
        glm::vec4       nearPoint = inverse * glm::vec4(ndc, -1.0f, 1.0f);
        glm::vec4       farPoint  = inverse * glm::vec4(ndc, 1.0f, 1.0f);
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        return {glm::vec3(nearPoint), glm::normalize(glm::vec3(farPoint - nearPoint))};
    }

    void EditorCamera::UpdateView() { m_View = glm::lookAt(GetPosition(), m_Focus, Up()); }

    void EditorCamera::UpdateProjection()
    {
        const float aspect = float(m_Width) / float(std::max(m_Height, 1u));
        if (m_Orthographic)
        {
            // The visible height matches what the perspective view shows at the focus.
            const float halfHeight = m_Distance * std::tan(m_Fov * 0.5f);
            const float halfWidth  = halfHeight * aspect;
            m_ProjectionMatrix = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, -m_Far, m_Far);
        }
        else
            m_ProjectionMatrix = glm::perspective(m_Fov, aspect, m_Near, m_Far);
    }
} // namespace Kans
