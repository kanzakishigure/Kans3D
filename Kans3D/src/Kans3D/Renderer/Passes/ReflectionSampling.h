#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
namespace Kans
{
    // Heitz 2018 visible-normal sampling, local +Z normal. PDF is over microfacet normals,
    // including normals whose reflected direction lies below the macrosurface.
    inline glm::vec3 SampleReflectionVndf(glm::vec3 v, float roughness, glm::vec2 u)
    {
        const float a     = std::max(roughness * roughness, .0001f);
        glm::vec3   vh    = glm::normalize(glm::vec3(a * v.x, a * v.y, v.z));
        const float lensq = vh.x * vh.x + vh.y * vh.y;
        glm::vec3   t1    = lensq > 0 ? glm::vec3(-vh.y, vh.x, 0) / std::sqrt(lensq) : glm::vec3(1, 0, 0);
        glm::vec3   t2    = glm::cross(vh, t1);
        float       r = std::sqrt(u.x), phi = 6.28318530718f * u.y;
        float       x = r * std::cos(phi), y = r * std::sin(phi), s = .5f * (1 + vh.z);
        y            = (1 - s) * std::sqrt(std::max(0.f, 1 - x * x)) + s * y;
        glm::vec3 nh = x * t1 + y * t2 + std::sqrt(std::max(0.f, 1 - x * x - y * y)) * vh;
        return glm::normalize(glm::vec3(a * nh.x, a * nh.y, std::max(0.f, nh.z)));
    }
    inline float ReflectionVndfPdf(glm::vec3 v, glm::vec3 h, float roughness)
    {
        if (h.z <= 0 || v.z <= 0 || glm::dot(v, h) <= 0)
            return 0;
        const float a = std::max(roughness * roughness, .0001f), a2 = a * a;
        const float d  = (a2 * h.z * h.z + (1 - h.z * h.z));
        const float g1 = 2 * v.z / (v.z + std::sqrt(a2 + (1 - a2) * v.z * v.z));
        return a2 / (3.14159265359f * d * d) * g1 * glm::dot(v, h) / v.z;
    }
} // namespace Kans
