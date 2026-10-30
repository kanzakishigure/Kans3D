#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Physics/Shapes.h"
namespace Kans
{
    enum class DebugDepth : uint8_t
    {
        Tested,
        Overlay
    };
    struct DebugLine
    {
        glm::vec3  A, B;
        glm::vec4  Color;
        DebugDepth Depth;
    };
    // One logical frame, shared across views. BeginFrame explicitly starts the
    // next frame; rendering never consumes lines (split viewports see the same data).
    class DebugDraw
    {
    public:
        void                          BeginFrame() { m_Lines.clear(); }
        const std::vector<DebugLine>& GetLines() const { return m_Lines; }
        void Line(glm::vec3 a, glm::vec3 b, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void
        Box(const glm::mat4& transform, glm::vec3 halfExtents, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void Sphere(glm::vec3 centre, float radius, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void Capsule(glm::vec3 a, glm::vec3 b, float radius, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void Arrow(glm::vec3 a, glm::vec3 b, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void Cross(glm::vec3 centre, float radius, glm::vec4 color, DebugDepth depth = DebugDepth::Tested);
        void Shape(const Physics::Shape&,
                   const glm::mat4& world,
                   glm::vec4        color,
                   const Bounds&    meshBounds = {},
                   DebugDepth       depth      = DebugDepth::Tested);

    private:
        std::vector<DebugLine> m_Lines;
    };
} // namespace Kans
