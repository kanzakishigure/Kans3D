#pragma once
#include <algorithm>
#include <cmath>
#include <utility>
#include <glm/glm.hpp>

namespace Kans
{
    enum class LightUnit
    {
        Candela,
        Lumens
    };
    enum class AreaLightShape
    {
        Rectangle,
        Disc
    };
    enum class AreaLightUnit
    {
        Nits,
        Lumens
    };
    constexpr float LightPi = 3.14159265358979323846f;
    inline float    PointSolidAngle() { return 4.0f * LightPi; }
    float           AreaLightArea(AreaLightShape shape, float width, float height, float radius);
    // Lambert emitter: flux = pi * area * luminance per emitting side.
    // Radiance follows the engine's photometric convention (no 683 lm/W conversion).
    float AreaLightRadiance(float intensity, AreaLightUnit unit, float area, bool twoSided = false);

    // Cone angles are half angles in degrees. Integrating smoothstep in cosine
    // gives half the transition band, so lumen output stays constant as the cone changes.
    float        SpotSolidAngle(float inner, float outer);
    float        ToCandela(float value, LightUnit unit, float solidAngle = PointSolidAngle());
    float        FromCandela(float value, LightUnit unit, float solidAngle = PointSolidAngle());
    inline float LightRangeWindow(float distance, float range)
    {
        if (range <= 0)
            return 1; // Migrated unbounded punctual lights.
        float x = distance / range;
        float w = std::max(0.0f, 1.0f - x * x * x * x);
        return w * w;
    }
    inline float SpotConeWindow(float cosine, float inner, float outer)
    {
        float ci = std::cos(std::clamp(inner, 0.0f, outer) * LightPi / 180.0f);
        float co = std::cos(outer * LightPi / 180.0f);
        if (ci == co)
            return cosine >= co ? 1.0f : 0.0f;
        float t = std::clamp((cosine - co) / (ci - co), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    }
    // Approximate blackbody chromaticity, converted from sRGB to linear RGB.
    inline glm::vec3 LightTemperatureRGB(float kelvin)
    {
        float     t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
        glm::vec3 c(t <= 66 ? 255 : 329.698727446f * std::pow(t - 60, -0.1332047592f),
                    t <= 66 ? 99.4708025861f * std::log(t) - 161.1195681661f :
                              288.1221695283f * std::pow(t - 60, -0.0755148492f),
                    t >= 66 ? 255 :
                    t <= 19 ? 0 :
                              138.5177312231f * std::log(t - 10) - 305.0447927307f);
        c = glm::clamp(c / 255.0f, glm::vec3(0), glm::vec3(1));
        for (int i = 0; i < 3; ++i)
            c[i] = c[i] <= .04045f ? c[i] / 12.92f : std::pow((c[i] + .055f) / 1.055f, 2.4f);
        return c;
    }
    std::pair<glm::vec3, glm::vec3> AreaLightAxes(const glm::mat4& world);
    struct LightEmission
    {
        glm::vec3 Color {1};          // Linear RGB.
        float     Intensity      = 1; // Lux for directional; Unit for punctual lights.
        bool      UseTemperature = false;
        float     Temperature    = 6500;               // Kelvin; replaces Color when enabled.
        float     DiffuseScale = 1, SpecularScale = 1; // Authored controls [0,1].
        bool      operator==(const LightEmission& b) const
        {
            return Color == b.Color && Intensity == b.Intensity && UseTemperature == b.UseTemperature &&
                   Temperature == b.Temperature && DiffuseScale == b.DiffuseScale && SpecularScale == b.SpecularScale;
        }
        bool operator!=(const LightEmission& b) const { return !(*this == b); }
    };
    inline glm::vec3 LightColor(const LightEmission& light)
    {
        return light.UseTemperature ? LightTemperatureRGB(light.Temperature) : light.Color;
    }
    // Exposure remains the existing linear post-process multiplier (default .8).
    // One lux, or one candela at one metre, supplies the former diffuse=1 radiance.
    // No 683 lm/W or camera EV conversion is applied; camera exposure is a later feature.
    inline glm::vec3 LightRadiance(const LightEmission& light, float intensity)
    {
        return LightColor(light) * std::max(0.0f, intensity);
    }
    inline glm::vec3 LightRadiance(const LightEmission& light) { return LightRadiance(light, light.Intensity); }
    void             MigrateLightEmission(LightEmission& light, glm::vec3 diffuse, glm::vec3 specular);

} // namespace Kans
