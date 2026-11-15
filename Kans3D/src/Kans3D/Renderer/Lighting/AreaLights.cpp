#include "kspch.h"
#include "AreaLights.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "LtcTables.h"
namespace Kans
{
    std::shared_ptr<AreaLightResources> CreateAreaLightFallback(RenderHardware::Device& device)
    {
        using namespace RenderHardware;
        auto result = std::make_shared<AreaLightResources>();
        auto upload = [&](uint32_t size, const float* data) {
            TextureDesc desc;
            desc.Width = desc.Height = size;
            desc.PixelFormat         = Format::RGBA32Float;
            desc.Usage          = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
            auto         image  = device.CreateTexture(desc);
            const size_t bytes  = size * size * 4 * sizeof(float);
            auto         buffer = device.CreateBuffer({bytes, BufferUsage::CopySource, MemoryUsage::Upload});
            buffer->Write(0, data, bytes);
            auto commands = device.CreateCommandList();
            commands->CopyBufferToTexture(buffer, 0, image);
            device.Submit(commands);
            return image;
        };
        result->Matrix      = upload(64, Ltc1.data());
        result->Amplitude   = upload(64, Ltc2.data());
        const float white[] = {1, 1, 1, 1};
        result->Sources.fill(upload(1, white));
        SamplerDesc sampler;
        sampler.U = sampler.V = sampler.W = AddressMode::ClampToEdge;
        result->Sampler                   = device.CreateSampler(sampler);
        return result;
    }
    std::shared_ptr<AreaLightResources> RenderAssetCache::GetAreaLightFallback()
    {
        if (!m_AreaLightFallback)
            m_AreaLightFallback = CreateAreaLightFallback(*GetDevice());
        return m_AreaLightFallback;
    }
    std::vector<RenderHardware::Handle<RenderHardware::Texture>> AreaLightImages(const AreaLightResources& r)
    {
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> images {r.Matrix, r.Amplitude};
        for (const auto& source : r.Sources)
            if (std::find(images.begin(), images.end(), source) == images.end())
                images.push_back(source);
        return images;
    }
    void PrepareAreaLights(RenderFrame& frame)
    {
        if (!frame.Assets())
            return;
        auto& view      = frame.View;
        auto  resources = std::make_shared<AreaLightResources>(*frame.Assets()->GetAreaLightFallback());
        for (size_t i = 0; i < std::min(view.Scene.areaLights.size(), size_t(EngineAreaLightLimit)); ++i)
        {
            const auto& light = view.Scene.areaLights[i];
            if (light.Shape != AreaLightShape::Rectangle)
                continue;
            auto image = light.SourceTextureHandle;
            if (auto it = view.AreaLightSources.find(light.EntityID); it != view.AreaLightSources.end())
                image = it->second;
            if (!image && light.SourceTexture.IsValid())
                image = frame.Assets()->GetTextureResource(light.SourceTexture);
            if (image && image->GetDesc().Dimension == RenderHardware::TextureDimension::Texture2D &&
                image->GetDesc().Samples == 1 && image->GetDesc().ArrayLayers == 1 &&
                (uint32_t(image->GetDesc().Usage) & uint32_t(RenderHardware::TextureUsage::Sampled)) != 0)
                resources->Sources[i] = image;
        }
        view.AreaLighting = resources;
        // Source metadata is populated together with the authored light data; do not overwrite
        // externally supplied shadow/temporal uniforms.
        if (!view.LightingUniforms)
            view.LightingUniforms = MakeLightingUniforms(view.Scene);
        // Only active slots carry source metadata; unused slots stay zero like every other light array.
        const size_t active = std::min(view.Scene.areaLights.size(), size_t(EngineAreaLightLimit));
        for (size_t i = 0; i < active; ++i)
        {
            view.LightingUniforms->AreaScales[i].w =
                resources->Sources[i] != frame.Assets()->GetAreaLightFallback()->Sources[i] ? 1 : 0;
            const auto& d                            = resources->Sources[i]->GetDesc();
            view.LightingUniforms->AreaSourceSize[i] = {d.Width, d.Height, d.MipLevels, 0};
        }
    }
} // namespace Kans
