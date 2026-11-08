#include "kspch.h"
#include "Temporal.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/SkinningSource.h"
#include "RenderFrame.h"

namespace Kans
{
    uint32_t JitterPhaseCount(uint32_t displayWidth, uint32_t renderWidth)
    {
        if (!displayWidth || !renderWidth)
            throw std::invalid_argument("Jitter requires nonzero widths");
        const double ratio = double(displayWidth) / renderWidth;
        const double count = std::ceil(8 * ratio * ratio);
        if (count > UINT32_MAX)
            throw std::overflow_error("Jitter phase count overflow");
        return std::max(1u, uint32_t(count));
    }
    glm::vec2 JitterOffset(uint64_t frameIndex, uint32_t phaseCount)
    {
        if (!phaseCount)
            throw std::invalid_argument("Jitter requires a phase count");
        const auto halton = [](uint64_t index, uint32_t base) {
            float value = 0, fraction = 1;
            while (index)
            {
                fraction /= base;
                value += fraction * (index % base);
                index /= base;
            }
            return value;
        };
        const uint64_t index = frameIndex % phaseCount + 1;
        return {halton(index, 2) - .5f, halton(index, 3) - .5f};
    }
    glm::mat4 JitterProjection(const glm::mat4& projection, glm::vec2 clipOffset)
    {
        glm::mat4 translation(1);
        translation[3][0] = clipOffset.x;
        translation[3][1] = clipOffset.y;
        return translation * projection;
    }
    void RenderFrame::PrepareTemporal()
    {
        TemporalActive = Samples == 1 && (View.TemporalJitter || View.Velocity ||
                                          View.DebugBuffer == RenderView::DebugBuffer::Velocity);
        VelocityActive = TemporalActive && (View.Velocity || View.DebugBuffer == RenderView::DebugBuffer::Velocity);
        if (!TemporalActive)
        {
            Output.InvalidateTemporalHistory();
            return;
        }
        auto&      state      = Output.GetTemporalState();
        const auto projection = View.Scene.sceneCamera.camera.GetProjectionMatrix();
        const auto current =
            DeviceProjection(projection * View.Scene.sceneCamera.viewMatrix, GetDevice()->GetCoordinateConventions());
        state.Reset       = !state.Valid || View.CameraCut || state.Projection != projection || state.World != &World;
        state.Previous    = state.Reset ? current : state.Current;
        state.Current     = current;
        state.Projection  = projection;
        state.Valid       = true;
        state.World       = &World;
        state.Frame       = Output.GetFrameNumber();
        const auto jitter = View.TemporalJitter ?
                                JitterOffset(state.Frame - 1, JitterPhaseCount(Output.GetSize().x, View.Viewport.x)) :
                                glm::vec2(0);
        Output.SetJitter(jitter);
        state.Jitter = glm::vec4(jitter, 2.f * jitter / glm::vec2(View.Viewport));
        for (auto it = state.Objects.begin(); it != state.Objects.end();)
            if (state.Frame - it->second.LastSeen >= 4)
                it = state.Objects.erase(it);
            else
                ++it;
    }
    void RenderFrame::ApplyTemporalUniforms(EngineFrameUniforms& frame) const
    {
        if (!TemporalActive)
            return;
        auto& state                          = Output.GetTemporalState();
        frame.ViewProjectionNoJitter         = state.Current;
        frame.PreviousViewProjectionNoJitter = state.Previous;
        frame.Jitter                         = state.Jitter;
        if (View.TemporalJitter)
        {
            frame.ViewProjection = JitterProjection(frame.ViewProjection, glm::vec2(state.Jitter.z, state.Jitter.w));
            frame.Projection     = JitterProjection(frame.Projection, glm::vec2(state.Jitter.z, state.Jitter.w));
        }
    }
    glm::mat4 RenderFrame::SceneProjection() const
    {
        auto projection = DeviceProjection(View.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                           GetDevice()->GetCoordinateConventions());
        if (TemporalActive && View.TemporalJitter)
            projection = JitterProjection(projection, 2.f * Output.GetJitter() / glm::vec2(View.Viewport));
        return projection;
    }
    Camera RenderFrame::SceneCamera() const
    {
        auto camera = View.Scene.sceneCamera.camera;
        if (TemporalActive && View.TemporalJitter)
        {
            // SpriteBatch/skybox apply the device conversion themselves. Conjugate its Y sign here.
            auto offset = 2.f * Output.GetJitter() / glm::vec2(View.Viewport);
            if (GetDevice()->GetCoordinateConventions().ClipY == RenderHardware::ClipYDirection::Down)
                offset.y = -offset.y;
            camera.SetProjectionMatirx(JitterProjection(camera.GetProjectionMatrix(), offset));
        }
        return camera;
    }
    namespace
    {
        using namespace RenderHardware;
        const char* FullscreenVertex = R"(#version 450
void main() { vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(p*2-1,0,1); }
)";
        struct CameraData
        {
            glm::mat4 InverseCurrent, Previous;
            glm::vec4 CameraPosition, JitterSize;
        };
        const char* CameraFragment = R"(#version 450
layout(set=0,binding=0,std140) uniform CameraData {
    mat4 inverseCurrent; mat4 previous; vec4 cameraPosition; vec4 jitterSize;
} u;
layout(set=0,binding=1) uniform sampler2D sceneDepth;
layout(location=0) out vec2 velocity;
void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    vec2 currentNDC=gl_FragCoord.xy/u.jitterSize.zw*2-1-u.jitterSize.xy;
    float depth=texelFetch(sceneDepth,pixel,0).r;
    vec4 world=u.inverseCurrent*vec4(currentNDC,depth,1);
    // A point at infinity removes camera translation, retaining rotation for sky pixels.
    if(depth>=1.0) world=vec4(world.xyz-u.cameraPosition.xyz*world.w,0);
    vec4 old=u.previous*world;
    velocity=abs(old.w)>1e-8 ? (old.xy/old.w-currentNDC)*.5 : vec2(0);
}
)";
        std::string ObjectVertex(bool skinned)
        {
            std::string source = R"(#version 450
#include <Kans3D/Engine.glsl>
#include <Kans3D/VertexInput.glsl>
layout(set=0,binding=15,std140) uniform PreviousObject { mat4 transform; vec4 deformed; } previousObject;
layout(location=0) out vec4 currentClip;
layout(location=1) out vec4 previousClip;
layout(location=2) out vec2 uv;
layout(location=3) out vec4 color;
invariant gl_Position;
)";
            if (skinned)
            {
                source += R"(
layout(location=8) in vec3 restPosition;
layout(location=5) in uvec4 kans_Joints;
layout(location=6) in vec4 kans_Weights;
layout(set=2,binding=0,std430) readonly buffer Palette { mat4 kans_Palette[]; };
layout(set=2,binding=1,std430) readonly buffer Deltas { uint kans_Deltas[]; };
layout(set=2,binding=2,std430) readonly buffer PreviousPalette { mat4 kans_PreviousPalette[]; };
)";
                source += SkinningMorphVectorGLSL;
                source += R"(
