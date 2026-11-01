#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/RenderStatistics.h"

namespace Kans
{
    using GraphTexture = uint32_t;
    using GraphPass    = uint32_t;
    struct GraphBuffer
    {
        uint32_t              Index = uint32_t(-1);
        friend constexpr bool operator==(GraphBuffer a, GraphBuffer b) { return a.Index == b.Index; }
        friend constexpr bool operator!=(GraphBuffer a, GraphBuffer b) { return !(a == b); }
    };
    enum class GraphAccess
    {
        SampledRead,
        ColorWrite,
        ColorReadWrite,
        DepthWrite,
        DepthReadWrite,
        TransferRead,
        TransferWrite,
        StorageRead,
        StorageWrite,
        StorageReadWrite,
        IndirectRead,
        VertexRead,
        IndexRead,
        UniformRead
    };
    struct GraphUse
    {
        GraphTexture Texture  = uint32_t(-1);
        GraphAccess  Access   = GraphAccess::SampledRead;
        bool         IsBuffer = false;
        GraphUse()            = default;
        GraphUse(GraphTexture texture, GraphAccess access) : Texture(texture), Access(access) {}
        GraphUse(GraphBuffer buffer, GraphAccess access) : Texture(buffer.Index), Access(access), IsBuffer(true) {}
    };
    struct CompiledResourceAccess
    {
        GraphPass   Pass;
        uint32_t    ExecutionIndex;
        GraphAccess Access;
    };
    struct CompiledGraphResource
    {
        std::string                         Name;
        RenderHardware::TextureDesc         Description;
        bool                                Imported;
        uint32_t                            PhysicalSlot = uint32_t(-1);
        std::vector<CompiledResourceAccess> Accesses; // Executed access sequence (used for allocation).
        GraphDebugID                        ID = 0;
        std::vector<CompiledResourceAccess> DeclaredAccesses; // Also includes culled passes, index == -1.
        bool                                IsBuffer = false;
        RenderHardware::BufferDesc          BufferDescription;
    };
    struct CompiledGraphPass
    {
        GraphDebugID           ID = 0;
        std::string            Name;
        uint32_t               ExecutionIndex = uint32_t(-1);
        bool                   Culled         = true;
        std::vector<GraphPass> Dependencies; // Includes RAW, WAR, WAW and explicit scheduling edges.
        std::vector<GraphUse>  Uses;
        GraphPassKind          Kind = GraphPassKind::Graphics;
    };
    class GraphCapture;
    // Retain one allocation per compatible physical slot, across graph instances/frames.
    // Release unmatched descriptions after allocation (submitted commands retain old images).
    class TransientTexturePool
    {
    public:
        using Factory =
            std::function<RenderHardware::Handle<RenderHardware::Texture>(const RenderHardware::TextureDesc&)>;
        std::vector<RenderHardware::Handle<RenderHardware::Texture>>
               Acquire(const std::vector<RenderHardware::TextureDesc>& descriptions, const Factory& create);
        size_t Size() const { return m_Textures.size(); }
        void   DiscardMultisampled();

    private:
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> m_Textures;
    };
    class TransientBufferPool
    {
    public:
        using Factory =
            std::function<RenderHardware::Handle<RenderHardware::Buffer>(const RenderHardware::BufferDesc&)>;
        std::vector<RenderHardware::Handle<RenderHardware::Buffer>>
               Acquire(const std::vector<RenderHardware::BufferDesc>& descriptions, const Factory& create);
        size_t Size() const { return m_Buffers.size(); }

