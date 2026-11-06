#include "kspch.h"
#include "Kans3D/Renderer/Camera/OrthographicCamera.h"

#include <glm/gtc/matrix_transform.hpp>
namespace Kans
{

    OrthographicCamera::OrthographicCamera(float left, float right, float bottom, float top) :
        m_ProjectionMatrix(glm::ortho(left, right, bottom, top)), m_ViewMatrix(glm::mat4(1.0f))
    {
        PROFILE_FUCTION();

        m_ViewProjectionMatrtix = m_ProjectionMatrix * m_ViewMatrix;
    }

    void OrthographicCamera::SetProjection(float left, float right, float bottom, float top)
    {
        PROFILE_FUCTION();

        m_ProjectionMatrix      = glm::ortho(left, right, bottom, top);
        m_ViewProjectionMatrtix = m_ProjectionMatrix * m_ViewMatrix;
    }

    void OrthographicCamera::RecalculateViewMatrix()
    {
        PROFILE_FUCTION();

        glm::mat4 transform = glm::translate(glm::mat4(1.0), m_Position) *
                              glm::rotate(glm::mat4(1.0),
                                          m_Rotation,
                                          glm::vec3(0, 0, 1)); // 因为此处为2d平面，沿z轴旋转，固为向量vec3(0,0,1)

        m_ViewMatrix            = glm::inverse(transform); // 矩阵求逆
        m_ViewProjectionMatrtix = m_ProjectionMatrix * m_ViewMatrix;
    }

} // namespace Kans