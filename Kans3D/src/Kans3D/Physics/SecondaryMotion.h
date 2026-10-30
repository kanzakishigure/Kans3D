#pragma once
#include <algorithm>
#include "Shapes.h"
namespace Kans::Physics
{
    struct Plane
    {
        glm::vec3   Normal {0, 1, 0};
        ShapeOffset Offset;
    };
    using MotionColliderShape = std::variant<Sphere, Capsule, Plane>;
    // Shared point/sphere projection for bone and future mesh solvers. Plane's Inside is ignored.
    glm::vec3 ProjectMotionParticle(const MotionColliderShape&,
                                    const glm::mat4& world,
                                    bool             inside,
                                    glm::vec3        point,
                                    float            hitRadius);
    struct SubstepClock
    {
        double   Remainder = 0, Time = 0;
        uint64_t Steps = 0;
        uint32_t Accumulate(double seconds, double step, uint32_t maxSteps);
        double   Alpha(double step) const { return std::clamp(Remainder / step, 0., 1.); }
        void     Reset()
        {
            Remainder = Time = 0;
            Steps            = 0;
        }
    };
    glm::mat4 InterpolateMotionTransform(const glm::mat4&, const glm::mat4&, float alpha);
} // namespace Kans::Physics
