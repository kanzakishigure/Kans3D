#pragma once
#include <thread>
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Text/GlyphAtlas.h"
namespace Kans
{
    struct FontAtlasUpload
    {
        size_t Bytes = 0, Regions = 0;
    };
    // One GPU copy per renderer/font/device, alongside RenderAssetCache. Owners
    // catch up independently. Flush after FontSystem::Rasterize and before drawing;
    // no implicit per-frame full uploads.
    // Retains the font and device. Release before shutting down the device context.
    class GpuFontAtlas
    {
    public:
        GpuFontAtlas(std::shared_ptr<RenderHardware::Device>, Ref<const FontAsset>);
        ~GpuFontAtlas();
        GpuFontAtlas(const GpuFontAtlas&)                                                                  = delete;
        GpuFontAtlas&                                                       operator=(const GpuFontAtlas&) = delete;
        FontAtlasUpload                                                     Flush();
        const std::vector<RenderHardware::Handle<RenderHardware::Texture>>& Pages() const { return m_Pages; }
        RenderHardware::Handle<RenderHardware::Sampler>                     Sampler() const { return m_Sampler; }

    private:
        std::shared_ptr<RenderHardware::Device>                      m_Device;
        Ref<const FontAsset>                                         m_Font;
        std::thread::id                                              m_Owner;
        uint64_t                                                     m_UploadOwner = 0;
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> m_Pages;
        RenderHardware::Handle<RenderHardware::Sampler>              m_Sampler;
    };
} // namespace Kans
