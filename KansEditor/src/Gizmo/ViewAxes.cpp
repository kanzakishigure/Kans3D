#include "ViewAxes.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace Kans::Gizmo
{
    std::array<AxisBall, 6> ViewAxisBalls(const glm::mat4& view, glm::vec2 centre, float radius)
    {
        std::array<AxisBall, 6> balls;
        const glm::mat3         rotation(view);
        for (int i = 0; i < 6; ++i)
        {
            glm::vec3 axis(0.0f);
            axis[i % 3]     = i < 3 ? 1.0f : -1.0f;
            const glm::vec3 d = rotation * axis; // View space: x right, y up, z toward the camera.
            balls[i]        = {i % 3, i >= 3, centre + glm::vec2(d.x, -d.y) * radius, -d.z};
        }
        std::stable_sort(balls.begin(), balls.end(), [](const AxisBall& a, const AxisBall& b) { return a.Depth > b.Depth; });
        return balls;
    }

    int PickViewAxisBall(const std::array<AxisBall, 6>& balls, glm::vec2 mouse, float ballRadius)
    {
        // Back to front: the last hit is the front-most.
        int hit = -1;
        for (int i = 0; i < 6; ++i)
            if (glm::length(balls[i].Screen - mouse) <= ballRadius)
                hit = i;
        return hit;
    }

    EditorCamera::View BallView(int axis, bool negative)
    {
        using View = EditorCamera::View;
        switch (axis)
        {
            case 0:
                return negative ? View::Left : View::Right;
            case 1:
                return negative ? View::Bottom : View::Top;
            default:
                return negative ? View::Back : View::Front;
        }
    }

    bool IsLookingFrom(const EditorCamera& camera, int axis, bool negative)
    {
        glm::vec3 from(0.0f);
        from[axis] = negative ? -1.0f : 1.0f;
        // Looking from +axis means looking along -axis.
        return glm::dot(camera.Forward(), -from) > std::cos(glm::radians(1.0f));
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
} // namespace Kans::Gizmo
