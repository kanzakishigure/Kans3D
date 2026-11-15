#include "kspch.h"
#include "LightUnits.h"
namespace Kans
{
    float AreaLightArea(AreaLightShape shape, float width, float height, float radius)
    {
        return shape == AreaLightShape::Disc ? LightPi * radius * radius : width * height;
    }
    float AreaLightRadiance(float intensity, AreaLightUnit unit, float area, bool twoSided)
    {
        return unit == AreaLightUnit::Nits ?
                   std::max(0.f, intensity) :
                   std::max(0.f, intensity) / std::max(1e-8f, LightPi * area * (twoSided ? 2.f : 1.f));
    }
    std::pair<glm::vec3, glm::vec3> AreaLightAxes(const glm::mat4& world)
    {
        auto z = glm::vec3(world[2]);
        z      = glm::length(z) > 1e-6f ? glm::normalize(z) : glm::vec3(0, 0, 1);
        auto x = glm::vec3(world[0]);
        x -= z * glm::dot(z, x);
        if (glm::length(x) <= 1e-6f)
            x = glm::cross(std::abs(z.y) < .99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), z);
        x = glm::normalize(x);
        return {x, glm::cross(z, x)};
    }
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
