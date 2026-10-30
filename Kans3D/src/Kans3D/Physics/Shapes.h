#pragma once
#include <variant>
#include <vector>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/Math/Bounds.h"

namespace Kans::Physics
{
    enum class Axis : uint8_t
    {
        X,
        Y,
        Z
    };
    struct ShapeOffset
    {
        glm::vec3 Position {0};
        glm::quat Rotation {1, 0, 0, 0};
        glm::vec3 Scale {1};
        glm::mat4 Matrix() const;
    };
    struct Sphere
    {
        float       Radius = .5f;
        ShapeOffset Offset;
    };
    struct Capsule
    {
        float       Radius = .5f, HalfHeight = .5f;
        Axis        Direction = Axis::Y;
        ShapeOffset Offset;
    };
    struct Box
    {
        glm::vec3   HalfExtents {.5f};
        ShapeOffset Offset;
    };
    // Section == UINT32_MAX selects the entire mesh. Points overrides Mesh for convexes.
    struct ConvexMesh
    {
        AssetID                Mesh;
        uint32_t               Section = UINT32_MAX;
        std::vector<glm::vec3> Points;
        ShapeOffset            Offset;
    };
    struct TriangleMesh
    {
        AssetID     Mesh;
        uint32_t    Section = UINT32_MAX;
        ShapeOffset Offset;
    };
    using Shape = std::variant<Sphere, Capsule, Box, ConvexMesh, TriangleMesh>;

    struct WorldShape
    {
        glm::vec3 Position {0}, Scale {1};
        glm::quat Rotation {1, 0, 0, 0};
    };
    WorldShape TransformShape(const ShapeOffset&, const glm::mat4& world);
    // Absolute world scale: sphere uses max(X,Y,Z); capsule uses max of the
    // two radial axes for radius and its longitudinal axis for half height.
    float  ScaledRadius(const Sphere&, const WorldShape&);
    void   CapsuleSegment(const Capsule&, const WorldShape&, glm::vec3& a, glm::vec3& b, float& radius);
    Bounds WorldAABB(const Shape&, const glm::mat4& world, const Bounds& meshBounds = {});
    struct PointContact
    {
        glm::vec3 ClosestPoint {0}, Normal {1, 0, 0};
        float     SignedDistance = 0; // Negative inside; surface point is always on the boundary.
        float     Penetration    = 0;
    };
    PointContact ClosestPoint(const Sphere&, const glm::mat4& world, glm::vec3 point);
    PointContact ClosestPoint(const Capsule&, const glm::mat4& world, glm::vec3 point);
    PointContact ClosestPoint(const Box&, const glm::mat4& world, glm::vec3 point);
} // namespace Kans::Physics
