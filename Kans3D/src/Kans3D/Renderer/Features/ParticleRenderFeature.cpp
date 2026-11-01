#include "kspch.h"
#include "ParticleRenderFeature.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Resource/InternalShader.h"
#include "Kans3D/Renderer/Resource/MaterialPass.h"
#include "Kans3D/Renderer/Resource/ParticleSprite.h"
#include "Kans3D/VFX/VfxGpu.h"
namespace Kans
{
    void ParticleRenderFeature::Prepare(RenderView&, RenderFrame& frame)
    {
        auto world = frame.World.GetVfxWorld();
        if (!world || world->Instances().empty())
            return;
        frame.Output.EnsureSampledDepth();
    }
    void ParticleRenderFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        using namespace RenderHardware;
        auto frame = context.Frame;
        if (!frame)
            return;
        auto world = frame->World.GetVfxWorld();
        if (!world || world->Instances().empty())
            return;
        auto& graph = context.Graph;
        if (point == RenderExtensionPoint::BeforeOpaque)
        {
            for (const auto& [id, instance] : world->Instances())
            {
                Vfx::GpuEmitter::AddSystemPasses(graph, *instance, *frame->GetDevice());
                for (const auto& e : instance->Emitters())
                    frame->Stats.SimulatedParticles += e.ParticleCount;
            }
            return;
        }
        if (point != RenderExtensionPoint::AfterTransparent || !frame->Assets())
            return;
        struct Draw
        {
            Vfx::SystemInstance*  System;
            Vfx::EmitterInstance* Emitter;
            const Vfx::Renderer*  Renderer;
            float                 Distance;
        };
        std::vector<Draw> draws;
        const auto        camera = context.View.Scene.sceneCamera;
        const auto        projection =
            DeviceProjection(camera.camera.GetProjectionMatrix(), frame->GetDevice()->GetCoordinateConventions());
        Frustum frustum(projection * camera.viewMatrix);
        for (const auto& [id, instance] : world->Instances())
            for (auto& e : instance->Emitters())
            {
                if (!e.GPU || e.HiddenByDistance)
                    continue;
                const auto& settings = e.Compiled->Settings;
                Bounds      bounds   = settings.FixedBounds.value_or(e.DynamicBounds);
                if (settings.SimulationSpace == Vfx::Space::Local)
                    bounds = bounds.Transformed(instance->OwnerTransform());
                if (!settings.FixedBounds && !instance->FixedBounds && bounds.IsValid())
                {
                    bounds.Min -= glm::vec3(e.SpriteRadius);
                    bounds.Max += glm::vec3(e.SpriteRadius);
                }
                if (instance->FixedBounds)
                    bounds = instance->FixedBounds->Transformed(instance->OwnerTransform());
                if (bounds.IsValid() && !frustum.Intersects(bounds))
                    continue;
                float distance = glm::distance(
                    camera.Position, bounds.IsValid() ? bounds.Center() : glm::vec3(instance->OwnerTransform()[3]));
                for (const auto& renderer : settings.Renderers)
                    draws.push_back({instance.get(), &e, &renderer, distance});
            }
        if (draws.empty())
            return;
        std::stable_sort(
            draws.begin(), draws.end(), [](const auto& a, const auto& b) { return a.Distance > b.Distance; });
        auto sourceDepth = graph.ImportTexture("VFX scene depth", frame->Output.GetDepth());
        if (frame->Samples > 1 && !frame->MSAAResolved)
        {
            auto msDepth = frame->MSDepth;
            graph.AddPass("VFX resolve scene depth",
                          {{msDepth, GraphAccess::DepthReadWrite}, {sourceDepth, GraphAccess::DepthWrite}},
                          [msDepth, sourceDepth](auto& c) {
                              RenderingDesc resolve;
                              resolve.Depth.Target        = c.Texture(msDepth);
                              resolve.Depth.Load          = LoadOp::Load;
                              resolve.Depth.Store         = StoreOp::Store;
                              resolve.Depth.ResolveTarget = c.Texture(sourceDepth);
                              c.Commands.BeginRendering(resolve);
                              c.Commands.EndRendering();
                          });
        }
        auto desc  = frame->Output.GetDepth()->GetDesc();
        desc.Usage = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
        auto depth = graph.CreateTexture("VFX soft particle depth", desc);
        graph.AddPass(
            "VFX depth snapshot",
            GraphPassKind::Transfer,
            {{sourceDepth, GraphAccess::TransferRead}, {depth, GraphAccess::TransferWrite}},
            [sourceDepth, depth](auto& c) { c.Commands.CopyTexture(c.Texture(sourceDepth), {}, c.Texture(depth)); });
        bool workingHDR = frame->PostProcessHDR;
        for (const auto& draw : draws)
        {
            const auto& r        = *draw.Renderer;
            auto        bindings = frame->GetBindings(r.Material, false);
            if (!bindings)
                continue;
            auto shader = frame->GetShader(r.Material, bindings->Shader);
            if (!shader || !shader->PrepareParticleSprite())
                continue;
            auto                   gpu = draw.Emitter->GPU.get();
            ParticleSpriteUniforms uniforms;
            uniforms.Owner             = draw.System->OwnerTransform();
            uniforms.View              = camera.viewMatrix;
            uniforms.Projection        = projection;
            uniforms.InverseView       = glm::inverse(camera.viewMatrix);
            uniforms.InverseProjection = glm::inverse(projection);
            const auto offset          = [&](const char* binding) {
                auto name = r.Bindings.at(binding);
                for (const auto& a : draw.Emitter->Compiled->Attributes)
                    if (a.Name == name)
                        return a.Offset;
                throw std::invalid_argument("Particle renderer binding has no attribute: " + name);
            };
            uniforms.Attributes0 = {
                offset("Position"), offset("Color"), offset("SpriteSize"), offset("SpriteRotation")};
            uniforms.Attributes1 = {offset("Velocity"),
                                    offset("SubImageIndex"),
                                    offset("DynamicMaterialParameter"),
                                    draw.Emitter->Compiled->Settings.Capacity};
            uniforms.PivotGrid   = {r.Pivot.x, r.Pivot.y, float(r.Columns), float(r.Rows)};
            uniforms.Facing =
                glm::vec4(r.FacingVector, float(bindings->Render.AlphaMode == MaterialAlphaMode::Premultiplied));
            uniforms.Alignment     = glm::vec4(r.AlignmentVector, 0);
            uniforms.Options       = {float(r.FacingMode),
                                      r.DepthFadeDistance,
                                      float(r.FrameBlending),
                                      float(draw.Emitter->Compiled->Settings.SimulationSpace == Vfx::Space::Local)};
            EngineFrameUniforms ef = context.View.LightingUniforms.value_or(MakeLightingUniforms(context.View.Scene));
            ef.View                = camera.viewMatrix;
            ef.Projection          = projection;
            ef.ViewProjection      = projection * camera.viewMatrix;
            ef.InverseView         = uniforms.InverseView;
            ef.InverseProjection   = uniforms.InverseProjection;
            ef.CameraPosition      = glm::vec4(camera.Position, 1);
            ef.Time                = context.View.Time;
            ef.LightingInfo.y      = context.View.GetPostProcessSettings().Exposure;
            float environmentAngle = glm::radians(context.View.Environment.Rotation);
            ef.EnvironmentInfo     = {context.View.Environment.DiffuseIntensity,
                                      context.View.Environment.SpecularIntensity,
                                      std::cos(environmentAngle),
                                      std::sin(environmentAngle)};
            EngineObjectUniforms object;
            object.MaterialRender = {float(bindings->Render.AlphaMode),
                                     bindings->Render.AlphaCutoff,
                                     float(bindings->Render.DoubleSided),
                                     bindings->Render.ReceiveShadows ? 0.f : 1.f};
            const auto upload     = [&](const void* data, size_t bytes) {
                auto b = frame->GetDevice()->CreateBuffer({bytes, BufferUsage::Uniform, MemoryUsage::Upload});
                b->Write(0, data, bytes);
                return b;
            };
            MaterialPassData pass;
            pass.Frame              = upload(&ef, sizeof(ef));
            pass.Object             = upload(&object, sizeof(object));
            auto sprite             = upload(&uniforms, sizeof(uniforms));
            pass.Render             = bindings->Render;
            pass.VertexSource       = shader->SpriteProgram;
            pass.VertexLayout       = shader->SpriteLayout;
            pass.IndirectArguments  = gpu->DrawArgs;
            pass.Environment        = frame->GetMaterialEnvironment();
            pass.EnvironmentSampler = frame->Services.GetMaterialEnvironmentSampler();
            pass.Samples            = frame->Samples;
            pass.Shadows            = context.View.Shadows;
            pass.ColorFormats.clear();
            const bool materialHDR = shader->OutputSpace == ShaderOutputSpace::LinearHDR;
            const bool previousHDR = workingHDR;
            auto       target      = frame->Output.Target(LoadOp::Load, materialHDR);
            for (const auto& color : target.Colors)
                pass.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
            auto       uses = context.Target;
            const auto use  = [&](GraphUse entry) {
                auto at = std::find_if(uses.begin(), uses.end(), [&](const auto& existing) {
                    return existing.IsBuffer == entry.IsBuffer && existing.Texture == entry.Texture;
                });
                if (at == uses.end())
                    uses.push_back(entry);
                else if (entry.Access == GraphAccess::ColorReadWrite || entry.Access == GraphAccess::DepthReadWrite)
                    at->Access = entry.Access;
            };

            auto hdr = frame->Samples > 1 && !frame->MSAAResolved ?
                           frame->MSHDR :
                           graph.ImportTexture("SceneHDR", frame->Output.GetHDR());
            use({hdr, GraphAccess::ColorReadWrite});
            use({graph.ImportBuffer("VFX attributes", gpu->Attributes), GraphAccess::StorageRead});
            use({graph.ImportBuffer("VFX alive", gpu->Alive), GraphAccess::StorageRead});
            use({graph.ImportBuffer("VFX draw indirect", gpu->DrawArgs), GraphAccess::IndirectRead});
            use({depth, GraphAccess::SampledRead});
            auto material = frame->Assets()->GetMaterialGroup(*shader, *bindings);
            if (!material)
                continue;
            pass.MaterialGroup = material;
            for (auto image : bindings->Images)
                use({graph.ImportTexture("VFX material texture", image), GraphAccess::SampledRead});
            for (auto image : pass.Environment)
                if (image)
                    use({graph.ImportTexture("VFX environment", image), GraphAccess::SampledRead});
            auto a = gpu->Attributes, al = gpu->Alive;
            auto sampler = frame->GetDevice()->CreateSampler({Filter::Nearest,
                                                              Filter::Nearest,
                                                              Filter::Nearest,
                                                              AddressMode::ClampToEdge,
                                                              AddressMode::ClampToEdge});
            graph.AddPass("VFX sprite " + draw.Emitter->Compiled->Settings.Name,
                          std::move(uses),
                          [frame, shader, bindings, pass, sprite, a, al, sampler, depth, materialHDR, previousHDR](
                              auto& c) mutable {
                              BindGroupDesc group;
                              group.Layout  = pass.VertexLayout;
                              group.Entries = {{0, a, 0, a->GetDesc().Bytes},
                                               {1, al, 0, al->GetDesc().Bytes},
                                               {2, sprite, 0, sizeof(ParticleSpriteUniforms)}};
                              BindGroupEntry sceneDepth;
                              sceneDepth.Slot            = 3;
                              sceneDepth.TextureResource = c.Texture(depth);
                              sceneDepth.SamplerResource = sampler;
                              group.Entries.push_back(sceneDepth);
                              pass.VertexGroup = c.Device.CreateBindGroup(group);
                              const auto post  = frame->View.GetPostProcessSettings();
                              const auto tone  = [&](bool inverse) {
                                  frame->Services.GetToneMapping().Record(
                                      c.Device,
                                      c.Commands,
                                      inverse ? frame->Colors()[0] : frame->HDR(),
                                      inverse ? std::vector<Handle<RenderHardware::Texture>> {frame->HDR()} :
                                                frame->Colors(),
                                      post.Exposure,
                                      inverse,
                                      post.ToneMapping);
                              };
                              bool inputHDR = frame->Samples > 1 ? frame->PostProcessHDR : previousHDR;
                              if (!inputHDR)
                                  tone(true);
                              if (!materialHDR)
                                  tone(false);
                              c.Commands.BeginRendering(frame->SceneTarget(LoadOp::Load, materialHDR));
                              GpuMesh empty;
                              bool    recorded = false;
                              RecordMaterialDraw(
                                  *frame->Assets(), c.Commands, *shader, *bindings, empty, pass, nullptr, &recorded);
                              c.Commands.EndRendering();
                              if (!materialHDR)
                                  tone(true);
                              frame->PostProcessHDR = true;
                              if (!frame->DeferToneMapping)
                                  tone(false);
                              if (recorded)
                              {
                                  ++frame->Stats.DrawCalls;
                                  ++frame->Stats.ParticleDraws;
                              }
                          });
            workingHDR            = true;
            frame->PostProcessHDR = true;
        }
    }
} // namespace Kans
