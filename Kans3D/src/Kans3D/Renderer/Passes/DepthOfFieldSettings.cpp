#include "kspch.h"
#include "DepthOfFieldSettings.h"
#include "Kans3D/Scene/Entity.h"
namespace Kans
{
    float DepthOfFieldSettings::PhysicalCoCPixels(float depth,
                                                  float focus,
                                                  float focalMm,
                                                  float aperture,
                                                  float sensorHeightMm,
                                                  float displayHeight)
    {
        const float f = focalMm * .001f;
        return .5f * f * f * (depth - focus) /
               (std::max(aperture, .1f) * std::max(depth, .001f) * std::max(focus - f, .001f)) /
               (std::max(sensorHeightMm, .1f) * .001f) * displayHeight;
    }
    float DepthOfFieldSettings::ResolveFocusDistance(Scene& scene, const glm::mat4& view) const
    {
        if (FocusTarget)
            if (auto target = scene.GetEntityByUUID(UUID(FocusTarget)))
            {
                // Optical-axis distance agrees with linear scene depth, including off-axis targets.
                float z = -(view * scene.GetWorldTransform(target)[3]).z;
                if (std::isfinite(z) && z > .001f)
                    return z;
            }
        return Validated().FocusDistance;
    }
} // namespace Kans
