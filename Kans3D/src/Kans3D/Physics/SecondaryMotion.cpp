#include "kspch.h"
#include "SecondaryMotion.h"
#include "Kans3D/Animation/AnimationData.h"
namespace Kans::Physics
{
    uint32_t SubstepClock::Accumulate(double seconds, double step, uint32_t maxSteps)
    {
        if (std::isfinite(seconds) && seconds > 0)
            Remainder += seconds;
        const double epsilon   = step * 1e-5;
        const auto   available = std::floor((Remainder + epsilon) / step);
        const auto   count     = uint32_t(std::min(available, double(maxSteps)));
        Remainder              = std::max(0., Remainder - available * step); // Drop overload, keep fractional phase.
        return count;
    }
    glm::mat4 InterpolateMotionTransform(const glm::mat4& a, const glm::mat4& b, float t)
    {
        if (a == b)
            return a; // Preserve identical inputs exactly across different frame/substep phases.
        const auto     x = JointTransform::FromMatrix(a), y = JointTransform::FromMatrix(b);
        JointTransform out;
        out.Translation = glm::mix(x.Translation, y.Translation, t);
        out.Rotation    = glm::slerp(x.Rotation, y.Rotation, t);
        out.Scale       = glm::mix(x.Scale, y.Scale, t);
        return out.Matrix();
    }
    glm::vec3 ProjectMotionParticle(const MotionColliderShape& shape,
                                    const glm::mat4&           world,
                                    bool                       inside,
                                    glm::vec3                  point,
                                    float                      radius)
    {
        return std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, Plane>)
                {
                    const auto m      = world * s.Offset.Matrix();
                    auto       normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(m))) * s.Normal);
                    const auto d      = glm::dot(point - glm::vec3(m[3]), normal) - radius;
                    return point - std::min(d, 0.f) * normal;
                }
                else
                {
                    const auto c = ClosestPoint(s, world, point);
                    const bool containment = inside && !std::is_same_v<T, Box>;
                    const auto d           = containment ? c.SignedDistance + radius : radius - c.SignedDistance;
                    return point + (containment ? -1.f : 1.f) * std::max(d, 0.f) * c.Normal;
                }
            },
            shape);
    }
} // namespace Kans::Physics
