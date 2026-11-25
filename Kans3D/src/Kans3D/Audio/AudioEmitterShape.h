#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace Kans
{
    enum class AudioSourceShape
    {
        Point,
        Sphere,
        Box,
        Line
    };

    struct AudioEmitterShape
    {
        AudioSourceShape Type   = AudioSourceShape::Point;
        float            Radius = 1;
        glm::vec3        HalfExtents {1};
        float            Length = 1; // Full length, centred on the local X axis.

        bool operator==(const AudioEmitterShape& other) const
        {
            return Type == other.Type && Radius == other.Radius && HalfExtents == other.HalfExtents &&
                   Length == other.Length;
        }
    };

    glm::vec3 ClosestAudioEmitterPoint(const AudioEmitterShape& shape, const glm::mat4& world, glm::vec3 listener);
} // namespace Kans
