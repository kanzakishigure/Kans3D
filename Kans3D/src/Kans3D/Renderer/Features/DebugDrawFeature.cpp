#include "kspch.h"
#include "DebugDrawFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
namespace Kans
{
    namespace
    {
        constexpr const char* Vertex   = R"glsl(#version 450
layout(location=0) in vec3 Position;
layout(location=1) in vec4 Color;
layout(location=0) out vec4 LineColor;
layout(set=0,binding=0,std140) uniform Parameters { mat4 VP; } p;
void main() { gl_Position=p.VP*vec4(Position,1); LineColor=Color; }
)glsl";
        constexpr const char* Fragment = R"glsl(#version 450
layout(location=0) in vec4 LineColor;
layout(location=0) out vec4 Color;
void main() { Color=LineColor; }
)glsl";
        struct LineVertex
        {
            glm::vec3 Position;
            glm::vec4 Color;
        };
    } // namespace
    void DebugDrawFeature::Record(Renderer&                            renderer,
                                  RenderHardware::CommandList&         commands,
                                  const RenderView&                    view,
                                  const DebugDraw&                     draw,
                                  DebugDepth                           depth,
                                  const RenderHardware::RenderingDesc& target)
    {
        using namespace RenderHardware;
        std::vector<LineVertex> vertices;
        for (const auto& l : draw.GetLines())
            if (l.Depth == depth)
            {
                vertices.push_back({l.A, l.Color});
                vertices.push_back({l.B, l.Color});
            }
        if (vertices.empty() || target.Colors.empty())
            return;
        auto device         = renderer.GetDevice();
        auto desc           = renderer.ScenePipeline("DebugDraw lines", Vertex, Fragment, false);
        desc.Primitive      = Topology::LineList;
        desc.VertexBindings = {{0, sizeof(LineVertex), false}};
        desc.Attributes     = {{0, 0, uint32_t(offsetof(LineVertex, Position)), VertexFormat::Float3},
                               {1, 0, uint32_t(offsetof(LineVertex, Color)), VertexFormat::Float4}};
        desc.ColorFormats.clear();
        desc.Blends.clear();
        for (size_t i = 0; i < target.Colors.size(); ++i)
        {
            desc.ColorFormats.push_back(target.Colors[i].Target->GetDesc().PixelFormat);
            BlendState blend;
            blend.Enabled          = true;
            blend.SourceColor      = BlendFactor::SourceAlpha;
            blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
            blend.WriteMask        = i == 0 ? 15 : 0;
            desc.Blends.push_back(blend);
        }
        desc.HasDepthStencil = bool(target.Depth.Target);
        if (desc.HasDepthStencil)
            desc.DepthStencilFormat = target.Depth.Target->GetDesc().PixelFormat;
        desc.DepthStencil.DepthTest    = depth == DebugDepth::Tested && desc.HasDepthStencil;
        desc.DepthStencil.DepthWrite   = false;
        desc.DepthStencil.DepthCompare = CompareOp::LessEqual;
        desc.Samples                   = target.Colors[0].Target->GetDesc().Samples;
        const auto vp =
            DeviceProjection(view.Scene.sceneCamera.camera.GetProjectionMatrix() * view.Scene.sceneCamera.viewMatrix,
                             device->GetCoordinateConventions());
        auto uniform = device->CreateBuffer({sizeof(vp), BufferUsage::Uniform, MemoryUsage::Upload});
        uniform->Write(0, &vp, sizeof(vp));
        auto buffer =
            device->CreateBuffer({vertices.size() * sizeof(LineVertex), BufferUsage::Vertex, MemoryUsage::Upload});
        buffer->Write(0, vertices.data(), vertices.size() * sizeof(LineVertex));
        BindGroupDesc group;
        group.Layout = desc.GroupLayouts[0];
        group.Entries.push_back({0, uniform, 0, sizeof(vp)});
        commands.BeginRendering(target);
        commands.SetViewport({0, 0, float(view.Viewport.x), float(view.Viewport.y)});
        commands.SetScissor({0, 0, view.Viewport.x, view.Viewport.y});
        commands.BindPipeline(renderer.CachedPipeline(desc));
        commands.BindGroup(device->CreateBindGroup(group));
        commands.BindVertexBuffers({{0, buffer, 0}});
        commands.Draw(uint32_t(vertices.size()));
        commands.EndRendering();
    }
    void DebugDrawFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (!context.Frame || !RunsAt(point))
            return;
        auto&       frame = *context.Frame;
        const auto& lines = frame.World.Debug.GetLines();
        // Both variants follow tone mapping, retaining exact diagnostic colours.
        // The tested variant still reads scene depth; the overlay ignores it.
        for (auto depth : {DebugDepth::Tested, DebugDepth::Overlay})
        {
            if (std::none_of(lines.begin(), lines.end(), [&](const auto& l) { return l.Depth == depth; }))
                continue;
            context.Graph.AddPass(depth == DebugDepth::Tested ? "DebugDraw depth" : "DebugDraw overlay",
                                  context.Target,
                                  [&frame, depth](auto& pass) {
                                      Record(frame.Services,
                                             pass.Commands,
                                             frame.View,
                                             frame.World.Debug,
                                             depth,
                                             frame.SceneTarget(RenderHardware::LoadOp::Load));
                                  });
        }
    }
} // namespace Kans
