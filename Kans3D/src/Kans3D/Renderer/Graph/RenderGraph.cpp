#include "kspch.h"
#include "RenderGraph.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include "GraphCapture.h"

namespace Kans
{
    namespace
    {
        bool Equal(const RenderHardware::TextureDesc& a, const RenderHardware::TextureDesc& b)
        {
            return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
                   a.Dimension == b.Dimension && a.ArrayLayers == b.ArrayLayers && a.PixelFormat == b.PixelFormat &&
                   a.Usage == b.Usage && a.Samples == b.Samples && a.Depth == b.Depth;
        }
        bool Equal(const RenderHardware::BufferDesc& a, const RenderHardware::BufferDesc& b)
        {
            return a.Bytes == b.Bytes && a.Usage == b.Usage && a.Memory == b.Memory;
        }
        bool Reads(GraphAccess a)
        {
            return a == GraphAccess::SampledRead || a == GraphAccess::TransferRead ||
                   a == GraphAccess::ColorReadWrite || a == GraphAccess::DepthReadWrite ||
                   a == GraphAccess::StorageRead || a == GraphAccess::StorageReadWrite ||
                   a == GraphAccess::IndirectRead || a == GraphAccess::VertexRead || a == GraphAccess::IndexRead ||
                   a == GraphAccess::UniformRead;
        }
        bool Writes(GraphAccess a)
        {
            return a == GraphAccess::ColorWrite || a == GraphAccess::ColorReadWrite || a == GraphAccess::DepthWrite ||
                   a == GraphAccess::DepthReadWrite || a == GraphAccess::TransferWrite ||
                   a == GraphAccess::StorageWrite || a == GraphAccess::StorageReadWrite;
        }
        void Validate(const RenderHardware::BufferDesc& d, GraphAccess a, const std::string& name)
        {
            using namespace RenderHardware;
            BufferUsage needed;
            switch (a)
            {
                case GraphAccess::StorageRead:
                case GraphAccess::StorageWrite:
                case GraphAccess::StorageReadWrite:
                    needed = BufferUsage::Storage;
                    break;
                case GraphAccess::IndirectRead:
                    needed = BufferUsage::Indirect;
                    break;
                case GraphAccess::VertexRead:
                    needed = BufferUsage::Vertex;
                    break;
                case GraphAccess::IndexRead:
                    needed = BufferUsage::Index;
                    break;
                case GraphAccess::UniformRead:
                    needed = BufferUsage::Uniform;
                    break;
                case GraphAccess::TransferRead:
                    needed = BufferUsage::CopySource;
                    break;
                case GraphAccess::TransferWrite:
                    needed = BufferUsage::CopyDestination;
                    break;
                default:
                    throw std::invalid_argument("RenderGraph: incompatible buffer access '" + name + "'");
            }
            if (!(uint32_t(d.Usage) & uint32_t(needed)))
                throw std::invalid_argument("RenderGraph: incompatible buffer usage '" + name + "'");
        }
        void Validate(const RenderHardware::TextureDesc& d, GraphAccess a, const std::string& name)
        {
            using namespace RenderHardware;
            if (a == GraphAccess::IndirectRead || a == GraphAccess::VertexRead || a == GraphAccess::IndexRead ||
                a == GraphAccess::UniformRead)
                throw std::invalid_argument("RenderGraph: buffer access on texture '" + name + "'");
            TextureUsage needed = (a == GraphAccess::StorageRead || a == GraphAccess::StorageWrite ||
                                   a == GraphAccess::StorageReadWrite) ?
                                      TextureUsage::Storage :
                                  a == GraphAccess::SampledRead   ? TextureUsage::Sampled :
                                  a == GraphAccess::TransferRead  ? TextureUsage::CopySource :
                                  a == GraphAccess::TransferWrite ? TextureUsage::CopyDestination :
                                  (a == GraphAccess::DepthWrite || a == GraphAccess::DepthReadWrite) ?
                                                                    TextureUsage::DepthStencil :
                                                                    TextureUsage::RenderTarget;
            const bool   depth  = d.PixelFormat == Format::Depth24Stencil8 || d.PixelFormat == Format::Depth32Float;
            if (!(uint32_t(d.Usage) & uint32_t(needed)) || (needed == TextureUsage::DepthStencil && !depth) ||
                (needed == TextureUsage::RenderTarget && depth))
                throw std::invalid_argument("RenderGraph: incompatible access for texture '" + name + "'");
        }
    } // namespace
    std::vector<RenderHardware::Handle<RenderHardware::Texture>>
    TransientTexturePool::Acquire(const std::vector<RenderHardware::TextureDesc>& descriptions, const Factory& create)
    {
        // A frame without transient consumers (e.g. deselection) must not discard
        // allocations needed again at the same viewport on the next frame.
        if (descriptions.empty())
            return {};
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> textures;
        std::vector<bool>                                            used(m_Textures.size(), false);
        for (const auto& desc : descriptions)
        {
            RenderHardware::Handle<RenderHardware::Texture> image;
            for (size_t i = 0; i < m_Textures.size(); ++i)
                if (!used[i] && Equal(desc, m_Textures[i]->GetDesc()))
                {
                    used[i] = true;
                    image   = m_Textures[i];
                    break;
                }
            if (!image)
                image = create(desc);
            if (!image || !Equal(desc, image->GetDesc()))
                throw std::invalid_argument("RenderGraph: texture factory returned an incompatible allocation");
            textures.push_back(std::move(image));
        }
        m_Textures = textures;
        return textures;
    }
    std::vector<RenderHardware::Handle<RenderHardware::Buffer>>
    TransientBufferPool::Acquire(const std::vector<RenderHardware::BufferDesc>& descriptions, const Factory& create)
    {
        if (descriptions.empty())
            return {};
        std::vector<RenderHardware::Handle<RenderHardware::Buffer>> buffers;
        std::vector<bool>                                           used(m_Buffers.size());
        for (const auto& desc : descriptions)
        {
            RenderHardware::Handle<RenderHardware::Buffer> buffer;
            for (size_t i = 0; i < m_Buffers.size(); ++i)
                if (!used[i] && Equal(desc, m_Buffers[i]->GetDesc()))
                {
                    used[i] = true;
                    buffer  = m_Buffers[i];
                    break;
                }
            if (!buffer)
                buffer = create(desc);
            if (!buffer || !Equal(desc, buffer->GetDesc()))
                throw std::invalid_argument("RenderGraph: incompatible buffer allocation");
            buffers.push_back(std::move(buffer));
        }
        m_Buffers = buffers;
        return buffers;
    }
    GraphBuffer RenderGraph::ImportBuffer(std::string name, RenderHardware::Handle<RenderHardware::Buffer> buffer)
    {
        if (!buffer)
            throw std::invalid_argument("RenderGraph: null imported buffer");
        for (uint32_t i = 0; i < m_Resources.size(); ++i)
            if (m_Resources[i].Imported && m_Resources[i].Data == buffer)
                return {i};
        auto  handle      = CreateBuffer(std::move(name), buffer->GetDesc());
        auto& resource    = m_Resources[handle.Index];
        resource.Imported = true;
        resource.Data     = std::move(buffer);
        return handle;
    }
    GraphBuffer RenderGraph::CreateBuffer(std::string name, RenderHardware::BufferDesc description)
    {
        if (!description.Bytes || !uint32_t(description.Usage))
            throw std::invalid_argument("RenderGraph: invalid buffer description");
        m_Compiled = false;
        Resource resource;
        resource.Name              = std::move(name);
        resource.IsBuffer          = true;
        resource.BufferDescription = description;
        m_Resources.push_back(std::move(resource));
        return {uint32_t(m_Resources.size() - 1)};
    }
    void RenderGraph::ExportBuffer(GraphBuffer buffer)
    {
        if (buffer.Index >= m_Resources.size() || !m_Resources[buffer.Index].IsBuffer)
            throw std::invalid_argument("RenderGraph: invalid exported buffer");
        m_Compiled                         = false;
        m_Resources[buffer.Index].Exported = true;
    }
    void TransientTexturePool::DiscardMultisampled()
    {
        m_Textures.erase(std::remove_if(m_Textures.begin(),
                                        m_Textures.end(),
                                        [](const auto& t) { return t->GetDesc().Samples > 1; }),
                         m_Textures.end());
    }
    GraphTexture RenderGraph::ImportTexture(std::string name, RenderHardware::Handle<RenderHardware::Texture> image)
    {
        if (!image)
            throw std::invalid_argument("RenderGraph: null imported texture '" + name + "'");
        // One logical identity for an imported image, so repeated sampling creates real dependencies.
        for (uint32_t i = 0; i < m_Resources.size(); ++i)
            if (m_Resources[i].Imported && m_Resources[i].Image == image)
                return i;
        m_Compiled = false;
        auto desc  = image->GetDesc();
        m_Resources.push_back({std::move(name), desc, std::move(image), true, false});
        return uint32_t(m_Resources.size() - 1);
    }
    GraphTexture RenderGraph::CreateTexture(std::string name, RenderHardware::TextureDesc desc)
    {
        if (!desc.Width || !desc.Height || !desc.Depth ||
            (desc.Dimension == RenderHardware::TextureDimension::Texture3D ? desc.ArrayLayers != 1 : desc.Depth != 1) ||
            !desc.MipLevels || !desc.ArrayLayers ||
            (desc.Dimension == RenderHardware::TextureDimension::Cube && desc.ArrayLayers != 1) ||
            (desc.Dimension == RenderHardware::TextureDimension::Cube && desc.Width != desc.Height))
            throw std::invalid_argument("RenderGraph: invalid texture description '" + name + "'");
        m_Compiled = false;
        m_Resources.push_back({std::move(name), desc, {}, false, false});
        return uint32_t(m_Resources.size() - 1);
    }
    GraphPass RenderGraph::AddPass(std::string           name,
                                   std::vector<GraphUse> uses,
                                   Record                record,
                                   bool                  sideEffect,
                                   GraphPassKind         kind)
    {
        m_Compiled = false;
        m_Passes.push_back({std::move(name), std::move(uses), std::move(record), sideEffect, {}});
        m_Passes.back().Kind = kind;
        return uint32_t(m_Passes.size() - 1);
    }
    GraphPass RenderGraph::AddRecordedPass(std::string                                         name,
                                           std::vector<GraphUse>                               uses,
                                           RenderHardware::Handle<RenderHardware::CommandList> commands,
                                           std::optional<double>                               cpuRecordMilliseconds,
                                           GraphPassKind                                       kind)
    {
        if (!commands)
            throw std::invalid_argument("RenderGraph: null recorded commands");
        auto pass                            = AddPass(std::move(name), std::move(uses), {}, false, kind);
        m_Passes[pass].Commands = std::move(commands);
        m_Passes[pass].CpuRecordMilliseconds = cpuRecordMilliseconds;
        return pass;
    }
    void RenderGraph::DependsOn(GraphPass pass, GraphPass prerequisite)
    {
        if (pass >= m_Passes.size() || prerequisite >= m_Passes.size())
            throw std::invalid_argument("RenderGraph: invalid pass dependency");
        m_Compiled = false;
        m_Passes[pass].Dependencies.push_back(prerequisite);
    }
    void RenderGraph::ExportTexture(GraphTexture texture)
    {
        if (texture >= m_Resources.size() || m_Resources[texture].IsBuffer)
            throw std::invalid_argument("RenderGraph: invalid exported texture");
        m_Compiled                    = false;
        m_Resources[texture].Exported = true;
    }
    void RenderGraph::OrderAfter(GraphPass pass, GraphPass predecessor)
    {
        if (pass >= m_Passes.size() || predecessor >= m_Passes.size() || pass == predecessor)
            throw std::invalid_argument("RenderGraph: invalid ordering predecessor");
        m_Compiled                      = false;
        m_Passes[pass].OrderPredecessor = predecessor;
    }
    void RenderGraph::Compile()
    {
        m_Compiled = false;
        m_Order.clear();
        m_Slots.clear();
        m_BufferSlots.clear();
        m_CompiledResources.clear();
        m_CompiledPasses.clear();
        m_Statistics.clear();
        const uint32_t                     count = uint32_t(m_Passes.size()), none = uint32_t(-1);
        std::vector<std::set<GraphPass>>   edges(count), inputs(count);
        std::vector<uint32_t>              latest(m_Resources.size(), none), firstWriter(m_Resources.size(), none);
        std::vector<std::vector<uint32_t>> readers(m_Resources.size());
        for (uint32_t p = 0; p < count; ++p)
        {
            std::set<GraphTexture> seen;
            for (auto use : m_Passes[p].Uses)
            {
                if (use.Texture >= m_Resources.size())
                    throw std::invalid_argument("RenderGraph: invalid texture in pass '" + m_Passes[p].Name + "'");
                if (!seen.insert(use.Texture).second)
                    throw std::invalid_argument("RenderGraph: duplicate texture use in pass '" + m_Passes[p].Name +
                                                "'");
                const auto& resource = m_Resources[use.Texture];
                if (use.IsBuffer != resource.IsBuffer)
                    throw std::invalid_argument("RenderGraph: resource handle type mismatch");
                if (m_Passes[p].Kind != GraphPassKind::Graphics &&
                    (use.Access == GraphAccess::ColorWrite || use.Access == GraphAccess::ColorReadWrite ||
                     use.Access == GraphAccess::DepthWrite || use.Access == GraphAccess::DepthReadWrite))
                    throw std::invalid_argument("RenderGraph: attachment access requires graphics pass");
                if (m_Passes[p].Kind == GraphPassKind::Transfer && use.Access != GraphAccess::TransferRead &&
                    use.Access != GraphAccess::TransferWrite)
                    throw std::invalid_argument("RenderGraph: transfer pass requires transfer access");
                if (resource.IsBuffer)
                    Validate(resource.BufferDescription, use.Access, resource.Name);
                else
                    Validate(resource.Description, use.Access, resource.Name);
                if (Writes(use.Access) && firstWriter[use.Texture] == none)
                    firstWriter[use.Texture] = p;
            }
        }
        auto edge = [&](uint32_t from, uint32_t to) {
            if (from != none && from != to)
                edges[from].insert(to);
        };
        for (uint32_t p = 0; p < count; ++p)
        {
            edge(m_Passes[p].OrderPredecessor, p);
            for (auto dependency : m_Passes[p].Dependencies)
            {
                if (dependency == p)
                    throw std::invalid_argument("RenderGraph: cycle at pass '" + m_Passes[p].Name + "'");
                edge(dependency, p);
                inputs[p].insert(dependency);
            }
            for (auto use : m_Passes[p].Uses)
            {
                const auto r = use.Texture;
                if (Reads(use.Access))
                {
                    uint32_t producer = latest[r];
                    if (producer == none && !m_Resources[r].Imported)
                    {
                        producer = firstWriter[r];
                        if (producer == none || producer == p || Writes(use.Access))
                            throw std::invalid_argument("RenderGraph: read before initialization of texture '" +
                                                        m_Resources[r].Name + "' in pass '" + m_Passes[p].Name + "'");
                    }
                    if (producer != none)
                    {
                        edge(producer, p);
                        inputs[p].insert(producer);
                    }
                    readers[r].push_back(p);
                }
                if (Writes(use.Access))
                {
                    edge(latest[r], p); // WAW; pure overwrite does not keep the older producer alive.
                    for (auto reader : readers[r])
                        if (!(latest[r] == none && !m_Resources[r].Imported && firstWriter[r] == p))
                            edge(reader, p); // WAR, including imported initial contents.
                    readers[r].clear();
                    latest[r] = p;
                }
            }
        }
        // Validate the whole graph before culling, with deterministic declaration-order ties.
        std::vector<uint32_t> indegree(count, 0), sorted;
        for (const auto& successors : edges)
            for (auto p : successors)
                ++indegree[p];
        std::set<uint32_t> available;
        for (uint32_t p = 0; p < count; ++p)
            if (!indegree[p])
                available.insert(p);
        while (!available.empty())
        {
            auto p = *available.begin();
            available.erase(available.begin());
            sorted.push_back(p);
            for (auto next : edges[p])
                if (!--indegree[next])
                    available.insert(next);
        }
        if (sorted.size() != count)
            throw std::invalid_argument("RenderGraph: dependency cycle");
        std::vector<bool>             live(count, false);
        std::function<void(uint32_t)> keep = [&](uint32_t p) {
            if (p == none || live[p])
                return;
            live[p] = true;
            for (auto dependency : inputs[p])
                keep(dependency);
        };
        for (uint32_t p = 0; p < count; ++p)
            if (m_Passes[p].SideEffect)
                keep(p);
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
            if (m_Resources[r].Exported)
            {
                if (!m_Resources[r].Imported && latest[r] == none)
                    throw std::invalid_argument("RenderGraph: exported texture has no producer '" +
                                                m_Resources[r].Name + "'");
                keep(latest[r]);
            }
        for (auto p : sorted)
            if (live[p])
                m_Order.push_back(p);
        for (auto& r : m_Resources)
        {
            m_CompiledResources.push_back({r.Name, r.Description, r.Imported, none, {}});
            m_CompiledResources.back().IsBuffer          = r.IsBuffer;
            m_CompiledResources.back().BufferDescription = r.BufferDescription;
        }
        for (uint32_t i = 0; i < m_Order.size(); ++i)
            for (auto use : m_Passes[m_Order[i]].Uses)
                m_CompiledResources[use.Texture].Accesses.push_back({m_Order[i], i, use.Access});
        for (uint32_t p = 0; p < count; ++p)
        {
            auto&    pass       = m_Passes[p];
            uint32_t occurrence = 0;
            for (uint32_t previous = 0; previous < p; ++previous)
                occurrence += m_Passes[previous].Name == pass.Name;
            CompiledGraphPass info;
            info.ID     = GraphNameID(pass.Name, false, occurrence);
            info.Name   = pass.Name;
            info.Culled = !live[p];
            info.Uses   = pass.Uses;
            info.Kind   = pass.Kind;
            if (live[p])
                info.ExecutionIndex = uint32_t(std::find(m_Order.begin(), m_Order.end(), p) - m_Order.begin());
            for (uint32_t dependency = 0; dependency < count; ++dependency)
                if (edges[dependency].count(p))
                    info.Dependencies.push_back(dependency);
            m_Statistics.push_back(
                {info.ID, info.Name, info.ExecutionIndex, info.Culled, pass.CpuRecordMilliseconds, {}, pass.Draws});
            m_Statistics.back().Kind = pass.Kind;
            m_CompiledPasses.push_back(std::move(info));
            for (auto use : pass.Uses)
                m_CompiledResources[use.Texture].DeclaredAccesses.push_back(
                    {p, m_CompiledPasses.back().ExecutionIndex, use.Access});
        }
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
        {
            uint32_t occurrence = 0;
            for (uint32_t previous = 0; previous < r; ++previous)
                occurrence += m_Resources[previous].Name == m_Resources[r].Name;
            m_CompiledResources[r].ID = GraphNameID(m_Resources[r].Name, true, occurrence);
        }
        // Exported transients remain live through output consumption at the end of execution.
        std::vector<uint32_t> resources, slotLast, bufferSlotLast;
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
            if (!m_Resources[r].Imported && !m_CompiledResources[r].Accesses.empty())
                resources.push_back(r);
        std::stable_sort(resources.begin(), resources.end(), [&](uint32_t a, uint32_t b) {
            return m_CompiledResources[a].Accesses.front().ExecutionIndex <
                   m_CompiledResources[b].Accesses.front().ExecutionIndex;
        });
        for (auto r : resources)
        {
            auto&    compiled = m_CompiledResources[r];
            uint32_t slot = 0, start = compiled.Accesses.front().ExecutionIndex;
            auto&        last      = compiled.IsBuffer ? bufferSlotLast : slotLast;
            const size_t slotCount = compiled.IsBuffer ? m_BufferSlots.size() : m_Slots.size();
            for (; slot < slotCount; ++slot)
                if (last[slot] < start && (compiled.IsBuffer ? Equal(m_BufferSlots[slot], compiled.BufferDescription) :
                                                               Equal(m_Slots[slot], compiled.Description)))
                    break;
            const auto end =
                m_Resources[r].Exported ? uint32_t(m_Order.size()) : compiled.Accesses.back().ExecutionIndex;
            if (slot == slotCount)
            {
                if (compiled.IsBuffer)
                    m_BufferSlots.push_back(compiled.BufferDescription);
                else
                    m_Slots.push_back(compiled.Description);
                last.push_back(end);
            }
            else
                last[slot] = end;
            compiled.PhysicalSlot = slot;
        }
        if (m_Order.size() < m_Submitted.size() || !std::equal(m_Submitted.begin(), m_Submitted.end(), m_Order.begin()))
            throw std::invalid_argument("RenderGraph: new dependencies reorder an already submitted prefix");
        m_Compiled = true;
    }
    void RenderGraph::Allocate(TransientTexturePool& pool, const TransientTexturePool::Factory& create)
    {
        if (!m_Compiled)
            throw std::logic_error("RenderGraph: Compile required before allocation");
        auto textures = pool.Acquire(m_Slots, create);
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
            if (!m_Resources[r].Imported && !m_Resources[r].IsBuffer)
            {
                auto slot            = m_CompiledResources[r].PhysicalSlot;
                m_Resources[r].Image = slot == uint32_t(-1) ? nullptr : textures.at(slot);
            }
    }
    void RenderGraph::AllocateBuffers(TransientBufferPool& pool, const TransientBufferPool::Factory& create)
    {
        if (!m_Compiled)
            throw std::logic_error("RenderGraph: Compile required before allocation");
        const auto buffers = pool.Acquire(m_BufferSlots, create);
        for (uint32_t r = 0; r < m_Resources.size(); ++r)
            if (!m_Resources[r].Imported && m_Resources[r].IsBuffer)
            {
                const auto slot     = m_CompiledResources[r].PhysicalSlot;
                m_Resources[r].Data = slot == uint32_t(-1) ? nullptr : buffers.at(slot);
            }
    }
    void RenderGraph::GuardPipelineFailures(size_t                                  first,
                                            std::function<void(const std::string&)> failure,
                                            std::function<bool()>                   skip)
    {
        for (size_t i = first; i < m_Passes.size(); ++i)
        {
            // An emitter's more precise boundary takes priority over a feature's outer boundary.
            if (m_Passes[i].PipelineFailure)
                continue;
            m_Passes[i].PipelineFailure = failure;
            m_Passes[i].Skip            = skip;
        }
    }
    void RenderGraph::ExecutePass(RenderHardware::Device& device, GraphPass p)
    {
        auto& pass = m_Passes[p];
        if (pass.Submitted)
            return;
        auto commands = pass.Commands;
        if (pass.Skip && pass.Skip())
        {
            // Eagerly recorded passes belong to the same optional feature boundary too.
            pass.Commands.reset();
            commands                   = device.CreateCommandList();
            pass.CpuRecordMilliseconds = 0;
        }
        else if (!commands)
        {
            const auto start = RenderClockMilliseconds();
            commands         = device.CreateCommandList();
            RenderGraphContext context {device, *commands, *this};
            try
            {
                if (pass.Recorder)
                    pass.Recorder(context);
            }
            catch (const RenderHardware::PipelineCreationError& error)
            {
                if (!pass.PipelineFailure)
                    throw;
                pass.PipelineFailure(error.what());
                // The failed recorder may be inside BeginRendering; never submit its partial work.
                commands = device.CreateCommandList();
            }
            pass.CpuRecordMilliseconds = RenderClockMilliseconds() - start;
        }
        pass.Draws                  = commands->GetStatistics();
        auto& stats                 = m_Statistics.at(p);
        stats.CpuRecordMilliseconds = pass.CpuRecordMilliseconds;
        stats.Draws                 = pass.Draws;
        if (m_Queries)
        {
            commands->WriteTimestamp(m_Queries, (stats.ExecutionIndex - m_QueryBase) * 2, true);
            commands->WriteTimestamp(m_Queries, (stats.ExecutionIndex - m_QueryBase) * 2 + 1);
        }
        device.Submit(commands);
        pass.Submitted = true;
        m_Submitted.push_back(p);
    }
    void RenderGraph::BeginQueries(RenderHardware::Device& device)
    {
        m_QueryBase = uint32_t(m_Submitted.size());
        if (!m_Profiler)
            return;
        auto pending = m_Statistics;
        for (auto p : m_Submitted)
            pending[p].Culled = true;
        m_Queries = m_Profiler->Begin(device, m_Frame, pending);
    }
    void RenderGraph::Execute(RenderHardware::Device& device,
                              TransientTexturePool&   pool,
                              FrameProfiler*          profiler,
                              uint64_t                frame,
                              GraphCapture*           capture)
    {
        Execute(device, pool, m_BufferPool, profiler, frame, capture);
    }
    void RenderGraph::Execute(RenderHardware::Device& device,
                              TransientTexturePool&   pool,
                              TransientBufferPool&    buffers,
                              FrameProfiler*          profiler,
                              uint64_t                frame,
                              GraphCapture*           capture)
    {
        AllocateBuffers(buffers, [&](const RenderHardware::BufferDesc& desc) { return device.CreateBuffer(desc); });
        Allocate(pool, [&](const RenderHardware::TextureDesc& desc) { return device.CreateTexture(desc); });
        if (profiler)
            m_Profiler = profiler;
        if (capture)
            m_Capture = capture;
        if (frame)
            m_Frame = frame;
        BeginQueries(device);
        for (auto p : m_Order)
        {
            const bool submitted = m_Passes[p].Submitted;
            ExecutePass(device, p);
            if (m_Capture && !submitted)
                m_Capture->AfterPass(*this, p);
        }
        m_Queries.reset();
    }
    void RenderGraph::ExecuteImportedPrefix(RenderHardware::Device& device)
    {
        if (!m_Compiled)
            throw std::logic_error("RenderGraph: Compile required before prefix execution");
        for (auto p : m_Order)
            for (auto use : m_Passes[p].Uses)
                if (!m_Resources[use.Texture].Imported)
                    throw std::invalid_argument("RenderGraph: prefix execution cannot use transient textures");
        BeginQueries(device);
        for (auto p : m_Order)
        {
            const bool submitted = m_Passes[p].Submitted;
            // An already observable operation cannot subsequently be culled.
            m_Passes[p].SideEffect = true;
            ExecutePass(device, p);
            if (m_Capture && !submitted)
                m_Capture->AfterPass(*this, p);
        }
        m_Queries.reset();
    }
    RenderHardware::Handle<RenderHardware::Buffer> RenderGraph::Buffer(GraphBuffer buffer) const
    {
        const auto& resource = m_Resources.at(buffer.Index);
        if (!resource.IsBuffer)
            throw std::invalid_argument("RenderGraph: buffer handle refers to texture");
        return resource.Data;
    }
    RenderHardware::Handle<RenderHardware::Buffer> RenderGraphContext::Buffer(GraphBuffer buffer) const
    {
        return Graph.Buffer(buffer);
    }
    RenderHardware::Handle<RenderHardware::Texture> RenderGraph::Texture(GraphTexture texture) const
    {
        const auto& resource = m_Resources.at(texture);
        if (resource.IsBuffer)
            throw std::invalid_argument("RenderGraph: texture handle refers to buffer");
        return resource.Image;
    }
    RenderHardware::Handle<RenderHardware::Texture> RenderGraphContext::Texture(GraphTexture texture) const
    {
        return Graph.Texture(texture);
    }
} // namespace Kans
