#include "kspch.h"
#include "TransformGizmoFeature.h"
#include <glm/gtc/constants.hpp>
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"

namespace Kans
{
    namespace
    {
        // Instance i < MaxDraws draws item i; +MaxDraws is its occluded copy (faded) and
        // +2 MaxDraws its dark silhouette (widened by Options.w pixels). Params.x is the kind:
        // 0 a shaded solid, 1 flat, 2 an arc band, 3 a sector, 4 ticks.
        constexpr const char* Vertex   = R"glsl(#version 450
layout(location=0) in vec3 Position;
layout(location=1) in vec3 Normal;
struct Item { mat4 Model; vec4 Color; vec4 Params; };
layout(set=0,binding=0,std140) uniform Parameters { mat4 ViewProjection; mat4 View; vec4 Options; Item Items[128]; } p;
layout(location=0) out vec4 HandleColor;
vec4 ClipOf(mat4 model, vec3 local) { return p.ViewProjection * (model * vec4(local, 1)); }
// The unit screen direction from `a` toward `b` (pixels; zero when they coincide).
vec2 ScreenDirection(vec4 a, vec4 b) {
    vec2 d = (b.xy / b.w - a.xy / a.w) * p.Options.yz;
    return dot(d, d) > 1e-12 ? normalize(d) : vec2(0);
}
vec4 Shift(vec4 clip, vec2 direction, float pixels) {
    clip.xy += direction * (pixels * 2.0) / p.Options.yz * clip.w;
    return clip;
}
vec2 Across(vec2 d) { return vec2(-d.y, d.x); }
void main() {
    Item  item     = p.Items[gl_InstanceIndex % 128];
    bool  occluded = (gl_InstanceIndex / 128) % 2 == 1;
    bool  outline  = gl_InstanceIndex >= 256;
    int   kind     = int(item.Params.x + 0.5);
    float extra    = outline ? p.Options.w : 0.0;
    float shade    = 1.0;
    vec4  clip;
    if (kind == 2) {
        float a = item.Params.y + Position.x * item.Params.z;
        vec3  c = vec3(cos(a), sin(a), 0), t = vec3(-sin(a), cos(a), 0);
        clip    = ClipOf(item.Model, c);
        vec2 d  = ScreenDirection(clip, ClipOf(item.Model, c + t * 0.01));
        // Where the circle is seen end-on, across the radius instead.
        if (d == vec2(0))
            d = Across(ScreenDirection(clip, ClipOf(item.Model, c * 1.01)));
        clip = Shift(clip, Across(d), Position.y * (item.Params.w + extra));
    } else if (kind == 3) {
        float a = item.Params.y + Position.x * item.Params.z;
        clip    = ClipOf(item.Model, Position.y * vec3(cos(a), sin(a), 0));
    } else if (kind == 4) {
        if (Position.x >= item.Params.w) {
            gl_Position = vec4(2, 2, 2, 1);
            HandleColor = vec4(0);
            return;
        }
        float a = item.Params.y + Position.x * item.Params.z;
        vec3  r = vec3(cos(a), sin(a), 0);
        vec3  c = r * mix(1.03 - extra * 0.01, 1.15 + extra * 0.01, Position.y);
        clip    = ClipOf(item.Model, c);
        vec2 d  = ScreenDirection(clip, ClipOf(item.Model, c + r * 0.01));
        clip    = Shift(clip, Across(d), Position.z * (0.75 + extra));
    } else {
        clip = ClipOf(item.Model, Position);
        if (dot(Normal, Normal) > 0.25) {
            if (outline)
                clip = Shift(clip, ScreenDirection(clip, ClipOf(item.Model, Position + Normal * 0.01)), extra);
            else if (kind == 0) {
                vec3 n = normalize(mat3(p.View) * (transpose(inverse(mat3(item.Model))) * Normal));
                shade  = 0.62 + 0.38 * abs(n.z);
            }
        }
    }
    gl_Position = clip;
    vec3 rgb    = outline ? item.Color.rgb * 0.2 : item.Color.rgb * shade;
    HandleColor = vec4(rgb, item.Color.a * (occluded ? p.Options.x : 1.0));
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
            glm::vec4 Color, Params;
        };
        struct Parameters
        {
            glm::mat4 ViewProjection, View;
            glm::vec4 Options;
            Item      Items[TransformGizmoFeature::MaxDraws];
        };
        static_assert(TransformGizmoFeature::MaxDraws == 128, "The shader's item array has 128 entries.");
        static_assert(sizeof(Parameters) <= 16384, "The block fits the smallest uniform range Vulkan allows.");

