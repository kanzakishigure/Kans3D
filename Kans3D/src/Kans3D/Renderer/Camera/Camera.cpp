#include "kspch.h"
#include "Kans3D/Renderer/Camera/Camera.h"

namespace Kans
{

    Camera::Camera(const glm::mat4& ProjectionMatrix) : m_ProjectionMatrix(ProjectionMatrix) {}

    void Camera::SetPerspectiveProjectionMatrix(const float radFov,
                                                const float width,
                                                const float height,
                                                const float nearP,
                                                const float farP)
    {
        m_ProjectionMatrix = glm::perspectiveFov(radFov, width, height, farP, nearP);
    }

} // namespace Kans