vec3 currentPosition() { vec3 p=a_Position;
)";
                source += SkinningEvaluationGLSL();
                source += "return kans_DeformedPosition; }\nvec3 previousPosition() { vec3 p=a_Position;\n";
                auto previous = SkinningEvaluationGLSL();
                for (size_t at = 0; (at = previous.find("kans_Palette", at)) != std::string::npos;)
                {
                    previous.replace(at, 12, "kans_PreviousPalette");
                    at += 20;
                }
                source += previous;
                source += "return kans_DeformedPosition; }\n";
            }
            source += "void main() { vec3 position=" +
                      std::string(skinned ? "(previousObject.deformed.x!=0 ? currentPosition() : restPosition)" :
                                            "a_Position") +
                      ";\n";
            source += R"(
    vec3 world=vec3(u_Object.Transform*vec4(position,1));
    gl_Position=u_Frame.ViewProjection*vec4(world,1);
    currentClip=u_Frame.ViewProjectionNoJitter*vec4(world,1);
)";
            source += "    previousClip=u_Frame.PreviousViewProjectionNoJitter*previousObject.transform*vec4(" +
                      std::string(skinned ? "(previousObject.deformed.y!=0 ? previousPosition() : restPosition)" :
                                            "a_Position") +
                      ",1);\n";
            source += "uv=a_UV; color=a_Color; }\n";
            return source;
        }
        std::string ObjectFragment(bool standard)
        {
            std::string source = R"(#version 450
#include <Kans3D/Alpha.glsl>
layout(location=0) in vec4 currentClip;
layout(location=1) in vec4 previousClip;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 color;
layout(location=0) out vec2 velocity;
)";
            if (standard)
                source += R"(