        // The shader's kind for a draw (Params.x).
        float KindOf(const GizmoDraw& draw)
        {
            switch (draw.Shape)
            {
                case GizmoShape::Arc:
                    return 2;
                case GizmoShape::Sector:
                    return 3;
                case GizmoShape::Ticks:
                    return 4;
                default:
                    return draw.Flat ? 1.0f : 0.0f;
            }
        }
        // Solids and bands get a dark silhouette; flat fills, lines and sectors do not.
        bool HasOutline(const GizmoDraw& draw)
        {
            if (IsGizmoLineShape(draw.Shape) || draw.Shape == GizmoShape::Sector)
                return false;
            if (draw.Shape == GizmoShape::Arc || draw.Shape == GizmoShape::Ticks)
                return true;
            return !draw.Flat && draw.Shape != GizmoShape::Quad && draw.Shape != GizmoShape::Disc;
        }

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
        // static: capture-less lambdas below use these (MSVC requires captures for automatic constexpr locals).
        static constexpr float pi = glm::pi<float>(), tau = glm::two_pi<float>();
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
        static constexpr float minor = 0.02f;
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

        // The parametric shapes: the shader places their vertices from Params. Both windings,
        // since they are seen from either side.
        const auto both = [&](uint32_t a, uint32_t b, uint32_t c) {
            m.Triangle(a, b, c);
            m.Triangle(a, c, b);
        };
        begin(GizmoShape::Arc);
        {
            // x: the fraction along the arc, y: the side (-1 or 1).
            constexpr uint32_t segments = 128;
            const uint32_t     first    = uint32_t(m.Vertices.size());
            for (uint32_t i = 0; i <= segments; ++i)
            {
                m.Add({float(i) / segments, -1, 0}, glm::vec3(0));
                m.Add({float(i) / segments, 1, 0}, glm::vec3(0));
            }
            for (uint32_t i = 0; i < segments; ++i)
            {
                const uint32_t a = first + 2 * i;
                both(a, a + 2, a + 3);
                both(a, a + 3, a + 1);
            }
        }
        end(GizmoShape::Arc);

        begin(GizmoShape::Sector);
        {
            // x: the fraction along the arc, y: 0 at the centre and 1 on the rim.
            constexpr uint32_t segments = 128;
            const uint32_t     centre   = m.Add({0, 0, 0}, glm::vec3(0));
            const uint32_t     first    = uint32_t(m.Vertices.size());
            for (uint32_t i = 0; i <= segments; ++i)
                m.Add({float(i) / segments, 1, 0}, glm::vec3(0));
            for (uint32_t i = 0; i < segments; ++i)
                both(centre, first + i, first + i + 1);
        }
        end(GizmoShape::Sector);

        begin(GizmoShape::Ticks);
        // x: the mark's index, y: 0 inside and 1 outside, z: the side.
        for (uint32_t k = 0; k < GizmoMaxTicks; ++k)
        {
            const uint32_t a = m.Add({float(k), 0, -1}, glm::vec3(0)), b = m.Add({float(k), 0, 1}, glm::vec3(0)),
                           c = m.Add({float(k), 1, 1}, glm::vec3(0)), d = m.Add({float(k), 1, -1}, glm::vec3(0));
            both(a, b, c);
            both(a, c, d);
        }
        end(GizmoShape::Ticks);

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
        parameters.Options = {OccludedAlpha, float(std::max(view.Viewport.x, 1u)), float(std::max(view.Viewport.y, 1u)),
                              OutlinePixels};
        for (size_t i = 0; i < count; ++i)
        {
            const auto& draw    = view.Gizmo[i];
            parameters.Items[i] = {draw.Transform, draw.Color,
                                   {KindOf(draw), draw.Params.y, draw.Params.z, draw.Params.w}};
        }
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
        const auto pipeline = [&](bool lines, bool occluded, bool outline) {
            auto d                   = desc;
            d.Primitive              = lines ? Topology::LineList : Topology::TriangleList;
            d.DepthStencil.DepthTest = d.HasDepthStencil;
            // The visible part writes depth so the handles hide each other's far sides; the
            // silhouettes do not, so the handle drawn over one is never refused.
            d.DepthStencil.DepthWrite   = d.HasDepthStencil && !occluded && !lines && !outline;
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
            int        bound = -1;
            const auto bind  = [&](bool lines, bool outline) {
                const int key = int(lines) | int(outline) << 1;
                if (key == bound)
                    return;
                commands.BindPipeline(pipeline(lines, occluded, outline));
                commands.BindGroup(bindGroup);
                bound = key;
            };
            for (size_t i = 0; i < count; ++i)
            {
                const auto&    draw     = view.Gizmo[i];
                const bool     lines    = IsGizmoLineShape(draw.Shape);
                const auto&    range    = m_Shapes[size_t(draw.Shape)];
                const uint32_t instance = uint32_t(i + (occluded ? MaxDraws : 0));
                if (HasOutline(draw))
                {
                    bind(false, true);
                    commands.DrawIndexed(range.Count, 1, range.First, 0, instance + 2 * MaxDraws);
                }
                bind(lines, false);
                commands.DrawIndexed(range.Count, 1, range.First, 0, instance);
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
