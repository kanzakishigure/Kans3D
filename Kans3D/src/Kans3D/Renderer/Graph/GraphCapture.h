#pragma once
#include <array>
#include <map>
#include <optional>
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Resource/AntiAliasingPass.h"
#include "RenderGraph.h"

namespace Kans
{
    struct CaptureSelection
    {
        GraphDebugID                       Pass = 0, Resource = 0;
        RenderHardware::TextureSubresource Subresource;
    };
    struct CaptureRequest : CaptureSelection
    {
        bool AllLayers = false; // Request all cube faces / array layers of Subresource.Mip.
    };
    struct GraphCaptureSettings
    {
        bool                            CaptureGraph     = false;
        uint32_t                        ThumbnailMaxEdge = 256;
        std::optional<CaptureSelection> Inspector; // Only this pass/resource/mip/layer gets full resolution.
        std::vector<CaptureRequest>     Requests;  // Additional thumbnail subresources; default mip 0, layer 0.
    };
    struct CapturedImage
    {
        CaptureSelection            Selection;
        uint64_t                    Frame = 0;
        RenderHardware::TextureDesc SourceDescription;
        bool                        FullResolution = false;
        bool                        DepthInColor   = false; // R = raw depth, G = stencil (when present), B = 0, A = 1.
        glm::mat4                   InverseProjection {1};  // Device depth 0..1; captured with the view.
        RenderHardware::Handle<RenderHardware::Texture> Texture;
        std::string                                     Label; // Includes the resolve method for multisampled sources.
        std::string Unavailable; // Nonempty when the backend/source does not support capture.
    };
    struct CapturedGraphFrame
    {
        uint64_t                   Frame = 0;
        std::vector<CapturedImage> Images;
        std::vector<CompiledGraphResource> Resources;
        std::vector<CompiledGraphPass>     Passes;
        const CapturedImage*       Find(CaptureSelection selection, bool fullResolution = false) const;
    };
    struct CaptureDisplaySettings
    {
        uint8_t ChannelMask = 7;  // R=1 G=2 B=4 A=8. With RGB, output alpha is opaque.
        int     Solo        = -1; // -1 = mask, 0..3 = selected channel as opaque grayscale.
        float   Exposure    = 0;  // Stops, multiplier exp2(Exposure).
        float   RangeMin = 0, RangeMax = 1;
        bool    SRGB        = false; // Encode linear RGB to sRGB for display.
        bool    LinearDepth = false; // Reconstruct eye-space distance with captured inverse projection.
    };
    struct PixelProbeResult
    {
        uint64_t               Request = 0, Frame = 0;
        CaptureSelection       Selection;
        glm::uvec2             Pixel {0}; // Coordinates in the capture, row zero follows RHI conventions.
        RenderHardware::Format StoredFormat = RenderHardware::Format::RGBA8Unorm;
        // Exact stored bytes. D24S8 canonical word: depth bits 31..8, stencil bits 7..0.
        std::vector<uint8_t>  Bytes;
        std::array<double, 4> Value {0, 0, 0, 1}; // Decoded raw channels, no display transform.
    };
    // Render thread, one instance per view/device. No added waits in any method.
    // Frame/image/display handles own their allocations. Retain as long as needed;
    // GetImGuiTextureID additionally retains display images through UI GPU completion.
    // Reuse only occurs after ALL frame, caller, command and UI leases are gone.
    class GraphCapture
    {
    public:
        explicit GraphCapture(std::shared_ptr<RenderHardware::Device> device);
        void BeginFrame(const GraphCaptureSettings& settings, uint64_t frame, const glm::mat4& deviceProjection);
        void AfterPass(const RenderGraph& graph, GraphPass pass);
        std::shared_ptr<const CapturedGraphFrame> Latest() const { return m_Current; }
        // RGBA8Unorm, sampled 2D, suitable for Device::GetImGuiTextureID.
        // Mip/layer/face is selected by CapturedImage::Selection, requested before rendering.
        RenderHardware::Handle<RenderHardware::Texture> Display(const CapturedImage&          image,
                                                                const CaptureDisplaySettings& settings = {});
        // Full-resolution inspector images give original texel coordinates and native stored values.
        // Returns no request if unsupported or the bounded probe queue is full.
        std::optional<uint64_t> RequestPixel(const CapturedImage& image, glm::uvec2 pixel);
        // Empty until readback retires. A result is consumed by this call. No WaitIdle.
        std::optional<PixelProbeResult> PollPixel(uint64_t request);

    private:
        RenderHardware::Handle<RenderHardware::Texture> Acquire(RenderHardware::TextureDesc desc);
        void                                    Convert(RenderHardware::CommandList&                           commands,
                                                        const RenderHardware::Handle<RenderHardware::Texture>& source,
                                                        RenderHardware::TextureSubresource                     subresource,
                                                        const RenderHardware::Handle<RenderHardware::Texture>& target,
                                                        bool                                                   depthInColor,
                                                        bool                                                   snapshot,
                                                        const glm::mat4&                                       inverseProjection,
                                                        const CaptureDisplaySettings&                          settings = {});
        AntiAliasingPass                                m_AntiAliasing;
        std::shared_ptr<RenderHardware::Device> m_Device;
        GraphCaptureSettings                    m_Settings;
        std::shared_ptr<CapturedGraphFrame>     m_Current;
        glm::mat4                               m_InverseProjection {1};
        struct PooledImage
        {
            RenderHardware::Handle<RenderHardware::Texture> Image;
            uint64_t                                        LastFrame;
        };
        std::vector<PooledImage>                                m_Images;
        RenderHardware::Handle<RenderHardware::BindGroupLayout> m_Layout, m_StencilLayout;
        RenderHardware::Handle<RenderHardware::Sampler>         m_Sampler;
        std::map<std::pair<unsigned, RenderHardware::Format>, RenderHardware::Handle<RenderHardware::Pipeline>>
            m_Pipelines;
        struct Probe
        {
            PixelProbeResult                               Result;
            RenderHardware::Handle<RenderHardware::Buffer> Buffer;
        };
        uint64_t           m_NextProbe = 1;
        std::vector<Probe> m_Probes;
    };
} // namespace Kans
