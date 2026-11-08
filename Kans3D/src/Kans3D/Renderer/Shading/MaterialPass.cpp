#include "kspch.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
#include "Kans3D/RHI/PipelineKey.h"
#include "Kans3D/Renderer/Lighting/ShadowMaps.h"

namespace Kans
{
    bool RecordMaterialDraw(RenderAssetCache&            cache,
                            RenderHardware::CommandList& commands,
                            const GpuShader&             shader,
                            const GpuMaterialBindings&   bindings,
                            const GpuMesh&               mesh,
                            const MaterialPassData&      data,
                            const GpuShaderPass*         extraPass,
                            bool*                        recorded)
    {
        if (recorded)
            *recorded = false;
        if (shader.Domain != ShaderDomain::Surface)
        {
            cache.WarnMaterialDomain(bindings.Shader, false);
            return true;
        }
        using namespace RenderHardware;
        auto       device         = cache.GetDevice();
        const bool skinned        = bool(mesh.Skin && data.Palette);
        const bool smoothNormals  = extraPass ? extraPass->UsesSmoothNormals : shader.UsesSmoothNormals;
        const bool smoothStream   = smoothNormals && bool(mesh.SmoothNormals);
        const bool smoothFallback = smoothNormals && !smoothStream;
        if (skinned && !shader.PrepareSkinning(smoothFallback))
            return false;
        const bool coverage    = data.Samples > 1 && (data.Render.AlphaMode == MaterialAlphaMode::Mask ||
                                                      data.Render.AlphaMode == MaterialAlphaMode::Dithered);
        const bool transparent = IsTransparentMaterial(data.Render.AlphaMode);
        // Include attachment formats as well as count: display and HDR share a shader cache.
        uint64_t key = (uint32_t(skinned) << 7) | uint32_t(transparent) | (uint32_t(data.Render.DoubleSided) << 1) |
                       (uint32_t(data.Wireframe) << 2) | (uint32_t(data.WriteStencil) << 3) |
                       (uint32_t(data.ColorFormats.size()) << 8) |
                       ((data.Samples == 8 ? 3u :
                         data.Samples == 4 ? 2u :
                         data.Samples == 2 ? 1u :
                                             0u)
                        << 4) |
                       (uint32_t(coverage) << 6);
        for (size_t i = 0; i < data.ColorFormats.size(); ++i)
            key |= uint32_t(data.ColorFormats[i]) << (12 + i * 6);
        key |= uint32_t(smoothStream) << 31;
        key |= uint32_t(data.Render.AlphaMode == MaterialAlphaMode::Additive) << 28;
        key |= uint32_t(data.Render.AlphaMode == MaterialAlphaMode::Premultiplied) << 29;
        key |= uint32_t(bool(data.VertexSource)) << 30;
        key |= uint32_t(data.ParticleMesh) << 27;
        // Ribbons and sprites both use procedural vertices, but have different storage contracts.
        key |= uint64_t(data.VertexSource && data.VertexSource == shader.RibbonProgram) << 32;
        key |= uint64_t(data.DepthPrepass) << 33;
        key |= uint64_t(data.PrepassCovered && !extraPass) << 34;
        key |= uint64_t(data.InvalidatePrepassNormals) << 35;
        Handle<Pipeline> shadowPipeline;
        auto&            pipeline = (data.ShadowDepth || data.CustomDepth) ? shadowPipeline :
                                    extraPass                              ? extraPass->Pipelines[key] :
                                                                             shader.Pipelines[key];
        if (!pipeline)
        {
            PipelineDesc desc;
            desc.Samples         = data.Samples;
            desc.AlphaToCoverage = coverage;
            desc.Program         = data.VertexSource ? data.VertexSource :
                                   data.DepthProgram ? data.DepthProgram :
                                   extraPass         ? extraPass->Program :
                                                       shader.Program;
            if (skinned && !data.DepthProgram)
                desc.Program = smoothFallback ? (extraPass ? extraPass->SkinnedNormalFallbackProgram :
                                                             shader.SkinnedNormalFallbackProgram) :
                                                (extraPass ? extraPass->SkinnedProgram : shader.SkinnedProgram);
            if (data.InvalidatePrepassNormals && !extraPass)
                desc.Program =
                    shader.PreparePrepassInvalidation(uint32_t(data.ColorFormats.size() - 1), skinned, smoothFallback);
            desc.DebugName =
                data.DebugName.empty() ? desc.Program->DebugName : data.DebugName + "/" + desc.Program->DebugName;
            desc.GroupLayouts = {shader.EngineLayout, shader.MaterialLayout};
            if (skinned)
                desc.GroupLayouts.push_back(shader.SkinLayout);
            if (data.VertexLayout)
                desc.GroupLayouts.push_back(data.VertexLayout);
            desc.VertexBindings = mesh.VertexBindings;
            desc.Attributes     = mesh.Attributes;
            if (!skinned && mesh.Skin)
            {
                desc.VertexBindings.resize(1);
                desc.Attributes.erase(std::remove_if(desc.Attributes.begin(),
                                                     desc.Attributes.end(),
                                                     [](const auto& a) { return a.BufferSlot == 1; }),
                                      desc.Attributes.end());
            }
            if (smoothNormals)
            {
                if (smoothStream)
                {
                    desc.VertexBindings.push_back({2, 12, false});
                    desc.Attributes.push_back({7, 2, 0, VertexFormat::Float3});
                }
                else
                    // Alias the existing normal: no allocation, upload, or retained CPU copy.
                    desc.Attributes.push_back({7, 0, 12, VertexFormat::Float3});
            }
            if (data.VertexSource && !data.ParticleMesh)
            {
                desc.VertexBindings.clear();
                desc.Attributes.clear();
            }
            desc.ColorFormats                 = data.ColorFormats;
            desc.HasDepthStencil              = true;
            desc.DepthStencilFormat           = Format::Depth24Stencil8;
            desc.DepthStencil.DepthTest       = true;
            desc.DepthStencil.DepthWrite      = !transparent;
            desc.DepthStencil.StencilTest     = data.WriteStencil;
            desc.DepthStencil.Front.Reference = desc.DepthStencil.Back.Reference = 1;
            desc.DepthStencil.Front.Pass = desc.DepthStencil.Back.Pass = StencilOp::Replace;
            // DeviceProjection already converts clip Y using the coordinate query.
            // The converted projection preserves the mesh's visual CCW facing.
            desc.Rasterizer.Front   = FrontFace::CounterClockwise;
            desc.Rasterizer.Cull    = data.Render.DoubleSided ? CullMode::None : CullMode::Back;
            desc.Rasterizer.Polygon = data.Wireframe ? PolygonMode::Line : PolygonMode::Fill;
            BlendState blend;
            blend.Enabled          = transparent;
            blend.WriteMask        = 7;
            blend.SourceColor      = BlendFactor::SourceAlpha;
            blend.DestinationColor = BlendFactor::OneMinusSourceAlpha;
            blend.SourceAlpha      = BlendFactor::Zero;
            blend.DestinationAlpha = BlendFactor::One;
            if (data.Render.AlphaMode == MaterialAlphaMode::Additive)
                blend.DestinationColor = BlendFactor::One;
            if (data.Render.AlphaMode == MaterialAlphaMode::Premultiplied)
                blend.SourceColor = BlendFactor::One;
            if (extraPass)
            {
                const auto& state              = extraPass->State;
                desc.Rasterizer.Cull           = static_cast<CullMode>(state.Cull);
                desc.Rasterizer.Polygon        = PolygonMode::Fill;
                desc.DepthStencil.DepthTest    = state.DepthTest;
                desc.DepthStencil.DepthCompare = static_cast<CompareOp>(state.DepthCompare);
                desc.DepthStencil.DepthWrite   = state.DepthWrite;
                desc.DepthStencil.StencilTest  = false;
                blend.Enabled                  = state.Blend;
                blend.SourceColor              = static_cast<BlendFactor>(state.SourceColor);
                blend.DestinationColor         = static_cast<BlendFactor>(state.DestinationColor);
                blend.SourceAlpha              = static_cast<BlendFactor>(state.SourceAlpha);
                blend.DestinationAlpha         = static_cast<BlendFactor>(state.DestinationAlpha);
                blend.WriteMask                = state.ColorWriteMask;
            }
            if (data.ShadowDepth || data.CustomDepth)
            {
                desc.Rasterizer.Front       = data.DepthFront;
                desc.DepthStencilFormat     = data.CustomDepth ? Format::Depth24Stencil8 : Format::Depth32Float;
                desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = true;
                desc.DepthStencil.DepthCompare                             = CompareOp::LessEqual;
                desc.DepthStencil.StencilTest                              = data.CustomDepth;
                desc.DepthStencil.Front.Reference = desc.DepthStencil.Back.Reference = data.CustomStencil;
                desc.DepthStencil.Front.Pass = desc.DepthStencil.Back.Pass = StencilOp::Replace;
                desc.Rasterizer.DepthBiasConstant                          = data.ConstantBias;
                desc.Rasterizer.DepthBiasSlope                             = data.SlopeBias;
                desc.Rasterizer.Cull = data.Render.DoubleSided ? CullMode::None :
                                       extraPass               ? static_cast<CullMode>(extraPass->State.Cull) :
                                                                 CullMode::Back;
            }
            if (data.DepthPrepass)
            {
                desc.DepthStencil.DepthTest = desc.DepthStencil.DepthWrite = true;
                desc.DepthStencil.DepthCompare                             = CompareOp::LessEqual;
                desc.DepthStencil.StencilTest                              = data.WriteStencil;
                desc.Rasterizer.Cull    = data.Render.DoubleSided ? CullMode::None : CullMode::Back;
                desc.Rasterizer.Polygon = data.Wireframe ? PolygonMode::Line : PolygonMode::Fill;
                blend.Enabled           = false;
                blend.WriteMask         = 15;
            }
            if (data.PrepassCovered && !extraPass)
            {
                desc.DepthStencil.DepthWrite   = false;
                desc.DepthStencil.DepthCompare = CompareOp::LessEqual;
            }
            desc.Blends.resize(data.ColorFormats.size(), blend);
            if (data.InvalidatePrepassNormals)
            {
                desc.Blends.back()           = {};
                desc.Blends.back().WriteMask = extraPass ? 0 : 15;
            }
            if (data.ShadowDepth || data.CustomDepth)
            {
                auto  resources = data.Shadows ? data.Shadows : cache.GetShadowFallback();
                auto& cached    = resources->DepthPipelines[Detail::PipelineKey(desc)];
                if (!cached)
                    cached = device->CreatePipeline(desc);
                pipeline = cached;
            }
            else
                pipeline = device->CreatePipeline(desc);
        }
        BindGroupDesc engine;
        engine.Layout = shader.EngineLayout;
        for (const auto& binding : shader.EngineResources)
        {
            BindGroupEntry entry;
            entry.Slot = binding.Binding;
            if (binding.Binding == uint32_t(EngineBinding::Frame))
            {
                entry.BufferResource = data.Frame;
                entry.Offset         = data.FrameOffset;
                entry.Bytes          = sizeof(EngineFrameUniforms);
            }
            else if (binding.Binding == uint32_t(EngineBinding::Object))
            {
                entry.BufferResource = data.Object;
                entry.Offset         = data.ObjectOffset;
                entry.Bytes          = sizeof(EngineObjectUniforms);
            }
            else if (binding.Binding >= uint32_t(EngineBinding::DirectionalShadow))
            {
                auto shadows = data.Shadows ? data.Shadows : cache.GetShadowFallback();
                entry.TextureResource =
                    binding.Binding <= 6 ? shadows->Directional : shadows->Points.at(binding.Binding - 7);
                entry.SamplerResource = binding.Binding == 6 ? shadows->Raw : shadows->Compare;
            }
            else
            {
                entry.TextureResource = data.Environment.at(binding.Binding - 2);
                entry.SamplerResource = data.EnvironmentSampler;
            }
            engine.Entries.push_back(std::move(entry));
        }
        bool        complete = true;
        const auto* instance =
            data.MaterialGroup ? &bindings : cache.GetInstanceMaterialBindings(bindings, data.Parameters);
        auto material = data.MaterialGroup ? data.MaterialGroup : cache.GetMaterialGroup(shader, *instance, &complete);
        if (!material)
            return false;
        commands.BindPipeline(pipeline);
        commands.BindGroup(device->CreateBindGroup(engine));
        commands.BindGroup(material);
        if (data.VertexSource)
        {
            commands.BindGroup(data.VertexGroup);
            if (data.ParticleMesh)
            {
                commands.BindVertexBuffers({{0, mesh.BindVertices ? mesh.BindVertices : mesh.Vertices, 0}});
                if (smoothStream)
                    commands.BindVertexBuffers(
                        {{2, mesh.BindSmoothNormals ? mesh.BindSmoothNormals : mesh.SmoothNormals, 0}});
                commands.BindIndexBuffer(mesh.Indices, IndexType::Uint32);
                commands.DrawIndexedIndirect(data.IndirectArguments, data.IndirectOffset);
            }
            else
                commands.DrawIndirect(data.IndirectArguments);
            if (recorded)
                *recorded = true;
            return complete;
        }
        if (skinned)
        {
            BindSkinning(
                *device, commands, shader.SkinLayout, data.Palette ? data.Palette : mesh.BindPalette, mesh.MorphDeltas);
            commands.BindVertexBuffers({{0, mesh.Vertices, 0}, {1, mesh.Skin, 0}});
        }
        else
            commands.BindVertexBuffers({{0, mesh.BindVertices ? mesh.BindVertices : mesh.Vertices, 0}});
        if (smoothStream)
            commands.BindVertexBuffers(
                {{2, !skinned && mesh.BindSmoothNormals ? mesh.BindSmoothNormals : mesh.SmoothNormals, 0}});
        commands.BindIndexBuffer(mesh.Indices, IndexType::Uint32);
        const auto& section = mesh.Sections.at(data.Section);
        commands.DrawIndexed(section.IndexCount, 1, section.FirstIndex);
        if (recorded)
            *recorded = true;
        return complete;
    }
} // namespace Kans
