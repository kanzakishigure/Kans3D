#include "kspch.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Renderer/2D/TextRenderer.h"
#include "Kans3D/Renderer/2D/UIRenderer.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
#include "Kans3D/Renderer/Shading/SceneShaders.h"
#include "RenderFrame.h"
#include "RenderPipeline.h"
namespace Kans
{
    namespace
    {
        using namespace RenderHardware;
        struct SceneDrawData
        {
            glm::mat4 ViewProjection {1}, Transform {1}, DeviceClip {1};
            glm::vec4 ViewPos {0}, Settings {0};
        };
        template<class T>
        Handle<BindGroup> DrawGroup(Device&                         device,
                                    const Handle<BindGroupLayout>&  layout,
                                    const T&                        data,
                                    Handle<RenderHardware::Texture> texture = {},
                                    Handle<Sampler>                 sampler = {})
        {
            auto buffer = device.CreateBuffer({sizeof(T), BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, &data, sizeof(T));
            BindGroupDesc group;
            group.Layout = layout;
            group.Entries.push_back({0, buffer, 0, sizeof(T), {}, {}});
            if (texture)
                group.Entries.push_back({1, {}, 0, 0, std::move(texture), std::move(sampler)});
            return device.CreateBindGroup(group);
        }
        template<class F>
        void ForEachSection(const ModelInstance& instance, const GpuMesh& mesh, F&& action)
        {
            const size_t first = instance.AllSections ? 0 : instance.Section;
            const size_t end   = instance.AllSections ? mesh.Sections.size() : first + 1;
            for (size_t i = first; i < end && i < mesh.Sections.size(); ++i)
            {
                auto part        = instance;
                part.Section     = static_cast<uint32_t>(i);
                part.AllSections = false;
                action(part, mesh.Sections[i]);
            }
        }
    } // namespace

    bool RenderFrame::IncludesSection(UUID entity, UUID instanceRoot, const GpuMesh* mesh, uint32_t section) const
    {
        return View.Highlights.Isolate.empty() ||
               (mesh && section < mesh->Sections.size() &&
                World.IsIsolated(View.Highlights, entity, instanceRoot, mesh->Sections[section].MaterialSlot));
    }

    void RenderFrame::PrepareDrawLists()
    {
        if (!View.Highlights.Isolate.empty())
        {
            View.Draws.erase(std::remove_if(View.Draws.begin(),
                                            View.Draws.end(),
                                            [&](const auto& draw) {
                                                return !IncludesSection(
                                                    draw.EntityID, draw.InstanceRoot, GetMesh(draw.Mesh), draw.Section);
                                            }),
                             View.Draws.end());
            // Legacy geometry submissions may cover all sections: split before filtering.
            std::vector<ViewGeometryDraw> geometry;
            for (const auto& draw : View.Geometry)
                if (const auto* mesh = GetMesh(draw.Mesh))
                    for (uint32_t section = 0; section < mesh->Sections.size(); ++section)
                        if ((!draw.Section || *draw.Section == section) &&
                            IncludesSection(draw.EntityID, 0, mesh, section))
                        {
                            auto part    = draw;
                            part.Section = section;
                            geometry.push_back(std::move(part));
                        }
            View.Geometry = std::move(geometry);
            View.Sprites.clear();
            View.SpriteBatches.clear();
            View.ImmediateOrder.erase(
                std::remove_if(View.ImmediateOrder.begin(),
                               View.ImmediateOrder.end(),
                               [](const auto& item) { return item.Kind != ViewImmediateKind::Skybox; }),
                View.ImmediateOrder.end());
        }
        for (auto& draw : View.Draws)
        {
            const auto* mesh     = GetMesh(draw.Mesh);
            const auto* bindings = GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit);
            bool        post     = false;
            if (const auto assigned = View.MaterialResources.find(draw.Material);
                assigned != View.MaterialResources.end())
                post = assigned->second.Shader && assigned->second.Shader->Domain == ShaderDomain::PostProcess;
            else if (Assets() && draw.Material.IsValid())
            {
                // Domain checks must not upload custom surface shaders just for a debug view.
                auto material = Assets()->GetAssetManager().Request<MaterialDataAsset>(draw.Material);
                if (material.IsReady() && !IsStandardShader(material.Asset->GetData().Shader))
                {
                    auto shader =
                        Assets()->GetAssetManager().Request<ShaderDataAsset>(material.Asset->GetData().Shader);
                    post = shader.IsReady() && shader.Asset->GetData().Domain == ShaderDomain::PostProcess;
                }
            }
            if (post && Assets())
                Assets()->WarnMaterialDomain(draw.Material, false);
            draw.Ready = !post && mesh && draw.Section < mesh->Sections.size() && bindings;
            if (draw.Ready)
            {
                draw.LocalBounds = mesh->Sections[draw.Section].LocalBounds;
                draw.Render      = bindings->Render;
            }
        }
    }

