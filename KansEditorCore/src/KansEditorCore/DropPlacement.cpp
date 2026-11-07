#include "kspch.h"
#include "DropPlacement.h"

namespace Kans
{
    glm::vec3 ComputeDropPosition(const glm::vec3&                rayOrigin,
                                  const glm::vec3&                rayDirection,
                                  const std::optional<glm::vec3>& surface,
                                  const Bounds&                   localBounds,
                                  float                           fallbackDistance,
                                  float                           maxDistance)
    {
        glm::vec3 target = rayOrigin + rayDirection * fallbackDistance;
        if (surface)
            target = *surface;
        else if (rayDirection.y != 0.0f)
        {
            const float distance = -rayOrigin.y / rayDirection.y;
            if (distance >= 0.0f && distance <= maxDistance)
                target = rayOrigin + rayDirection * distance;
        }
        if (localBounds.IsValid())
        {
            const glm::vec3 center = localBounds.Center();
            target -= glm::vec3(center.x, localBounds.Min.y, center.z);
        }
        return target;
    }
} // namespace Kans
