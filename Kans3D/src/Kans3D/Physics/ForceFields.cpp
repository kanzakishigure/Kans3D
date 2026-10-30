#include "ForceFields.h"
#include <cmath>
#include <cstdint>
namespace Kans
{
    namespace
    {
        float Noise(glm::vec3 p)
        {
            const auto cell = glm::ivec3(glm::floor(p));
            auto       f    = glm::fract(p);
            f               = f * f * (3.f - 2.f * f);
            float result    = 0;
            for (uint32_t i = 0; i < 8; ++i)
            {
                uint32_t h = uint32_t(cell.x + int(i & 1)) * 0x8da6b343u ^
                             uint32_t(cell.y + int((i >> 1) & 1)) * 0xd8163841u ^
                             uint32_t(cell.z + int(i >> 2)) * 0xcb1ab31fu;
                h ^= h >> 16;
                h *= 0x7feb352du;
                h ^= h >> 15;
                h *= 0x846ca68bu;
                h ^= h >> 16;
                float v = float(h & 0xffffffu) / float(0xffffffu) * 2.f - 1.f;
                result += v * ((i & 1) ? f.x : 1 - f.x) * ((i & 2) ? f.y : 1 - f.y) * ((i & 4) ? f.z : 1 - f.z);
            }
            return result;
        }
    } // namespace
    glm::vec3 ForceFieldSet::SampleWind(glm::vec3 position, double time) const
    {
        glm::vec3 result(0);
        for (const auto& w : m_Winds)
        {
            if (glm::dot(w.Direction, w.Direction) > 1e-12f)
                result += glm::normalize(w.Direction) * w.Strength;
            const auto p = position * w.TurbulenceFrequency + glm::vec3(float(time * w.TurbulenceFrequency));
            result += w.TurbulenceAmplitude *
                      glm::vec3(Noise(p), Noise(p + glm::vec3(19, 37, 11)), Noise(p + glm::vec3(47, 7, 29)));
        }
        return result;
    }
} // namespace Kans