    bool RenderFrame::HasMaterialPass(const RenderView& view, const std::string& tag)
    {
        if (!Assets())
            return false;
        for (const auto& item : view.MaterialOrder)
        {
            if (item.Text || item.UI)
                continue;
            const auto& draw = view.Draws[item.DrawIndex];
            if (!draw.Ready)
                continue;
            const auto* bindings = GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit);
            const auto* shader   = bindings ? GetShader(draw.Material, bindings->Shader) : nullptr;
            if (shader && std::any_of(shader->Passes.begin(), shader->Passes.end(), [&](const auto& pass) {
                    return pass.Tag == tag && bindings->PassEnabled(pass.Name);
                }))
                return true;
        }
        return false;
    }

    void RenderFrame::DrawShaderMaterials(
        RenderHardware::CommandList&                                                       commandList,
        const std::function<void(const RenderHardware::Handle<RenderHardware::Texture>&)>& sampled,
        const std::string&                                                                 tag,
        const RenderView*                                                                  suppliedView,
        RenderHardware::Handle<RenderHardware::CommandList>*                               finalPost,
        std::optional<bool>                                                                transparentOnly)
    {
        const auto& view = suppliedView ? *suppliedView : View;
        if (view.MaterialOrder.empty())
            return;
        const auto&         ready    = view.MaterialOrder;
        auto*               commands = &commandList;
        EngineFrameUniforms frame    = View.LightingUniforms.value_or(MakeLightingUniforms(View.Scene));
        const auto coordinates = Services.GetDevice()->GetCoordinateConventions();
        frame.View                      = View.Scene.sceneCamera.viewMatrix;
        frame.Projection                = View.Scene.sceneCamera.camera.GetProjectionMatrix();
        frame.ViewProjection            = DeviceProjection(frame.Projection * frame.View, coordinates);
        frame.Projection                = DeviceProjection(frame.Projection, coordinates);
        if (!suppliedView && tag.empty())
            ApplyTemporalUniforms(frame);
        frame.InverseView               = glm::inverse(frame.View);
        frame.InverseProjection         = glm::inverse(frame.Projection);
        frame.CameraPosition   = glm::vec4(View.Scene.sceneCamera.Position, 1.0f);
        frame.Time             = View.Time;
        const float rotation   = glm::radians(View.Environment.Rotation);
        frame.EnvironmentInfo  = {View.Environment.DiffuseIntensity,
                                  View.Environment.SpecularIntensity,
                                  std::cos(rotation),
                                  std::sin(rotation)};
        frame.LightingInfo.y   = View.GetPostProcessSettings().Exposure;
        using namespace RenderHardware;
        const size_t alignment   = Services.GetDevice()->GetBufferBindingAlignment(BindingType::UniformBuffer);
        const auto   align       = [alignment](size_t n) { return (n + alignment - 1) / alignment * alignment; };
        const size_t frameStride = align(sizeof(frame)), objectStride = align(sizeof(EngineObjectUniforms));
        // Each draw owns disjoint immutable ranges until command/frame retirement.
        if (ready.empty())
            return;
        auto frames =
            Services.GetDevice()->CreateBuffer({frameStride * ready.size(), BufferUsage::Uniform, MemoryUsage::Upload});
        auto objects = Services.GetDevice()->CreateBuffer(
            {objectStride * ready.size(), BufferUsage::Uniform, MemoryUsage::Upload});
        const bool writeTemporalMasks = TemporalMasks != uint32_t(-1) && tag.empty() && !suppliedView;
        const auto materialTarget     = [&](LoadOp load, bool hdr, bool invalidate = false) {
            auto target = SceneTarget(load, hdr);
            if (invalidate)
                target.Colors.push_back({Output.GetNormalRoughness(), {}, load});
            if (writeTemporalMasks)
                target.Colors.push_back({SceneGraph->Texture(TemporalMasks), {}, load});
            return target;
        };
        // TAA material recording is deferred past graph construction's display-space state changes.
        const bool deferToneMapping = DeferToneMapping || writeTemporalMasks;
        bool       rendering        = false,
                   hdr = writeTemporalMasks || ((deferToneMapping || HasBackgroundColor()) && PostProcessHDR);
        bool invalidatingNormals = false;
        auto resolve = [&](RenderHardware::CommandList& output) {
            Services.GetToneMapping().Record(*Services.GetDevice(),
                                             output,
                                             HDR(),
                                             Colors(),
                                             frame.LightingInfo.y,
                                             false,
                                             View.GetPostProcessSettings().ToneMapping);
        };
        auto beginTarget = [&](bool nextHDR, bool invalidate = false) {
            if (rendering && nextHDR == hdr && invalidatingNormals == invalidate)
                return;
            if (rendering)
                commands->EndRendering();
            if (hdr && !nextHDR)
                resolve(*commands);
            if (nextHDR && !hdr)
                Services.GetToneMapping().Record(*Services.GetDevice(),
                                                 *commands,
                                                 Colors()[0],
                                                 {HDR()},
                                                 frame.LightingInfo.y,
                                                 true,
                                                 View.GetPostProcessSettings().ToneMapping);
            hdr = nextHDR;
            invalidatingNormals = invalidate;
            auto target         = materialTarget(LoadOp::Load, hdr, invalidate);
            commands->BeginRendering(target);
            commands->SetViewport({0, 0, float(View.Viewport.x), float(View.Viewport.y), 0, 1});
            commands->SetScissor({0, 0, View.Viewport.x, View.Viewport.y});
            rendering = true;
        };
        const auto           environment = GetMaterialEnvironment();
        size_t               range = 0, frameRange = 0;
        std::optional<float> uploadedDebugView;
        const auto           litFrame = frame;
        for (const auto& item : ready)
        {
            if (transparentOnly &&
                *transparentOnly != (item.Text || item.UI || IsTransparentMaterial(item.Render.AlphaMode)))
                continue;
            if (item.UI)
            {
                if (!tag.empty())
                    continue;
                beginTarget(true);
                Services.GetUIRenderer().Draw(*commands,
                                              view.UIs[item.DrawIndex],
                                              frame.ViewProjection,
                                              materialTarget(LoadOp::Load, true),
                                              View.Viewport);
                continue;
            }
            if (item.Text)
            {
                if (!tag.empty())
                    continue;
                beginTarget(true);
                auto text      = view.Texts[item.DrawIndex];
                text.Transform = TextWorldTransform(
                    text.Style, text.Transform, View.Scene.sceneCamera.Position, View.Scene.sceneCamera.viewMatrix);
                Services.GetTextRenderer().Draw(
                    *commands, text, frame.ViewProjection, materialTarget(LoadOp::Load, true));
                ++Stats.DrawCalls;
                Stats.Triangles += text.Layout->Get().Glyphs.size() * 2;
                continue;
            }
            const auto& draw = view.Draws[item.DrawIndex];
            // Requests may finish during this pass. Snapshot the state used for sorting,
            // but acquire GPU resources at the draw so a cache replacement cannot dangle a queued pointer.
            const auto* mesh     = GetMesh(draw.Mesh);
            const auto* bindings = GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit);
            const auto* shader   = bindings ? GetShader(draw.Material, bindings->Shader) : nullptr;
            if (!mesh || draw.Section >= mesh->Sections.size() || !bindings || !shader ||
                shader->Domain == ShaderDomain::PostProcess)
                continue;
            if (!tag.empty() &&
                (!draw.Ready || std::none_of(shader->Passes.begin(), shader->Passes.end(), [&](const auto& pass) {
                    return pass.Tag == tag && bindings->PassEnabled(pass.Name);
                })))
                continue;
            const auto& render = item.Render;
            frame              = litFrame;
            frame.DebugView    = draw.ViewMode == SceneViewMode::BaseColor      ? 1.0f :
                                 draw.ViewMode == SceneViewMode::Normals        ? 2.0f :
                                 draw.ViewMode == SceneViewMode::Wireframe      ? 3.0f :
                                 draw.ViewMode == SceneViewMode::ShadowCascades ? 4.0f :
                                                                                  0.0f;
            if (draw.ViewMode == SceneViewMode::Wireframe)
            {
                // The wireframe diagnostic intentionally retains its phase-one appearance.
                frame.LightingInfo.x = EngineLightCount;
                for (uint32_t i = 0; i < EngineLightCount; ++i)
                {
                    frame.PointPositions[i] = glm::vec4(View.Scene.pointLight.Position, 1);
                    frame.PointRadiance[i]  = glm::vec4(View.Scene.pointLight.Ambient_Intensity, 0);
                    frame.PointSpecular[i]  = glm::vec4(1);
                }
                frame.DirectionalDirection = frame.DirectionalAmbient = glm::vec4(0);
            }
            if (!uploadedDebugView || *uploadedDebugView != frame.DebugView)
            {
                if (uploadedDebugView)
                    ++frameRange;
                frames->Write(frameRange * frameStride, &frame, sizeof(frame));
                uploadedDebugView = frame.DebugView;
            }
            EngineObjectUniforms object;
            object.Transform      = draw.Transform;
            object.NormalMatrix   = glm::transpose(glm::inverse(draw.Transform));
            object.MaterialRender = {float(render.AlphaMode),
                                     render.AlphaCutoff,
                                     render.DoubleSided ? 1.0f : 0.0f,
                                     render.ReceiveShadows && draw.ReceiveShadows ? 0.0f : 1.0f};
            if (Samples > 1 && !MSAAResolved && render.AlphaMode == MaterialAlphaMode::Mask)
                object.MaterialRender.x = 4.0f;
            if (Samples > 1 && !MSAAResolved && render.AlphaMode == MaterialAlphaMode::Dithered)
                object.MaterialRender.x = 5.0f;
            objects->Write(range * objectStride, &object, sizeof(object));
            MaterialPassData pass;
            pass.Palette            = mesh->Skin ? Assets()->GetSkinningPalette(draw.SkinPose, mesh) :
                                                   RenderHardware::Handle<RenderHardware::Buffer>();
            pass.Frame              = frames;
            pass.Object             = objects;
            pass.FrameOffset        = frameRange * frameStride;
            pass.ObjectOffset       = range * objectStride;
            pass.Shadows            = View.Shadows;
            pass.Environment        = environment;
            pass.EnvironmentSampler = Services.GetMaterialEnvironmentSampler();
            // Debug views, including wireframe's lit shading, retain display output.
            const bool nextHDR =
                shader->OutputSpace == ShaderOutputSpace::LinearHDR && draw.ViewMode == SceneViewMode::Lit;
            // An uncovered custom surface may replace a covered surface's depth.
            // Invalidate that older normal in the same draw, after its original
            // fragment/discard logic, without changing its color or depth state.
            const bool invalidate = Output.GetNormalRoughness() && tag.empty() && !suppliedView &&
                                    !IsTransparentMaterial(render.AlphaMode) && !PrepassDraws.count(item.DrawIndex);
            beginTarget(nextHDR, invalidate);
            pass.ColorFormats.clear();
            for (const auto& color : materialTarget(LoadOp::Load, nextHDR, invalidate).Colors)
                pass.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
            pass.InvalidatePrepassNormals = invalidate;
            pass.WriteTemporalMasks       = writeTemporalMasks;
            pass.Wireframe    = draw.ViewMode == SceneViewMode::Wireframe;
            pass.WriteStencil = draw.Stencil == ModelStencilMode::WriteReference;
            pass.Render       = render;
            pass.Parameters   = draw.Parameters;
            pass.Samples      = SceneTarget(LoadOp::Load, nextHDR).Colors[0].Target->GetDesc().Samples;
            pass.Section      = draw.Section;
            // Keep Mask/Dithered clipping in the main fragment program: LessEqual
            // admits matching depth. Surface position outputs are Invariant, and authored
            // camera passes must preserve Main's position/clip expressions. Keeping the
            // clip also preserves Mask/Dithered coverage at LessEqual.
            pass.PrepassCovered = tag.empty() && !suppliedView && PrepassDraws.count(item.DrawIndex);
            // Texture requests can finish while shadows are being built. Retain the
            // prepass's exact material descriptor (including instance overrides) so
            // Mask/Dithered coverage cannot change between the two camera draws.
            if (pass.PrepassCovered)
                pass.MaterialGroup = PrepassDraws.at(item.DrawIndex).Material;
            // Velocity must use the exact alpha descriptor consumed by Main, including
            // the prepass snapshot and instance overrides; asynchronous loads may replace caches.
            if (VelocityActive && tag.empty() && !suppliedView)
            {
                const auto* instance = Assets()->GetInstanceMaterialBindings(*bindings, draw.Parameters);
                bool        complete = true;
                if (!pass.MaterialGroup && writeTemporalMasks && VelocitySurfaceDraws.count(item.DrawIndex))
                    pass.MaterialGroup = VelocitySurfaceDraws.at(item.DrawIndex).Material;
                if (!pass.MaterialGroup)
                    pass.MaterialGroup = Assets()->GetMaterialGroup(*shader, *instance, &complete);
                Complete &= complete;
                VelocitySurfaceDraws[item.DrawIndex] = {pass.MaterialGroup,
                                                        pass.PrepassCovered ? PrepassDraws.at(item.DrawIndex).Images :
                                                                              instance->Images};
            }
            bool recorded     = false;
            if (tag.empty())
                Complete &=
                    RecordMaterialDraw(*Assets(), *commands, *shader, *bindings, *mesh, pass, nullptr, &recorded);
            else
                for (const auto& extra : shader->Passes)
                    if (extra.Tag == tag && bindings->PassEnabled(extra.Name))
                    {
                        Complete &= RecordMaterialDraw(*Assets(), *commands, *shader, *bindings, *mesh, pass, &extra);
                        ++Stats.DrawCalls;
                        Stats.Triangles += mesh->Sections[draw.Section].IndexCount / 3;
                    }
            if (VelocityActive && tag.empty() && !recorded)
                VelocitySurfaceDraws.erase(item.DrawIndex);
            if (tag.empty() && recorded)
            {
                ++View.Culling.Drawn;
                for (const auto& extra : shader->Passes)
                    if (extra.Tag == "AfterMain" && bindings->PassEnabled(extra.Name))
                    {
                        Complete &= RecordMaterialDraw(*Assets(), *commands, *shader, *bindings, *mesh, pass, &extra);
                        ++Stats.DrawCalls;
                        Stats.Triangles += mesh->Sections[draw.Section].IndexCount / 3;
                    }
            }
            if (sampled)
                for (const auto& image :
                     pass.PrepassCovered ? PrepassDraws.at(item.DrawIndex).Images : bindings->Images)
                    sampled(image);
            if (tag.empty())
            {
                ++Stats.DrawCalls;
                Stats.Triangles += mesh->Sections[draw.Section].IndexCount / 3;
            }
            ++range;
        }
        if (rendering)
            commands->EndRendering();
        if (View.EffectiveAA == AntiAliasing::TAA && !hdr)
        {
            Services.GetToneMapping().Record(*GetDevice(),
                                             *commands,
                                             Colors()[0],
                                             {HDR()},
                                             View.GetPostProcessSettings().Exposure,
                                             true,
                                             View.GetPostProcessSettings().ToneMapping);
            hdr = true;
        }
        if (deferToneMapping)
            PostProcessHDR = hdr;
        if (hdr && !deferToneMapping)
        {
            if (finalPost)
            {
                const auto start = RenderClockMilliseconds();
                *finalPost = Services.GetDevice()->CreateCommandList();
                resolve(**finalPost);
                FinalPostRecordMilliseconds = RenderClockMilliseconds() - start;
            }
            else
                resolve(*commands);
        }
    }

    void RenderFrame::DrawModelGeometry(const ViewGeometryDraw& draw, RenderHardware::CommandList& commandList)
    {
        using namespace RenderHardware;
        if (!Assets())
            return;
        ModelInstance instance;
        instance.Section     = draw.Section.value_or(0);
        instance.AllSections = !draw.Section;
        const bool  normals  = draw.ShaderName == "DebugNormalShader";
        const char* vertex   = normals                            ? SceneShaders::NormalVertex :
                               draw.ShaderName == "OutLineShader" ? SceneShaders::OutlineVertex :
                               draw.ShaderName == "StencilShader" ? SceneShaders::StencilVertex :
                                                                    nullptr;
        const char* fragment = normals                            ? SceneShaders::NormalFragment :
                               draw.ShaderName == "OutLineShader" ? SceneShaders::OutlineFragment :
                                                                    SceneShaders::StencilFragment;
        if (!vertex)
            return;
        const bool  reconstruct    = ReconstructsHDR();
        const bool  hdrGeometry    = (Samples > 1 && !MSAAResolved && PostProcessHDR) || (Samples == 1 && reconstruct);
        std::string fragmentSource = fragment;
        if (hdrGeometry)
        {
            const auto at = fragmentSource.find("void main");
            fragmentSource.replace(at, 9, "void displayMain");
            const std::string output = draw.ShaderName == "OutLineShader" ? "O_Color" : "color";
            fragmentSource += R"(
layout(set=0,binding=0,std140) uniform SceneDraw { mat4 VP; mat4 Transform; mat4 Clip; vec4 Position; vec4 Settings; } p;
void main() {
    displayMain();
    vec3 c = OUTPUT.rgb;
    vec3 y = mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));
    vec3 b=0.59*y-0.03, a=2.43*y-2.51;
    OUTPUT.rgb=(p.Settings.z==1 ? y : 2*0.14*y/max(sqrt(b*b-4*a*0.14*y)-b,vec3(1e-8)))/p.Settings.w;
})";
            size_t atOutput;
            while ((atOutput = fragmentSource.find("OUTPUT")) != std::string::npos)
                fragmentSource.replace(atOutput, 6, output);
        }
        PipelineDesc desc = Services.ScenePipeline(draw.ShaderName.c_str(), vertex, fragmentSource.c_str(), false);
        if (Samples > 1 || hdrGeometry)
        {
            desc.ColorFormats.clear();
            for (const auto& color : SceneTarget(LoadOp::Load, hdrGeometry).Colors)
                desc.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
            desc.Blends.resize(desc.ColorFormats.size(), desc.Blends[0]);
        }
        if (!desc.Program)
            return;
        desc.Samples = Samples > 1 && !MSAAResolved ? Samples : 1;
        desc.DepthStencil.DepthCompare =
            draw.Mode == ModelGeometryMode::StencilOutline ? CompareOp::LessEqual : CompareOp::Less;
        desc.DepthStencil.StencilTest   = draw.Mode == ModelGeometryMode::StencilOutline;
        desc.DepthStencil.Front.Compare = desc.DepthStencil.Back.Compare = CompareOp::NotEqual;
        desc.DepthStencil.Front.Reference = desc.DepthStencil.Back.Reference = 1;
        desc.DepthStencil.Front.WriteMask = desc.DepthStencil.Back.WriteMask = 0;
        desc.Rasterizer.Cull = draw.Mode == ModelGeometryMode::BackFaces ? CullMode::Front : CullMode::None;
        if (normals)
        {
            desc.Primitive      = Topology::LineList;
            desc.VertexBindings = {{0, 28, false}};
            desc.Attributes     = {
                {0, 0, 0, VertexFormat::Float3}, {1, 0, 12, VertexFormat::Float3}, {2, 0, 24, VertexFormat::Float}};
        }
        else
        {
            desc.VertexBindings = {{0, 64, false}};
            desc.Attributes     = {{0, 0, 0, VertexFormat::Float3},
                                   {1, 0, 12, VertexFormat::Float3},
                                   {2, 0, 24, VertexFormat::Float2},
                                   {3, 0, 32, VertexFormat::Float4},
                                   {4, 0, 48, VertexFormat::Float4}};
        }
        const auto* geometryMesh = GetMesh(draw.Mesh);
        if (geometryMesh && geometryMesh->Skin && draw.SkinPose)
        {
            desc.Program = GetSkinnedInternalProgram(*Services.GetDevice(), draw.ShaderName, vertex, fragmentSource);
            desc.GroupLayouts.push_back(Services.GetDevice()->CreateBindGroupLayout({1, {}}));
            desc.GroupLayouts.push_back(SkinningLayout(*Services.GetDevice()));
            desc.VertexBindings.push_back({1, 32, false});
            desc.Attributes.push_back({5, 1, 0, VertexFormat::Uint4});
            desc.Attributes.push_back({6, 1, 16, VertexFormat::Float4});
        }
        auto  pipeline = Services.CachedPipeline(desc);
        auto* commands = &commandList;
        commands->BeginRendering(SceneTarget(LoadOp::Load, hdrGeometry));
        commands->SetViewport({0, 0, float(View.Viewport.x), float(View.Viewport.y)});
        commands->BindPipeline(pipeline);
        SceneDrawData data;
        data.DeviceClip     = DeviceProjection(glm::mat4(1), Services.GetDevice()->GetCoordinateConventions());
        data.ViewProjection = View.Scene.sceneCamera.camera.GetProjectionMatrix() * View.Scene.sceneCamera.viewMatrix;
        data.ViewPos        = glm::vec4(View.Scene.sceneCamera.Position, 1);
        if (hdrGeometry)
        {
            data.Settings.w = ToneMappingPass::EffectiveExposure(View.GetPostProcessSettings().Exposure);
            data.Settings.z = reconstruct ? float(View.GetPostProcessSettings().ToneMapping) : 0;
        }
        if (const GpuMesh* mesh = GetMesh(draw.Mesh))
            ForEachSection(instance, *mesh, [&](const auto& part, const auto& section) {
                data.Transform = draw.Transform;
                commands->BindGroup(DrawGroup(*Services.GetDevice(), desc.GroupLayouts[0], data));
                if (mesh->Skin && draw.SkinPose)
                {
                    commands->BindGroup(Services.GetDevice()->CreateBindGroup({desc.GroupLayouts[1], {}}));
                    auto palette = Assets()->GetSkinningPalette(draw.SkinPose, mesh);
                    BindSkinning(*Services.GetDevice(),
                                 *commands,
                                 desc.GroupLayouts[2],
                                 palette ? palette : mesh->BindPalette,
                                 mesh->MorphDeltas);
                    commands->BindVertexBuffers({{0, normals ? mesh->NormalLines : mesh->Vertices, 0},
                                                 {1, normals ? mesh->NormalSkin : mesh->Skin, 0}});
                }
                else
                    commands->BindVertexBuffers(
                        {{0,
                          normals ? (mesh->BindNormalLines ? mesh->BindNormalLines : mesh->NormalLines) :
                                    (mesh->BindVertices ? mesh->BindVertices : mesh->Vertices),
                          0}});
                if (normals)
                    commands->Draw(section.IndexCount * 2, 1, section.FirstIndex * 2);
                else
                {
                    commands->BindIndexBuffer(mesh->Indices, IndexType::Uint32);
                    commands->DrawIndexed(section.IndexCount, 1, section.FirstIndex);
                }
                ++Stats.DrawCalls;
                Stats.Triangles += section.IndexCount / 3;
            });
        commands->EndRendering();
    }

    void RenderFrame::SubmitPrefabPreview(const PrefabPreview& preview)
    {
        if (!Assets())
            return;
        if (const auto* instances = Assets()->GetModel(preview.Prefab))
            for (const auto& instance : instances->Instances)
                if (const auto* mesh = GetMesh(instance.Mesh))
                    ForEachSection(instance, *mesh, [&](const auto& part, const auto&) {
                        ExtractMeshDraws(View,
                                         {{part.Mesh, {}, part.Section}, preview.Transform * part.Transform},
                                         SceneViewMode::Lit,
                                         mesh);
                    });
    }

    void RenderFrame::AppendPrefabPreviewDraws(const PrefabPreview& preview, std::vector<MeshDraw>& draws)
    {
        if (!Assets())
            return;
        if (const auto* instances = Assets()->GetModel(preview.Prefab))
            for (const auto& instance : instances->Instances)
                if (const auto* mesh = GetMesh(instance.Mesh))
                    ForEachSection(instance, *mesh, [&](const auto& part, const auto&) {
                        draws.push_back({mesh, preview.Transform * part.Transform, 0, part.Section});
                    });
    }

    void RenderFrame::DrawSkybox(const ViewSkybox& sky, RenderHardware::CommandList& commandList)
    {
        const auto& skybox = sky.Texture;
        const auto& state  = sky.State;
        using namespace RenderHardware;
        if (!skybox || !View.Environment.ShowSkybox)
            return;
        const bool hdrSkybox = (Samples > 1 && !MSAAResolved) || ReconstructsHDR();
        if (hdrSkybox && !PostProcessHDR)
        {
            Services.GetToneMapping().Record(*GetDevice(),
                                             commandList,
                                             Colors()[0],
                                             {HDR()},
                                             View.GetPostProcessSettings().Exposure,
                                             true,
                                             View.GetPostProcessSettings().ToneMapping);
            PostProcessHDR = true;
        }
        auto desc = Services.ScenePipeline("skybox", SceneShaders::SkyboxVertex, SceneShaders::SkyboxFragment, true);
        if (hdrSkybox)
        {
            desc.ColorFormats.clear();
            for (const auto& color : SceneTarget(LoadOp::Load, true).Colors)
                desc.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
            desc.Blends.resize(desc.ColorFormats.size(), desc.Blends[0]);
        }
        if (!desc.Program)
            return;
        desc.Samples                   = Samples > 1 && !MSAAResolved ? Samples : 1;
        const auto& cube               = Services.GetCube();
        desc.VertexBindings            = {{0, 64, false}};
        desc.Attributes                = {{0, 0, 0, VertexFormat::Float3}};
        desc.DepthStencil.DepthCompare = state.DepthCompare;
        desc.DepthStencil.DepthWrite   = state.DepthWrite;
        desc.Blends[0].Enabled         = state.Blend;
        desc.Blends[0].WriteMask       = state.WriteMask;
        desc.Blends[0].SourceColor = desc.Blends[0].SourceAlpha = BlendFactor::SourceAlpha;
        desc.Blends[0].DestinationColor = desc.Blends[0].DestinationAlpha = BlendFactor::OneMinusSourceAlpha;
        if (desc.Blends.size() > 1)
            desc.Blends[1] = desc.Blends[0];
        SceneDrawData data;
        data.DeviceClip     = DeviceProjection(glm::mat4(1), Services.GetDevice()->GetCoordinateConventions());
        data.ViewProjection  = SceneCamera().GetProjectionMatrix();
        data.Transform      = View.Scene.sceneCamera.viewMatrix;
        const float rotation = glm::radians(View.Environment.Rotation);
        data.Settings        = {
            View.Environment.SkyboxIntensity, std::cos(rotation), std::sin(rotation), hdrSkybox ? 1.0f : 0.0f};
        GetMaterialEnvironment();
        auto* commands = &commandList;
        commands->BeginRendering(SceneTarget(LoadOp::Load, hdrSkybox));
        commands->SetViewport({0, 0, float(View.Viewport.x), float(View.Viewport.y)});
        commands->BindPipeline(Services.CachedPipeline(desc));
        commands->BindGroup(DrawGroup(
            *Services.GetDevice(), desc.GroupLayouts[0], data, skybox, Services.GetMaterialEnvironmentSampler()));
        commands->BindVertexBuffers({{0, cube.Vertices, 0}});
        commands->BindIndexBuffer(cube.Indices, IndexType::Uint32);
        commands->DrawIndexed(cube.IndexCount);
        commands->EndRendering();
    }

    std::array<RenderHardware::Handle<RenderHardware::Texture>, 3> RenderFrame::GetMaterialEnvironment()
    {
        EnvironmentLighting lighting;
        lighting.Maps.IrradianceMap = View.EnvironmentMaps.IrradianceMap;
        lighting.Maps.RadianceMap   = View.EnvironmentMaps.RadianceMap;
        lighting.BrdfLUT            = View.BrdfLUT;
        return Services.GetMaterialEnvironment(lighting);
    }
    void RenderFrame::AppendEntityDraws(UUID entity, uint32_t id, std::vector<MeshDraw>& draws)
    {
        Services.AppendEntityDraws(World, View, entity, id, draws);
    }
    const GpuMesh* RenderFrame::GetMesh(const MeshKey& id) const
    {
        auto it = View.MeshResources.find(id);
        return it != View.MeshResources.end() ? it->second.get() : Assets() ? Assets()->GetMesh(id) : nullptr;
    }
    const GpuMaterialBindings* RenderFrame::GetBindings(AssetID id, bool debug) const
    {
        auto it = View.MaterialResources.find(id);
        return it != View.MaterialResources.end() ? it->second.Bindings.get() :
               Assets()                           ? Assets()->GetMaterialBindings(id, debug) :
                                                    nullptr;
    }
    const GpuShader* RenderFrame::GetShader(AssetID material, AssetID shader) const
    {
        auto it = View.MaterialResources.find(material);
        return it != View.MaterialResources.end() ? it->second.Shader.get() :
               Assets()                           ? Assets()->GetShader(shader) :
                                                    nullptr;
    }
} // namespace Kans
