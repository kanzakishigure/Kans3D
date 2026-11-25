#include "kspch.h"
#include "AudioEmitterShape.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <glm/geometric.hpp>

namespace Kans
{
    namespace
    {
        using Basis = std::array<glm::dvec3, 3>;

        glm::dvec3 ClosestSphere(const Basis& axes, const glm::dvec3& offset)
        {
            const auto& [x, y, z] = axes;
            const auto rhs        = glm::dvec3(glm::dot(x, offset), glm::dot(y, offset), glm::dot(z, offset));
            double     upper      = glm::length(rhs);
            if (upper == 0)
                return {};
            const double scale = glm::dot(x, x) + glm::dot(y, y) + glm::dot(z, z);
            // Rotated uniform spheres are common. The same formula also covers their interiors.
            if (std::abs(glm::dot(x, x) - scale / 3) <= scale * 1e-7 &&
                std::abs(glm::dot(y, y) - scale / 3) <= scale * 1e-7 &&
                std::abs(glm::dot(z, z) - scale / 3) <= scale * 1e-7 && std::abs(glm::dot(x, y)) <= scale * 1e-7 &&
                std::abs(glm::dot(x, z)) <= scale * 1e-7 && std::abs(glm::dot(y, z)) <= scale * 1e-7)
            {
                const double radius = std::sqrt(scale / 3);
                return offset * std::min(1., radius / glm::length(offset));
            }
            // Solve (A^T A + lambda I)u = A^T offset. Precompute the
            // coefficients used by the adjugate and determinant at each step.
            const auto   xy          = glm::cross(x, y);
            const auto   xz          = glm::cross(x, z);
            const auto   yz          = glm::cross(y, z);
            const double determinant = glm::dot(x, yz);
            const auto   constant =
                determinant * glm::dvec3(glm::dot(offset, yz), -glm::dot(offset, xz), glm::dot(offset, xy));
            const glm::dvec3 linear {glm::dot(offset, glm::cross(y, xy) + glm::cross(z, xz)),
                                     glm::dot(offset, glm::cross(xy, x) + glm::cross(z, yz)),
                                     glm::dot(offset, glm::cross(xz, x) + glm::cross(yz, y))};
            const double     detSquared  = determinant * determinant;
            const double     area        = glm::dot(xy, xy) + glm::dot(xz, xz) + glm::dot(yz, yz);
            const auto       coordinates = [&](double lambda) {
                return (constant + lambda * (linear + lambda * rhs)) /
                       (detSquared + lambda * (area + lambda * (scale + lambda)));
            };

            double lower = 0;
            if (detSquared == 0)
                lower = scale * 1e-15; // Regularized projection for collapsed axes.
            auto local = coordinates(lower);
            if (glm::dot(local, local) <= 1)
                return lower == 0 ? offset : x * local.x + y * local.y + z * local.z;

            // A larger multiplier shrinks the unconstrained solution monotonically.
            for (int i = 0; i < 50; ++i)
            {
                const double lambda    = lower + (upper - lower) * 0.5;
                const auto   candidate = coordinates(lambda);
                if (glm::dot(candidate, candidate) > 1)
                    lower = lambda;
                else
                {
                    upper = lambda;
                    local = candidate;
                }
            }
            return x * local.x + y * local.y + z * local.z;
        }

        glm::dvec3 ClosestBox(const Basis& axes, const glm::dvec3& offset)
        {
            const auto& [x, y, z]    = axes;
            const auto   yz          = glm::cross(y, z);
            const double determinant = glm::dot(x, yz);
            if (determinant != 0 && std::abs(glm::dot(offset, yz) / determinant) <= 1 &&
                std::abs(glm::dot(offset, glm::cross(z, x)) / determinant) <= 1 &&
                std::abs(glm::dot(offset, glm::cross(x, y)) / determinant) <= 1)
                return offset;

            // The closest point outside lies on a face, edge or vertex. Solve over each
            // feature in world space, including for sheared or collapsed transforms.
            glm::dvec3 best {};
            double     bestSquared = std::numeric_limits<double>::infinity();
            for (int state = 0; state < 26; ++state)
            {
                glm::dvec3              point {};
                unsigned                freeCount = 0;
                std::array<unsigned, 2> freeAxes {};
                int                     code = state;
                for (unsigned axis = 0; axis < axes.size(); ++axis, code /= 3)
                {
                    const int side = code % 3;
                    if (side == 2)
                        freeAxes[freeCount++] = axis;
                    else
                        point += axes[axis] * (side == 0 ? -1. : 1.);
                }
                const auto residual = offset - point;
                if (freeCount == 1)
                {
                    const auto&  axis    = axes[freeAxes[0]];
                    const double squared = glm::dot(axis, axis);
                    if (squared == 0)
                        continue;
                    const double coordinate = glm::dot(residual, axis) / squared;
                    if (std::abs(coordinate) > 1)
                        continue;
                    point += axis * coordinate;
                }
                if (freeCount == 2)
                {
                    const auto&  lhs     = axes[freeAxes[0]];
                    const auto&  rhs     = axes[freeAxes[1]];
                    const auto   normal  = glm::cross(lhs, rhs);
                    const double squared = glm::dot(normal, normal);
                    if (squared == 0)
                        continue;
                    const double u = glm::dot(glm::cross(residual, rhs), normal) / squared;
                    const double v = glm::dot(glm::cross(lhs, residual), normal) / squared;
                    if (std::abs(u) > 1 || std::abs(v) > 1)
                        continue;
                    point += lhs * u + rhs * v;
                }
                const auto   delta   = offset - point;
                const double squared = glm::dot(delta, delta);
                if (squared < bestSquared)
                {
                    best        = point;
                    bestSquared = squared;
                }
            }
            return best;
        }
    } // namespace

    glm::vec3 ClosestAudioEmitterPoint(const AudioEmitterShape& shape, const glm::mat4& world, glm::vec3 listener)
    {
        const glm::dvec3 center(world[3]);
        const auto       offset = glm::dvec3(listener) - center;
        const Basis      axes {glm::dvec3(world[0]), glm::dvec3(world[1]), glm::dvec3(world[2])};
        switch (shape.Type)
        {
            case AudioSourceShape::Point:
                return glm::vec3(center);
            case AudioSourceShape::Sphere: {
                const double radius = std::max(0.f, shape.Radius);
                const auto   point  = ClosestSphere({axes[0] * radius, axes[1] * radius, axes[2] * radius}, offset);
                // Preserve exact zero distance for an interior point after solving in double.
                const auto delta = point - offset;
                if (glm::dot(delta, delta) <= glm::dot(offset, offset) * 1e-24)
                    return listener;
                return glm::vec3(center + point);
            }
            case AudioSourceShape::Box: {
                const auto point = ClosestBox({axes[0] * double(std::max(0.f, shape.HalfExtents.x)),
                                               axes[1] * double(std::max(0.f, shape.HalfExtents.y)),
                                               axes[2] * double(std::max(0.f, shape.HalfExtents.z))},
                                              offset);
                return point == offset ? listener : glm::vec3(center + point);
            }
            case AudioSourceShape::Line: {
                const auto axis    = axes[0] * (0.5 * std::max(0.f, shape.Length));
                const auto squared = glm::dot(axis, axis);
                return glm::vec3(center + (squared > 0 ? axis * std::clamp(glm::dot(axis, offset) / squared, -1., 1.) :
                                                         glm::dvec3(0)));
            }
        }
        return glm::vec3(center);
    }
} // namespace Kans