layout(set=1,binding=0,std140) uniform StandardParameters {
    vec4 BaseColor; float Metallic; float Roughness;
    vec3 EmissiveColor; float EmissiveIntensity; float NormalStrength; float OcclusionStrength;
    vec2 UVTiling; vec2 UVOffset; float VertexColorStrength;
} p;
layout(set=1,binding=1) uniform sampler2D BaseColorMap;
)";
            source += "void main() {\n";
            if (standard)
                source += "KansAlphaClip(p.BaseColor.a*mix(1.0,color.a,p.VertexColorStrength)*texture(BaseColorMap,uv*"
                          "p.UVTiling+p.UVOffset).a);\n";
            source += "velocity=(previousClip.xy/previousClip.w-currentClip.xy/currentClip.w)*.5; }\n";
            return source;
        }
        void PrepareResources(TemporalState& state, Device& device)
        {
            if (state.FullscreenLayout)
                return;
            state.FullscreenLayout = device.CreateBindGroupLayout(
                {0, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
            state.ObjectLayout        = device.CreateBindGroupLayout({0,
                                                                      {{0, BindingType::UniformBuffer},
                                                                       {1, BindingType::UniformBuffer},
                                                                       {2, BindingType::CombinedTextureSampler},
                                                                       {3, BindingType::CombinedTextureSampler},
                                                                       {4, BindingType::CombinedTextureSampler},
                                                                       {15, BindingType::UniformBuffer}}});
            state.SkinLayout          = device.CreateBindGroupLayout({2,
                                                                      {{0, BindingType::ReadOnlyStorageBuffer},
                                                                       {1, BindingType::ReadOnlyStorageBuffer},
                                                                       {2, BindingType::ReadOnlyStorageBuffer}}});
            state.EmptyMaterialLayout = device.CreateBindGroupLayout({1, {}});
            SamplerDesc sampler;
            sampler.Min = sampler.Mag = sampler.Mip = Filter::Nearest;
            sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
            sampler.MaxLod                    = 0;
            state.PointSampler                = device.CreateSampler(sampler);
        }
        void Order(RenderGraph& graph, GraphPass pass)
        {
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        }
        void Fullscreen(RenderFrame&                           frame,
                        RenderGraph&                           graph,
                        const char*                            name,
                        const char*                            fragment,
                        const void*                            data,
                        size_t                                 bytes,
                        const Handle<RenderHardware::Texture>& input,
                        const Handle<RenderHardware::Texture>& output,
                        std::vector<GraphUse>                  uses)
        {
            auto  device = frame.GetDevice();
            auto& state  = frame.Output.GetTemporalState();
            PrepareResources(state, *device);
            auto buffer = device->CreateBuffer({bytes, BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, data, bytes);
            PipelineDesc pipeline;
            pipeline.Program         = frame.Services.GetInternalProgram(name, FullscreenVertex, fragment);
            pipeline.GroupLayouts    = {state.FullscreenLayout};
            pipeline.ColorFormats    = {output->GetDesc().PixelFormat};
            pipeline.Rasterizer.Cull = CullMode::None;
            pipeline.Blends.resize(1);
            auto gpuPipeline = frame.Services.CachedPipeline(pipeline);
            auto group       = device->CreateBindGroup(
                {state.FullscreenLayout, {{0, buffer, 0, bytes}, {1, {}, 0, 0, input, state.PointSampler}}});
            Order(graph, graph.AddPass(name, std::move(uses), [gpuPipeline, group, output](auto& c) {
                RenderingDesc target;
                target.Colors.push_back({output, {}, LoadOp::Discard});
                c.Commands.BeginRendering(target);
                c.Commands.SetViewport({0, 0, float(output->GetDesc().Width), float(output->GetDesc().Height)});
                c.Commands.SetScissor({0, 0, output->GetDesc().Width, output->GetDesc().Height});
                c.Commands.BindPipeline(gpuPipeline);
                c.Commands.BindGroup(group);
                c.Commands.Draw(3);
                c.Commands.EndRendering();
            }));
        }
        bool SamePalette(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b)
        {
            return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
        }
    } // namespace
    void RenderFrame::BuildVelocity(RenderGraph& graph)
    {
        using namespace RenderHardware;
        Output.EnsureSampledDepth();
        Output.EnsureVelocity();
        auto& state  = Output.GetTemporalState();
        auto  device = GetDevice();
        PrepareResources(state, *device);
        const auto velocity = graph.ImportTexture("SceneVelocity", Output.GetVelocity());
        const auto depth    = graph.ImportTexture("SceneDepthStencil", Output.GetDepth());
        graph.ExportTexture(velocity);
        CameraData camera {glm::inverse(state.Current),
                           state.Previous,
                           glm::vec4(View.Scene.sceneCamera.Position, 1),
                           glm::vec4(state.Jitter.z, state.Jitter.w, View.Viewport.x, View.Viewport.y)};
        // Derive the actual camera position, including callers that only supply a view matrix.
        camera.CameraPosition = glm::inverse(View.Scene.sceneCamera.viewMatrix)[3];
        Fullscreen(*this,
                   graph,
                   "Camera velocity",
                   CameraFragment,
                   &camera,
                   sizeof(camera),
                   Output.GetDepth(),
                   Output.GetVelocity(),
                   {{depth, GraphAccess::SampledRead}, {velocity, GraphAccess::ColorWrite}});
        auto          commands = device->CreateCommandList();
        RenderingDesc target;
        target.Colors.push_back({Output.GetVelocity(), {}, LoadOp::Load});
        target.Depth.Target  = Output.GetDepth();
        target.Depth.Load    = LoadOp::Load;
        bool       rendering = false;
        const auto begin     = [&] {
            if (rendering)
                return;
            commands->BeginRendering(target);
            commands->SetViewport({0, 0, float(View.Viewport.x), float(View.Viewport.y)});
            commands->SetScissor({0, 0, View.Viewport.x, View.Viewport.y});
            rendering = true;
        };
        const auto alignment  = device->GetBufferBindingAlignment(BindingType::UniformBuffer);
        const auto align      = [alignment](size_t n) { return (n + alignment - 1) / alignment * alignment; };
        const auto frameBytes = align(sizeof(EngineFrameUniforms)), objectBytes = align(sizeof(EngineObjectUniforms));
        struct PreviousObject
        {
            glm::mat4 Transform;
            glm::vec4 Deformed;
        };
        const auto           stride = frameBytes + objectBytes + align(sizeof(PreviousObject));
        std::vector<uint8_t> packed(stride * View.MaterialOrder.size());
        auto   uniforms = packed.empty() ?
                              Handle<RenderHardware::Buffer>() :
                              device->CreateBuffer({packed.size(), BufferUsage::Uniform, MemoryUsage::Upload});
        size_t range    = 0;
        std::vector<GraphUse> uses {{velocity, GraphAccess::ColorReadWrite}, {depth, GraphAccess::DepthReadWrite}};
        EngineFrameUniforms   frame;
        frame.ViewProjection = state.Current;
        frame.Projection     = DeviceProjection(state.Projection, device->GetCoordinateConventions());
        ApplyTemporalUniforms(frame);
        for (const auto& item : View.MaterialOrder)
        {
            if (item.Text || item.UI || item.AlwaysOnTop || IsTransparentMaterial(item.Render.AlphaMode))
                continue;
            const auto& draw     = View.Draws[item.DrawIndex];
            const auto* mesh     = GetMesh(draw.Mesh);
            const auto* bindings = GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit);
            const auto* shader   = bindings ? GetShader(draw.Material, bindings->Shader) : nullptr;
            if (!draw.Ready || !mesh || draw.Section >= mesh->Sections.size() || !shader ||
                shader->Domain != ShaderDomain::Surface || !VelocitySurfaceDraws.count(item.DrawIndex))
                continue;
            const bool                   skinned  = bool(mesh->Skin);
            const bool                   deformed = skinned && bool(Assets()->GetSkinningPalette(draw.SkinPose, mesh));
            const std::vector<glm::mat4> palette =
                skinned ? Assets()->GetSkinningPaletteData(draw.SkinPose, mesh) : std::vector<glm::mat4>();
            const TemporalState::Key key {
                uint64_t(draw.EntityID), uint64_t(draw.InstanceRoot), draw.Mesh, draw.Section};
            auto&       history  = state.Objects[key];
            const auto  identity = World.TemporalIdentity(draw.EntityID, draw.InstanceRoot);
            const bool  reset    = state.Reset || draw.EntityID == UUID(0) || history.LastSeen + 1 != state.Frame ||
                                   history.Identity != identity || history.MeshVertices.lock() != mesh->Vertices;
            const auto  previousTransform = reset ? draw.Transform : history.Transform;
            const auto& previousPalette   = reset ? palette : history.Palette;
            const bool  previousDeformed  = reset ? deformed : history.Deformed;
            const bool  moving = previousTransform != draw.Transform || !SamePalette(previousPalette, palette) ||
                                 previousDeformed != deformed;
            const auto  upload = [&](Handle<RenderHardware::Buffer>& buffer, const std::vector<glm::mat4>& data) {
                if (rendering)
                {
                    commands->EndRendering();
                    rendering = false;
                }
                const auto bytes    = data.size() * sizeof(glm::mat4);
                const auto joints   = draw.SkinPose && !draw.SkinPose->Palette.empty() ? draw.SkinPose->Palette.size() :
                                                                                         mesh->RestPaletteData.size();
                const auto capacity = std::max(
                    bytes,
                    (joints + (mesh->MorphTargets.empty() ? 0 : mesh->MorphTargets.size() + 1)) * sizeof(glm::mat4));
                if (!buffer || buffer->GetDesc().Bytes < capacity)
                    buffer = device->CreateBuffer(
                        {capacity, BufferUsage::Storage | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
                // Queue-ordered transfer writes are safe with asynchronous frames too; no host overwrite.
                for (size_t offset = 0; offset < bytes; offset += 65536)
                    commands->UpdateBuffer(buffer,
                                           offset,
                                           reinterpret_cast<const uint8_t*>(data.data()) + offset,
                                           std::min(size_t(65536), bytes - offset));
                uses.push_back({graph.ImportBuffer("Velocity palette", buffer), GraphAccess::StorageReadWrite});
            };
            // Preserve a GPU copy from the first observed pose, even while the object is static.
            if (skinned && draw.EntityID != UUID(0) && (moving || reset))
                upload(history.PreviousPalette, previousPalette);
            if (moving)
            {
                const bool           standard = IsStandardShader(bindings->Shader);
                const auto           offset   = range * stride;
                EngineObjectUniforms object;
                object.Transform      = draw.Transform;
                object.MaterialRender = {
                    float(item.Render.AlphaMode), item.Render.AlphaCutoff, float(item.Render.DoubleSided), 0};
                std::memcpy(packed.data() + offset, &frame, sizeof(frame));
                std::memcpy(packed.data() + offset + frameBytes, &object, sizeof(object));
                const PreviousObject previous {previousTransform, {float(deformed), float(previousDeformed), 0, 0}};
                std::memcpy(packed.data() + offset + frameBytes + objectBytes, &previous, sizeof(previous));
                Handle<BindGroup> skinGroup;
                if (skinned)
                {
                    if (rendering)
                    {
                        commands->EndRendering();
                        rendering = false;
                    }
                    // This frame's cache palette is what Main reads; bind it rather than uploading a duplicate.
                    // Only the rest pose (no cache buffer) needs its own upload.
                    auto current = Assets()->GetSkinningPalette(draw.SkinPose, mesh);
                    if (!current)
                    {
                        upload(history.CurrentPalette, palette);
                        current = history.CurrentPalette;
                    }
                    else
                        uses.push_back({graph.ImportBuffer("Velocity current palette", current), GraphAccess::StorageRead});
                    auto deltas = mesh->MorphDeltas ? mesh->MorphDeltas : current;
                    skinGroup   = device->CreateBindGroup(
                        {state.SkinLayout,
                         {{0, current, 0, palette.size() * sizeof(glm::mat4)},
                          {1, deltas, 0, deltas->GetDesc().Bytes},
                          {2, history.PreviousPalette, 0, previousPalette.size() * sizeof(glm::mat4)}}});
                }
                begin();
                PipelineDesc pipeline;
                const auto   vertex = ObjectVertex(skinned), fragment = ObjectFragment(standard);
                pipeline.Program = Services.GetInternalProgram(
                    skinned ? (standard ? "Skinned Standard velocity" : "Skinned velocity") :
                              (standard ? "Standard velocity" : "Static velocity"),
                    vertex.c_str(),
                    fragment.c_str());
                pipeline.GroupLayouts = {state.ObjectLayout,
                                         standard ? shader->MaterialLayout : state.EmptyMaterialLayout};
                if (skinned)
                    pipeline.GroupLayouts.push_back(state.SkinLayout);
                pipeline.VertexBindings = mesh->VertexBindings;
                pipeline.Attributes     = mesh->Attributes;
                if (skinned)
                {
                    pipeline.VertexBindings.push_back({2, 64, false});
                    pipeline.Attributes.push_back({8, 2, 0, VertexFormat::Float3});
                }
                if (!skinned)
                {
                    pipeline.VertexBindings.resize(1);
                    pipeline.Attributes.resize(5);
                }
                pipeline.ColorFormats              = {Format::RG16Float};
                pipeline.HasDepthStencil           = true;
                pipeline.DepthStencilFormat        = Output.GetDepth()->GetDesc().PixelFormat;
                pipeline.DepthStencil.DepthTest    = true;
                pipeline.DepthStencil.DepthWrite   = false;
                pipeline.DepthStencil.DepthCompare = CompareOp::LessEqual;
                pipeline.Rasterizer.Cull           = item.Render.DoubleSided ? CullMode::None : CullMode::Back;
                pipeline.Rasterizer.Polygon =
                    draw.ViewMode == SceneViewMode::Wireframe ? PolygonMode::Line : PolygonMode::Fill;
                // One D24 quantization step, no slope term: cannot reach meaningfully occluded geometry.
                pipeline.Rasterizer.DepthBiasConstant = -1;
                pipeline.Blends.resize(1);
                commands->BindPipeline(Services.CachedPipeline(pipeline));
                const auto environment = GetMaterialEnvironment();
                commands->BindGroup(
                    device->CreateBindGroup({state.ObjectLayout,
                                             {{0, uniforms, offset, sizeof(frame)},
                                              {1, uniforms, offset + frameBytes, sizeof(object)},
                                              {2, {}, 0, 0, environment[0], Services.GetMaterialEnvironmentSampler()},
                                              {3, {}, 0, 0, environment[1], Services.GetMaterialEnvironmentSampler()},
                                              {4, {}, 0, 0, environment[2], Services.GetMaterialEnvironmentSampler()},
                                              {15, uniforms, offset + frameBytes + objectBytes, sizeof(previous)}}}));
                for (const auto& image : environment)
                {
                    const auto id = graph.ImportTexture("Velocity engine input", image);
                    if (std::none_of(uses.begin(), uses.end(), [&](auto use) { return use.Texture == id; }))
                        uses.push_back({id, GraphAccess::SampledRead});
                }
                if (standard)
                {
                    const auto& surface = VelocitySurfaceDraws.at(item.DrawIndex);
                    commands->BindGroup(surface.Material);
                    for (const auto& image : surface.Images)
                    {
                        const auto id = graph.ImportTexture("Velocity alpha input", image);
                        if (std::none_of(uses.begin(), uses.end(), [&](auto use) { return use.Texture == id; }))
                            uses.push_back({id, GraphAccess::SampledRead});
                    }
                }
                else
                    commands->BindGroup(device->CreateBindGroup({state.EmptyMaterialLayout, {}}));
                if (skinned)
                {
                    commands->BindGroup(skinGroup);
                    commands->BindVertexBuffers({{0, mesh->Vertices, 0},
                                                 {1, mesh->Skin, 0},
                                                 {2, mesh->BindVertices ? mesh->BindVertices : mesh->Vertices, 0}});
                }
                else
                    commands->BindVertexBuffers({{0, mesh->BindVertices ? mesh->BindVertices : mesh->Vertices, 0}});
                commands->BindIndexBuffer(mesh->Indices, IndexType::Uint32);
                const auto& section = mesh->Sections[draw.Section];
                commands->DrawIndexed(section.IndexCount, 1, section.FirstIndex);
                ++Stats.VelocityDrawCalls;
                ++range;
            }
            // Retain bytes, never a cache palette handle. Next frame uploads these bytes to a distinct buffer.
            history.Deformed     = deformed;
            history.Transform    = draw.Transform;
            history.Palette      = palette;
            history.LastSeen     = state.Frame;
            history.Identity     = identity;
            history.MeshVertices = mesh->Vertices;
        }
        if (range)
            uniforms->Write(0, packed.data(), range * stride);
        if (rendering)
            commands->EndRendering();
        Order(graph, graph.AddRecordedPass("Object velocity", std::move(uses), commands));
    }
    void RenderFrame::BuildDebugBuffer(RenderGraph& graph)
    {
        using namespace RenderHardware;
        if (Samples > 1 || View.DebugBuffer == RenderView::DebugBuffer::None)
            return;
        const bool velocity = View.DebugBuffer == RenderView::DebugBuffer::Velocity;
        auto       input    = velocity ? Output.GetVelocity() : Output.GetNormalRoughness();
        if (!input)
            return;
        const auto  src = graph.ImportTexture(velocity ? "SceneVelocity" : "SceneNormalRoughness", input);
        const auto  dst = graph.ImportTexture("SceneColor0", Output.GetColors()[0]);
        glm::vec4   size(View.Viewport.x, View.Viewport.y, 0, 0);
        const char* fragment = R"(#version 450
layout(set=0,binding=0,std140) uniform Settings { vec4 size; } u;
layout(set=0,binding=1) uniform sampler2D image;
layout(location=0) out vec4 color;
void main() {
    vec2 v=texelFetch(image,ivec2(gl_FragCoord.xy),0).rg;
    float hue=dot(v,v)>0 ? atan(v.y,v.x)/6.28318530718 : 0;
    vec3 rgb=clamp(abs(fract(hue+vec3(0,2.0/3.0,1.0/3.0))*6-3)-1,0,1);
    color=vec4(rgb*clamp(length(v)*32,0,1),1);
}
)";
        const char* normals  = R"(#version 450
layout(set=0,binding=0,std140) uniform Settings { vec4 size; } u;
layout(set=0,binding=1) uniform sampler2D image;
layout(location=0) out vec4 color;
void main() {
    vec4 packed=texelFetch(image,ivec2(gl_FragCoord.xy),0);
    vec2 f=packed.rg*2-1;
    vec3 n=vec3(f,1-abs(f.x)-abs(f.y));
    float t=clamp(-n.z,0,1);
    n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));
    vec3 rgb=packed.a==1 ? normalize(n)*.5+.5 : vec3(0);
    // Upper-right quarter displays the same pixels' perceptual roughness in grayscale.
    if(gl_FragCoord.x>u.size.x*.75 && gl_FragCoord.y<u.size.y*.25) rgb=vec3(packed.b);
    color=vec4(rgb,1);
}
)";
        Fullscreen(*this,
                   graph,
                   velocity ? "Debug velocity" : "Debug normal roughness",
                   velocity ? fragment : normals,
                   &size,
                   sizeof(size),
                   input,
                   Output.GetColors()[0],
                   {{src, GraphAccess::SampledRead}, {dst, GraphAccess::ColorWrite}});
    }
} // namespace Kans
