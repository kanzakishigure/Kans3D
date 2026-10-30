#include "DebugDraw.h"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
namespace Kans
{
    void DebugDraw::Line(glm::vec3 a, glm::vec3 b, glm::vec4 c, DebugDepth d) { m_Lines.push_back({a, b, c, d}); }
    void DebugDraw::Box(const glm::mat4& t, glm::vec3 h, glm::vec4 c, DebugDepth d)
    {
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned a = 0; a < 3; ++a)
                if (!(i & (1u << a)))
                {
                    glm::vec3 p((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z), q = p;
                    q[a] = h[a];
                    Line(glm::vec3(t * glm::vec4(p, 1)), glm::vec3(t * glm::vec4(q, 1)), c, d);
                }
    }
    void DebugDraw::Sphere(glm::vec3 p, float r, glm::vec4 c, DebugDepth d)
    {
        for (int a = 0; a < 3; ++a)
            for (int i = 0; i < 32; ++i)
            {
                float     t = float(i) * 6.28318530718f / 32, u = float(i + 1) * 6.28318530718f / 32;
                glm::vec3 x(0), y(0);
                x[(a + 1) % 3] = std::cos(t) * r;
                x[(a + 2) % 3] = std::sin(t) * r;
                y[(a + 1) % 3] = std::cos(u) * r;
                y[(a + 2) % 3] = std::sin(u) * r;
                Line(p + x, p + y, c, d);
            }
    }
    void DebugDraw::Capsule(glm::vec3 a, glm::vec3 b, float r, glm::vec4 c, DebugDepth d)
    {
        const auto axis = glm::length(b - a) > 1e-8f ? glm::normalize(b - a) : glm::vec3(0, 1, 0);
        const auto u =
            glm::normalize(glm::cross(axis, std::abs(axis.x) < .8f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1)));
        const auto v = glm::cross(axis, u);
        for (int i = 0; i < 32; ++i)
        {
            float t = float(i) * 6.28318530718f / 32, t1 = float(i + 1) * 6.28318530718f / 32;
            auto  p = (u * std::cos(t) + v * std::sin(t)) * r, q = (u * std::cos(t1) + v * std::sin(t1)) * r;
            Line(a + p, a + q, c, d);
            Line(b + p, b + q, c, d);
            if (i % 8 == 0)
                Line(a + p, b + p, c, d);
        }
        for (auto radial : {u, v})
            for (int i = 0; i < 16; ++i)
            {
                float t = float(i) * 3.14159265359f / 16, t1 = float(i + 1) * 3.14159265359f / 16;
                Line(a + radial * r * std::cos(t) - axis * r * std::sin(t),
                     a + radial * r * std::cos(t1) - axis * r * std::sin(t1),
                     c,
                     d);
                Line(b + radial * r * std::cos(t) + axis * r * std::sin(t),
                     b + radial * r * std::cos(t1) + axis * r * std::sin(t1),
                     c,
                     d);
            }
    }
    void DebugDraw::Arrow(glm::vec3 a, glm::vec3 b, glm::vec4 c, DebugDepth d)
    {
        Line(a, b, c, d);
        const auto  delta = b - a;
        const float l     = glm::length(delta);
        if (l < 1e-8f)
            return;
        const auto n = delta / l;
        const auto side =
            glm::normalize(glm::cross(n, std::abs(n.x) < .8f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0))) * l * .1f;
        Line(b, b - delta * .2f + side, c, d);
        Line(b, b - delta * .2f - side, c, d);
    }
    void DebugDraw::Cross(glm::vec3 p, float r, glm::vec4 c, DebugDepth d)
    {
        for (int a = 0; a < 3; ++a)
        {
            glm::vec3 v(0);
            v[a] = r;
            Line(p - v, p + v, c, d);
        }
    }
    void DebugDraw::Shape(const Physics::Shape& shape,
                          const glm::mat4&      world,
                          glm::vec4             c,
                          const Bounds&         bounds,
                          DebugDepth            d)
    {
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                auto w  = Physics::TransformShape(s.Offset, world);
                if constexpr (std::is_same_v<T, Physics::Sphere>)
                    Sphere(w.Position, Physics::ScaledRadius(s, w), c, d);
                else if constexpr (std::is_same_v<T, Physics::Capsule>)
                {
                    glm::vec3 a, b;
                    float     r;
                    Physics::CapsuleSegment(s, w, a, b, r);
                    Capsule(a, b, r, c, d);
                }
                else if constexpr (std::is_same_v<T, Physics::Box>)
                    Box(glm::translate(glm::mat4(1), w.Position) * glm::mat4_cast(w.Rotation),
                        glm::abs(s.HalfExtents) * w.Scale,
                        c,
                        d);
                else
                {
                    auto bb = Physics::WorldAABB(shape, world, bounds);
                    if (bb.IsValid())
                        Box(glm::translate(glm::mat4(1), bb.Center()), (bb.Max - bb.Min) * .5f, c, d);
                }
            },
            shape);
    }
} // namespace Kans
