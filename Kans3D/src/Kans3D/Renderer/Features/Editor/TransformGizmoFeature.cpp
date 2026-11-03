#include "kspch.h"
#include "TransformGizmoFeature.h"
#include <glm/gtc/constants.hpp>
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Resource/InternalShader.h"

namespace Kans
{
    namespace
    {
        // Instances at or above MaxDraws are the occluded copies (faded); the index wraps.
        constexpr const char* Vertex   = R"glsl(#version 450
layout(location=0) in vec3 Position;
layout(location=1) in vec3 Normal;
struct Item { mat4 Model; vec4 Color; };
layout(set=0,binding=0,std140) uniform Parameters { mat4 ViewProjection; mat4 View; vec4 Options; Item Items[128]; } p;
layout(location=0) out vec4 HandleColor;
void main() {
    Item item = p.Items[gl_InstanceIndex % 128];
    gl_Position = p.ViewProjection * (item.Model * vec4(Position, 1));
    float shade = 1.0;
    if (dot(Normal, Normal) > 0.25) {
        vec3 n = normalize(mat3(p.View) * (transpose(inverse(mat3(item.Model))) * Normal));
        shade = 0.62 + 0.38 * abs(n.z);
    }
    HandleColor = vec4(item.Color.rgb * shade, item.Color.a * (gl_InstanceIndex >= 128 ? p.Options.x : 1.0));
}
)glsl";
        constexpr const char* Fragment = R"glsl(#version 450
layout(location=0) in vec4 HandleColor;
layout(location=0) out vec4 Color;
void main() { Color = HandleColor; }
)glsl";
        struct HandleVertex
        {
            glm::vec3 Position, Normal;
        };
        struct Item
        {
            glm::mat4 Model;
            glm::vec4 Color;
        };
        struct Parameters
        {
            glm::mat4 ViewProjection, View;
            glm::vec4 Options;
            Item      Items[TransformGizmoFeature::MaxDraws];
        };
        static_assert(TransformGizmoFeature::MaxDraws == 128, "The shader's item array has 128 entries.");

        struct MeshBuilder
        {
            std::vector<HandleVertex> Vertices;
            std::vector<uint32_t>     Indices;
            uint32_t                  Add(glm::vec3 position, glm::vec3 normal)
            {
                Vertices.push_back({position, normal});
                return uint32_t(Vertices.size() - 1);
            }
            void Triangle(uint32_t a, uint32_t b, uint32_t c) { Indices.insert(Indices.end(), {a, b, c}); }
            void Line(uint32_t a, uint32_t b) { Indices.insert(Indices.end(), {a, b}); }
            // A grid wrapped around in u (columns) and spanning v (rows); counter-clockwise faces
            // point along cross(dP/du, dP/dv).
            template<typename Surface>
            void Grid(uint32_t columns, uint32_t rows, bool wrapRows, Surface surface)
            {
                const uint32_t first = uint32_t(Vertices.size());
                const uint32_t ring  = rows + (wrapRows ? 0 : 1);
                for (uint32_t i = 0; i < columns; ++i)
                    for (uint32_t j = 0; j < ring; ++j)
                    {
                        auto [position, normal] = surface(float(i) / columns, float(j) / rows);
                        Add(position, normal);
                    }
                const auto at = [&](uint32_t i, uint32_t j) { return first + (i % columns) * ring + (j % ring); };
                for (uint32_t i = 0; i < columns; ++i)
                    for (uint32_t j = 0; j < rows; ++j)
                    {
                        Triangle(at(i, j), at(i + 1, j), at(i + 1, j + 1));
                        Triangle(at(i, j), at(i + 1, j + 1), at(i, j + 1));
                    }
            }
            // A flat fan facing +z (front) or -z.
            void Fan(float z, bool front, uint32_t segments)
            {
                const glm::vec3 normal(0, 0, front ? 1.0f : -1.0f);
                const uint32_t  centre = Add({0, 0, z}, normal);
                const uint32_t  first  = uint32_t(Vertices.size());
                for (uint32_t i = 0; i < segments; ++i)
                {
                    const float a = glm::two_pi<float>() * float(i) / segments;
                    Add({std::cos(a), std::sin(a), z}, normal);
                }
                for (uint32_t i = 0; i < segments; ++i)
                {
                    const uint32_t a = first + i, b = first + (i + 1) % segments;
                    if (front)
                        Triangle(centre, a, b);
                    else
                        Triangle(centre, b, a);
                }
            }
        };
    } // namespace

    void TransformGizmoFeature::Build(RenderHardware::Device& device)
    {
        using namespace RenderHardware;
        constexpr float pi = glm::pi<float>(), tau = glm::two_pi<float>();
        MeshBuilder     m;
        const auto      begin = [&](GizmoShape shape) { m_Shapes[size_t(shape)].First = uint32_t(m.Indices.size()); };
        const auto      end   = [&](GizmoShape shape) {
            auto& range = m_Shapes[size_t(shape)];
            range.Count = uint32_t(m.Indices.size()) - range.First;
        };
        constexpr uint32_t Round = 24;

        begin(GizmoShape::Cylinder);
        m.Grid(Round, 1, false, [](float u, float v) {
            const float a = tau * u;
            return std::pair {glm::vec3(std::cos(a), std::sin(a), v), glm::vec3(std::cos(a), std::sin(a), 0)};
        });
        m.Fan(0, false, Round);
        m.Fan(1, true, Round);
        end(GizmoShape::Cylinder);

        begin(GizmoShape::Cone);
        m.Grid(Round, 1, false, [](float u, float v) {
            const float a = tau * u;
            return std::pair {glm::vec3(std::cos(a) * (1 - v), std::sin(a) * (1 - v), v),
                              glm::normalize(glm::vec3(std::cos(a), std::sin(a), 1))};
        });
        m.Fan(0, false, Round);
        end(GizmoShape::Cone);

        begin(GizmoShape::Cube);
        for (int axis = 0; axis < 3; ++axis)
            for (float sign : {1.0f, -1.0f})
            {
                glm::vec3 n(0), u(0), v(0);
                n[axis]           = sign;
                u[(axis + 1) % 3] = 1;
                v                 = glm::cross(n, u);
                const uint32_t a  = m.Add(n - u - v, n);
                const uint32_t b  = m.Add(n + u - v, n);
                const uint32_t c  = m.Add(n + u + v, n);
                const uint32_t d  = m.Add(n - u + v, n);
                m.Triangle(a, b, c);
                m.Triangle(a, c, d);
            }
        end(GizmoShape::Cube);

        begin(GizmoShape::Quad);
        for (float sign : {1.0f, -1.0f})
        {
            const glm::vec3 n(0, 0, sign);
            const uint32_t  a = m.Add({-1, -1, 0}, n), b = m.Add({1, -1, 0}, n), c = m.Add({1, 1, 0}, n),
                           d = m.Add({-1, 1, 0}, n);
            if (sign > 0)
                m.Triangle(a, b, c), m.Triangle(a, c, d);
            else
                m.Triangle(a, c, b), m.Triangle(a, d, c);
        }
        end(GizmoShape::Quad);

        begin(GizmoShape::Torus);
        // Thin enough for a ring of about 100 px to be about 4 px thick.
        constexpr float minor = 0.02f;
        m.Grid(96, 8, true, [](float u, float v) {
            const float     a = tau * u, b = tau * v;
            const glm::vec3 normal(std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b));
            return std::pair {glm::vec3(std::cos(a), std::sin(a), 0) + normal * minor, normal};
        });
        end(GizmoShape::Torus);

        begin(GizmoShape::Sphere);
        m.Grid(Round, 12, false, [](float u, float v) {
            const float     a = tau * u, b = pi * (v - 0.5f);
            const glm::vec3 p(std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b));
            return std::pair {p, p};
        });
        end(GizmoShape::Sphere);

        begin(GizmoShape::Disc);
        m.Fan(0, true, 32);
        m.Fan(0, false, 32);
        end(GizmoShape::Disc);

        // Lines carry no normal: they are drawn unshaded.
        begin(GizmoShape::Line);
        m.Line(m.Add({0, 0, 0}, glm::vec3(0)), m.Add({0, 0, 1}, glm::vec3(0)));
        end(GizmoShape::Line);

        begin(GizmoShape::Square);
        {
            const uint32_t first = m.Add({-1, -1, 0}, glm::vec3(0));
            m.Add({1, -1, 0}, glm::vec3(0));
            m.Add({1, 1, 0}, glm::vec3(0));
            m.Add({-1, 1, 0}, glm::vec3(0));
            for (uint32_t i = 0; i < 4; ++i)
                m.Line(first + i, first + (i + 1) % 4);
        }
        end(GizmoShape::Square);

        begin(GizmoShape::Circle);
        {
            const uint32_t first = uint32_t(m.Vertices.size());
            for (uint32_t i = 0; i < 96; ++i)
                m.Add({std::cos(tau * i / 96.0f), std::sin(tau * i / 96.0f), 0}, glm::vec3(0));
            for (uint32_t i = 0; i < 96; ++i)
                m.Line(first + i, first + (i + 1) % 96);
        }
        end(GizmoShape::Circle);

        const size_t vertexBytes = m.Vertices.size() * sizeof(HandleVertex);
        const size_t indexBytes  = m.Indices.size() * sizeof(uint32_t);
        m_Vertices               = device.CreateBuffer({vertexBytes, BufferUsage::Vertex, MemoryUsage::Upload});
        m_Vertices->Write(0, m.Vertices.data(), vertexBytes);
        m_Indices = device.CreateBuffer({indexBytes, BufferUsage::Index, MemoryUsage::Upload});
        m_Indices->Write(0, m.Indices.data(), indexBytes);
    }

    RenderHardware::Handle<RenderHardware::Buffer> TransformGizmoFeature::TakeUniform(RenderHardware::Device& device)
    {
        using namespace RenderHardware;
        // A block no command list still references (frames in flight hold theirs). The set is
        // created whole on first use, so later frames create none.
        if (m_Uniforms.empty())
            for (uint32_t i = 0; i < 3; ++i)
                m_Uniforms.push_back(device.CreateBuffer({sizeof(Parameters), BufferUsage::Uniform, MemoryUsage::Upload}));
        for (const auto& buffer : m_Uniforms)
            if (buffer.use_count() == 1)
                return buffer;
        m_Uniforms.push_back(device.CreateBuffer({sizeof(Parameters), BufferUsage::Uniform, MemoryUsage::Upload}));
        return m_Uniforms.back();
    }

    void TransformGizmoFeature::Record(Renderer&                            renderer,
                                       RenderHardware::CommandList&         commands,
                                       const RenderView&                    view,
                                       const RenderHardware::RenderingDesc& target)
    {
        using namespace RenderHardware;
        if (view.Gizmo.empty() || target.Colors.empty())
            return;
        auto device = renderer.GetDevice();
        if (!m_Vertices)
            Build(*device);

        const size_t count = std::min<size_t>(view.Gizmo.size(), MaxDraws);
        Parameters   parameters {};
        parameters.ViewProjection =
            DeviceProjection(view.Scene.sceneCamera.camera.GetProjectionMatrix() * view.Scene.sceneCamera.viewMatrix,
                             device->GetCoordinateConventions());
        parameters.View    = view.Scene.sceneCamera.viewMatrix;
        parameters.Options = {OccludedAlpha, 0, 0, 0};
        for (size_t i = 0; i < count; ++i)
            parameters.Items[i] = {view.Gizmo[i].Transform, view.Gizmo[i].Color};
        // Only the used items are written.
        const size_t bytes   = offsetof(Parameters, Items) + count * sizeof(Item);
        auto         uniform = TakeUniform(*device);
        uniform->Write(0, &parameters, bytes);

        auto desc           = renderer.ScenePipeline("Transform gizmo", Vertex, Fragment, false);
        desc.VertexBindings = {{0, sizeof(HandleVertex), false}};
        desc.Attributes     = {{0, 0, uint32_t(offsetof(HandleVertex, Position)), VertexFormat::Float3},
                               {1, 0, uint32_t(offsetof(HandleVertex, Normal)), VertexFormat::Float3}};
        desc.ColorFormats.clear();
        desc.Blends.clear();
        for (size_t i = 0; i < target.Colors.size(); ++i)
        {
            desc.ColorFormats.push_back(target.Colors[i].Target->GetDesc().PixelFormat);
            BlendState blend;
            blend.Enabled          = true;
            blend.SourceColor      = BlendFactor::SourceAlpha;
            blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
            // The image stays opaque: the handles' coverage never reaches the target's alpha.
            blend.SourceAlpha      = BlendFactor::Zero;
            blend.DestinationAlpha = BlendFactor::One;
            blend.WriteMask        = i == 0 ? 15 : 0;
            desc.Blends.push_back(blend);
        }
        desc.HasDepthStencil = bool(target.Depth.Target);
        if (desc.HasDepthStencil)
            desc.DepthStencilFormat = target.Depth.Target->GetDesc().PixelFormat;
        desc.Samples         = target.Colors[0].Target->GetDesc().Samples;
        desc.Rasterizer.Cull = CullMode::Back;

        BindGroupDesc group;
        group.Layout = desc.GroupLayouts[0];
        group.Entries.push_back({0, uniform, 0, sizeof(Parameters)});
        const auto pipeline = [&](bool lines, bool occluded) {
            auto d                   = desc;
            d.Primitive              = lines ? Topology::LineList : Topology::TriangleList;
            d.DepthStencil.DepthTest = d.HasDepthStencil;
            // The visible part writes depth so the handles hide each other's far sides.
            d.DepthStencil.DepthWrite   = d.HasDepthStencil && !occluded && !lines;
            d.DepthStencil.DepthCompare = occluded ? CompareOp::Greater : CompareOp::LessEqual;
            return renderer.CachedPipeline(d);
        };

        commands.BeginRendering(target);
        commands.SetViewport({0, 0, float(view.Viewport.x), float(view.Viewport.y)});
        commands.SetScissor({0, 0, view.Viewport.x, view.Viewport.y});
        commands.BindVertexBuffers({{0, m_Vertices, 0}});
        commands.BindIndexBuffer(m_Indices, IndexType::Uint32);
        const auto bindGroup = device->CreateBindGroup(group);
        for (bool occluded : {true, false})
        {
            if (occluded && !desc.HasDepthStencil)
                continue;
            int bound = -1;
            for (size_t i = 0; i < count; ++i)
            {
                const auto& draw  = view.Gizmo[i];
                const bool  lines = IsGizmoLineShape(draw.Shape);
                if (int(lines) != bound)
                {
                    commands.BindPipeline(pipeline(lines, occluded));
                    commands.BindGroup(bindGroup);
                    bound = int(lines);
                }
                const auto& range = m_Shapes[size_t(draw.Shape)];
                commands.DrawIndexed(range.Count, 1, range.First, 0, uint32_t(i + (occluded ? MaxDraws : 0)));
            }
        }
        commands.EndRendering();
    }

    void TransformGizmoFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (!context.Frame || !RunsAt(point) || context.View.Gizmo.empty())
            return;
        auto& frame = *context.Frame;
        context.Graph.AddPass("Transform gizmo", context.Target, [this, &frame](auto& pass) {
            Record(frame.Services, pass.Commands, frame.View, frame.SceneTarget(RenderHardware::LoadOp::Load));
        });
    }
} // namespace Kans
