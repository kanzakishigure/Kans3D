#pragma once
#include <optional>
#include <glm/glm.hpp>
#include "Kans3D/Core/Math/Bounds.h"

namespace Kans
{
    // Uses the surface, else a forward ground hit within maxDistance, else
    // fallbackDistance along the unit ray. Bounds include the caller's scale;
    // their bottom-centre sits on the target, or the origin if bounds are empty.
    glm::vec3 ComputeDropPosition(const glm::vec3&                rayOrigin,
                                  const glm::vec3&                rayDirection,
                                  const std::optional<glm::vec3>& surface,
                                  const Bounds&                   localBounds,
                                  float                           fallbackDistance,
                                  float                           maxDistance = 1000.0f);
} // namespace Kans
