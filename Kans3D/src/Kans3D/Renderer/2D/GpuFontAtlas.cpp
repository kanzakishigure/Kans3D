#include "kspch.h"
#include "Kans3D/Renderer/2D/GpuFontAtlas.h"
#include <cstring>
namespace Kans
{
    namespace RH = RenderHardware;
    GpuFontAtlas::GpuFontAtlas(std::shared_ptr<RH::Device> device, Ref<const FontAsset> font) :
        m_Device(std::move(device)), m_Font(std::move(font)), m_Owner(std::this_thread::get_id())
    {
        if (!m_Device || !m_Font)
            throw std::invalid_argument("GPU font atlas needs a device and font");
        RH::SamplerDesc s;
        s.U = s.V = s.W = RH::AddressMode::ClampToEdge;
        s.MaxLod        = 0;
        m_Sampler       = m_Device->CreateSampler(s);
        m_UploadOwner   = m_Font->Atlas().RegisterUploadOwner();
    }
    GpuFontAtlas::~GpuFontAtlas() { m_Font->Atlas().UnregisterUploadOwner(m_UploadOwner); }
    FontAtlasUpload GpuFontAtlas::Flush()
    {
        if (std::this_thread::get_id() != m_Owner)
            throw std::runtime_error("GPU font atlas requires owner thread");
        auto&       atlas    = m_Font->Atlas();
        const auto& pages    = atlas.Pages();
        auto        commands = m_Device->CreateCommandList();
        bool        created  = false;
        while (m_Pages.size() < pages.size())
        {
            RH::TextureDesc d;
            d.Width = d.Height = GlyphAtlas::PageSize;
            d.Usage = RH::TextureUsage::Sampled | RH::TextureUsage::CopyDestination | RH::TextureUsage::CopySource |
                      RH::TextureUsage::RenderTarget;
            m_Pages.push_back(m_Device->CreateTexture(d));
            // Initialize unused texels once on GPU, without uploading a page of zeros.
            RH::RenderingDesc clear;
            clear.Colors.push_back({m_Pages.back()});
            commands->BeginRendering(clear);
            commands->EndRendering();
            created = true;
        }
        FontAtlasUpload stats;
        for (const auto& region : atlas.PendingUploads(m_UploadOwner))
        {
            const auto           p     = region.Page;
            const auto           rect  = region.Rect;
            size_t               bytes = size_t(rect.Width) * rect.Height * 4;
            std::vector<uint8_t> packed(bytes);
            for (uint32_t y = 0; y < rect.Height; ++y)
                std::memcpy(packed.data() + size_t(y) * rect.Width * 4,
                            pages[p].Pixels.data() + ((size_t(rect.Y + y) * GlyphAtlas::PageSize + rect.X) * 4),
                            rect.Width * 4);
            auto upload = m_Device->CreateBuffer({bytes, RH::BufferUsage::CopySource, RH::MemoryUsage::Upload});
            upload->Write(0, packed.data(), bytes);
            commands->CopyBufferToTextureRegion(upload, 0, m_Pages[p], rect.X, rect.Y, rect.Width, rect.Height);
            stats.Bytes += bytes;
            ++stats.Regions;
        }
        if (stats.Regions || created)
        {
            m_Device->Submit(commands);
            for (uint32_t p = 0; p < pages.size(); ++p)
                atlas.ClearDirty(p);
        }
        atlas.AcknowledgeUploads(m_UploadOwner);
        return stats;
    }
} // namespace Kans
