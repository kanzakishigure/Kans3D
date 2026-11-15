#include "kspch.h"
#include "LightUnits.h"
namespace Kans
{
    float SpotSolidAngle(float inner, float outer)
    {
        outer = std::clamp(outer, 0.01f, 89.0f);
        inner = std::clamp(inner, 0.0f, outer);
        // sin-squared avoids cancellation for very narrow cones.
        float si = std::sin(inner * LightPi / 360.0f), so = std::sin(outer * LightPi / 360.0f);
        return 2.0f * LightPi * (si * si + so * so);
    }
    float ToCandela(float value, LightUnit unit, float solidAngle)
    {
        return unit == LightUnit::Lumens ? value / solidAngle : value;
    }
    float FromCandela(float value, LightUnit unit, float solidAngle)
    {
        return unit == LightUnit::Lumens ? value * solidAngle : value;
    }

    void MigrateLightEmission(LightEmission& light, glm::vec3 diffuse, glm::vec3 specular)
    {
        float magnitude      = glm::length(diffuse);
        light.Color          = magnitude > 0 ? diffuse / magnitude : glm::vec3(1);
        light.Intensity      = magnitude;
        light.UseTemperature = false;
        light.DiffuseScale   = 1;
        // Legacy ratios above one remain representable during migration; the editor
        // constrains newly authored multipliers. Black diffuse has no defined ratio.
        light.SpecularScale = magnitude > 0 ? glm::length(specular) / magnitude : 0;
    }
} // namespace Kans
