#include "Shapes.h"
#include <algorithm>
#include <cmath>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace Kans::Physics
{
    glm::mat4 ShapeOffset::Matrix() const
    {
        return glm::translate(glm::mat4(1), Position) * glm::mat4_cast(Rotation) * glm::scale(glm::mat4(1), Scale);
    }
    WorldShape TransformShape(const ShapeOffset& offset, const glm::mat4& world)
    {
        WorldShape result;
        glm::vec3  skew;
        glm::vec4  perspective;
        glm::decompose(world * offset.Matrix(), result.Scale, result.Rotation, result.Position, skew, perspective);
        result.Scale    = glm::abs(result.Scale);
        result.Rotation = glm::normalize(result.Rotation);
        return result;
    }
    float ScaledRadius(const Sphere& s, const WorldShape& w)
    {
        return std::abs(s.Radius) * std::max({w.Scale.x, w.Scale.y, w.Scale.z});
    }
    void CapsuleSegment(const Capsule& s, const WorldShape& w, glm::vec3& a, glm::vec3& b, float& radius)
    {
        const auto axis = unsigned(s.Direction);
        glm::vec3  delta(0);
        delta[axis] = std::abs(s.HalfHeight) * w.Scale[axis];
        delta       = w.Rotation * delta;
        a           = w.Position - delta;
        b           = w.Position + delta;
        radius      = std::abs(s.Radius) * std::max(w.Scale[(axis + 1) % 3], w.Scale[(axis + 2) % 3]);
    }
    Bounds WorldAABB(const Shape& shape, const glm::mat4& world, const Bounds& meshBounds)
    {
        return std::visit(
            [&](const auto& s) {
                using T      = std::decay_t<decltype(s)>;
                const auto w = TransformShape(s.Offset, world);
                Bounds     result;
                if constexpr (std::is_same_v<T, Sphere>)
                {
                    const glm::vec3 r(ScaledRadius(s, w));
                    result = {w.Position - r, w.Position + r};
                }
                else if constexpr (std::is_same_v<T, Capsule>)
                {
                    glm::vec3 a, b;
                    float     r;
                    CapsuleSegment(s, w, a, b, r);
                    result = {glm::min(a, b) - r, glm::max(a, b) + r};
                }
                else if constexpr (std::is_same_v<T, Box>)
                {
                    const auto h = glm::abs(s.HalfExtents) * w.Scale;
                    result       = Bounds {-h, h}.Transformed(glm::translate(glm::mat4(1), w.Position) *
                                                              glm::mat4_cast(w.Rotation));
                }
                else
                {
                    auto local = meshBounds;
                    if constexpr (std::is_same_v<T, ConvexMesh>)
                    {
                        if (!s.Points.empty())
                            local = {};
                        for (const auto& p : s.Points)
                            local.Expand(p);
                    }
                    result = local.Transformed(world * s.Offset.Matrix());
                }
                return result;
            },
            shape);
    }
    static PointContact RadialContact(glm::vec3 centre, float radius, glm::vec3 point, glm::vec3 fallback)
    {
        const auto  delta    = point - centre;
        const float length   = glm::length(delta);
        const auto  normal   = length > 1e-8f ? delta / length : fallback;
        const float distance = length - radius;
        return {centre + normal * radius, normal, distance, std::max(0.f, -distance)};
    }
    PointContact ClosestPoint(const Sphere& s, const glm::mat4& world, glm::vec3 point)
    {
        const auto w = TransformShape(s.Offset, world);
        return RadialContact(w.Position, ScaledRadius(s, w), point, w.Rotation * glm::vec3(1, 0, 0));
    }
    PointContact ClosestPoint(const Capsule& s, const glm::mat4& world, glm::vec3 point)
    {
        const auto w = TransformShape(s.Offset, world);
        glm::vec3  a, b;
        float      radius;
        CapsuleSegment(s, w, a, b, radius);
        const auto  ab = b - a;
        const float t =
            glm::dot(ab, ab) > 1e-12f ? glm::clamp(glm::dot(point - a, ab) / glm::dot(ab, ab), 0.f, 1.f) : 0.f;
        glm::vec3 fallback(0);
        fallback[(unsigned(s.Direction) + 1) % 3] = 1;
        return RadialContact(a + ab * t, radius, point, w.Rotation * fallback);
    }
    PointContact ClosestPoint(const Box& s, const glm::mat4& world, glm::vec3 point)
    {
        const auto w        = TransformShape(s.Offset, world);
        const auto h        = glm::abs(s.HalfExtents) * w.Scale;
        const auto p        = glm::conjugate(w.Rotation) * (point - w.Position);
        auto       q        = glm::clamp(p, -h, h);
        auto       d        = p - q;
        float      distance = glm::length(d);
        glm::vec3  n(0);
        if (distance > 1e-8f)
            n = d / distance;
        else
        {
            const auto gaps = h - glm::abs(p);
            unsigned   axis = gaps.x <= gaps.y && gaps.x <= gaps.z ? 0 : (gaps.y <= gaps.z ? 1 : 2);
            n[axis]         = p[axis] < 0 ? -1.f : 1.f;
            q[axis]         = h[axis] * n[axis];
            distance        = -gaps[axis];
        }
        return {w.Position + w.Rotation * q, w.Rotation * n, distance, std::max(0.f, -distance)};
    }
} // namespace Kans::Physics
