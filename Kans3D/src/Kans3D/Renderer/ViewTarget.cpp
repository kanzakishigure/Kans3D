#include "kspch.h"
#include "ViewTarget.h"
#include "Kans3D/Core/Debug/FatalError.h"
#include "Features/BloomFeature.h"
#include "Graph/GraphCapture.h"
#include "Passes/Fsr3Upscaler.h"
#include "Temporal.h"
namespace Kans
{
    void ViewTarget::RecoverFailedFrame()
    {
        try
        {
            InvalidateTemporalHistory();
            m_NormalRoughnessValid = m_VelocityValid = false;
            m_Jitter                                 = {};
            m_Profiler.AbortFrame(m_FrameNumber);
            // Earlier passes may already be queued. A fresh list clears the outputs
            // after them on the same queue; the failed recorder is never submitted.
            if (!m_Colors.empty())
            {
                auto commands = m_Device->CreateCommandList();
                commands->BeginRendering(Target(RenderHardware::LoadOp::Clear));
                commands->EndRendering();
                m_Device->Submit(commands);
            }
        }
        catch (...)
        {
            // A device that cannot even submit a clear cannot safely continue rendering.
            FatalError::ReportException(std::current_exception(), "View recovery");
        }
    }
    ViewTarget::ViewTarget(std::shared_ptr<RenderHardware::Device> device, uint32_t outputs, bool sampledDepth) :
        m_Device(std::move(device)), m_OutputCount(outputs), m_SampledDepth(sampledDepth)
    {
        m_DefaultFeatures.Register("Bloom", std::make_unique<BloomFeature>());
        if (!m_Device || !outputs)
            throw std::invalid_argument("ViewTarget needs a device and at least one output");
    }
    std::shared_ptr<GraphCapture> ViewTarget::GetGraphCapture()
    {
        if (!m_Capture)
            m_Capture = std::make_shared<GraphCapture>(m_Device);
        return m_Capture;
    }
    Fsr3Upscaler& ViewTarget::EnsureFsr3Upscaler()
    {
        if (!m_Fsr3Upscaler)
            m_Fsr3Upscaler = std::make_shared<Fsr3Upscaler>(*m_Device);
        return *m_Fsr3Upscaler;
    }
    ViewTarget& ViewTarget::ConfigureRenderTarget(glm::uvec2 size, float ratio)
    {
        if (ratio != m_RenderRatio || (m_RenderTarget && m_RenderTarget->GetSize() != size))
        {
            m_RenderTarget.reset();
            ReleaseFsr3Upscaler();
            InvalidateTemporalHistory();
            m_Pool = {};
        }
        m_RenderRatio = ratio;
        if (ratio == 1.f)
            return *this;
        if (!m_RenderTarget)
            m_RenderTarget = std::make_shared<ViewTarget>(m_Device, m_OutputCount, true);
        m_RenderTarget->Resize(size);
        return *m_RenderTarget;
    }
    void ViewTarget::EnsureSampledDepth()
    {
        using namespace RenderHardware;
        m_SampledDepth = true;
        auto desc = m_Depth->GetDesc();
        if ((uint32_t(desc.Usage) & uint32_t(RenderHardware::TextureUsage::Sampled)) == 0)
        {
            desc.Usage = desc.Usage | RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource;
            m_Depth    = m_Device->CreateTexture(desc);
        }
    }
    void ViewTarget::EnsureNormalRoughness()
    {
        using namespace RenderHardware;
        if (!m_NormalRoughness)
        {
            TextureDesc desc;
            desc.Width        = m_Size.x;
            desc.Height       = m_Size.y;
            desc.PixelFormat  = Format::RGBA16Float;
            desc.Usage        = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled |
                                RenderHardware::TextureUsage::CopySource;
            m_NormalRoughness = m_Device->CreateTexture(desc);
        }
        m_NormalRoughnessValid = true;
    }
    TemporalState& ViewTarget::GetTemporalState()
    {
        if (!m_Temporal)
            m_Temporal = std::make_shared<TemporalState>();
        return *m_Temporal;
    }
    void ViewTarget::InvalidateTemporalHistory()
    {
        if (m_RenderTarget)
            m_RenderTarget->InvalidateTemporalHistory();
        if (m_Temporal)
        {
            m_Temporal->Valid = false;
            for (auto it = m_Temporal->Objects.begin(); it != m_Temporal->Objects.end();)
                if (m_FrameNumber - it->second.LastSeen >= 4)
                    it = m_Temporal->Objects.erase(it);
                else
                    ++it;
        }
    }
    void ViewTarget::EnsureVelocity()
    {
        using namespace RenderHardware;
        if (!m_SceneVelocity)
        {
            TextureDesc desc;
            desc.Width       = m_Size.x;
            desc.Height      = m_Size.y;
            desc.PixelFormat = Format::RG16Float;
            desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled |
                               RenderHardware::TextureUsage::CopySource;
            m_SceneVelocity  = m_Device->CreateTexture(desc);
        }
        m_VelocityValid = true;
    }
    void ViewTarget::Resize(glm::uvec2 size)
    {
        if (size == m_Size && m_HDR)
            return;
        using namespace RenderHardware;
        TextureDesc desc;
        desc.Width  = size.x;
        desc.Height = size.y;
        desc.Usage  = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled |
                      RenderHardware::TextureUsage::CopySource | RenderHardware::TextureUsage::CopyDestination;
        std::vector<Handle<RenderHardware::Texture>> colors;
        for (uint32_t i = 0; i < m_OutputCount; ++i)
            colors.push_back(m_Device->CreateTexture(desc));
        desc.PixelFormat = Format::RGBA16Float;
        auto hdr         = m_Device->CreateTexture(desc);
        desc.PixelFormat = Format::Depth24Stencil8;
        desc.Usage       = RenderHardware::TextureUsage::DepthStencil;
        if (m_SampledDepth)
            desc.Usage = desc.Usage | RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopySource;
        auto depth = m_Device->CreateTexture(desc);
        m_Colors   = std::move(colors);
        m_HDR      = std::move(hdr);
        m_Depth    = std::move(depth);
        m_Size     = size;
        m_RenderTarget.reset();
        // AddPasses recreates size-dependent FSR resources while preserving the context identity.
        m_NormalRoughness.reset();
        m_SceneVelocity.reset();
        m_VelocityValid = false;
        m_Jitter        = {};
        InvalidateTemporalHistory();
        m_NormalRoughnessValid = false;
        m_Pool     = {};
        m_ReconstructionFilter = {};
    }
    RenderHardware::RenderingDesc ViewTarget::Target(RenderHardware::LoadOp load, bool hdr) const
    {
        RenderHardware::RenderingDesc desc;
        for (const auto& color : m_Colors)
            desc.Colors.push_back({color, {}, load, RenderHardware::StoreOp::Store, {0.02f, 0.02f, 0.02f, 1}});
        if (hdr)
            desc.Colors[0].Target = m_HDR;
        desc.Depth.Target = m_Depth;
        desc.Depth.Load   = load;
        return desc;
    }
    uintptr_t ViewTarget::GetOutput(uint32_t index) const
    {
        if (index > m_Colors.size() + 1)
            return uint32_t(-1);
        return m_Device->GetImGuiTextureID(index < m_Colors.size() ? m_Colors[index] : m_Depth);
    }
} // namespace Kans