    private:
        std::vector<RenderHardware::Handle<RenderHardware::Buffer>> m_Buffers;
    };
    class RenderGraph;
    struct RenderGraphContext
    {
        RenderHardware::Device&                         Device;
        RenderHardware::CommandList&                    Commands;
        const RenderGraph&                              Graph;
        RenderHardware::Handle<RenderHardware::Texture> Texture(GraphTexture texture) const;
        RenderHardware::Handle<RenderHardware::Buffer>  Buffer(GraphBuffer buffer) const;
    };
    class RenderGraph
    {
    public:
        using Record = std::function<void(RenderGraphContext&)>;
        GraphBuffer  ImportBuffer(std::string name, RenderHardware::Handle<RenderHardware::Buffer> buffer);
        GraphBuffer  CreateBuffer(std::string name, RenderHardware::BufferDesc description);
        void         ExportBuffer(GraphBuffer buffer);
        GraphTexture ImportTexture(std::string name, RenderHardware::Handle<RenderHardware::Texture> texture);
        GraphTexture CreateTexture(std::string name, RenderHardware::TextureDesc description);
        GraphPass    AddPass(std::string           name,
                             std::vector<GraphUse> uses,
                             Record                record,
                             bool                  sideEffect = false,
                             GraphPassKind         kind       = GraphPassKind::Graphics);
        GraphPass    AddPass(std::string           name,
                             GraphPassKind         kind,
                             std::vector<GraphUse> uses,
                             Record                record,
                             bool                  sideEffect = false)
        {
            return AddPass(std::move(name), std::move(uses), std::move(record), sideEffect, kind);
        }
        GraphPass    AddRecordedPass(std::string                                         name,
                                     std::vector<GraphUse>                               uses,
                                     RenderHardware::Handle<RenderHardware::CommandList> commands,
                                     std::optional<double>                               cpuRecordMilliseconds = {},
                                     GraphPassKind                                       kind = GraphPassKind::Graphics);
        void         DependsOn(GraphPass pass, GraphPass prerequisite);
        // A single scheduling predecessor, without keeping its output alive. Replaces the previous one.
        void         OrderAfter(GraphPass pass, GraphPass predecessor);
        void         ExportTexture(GraphTexture texture);
        // Throws invalid_argument for invalid use or a cycle, including in culled passes.
        // No device calls: scheduling, culling, access sequences and alias plan are CPU only.
        void Compile();
        void Allocate(TransientTexturePool& pool, const TransientTexturePool::Factory& create);
        void AllocateBuffers(TransientBufferPool& pool, const TransientBufferPool::Factory& create);
        void Execute(RenderHardware::Device& device,
                     TransientTexturePool&   textures,
                     TransientBufferPool&    buffers,
                     FrameProfiler*          profiler = nullptr,
                     uint64_t                frame    = 0,
                     GraphCapture*           capture  = nullptr);
        // Install before pipeline construction to cover ExecuteImportedPrefix as well. Borrowed until ReleaseRecorders.
        void SetExecutionDebug(FrameProfiler* profiler, uint64_t frame, GraphCapture* capture = nullptr)
        {
            m_Profiler = profiler;
            m_Frame    = frame;
            m_Capture  = capture;
        }
        void Execute(RenderHardware::Device& device,
                     TransientTexturePool&   pool,
                     FrameProfiler*          profiler = nullptr,
                     uint64_t                frame    = 0,
                     GraphCapture*           capture  = nullptr);
        // Run an imported-resource prefix without touching transient allocations.
        // Its access history survives subsequent pass additions and compilation.
        void                                      ExecuteImportedPrefix(RenderHardware::Device& device);
        void                                      ReleaseRecorders()
        {
            for (auto& pass : m_Passes)
                pass.Recorder = {};
            m_Profiler = nullptr;
            m_Capture  = nullptr;
        }
        std::weak_ptr<const void>                       Lifetime() const { return m_Lifetime; }
        size_t                                    PassCount() const { return m_Passes.size(); }
        const std::vector<GraphPass>&             ExecutionOrder() const { return m_Order; }
        const std::vector<CompiledGraphResource>& Resources() const { return m_CompiledResources; }
        const std::vector<CompiledGraphPass>&           Passes() const { return m_CompiledPasses; }
        const std::vector<PassStatistics>&              Statistics() const { return m_Statistics; }
        const std::string&                        PassName(GraphPass pass) const { return m_Passes.at(pass).Name; }
        RenderHardware::Handle<RenderHardware::Texture> Texture(GraphTexture texture) const;
        RenderHardware::Handle<RenderHardware::Buffer>  Buffer(GraphBuffer buffer) const;

    private:
        struct Resource
        {
            std::string                                     Name;
            RenderHardware::TextureDesc                     Description;
            RenderHardware::Handle<RenderHardware::Texture> Image;
            bool                                            Imported = false, Exported = false;
            bool                                            IsBuffer = false;
            RenderHardware::BufferDesc                      BufferDescription;
            RenderHardware::Handle<RenderHardware::Buffer>  Data;
        };
        struct Pass
        {
            std::string                                         Name;
            std::vector<GraphUse>                               Uses;
            Record                                              Recorder;
            bool                                                SideEffect;
            std::vector<GraphPass>                              Dependencies;
            RenderHardware::Handle<RenderHardware::CommandList> Commands;
            bool                                                Submitted = false;
            GraphPass                                           OrderPredecessor = GraphPass(-1);
            std::optional<double>                               CpuRecordMilliseconds;
            std::optional<RenderHardware::CommandStatistics>    Draws;
            GraphPassKind                                       Kind = GraphPassKind::Graphics;
        };
        std::vector<Resource>                    m_Resources;
        std::vector<Pass>                        m_Passes;
        std::vector<GraphPass>                   m_Order;
        std::vector<CompiledGraphResource>       m_CompiledResources;
        std::vector<RenderHardware::TextureDesc> m_Slots;
        std::vector<RenderHardware::BufferDesc>                    m_BufferSlots;
        TransientBufferPool                                        m_BufferPool;
        std::vector<GraphPass>                   m_Submitted;
        std::vector<CompiledGraphPass>                             m_CompiledPasses;
        std::vector<PassStatistics>                                m_Statistics;
        RenderHardware::Handle<RenderHardware::TimestampQueryPool> m_Queries;
        void                                     ExecutePass(RenderHardware::Device& device, GraphPass pass);
        FrameProfiler*                           m_Profiler  = nullptr;
        GraphCapture*                            m_Capture   = nullptr;
        uint64_t                                 m_Frame     = 0;
        uint32_t                                 m_QueryBase = 0;
        void                                     BeginQueries(RenderHardware::Device& device);
        std::shared_ptr<const void>              m_Lifetime = std::make_shared<char>();
        bool                                     m_Compiled = false;
    };
} // namespace Kans
