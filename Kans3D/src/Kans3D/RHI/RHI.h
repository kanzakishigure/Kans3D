#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Kans
{
    struct ShaderData;
}

namespace Kans::RenderHardware
{
    template<class T>
    using Handle = std::shared_ptr<T>;

    enum class BufferUsage : uint32_t
    {
        Vertex          = 1,
        Index           = 2,
        Uniform         = 4,
        Storage         = 8,
        CopySource      = 16,
        CopyDestination = 32,
        Indirect        = 64
    };
    enum class TextureUsage : uint32_t
    {
        Sampled         = 1,
        RenderTarget    = 2,
        DepthStencil    = 4,
        CopySource      = 8,
        CopyDestination = 16,
        Storage         = 32
    };
    constexpr BufferUsage  operator|(BufferUsage a, BufferUsage b) { return BufferUsage(uint32_t(a) | uint32_t(b)); }
    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) { return TextureUsage(uint32_t(a) | uint32_t(b)); }
    enum class MemoryUsage
    {
        DeviceLocal,
        Upload,
        Readback
    };
    enum class Format
    {
        RGBA8Unorm,
        RGBA8Srgb,
        RGBA16Float,
        RGBA32Float,
        RG8Unorm,
        R32Uint,
        R8Unorm,
        Depth32Float,
        Depth24Stencil8,
        R32Float,
        RG16Float,
        R16Float
    };
    enum class TextureDimension
    {
        Texture2D,
        Cube,
        Texture3D
    };
    enum class Filter
    {
        Nearest,
        Linear
    };
    enum class AddressMode
    {
        Repeat,
        ClampToEdge,
        MirrorRepeat
    };
    enum class BindingType
    {
        UniformBuffer,
        SampledTexture,
        Sampler,
        CombinedTextureSampler,
        StorageBuffer,
        ReadOnlyStorageBuffer,
        StorageTexture
    };
    enum class VertexFormat
    {
        Float,
        Float2,
        Float3,
        Float4,
        Uint,
        Uint2,
        Uint3,
        Uint4,
        Unorm8x4
    };
    enum class Topology
    {
        TriangleList,
        TriangleStrip,
        LineList,
        PointList
    };
    enum class CullMode
    {
        None,
        Front,
        Back
    };
    enum class FrontFace
    {
        CounterClockwise,
        Clockwise
    };
    enum class PolygonMode
    {
        Fill,
        Line
    };
    enum class CompareOp
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always
    };
    enum class StencilOp
    {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap
    };
    enum class BlendFactor
    {
        Zero,
        One,
        SourceColor,
        OneMinusSourceColor,
        SourceAlpha,
        OneMinusSourceAlpha,
        DestinationColor,
        OneMinusDestinationColor,
        DestinationAlpha,
        OneMinusDestinationAlpha
    };
    enum class BlendOp
    {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max
    };
    enum class LoadOp
    {
        Load,
        Clear,
        Discard
    };
    enum class StoreOp
    {
        Store,
        Discard
    };
    enum class IndexType
    {
        Uint16,
        Uint32
    };
    enum class ClipYDirection
    {
        Up,
        Down
    };
    enum class DepthRange
    {
        MinusOneToOne,
        ZeroToOne
    };
    enum class FramebufferOrigin
    {
        LowerLeft,
        UpperLeft
    };

    struct CoordinateConventions
    {
        ClipYDirection ClipY;
        DepthRange     Depth;
        // Viewport, scissor and tightly packed texture-copy row zero use this origin.
        FramebufferOrigin Origin;
    };
    struct DeviceDesc
    {
        uint32_t FramesInFlight = 2;
    };
    struct DeviceStats
    {
        // Native allocations, including allocations awaiting safe destruction.
        uint64_t LiveResources = 0, PendingDestructions = 0;
        // Actual allocator allocation sizes, including deferred frees; not heap budgets.
        std::optional<uint64_t> TextureBytes, BufferBytes;
    };
    struct BufferDesc
    {
        size_t      Bytes  = 0;
        BufferUsage Usage  = BufferUsage::Vertex;
        MemoryUsage Memory = MemoryUsage::DeviceLocal;
    };
    struct TextureDesc
    {
        uint32_t         Width = 1, Height = 1, MipLevels = 1;
        TextureDimension Dimension   = TextureDimension::Texture2D;
        Format           PixelFormat = Format::RGBA8Unorm;
        TextureUsage     Usage       = TextureUsage::Sampled;
        uint32_t         ArrayLayers = 1; // Cube has six faces; cube arrays are not supported.
        uint32_t         Samples     = 1; // 1, 2, 4 or 8. Multisampled images have one mip and are 2D.
        uint32_t         Depth       = 1; // 3D extent; array layers must be one for Texture3D.
    };
    struct SamplerDesc
    {
        Filter      Min = Filter::Linear, Mag = Filter::Linear, Mip = Filter::Linear;
        AddressMode U = AddressMode::Repeat, V = AddressMode::Repeat, W = AddressMode::Repeat;
        float       MinLod = 0, MaxLod = 1000;
        bool        CompareEnabled = false;
        CompareOp   Compare        = CompareOp::LessEqual;
        float       MipLodBias     = 0;
    };
    class Resource
    {
    public:
        virtual ~Resource() = default;
    };
    class Buffer : public Resource
    {
    public:
        virtual const BufferDesc& GetDesc() const = 0;
        // Upload buffers only. The caller must not overwrite a recorded or in-flight range.
        virtual void Write(size_t offset, const void* data, size_t bytes) = 0;
        // Readback buffers only, after WaitIdle or retirement of the frame using it.
        // Read returns owned bytes, rather than a pointer with backend-specific map lifetime.
        virtual std::vector<uint8_t> Read(size_t offset, size_t bytes) const = 0;
        virtual bool IsReadbackReady() const { return false; } // Nonblocking; false on unsupported backends.
    };
    class Texture : public Resource
    {
    public:
        virtual const TextureDesc& GetDesc() const = 0;
    };
    class Sampler : public Resource
    {
    public:
        virtual const SamplerDesc& GetDesc() const = 0;
    };
    class PipelineCreationError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
    class Shader : public Resource
    {
    public:
        std::string                     DebugName {};
        virtual uint32_t GetPushConstantBytes() const = 0;
        virtual std::array<uint32_t, 3> GetWorkgroupSize() const { return {0, 0, 0}; }
    };
    struct BindGroupLayoutEntry
    {
        uint32_t    Slot = 0;
        BindingType Type = BindingType::UniformBuffer;
    };
    struct BindGroupLayoutDesc
    {
        uint32_t                          Group = 0;
        std::vector<BindGroupLayoutEntry> Entries;
    };
    class BindGroupLayout : public Resource
    {
    public:
        virtual const BindGroupLayoutDesc& GetDesc() const = 0;
    };
    struct TextureSubresource
    {
        uint32_t Mip = 0, Face = 0; // Face is the array layer for 2D arrays.
    };
    enum class TextureAspect
    {
        Default,
        Stencil
    };
    struct BindGroupEntry
    {
        uint32_t        Slot = 0;
        Handle<Buffer>  BufferResource;
        size_t          Offset = 0, Bytes = 0; // Bytes == 0 means the remainder of the buffer.
        Handle<Texture> TextureResource;
        Handle<Sampler> SamplerResource;
        // Optional single mip/layer 2D view, including cube faces.
        std::optional<TextureSubresource> Subresource;
        TextureAspect                     Aspect = TextureAspect::Default;
    };
    struct BindGroupDesc
    {
        Handle<BindGroupLayout>     Layout;
        std::vector<BindGroupEntry> Entries;
    };
    class BindGroup : public Resource
    {};
    struct VertexBinding
    {
        uint32_t Slot = 0, Stride = 0;
        bool     PerInstance = false;
    };
    struct VertexAttribute
    {
        uint32_t     Location = 0, BufferSlot = 0, Offset = 0;
        VertexFormat Type = VertexFormat::Float3;
    };
    struct RasterizerState
    {
        CullMode    Cull              = CullMode::Back;
        FrontFace   Front             = FrontFace::CounterClockwise;
        PolygonMode Polygon           = PolygonMode::Fill;
        float       DepthBiasConstant = 0, DepthBiasSlope = 0;
    };
    struct StencilFaceState
    {
        CompareOp Compare = CompareOp::Always;
        StencilOp Fail = StencilOp::Keep, DepthFail = StencilOp::Keep, Pass = StencilOp::Keep;
        uint32_t  Reference = 0, ReadMask = 255, WriteMask = 255;
    };
    struct DepthStencilState
    {
        bool             DepthTest = false, DepthWrite = false;
        CompareOp        DepthCompare = CompareOp::Less;
        bool             StencilTest  = false;
        StencilFaceState Front, Back;
    };
    struct BlendState
    {
        bool        Enabled     = false;
        BlendFactor SourceColor = BlendFactor::One, DestinationColor = BlendFactor::Zero;
        BlendOp     ColorOp     = BlendOp::Add;
        BlendFactor SourceAlpha = BlendFactor::One, DestinationAlpha = BlendFactor::Zero;
        BlendOp     AlphaOp   = BlendOp::Add;
        uint8_t     WriteMask = 15; // R=1, G=2, B=4, A=8.
    };
    struct PipelineDesc
    {
        Handle<Shader>                       Program;
        std::vector<Handle<BindGroupLayout>> GroupLayouts;
        uint32_t                   PushConstantBytes = 0; // One byte range shared by the graphics stages; maximum 128.
        std::vector<VertexBinding> VertexBindings;
        std::vector<VertexAttribute> Attributes;
        Topology                     Primitive = Topology::TriangleList;
        RasterizerState              Rasterizer;
        DepthStencilState            DepthStencil;
        std::vector<BlendState>      Blends;
        std::vector<Format>          ColorFormats;
        // Ignored when HasDepthStencil is false.
        bool   HasDepthStencil    = false;
        Format DepthStencilFormat = Format::Depth32Float;
        uint32_t Samples            = 1;
        bool     AlphaToCoverage    = false;
        std::string DebugName {};
    };
    class Pipeline : public Resource
    {
    public:
        virtual const PipelineDesc& GetDesc() const = 0;
    };
    struct ComputePipelineDesc
    {
        Handle<Shader>                       Program;
        std::vector<Handle<BindGroupLayout>> GroupLayouts;
        uint32_t                             PushConstantBytes = 0;
        std::string                          DebugName {};
    };
    class ComputePipeline : public Resource
    {
    public:
        virtual const ComputePipelineDesc& GetDesc() const = 0;
    };
    struct ComputeLimits
    {
        std::array<uint32_t, 3> MaxGroupCount {}, MaxGroupSize {};
        uint32_t                MaxInvocations = 0, SharedMemoryBytes = 0;
    };
    struct IndirectCapabilities
    {
        bool     MultiDraw = false, DrawCount = false;
        uint32_t MaxDrawCount = 1;
    };
    struct ColorAttachment
    {
        Handle<Texture>      Target;
        TextureSubresource   Subresource;
        LoadOp               Load  = LoadOp::Clear;
        StoreOp              Store = StoreOp::Store;
        std::array<float, 4> ClearColor {0, 0, 0, 0};
        uint32_t             ClearUint = 0; // R32Uint only.
        Handle<Texture>      ResolveTarget; // Same format/extent, one sample; average resolve at EndRendering.
        TextureSubresource   ResolveSubresource;
    };
    struct DepthAttachment
    {
        Handle<Texture>    Target;
        TextureSubresource Subresource;
        LoadOp             Load         = LoadOp::Clear;
        StoreOp            Store        = StoreOp::Store;
        float              ClearDepth   = 1;
        uint32_t           ClearStencil = 0;
        Handle<Texture>    ResolveTarget; // Same format/extent, one sample; sample 0 for BOTH depth and stencil.
        TextureSubresource ResolveSubresource;
    };
    struct RenderingDesc
    {
        std::vector<ColorAttachment> Colors;
        DepthAttachment              Depth;
    };
    struct Viewport
    {
        float X = 0, Y = 0, Width = 0, Height = 0, MinDepth = 0, MaxDepth = 1;
    };
    struct Scissor
    {
        int32_t  X = 0, Y = 0;
        uint32_t Width = 0, Height = 0;
    };
    struct VertexBufferView
    {
        uint32_t       Slot = 0;
        Handle<Buffer> Resource;
        size_t         Offset = 0;
    };
    struct CommandStatistics
    {
        uint64_t DrawCalls = 0, Triangles = 0,
                 IndirectDraws = 0; // Includes instances; lines/points contribute no triangles.
    };
    class TimestampQueryPool : public Resource
    {
    public:
        // Nanoseconds per tick and number of valid bits (for wrap-safe differences).
        virtual double   GetTimestampPeriod() const = 0;
        virtual uint32_t GetValidBits() const       = 0;
        // No wait. Empty until ALL queries in this pool have completed.
        virtual std::optional<std::vector<uint64_t>> TryGetResults() const = 0;
    };
    class CommandList : public Resource
    {
    public:
        virtual std::optional<CommandStatistics> GetStatistics() const { return {}; }
        // Outside rendering. Beginning inserts before previously recorded commands. Each index
        // must be written once per submission, and a pool reused only after completion.
        virtual void WriteTimestamp(const Handle<TimestampQueryPool>&, uint32_t, bool beginning = false)
        {
            throw std::runtime_error("Timestamp queries unavailable");
        }
        // Exact, same-format copy of a complete subresource, including depth and stencil.
        virtual void
        CopyTexture(const Handle<Texture>&, TextureSubresource, const Handle<Texture>&, TextureSubresource = {})
        {
            throw std::runtime_error("Texture copy unavailable");
        }
        // One texel, no synchronous depth/stencil repacking. D24S8 returns a native
        // depth uint32 (low 24 bits), followed by a stencil byte at offset+4 (8 bytes reserved).
        virtual void CopyTextureTexelToBuffer(const Handle<Texture>&,
                                              TextureSubresource,
                                              uint32_t,
                                              uint32_t,
                                              const Handle<Buffer>&,
                                              size_t = 0)
        {
            throw std::runtime_error("Texel readback unavailable");
        }
        virtual void BindComputePipeline(const Handle<ComputePipeline>&)
        {
            throw std::runtime_error("Compute unavailable");
        }
        virtual void Dispatch(uint32_t, uint32_t = 1, uint32_t = 1) { throw std::runtime_error("Compute unavailable"); }
        virtual void DispatchIndirect(const Handle<Buffer>&, size_t = 0)
        {
            throw std::runtime_error("Compute unavailable");
        }
        virtual void DrawIndirect(const Handle<Buffer>&, size_t = 0, uint32_t = 1, uint32_t = 16)
        {
            throw std::runtime_error("Indirect draw unavailable");
        }
        virtual void DrawIndexedIndirect(const Handle<Buffer>&, size_t = 0, uint32_t = 1, uint32_t = 20)
        {
            throw std::runtime_error("Indirect draw unavailable");
        }
        virtual void
        DrawIndirectCount(const Handle<Buffer>&, size_t, const Handle<Buffer>&, size_t, uint32_t, uint32_t = 16)
        {
            throw std::runtime_error("Indirect count unavailable");
        }
        virtual void
        DrawIndexedIndirectCount(const Handle<Buffer>&, size_t, const Handle<Buffer>&, size_t, uint32_t, uint32_t = 20)
        {
            throw std::runtime_error("Indirect count unavailable");
        }
        virtual void FillBuffer(const Handle<Buffer>&, size_t, size_t, uint32_t)
        {
            throw std::runtime_error("Buffer fill unavailable");
        }
        virtual void UpdateBuffer(const Handle<Buffer>&, size_t, const void*, size_t)
        {
            throw std::runtime_error("Buffer update unavailable");
        }
        // Clear all mips/layers of a colour storage image; integer formats use ClearUint.
        virtual void ClearStorageTexture(const Handle<Texture>&, const std::array<float, 4>& = {}, uint32_t = 0)
        {
            throw std::runtime_error("Storage clear unavailable");
        }
        virtual void BeginRendering(const RenderingDesc& desc)                                        = 0;
        virtual void EndRendering()                                                                   = 0;
        virtual void SetViewport(const Viewport& viewport)                                            = 0;
        virtual void SetScissor(const Scissor& scissor)                                               = 0;
        virtual void BindPipeline(const Handle<Pipeline>& pipeline)                                   = 0;
        virtual void BindGroup(const Handle<BindGroup>& group)                                        = 0;
        virtual void PushConstants(uint32_t offset, const void* data, uint32_t bytes)                 = 0;
        virtual void BindVertexBuffers(const std::vector<VertexBufferView>& buffers)                  = 0;
        virtual void BindIndexBuffer(const Handle<Buffer>& buffer, IndexType type, size_t offset = 0) = 0;
        virtual void
        Draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
        virtual void DrawIndexed(uint32_t indices,
                                 uint32_t instances     = 1,
                                 uint32_t firstIndex    = 0,
                                 int32_t  baseVertex    = 0,
                                 uint32_t firstInstance = 0)                                                  = 0;
        virtual void CopyBuffer(const Handle<Buffer>& source,
                                size_t                sourceOffset,
                                const Handle<Buffer>& destination,
                                size_t                destinationOffset,
                                size_t                bytes)                                                  = 0;
        // Full mip/face, tightly packed rows; buffer offsets are aligned to bytes/pixel.
        // RGBA16Float uses IEEE half (8 bytes/pixel).
        // Depth32Float uses float; Depth24Stencil8 uses native uint32 words:
        // depth occupies bits 31..8 and stencil bits 7..0.
        virtual void CopyBufferToTexture(const Handle<Buffer>&  source,
                                         size_t                 offset,
                                         const Handle<Texture>& destination,
                                         TextureSubresource     subresource = {}) = 0;
        // Tightly packed color rectangle. Bounds, offset alignment and source length
        // are validated before recording. Depth/stencil partial uploads are unsupported.
        virtual void CopyBufferToTextureRegion(const Handle<Buffer>&,
                                               size_t,
                                               const Handle<Texture>&,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               uint32_t,
                                               TextureSubresource = {})
        {
            throw std::runtime_error("Partial texture upload unavailable");
        }
        virtual void CopyTextureToBuffer(const Handle<Texture>& source,
                                         TextureSubresource     subresource,
                                         const Handle<Buffer>&  destination,
                                         size_t                 offset = 0)       = 0;
        virtual void GenerateMips(const Handle<Texture>& texture)                 = 0;
    };
    class Device
    {
    public:
        virtual ~Device()                                              = default;
        virtual const DeviceDesc&     GetDesc() const                  = 0;
        virtual CoordinateConventions GetCoordinateConventions() const = 0;
        virtual DeviceStats           GetStats() const                 = 0;
        // Bit mask of supported sample counts (bits 1/2/4/8), for this format, usage and dimension.
        // Samples is ignored. Unsupported backends report single-sample only.
        virtual uint32_t GetSupportedSampleCounts(const TextureDesc&) const { return 1; }
        virtual ComputeLimits           GetComputeLimits() const { return {}; }
        virtual IndirectCapabilities    GetIndirectCapabilities() const { return {}; }
        virtual bool                    SupportsVertexStorageWrites() const { return false; }
        virtual bool                    SupportsFragmentStorageWrites() const { return false; }
        virtual Handle<ComputePipeline> CreateComputePipeline(const ComputePipelineDesc&)
        {
            throw std::runtime_error("Compute unavailable");
        }
        virtual bool     SupportsSampleRateShading() const { return false; }
        // Required byte alignment for uniform/storage buffer binding offsets.
        virtual size_t                  GetBufferBindingAlignment(BindingType type) const      = 0;
        virtual Handle<TimestampQueryPool> CreateTimestampQueryPool(uint32_t count) { return {}; }
        virtual Handle<Buffer>          CreateBuffer(const BufferDesc& desc)                   = 0;
        virtual Handle<Texture>         CreateTexture(const TextureDesc& desc)                 = 0;
        virtual Handle<Sampler>         CreateSampler(const SamplerDesc& desc = {})            = 0;
        virtual Handle<Shader>          CreateShader(const Kans::ShaderData& data)             = 0;
        virtual Handle<BindGroupLayout> CreateBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
        virtual Handle<BindGroup>       CreateBindGroup(const BindGroupDesc& desc)             = 0;
        // Equal descriptions share an immutable object. Cache entries are weak and collision checked.
        virtual Handle<Pipeline>    CreatePipeline(const PipelineDesc& desc) = 0;
        virtual Handle<CommandList> CreateCommandList()                      = 0;
        virtual void                BeginFrame()                             = 0;
        // Submit seals a list, inside or outside a frame. Resources remain retained
        // until frame retirement or WaitIdle. Window frames retire at slot reuse;
        // standalone work must eventually call WaitIdle.
        virtual void      Submit(const Handle<CommandList>& commands)             = 0;
        virtual void      EndFrame()                                              = 0;
        virtual void      WaitIdle()                                              = 0;
        // Optional sampling configures the UI texture view; ordinary RHI draws use independent samplers.
        // Exports during an ImGui frame retain the image through all platform draws
        // and presentation; asynchronous window frames keep it through GPU completion.
        virtual uintptr_t GetImGuiTextureID(const Handle<Texture>& texture,
                                            const SamplerDesc*     sampling = nullptr) const = 0;
    };
    // Objects belong to one device. All calls require its owning thread. Resource
    // handles must be released before the device, and its context must outlive it.
    // Released native resources are deferred until no recorded/submitted GPU use
    // remains. EndFrame may block to retire a frame; WaitIdle retires all submitted
    // work and makes its readback buffers readable. Unsubmitted lists retain their
    // resources but do not participate in WaitIdle. No caller barriers are needed.
} // namespace Kans::RenderHardware
