#pragma once
#include <utility>
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Resource/Camera.h"

namespace Kans
{
    // Scene view camera orbiting a focus point. Navigation functions take mouse
    // deltas in pixels and times in seconds; the input mapping lives in the
    // editor's viewport. Rotation is yaw (around +Y) then pitch.
    class EditorCamera : public Camera
    {
    public:
        enum class View
        {
            Perspective,
            Top,
            Bottom,
            Front,
            Back,
            Right,
            Left,
        };

        static constexpr float MinFlySpeed = 0.1f, MaxFlySpeed = 500.0f;

        explicit EditorCamera(float verticalFovDegrees = 45.0f, float nearClip = 0.05f, float farClip = 5000.0f);

        void SetViewportSize(uint32_t width, uint32_t height);

        // Rotates around the focus point.
        void Orbit(glm::vec2 delta);
        // Rotates in place; the focus point moves with the view.
        void Look(glm::vec2 delta);
        // Slides the view so the point under the cursor follows it.
        void Pan(glm::vec2 delta);
        // Moves toward (positive) or away from the focus; pushes the focus forward
        // when it gets too close.
        void Dolly(float steps);
        // Moves by `direction` (x right, y up, z forward) at the fly speed.
        void Fly(const glm::vec3& direction, float seconds);
        // Frames a sphere.
        void Focus(const glm::vec3& center, float radius);
        // Axis views are orthographic; Perspective returns to a perspective three-quarter view.
        void SetView(View view);
        void SetOrthographic(bool orthographic);
        // Adopts an externally edited view matrix, keeping the focus distance.
        void SetViewMatrix(const glm::mat4& view);
        // Turns around the focus to a yaw and pitch (radians; pitch is clamped short of straight up/down).
        void SetRotation(float yaw, float pitch);
        float GetYaw() const { return m_Yaw; }
        float GetPitch() const { return m_Pitch; }
        // The yaw and pitch SetView uses for a view.
        static std::pair<float, float> ViewAngles(View view);

        bool             IsOrthographic() const { return m_Orthographic; }
        const glm::mat4& GetViewMatrix() const { return m_View; }
        glm::mat4        GetViewProjection() const { return m_ProjectionMatrix * m_View; }
        glm::vec3        GetPosition() const;
        glm::vec3        GetFocus() const { return m_Focus; }
        float            GetDistance() const { return m_Distance; }
        glm::vec3        Forward() const;
        glm::vec3        Right() const;
        glm::vec3        Up() const;
        float            GetFlySpeed() const { return m_FlySpeed; }
        void             SetFlySpeed(float speed);
        float            GetVerticalFov() const { return m_Fov; }
        // World-space origin and unit direction of the ray through a viewport
        // pixel (top-left origin).
        std::pair<glm::vec3, glm::vec3> ScreenRay(glm::vec2 pixel) const;

    private:
        void UpdateView();
        void UpdateProjection();

        float     m_Fov, m_Near, m_Far; // Vertical field of view in radians.
        uint32_t  m_Width = 1280, m_Height = 720;
        bool      m_Orthographic = false;
        glm::vec3 m_Focus {0.0f};
        float     m_Distance = 10.0f;
        float     m_Yaw = 0.0f, m_Pitch = 0.0f; // Radians.
        float     m_FlySpeed = 5.0f;           // Units per second.
        glm::mat4 m_View {1.0f};
    };
} // namespace Kans
