#include "kspch.h"
#include "RenderPipeline.h"
#include <map>
#include <stdexcept>
#include "Kans3D/Renderer/2D/SpriteBatch.h"
#include "Kans3D/Renderer/2D/TextRenderer.h"
#include "Kans3D/Renderer/2D/UIRenderer.h"
#include "Kans3D/Renderer/Features/DebugDrawFeature.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Features/ParticleRenderFeature.h"
#include "RenderFrame.h"

namespace Kans
{
    namespace
    {
        std::map<std::string, RenderPipelineRegistry::Factory>& Factories()
        {
            static std::map<std::string, RenderPipelineRegistry::Factory> factories {
                {"Forward", [] { return std::make_unique<ForwardPipeline>(); }}};
            return factories;
        }
    } // namespace
    void RenderPipelineRegistry::Register(std::string name, Factory factory)
    {
        if (name.empty() || !factory)
            throw std::invalid_argument("RenderPipeline: invalid registration");
        if (!Factories().emplace(std::move(name), std::move(factory)).second)
            throw std::invalid_argument("RenderPipeline: duplicate registration");
    }
    std::unique_ptr<RenderPipeline> RenderPipelineRegistry::Create(const std::string& name)
    {
        auto it = Factories().find(name);
        if (it == Factories().end())
            throw std::invalid_argument("RenderPipeline: unknown pipeline '" + name + "'");
        auto pipeline = it->second();
        if (!pipeline)
            throw std::invalid_argument("RenderPipeline: factory returned null");
        return pipeline;
    }
    void RenderPipeline::Inject(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        ParticleRenderFeature particles;
        if (particles.RunsAt(point))
            particles.AddPasses(point, context); // Contains failures per emitter.
        DebugDrawFeature debug;
        if (debug.RunsAt(point))
        {
            const auto first       = context.Graph.PassCount();
            bool       failedBuild = false;
            try
            {
                debug.AddPasses(point, context);
            }
            catch (const RenderHardware::PipelineCreationError& error)
            {
                failedBuild = true;
                RenderFeatureSet::ReportPipelineFailure(error.what());
            }
            if (context.Graph.PassCount() != first)
            {
                // DebugDrawFeature is local; keep only its failure flag alive for deferred recording.
                auto failed = std::make_shared<bool>(failedBuild);
                context.Graph.GuardPipelineFailures(
                    first,
                    [failed](const auto& error) {
                        *failed = true;
                        RenderFeatureSet::ReportPipelineFailure(error);
                    },
                    [failed] { return *failed; });
            }
        }
        if (context.View.Features)
            context.View.Features->Inject(point, context);
    }
    void ForwardPipeline::Prepare(RenderView& view)
    {
        using namespace RenderHardware;
        const auto projection =
            DeviceProjection(view.Scene.sceneCamera.camera.GetProjectionMatrix(),
                             {ClipYDirection::Down, DepthRange::ZeroToOne, FramebufferOrigin::UpperLeft});
        const Frustum frustum(projection * view.Scene.sceneCamera.viewMatrix);
        ExtractDrawLists(view, &frustum);
    }
    void ForwardPipeline::Begin(RenderGraph& graph, RenderFrame& renderer)
    {
        const auto& view = renderer.View;
        using namespace RenderHardware;
        std::vector<GraphUse> clear;
        for (size_t i = 0; i < renderer.Output.GetColors().size(); ++i)
        {
            auto image = graph.ImportTexture("SceneColor" + std::to_string(i), renderer.Output.GetColors()[i]);
            clear.push_back({image, GraphAccess::ColorWrite});
            graph.ExportTexture(image);
        }
        const auto depth = graph.ImportTexture("SceneDepthStencil", renderer.Output.GetDepth());
        clear.push_back({depth, GraphAccess::DepthWrite});
        graph.ExportTexture(depth);
        graph.AddPass("Clear", clear, [&renderer](auto& context) {
            context.Commands.BeginRendering(renderer.SceneTarget(LoadOp::Clear));
            context.Commands.EndRendering();
        });
    }
    void ForwardPipeline::PreparePostMaterials(RenderFrame& frame)
    {
        m_PostDraws.clear();
        if (!frame.Assets())
            return;
        for (const auto& entry : frame.View.GetPostProcessSettings().Materials)
        {
            if (!entry.Enabled || !entry.Material.IsValid())
                continue;
            // Explicit per-view snapshots are used by previews and deterministic GPU fixtures.
            if (!frame.View.MaterialResources.count(entry.Material))
            {
                auto status = frame.Assets()->GetAssetManager().Request<MaterialDataAsset>(entry.Material);
                if (!status.IsReady())
                {
                    frame.Complete &= status.State != AssetLoadState::Loading;
                    continue;
                }
                const auto* material = frame.Assets()->GetMaterial(entry.Material);
                if (!material)
                    continue;
                const auto* shader = frame.Assets()->GetShader(material->Shader);
                if (!shader)
                {
                    auto state = frame.Assets()->GetAssetManager().Request<ShaderDataAsset>(material->Shader);
                    frame.Complete &= state.State != AssetLoadState::Loading;
                    continue;
                }
            }
            const auto* bindings = frame.GetBindings(entry.Material, false);
            const auto* shader   = bindings ? frame.GetShader(entry.Material, bindings->Shader) : nullptr;
            if (!bindings || !shader)
                continue;
            if (shader->Domain != ShaderDomain::PostProcess)
            {
                frame.Assets()->WarnMaterialDomain(entry.Material, true);
                continue;
            }
            bool complete = true;
            auto group    = frame.Assets()->GetMaterialGroup(*shader, *bindings, &complete);
            frame.Complete &= complete;
            if (!group)
                continue;
            m_PostDraws.push_back(
                {std::make_shared<GpuShader>(shader->Snapshot()), group, bindings->Images, entry.Material});
        }
        if (!m_PostDraws.empty())
            frame.Output.EnsureSampledDepth();
        if (std::any_of(m_PostDraws.begin(), m_PostDraws.end(), [](const auto& draw) {
                return draw.Shader->PostStage == PostProcessStage::BeforeToneMap;
            }))
            frame.DeferToneMapping = true;
    }
    void ForwardPipeline::AddPostMaterials(RenderGraph& graph, RenderFrame& frame, PostProcessStage stage)
    {
        using namespace RenderHardware;
        const bool hdr    = stage == PostProcessStage::BeforeToneMap;
        const auto append = [&](std::string name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            const auto pass = graph.AddPass(std::move(name), std::move(uses), std::move(record));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        for (const auto& draw : m_PostDraws)
        {
            if (draw.Shader->PostStage != stage)
                continue;
            const auto image  = hdr ? frame.Output.GetHDR() : frame.Output.GetColors()[0];
            const auto output = graph.ImportTexture(hdr ? "SceneHDR" : "SceneColor0", image);
            if (hdr && !frame.PostProcessHDR)
            {
                const auto display = graph.ImportTexture("Post display input", frame.Output.GetColors()[0]);
                append("Post material HDR prefill",
                       {{display, GraphAccess::SampledRead}, {output, GraphAccess::ColorWrite}},
                       [&frame](auto& c) {
                           frame.Services.GetToneMapping().Record(c.Device,
                                                                  c.Commands,
                                                                  frame.Output.GetColors()[0],
                                                                  {frame.Output.GetHDR()},
                                                                  frame.View.GetPostProcessSettings().Exposure,
                                                                  true,
                                                                  frame.View.GetPostProcessSettings().ToneMapping);
                       });
            }
            auto desc        = image->GetDesc();
            desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
            const auto input = graph.CreateTexture("Post material input", desc);
            append("Post material input copy",
                   {{output, GraphAccess::TransferRead}, {input, GraphAccess::TransferWrite}},
                   [output, input](auto& c) { c.Commands.CopyTexture(c.Texture(output), {}, c.Texture(input)); });
            const auto            depth = graph.ImportTexture("Post scene depth", frame.Output.GetDepth());
            std::vector<GraphUse> uses {{input, GraphAccess::SampledRead},
                                        {depth, GraphAccess::SampledRead},
                                        {m_CustomDepth, GraphAccess::SampledRead},
                                        {output, GraphAccess::ColorReadWrite}};
            for (const auto& texture : draw.Images)
            {
                const auto id = graph.ImportTexture("Post material texture", texture);
                if (std::none_of(uses.begin(), uses.end(), [&](auto u) { return u.Texture == id; }))
                    uses.push_back({id, GraphAccess::SampledRead});
            }
            auto uniforms              = frame.View.LightingUniforms.value_or(MakeLightingUniforms(frame.View.Scene));
            uniforms.View              = frame.View.Scene.sceneCamera.viewMatrix;
            uniforms.Projection        = DeviceProjection(frame.View.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                                          frame.GetDevice()->GetCoordinateConventions());
            uniforms.ViewProjection    = uniforms.Projection * uniforms.View;
            uniforms.InverseView       = glm::inverse(uniforms.View);
            uniforms.InverseProjection = glm::inverse(uniforms.Projection);
            uniforms.CameraPosition    = glm::vec4(frame.View.Scene.sceneCamera.Position, 1);
            uniforms.Time              = frame.View.Time;
            uniforms.LightingInfo.y    = frame.View.GetPostProcessSettings().Exposure;
            auto buffer =
                frame.GetDevice()->CreateBuffer({sizeof(uniforms), BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, &uniforms, sizeof(uniforms));
            const auto custom = m_CustomDepth;
            if (!m_PostLinearSampler)
            {
                SamplerDesc sampler;
                sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
                sampler.Min = sampler.Mag = Filter::Linear;
                sampler.Mip               = Filter::Nearest;
                sampler.MaxLod            = 0;
                m_PostLinearSampler       = frame.GetDevice()->CreateSampler(sampler);
                sampler.Min = sampler.Mag = Filter::Nearest;
                m_PostPointSampler        = frame.GetDevice()->CreateSampler(sampler);
            }
            append(std::string("Post material ") + (hdr ? "before_tonemap " : "after_tonemap ") + draw.ID.ToString(),
                   std::move(uses),
                   [draw,
                    &frame,
                    buffer,
                    input,
                    output,
                    depth,
                    custom,
                    linear = m_PostLinearSampler,
                    point  = m_PostPointSampler](auto& c) {
                       PipelineDesc pipeline;
                       pipeline.Program         = draw.Shader->Program;
                       pipeline.GroupLayouts    = {draw.Shader->EngineLayout, draw.Shader->MaterialLayout};
                       pipeline.ColorFormats    = {c.Texture(output)->GetDesc().PixelFormat};
                       pipeline.Rasterizer.Cull = CullMode::None;
                       pipeline.Blends.resize(1);
                       pipeline.Blends[0].WriteMask = 7; // Opaque RGB replace, preserve destination alpha.
                       BindGroupDesc engine;
                       engine.Layout = draw.Shader->EngineLayout;
                       for (const auto& resource : draw.Shader->EngineResources)
                       {
                           BindGroupEntry entry;
                           entry.Slot = resource.Binding;
                           if (resource.Binding == 0)
                               entry.BufferResource = buffer;
                           else
                           {
                               entry.TextureResource = c.Texture(resource.Binding == 11 ? input :
                                                                 resource.Binding == 12 ? depth :
                                                                                          custom);
                               entry.SamplerResource = resource.Binding == 11 ? linear : point;
                               if (resource.Binding == 14)
                               {
                                   entry.Aspect      = TextureAspect::Stencil;
                                   entry.Subresource = TextureSubresource {};
                               }
                           }
                           engine.Entries.push_back(entry);
                       }
                       RenderingDesc render;
                       render.Colors.push_back({c.Texture(output), {}, LoadOp::Load});
                       c.Commands.BeginRendering(render);
                       c.Commands.SetViewport({0, 0, float(frame.View.Viewport.x), float(frame.View.Viewport.y)});
                       c.Commands.SetScissor({0, 0, frame.View.Viewport.x, frame.View.Viewport.y});
                       c.Commands.BindPipeline(frame.Services.CachedPipeline(pipeline));
                       c.Commands.BindGroup(c.Device.CreateBindGroup(engine));
                       c.Commands.BindGroup(draw.Material);
                       c.Commands.Draw(3);
                       c.Commands.EndRendering();
                   });
            if (hdr)
                frame.DeferToneMapping = frame.PostProcessHDR = true;
        }
    }
    void ForwardPipeline::Build(RenderGraph& graph, RenderFrame& renderer)
    {
        PreparePostMaterials(renderer);
        const auto& view = renderer.View;
        using namespace RenderHardware;
        const bool reconstruct = view.GetPostProcessSettings().FilterSize >= 1;
        if (reconstruct)
            renderer.DeferToneMapping = true;
        if (renderer.Samples > 1)
        {
            BuildMSAA(graph, renderer);
            return;
        }
        if (!graph.PassCount())
            Begin(graph, renderer);
        if (renderer.HasBackgroundColor())
        {
            std::vector<GraphUse> background {
                {graph.ImportTexture("SceneHDR", renderer.Output.GetHDR()), GraphAccess::ColorWrite}};
            for (size_t i = 0; i < renderer.Output.GetColors().size(); ++i)
                background.push_back(
                    {graph.ImportTexture("SceneColor" + std::to_string(i), renderer.Output.GetColors()[i]),
                     GraphAccess::ColorReadWrite});
            graph.AddPass("Scene background (linear HDR)", background, [&renderer](auto& context) {
                renderer.ClearBackground(context.Commands);
            });
            renderer.PostProcessHDR = true;
        }
        ParticleRenderFeature::Simulate(graph, renderer);
        BuildShadowPasses(graph, renderer);
        std::vector<GraphUse> target;
        for (size_t i = 0; i < renderer.Output.GetColors().size(); ++i)
            target.push_back({graph.ImportTexture("SceneColor" + std::to_string(i), renderer.Output.GetColors()[i]),
                              GraphAccess::ColorReadWrite});
        const auto depth = graph.ImportTexture("SceneDepthStencil", renderer.Output.GetDepth());
        target.push_back({depth, GraphAccess::DepthReadWrite});
        // Preserve deterministic legacy order, even when two passes have disjoint writes.
        auto append = [&](std::string name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            auto pass = graph.AddPass(std::move(name), std::move(uses), std::move(record));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
            return pass;
        };
        auto sampled =
            [&](std::vector<GraphUse>& uses, const Handle<RenderHardware::Texture>& image, std::string name) {
                if (!image)
                    return;
                auto texture = graph.ImportTexture(std::move(name), image);
                if (std::none_of(uses.begin(), uses.end(), [&](auto use) { return use.Texture == texture; }))
                    uses.push_back({texture, GraphAccess::SampledRead});
            };
        RenderFeatureContext featureContext {graph, view, target, &renderer};
        for (const auto& immediate : view.ImmediateOrder)
        {
            // Single-sample immediate draws use the compatibility display target.
            // A skipped skybox must leave the scene-linear background available.
            if (renderer.HasBackgroundColor() && !reconstruct &&
                (immediate.Kind != ViewImmediateKind::Skybox ||
                 (view.Skyboxes.at(immediate.Index).Texture && view.Environment.ShowSkybox)))
                renderer.PostProcessHDR = false;
            featureContext.Immediate = &immediate;
            Inject(RenderExtensionPoint::Immediate, featureContext);
            auto uses = target;
            if (reconstruct && immediate.Kind != ViewImmediateKind::Skybox && renderer.PostProcessHDR)
            {
                auto resolveUses = target;
                resolveUses.pop_back();
                resolveUses.push_back(
                    {graph.ImportTexture("SceneHDR", renderer.Output.GetHDR()), GraphAccess::SampledRead});
                auto commands = renderer.GetDevice()->CreateCommandList();
                renderer.Services.GetToneMapping().Record(*renderer.GetDevice(),
                                                          *commands,
                                                          renderer.Output.GetHDR(),
                                                          renderer.Output.GetColors(),
                                                          view.GetPostProcessSettings().Exposure,
                                                          false,
                                                          view.GetPostProcessSettings().ToneMapping);
                auto pass = graph.AddRecordedPass("Immediate display prefill", std::move(resolveUses), commands);
                if (pass)
                    graph.OrderAfter(pass, pass - 1);
                renderer.PostProcessHDR = false;
            }
            if (immediate.Kind == ViewImmediateKind::RecordedSprites)
            {
                const auto& batch = view.SpriteBatches.at(immediate.Index);
                sampled(uses, batch.Texture, "SpriteTexture");
                auto pass = graph.AddRecordedPass("Renderer2D batch", std::move(uses), batch.Commands);
                if (pass)
                    graph.OrderAfter(pass, pass - 1);
            }
            else if (immediate.Kind == ViewImmediateKind::Sprites)
            {
                // Flush reports the actual image, including the internal white fallback.
                // Record each original batch once and declare its exact sampled resource.
                auto& sprites = renderer.Services.GetSpriteBatch();
                sprites.ResetStats();
                sprites.SetTarget(renderer.GetDevice(), renderer.SceneTarget(LoadOp::Load));
                sprites.SetSubmissionSink([&](auto commands, auto image) {
                    auto batchUses = target;
                    sampled(batchUses, image, "SpriteTexture");
                    auto pass = graph.AddRecordedPass("Renderer2D sprites",
                                                      std::move(batchUses),
                                                      std::move(commands),
                                                      sprites.GetLastRecordMilliseconds());
                    if (pass)
                        graph.OrderAfter(pass, pass - 1);
                });
                ImmediatePassState spriteState;
                spriteState.DepthCompare = CompareOp::Always;
                sprites.BeginScene(view.Scene.sceneCamera.camera, view.Scene.sceneCamera.viewMatrix, spriteState);
                const auto end = immediate.Count == SIZE_MAX ? view.Sprites.size() : immediate.Index + immediate.Count;
                for (size_t i = immediate.Index; i < end; ++i)
                {
                    const auto& sprite = view.Sprites.at(i);
                    sprites.DrawSprite(sprite.Transform,
                                       sprite.Texture,
                                       sprite.Color,
                                       sprite.TilingFactor,
                                       sprite.UVRect,
                                       sprite.FlipX,
                                       sprite.FlipY);
                }
                sprites.EndScene();
                renderer.SpriteStats.DrawCalls += sprites.GetStats().DrawCalls;
                renderer.SpriteStats.QuadCount += sprites.GetStats().QuadCount;
                sprites.ClearTarget();
            }
            else
            {
                sampled(uses, view.Skyboxes.at(immediate.Index).Texture, "Skybox");
                if (reconstruct)
                {
                    uses.push_back(
                        {graph.ImportTexture("SceneHDR", renderer.Output.GetHDR()), GraphAccess::ColorReadWrite});
                    auto commands = renderer.GetDevice()->CreateCommandList();
                    renderer.DrawSkybox(view.Skyboxes.at(immediate.Index), *commands);
                    auto pass = graph.AddRecordedPass("Skybox (HDR reconstruction input)", std::move(uses), commands);
                    if (pass)
                        graph.OrderAfter(pass, pass - 1);
                }
                else
                    append("Skybox", std::move(uses), [&renderer, &view, immediate](auto& context) {
                        renderer.DrawSkybox(view.Skyboxes.at(immediate.Index), context.Commands);
                    });
            }
        }
        featureContext.Immediate = nullptr;
        Inject(RenderExtensionPoint::BeforeOpaque, featureContext);
        if (!view.MaterialOrder.empty())
        {
            auto uses = target;
            for (const auto& image : renderer.Services.GetUIRenderer().Textures())
                sampled(uses, image, "UITexture");
            for (const auto& image : renderer.Services.GetTextRenderer().Textures())
                sampled(uses, image, "FontAtlas");
            for (const auto& image : renderer.GetMaterialEnvironment())
                sampled(uses, image, "MaterialEnvironment");
            if (view.Shadows)
            {
                sampled(uses, view.Shadows->Directional, "DirectionalShadowAtlas");
                for (const auto& point : view.Shadows->Points)
                    sampled(uses, point, "PointShadowCube");
            }
            // Material groups resolve asynchronous textures while recording. Record once now,
            // declare the exact retained images, then let graph execution submit the immutable list.
            // This keeps the existing per-draw allocation count and avoids resolving groups twice.
            const auto hdr = graph.ImportTexture("SceneHDR", renderer.Output.GetHDR());
            uses.push_back({hdr, GraphAccess::ColorReadWrite});
            const auto          recordStart = RenderClockMilliseconds();
            auto                commands = renderer.GetDevice()->CreateCommandList();
            Handle<CommandList> post;
            renderer.DrawShaderMaterials(
                *commands, [&](const auto& image) { sampled(uses, image, "MaterialTexture"); }, {}, nullptr, &post);
            auto pass = graph.AddRecordedPass("Materials (opaque/mask, blend)",
                                              std::move(uses),
                                              commands,
                                              RenderClockMilliseconds() - recordStart -
                                                  renderer.FinalPostRecordMilliseconds.value_or(0));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
            if (post)
            {
                auto postUses = target;
                postUses.pop_back(); // Fullscreen conversion does not access depth/stencil.
                postUses.push_back({hdr, GraphAccess::SampledRead});
                auto resolve = graph.AddRecordedPass("Tone mapping (ACES fitted + sRGB)",
                                                     std::move(postUses),
                                                     post,
                                                     renderer.FinalPostRecordMilliseconds);
                graph.OrderAfter(resolve, resolve - 1);
            }
        }
        Inject(RenderExtensionPoint::AfterTransparent, featureContext);
        AddReconstructionFilter(graph, renderer);
        if (!m_PostDraws.empty())
            m_CustomDepth = BuildCustomDepthPass(graph, renderer);
        AddPostMaterials(graph, renderer, PostProcessStage::BeforeToneMap);
        Inject(RenderExtensionPoint::BeforePostProcess, featureContext);
        if (renderer.DeferToneMapping && renderer.PostProcessHDR)
        {
            auto uses = target;
            uses.pop_back(); // The final resolve does not access depth/stencil.
            uses.push_back({graph.ImportTexture("SceneHDR", renderer.Output.GetHDR()), GraphAccess::SampledRead});
            append("Tone mapping (ACES fitted + sRGB)", std::move(uses), [&renderer, &view](auto& context) {
                const float exposure = view.GetPostProcessSettings().Exposure;
                renderer.Services.GetToneMapping().Record(context.Device,
                                                          context.Commands,
                                                          renderer.Output.GetHDR(),
                                                          renderer.Output.GetColors(),
                                                          exposure,
                                                          false,
                                                          view.GetPostProcessSettings().ToneMapping);
            });
        }
        // Display-space overlays follow every post effect and the final tone mapping.
        renderer.DeferToneMapping = renderer.PostProcessHDR = false;
        AddPostMaterials(graph, renderer, PostProcessStage::AfterToneMap);
        if (view.EffectiveAA == AntiAliasing::FXAA)
            AddFXAA(graph, renderer);
        AddScreenUI(graph, renderer);
        Inject(RenderExtensionPoint::Overlay, featureContext);
    }

    void ForwardPipeline::AddScreenUI(RenderGraph& graph, RenderFrame& frame)
    {
        using namespace RenderHardware;
        if (std::none_of(frame.View.UIs.begin(), frame.View.UIs.end(), [](auto& ui) { return ui.Screen; }))
            return;
        auto output      = graph.ImportTexture("SceneColor0", frame.Output.GetColors()[0]);
        auto desc        = frame.Output.GetColors()[0]->GetDesc();
        desc.PixelFormat = Format::RGBA8Srgb;
        desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::CopySource |
                           RenderHardware::TextureUsage::CopyDestination;
        auto srgb        = graph.CreateTexture("UI sRGB composite", desc);
        auto append      = [&](const char* name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            auto p = graph.AddPass(name, std::move(uses), std::move(record));
            if (p)
                graph.OrderAfter(p, p - 1);
        };
        // Compatible UNORM/sRGB copies preserve encoded bytes. Hardware blending decodes/encodes sRGB.
        append("UI background copy",
               {{output, GraphAccess::TransferRead}, {srgb, GraphAccess::TransferWrite}},
               [output, srgb](auto& c) { c.Commands.CopyTexture(c.Texture(output), {}, c.Texture(srgb)); });
        std::vector<GraphUse> uses {{srgb, GraphAccess::ColorReadWrite}};
        for (auto& image : frame.Services.GetUIRenderer().Textures())
            uses.push_back({graph.ImportTexture("UI image/atlas", image), GraphAccess::SampledRead});
        append("Screen UI (linear blending, sRGB output)", std::move(uses), [&frame, srgb](auto& c) {
            RenderingDesc target;
            target.Colors.push_back({c.Texture(srgb)});
            target.Colors[0].Load = LoadOp::Load;
            c.Commands.BeginRendering(target);
            auto viewport = frame.View.Viewport;
            c.Commands.SetViewport({0, 0, float(viewport.x), float(viewport.y)});
            glm::mat4 projection(1);
            projection[0][0] = 2.f / viewport.x;
            projection[1][1] = 2.f / viewport.y;
            projection[3][0] = -1;
            projection[3][1] = -1;
            for (auto& ui : frame.View.UIs)
                if (ui.Screen)
                    frame.Services.GetUIRenderer().Draw(c.Commands, ui, projection, target, viewport);
            c.Commands.EndRendering();
        });
        append("UI display copy",
               {{srgb, GraphAccess::TransferRead}, {output, GraphAccess::TransferWrite}},
               [output, srgb](auto& c) { c.Commands.CopyTexture(c.Texture(srgb), {}, c.Texture(output)); });
    }
    void ForwardPipeline::AddReconstructionFilter(RenderGraph& graph, RenderFrame& frame)
    {
        const float radius = frame.View.GetPostProcessSettings().FilterSize;
        if (radius < 1)
            return;
        using namespace RenderHardware;
        const auto hdr    = graph.ImportTexture("SceneHDR", frame.Output.GetHDR());
        auto       append = [&](const char* name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            const auto pass = graph.AddPass(name, std::move(uses), std::move(record));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        if (!frame.PostProcessHDR)
        {
            const auto display = graph.ImportTexture("Reconstruction display input", frame.Output.GetColors()[0]);
            append("Reconstruction HDR prefill",
                   {{display, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
                   [&frame](auto& c) {
                       frame.Services.GetToneMapping().Record(c.Device,
                                                              c.Commands,
                                                              frame.Output.GetColors()[0],
                                                              {frame.Output.GetHDR()},
                                                              frame.View.GetPostProcessSettings().Exposure,
                                                              true,
                                                              frame.View.GetPostProcessSettings().ToneMapping);
                   });
        }
        auto desc        = frame.Output.GetHDR()->GetDesc();
        desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        const auto input = graph.CreateTexture("Reconstruction HDR input", desc);
        append("Reconstruction HDR input copy",
               {{hdr, GraphAccess::TransferRead}, {input, GraphAccess::TransferWrite}},
               [hdr, input](auto& c) { c.Commands.CopyTexture(c.Texture(hdr), {}, c.Texture(input)); });
        append("Radial Blackman-Harris reconstruction",
               {{input, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
               [&frame, input, hdr, radius](auto& c) {
                   frame.Output.GetReconstructionFilter().Record(
                       c.Device, c.Commands, c.Texture(input), c.Texture(hdr), radius);
               });
        frame.DeferToneMapping = frame.PostProcessHDR = true;
    }

    void ForwardPipeline::AddFXAA(RenderGraph& graph, RenderFrame& frame)
    {
        using namespace RenderHardware;
        const auto output = graph.ImportTexture("SceneColor0", frame.Output.GetColors()[0]);
        auto       desc   = frame.Output.GetColors()[0]->GetDesc();
        desc.Usage        = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        const auto input  = graph.CreateTexture("FXAA LDR input", desc);
        auto       copy   = graph.AddPass(
            "FXAA input copy",
            {{output, GraphAccess::TransferRead}, {input, GraphAccess::TransferWrite}},
            [output, input](auto& c) { c.Commands.CopyTexture(c.Texture(output), {}, c.Texture(input)); });
        if (copy)
            graph.OrderAfter(copy, copy - 1);
        auto pass = graph.AddPass("FXAA quality (LDR)",
                                  {{input, GraphAccess::SampledRead}, {output, GraphAccess::ColorWrite}},
                                  [&frame, input, output](auto& c) {
                                      frame.Services.GetAntiAliasing().FXAA(
                                          c.Device, c.Commands, c.Texture(input), c.Texture(output));
                                  });
        graph.OrderAfter(pass, pass - 1);
    }
    void ForwardPipeline::BuildMSAA(RenderGraph& graph, RenderFrame& frame)
    {
        using namespace RenderHardware;
        auto& view       = frame.View;
        frame.SceneGraph = &graph;
        std::vector<GraphUse> scene, resolved;
        for (size_t i = 0; i < frame.Output.GetColors().size(); ++i)
        {
            auto desc     = frame.Output.GetColors()[i]->GetDesc();
            desc.Samples  = frame.Samples;
            const auto id = graph.CreateTexture("SceneColor MSAA " + std::to_string(i), desc);
            frame.MSColors.push_back(id);
            scene.push_back({id, GraphAccess::ColorReadWrite});
            auto out = graph.ImportTexture("SceneColor" + std::to_string(i), frame.Output.GetColors()[i]);
            graph.ExportTexture(out);
            resolved.push_back({out, GraphAccess::ColorReadWrite});
        }
        auto desc    = frame.Output.GetHDR()->GetDesc();
        desc.Samples = frame.Samples;
        frame.MSHDR  = graph.CreateTexture("SceneHDR MSAA", desc);
        scene.push_back({frame.MSHDR, GraphAccess::ColorReadWrite});
        desc          = frame.Output.GetDepth()->GetDesc();
        desc.Samples  = frame.Samples;
        frame.MSDepth = graph.CreateTexture("SceneDepthStencil MSAA", desc);
        scene.push_back({frame.MSDepth, GraphAccess::DepthReadWrite});
        const auto depth = graph.ImportTexture("SceneDepthStencil (sample 0 resolved)", frame.Output.GetDepth());
        graph.ExportTexture(depth);
        resolved.push_back({depth, GraphAccess::DepthReadWrite});
        auto append = [&](std::string name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            const auto pass = graph.AddPass(std::move(name), std::move(uses), std::move(record));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        auto clear = scene;
        for (auto& use : clear)
            use.Access = use.Texture == frame.MSDepth ? GraphAccess::DepthWrite : GraphAccess::ColorWrite;
        append("Clear MSAA", clear, [&frame](auto& c) {
            frame.DeferToneMapping = true;
            frame.PostProcessHDR   = false;
            c.Commands.BeginRendering(frame.SceneTarget(LoadOp::Clear));
            c.Commands.EndRendering();
            RenderingDesc hdr;
            hdr.Colors.push_back({frame.HDR(), {}, LoadOp::Clear});
            c.Commands.BeginRendering(hdr);
            c.Commands.EndRendering();
            if (frame.HasBackgroundColor())
            {
                frame.ClearBackground(c.Commands);
                frame.PostProcessHDR = true;
            }
        });
        ParticleRenderFeature::Simulate(graph, frame);
        BuildShadowPasses(graph, frame);
        RenderFeatureContext features {graph, view, scene, &frame};
        for (const auto& immediate : view.ImmediateOrder)
        {
            features.Immediate = &immediate;
            Inject(RenderExtensionPoint::Immediate, features);
            auto uses = scene;
            if (immediate.Kind == ViewImmediateKind::Skybox)
            {
                const auto& sky = view.Skyboxes.at(immediate.Index);
                if (sky.Texture)
                    uses.push_back({graph.ImportTexture("Skybox", sky.Texture), GraphAccess::SampledRead});
                append("Skybox MSAA", uses, [&frame, immediate](auto& c) {
                    frame.DrawSkybox(frame.View.Skyboxes.at(immediate.Index), c.Commands);
                });
            }
            else if (immediate.Kind == ViewImmediateKind::Sprites)
            {
                const auto end = immediate.Count == SIZE_MAX ? view.Sprites.size() : immediate.Index + immediate.Count;
                for (size_t i = immediate.Index; i < end; ++i)
                    if (view.Sprites[i].Texture)
                        uses.push_back({graph.ImportTexture("SpriteTexture", view.Sprites[i].Texture->GetResource()),
                                        GraphAccess::SampledRead});
                // Deduplicate a shared image used by multiple sprites.
                std::sort(uses.begin(), uses.end(), [](auto a, auto b) { return a.Texture < b.Texture; });
                uses.erase(std::unique(uses.begin(), uses.end(), [](auto a, auto b) { return a.Texture == b.Texture; }),
                           uses.end());
                append("Renderer2D sprites MSAA", uses, [&frame, immediate, end](auto& c) {
                    if (frame.PostProcessHDR)
                    {
                        frame.Services.GetToneMapping().Record(c.Device,
                                                               c.Commands,
                                                               frame.HDR(),
                                                               frame.Colors(),
                                                               frame.View.GetPostProcessSettings().Exposure,
                                                               false,
                                                               frame.View.GetPostProcessSettings().ToneMapping);
                        frame.PostProcessHDR = false;
                    }
                    auto& sprites = frame.Services.GetSpriteBatch();
                    sprites.ResetStats();
                    sprites.SetTarget(frame.GetDevice(), frame.SceneTarget(LoadOp::Load));
                    sprites.SetCommandList(&c.Commands);
                    ImmediatePassState spriteState;
                    spriteState.DepthCompare = CompareOp::Always;
                    sprites.BeginScene(
                        frame.View.Scene.sceneCamera.camera, frame.View.Scene.sceneCamera.viewMatrix, spriteState);
                    for (size_t i = immediate.Index; i < end; ++i)
                    {
                        const auto& s = frame.View.Sprites[i];
                        sprites.DrawSprite(s.Transform, s.Texture, s.Color, s.TilingFactor, s.UVRect, s.FlipX, s.FlipY);
                    }
                    sprites.EndScene();
                    frame.SpriteStats.DrawCalls += sprites.GetStats().DrawCalls;
                    frame.SpriteStats.QuadCount += sprites.GetStats().QuadCount;
                    sprites.ClearTarget();
                });
            }
            else
            {
                throw std::invalid_argument(
                    "MSAA views require sprite submissions, not pre-recorded single-sample batches");
            }
        }
        features.Immediate = nullptr;
        Inject(RenderExtensionPoint::BeforeOpaque, features);
        if (!view.MaterialOrder.empty())
        {
            auto uses    = scene;
            auto sampled = [&](const auto& image) {
                if (!image)
                    return;
                const auto id = graph.ImportTexture("MaterialTexture", image);
                if (std::none_of(uses.begin(), uses.end(), [&](auto u) { return u.Texture == id; }))
                    uses.push_back({id, GraphAccess::SampledRead});
            };
            for (const auto& image : frame.Services.GetUIRenderer().Textures())
                sampled(image);
            for (const auto& image : frame.Services.GetTextRenderer().Textures())
                sampled(image);
            for (const auto& image : frame.GetMaterialEnvironment())
                sampled(image);
            if (view.Shadows)
            {
                sampled(view.Shadows->Directional);
                for (const auto& image : view.Shadows->Points)
                    sampled(image);
            }
            for (const auto& draw : view.Draws)
                if (const auto* bindings = frame.GetBindings(draw.Material, draw.ViewMode != SceneViewMode::Lit))
                    for (const auto& image : bindings->Images)
                        sampled(image);
            append("Materials MSAA (opaque/mask, blend)", uses, [&frame](auto& c) {
                frame.DrawShaderMaterials(c.Commands);
            });
        }
        Inject(RenderExtensionPoint::AfterTransparent, features);
        const auto hdr         = graph.ImportTexture("SceneHDR (Karis resolved)", frame.Output.GetHDR());
        auto       resolveUses = scene;
        for (auto& use : resolveUses)
            use.Access = use.Texture == frame.MSDepth ? GraphAccess::DepthReadWrite :
                         use.Texture == frame.MSHDR   ? GraphAccess::ColorReadWrite :
                                                        GraphAccess::SampledRead;
        for (auto use : resolved)
        {
            use.Access = use.Texture == depth ? GraphAccess::DepthWrite : GraphAccess::ColorWrite;
            resolveUses.push_back(use);
        }
        resolveUses.push_back({hdr, GraphAccess::ColorWrite});
        append("MSAA Karis HDR + sample 0 depth/stencil resolve", resolveUses, [&frame](auto& c) {
            const float exposure = frame.View.GetPostProcessSettings().Exposure;
            if (!frame.PostProcessHDR)
                frame.Services.GetToneMapping().Record(c.Device,
                                                       c.Commands,
                                                       frame.Colors()[0],
                                                       {frame.HDR()},
                                                       exposure,
                                                       true,
                                                       frame.View.GetPostProcessSettings().ToneMapping);
            frame.Services.GetAntiAliasing().Resolve(c.Device, c.Commands, frame.HDR(), frame.Output.GetHDR());
            // Compatibility color outputs and depth/stencil use attachment resolves.
            auto render = frame.SceneTarget(LoadOp::Load);
            for (size_t i = 0; i < render.Colors.size(); ++i)
                render.Colors[i].ResolveTarget = frame.Output.GetColors()[i];
            render.Depth.ResolveTarget = frame.Output.GetDepth();
            c.Commands.BeginRendering(render);
            c.Commands.EndRendering();
            frame.MSAAResolved   = true;
            frame.PostProcessHDR = true;
        });
        RenderFeatureContext postFeatures {graph, view, resolved, &frame};
        // Bloom consumes the single-sample HDR resolve. Its build-time contract is HDR input.
        frame.PostProcessHDR = true;
        AddReconstructionFilter(graph, frame);
        if (!m_PostDraws.empty())
            m_CustomDepth = BuildCustomDepthPass(graph, frame);
        AddPostMaterials(graph, frame, PostProcessStage::BeforeToneMap);
        Inject(RenderExtensionPoint::BeforePostProcess, postFeatures);
        auto toneUses = resolved;
        toneUses.pop_back();
        toneUses.push_back({hdr, GraphAccess::SampledRead});
        append("Tone mapping (ACES fitted + sRGB)", toneUses, [&frame](auto& c) {
            frame.Services.GetToneMapping().Record(c.Device,
                                                   c.Commands,
                                                   frame.Output.GetHDR(),
                                                   frame.Output.GetColors(),
                                                   frame.View.GetPostProcessSettings().Exposure,
                                                   false,
                                                   frame.View.GetPostProcessSettings().ToneMapping);
            frame.DeferToneMapping = frame.PostProcessHDR = false;
        });
        AddPostMaterials(graph, frame, PostProcessStage::AfterToneMap);
        AddScreenUI(graph, frame);
        Inject(RenderExtensionPoint::Overlay, postFeatures);
    }
} // namespace Kans
