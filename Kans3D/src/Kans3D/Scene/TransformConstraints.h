#pragma once
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Core/UUID.h"

namespace YAML
{
    class Node;
}
namespace Kans
{
    // Shared source model for entity transform constraints. Offset is in joint/target space.
    struct TransformConstraintSource
    {
        UUID        Entity = 0;
        float       Weight = 1;
        std::string Joint;
        glm::vec3   Offset {0};
    };
    enum class ConstraintAxis : uint32_t
    {
        PositiveX,
        PositiveY,
        PositiveZ,
        NegativeX,
        NegativeY,
        NegativeZ
    };
    struct AimConstraintComponent
    {
        std::vector<TransformConstraintSource> Sources;
        bool                                   Active  = true;
        float                                  Weight  = 1;
        ConstraintAxis                         AimAxis = ConstraintAxis::NegativeZ, UpAxis = ConstraintAxis::PositiveY;
        glm::vec3                              WorldUp {0, 1, 0};
        UUID      UpObject      = 0; // If present, WorldUp is a direction in this object's space.
        bool      LimitsEnabled = false;
        glm::vec3 MinRotation {-180},
            MaxRotation {180};     // Degrees: X tilt, Y pan, Z roll (YXZ), relative to authored rotation.
        float MaxAngularSpeed = 0; // Degrees/s; zero is unlimited. Seek/zero delta samples statelessly.
        void  RemapEntities(const std::function<uint64_t(uint64_t)>&);
    };
    glm::vec3              ConstraintAxisVector(ConstraintAxis);
    bool                   operator==(const TransformConstraintSource&, const TransformConstraintSource&);
    bool                   operator==(const AimConstraintComponent&, const AimConstraintComponent&);
    bool                   operator!=(const AimConstraintComponent&, const AimConstraintComponent&);
    YAML::Node             EncodeAimConstraint(const AimConstraintComponent&);
    AimConstraintComponent DecodeAimConstraint(const YAML::Node&);
} // namespace Kans
