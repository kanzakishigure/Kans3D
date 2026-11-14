#include "kspch.h"
#include "ParticleRenderFeature.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"
#include "Kans3D/Renderer/Particles/ParticleSprite.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
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
    void ParticleRenderFeature::Simulate(RenderGraph& graph, RenderFrame& frame)
    {
        auto world = frame.World.GetVfxWorld();
        if (!world)
            return;
        for (const auto& [id, instance] : world->Instances())
        {
            instance->Depth = world->Depth;
            Vfx::GpuEmitter::AddSystemPasses(graph, *instance, *frame.GetDevice());
            for (const auto& e : instance->Emitters())
                frame.Stats.SimulatedParticles += e.ParticleCount;
        }
    }
    void ParticleRenderFeature::RecordMeshShadows(RenderGraph&                 graph,
                                                  RenderFrame&                 frame,
                                                  RenderHardware::CommandList& commands,
                                                  const EngineFrameUniforms&   lighting,
                                                  const ShaderData&            depthSource,
                                                  std::vector<GraphUse>&       uses,
                                                  bool                         point,
                                                  float                        constantBias,
                                                  float                        slopeBias)
    {
        using namespace RenderHardware;
        auto world = frame.World.GetVfxWorld();
        if (!world || !frame.Assets())
            return;
        auto                           device = frame.GetDevice();
        Handle<RenderHardware::Buffer> frameBuffer;
        const auto                     use = [&](GraphUse u) {
            if (std::none_of(uses.begin(), uses.end(), [&](const auto& v) {
                    return u.IsBuffer == v.IsBuffer && u.Texture == v.Texture;
                }))
                uses.push_back(u);
        };
        for (const auto& [id, instance] : world->Instances())
            for (auto& e : instance->Emitters())
            {
                if (frame.View.HiddenHierarchy.count(id) || !e.Failure.empty() || !e.GPU || e.HiddenByDistance)
                    continue;
                const auto first = graph.PassCount();
                try
                {
                    auto gpu = e.GPU.get();
                    for (const auto& renderer : e.Compiled->Settings.Renderers)
                    {
                        if ((renderer.Kind != Vfx::RendererKind::Mesh && renderer.Kind != Vfx::RendererKind::Ribbon) ||
                            !renderer.CastsShadows())
                            continue;
                        const auto offset = [&](const char* name) {
                            for (const auto& a : e.Compiled->Attributes)
                                if (a.Name == renderer.Bindings.at(name))
                                    return a.Offset;
                            return 0u;
                        };
                        if (renderer.Kind == Vfx::RendererKind::Ribbon)
                        {
                            auto bindings = frame.GetBindings(renderer.Material, false);
                            auto shader   = bindings ? frame.GetShader(renderer.Material, bindings->Shader) : nullptr;
                            if (!bindings || !shader || IsTransparentMaterial(bindings->Render.AlphaMode))
                                continue;
                            const GpuShaderPass* caster    = nullptr;
                            bool                 hasCaster = false;
                            for (const auto& pass : shader->Passes)
                                if (pass.Tag == "ShadowCaster")
                                {
                                    hasCaster = true;
                                    if (bindings->PassEnabled(pass.Name))
                                    {
                                        caster = &pass;
                                        break;
                                    }
                                }
                            if (hasCaster && !caster)
                                continue;
                            if (!caster)
                            {
                                bindings = frame.GetBindings(renderer.Material, true);
                                shader   = bindings ? frame.GetShader(renderer.Material, bindings->Shader) : nullptr;
                            }
                            if (!bindings || !shader)
                                continue;
                            if (!shader->RibbonDepthProgram)
                            {
                                auto source = depthSource;
                                if (caster && shader->Source())
                                    for (const auto& pass : shader->Source()->Passes)
                                        if (pass.Name == caster->Name)
                                            source.Code = pass.Code;
                                source.Passes.clear();
                                auto converted = MakeParticleSpriteShader(source, false, true);
                                if (auto error = std::get_if<AssetError>(&converted))
                                    throw std::invalid_argument(error->Message);
                                shader->RibbonDepthProgram = device->CreateShader(std::get<ShaderData>(converted));
                            }
                            auto                   ribbon = gpu->BuildRibbon(graph,
                                                                             e,
                                                                             renderer,
                                                                             frame.View.Scene.sceneCamera.viewMatrix,
                                                                             instance->OwnerTransform());
                            ParticleSpriteUniforms uniform;
                            EngineObjectUniforms   object;
                            object.MaterialRender     = {float(bindings->Render.AlphaMode),
                                                         bindings->Render.AlphaCutoff,
                                                         float(bindings->Render.DoubleSided),
                                                         0};
                            auto             uniforms = gpu->UploadUniform(graph, &uniform, sizeof(uniform));
                            MaterialPassData pass;
                            pass.DebugName         = "VFX/" + instance->Asset()->assetID.ToString() + "/" +
                                                     e.Compiled->Settings.Name + "/Shadow";
                            pass.Frame             = gpu->UploadUniform(graph, &lighting, sizeof(lighting));
                            pass.Object            = gpu->UploadUniform(graph, &object, sizeof(object));
                            pass.Render            = bindings->Render;
                            pass.VertexSource      = shader->RibbonDepthProgram;
                            pass.VertexLayout      = ParticleSpriteLayout(*device);
                            pass.IndirectArguments = ribbon.Arguments;
                            pass.ShadowDepth       = true;
                            pass.Shadows           = frame.Assets()->GetShadowFallback();
                            pass.ColorFormats.clear();
                            pass.DepthFront         = point ? FrontFace::Clockwise : FrontFace::CounterClockwise;
                            pass.ConstantBias       = constantBias;
                            pass.SlopeBias          = slopeBias;
                            pass.Environment        = frame.GetMaterialEnvironment();
                            pass.EnvironmentSampler = frame.Services.GetMaterialEnvironmentSampler();
                            BindGroupDesc group;
                            group.Layout  = pass.VertexLayout;
                            group.Entries = {{0, ribbon.Vertices, 0, ribbon.Vertices->GetDesc().Bytes},
                                             {1, gpu->Alive, 0, gpu->Alive->GetDesc().Bytes},
                                             {2, uniforms, 0, sizeof(uniform)}};
                            BindGroupEntry image;
                            image.Slot            = 3;
                            image.TextureResource = pass.Shadows->Directional;
                            image.SamplerResource = pass.Shadows->Raw;
                            group.Entries.push_back(image);
                            pass.VertexGroup = device->CreateBindGroup(group);
                            use({graph.ImportBuffer("VFX ribbon shadow vertices", ribbon.Vertices),
                                 GraphAccess::StorageRead});
                            use({graph.ImportBuffer("VFX ribbon shadow arguments", ribbon.Arguments),
                                 GraphAccess::IndirectRead});
                            for (auto u : {pass.Frame, pass.Object, uniforms})
                                use({graph.ImportBuffer("VFX ribbon shadow uniform", u), GraphAccess::UniformRead});
                            for (auto image : bindings->Images)
                                use({graph.ImportTexture("VFX ribbon shadow material", image),
                                     GraphAccess::SampledRead});
                            use({graph.ImportTexture("VFX ribbon shadow fallback", pass.Shadows->Directional),
                                 GraphAccess::SampledRead});
                            GpuMesh empty;
                            bool    recorded = false;
                            RecordMaterialDraw(
                                *frame.Assets(), commands, *shader, *bindings, empty, pass, caster, &recorded);
                            if (recorded)
                            {
                                ++frame.Stats.DrawCalls;
                                ++frame.View.ShadowStats.DrawCalls;
                            }
                            continue;
                        }
                        if (!frameBuffer)
                            frameBuffer = gpu->UploadUniform(graph, &lighting, sizeof(lighting));
                        for (uint32_t entry = 0; entry < renderer.Meshes.size(); ++entry)
                        {
                            const auto& item = renderer.Meshes[entry];
                            auto        mesh = frame.GetMesh(item.Mesh);
                            if (!mesh)
                                continue;
                            auto bin = gpu->BinMesh(graph, renderer, entry, mesh->Sections, gpu->Alive);
                            for (uint32_t section = 0; section < mesh->Sections.size(); ++section)
                            {
                                auto    slot     = mesh->Sections[section].MaterialSlot;
                                auto    override = item.MaterialOverrides.find(slot);
                                AssetID material =
                                    override != item.MaterialOverrides.end() ? override->second :
                                    slot < mesh->MaterialSlots.size() ? mesh->MaterialSlots[slot].DefaultMaterial :
                                                                        mesh->Material;
                                auto bindings = frame.GetBindings(material, false);
                                auto shader   = bindings ? frame.GetShader(material, bindings->Shader) : nullptr;
                                if (!bindings || !shader || IsTransparentMaterial(bindings->Render.AlphaMode))
                                    continue;
                                const GpuShaderPass* caster    = nullptr;
                                bool                 hasCaster = false;
                                for (const auto& pass : shader->Passes)
                                    if (pass.Tag == "ShadowCaster")
                                    {
                                        hasCaster = true;
                                        if (bindings->PassEnabled(pass.Name))
                                        {
                                            caster = &pass;
                                            break;
                                        }
                                    }
                                if (hasCaster && !caster)
                                    continue;
                                if (!caster)
                                {
                                    bindings = frame.GetBindings(material, true);
                                    shader   = bindings ? frame.GetShader(material, bindings->Shader) : nullptr;
                                }
                                if (!bindings || !shader)
                                    continue;
                                if (!shader->MeshDepthProgram)
                                {
                                    auto source = depthSource;
                                    if (caster && shader->Source())
                                        for (const auto& pass : shader->Source()->Passes)
                                            if (pass.Name == caster->Name)
                                                source.Code = pass.Code;
                                    source.Passes.clear();
                                    auto converted = MakeParticleSpriteShader(source, true);
                                    if (auto error = std::get_if<AssetError>(&converted))
                                        throw std::invalid_argument(error->Message);
                                    shader->MeshDepthProgram = device->CreateShader(std::get<ShaderData>(converted));
                                }
                                ParticleSpriteUniforms uniform;
                                uniform.Owner       = instance->OwnerTransform();
                                uniform.InverseView = glm::inverse(frame.View.Scene.sceneCamera.viewMatrix);
                                uniform.Attributes0 = {
                                    offset("Position"), offset("Color"), offset("Scale"), offset("MeshOrientation")};
                                uniform.Attributes1 = {offset("Velocity"),
                                                       0,
                                                       offset("DynamicMaterialParameter"),
                                                       e.Compiled->Settings.Capacity};
                                uniform.MeshScale   = glm::vec4(item.Scale, 0);
                                uniform.MeshPivot   = glm::vec4(item.PivotOffset, 0);
                                uniform.LockedAxis  = glm::vec4(renderer.LockedAxis, float(renderer.LockedAxisEnable));
                                uniform.Options = {float(renderer.FacingMode),
                                                   0,
                                                   0,
                                                   float(e.Compiled->Settings.SimulationSpace == Vfx::Space::Local)};
                                auto                 uniforms = gpu->UploadUniform(graph, &uniform, sizeof(uniform));
                                EngineObjectUniforms object;
                                object.MaterialRender = {float(bindings->Render.AlphaMode),
                                                         bindings->Render.AlphaCutoff,
                                                         float(bindings->Render.DoubleSided),
                                                         0};
                                MaterialPassData pass;
                                pass.DebugName         = "VFX/" + instance->Asset()->assetID.ToString() + "/" +
                                                         e.Compiled->Settings.Name + "/Shadow";
                                pass.Frame             = frameBuffer;
                                pass.Object            = gpu->UploadUniform(graph, &object, sizeof(object));
                                pass.Render            = bindings->Render;
                                pass.ParticleMesh      = true;
                                pass.VertexSource      = shader->MeshDepthProgram;
                                pass.VertexLayout      = ParticleSpriteLayout(*device);
                                pass.IndirectArguments = bin.Arguments;
                                pass.IndirectOffset    = section * 20;
                                pass.Section           = section;
                                pass.ShadowDepth       = true;
                                pass.Shadows           = frame.Assets()->GetShadowFallback();
                                pass.ColorFormats.clear();
                                pass.DepthFront         = point ? FrontFace::Clockwise : FrontFace::CounterClockwise;
                                pass.ConstantBias       = constantBias;
                                pass.SlopeBias          = slopeBias;
                                pass.Environment        = frame.GetMaterialEnvironment();
                                pass.EnvironmentSampler = frame.Services.GetMaterialEnvironmentSampler();
                                BindGroupDesc group;
                                group.Layout  = pass.VertexLayout;
                                group.Entries = {{0, gpu->Attributes, 0, gpu->Attributes->GetDesc().Bytes},
                                                 {1, bin.Indices, 0, bin.Indices->GetDesc().Bytes},
                                                 {2, uniforms, 0, sizeof(uniform)}};
                                BindGroupEntry image;
                                image.Slot            = 3;
                                image.TextureResource = pass.Shadows->Directional;
                                image.SamplerResource = pass.Shadows->Raw;
                                group.Entries.push_back(image);
                                pass.VertexGroup = device->CreateBindGroup(group);
                                for (auto u : {pass.Frame, pass.Object, uniforms})
                                    use({graph.ImportBuffer("VFX mesh shadow uniform", u), GraphAccess::UniformRead});
                                use({graph.ImportBuffer("VFX shadow attributes", gpu->Attributes),
                                     GraphAccess::StorageRead});
                                use({graph.ImportBuffer("VFX shadow alive", bin.Indices), GraphAccess::StorageRead});
                                use({graph.ImportBuffer("VFX shadow arguments", bin.Arguments),
                                     GraphAccess::IndirectRead});
                                use({graph.ImportBuffer("VFX shadow vertices", mesh->Vertices),
                                     GraphAccess::VertexRead});
                                use({graph.ImportBuffer("VFX shadow indices", mesh->Indices), GraphAccess::IndexRead});
                                for (auto image : bindings->Images)
                                    use({graph.ImportTexture("VFX shadow material", image), GraphAccess::SampledRead});
                                use({graph.ImportTexture("VFX shadow fallback", pass.Shadows->Directional),
                                     GraphAccess::SampledRead});
                                bool recorded = false;
                                RecordMaterialDraw(
                                    *frame.Assets(), commands, *shader, *bindings, *mesh, pass, caster, &recorded);
                                if (recorded)
                                {
                                    ++frame.Stats.DrawCalls;
                                    ++frame.View.ShadowStats.DrawCalls;
                                }
                            }
                        }
                    }
                }
                catch (const std::exception& error)
                {
                    e.Fail(error.what());
                }
                graph.GuardPipelineFailures(
                    first, [&e](const auto& error) { e.Fail(error); }, [&e] { return !e.Failure.empty(); });
            }
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
        if ((point != RenderExtensionPoint::BeforeOpaque && point != RenderExtensionPoint::AfterTransparent) ||
            !frame->Assets())
            return;
        struct Draw
        {
            Vfx::SystemInstance*  System;
            Vfx::EmitterInstance* Emitter;
            const Vfx::Renderer*  Renderer;
            float                 Distance;
            const GpuMesh*        Mesh  = nullptr;
            uint32_t              Entry = 0, Section = 0;
            AssetID               Material;
        };
        std::vector<Draw> draws;
        bool              needsCollisionDepth = false;
        const auto        camera              = context.View.Scene.sceneCamera;
        const auto        projection          = frame->SceneProjection();
        Frustum frustum(projection * camera.viewMatrix);
        for (const auto& [id, instance] : world->Instances())
            for (auto& e : instance->Emitters())
            {
                if (context.View.HiddenHierarchy.count(id) || !e.Failure.empty() || !e.GPU)
                    continue;
                if (point == RenderExtensionPoint::AfterTransparent &&
                    e.Compiled->Settings.Target == Vfx::SimTarget::GPU)
                    for (const auto& [stage, program] : e.Compiled->Programs)
                        needsCollisionDepth |= Vfx::UsesDepthCollision(program);
                if (e.HiddenByDistance)
                    continue;
                const auto& settings = e.Compiled->Settings;
                Bounds      bounds   = settings.FixedBounds.value_or(e.DynamicBounds);
                if (settings.SimulationSpace == Vfx::Space::Local)
                    bounds = bounds.Transformed(instance->OwnerTransform());
                if (!settings.FixedBounds && !instance->FixedBounds && bounds.IsValid())
                {
                    float radius = e.SpriteRadius;
                    if (settings.SimulationSpace == Vfx::Space::Local)
                    {
                        auto owner = instance->OwnerTransform();
                        radius *= std::max({glm::length(glm::vec3(owner[0])),
                                            glm::length(glm::vec3(owner[1])),
                                            glm::length(glm::vec3(owner[2]))});
                    }
                    bounds.Min -= glm::vec3(radius);
                    bounds.Max += glm::vec3(radius);
                }
                if (instance->FixedBounds)
                    bounds = instance->FixedBounds->Transformed(instance->OwnerTransform());
                if (bounds.IsValid() && !frustum.Intersects(bounds))
                    continue;
                float distance = glm::distance(
                    camera.Position, bounds.IsValid() ? bounds.Center() : glm::vec3(instance->OwnerTransform()[3]));
                for (const auto& renderer : settings.Renderers)
                {
                    if (renderer.Kind == Vfx::RendererKind::Sprite)
                    {
                        if (point == RenderExtensionPoint::AfterTransparent)
                            draws.push_back(
                                {instance.get(), &e, &renderer, distance, nullptr, 0, 0, renderer.Material});
                    }
                    else if (renderer.Kind == Vfx::RendererKind::Ribbon)
                    {
                        auto bindings = frame->GetBindings(renderer.Material, false);
                        if (bindings && IsTransparentMaterial(bindings->Render.AlphaMode) ==
                                            (point == RenderExtensionPoint::AfterTransparent))
                            draws.push_back(
                                {instance.get(), &e, &renderer, distance, nullptr, 0, 0, renderer.Material});
                    }
                    else if (renderer.Kind == Vfx::RendererKind::Mesh)
                        for (uint32_t entry = 0; entry < renderer.Meshes.size(); ++entry)
                        {
                            const auto& item = renderer.Meshes[entry];
                            auto        mesh = frame->GetMesh(item.Mesh);
                            if (!mesh)
                                continue;
                            for (uint32_t section = 0; section < mesh->Sections.size(); ++section)
                            {
                                auto    slot     = mesh->Sections[section].MaterialSlot;
                                auto    override = item.MaterialOverrides.find(slot);
                                AssetID material =
                                    override != item.MaterialOverrides.end() ? override->second :
                                    slot < mesh->MaterialSlots.size() ? mesh->MaterialSlots[slot].DefaultMaterial :
                                                                        mesh->Material;
                                auto bindings = frame->GetBindings(material, false);
                                if (!bindings || IsTransparentMaterial(bindings->Render.AlphaMode) !=
                                                     (point == RenderExtensionPoint::AfterTransparent))
                                    continue;
                                draws.push_back(
                                    {instance.get(), &e, &renderer, distance, mesh, entry, section, material});
                            }
                        }
                }
            }
        if (draws.empty() && !needsCollisionDepth)
            return;
        std::stable_sort(
            draws.begin(), draws.end(), [](const auto& a, const auto& b) { return a.Distance > b.Distance; });
        const bool needsSoftDepth = std::any_of(
            draws.begin(), draws.end(), [](const auto& draw) { return draw.Renderer->DepthFadeDistance > 0; });
        GraphTexture depth = uint32_t(-1);
        if (needsSoftDepth || needsCollisionDepth)
        {
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
            if (!world->Depth)
                world->Depth = std::make_shared<Vfx::CollisionDepthSnapshot>();
            auto& snapshot = *world->Depth;
            if (!snapshot.Texture || snapshot.Texture->GetDesc().Width != desc.Width ||
                snapshot.Texture->GetDesc().Height != desc.Height ||
                snapshot.Texture->GetDesc().PixelFormat != desc.PixelFormat)
                snapshot.Texture = frame->GetDevice()->CreateTexture(desc);
            snapshot.ViewProjection        = projection * camera.viewMatrix;
            snapshot.InverseViewProjection = glm::inverse(snapshot.ViewProjection);
            depth                          = graph.ImportTexture("VFX soft particle depth", snapshot.Texture);
            graph.AddPass(
                "VFX depth snapshot",
                GraphPassKind::Transfer,
                {{sourceDepth, GraphAccess::TransferRead}, {depth, GraphAccess::TransferWrite}},
                [sourceDepth, depth](auto& c) { c.Commands.CopyTexture(c.Texture(sourceDepth), {}, c.Texture(depth)); },
                true);
        }
        else if (!draws.empty())
        {
            // The shader layout always binds depth; zero fade never samples it.
            if (!m_EmptyDepth)
            {
                TextureDesc empty;
                empty.PixelFormat = Format::Depth32Float;
                empty.Usage       = RenderHardware::TextureUsage::Sampled;
                m_EmptyDepth      = frame->GetDevice()->CreateTexture(empty);
            }
            depth = graph.ImportTexture("VFX unused soft particle depth", m_EmptyDepth);
        }
        if (draws.empty())
            return;
        bool workingHDR = frame->PostProcessHDR;
        std::map<std::pair<Vfx::EmitterInstance*, const Vfx::Renderer*>, Handle<RenderHardware::Buffer>>      sorted;
        std::map<std::tuple<Vfx::EmitterInstance*, const Vfx::Renderer*, uint32_t>, Vfx::GpuEmitter::MeshBin> bins;
        for (const auto& draw : draws)
        {
            auto& emitter = *draw.Emitter;
            if (!emitter.Failure.empty())
                continue;
            const auto first = graph.PassCount();
            try
            {
                const auto& r        = *draw.Renderer;
                auto        bindings = frame->GetBindings(draw.Material, false);
                if (!bindings)
                    continue;
                auto       shader = frame->GetShader(draw.Material, bindings->Shader);
                const bool ribbon = r.Kind == Vfx::RendererKind::Ribbon;
                if (!shader || !(ribbon    ? shader->PrepareParticleRibbon() :
                                 draw.Mesh ? shader->PrepareParticleMesh() :
                                             shader->PrepareParticleSprite()))
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
                if (!ribbon)
                    uniforms.Attributes0 = {offset("Position"),
                                            offset("Color"),
                                            offset(draw.Mesh ? "Scale" : "SpriteSize"),
                                            offset(draw.Mesh ? "MeshOrientation" : "SpriteRotation")};
                if (!ribbon)
                    uniforms.Attributes1 = {offset("Velocity"),
                                            draw.Mesh ? 0 : offset("SubImageIndex"),
                                            offset("DynamicMaterialParameter"),
                                            draw.Emitter->Compiled->Settings.Capacity};
                if (draw.Mesh)
                {
                    uniforms.MeshScale  = glm::vec4(r.Meshes[draw.Entry].Scale, 0);
                    uniforms.MeshPivot  = glm::vec4(r.Meshes[draw.Entry].PivotOffset, 0);
                    uniforms.LockedAxis = glm::vec4(r.LockedAxis, float(r.LockedAxisEnable));
                }
                uniforms.Lighting = {float(r.NormalMode),
                                     r.Curvature,
                                     float(r.Lighting),
                                     r.Kind == Vfx::RendererKind::Sprite &&
                                             r.NormalMode == Vfx::LightingNormal::ParticleNormal ?
                                         float(offset("Normal")) :
                                         0.f};
                if (!ribbon && r.Bindings.count("DirectionalTransmittance") &&
                    !r.Bindings.at("DirectionalTransmittance").empty())
                    uniforms.SmokeShadow.x = float(offset("DirectionalTransmittance"));
                uniforms.PivotGrid = {r.Pivot.x, r.Pivot.y, float(r.Columns), float(r.Rows)};
                uniforms.Facing =
                    glm::vec4(r.FacingVector, float(bindings->Render.AlphaMode == MaterialAlphaMode::Premultiplied));
                uniforms.Alignment = glm::vec4(r.AlignmentVector, 0);
                uniforms.Options   = {float(r.FacingMode),
                                      r.DepthFadeDistance,
                                      float(r.FrameBlending),
                                      float(draw.Emitter->Compiled->Settings.SimulationSpace == Vfx::Space::Local)};
                EngineFrameUniforms ef =
                    context.View.LightingUniforms.value_or(MakeLightingUniforms(context.View.Scene));
                frame->ApplyTemporalUniforms(ef);
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
                                         bindings->Render.ReceiveShadows && r.ReceiveShadows ? 0.f : 1.f};
                const auto upload     = [&](const void* data, size_t bytes) {
                    return gpu->UploadUniform(graph, data, bytes);
                };
                MaterialPassData pass;
                pass.DebugName = "VFX/" + draw.System->Asset()->assetID.ToString() + "/" +
                                 emitter.Compiled->Settings.Name + "/Renderer/" + r.Guid;
                pass.Frame     = upload(&ef, sizeof(ef));
                pass.Object    = upload(&object, sizeof(object));
                auto sprite    = upload(&uniforms, sizeof(uniforms));
                pass.Render    = bindings->Render;
                auto& indices  = sorted[{draw.Emitter, &r}];
                if (!indices && !ribbon)
                    indices = gpu->SortForView(graph,
                                               *draw.Emitter,
                                               r,
                                               camera.viewMatrix,
                                               draw.System->OwnerTransform(),
                                               IsTransparentMaterial(bindings->Render.AlphaMode));
                pass.VertexSource      = ribbon    ? shader->RibbonProgram :
                                         draw.Mesh ? shader->MeshProgram :
                                                     shader->SpriteProgram;
                pass.VertexLayout      = ribbon    ? shader->RibbonLayout :
                                         draw.Mesh ? shader->MeshLayout :
                                                     shader->SpriteLayout;
                pass.IndirectArguments = gpu->DrawArgs;
                pass.ParticleMesh      = bool(draw.Mesh);
                pass.Section           = draw.Section;
                if (draw.Mesh)
                {
                    auto& bin = bins[{draw.Emitter, &r, draw.Entry}];
                    if (!bin.Indices)
                        bin = gpu->BinMesh(graph, r, draw.Entry, draw.Mesh->Sections, indices);
                    pass.IndirectArguments = bin.Arguments;
                    pass.IndirectOffset    = draw.Section * 20;
                }
                auto attributes = gpu->Attributes;
                if (ribbon)
                {
                    auto data =
                        gpu->BuildRibbon(graph, *draw.Emitter, r, camera.viewMatrix, draw.System->OwnerTransform());
                    attributes             = data.Vertices;
                    pass.IndirectArguments = data.Arguments;
                    indices                = gpu->Alive;
                }
                auto drawIndices        = draw.Mesh ? bins.at({draw.Emitter, &r, draw.Entry}).Indices : indices;
                pass.Environment        = frame->GetMaterialEnvironment();
                pass.EnvironmentSampler = frame->Services.GetMaterialEnvironmentSampler();
                pass.Samples            = frame->Samples;
                pass.Shadows            = context.View.Shadows;
                pass.ColorFormats.clear();
                const bool materialHDR = shader->OutputSpace == ShaderOutputSpace::LinearHDR;
                const bool previousHDR = workingHDR;
                auto       target      = frame->Output.Target(LoadOp::Load, materialHDR);
                pass.WriteTemporalMasks = frame->TemporalMasks != uint32_t(-1);
                for (const auto& color : target.Colors)
                    pass.ColorFormats.push_back(color.Target->GetDesc().PixelFormat);
                if (pass.WriteTemporalMasks)
                    pass.ColorFormats.push_back(Format::RG8Unorm);
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

                if (pass.WriteTemporalMasks)
                    use({frame->TemporalMasks, GraphAccess::ColorReadWrite});
                auto hdr = frame->Samples > 1 && !frame->MSAAResolved ?
                               frame->MSHDR :
                               graph.ImportTexture("SceneHDR", frame->Output.GetHDR());
                use({hdr, GraphAccess::ColorReadWrite});
                use({graph.ImportBuffer("VFX attributes", attributes), GraphAccess::StorageRead});
                for (auto u : {pass.Frame, pass.Object, sprite})
                    use({graph.ImportBuffer("VFX render uniform", u), GraphAccess::UniformRead});
                use({graph.ImportBuffer("VFX draw indices", drawIndices), GraphAccess::StorageRead});
                use({graph.ImportBuffer("VFX draw indirect", pass.IndirectArguments), GraphAccess::IndirectRead});
                if (draw.Mesh)
                {
                    use({graph.ImportBuffer("VFX mesh vertices", draw.Mesh->Vertices), GraphAccess::VertexRead});
                    use({graph.ImportBuffer("VFX mesh indices", draw.Mesh->Indices), GraphAccess::IndexRead});
                }
                if (pass.Shadows)
                {
                    use({graph.ImportTexture("VFX directional shadows", pass.Shadows->Directional),
                         GraphAccess::SampledRead});
                    for (const auto& image : pass.Shadows->Points)
                        use({graph.ImportTexture("VFX point shadows", image), GraphAccess::SampledRead});
                }
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
                auto a = attributes, al = drawIndices;
                auto sampler = frame->GetDevice()->CreateSampler({Filter::Nearest,
                                                                  Filter::Nearest,
                                                                  Filter::Nearest,
                                                                  AddressMode::ClampToEdge,
                                                                  AddressMode::ClampToEdge});
                graph.AddPass(std::string(ribbon    ? "VFX ribbon " :
                                          draw.Mesh ? "VFX mesh " :
                                                      "VFX sprite ") +
                                  draw.Emitter->Compiled->Settings.Name,
                              std::move(uses),
                              [frame,
                               emitter = draw.Emitter,
                               mesh    = draw.Mesh,
                               shader,
                               bindings,
                               pass,
                               sprite,
                               a,
                               al,
                               sampler,
                               depth,
                               materialHDR,
                               previousHDR](auto& c) mutable {
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
                                  c.Commands.BeginRendering(frame->MaterialTarget(LoadOp::Load, materialHDR));
                                  GpuMesh empty;
                                  bool    recorded = false;
                                  try
                                  {
                                      RecordMaterialDraw(*frame->Assets(),
                                                         c.Commands,
                                                         *shader,
                                                         *bindings,
                                                         mesh ? *mesh : empty,
                                                         pass,
                                                         nullptr,
                                                         &recorded);
                                  }
                                  catch (const std::exception& error)
                                  {
                                      // Pipeline creation precedes all draw commands. Keep the HDR conversions
                                      // and close rendering so later passes see the expected color representation.
                                      emitter->Fail(error.what());
                                  }
                                  c.Commands.EndRendering();
                                  if (!materialHDR)
                                      tone(true);
                                  frame->PostProcessHDR = true;
                                  if (!frame->DeferToneMapping && frame->View.EffectiveAA != AntiAliasing::TAA)
                                      tone(false);
                                  if (recorded)
                                  {
                                      ++frame->Stats.DrawCalls;
                                      ++frame->Stats.ParticleDraws;
                                  }
                              });
                graph.GuardPipelineFailures(
                    first,
                    [&emitter](const auto& error) { emitter.Fail(error); },
                    [&emitter] { return !emitter.Failure.empty(); });
                workingHDR            = true;
                frame->PostProcessHDR = true;
            }
            catch (const std::exception& error)
            {
                emitter.Fail(error.what());
                graph.GuardPipelineFailures(
                    first,
                    [&emitter](const auto& message) { emitter.Fail(message); },
                    [&emitter] { return !emitter.Failure.empty(); });
            }
        }
    }
} // namespace Kans
