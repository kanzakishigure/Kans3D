#include "kspch.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/Shading/MaterialPass.h"
#include "Kans3D/Renderer/Shading/RenderTextureShader.h"
#include "RenderAssetCache.h"
namespace Kans
{
    uint64_t RenderAssetCache::GetRenderTextureUpdateCount(AssetID id) const
    {
        const auto found = m_RenderTextures.find(id);
        return found == m_RenderTextures.end() ? 0 : found->second.Schedule.Updates;
    }
    bool RenderAssetCache::UpdateRenderTextures(Renderer& renderer, uint64_t frame, double time)
    {
        using namespace RenderHardware;
        bool                        complete = true;
        std::unordered_set<AssetID> visited, visiting;
        const auto                  update = [&](const auto& self, AssetID id) -> bool {
            if (visited.count(id))
                return true;
            if (!visiting.insert(id).second)
                return false; // Feedback needs explicit double buffering; never sample the attachment.
            const auto texture = GetTexture(id);
            const auto found   = m_Textures.find(id);
            if (!texture || found == m_Textures.end() || !found->second.Source ||
                !found->second.Source->GetData().RenderTexture)
            {
                visiting.erase(id);
                return bool(texture);
            }
            const auto source   = found->second.Source;
            const auto spec     = *source->GetData().RenderTexture;
            auto&      state    = m_RenderTextures[id];
            const auto requests = source->UpdateRequests();
            if (!state.Schedule.Due(spec, frame, time, requests))
            {
                visiting.erase(id);
                visited.insert(id);
                return true;
            }
            const GpuMaterialBindings* bindings = nullptr;
            const GpuShader*           base     = nullptr;
            Handle<BindGroup>          material;
            if (spec.Material.IsValid())
            {
                const auto status = m_Assets.Request<MaterialDataAsset>(spec.Material);
                bindings          = GetMaterialBindings(spec.Material);
                base              = bindings ? GetShader(bindings->Shader) : nullptr;
                if (!status.IsReady() || !bindings || !base || bindings->Shader != status.Asset->GetData().Shader)
                    return false; // Never consume an OnLoad/request while a fallback shader is in use.
                for (const auto& [slot, dependency] : bindings->Textures)
                {
                    GetTexture(dependency);
                    const auto dep = m_Textures.find(dependency);
                    if (dep != m_Textures.end() && dep->second.Source && dep->second.Source->GetData().RenderTexture &&
                        !self(self, dependency))
                        return false;
                }
                bool ready = false;
                material   = GetMaterialGroup(*base, *bindings, &ready);
                if (!material || !ready)
                    return false;
                if (state.Source != base->Source())
                {
                    auto built = MakeRenderTextureShader(*base->Source(), GetDevice());
                    if (const auto* e = std::get_if<AssetError>(&built))
                    {
                        if (Log::GetCoreLogger())
                            CORE_ERROR("RenderTexture {}: {}", id.ToString(), e->Message);
                        return false;
                    }
                    state.Shader = CreateScope<GpuShader>(std::get<GpuShader>(std::move(built)));
                    state.Source = base->Source();
                    state.Pipeline.reset();
                }
            }
            auto          device   = GetDevice();
            auto          commands = device->CreateCommandList();
            RenderingDesc render;
            render.Colors.push_back({texture->GetResource(), {}, LoadOp::Clear, StoreOp::Store, spec.ClearColor});
            Handle<BindGroup> engine;
            if (base)
            {
                auto& shader = *state.Shader;
                if (!state.Pipeline)
                {
                    PipelineDesc pipeline;
                    pipeline.Program      = shader.Program;
                    pipeline.GroupLayouts = {shader.EngineLayout, base->MaterialLayout};
                    pipeline.ColorFormats = {texture->GetResource()->GetDesc().PixelFormat};
                    pipeline.Blends.resize(1);
                    pipeline.DepthStencil.DepthTest = pipeline.DepthStencil.DepthWrite = false;
                    pipeline.Rasterizer.Cull                                           = CullMode::None;
                    pipeline.DebugName                                                 = "Material render texture";
                    state.Pipeline = device->CreatePipeline(pipeline);
                }
                EngineFrameUniforms uniforms;
                uniforms.Time            = float(time);
                uniforms.Padding[0]      = 1.f / spec.Width;
                uniforms.Padding[1]      = 1.f / spec.Height;
                uniforms.EnvironmentInfo = {0, 0, 1, 0};
                uniforms.LightingInfo    = {0, 1, 0, 0};
                EngineObjectUniforms object;
                GpuUniformBuffer     frameBuffer(sizeof(uniforms), &uniforms, device),
                    objectBuffer(sizeof(object), &object, device);
                MaterialPassData pass;
                pass.Frame              = frameBuffer.Resource();
                pass.Object             = objectBuffer.Resource();
                pass.Environment        = renderer.GetMaterialEnvironment(EnvironmentLighting {});
                pass.EnvironmentSampler = renderer.GetMaterialEnvironmentSampler();
                engine                  = CreateMaterialEngineGroup(*this, shader, pass);
            }
            commands->BeginRendering(render);
            commands->SetViewport({0, 0, float(spec.Width), float(spec.Height)});
            commands->SetScissor({0, 0, spec.Width, spec.Height});
            if (base)
            {
                commands->BindPipeline(state.Pipeline);
                commands->BindGroup(engine);
                commands->BindGroup(material);
                commands->Draw(3);
            }
            commands->EndRendering();
            if (texture->GetResource()->GetDesc().MipLevels > 1)
                commands->GenerateMips(texture->GetResource());
            device->Submit(commands);
            state.Schedule.Complete(frame, time, requests);
            visiting.erase(id);
            visited.insert(id);
            return true;
        };
        // Resolving the producer material may discover further texture dependencies.
        auto pending = std::move(m_PendingRenderTextures);
        m_PendingRenderTextures.clear();
        for (auto id : pending)
            complete &= update(update, id);
        m_PendingRenderTextures.clear();
        return complete;
    }
} // namespace Kans
