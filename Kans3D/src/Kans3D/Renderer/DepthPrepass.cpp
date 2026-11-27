#include "kspch.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Renderer/Lighting/AreaLights.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
#include "RenderFrame.h"
#include "Temporal.h"

namespace Kans
{
    void RenderFrame::BuildDepthPrepass(RenderGraph& graph)
    {
        KANS_PROFILE_SCOPE("Record camera depth prepass");
        using namespace RenderHardware;
        Output.EnsureNormalRoughness();
        const auto normal = graph.ImportTexture("SceneNormalRoughness", Output.GetNormalRoughness());
        const bool reflections = View.Environment.ScreenSpaceReflections.Enabled;
        if (reflections)
        {
            auto& surface = Output.GetTemporalState().ReflectionSurface;
            if (!surface || surface->GetDesc().Width != View.Viewport.x || surface->GetDesc().Height != View.Viewport.y)
            {
                TextureDesc d;
                d.Width       = View.Viewport.x;
                d.Height      = View.Viewport.y;
                d.PixelFormat = Format::RGBA16Float;
                d.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled;
                surface       = GetDevice()->CreateTexture(d);
            }
            ReflectionSurface = surface;
        }
        const auto depth  = graph.ImportTexture("SceneDepthStencil", Output.GetDepth());
        graph.ExportTexture(normal);
        std::vector<GraphUse> uses {{normal, GraphAccess::ColorWrite}, {depth, GraphAccess::DepthReadWrite}};
        if (reflections)
            uses.push_back({graph.ImportTexture("SSR receiver surface", ReflectionSurface), GraphAccess::ColorWrite});
        const auto            sampled = [&](const auto& image) {
            if (!image)
                return;
            const auto id = graph.ImportTexture("Prepass material input", image);
            if (std::none_of(uses.begin(), uses.end(), [&](const auto& use) { return use.Texture == id; }))
                uses.push_back({id, GraphAccess::SampledRead});
        };
        const auto    start    = RenderClockMilliseconds();
        auto          device   = GetDevice();
        auto          commands = device->CreateCommandList();
        RenderingDesc target;
        target.Colors.push_back({Output.GetNormalRoughness(), {}, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}});
        if (reflections)
            target.Colors.push_back({ReflectionSurface, {}, LoadOp::Clear});
        target.Depth.Target = Output.GetDepth();
        target.Depth.Load   = LoadOp::Load;
        commands->BeginRendering(target);
        commands->SetViewport({0, 0, float(View.Viewport.x), float(View.Viewport.y), 0, 1});
        commands->SetScissor({0, 0, View.Viewport.x, View.Viewport.y});
        if (Assets() && !View.MaterialOrder.empty())
        {
            auto frame            = View.LightingUniforms.value_or(MakeLightingUniforms(View.Scene));
            frame.View            = View.Scene.sceneCamera.viewMatrix;
            const auto projection = View.Scene.sceneCamera.camera.GetProjectionMatrix();
            // Keep the multiplication order identical to DrawShaderMaterials.
            frame.ViewProjection    = DeviceProjection(projection * frame.View, device->GetCoordinateConventions());
            frame.Projection        = DeviceProjection(projection, device->GetCoordinateConventions());
            ApplyTemporalUniforms(frame);
            frame.InverseView       = glm::inverse(frame.View);
            frame.InverseProjection = glm::inverse(frame.Projection);
            frame.CameraPosition    = glm::vec4(View.Scene.sceneCamera.Position, 1);
            frame.Time              = View.Time;
            frame.ShadowPass        = glm::vec4(0);
            frame.LightingInfo.y    = View.GetPostProcessSettings().Exposure;
            const float rotation    = glm::radians(View.Environment.Rotation);
            frame.EnvironmentInfo   = {View.Environment.DiffuseIntensity,
                                       View.Environment.SpecularIntensity,
                                       std::cos(rotation),
                                       std::sin(rotation)};
            const size_t alignment  = device->GetBufferBindingAlignment(BindingType::UniformBuffer);
            const auto   align      = [alignment](size_t n) { return (n + alignment - 1) / alignment * alignment; };
            const size_t frameBytes = align(sizeof(frame)), stride = align(sizeof(EngineObjectUniforms));
            // One upload allocation for this pass; every draw owns immutable aligned ranges.
            const auto           upload = AllocateUniforms(*commands, frameBytes + stride * View.MaterialOrder.size());
            auto                 uniforms = upload.Buffer;
            std::vector<uint8_t> packed(frameBytes + stride * View.MaterialOrder.size());
            std::memcpy(packed.data(), &frame, sizeof(frame));
            size_t               range       = 0;
            const auto           environment = GetMaterialEnvironment();
            for (const auto& item : View.MaterialOrder)
            {
                if (item.Text || item.UI || item.AlwaysOnTop || IsTransparentMaterial(item.Render.AlphaMode))
                    continue;
                const auto& draw = View.Draws[item.DrawIndex];
                if (!draw.Ready || draw.ViewMode != SceneViewMode::Lit)
                    continue;
                const auto* mesh     = GetMesh(draw.Mesh);
                const auto* bindings = GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit);
                const auto* shader   = bindings ? GetShader(draw.Material, bindings->Shader) : nullptr;
                if (!mesh || draw.Section >= mesh->Sections.size() || !shader ||
                    shader->Domain != ShaderDomain::Surface)
                    continue;
                // A pending custom shader can replace its Standard fallback during
                // graph preparation. Leave these unavailable materials to Main;
                // a retained Standard material group cannot bind a custom layout.
                if (draw.ViewMode == SceneViewMode::Lit && !View.MaterialResources.count(draw.Material) &&
                    IsStandardShader(bindings->Shader) && draw.Material.IsValid())
                {
                    auto material = Assets()->GetAssetManager().Request<MaterialDataAsset>(draw.Material);
                    if (material.IsReady() && !IsStandardShader(material.Asset->GetData().Shader))
                        continue;
                }
                size_t selected = SIZE_MAX;
                for (size_t i = 0; i < shader->Passes.size(); ++i)
                    if (shader->Passes[i].Tag == "DepthNormals" && bindings->PassEnabled(shader->Passes[i].Name))
                    {
                        selected = i;
                        break;
                    }
                const bool standard = selected == SIZE_MAX && IsStandardShader(bindings->Shader);
                if (selected == SIZE_MAX && !standard)
                {
                    // The material ABI has independent shader mains, not a reusable surface
                    // function. Authored depth passes must match Main's position and clip.
                    // ShadowCaster is a light-view contract and may extrude or bias
                    // vertices. Only DepthOnly is safe as the camera-depth fallback.
                    for (size_t i = 0; i < shader->Passes.size(); ++i)
                        if (shader->Passes[i].Tag == "DepthOnly" && bindings->PassEnabled(shader->Passes[i].Name))
                        {
                            selected = i;
                            break;
                        }
                    if (selected == SIZE_MAX)
                        continue; // Main retains its depth write; flag remains zero.
                }
                MaterialPassData data;
                data.MaterialParameters = View.MaterialParameters;
                data.MaterialMipBias = MaterialMipBias;
                data.DepthPrepass = true;
                // Preserve Main's stencil history even if a later unmarked opaque
                // surface hides this draw before Main runs (selection outlines use it).
                data.WriteStencil       = draw.Stencil == ModelStencilMode::WriteReference;
                data.ColorFormats       = {Format::RGBA16Float};
                if (reflections)
                    data.ColorFormats.push_back(Format::RGBA16Float);
                data.ReflectionSurface  = reflections && standard;
                data.Render             = item.Render;
                data.Section            = draw.Section;
                data.Parameters         = draw.Parameters;
                data.Environment        = environment;
                data.EnvironmentSampler = Services.GetMaterialEnvironmentSampler();
                data.AreaLighting       = Assets()->GetAreaLightFallback();
                data.Shadows            = Assets()->GetShadowFallback();
                data.Palette =
                    mesh->Skin ? Assets()->GetSkinningPalette(draw.SkinPose, mesh) : Handle<RenderHardware::Buffer>();
                const bool smooth = selected != SIZE_MAX && shader->Passes[selected].UsesSmoothNormals;
                if (data.Palette && !shader->PrepareSkinning(smooth && !mesh->SmoothNormals))
                {
                    Complete = false;
                    continue;
                }
                if (standard)
                {
                    // Share Main's exact vertex product, including its patched skin/morph module.
                    data.DepthProgram = shader->PrepareStandardDepthNormals(bool(data.Palette), reflections);
                    if (!data.DepthProgram)
                    {
                        Complete = false;
                        continue;
                    }
                }
                else if (shader->Passes[selected].Tag != "DepthNormals")
                {
                    data.DepthProgram =
                        shader->PrepareDepthOnlyPrepass(selected, bool(data.Palette), smooth && !mesh->SmoothNormals);
                    if (!data.DepthProgram)
                    {
                        Complete = false;
                        continue;
                    }
                }
                EngineObjectUniforms object;
                object.Transform      = draw.Transform;
                object.NormalMatrix   = glm::transpose(glm::inverse(draw.Transform));
                object.MaterialRender = {float(item.Render.AlphaMode),
                                         item.Render.AlphaCutoff,
                                         item.Render.DoubleSided ? 1.0f : 0.0f,
                                         item.Render.ReceiveShadows && draw.ReceiveShadows ? 0.0f : 1.0f};
                data.Frame = data.Object = uniforms;
                data.UniformStorage      = UniformStorage;
                data.FrameOffset         = upload.Offset;
                data.ObjectOffset        = upload.Offset + range * stride + frameBytes;
                std::memcpy(packed.data() + range * stride + frameBytes, &object, sizeof(object));
                const auto* instance = Assets()->GetInstanceMaterialBindings(*bindings, draw.Parameters);
                bool        complete = true;
                data.MaterialGroup   = Assets()->GetMaterialGroup(*shader, *instance, &complete, MaterialMipBias);
                Complete &= complete;
                if (!data.MaterialGroup)
                    continue;
                bool recorded = false;
                Complete &= RecordMaterialDraw(*Assets(),
                                               *commands,
                                               *shader,
                                               *instance,
                                               *mesh,
                                               data,
                                               selected == SIZE_MAX ? nullptr : &shader->Passes[selected],
                                               &recorded);
                if (recorded)
                {
                    PrepassDraws.emplace(item.DrawIndex, PrepassDraw {data.MaterialGroup, instance->Images});
                    ++Stats.PrepassDrawCalls;
                    Stats.PrepassTriangles += mesh->Sections[draw.Section].IndexCount / 3;
                    ++range;
                }
                for (const auto& image : instance->Images)
                    sampled(image);
                for (const auto& image : data.Environment)
                    sampled(image);
                sampled(data.Shadows->Directional);
                sampled(data.Shadows->Spots);
                sampled(data.Shadows->Cookies);
                if (!View.Scene.areaLights.empty())
                    for (const auto& image : AreaLightImages(*data.AreaLighting))
                        sampled(image);
                for (const auto& image : data.Shadows->Points)
                    sampled(image);
            }
            if (range)
                uniforms->Write(upload.Offset, packed.data(), frameBytes + range * stride);
        }
        commands->EndRendering();
        const auto pass =
            graph.AddRecordedPass("Depth prepass", std::move(uses), commands, RenderClockMilliseconds() - start);
        if (pass)
            graph.OrderAfter(pass, pass - 1);
    }
} // namespace Kans
