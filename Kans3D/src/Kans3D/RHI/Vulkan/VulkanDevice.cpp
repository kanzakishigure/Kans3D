#include "kspch.h"
#include <imgui.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <backends/imgui_impl_vulkan.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/Core/Window.h"
#include "Kans3D/RHI/PipelineKey.h"
#include "VulkanInternal.h"
#include "VulkanPresentation.h"
#include "VulkanTestProbes.h"

namespace Kans::RenderHardware
{
    namespace
    {
        using namespace VulkanDetail;
        struct Commands;
        struct VKSet;
        struct VKTexture;
        struct State
        {
            Context         Native;
            VkDescriptorSetLayout                                   UITextureLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayout                                   UISamplerLayout = VK_NULL_HANDLE;
            std::unordered_map<uintptr_t, std::weak_ptr<VKTexture>> UITextures;
            VkPipelineCache                                       PipelineCache = VK_NULL_HANDLE;
            std::vector<VkDescriptorPool>                         DescriptorPools;
            std::unordered_map<std::string, std::weak_ptr<VKSet>> DescriptorCache;
            uint64_t        PipelineCreations = 0, DescriptorAllocations = 0, GroupCreationNanoseconds = 0;
            uint64_t        PipelineAttempts = 0;
            std::string     FailPipeline;
            uint64_t        WaitIdleCalls = 0;
            uint64_t        BufferCreations = 0;
            VulkanTestProbes::BufferWriteObserver BufferWrites;
            VulkanTestProbes::BarrierObserver     Barriers;
            DeviceStats     Stats;
            bool            InFrame = false, Alive = true;
            std::thread::id Thread = std::this_thread::get_id();
            struct Frame
            {
                VkCommandPool Pool  = VK_NULL_HANDLE;
                VkFence       Fence = VK_NULL_HANDLE;
                std::vector<std::shared_ptr<Commands>> Lists;
                std::vector<Handle<Texture>>           UIImages;
            };
            std::vector<Frame>                     Frames;
            Frame                                  Standalone;
            size_t                                 FrameIndex = 0;
            std::vector<std::function<void()>>     Deletions;
            std::vector<std::shared_ptr<Commands>> Submitted;
            State(const DeviceDesc& desc, const VulkanDeviceOptions& options) :
                Native(options), Frames(desc.FramesInFlight)
            {
                try
                {
                    for (auto& frame : Frames)
                        CreateFrame(frame);
                    CreateFrame(Standalone);
                    VkDescriptorSetLayoutBinding binding {
                        0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
                    VkDescriptorSetLayoutCreateInfo layout {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                    layout.bindingCount = 1;
                    layout.pBindings    = &binding;
                    Check(vkCreateDescriptorSetLayout(Native.Logical, &layout, nullptr, &UITextureLayout));
                    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                    Check(vkCreateDescriptorSetLayout(Native.Logical, &layout, nullptr, &UISamplerLayout));
                    VkPipelineCacheCreateInfo cache {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
                    Check(vkCreatePipelineCache(Native.Logical, &cache, nullptr, &PipelineCache));
                }
                catch (...)
                {
                    DestroyFrames();
                    throw;
                }
            }
            void CheckThread() const
            {
                Require(std::this_thread::get_id() == Thread, "Device requires its owning thread");
            }
            void CreateFrame(Frame& frame)
            {
                VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pool.queueFamilyIndex = Native.QueueFamily;
                Check(vkCreateCommandPool(Native.Logical, &pool, nullptr, &frame.Pool));
                VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                Check(vkCreateFence(Native.Logical, &fence, nullptr, &frame.Fence));
            }
            void DestroyFrames()
            {
                auto destroy = [this](Frame& frame) {
                    if (frame.Fence)
                        vkDestroyFence(Native.Logical, frame.Fence, nullptr);
                    if (frame.Pool)
                        vkDestroyCommandPool(Native.Logical, frame.Pool, nullptr);
                    frame = {};
                };
                for (auto& frame : Frames)
                    destroy(frame);
                destroy(Standalone);
            }
            ~State()
            {
                DestroyFrames();
                vkDestroyDescriptorSetLayout(Native.Logical, UITextureLayout, nullptr);
                vkDestroyDescriptorSetLayout(Native.Logical, UISamplerLayout, nullptr);
                for (auto pool : DescriptorPools)
                    vkDestroyDescriptorPool(Native.Logical, pool, nullptr);
                if (PipelineCache)
                    vkDestroyPipelineCache(Native.Logical, PipelineCache, nullptr);
            }
            std::pair<VkDescriptorPool, VkDescriptorSet> AllocateSet(VkDescriptorSetLayout layout)
            {
                for (auto pool : DescriptorPools)
                {
                    VkDescriptorSetAllocateInfo allocation {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    allocation.descriptorPool     = pool;
                    allocation.descriptorSetCount = 1;
                    allocation.pSetLayouts        = &layout;
                    VkDescriptorSet set;
                    auto            result = vkAllocateDescriptorSets(Native.Logical, &allocation, &set);
                    if (result == VK_SUCCESS)
                    {
                        ++DescriptorAllocations;
                        return {pool, set};
                    }
                    if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
                        Check(result);
                }
                VkDescriptorPoolSize       sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
                                                      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
                                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4096},
                                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                                      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
                                                      {VK_DESCRIPTOR_TYPE_SAMPLER, 4096},
                                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
                VkDescriptorPoolCreateInfo pool {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pool.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pool.maxSets       = 1024;
                pool.poolSizeCount = 8;
                pool.pPoolSizes    = sizes;
                VkDescriptorPool native;
                Check(vkCreateDescriptorPool(Native.Logical, &pool, nullptr, &native));
                DescriptorPools.push_back(native);
                return AllocateSet(layout);
            }
            Frame& Current()
            {
                return (InFrame || Native.Options.AsynchronousWindowFrames) ? Frames[FrameIndex] : Standalone;
            }
            void   Drain()
            {
                // Destructors may enqueue more work while retained command captures
                // are being released. Drain only after queue/fence completion.
                while (!Deletions.empty())
                {
                    auto deletions = std::move(Deletions);
                    Deletions.clear();
                    for (auto& destroy : deletions)
                    {
                        destroy();
                        --Stats.LiveResources;
                        --Stats.PendingDestructions;
                    }
                }
            }
            void Retire();
            void BeginWindowFrame();
            void FinishWindowFrame();
        };
        struct Allocation
        {
            std::shared_ptr<State> Owner;
            std::function<void()>  Delete;
            explicit Allocation(std::shared_ptr<State> state) : Owner(std::move(state))
            {
                ++Owner->Stats.LiveResources;
            }
            ~Allocation()
            {
                if (Owner->Alive && Delete)
                {
                    Owner->Deletions.push_back(std::move(Delete));
                    ++Owner->Stats.PendingDestructions;
                }
                else if (!Delete)
                    --Owner->Stats.LiveResources;
            }
        };
        struct VKBuffer final : Buffer
        {
            Allocation    Memory;
            BufferDesc    Desc;
            VkBuffer      Native = VK_NULL_HANDLE;
            VmaAllocation Alloc  = VK_NULL_HANDLE;
            void*         Mapped = nullptr;
            Access        Last, Writer, Visible;
            size_t        PendingWrites = 0;
            VKBuffer(std::shared_ptr<State> owner, BufferDesc desc) : Memory(std::move(owner)), Desc(desc)
            {
                Require(desc.Bytes && uint32_t(desc.Usage) && !(uint32_t(desc.Usage) & ~127u),
                        "Invalid buffer description");
                VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                buffer.size = desc.Bytes;
                if (Has(desc.Usage, BufferUsage::Indirect))
                    buffer.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                if (Has(desc.Usage, BufferUsage::Vertex))
                    buffer.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                if (Has(desc.Usage, BufferUsage::Index))
                    buffer.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                if (Has(desc.Usage, BufferUsage::Uniform))
                    buffer.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                if (Has(desc.Usage, BufferUsage::Storage))
                    buffer.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                if (Has(desc.Usage, BufferUsage::CopySource))
                    buffer.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                if (Has(desc.Usage, BufferUsage::CopyDestination))
                    buffer.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocation {};
                allocation.usage = desc.Memory == MemoryUsage::DeviceLocal ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE :
                                                                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                if (desc.Memory != MemoryUsage::DeviceLocal)
                    allocation.flags =
                        VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        (desc.Memory == MemoryUsage::Upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT :
                                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
                VmaAllocationInfo info {};
                auto              allocator = Memory.Owner->Native.Allocator;
                Check(vmaCreateBuffer(allocator, &buffer, &allocation, &Native, &Alloc, &info));
                ++Memory.Owner->BufferCreations;
                Mapped        = info.pMappedData;
                Memory.Owner->Native.BufferAllocationBytes += info.size;
                Memory.Delete =
                    [allocator, buffer = Native, alloc = Alloc, owner = Memory.Owner.get(), bytes = info.size] {
                        vmaDestroyBuffer(allocator, buffer, alloc);
                        owner->Native.BufferAllocationBytes -= bytes;
                    };
            }
            const BufferDesc& GetDesc() const override { return Desc; }
            bool              IsReadbackReady() const override
            {
                Memory.Owner->CheckThread();
                return Desc.Memory == MemoryUsage::Readback && PendingWrites == 0;
            }
            void              Write(size_t offset, const void* data, size_t bytes) override
            {
                Memory.Owner->CheckThread();
                Require(Desc.Memory == MemoryUsage::Upload && (data || !bytes), "Write requires an upload buffer");
                Range(Desc.Bytes, offset, bytes);
                if (bytes)
                    std::memcpy(static_cast<uint8_t*>(Mapped) + offset, data, bytes);
                Check(vmaFlushAllocation(Memory.Owner->Native.Allocator, Alloc, offset, bytes));
                // Host writes precede submission; the next inferred use includes
                // the host access in its dependency (also for noncoherent memory).
                Last = Writer = {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT};
                Visible       = {};
                if (Memory.Owner->BufferWrites)
                    Memory.Owner->BufferWrites(Desc, offset, data, bytes);
            }
            std::vector<uint8_t> Read(size_t offset, size_t bytes) const override
            {
                Memory.Owner->CheckThread();
                Require(Desc.Memory == MemoryUsage::Readback && PendingWrites == 0,
                        "Readback requires frame retirement or WaitIdle");
                Range(Desc.Bytes, offset, bytes);
                Check(vmaInvalidateAllocation(Memory.Owner->Native.Allocator, Alloc, offset, bytes));
                const auto* start = static_cast<const uint8_t*>(Mapped) + offset;
                return {start, start + bytes};
            }
        };
        struct VKQueries final : TimestampQueryPool
        {
            Allocation  Memory;
            VkQueryPool Native = VK_NULL_HANDLE;
            uint32_t    Count;
            uint32_t    PendingWrites = 0;
            bool        HasWrites     = false;
            VKQueries(std::shared_ptr<State> owner, uint32_t count) : Memory(std::move(owner)), Count(count)
            {
                Require(count > 0, "Empty timestamp pool");
                VkQueryPoolCreateInfo desc {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                desc.queryType  = VK_QUERY_TYPE_TIMESTAMP;
                desc.queryCount = count;
                Check(vkCreateQueryPool(Memory.Owner->Native.Logical, &desc, nullptr, &Native));
                Memory.Delete = [device = Memory.Owner->Native.Logical, pool = Native] {
                    vkDestroyQueryPool(device, pool, nullptr);
                };
            }
            double GetTimestampPeriod() const override
            {
                return Memory.Owner->Native.Properties.limits.timestampPeriod;
            }
            uint32_t GetValidBits() const override { return Memory.Owner->Native.TimestampValidBits; }
            std::optional<std::vector<uint64_t>> TryGetResults() const override
            {
                Memory.Owner->CheckThread();
                if (PendingWrites || !HasWrites)
                    return {};
                std::vector<std::array<uint64_t, 2>> data(Count);
                auto result = vkGetQueryPoolResults(Memory.Owner->Native.Logical,
                                                    Native,
                                                    0,
                                                    Count,
                                                    data.size() * sizeof(data[0]),
                                                    data.data(),
                                                    sizeof(data[0]),
                                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                if (result == VK_NOT_READY)
                    return {};
                Check(result);
                std::vector<uint64_t> ticks;
                for (auto entry : data)
                {
                    if (!entry[1])
                        return {};
                    ticks.push_back(entry[0]);
                }
                return ticks;
            }
        };
        uint32_t Width(const TextureDesc& d, uint32_t mip) { return std::max(1u, d.Width >> mip); }
        uint32_t Height(const TextureDesc& d, uint32_t mip) { return std::max(1u, d.Height >> mip); }
        uint32_t Depth(const TextureDesc& d, uint32_t mip) { return std::max(1u, d.Depth >> mip); }
        size_t   TextureBytes(const TextureDesc& d, TextureSubresource sub)
        {
            Require(sub.Mip < d.MipLevels && sub.Face < (d.Dimension == TextureDimension::Cube ? 6u : d.ArrayLayers),
                    "Invalid texture subresource");
            return size_t(Width(d, sub.Mip)) * Height(d, sub.Mip) * Depth(d, sub.Mip) * Info(d.PixelFormat).Bytes;
        }
        uint32_t SupportedSamples(const Context& context, const TextureDesc& desc)
        {
            auto              f     = Info(desc.PixelFormat);
            VkImageUsageFlags usage = 0;
            if (Has(desc.Usage, TextureUsage::Storage))
                usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (Has(desc.Usage, TextureUsage::Sampled))
                usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            if (Has(desc.Usage, TextureUsage::RenderTarget))
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (Has(desc.Usage, TextureUsage::DepthStencil))
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if (Has(desc.Usage, TextureUsage::CopySource))
                usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (Has(desc.Usage, TextureUsage::CopyDestination))
                usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!usage)
                return 0;
            VkImageFormatProperties properties {};
            if (vkGetPhysicalDeviceImageFormatProperties(
                    context.Physical,
                    f.Native,
                    desc.Dimension == TextureDimension::Texture3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                    VK_IMAGE_TILING_OPTIMAL,
                    usage,
                    desc.Dimension == TextureDimension::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                    &properties) != VK_SUCCESS)
                return 0;
            uint32_t counts = properties.sampleCounts & 15u;
            if (desc.MipLevels != 1 || desc.Dimension != TextureDimension::Texture2D)
                return counts & 1u;
            const auto& limits = context.Properties.limits;
            if (Has(desc.Usage, TextureUsage::RenderTarget))
                counts &= limits.framebufferColorSampleCounts;
            if (Has(desc.Usage, TextureUsage::DepthStencil))
            {
                counts &= limits.framebufferDepthSampleCounts;
                if (f.Stencil())
                    counts &= limits.framebufferStencilSampleCounts;
            }
            if (Has(desc.Usage, TextureUsage::Sampled))
            {
                counts &= f.Depth()                           ? limits.sampledImageDepthSampleCounts :
                          desc.PixelFormat == Format::R32Uint ? limits.sampledImageIntegerSampleCounts :
                                                                limits.sampledImageColorSampleCounts;
            }
            if (Has(desc.Usage, TextureUsage::Sampled) && f.Stencil())
                counts &= limits.sampledImageStencilSampleCounts;
            return counts;
        }
        struct VKTexture final : Texture
        {
            struct UIView
            {
                SamplerDesc     Desc;
                Handle<Sampler> Sampling;
                VkDescriptorSet Set;
                VkDescriptorSet SamplerSet;
            };
            std::vector<UIView>      UIViews;
            Allocation               Memory;
            TextureDesc              Desc;
            VkImage                  Native = VK_NULL_HANDLE;
            VkImageView              SampledView = VK_NULL_HANDLE;
            VmaAllocation            Alloc  = VK_NULL_HANDLE;
            std::vector<VkImageView> Views;
            struct SampleView
            {
                TextureSubresource Sub;
                TextureAspect      Aspect;
                VkImageView        Native;
            };
            std::shared_ptr<std::vector<SampleView>> SampleViews;
            VkImageView                              SampleViewFor(TextureSubresource sub, TextureAspect aspect)
            {
                TextureBytes(Desc, sub);
                Require(aspect != TextureAspect::Stencil || Info(Desc.PixelFormat).Stencil(), "No stencil aspect");
                if (!SampleViews)
                {
                    SampleViews   = std::make_shared<std::vector<SampleView>>();
                    auto previous = std::move(Memory.Delete);
                    Memory.Delete =
                        [previous = std::move(previous), device = Memory.Owner->Native.Logical, views = SampleViews] {
                            for (auto& view : *views)
                                vkDestroyImageView(device, view.Native, nullptr);
                            previous();
                        };
                }
                for (auto& view : *SampleViews)
                    if (view.Sub.Mip == sub.Mip && view.Sub.Face == sub.Face && view.Aspect == aspect)
                        return view.Native;
                VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image    = Native;
                view.viewType =
                    Desc.Dimension == TextureDimension::Texture3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
                view.format   = Info(Desc.PixelFormat).Native;
                auto mask =
                    aspect == TextureAspect::Stencil ?
                        VK_IMAGE_ASPECT_STENCIL_BIT :
                        (Info(Desc.PixelFormat).Depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
                view.subresourceRange = {VkImageAspectFlags(mask), sub.Mip, 1, sub.Face, 1};
                VkImageView result;
                Check(vkCreateImageView(Memory.Owner->Native.Logical, &view, nullptr, &result));
                SampleViews->push_back({sub, aspect, result});
                return result;
            }
            std::vector<Access>      Last, Writer, Visible;
            size_t                   Index(TextureSubresource sub) const { return sub.Face * Desc.MipLevels + sub.Mip; }
            VKTexture(std::shared_ptr<State> owner, TextureDesc desc) : Memory(std::move(owner)), Desc(desc)
            {
                Require(desc.Width && desc.Height && desc.Depth && desc.MipLevels && desc.ArrayLayers &&
                            (desc.Dimension == TextureDimension::Texture3D ?
                                 (desc.ArrayLayers == 1 && desc.Samples == 1 &&
                                  !Has(desc.Usage, TextureUsage::RenderTarget) &&
                                  !Has(desc.Usage, TextureUsage::DepthStencil)) :
                                 desc.Depth == 1) &&
                            (desc.Dimension != TextureDimension::Cube || desc.ArrayLayers == 1) &&
                            uint32_t(desc.Usage) && !(uint32_t(desc.Usage) & ~63u),
                        "Invalid texture description");
                auto f = Info(desc.PixelFormat);
                if (Has(desc.Usage, TextureUsage::Storage))
                {
                    VkPhysicalDeviceFeatures features {};
                    vkGetPhysicalDeviceFeatures(Memory.Owner->Native.Physical, &features);
                    Require((desc.PixelFormat != Format::RG16Float && desc.PixelFormat != Format::R16Float &&
                             desc.PixelFormat != Format::R8Unorm) ||
                                features.shaderStorageImageExtendedFormats,
                            "Storage format needs shaderStorageImageExtendedFormats");
                    VkFormatProperties properties {};
                    vkGetPhysicalDeviceFormatProperties(Memory.Owner->Native.Physical, f.Native, &properties);
                    Require(!f.Depth() && desc.Samples == 1 &&
                                (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
                            "Storage texture format is unsupported by device");
                }
                Require((desc.Samples == 1 || desc.Samples == 2 || desc.Samples == 4 || desc.Samples == 8) &&
                            (SupportedSamples(Memory.Owner->Native, desc) & desc.Samples),
                        "Unsupported texture sample count");
                uint32_t maxMips = 1;
                for (uint32_t size = std::max({desc.Width, desc.Height, desc.Depth}); size > 1; size >>= 1)
                    ++maxMips;
                Require(desc.MipLevels <= maxMips &&
                            (desc.Dimension != TextureDimension::Cube || desc.Width == desc.Height),
                        "Invalid texture mip count or cube extent");
                Require((!Has(desc.Usage, TextureUsage::RenderTarget) || !f.Depth()) &&
                            (!Has(desc.Usage, TextureUsage::DepthStencil) || f.Depth()),
                        "Attachment format/usage mismatch");
                VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                image.flags       = desc.Dimension == TextureDimension::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
                image.imageType   = desc.Dimension == TextureDimension::Texture3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
                image.format      = f.Native;
                image.extent      = {desc.Width, desc.Height, desc.Depth};
                image.mipLevels   = desc.MipLevels;
                image.arrayLayers = desc.Dimension == TextureDimension::Cube ? 6 : desc.ArrayLayers;
                image.samples     = VkSampleCountFlagBits(desc.Samples);
                image.tiling      = VK_IMAGE_TILING_OPTIMAL;
                if (Has(desc.Usage, TextureUsage::Storage))
                    image.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (Has(desc.Usage, TextureUsage::Sampled))
                    image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                if (Has(desc.Usage, TextureUsage::RenderTarget))
                    image.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (Has(desc.Usage, TextureUsage::DepthStencil))
                    image.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                if (Has(desc.Usage, TextureUsage::CopySource))
                    image.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (Has(desc.Usage, TextureUsage::CopyDestination))
                    image.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                VkImageFormatProperties properties;
                Check(vkGetPhysicalDeviceImageFormatProperties(Memory.Owner->Native.Physical,
                                                               image.format,
                                                               image.imageType,
                                                               image.tiling,
                                                               image.usage,
                                                               image.flags,
                                                               &properties));
                Require(desc.Width <= properties.maxExtent.width && desc.Height <= properties.maxExtent.height &&
                            desc.Depth <= properties.maxExtent.depth && desc.ArrayLayers <= properties.maxArrayLayers &&
                            desc.MipLevels <= properties.maxMipLevels,
                        "Texture dimensions exceed device format limits");
                VmaAllocationCreateInfo allocation {};
                allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                auto allocator   = Memory.Owner->Native.Allocator;
                auto device      = Memory.Owner->Native.Logical;
                VmaAllocationInfo allocationInfo {};
                Check(vmaCreateImage(allocator, &image, &allocation, &Native, &Alloc, &allocationInfo));
                try
                {
                    Last.resize(size_t(image.arrayLayers) * image.mipLevels);
                    Writer.resize(Last.size());
                    Visible.resize(Last.size());
                    if (image.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
                    {
                        Views.resize(Last.size(), VK_NULL_HANDLE);
                        for (uint32_t face = 0; face < image.arrayLayers; ++face)
                            for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
                            {
                                VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                                view.image            = Native;
                                view.viewType = desc.Dimension == TextureDimension::Texture3D ? VK_IMAGE_VIEW_TYPE_3D :
                                                                                                VK_IMAGE_VIEW_TYPE_2D;
                                view.format           = f.Native;
                                view.subresourceRange = {f.Aspects, mip, 1, face, 1};
                                Check(vkCreateImageView(device, &view, nullptr, &Views[Index({mip, face})]));
                            }
                    }
                    if (Has(desc.Usage, TextureUsage::Sampled))
                    {
                        VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        view.image = Native;
                        view.viewType =
                            desc.Dimension == TextureDimension::Texture3D ?
                                VK_IMAGE_VIEW_TYPE_3D :
                            desc.Dimension == TextureDimension::Cube ?
                                VK_IMAGE_VIEW_TYPE_CUBE :
                                (desc.ArrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
                        view.format           = f.Native;
                        view.subresourceRange = {f.Depth() ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : f.Aspects,
                                                 0,
                                                 desc.MipLevels,
                                                 0,
                                                 image.arrayLayers};
                        Check(vkCreateImageView(device, &view, nullptr, &SampledView));
                    }
                    Memory.Delete = [allocator,
                                     device,
                                     image   = Native,
                                     alloc   = Alloc,
                                     views   = Views,
                                     sampled = SampledView,
                                     owner   = Memory.Owner.get(),
                                     bytes   = allocationInfo.size] {
                        owner->Native.TextureAllocationBytes -= bytes;
                        if (sampled)
                            vkDestroyImageView(device, sampled, nullptr);
                        for (auto view : views)
                            vkDestroyImageView(device, view, nullptr);
                        vmaDestroyImage(allocator, image, alloc);
                    };
                    Memory.Owner->Native.TextureAllocationBytes += allocationInfo.size;
                }
                catch (...)
                {
                    if (SampledView)
                        vkDestroyImageView(device, SampledView, nullptr);
                    for (auto view : Views)
                        if (view)
                            vkDestroyImageView(device, view, nullptr);
                    vmaDestroyImage(allocator, Native, Alloc);
                    throw;
                }
            }
            const TextureDesc& GetDesc() const override { return Desc; }
        };
        struct VKSampler final : Sampler
        {
            SamplerDesc        Desc;
            const SamplerDesc& GetDesc() const override { return Desc; }
            Allocation Memory;
            VkSampler  Native = VK_NULL_HANDLE;
            bool       Comparison = false;
            VKSampler(std::shared_ptr<State> owner, const SamplerDesc& desc) : Desc(desc), Memory(std::move(owner))
            {
                Require(std::isfinite(desc.MinLod) && std::isfinite(desc.MaxLod) && desc.MinLod >= 0 &&
                            desc.MaxLod >= desc.MinLod,
                        "Invalid sampler LOD range");
                auto address = [](AddressMode value) {
                    switch (value)
                    {
                        case AddressMode::Repeat:
                            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                        case AddressMode::ClampToEdge:
                            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                        case AddressMode::MirrorRepeat:
                            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                    }
                    throw std::invalid_argument("Invalid sampler address mode");
                };
                VkSamplerCreateInfo sampler {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                sampler.minFilter = desc.Min == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                sampler.magFilter = desc.Mag == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                sampler.mipmapMode =
                    desc.Mip == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                sampler.addressModeU = address(desc.U);
                sampler.addressModeV = address(desc.V);
                sampler.addressModeW = address(desc.W);
                Require(uint32_t(desc.Compare) <= uint32_t(CompareOp::Always), "Invalid sampler comparison");
                Comparison            = desc.CompareEnabled;
                sampler.compareEnable = desc.CompareEnabled;
                sampler.compareOp     = static_cast<VkCompareOp>(uint32_t(desc.Compare));
                sampler.mipLodBias    = std::clamp(desc.MipLodBias,
                                                   -Memory.Owner->Native.Properties.limits.maxSamplerLodBias,
                                                   Memory.Owner->Native.Properties.limits.maxSamplerLodBias);
                sampler.minLod       = desc.MinLod;
                sampler.maxLod       = desc.MaxLod;
                auto device          = Memory.Owner->Native.Logical;
                Check(vkCreateSampler(device, &sampler, nullptr, &Native));
                Memory.Delete = [device, sampler = Native] { vkDestroySampler(device, sampler, nullptr); };
            }
        };
        template<class T, class U>
        std::shared_ptr<T> Cast(const Handle<U>& resource, const std::shared_ptr<State>& state)
        {
            auto value = std::dynamic_pointer_cast<T>(resource);
            Require(value && value->Memory.Owner == state, "Resource belongs to another device or backend");
            return value;
        }
#include "VulkanGraphics.inl"
        constexpr Access TransferRead {VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        constexpr Access TransferWrite {VK_PIPELINE_STAGE_2_COPY_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        // NV2: buffer fills/updates execute at CLEAR, but older validation layers
        // classify them at COPY (Khronos VVL #7406). Cover both in every dependency,
        // including the producer retained after a read and readback host exports.
        constexpr Access BufferClearWrite {VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT};
        bool             Writes(VkAccessFlags2 access)
        {
            return (access & (VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_MEMORY_WRITE_BIT)) != 0;
        }
        constexpr Access HostRead {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        struct Execution
        {
            std::shared_ptr<State>              Owner;
            VkCommandPool                       Pool;
            VkCommandBuffer                     Native = VK_NULL_HANDLE;
            std::vector<Handle<Resource>>       Temporary;
            std::vector<VkBufferMemoryBarrier2> BufferBarriers;
            std::vector<VkImageMemoryBarrier2>  ImageBarriers;
            // Transition planning is independent of emission. A future compiled
            // graph access sequence can call Use for a whole pass, then flush
            // once, while command-inferred uses remain the correctness fallback.
            void FlushBarriers()
            {
                if (BufferBarriers.empty() && ImageBarriers.empty())
                    return;
                VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = uint32_t(BufferBarriers.size());
                dependency.pBufferMemoryBarriers    = BufferBarriers.data();
                dependency.imageMemoryBarrierCount  = uint32_t(ImageBarriers.size());
                dependency.pImageMemoryBarriers     = ImageBarriers.data();
                if (Owner->Barriers)
                    Owner->Barriers(BufferBarriers, ImageBarriers);
                vkCmdPipelineBarrier2(Native, &dependency);
                BufferBarriers.clear();
                ImageBarriers.clear();
            }
            explicit Execution(std::shared_ptr<State> state) : Owner(std::move(state)), Pool(Owner->Current().Pool)
            {
                Begin();
            }
            void Begin()
            {
                VkCommandBufferAllocateInfo allocation {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                allocation.commandPool        = Pool;
                allocation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocation.commandBufferCount = 1;
                Check(vkAllocateCommandBuffers(Owner->Native.Logical, &allocation, &Native));
                VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                Check(vkBeginCommandBuffer(Native, &begin));
            }
            void Submit()
            {
                FlushBarriers();
                Check(vkEndCommandBuffer(Native));
                VkCommandBufferSubmitInfo buffer {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                buffer.commandBuffer = Native;
                VkSubmitInfo2 submit {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                submit.commandBufferInfoCount = 1;
                submit.pCommandBufferInfos    = &buffer;
                Check(vkQueueSubmit2(Owner->Native.Queue, 1, &submit, VK_NULL_HANDLE));
            }
            void FlushAndWait()
            {
                // Packed depth/stencil requires CPU repacking between aspect copies.
                // Waiting here does NOT retire public readback tokens or lists.
                Submit();
                Check(vkQueueWaitIdle(Owner->Native.Queue));
                Begin();
            }
            std::shared_ptr<VKBuffer> Staging(size_t bytes, MemoryUsage memory)
            {
                auto value = std::make_shared<VKBuffer>(
                    Owner, BufferDesc {bytes, BufferUsage::CopySource | BufferUsage::CopyDestination, memory});
                Temporary.push_back(value);
                return value;
            }
            void Use(VKBuffer& buffer, Access next)
            {
                // Keep the producer until the next write. Visibility at one consumer stage
                // does not publish its contents to a different stage or access class.
                const bool write = Writes(next.Mask);
                if (!write && (next.Stage & ~buffer.Visible.Stage) == 0 && (next.Mask & ~buffer.Visible.Mask) == 0)
                {
                    buffer.Last.Stage |= next.Stage;
                    buffer.Last.Mask |= next.Mask;
                    return;
                }
                auto barrierFrom = [&](Access source) {
                    if (!source.Stage)
                        return;
                    VkBufferMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                    barrier.srcStageMask        = source.Stage;
                    barrier.srcAccessMask       = source.Mask;
                    barrier.dstStageMask        = next.Stage;
                    barrier.dstAccessMask       = next.Mask;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer                                            = buffer.Native;
                    barrier.size                                              = VK_WHOLE_SIZE;
                    BufferBarriers.push_back(barrier);
                };
                // Separate WAR execution and WAW memory dependencies retain every reader
                // without discarding the original producer's availability dependency.
                if (write && !Writes(buffer.Last.Mask))
                    barrierFrom(buffer.Last);
                barrierFrom(buffer.Writer);
                if (write)
                {
                    buffer.Writer = buffer.Last = next;
                    buffer.Visible              = {};
                }
                else
                {
                    if (Writes(buffer.Last.Mask))
                        buffer.Last = {};
                    buffer.Last.Stage |= next.Stage;
                    buffer.Last.Mask |= next.Mask;
                    buffer.Visible.Stage |= next.Stage;
                    buffer.Visible.Mask |= next.Mask;
                }
            }
            void Use(VKTexture& texture, TextureSubresource sub, Access next)
            {
                const auto index   = texture.Index(sub);
                auto&      last    = texture.Last.at(index);
                auto&      writer  = texture.Writer.at(index);
                auto&      visible = texture.Visible.at(index);
                const bool write = Writes(next.Mask), layout = last.Layout != next.Layout;
                if (!layout && !write && (next.Stage & ~visible.Stage) == 0 && (next.Mask & ~visible.Mask) == 0)
                {
                    last.Stage |= next.Stage;
                    last.Mask |= next.Mask;
                    return;
                }
                VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask        = writer.Stage | ((write || layout) ? last.Stage : 0);
                barrier.srcAccessMask       = writer.Mask | ((write || layout) ? last.Mask : 0);
                barrier.dstStageMask        = next.Stage;
                barrier.dstAccessMask       = next.Mask;
                barrier.oldLayout           = last.Layout;
                barrier.newLayout           = next.Layout;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image                                             = texture.Native;
                barrier.subresourceRange = {Info(texture.Desc.PixelFormat).Aspects, sub.Mip, 1, sub.Face, 1};
                if (layout || barrier.srcStageMask)
                    ImageBarriers.push_back(barrier);
                if (write)
                {
                    writer = last = next;
                    visible       = {};
                }
                else
                {
                    if (layout || Writes(last.Mask))
                        last = {0, 0, next.Layout};
                    if (layout)
                        visible = {};
                    last.Stage |= next.Stage;
                    last.Mask |= next.Mask;
                    visible.Stage |= next.Stage;
                    visible.Mask |= next.Mask;
                }
            }
            void Copy(VKBuffer& source, size_t from, VKBuffer& destination, size_t to, size_t bytes)
            {
                if (&source == &destination)
                    Use(source,
                        {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT});
                else
                {
                    Use(source, TransferRead);
                    Use(destination, TransferWrite);
                }
                FlushBarriers();
                VkBufferCopy copy {from, to, bytes};
                vkCmdCopyBuffer(Native, source.Native, destination.Native, 1, &copy);
                if (destination.Desc.Memory == MemoryUsage::Readback)
                {
                    Use(destination, HostRead);
                    FlushBarriers();
                    // Host visibility is an export, not the last GPU access.
                    // Preserve the producer for later queue dependencies.
                }
            }
            void ImageCopy(VKBuffer&          buffer,
                           size_t             offset,
                           VKTexture&         texture,
                           TextureSubresource sub,
                           VkImageAspectFlags aspect,
                           bool               upload)
            {
                Use(buffer, upload ? TransferRead : TransferWrite);
                Use(texture, sub, upload ? TransferWrite : TransferRead);
                FlushBarriers();
                VkBufferImageCopy region {};
                region.bufferOffset     = offset;
                region.imageSubresource = {aspect, sub.Mip, sub.Face, 1};
                region.imageExtent      = {
                    Width(texture.Desc, sub.Mip), Height(texture.Desc, sub.Mip), Depth(texture.Desc, sub.Mip)};
                if (upload)
                    vkCmdCopyBufferToImage(
                        Native, buffer.Native, texture.Native, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                else
                {
                    vkCmdCopyImageToBuffer(
                        Native, texture.Native, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.Native, 1, &region);
                    if (buffer.Desc.Memory == MemoryUsage::Readback)
                    {
                        Use(buffer, HostRead);
                        FlushBarriers();
                    }
                }
            }
            void PackedCopy(VKBuffer& buffer, size_t offset, VKTexture& texture, TextureSubresource sub, bool upload)
            {
                const size_t bytes = TextureBytes(texture.Desc, sub), pixels = bytes / 4;
                auto         depth   = Staging(bytes, upload ? MemoryUsage::Upload : MemoryUsage::Readback);
                auto         stencil = Staging(pixels, upload ? MemoryUsage::Upload : MemoryUsage::Readback);
                if (upload)
                {
                    auto packed = Staging(bytes, MemoryUsage::Readback);
                    Copy(buffer, offset, *packed, 0, bytes);
                    FlushAndWait();
                    auto                  raw = packed->Read(0, bytes);
                    std::vector<uint32_t> d(pixels);
                    std::vector<uint8_t>  s(pixels);
                    for (size_t i = 0; i < pixels; ++i)
                    {
                        uint32_t value;
                        std::memcpy(&value, raw.data() + i * 4, 4);
                        d[i] = value >> 8;
                        s[i] = uint8_t(value);
                    }
                    depth->Write(0, d.data(), bytes);
                    stencil->Write(0, s.data(), pixels);
                    ImageCopy(*depth, 0, texture, sub, VK_IMAGE_ASPECT_DEPTH_BIT, true);
                    ImageCopy(*stencil, 0, texture, sub, VK_IMAGE_ASPECT_STENCIL_BIT, true);
                }
                else
                {
                    ImageCopy(*depth, 0, texture, sub, VK_IMAGE_ASPECT_DEPTH_BIT, false);
                    ImageCopy(*stencil, 0, texture, sub, VK_IMAGE_ASPECT_STENCIL_BIT, false);
                    FlushAndWait();
                    auto                  d = depth->Read(0, bytes), s = stencil->Read(0, pixels);
                    std::vector<uint32_t> raw(pixels);
                    for (size_t i = 0; i < pixels; ++i)
                    {
                        uint32_t value;
                        std::memcpy(&value, d.data() + i * 4, 4);
                        raw[i] = (value << 8) | s[i];
                    }
                    auto packed = Staging(bytes, MemoryUsage::Upload);
                    packed->Write(0, raw.data(), bytes);
                    Copy(*packed, 0, buffer, offset, bytes);
                }
            }
        };
        VkAttachmentLoadOp Load(LoadOp op)
        {
            switch (op)
            {
                case LoadOp::Load:
                    return VK_ATTACHMENT_LOAD_OP_LOAD;
                case LoadOp::Clear:
                    return VK_ATTACHMENT_LOAD_OP_CLEAR;
                case LoadOp::Discard:
                    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }
            throw std::invalid_argument("Invalid load operation");
        }
        VkAttachmentStoreOp Store(StoreOp op)
        {
            switch (op)
            {
                case StoreOp::Store:
                    return VK_ATTACHMENT_STORE_OP_STORE;
                case StoreOp::Discard:
                    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
            }
            throw std::invalid_argument("Invalid store operation");
        }
        struct ScopeUses
        {
            std::map<VKBuffer*, std::pair<std::shared_ptr<VKBuffer>, Access>> Buffers;
            std::map<VKTexture*, std::shared_ptr<VKTexture>>                  Textures;
            std::map<std::pair<VKTexture*, size_t>, std::pair<std::shared_ptr<VKTexture>, Access>> StorageTextures;
            bool DepthWrites = false, PreserveAttachments = false, HasDraws = false;
            void Buffer(const std::shared_ptr<VKBuffer>& buffer, Access access)
            {
                auto& value = Buffers[buffer.get()];
                value.first = buffer;
                value.second.Stage |= access.Stage;
                value.second.Mask |= access.Mask;
            }
            void Prepare(Execution& execution, const DepthAttachment& depth)
            {
                for (auto& [key, value] : StorageTextures)
                    execution.Use(*value.first,
                                  {uint32_t(key.second % value.first->Desc.MipLevels),
                                   uint32_t(key.second / value.first->Desc.MipLevels)},
                                  value.second);
                for (auto& [key, value] : Buffers)
                    execution.Use(*value.first, value.second);
                for (auto& [key, texture] : Textures)
                    for (uint32_t face = 0;
                         face < (texture->Desc.Dimension == TextureDimension::Cube ? 6u : texture->Desc.ArrayLayers);
                         ++face)
                        for (uint32_t mip = 0; mip < texture->Desc.MipLevels; ++mip)
                        {
                            if (texture.get() == depth.Target.get() && face == depth.Subresource.Face &&
                                mip == depth.Subresource.Mip)
                                continue;
                            execution.Use(
                                *texture,
                                {mip, face},
                                {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                 Info(texture->Desc.PixelFormat).Depth() &&
                                         Has(texture->Desc.Usage, TextureUsage::DepthStencil) ?
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                        }
            }
        };
        struct Commands final : CommandList
        {
            std::shared_ptr<State>                       Owner;
            bool                                         Sealed = false, Rendering = false;
            CommandStatistics                            Statistics;
            std::optional<CommandStatistics>             GetStatistics() const override { return Statistics; }
            void                                         CountDraw(uint32_t count, uint32_t instances)
            {
                ++Statistics.DrawCalls;
                auto primitive = BoundPipeline->Desc.Primitive;
                if (primitive == Topology::TriangleList)
                    Statistics.Triangles += uint64_t(count / 3) * instances;
                if (primitive == Topology::TriangleStrip && count > 2)
                    Statistics.Triangles += uint64_t(count - 2) * instances;
            }
            uint32_t                                     RenderingWidth = 0, RenderingHeight = 0;
            std::vector<std::function<void(Execution&)>> Recorded;
            std::vector<std::shared_ptr<VKBuffer>>       Readbacks;
            std::vector<std::shared_ptr<VKQueries>>      QueryWrites;
            std::vector<Handle<Resource>>                Temporary;
            std::vector<Handle<Resource>>                RetainedBindings;
            std::shared_ptr<VKPipeline>                  BoundPipeline;
            std::shared_ptr<VKComputePipeline>           BoundCompute;
            std::shared_ptr<VKBuffer>                    BoundIndex;
            IndexType                                    IndexFormat = IndexType::Uint32;
            size_t                                       IndexOffset = 0;
            std::map<uint32_t, std::shared_ptr<VKGroup>> Groups;
            std::map<uint32_t, VertexBufferView>         Vertices;
            std::array<uint8_t, 128>                     PushData {};
            RenderingDesc                                RenderingInfo;
            std::shared_ptr<ScopeUses>                   Uses;
            bool                                         StorageWritten = false;
            Viewport                                     CurrentViewport;
            Scissor                                      CurrentScissor;
            explicit Commands(std::shared_ptr<State> owner) : Owner(std::move(owner)) {}
            ~Commands() { Retire(); }
            void Retire()
            {
                for (const auto& b : Readbacks)
                    --b->PendingWrites;
                Readbacks.clear();
                for (auto& query : QueryWrites)
                    --query->PendingWrites;
                QueryWrites.clear();
                Recorded.clear();
                Temporary.clear();
                BoundPipeline.reset();
                BoundCompute.reset();
                BoundIndex.reset();
                Groups.clear();
                Vertices.clear();
                Uses.reset();
                RenderingInfo = {};
                RetainedBindings.clear();
            }
            void Mutable()
            {
                Owner->CheckThread();
                Require(!Sealed, "Command list is sealed");
            }
            void Outside()
            {
                Mutable();
                Require(!Rendering, "Transfer requires no rendering scope");
            }
            void Mark(const std::shared_ptr<VKBuffer>& b)
            {
                if (b->Desc.Memory == MemoryUsage::Readback)
                {
                    Readbacks.push_back(b);
                    ++b->PendingWrites;
                }
            }
            void BeginRendering(const RenderingDesc& desc) override
            {
                Outside();
                Require(!desc.Colors.empty() || desc.Depth.Target, "Rendering requires an attachment");
                Require(desc.Colors.size() <= Owner->Native.Properties.limits.maxColorAttachments,
                        "Too many color attachments");
                uint32_t                                   width = 0, height = 0, samples = 0;
                std::vector<std::pair<VKTexture*, size_t>> seen;
                auto validate = [&](const Handle<Texture>& target, TextureSubresource sub, bool depth) {
                    auto t = Cast<VKTexture>(target, Owner);
                    TextureBytes(t->Desc, sub);
                    Require(t->Desc.Dimension != TextureDimension::Texture3D &&
                                Info(t->Desc.PixelFormat).Depth() == depth &&
                                Has(t->Desc.Usage, depth ? TextureUsage::DepthStencil : TextureUsage::RenderTarget),
                            "Invalid attachment usage or format");
                    if (!width)
                    {
                        width  = Width(t->Desc, sub.Mip);
                        height = Height(t->Desc, sub.Mip);
                        samples = t->Desc.Samples;
                    }
                    Require(width == Width(t->Desc, sub.Mip) && height == Height(t->Desc, sub.Mip),
                            "Attachment extents differ");
                    Require(samples == t->Desc.Samples, "Attachment sample counts differ");
                    auto key = std::make_pair(t.get(), t->Index(sub));
                    Require(std::find(seen.begin(), seen.end(), key) == seen.end(), "Duplicate rendering attachment");
                    seen.push_back(key);
                };
                auto validateResolve = [&](const Handle<Texture>& source,
                                           const Handle<Texture>& target,
                                           TextureSubresource     sub,
                                           bool                   depth) {
                    if (!target)
                        return;
                    auto t = Cast<VKTexture>(target, Owner);
                    Require(samples > 1 && t->Desc.Samples == 1 && t->Desc.PixelFormat == source->GetDesc().PixelFormat,
                            "Invalid resolve sample count or format");
                    const auto saved = samples;
                    samples          = 1;
                    validate(target, sub, depth);
                    samples = saved;
                };
                for (const auto& color : desc.Colors)
                {
                    validate(color.Target, color.Subresource, false);
                    validateResolve(color.Target, color.ResolveTarget, color.ResolveSubresource, false);
                    Load(color.Load);
                    Store(color.Store);
                }
                if (desc.Depth.Target)
                {
                    validate(desc.Depth.Target, desc.Depth.Subresource, true);
                    validateResolve(desc.Depth.Target, desc.Depth.ResolveTarget, desc.Depth.ResolveSubresource, true);
                    Load(desc.Depth.Load);
                    Store(desc.Depth.Store);
                    Require(desc.Depth.ClearDepth >= 0 && desc.Depth.ClearDepth <= 1, "Invalid depth clear");
                }
                Uses           = std::make_shared<ScopeUses>();
                RenderingInfo  = desc;
                StorageWritten = false;
                Recorded.push_back([state = Owner, desc, width, height, uses = Uses](Execution& e) {
                    uses->Prepare(e, desc.Depth);
                    std::vector<VkRenderingAttachmentInfo> colors;
                    for (const auto& a : desc.Colors)
                    {
                        auto t = Cast<VKTexture>(a.Target, state);
                        Require(!uses->Textures.count(t.get()), "Sampling a color attachment is unsupported");
                        e.Use(*t,
                              a.Subresource,
                              {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
                        VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                        attachment.imageView   = t->Views[t->Index(a.Subresource)];
                        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        attachment.loadOp      = Load(a.Load);
                        attachment.storeOp = uses->PreserveAttachments ? VK_ATTACHMENT_STORE_OP_STORE : Store(a.Store);
                        if (t->Desc.PixelFormat == Format::R32Uint)
                            attachment.clearValue.color.uint32[0] = a.ClearUint;
                        else
                            std::copy(a.ClearColor.begin(), a.ClearColor.end(), attachment.clearValue.color.float32);
                        if (a.ResolveTarget)
                        {
                            auto r = Cast<VKTexture>(a.ResolveTarget, state);
                            e.Use(*r,
                                  a.ResolveSubresource,
                                  {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
                            attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                            // Integer formats resolve sample zero, as required by Vulkan.
                            if (t->Desc.PixelFormat == Format::R32Uint)
                                attachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                            attachment.resolveImageView   = r->Views[r->Index(a.ResolveSubresource)];
                            attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        }
                        colors.push_back(attachment);
                    }
                    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    bool                      stencil = false;
                    if (desc.Depth.Target)
                    {
                        const auto& a = desc.Depth;
                        auto        t = Cast<VKTexture>(a.Target, state);
                        const bool  sampled = uses->Textures.count(t.get()) != 0;
                        Require(!sampled || (!uses->DepthWrites && a.Load == LoadOp::Load),
                                "Sampled depth attachment must be loaded and read-only");
                        const auto layout = sampled ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                        Access     access {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                           layout};
                        if (sampled)
                        {
                            access.Stage |=
                                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                            access.Mask |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        }
                        if (a.ResolveTarget)
                        {
                            access.Stage |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                            access.Mask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
                        }
                        e.Use(*t, a.Subresource, access);
                        depth.imageView               = t->Views[t->Index(a.Subresource)];
                        depth.imageLayout             = layout;
                        depth.loadOp                  = Load(a.Load);
                        depth.storeOp = uses->PreserveAttachments ? VK_ATTACHMENT_STORE_OP_STORE : Store(a.Store);
                        depth.clearValue.depthStencil = {a.ClearDepth, a.ClearStencil};
                        stencil                       = Info(t->Desc.PixelFormat).Stencil();
                        if (a.ResolveTarget)
                        {
                            auto r = Cast<VKTexture>(a.ResolveTarget, state);
                            // Depth/stencil resolves execute in COLOR_ATTACHMENT_OUTPUT, too.
                            e.Use(*r,
                                  a.ResolveSubresource,
                                  {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
                            depth.resolveMode        = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                            depth.resolveImageView   = r->Views[r->Index(a.ResolveSubresource)];
                            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                        }
                    }
                    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea.extent    = {width, height};
                    rendering.layerCount           = 1;
                    rendering.colorAttachmentCount = uint32_t(colors.size());
                    rendering.pColorAttachments    = colors.data();
                    rendering.pDepthAttachment     = desc.Depth.Target ? &depth : nullptr;
                    rendering.pStencilAttachment   = stencil ? &depth : nullptr;
                    e.FlushBarriers();
                    vkCmdBeginRendering(e.Native, &rendering);
                    VkViewport viewport {0, 0, float(width), float(height), 0, 1};
                    VkRect2D   scissor {{0, 0}, {width, height}};
                    vkCmdSetViewport(e.Native, 0, 1, &viewport);
                    vkCmdSetScissor(e.Native, 0, 1, &scissor);
                });
                CurrentViewport = {0, 0, float(width), float(height), 0, 1};
                CurrentScissor  = {0, 0, width, height};
                RenderingWidth  = width;
                RenderingHeight = height;
                Rendering       = true;
            }
            void EndRendering() override
            {
                Mutable();
                Require(Rendering, "No active rendering scope");
                Recorded.push_back([](Execution& e) { vkCmdEndRendering(e.Native); });
                Rendering = false;
            }
            void SetViewport(const Viewport& v) override
            {
                Mutable();
                Require(Rendering && v.Width > 0 && v.Height > 0 && v.MinDepth >= 0 && v.MaxDepth <= 1 &&
                            v.MinDepth <= v.MaxDepth,
                        "Invalid viewport");
                CurrentViewport = v;
                Recorded.push_back([v](Execution& e) {
                    VkViewport native {v.X, v.Y, v.Width, v.Height, v.MinDepth, v.MaxDepth};
                    vkCmdSetViewport(e.Native, 0, 1, &native);
                });
            }
            void SetScissor(const Scissor& s) override
            {
                Mutable();
                Require(Rendering, "Scissor requires a rendering scope");
                // Neutral scissors may extend beyond the framebuffer. Intersect
                // with the attachment bounds before forming a native rectangle,
                // whose offsets must be nonnegative and sums fit signed 32 bits.
                const int64_t  x      = std::clamp<int64_t>(s.X, 0, RenderingWidth);
                const int64_t  y      = std::clamp<int64_t>(s.Y, 0, RenderingHeight);
                const int64_t  right  = std::clamp<int64_t>(int64_t(s.X) + s.Width, x, RenderingWidth);
                const int64_t  bottom = std::clamp<int64_t>(int64_t(s.Y) + s.Height, y, RenderingHeight);
                const VkRect2D native {{int32_t(x), int32_t(y)}, {uint32_t(right - x), uint32_t(bottom - y)}};
                CurrentScissor = s;
                Recorded.push_back([native](Execution& e) { vkCmdSetScissor(e.Native, 0, 1, &native); });
            }
            void Inside()
            {
                Mutable();
                Require(Rendering, "Draw state requires a rendering scope");
            }
            void BindPipeline(const Handle<Pipeline>& pipeline) override
            {
                Inside();
                BoundPipeline = Cast<VKPipeline>(pipeline, Owner);
                BoundCompute.reset();
                RetainedBindings.push_back(pipeline);
            }
            void BindGroup(const Handle<RenderHardware::BindGroup>& group) override
            {
                Mutable();
                auto g                                  = Cast<VKGroup>(group, Owner);
                Groups[g->Desc.Layout->GetDesc().Group] = g;
                RetainedBindings.push_back(g);
            }
            void PushConstants(uint32_t offset, const void* data, uint32_t bytes) override
            {
                Mutable();
                Require((BoundPipeline || BoundCompute) && data && offset % 4 == 0 && bytes % 4 == 0,
                        "Invalid push constant write");
                Range(BoundCompute ? BoundCompute->Desc.PushConstantBytes : BoundPipeline->Desc.PushConstantBytes,
                      offset,
                      bytes);
                std::memcpy(PushData.data() + offset, data, bytes);
            }
            void BindVertexBuffers(const std::vector<VertexBufferView>& buffers) override
            {
                Inside();
                for (auto view : buffers)
                {
                    auto b = Cast<VKBuffer>(view.Resource, Owner);
                    Require(Has(b->Desc.Usage, BufferUsage::Vertex) && view.Offset < b->Desc.Bytes && view.Slot < 16,
                            "Invalid vertex buffer view");
                    Vertices[view.Slot] = view;
                    RetainedBindings.push_back(b);
                }
            }
            void BindIndexBuffer(const Handle<Buffer>& buffer, IndexType type, size_t offset) override
            {
                Inside();
                auto b = Cast<VKBuffer>(buffer, Owner);
                Require(Has(b->Desc.Usage, BufferUsage::Index) && offset < b->Desc.Bytes &&
                            offset % (type == IndexType::Uint16 ? 2 : 4) == 0,
                        "Invalid index buffer view");
                BoundIndex  = b;
                IndexFormat = type;
                IndexOffset = offset;
                RetainedBindings.push_back(b);
            }
            void PrepareDraw(bool indexed)
            {
                Inside();
                Require(bool(BoundPipeline), "Draw requires pipeline");
                const auto& d = BoundPipeline->Desc;
                Require(d.Samples ==
                            (RenderingInfo.Colors.empty() ? RenderingInfo.Depth.Target : RenderingInfo.Colors[0].Target)
                                ->GetDesc()
                                .Samples,
                        "Pipeline sample count mismatch");
                Require(d.ColorFormats.size() == RenderingInfo.Colors.size(), "Pipeline target count mismatch");
                for (size_t i = 0; i < d.ColorFormats.size(); ++i)
                    Require(d.ColorFormats[i] == RenderingInfo.Colors[i].Target->GetDesc().PixelFormat,
                            "Pipeline target format mismatch");
                Require(d.HasDepthStencil == bool(RenderingInfo.Depth.Target), "Pipeline depth target mismatch");
                if (d.HasDepthStencil)
                    Require(d.DepthStencilFormat == RenderingInfo.Depth.Target->GetDesc().PixelFormat,
                            "Pipeline depth format mismatch");
                for (const auto& layout : d.GroupLayouts)
                {
                    const auto& expected = layout->GetDesc();
                    Require(Groups.count(expected.Group), "Missing bind group");
                    const auto& actual = Groups.at(expected.Group)->Desc.Layout->GetDesc();
                    Require(actual.Entries.size() == expected.Entries.size(), "Bind group layout mismatch");
                    for (size_t i = 0; i < expected.Entries.size(); ++i)
                        Require(actual.Entries[i].Slot == expected.Entries[i].Slot &&
                                    actual.Entries[i].Type == expected.Entries[i].Type,
                                "Bind group layout mismatch");
                }
                auto       shader        = Cast<VKShader>(d.Program, Owner);
                const auto stencilWrites = [](const StencilFaceState& face) {
                    return face.WriteMask && (face.Fail != StencilOp::Keep || face.DepthFail != StencilOp::Keep ||
                                              face.Pass != StencilOp::Keep);
                };
                const bool writesDepth  = d.DepthStencil.DepthWrite ||
                                          (d.DepthStencil.StencilTest &&
                                           (stencilWrites(d.DepthStencil.Front) || stencilWrites(d.DepthStencil.Back)));
                bool       sampledDepth = false;
                for (auto binding : shader->Bindings)
                    if (binding.Type == BindingType::SampledTexture ||
                        binding.Type == BindingType::CombinedTextureSampler)
                    {
                        const auto& group = Groups.at(binding.Group);
                        const auto  entry = std::find_if(group->Desc.Entries.begin(),
                                                         group->Desc.Entries.end(),
                                                         [&](const auto& e) { return e.Slot == binding.Slot; });
                        Require(entry != group->Desc.Entries.end(), "Missing shader binding");
                        for (const auto& color : RenderingInfo.Colors)
                            Require(entry->TextureResource != color.Target,
                                    "Sampling a color attachment is unsupported");
                        sampledDepth |= entry->TextureResource == RenderingInfo.Depth.Target;
                    }
                Require(!sampledDepth || !writesDepth, "Attached sampled depth must be read-only");
                const bool changeDepthLayout =
                    RenderingInfo.Depth.Target &&
                    ((sampledDepth && (Uses->DepthWrites || RenderingInfo.Depth.Load != LoadOp::Load)) ||
                     (writesDepth && Uses->Textures.count(Cast<VKTexture>(RenderingInfo.Depth.Target, Owner).get())));
                const bool shaderWrites =
                    std::any_of(shader->Bindings.begin(), shader->Bindings.end(), [](const auto& b) {
                        return b.Type == BindingType::StorageBuffer || b.Type == BindingType::StorageTexture;
                    });
                if (StorageWritten || changeDepthLayout || (shaderWrites && Uses->HasDraws))
                {
                    // Dynamic rendering does not permit buffer barriers inside the
                    // scope. Preserve attachments and begin a new native segment.
                    auto info = RenderingInfo;
                    for (auto& a : info.Colors)
                        a.Load = LoadOp::Load;
                    if (info.Depth.Target)
                        info.Depth.Load = LoadOp::Load;
                    Uses->PreserveAttachments = true;
                    auto width = RenderingWidth, height = RenderingHeight;
                    auto viewport = CurrentViewport;
                    auto scissor  = CurrentScissor;
                    Recorded.push_back([](Execution& e) { vkCmdEndRendering(e.Native); });
                    Rendering = false;
                    BeginRendering(info);
                    RenderingWidth  = width;
                    RenderingHeight = height;
                    // Viewport/scissor are restored below from the recorded values.
                    SetViewport(viewport);
                    SetScissor(scissor);
                }
                Uses->DepthWrites |= writesDepth;
                Uses->HasDraws = true;
                for (auto binding : shader->Bindings)
                {
                    const auto& group = Groups.at(binding.Group);
                    auto        entry = std::find_if(group->Desc.Entries.begin(),
                                                     group->Desc.Entries.end(),
                                                     [&](const auto& e) { return e.Slot == binding.Slot; });
                    Require(entry != group->Desc.Entries.end(), "Missing shader binding");
                    if (entry->BufferResource)
                    {
                        auto b = Cast<VKBuffer>(entry->BufferResource, Owner);
                        Require((entry->Bytes ? entry->Bytes : b->Desc.Bytes - entry->Offset) >= binding.Bytes,
                                "Binding range is smaller than the shader block");
                        Access access {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       binding.Type == BindingType::StorageBuffer ?
                                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT :
                                       binding.Type == BindingType::ReadOnlyStorageBuffer ?
                                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT :
                                           VK_ACCESS_2_UNIFORM_READ_BIT};
                        Uses->Buffer(b, access);
                        StorageWritten |= binding.Type == BindingType::StorageBuffer;
                    }
                    if (entry->TextureResource)
                    {
                        auto texture = Cast<VKTexture>(entry->TextureResource, Owner);
                        Require(entry->Subresource || texture->Desc.ArrayLayers == 1,
                                "Array textures require a single-layer sampled view");
                        Require((entry->Subresource && texture->Desc.Dimension != TextureDimension::Texture3D ?
                                     TextureDimension::Texture2D :
                                     texture->Desc.Dimension) == binding.Dimension,
                                "Texture dimension disagrees with shader binding");
                        Require(binding.Multisampled == (texture->Desc.Samples > 1),
                                "Shader image sample count mismatch");
                        Require(binding.Unsigned == (entry->Aspect == TextureAspect::Stencil ||
                                                     texture->Desc.PixelFormat == Format::R32Uint),
                                "Sampled image scalar type disagrees with texture aspect/format");
                        if (binding.Type == BindingType::CombinedTextureSampler)
                        {
                            auto sampler = Cast<VKSampler>(entry->SamplerResource, Owner);
                            Require(sampler->Comparison == binding.Comparison,
                                    "Sampler comparison disagrees with shader binding");
                            Require(!binding.Comparison || Info(texture->Desc.PixelFormat).Depth(),
                                    "Comparison sampling requires a depth texture");
                        }
                        if (binding.Type == BindingType::StorageTexture)
                        {
                            ValidateStorageFormat(binding, *texture);
                            const auto sub   = entry->Subresource.value_or(TextureSubresource {});
                            auto&      value = Uses->StorageTextures[{texture.get(), texture->Index(sub)}];
                            value.first      = texture;
                            value.second = {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                            VK_IMAGE_LAYOUT_GENERAL};
                            StorageWritten = true;
                        }
                        else
                            Uses->Textures[texture.get()] = texture;
                    }
                }
                for (const auto& [key, value] : Uses->StorageTextures)
                {
                    Require(!Uses->Textures.count(key.first), "Cannot bind image as sampled and storage within a draw");
                    for (const auto& color : RenderingInfo.Colors)
                        Require(value.first != color.Target, "Storage access to a colour attachment is unsupported");
                }
                for (auto b : d.VertexBindings)
                {
                    Require(Vertices.count(b.Slot), "Missing vertex buffer");
                    Uses->Buffer(
                        Cast<VKBuffer>(Vertices.at(b.Slot).Resource, Owner),
                        {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT});
                }
                if (indexed)
                {
                    Require(bool(BoundIndex), "Indexed draw requires index buffer");
                    Uses->Buffer(BoundIndex, {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT});
                }
                Recorded.push_back([pipeline    = BoundPipeline,
                                    groups      = Groups,
                                    vertices    = Vertices,
                                    index       = indexed ? BoundIndex : nullptr,
                                    indexFormat = IndexFormat,
                                    indexOffset = IndexOffset,
                                    push        = PushData](Execution& e) {
                    vkCmdBindPipeline(e.Native, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Native);
                    for (const auto& layout : pipeline->Desc.GroupLayouts)
                    {
                        auto        slot  = layout->GetDesc().Group;
                        const auto& group = groups.at(slot);
                        auto        set   = group->Set->Native;
                        vkCmdBindDescriptorSets(e.Native,
                                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipeline->Layout,
                                                slot,
                                                1,
                                                &set,
                                                uint32_t(group->DynamicOffsets.size()),
                                                group->DynamicOffsets.data());
                    }
                    if (pipeline->Desc.PushConstantBytes)
                        vkCmdPushConstants(e.Native,
                                           pipeline->Layout,
                                           VK_SHADER_STAGE_ALL_GRAPHICS,
                                           0,
                                           pipeline->Desc.PushConstantBytes,
                                           push.data());
                    for (auto b : pipeline->Desc.VertexBindings)
                    {
                        const auto&  view   = vertices.at(b.Slot);
                        auto         buffer = Cast<VKBuffer>(view.Resource, e.Owner)->Native;
                        VkDeviceSize offset = view.Offset;
                        vkCmdBindVertexBuffers(e.Native, b.Slot, 1, &buffer, &offset);
                    }
                    if (index)
                        vkCmdBindIndexBuffer(e.Native,
                                             index->Native,
                                             indexOffset,
                                             indexFormat == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 :
                                                                                VK_INDEX_TYPE_UINT32);
                });
            }
            void Draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) override
            {
                PrepareDraw(false);
                CountDraw(vertices, instances);
                Recorded.push_back(
                    [=](Execution& e) { vkCmdDraw(e.Native, vertices, instances, firstVertex, firstInstance); });
            }
            void DrawIndexed(uint32_t indices,
                             uint32_t instances,
                             uint32_t firstIndex,
                             int32_t  baseVertex,
                             uint32_t firstInstance) override
            {
                Require(bool(BoundIndex), "Indexed draw requires index buffer");
                Range(BoundIndex->Desc.Bytes,
                      IndexOffset + size_t(firstIndex) * (IndexFormat == IndexType::Uint16 ? 2 : 4),
                      size_t(indices) * (IndexFormat == IndexType::Uint16 ? 2 : 4));
                PrepareDraw(true);
                CountDraw(indices, instances);
                Recorded.push_back([=](Execution& e) {
                    vkCmdDrawIndexed(e.Native, indices, instances, firstIndex, baseVertex, firstInstance);
                });
            }
#include "VulkanCompute.inl"
            void CopyBuffer(const Handle<Buffer>& source,
                            size_t                from,
                            const Handle<Buffer>& destination,
                            size_t                to,
                            size_t                bytes) override
            {
                Outside();
                auto s = Cast<VKBuffer>(source, Owner), d = Cast<VKBuffer>(destination, Owner);
                Require(Has(s->Desc.Usage, BufferUsage::CopySource) && Has(d->Desc.Usage, BufferUsage::CopyDestination),
                        "Buffer copy usage mismatch");
                Range(s->Desc.Bytes, from, bytes);
                Range(d->Desc.Bytes, to, bytes);
                Require(s != d || from + bytes <= to || to + bytes <= from, "Overlapping buffer copy");
                Recorded.push_back([s, d, from, to, bytes](Execution& e) {
                    if (bytes)
                        e.Copy(*s, from, *d, to, bytes);
                });
                Mark(d);
            }
            void CopyBufferToTexture(const Handle<Buffer>&  source,
                                     size_t                 offset,
                                     const Handle<Texture>& destination,
                                     TextureSubresource     sub) override
            {
                Outside();
                auto s = Cast<VKBuffer>(source, Owner);
                auto d = Cast<VKTexture>(destination, Owner);
                Require(Has(s->Desc.Usage, BufferUsage::CopySource) &&
                            Has(d->Desc.Usage, TextureUsage::CopyDestination),
                        "Texture upload usage mismatch");
                Require(d->Desc.Samples == 1, "Cannot upload multisampled texture");
                Require(offset % Info(d->Desc.PixelFormat).Bytes == 0, "Texture copy offset must be texel aligned");
                Range(s->Desc.Bytes, offset, TextureBytes(d->Desc, sub));
                Recorded.push_back([s, d, offset, sub](Execution& e) {
                    auto f = Info(d->Desc.PixelFormat);
                    if (f.Stencil())
                        e.PackedCopy(*s, offset, *d, sub, true);
                    else
                        e.ImageCopy(*s, offset, *d, sub, f.Aspects, true);
                });
            }
            void CopyBufferToTextureRegion(const Handle<Buffer>&  source,
                                           size_t                 offset,
                                           const Handle<Texture>& destination,
                                           uint32_t               x,
                                           uint32_t               y,
                                           uint32_t               width,
                                           uint32_t               height,
                                           TextureSubresource     sub) override
            {
                Outside();
                auto s = Cast<VKBuffer>(source, Owner);
                auto d = Cast<VKTexture>(destination, Owner);
                auto f = Info(d->Desc.PixelFormat);
                Require(Has(s->Desc.Usage, BufferUsage::CopySource) &&
                            Has(d->Desc.Usage, TextureUsage::CopyDestination),
                        "Texture region upload usage mismatch");
                Require(d->Desc.Samples == 1 && f.Aspects == VK_IMAGE_ASPECT_COLOR_BIT,
                        "Region upload requires single-sample color texture");
                TextureBytes(d->Desc, sub); // Validate mip and face before shifting dimensions.
                auto w = Width(d->Desc, sub.Mip), h = Height(d->Desc, sub.Mip);
                Require(width && height && x <= w && y <= h && width <= w - x && height <= h - y,
                        "Texture region out of bounds");
                Require(offset % f.Bytes == 0, "Texture region offset must be texel aligned");
                Range(s->Desc.Bytes, offset, size_t(width) * height * f.Bytes);
                Recorded.push_back([s, d, offset, sub, x, y, width, height](Execution& e) {
                    e.Use(*s, TransferRead);
                    e.Use(*d, sub, TransferWrite);
                    e.FlushBarriers();
                    VkBufferImageCopy region {};
                    region.bufferOffset     = offset;
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sub.Mip, sub.Face, 1};
                    region.imageOffset      = {int32_t(x), int32_t(y), 0};
                    region.imageExtent      = {width, height, 1};
                    vkCmdCopyBufferToImage(
                        e.Native, s->Native, d->Native, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                });
            }
            void CopyTextureToBuffer(const Handle<Texture>& source,
                                     TextureSubresource     sub,
                                     const Handle<Buffer>&  destination,
                                     size_t                 offset) override
            {
                Outside();
                auto s = Cast<VKTexture>(source, Owner);
                auto d = Cast<VKBuffer>(destination, Owner);
                Require(Has(s->Desc.Usage, TextureUsage::CopySource) &&
                            Has(d->Desc.Usage, BufferUsage::CopyDestination),
                        "Texture readback usage mismatch");
                Require(s->Desc.Samples == 1, "Cannot read back multisampled texture; resolve first");
                Require(offset % Info(s->Desc.PixelFormat).Bytes == 0, "Texture copy offset must be texel aligned");
                Range(d->Desc.Bytes, offset, TextureBytes(s->Desc, sub));
                Recorded.push_back([s, d, offset, sub](Execution& e) {
                    auto f = Info(s->Desc.PixelFormat);
                    if (f.Stencil())
                        e.PackedCopy(*d, offset, *s, sub, false);
                    else
                        e.ImageCopy(*d, offset, *s, sub, f.Aspects, false);
                });
                Mark(d);
            }
            void WriteTimestamp(const Handle<TimestampQueryPool>& pool, uint32_t index, bool beginning) override
            {
                Outside();
                auto q = Cast<VKQueries>(pool, Owner);
                Require(index < q->Count, "Timestamp index out of range");
                ++q->PendingWrites;
                q->HasWrites = true;
                QueryWrites.push_back(q);
                auto record = [q, index](Execution& e) {
                    e.FlushBarriers();
                    vkCmdResetQueryPool(e.Native, q->Native, index, 1);
                    vkCmdWriteTimestamp2(e.Native, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->Native, index);
                };
                if (beginning)
                    Recorded.insert(Recorded.begin(), record);
                else
                    Recorded.push_back(record);
            }
            void CopyTexture(const Handle<Texture>& source,
                             TextureSubresource     from,
                             const Handle<Texture>& destination,
                             TextureSubresource     to) override
            {
                Outside();
                auto s = Cast<VKTexture>(source, Owner), d = Cast<VKTexture>(destination, Owner);
                Require(s != d && Has(s->Desc.Usage, TextureUsage::CopySource) &&
                            Has(d->Desc.Usage, TextureUsage::CopyDestination),
                        "Texture copy usage mismatch");
                // Vulkan permits raw copies between compatible UNORM/sRGB texel blocks.
                const bool compatibleColor =
                    (s->Desc.PixelFormat == Format::RGBA8Unorm && d->Desc.PixelFormat == Format::RGBA8Srgb) ||
                    (s->Desc.PixelFormat == Format::RGBA8Srgb && d->Desc.PixelFormat == Format::RGBA8Unorm);
                Require(s->Desc.Samples == d->Desc.Samples &&
                            (s->Desc.PixelFormat == d->Desc.PixelFormat || compatibleColor) &&
                            TextureBytes(s->Desc, from) == TextureBytes(d->Desc, to) &&
                            Width(s->Desc, from.Mip) == Width(d->Desc, to.Mip) &&
                            Height(s->Desc, from.Mip) == Height(d->Desc, to.Mip) &&
                            s->Desc.Dimension == d->Desc.Dimension &&
                            Depth(s->Desc, from.Mip) == Depth(d->Desc, to.Mip),
                        "Texture copy description mismatch");
                Recorded.push_back([s, d, from, to](Execution& e) {
                    e.Use(*s,
                          from,
                          {VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL});
                    e.Use(*d,
                          to,
                          {VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL});
                    e.FlushBarriers();
                    auto aspects = Info(s->Desc.PixelFormat).Aspects;
                    for (auto aspect :
                         {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT})
                        if (aspects & aspect)
                        {
                            VkImageCopy region {};
                            region.srcSubresource = {VkImageAspectFlags(aspect), from.Mip, from.Face, 1};
                            region.dstSubresource = {VkImageAspectFlags(aspect), to.Mip, to.Face, 1};
                            region.extent         = {
                                Width(s->Desc, from.Mip), Height(s->Desc, from.Mip), Depth(s->Desc, from.Mip)};
                            vkCmdCopyImage(e.Native,
                                           s->Native,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           d->Native,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           1,
                                           &region);
                        }
                });
            }
            void CopyTextureTexelToBuffer(const Handle<Texture>& source,
                                          TextureSubresource     sub,
                                          uint32_t               x,
                                          uint32_t               y,
                                          const Handle<Buffer>&  destination,
                                          size_t                 offset) override
            {
                Outside();
                auto s      = Cast<VKTexture>(source, Owner);
                auto d      = Cast<VKBuffer>(destination, Owner);
                auto format = Info(s->Desc.PixelFormat);
                TextureBytes(s->Desc, sub);
                Require(Has(s->Desc.Usage, TextureUsage::CopySource) &&
                            Has(d->Desc.Usage, BufferUsage::CopyDestination),
                        "Texel copy usage mismatch");
                Require(s->Desc.Samples == 1, "Resolve before reading a multisampled texel");
                Require(x < Width(s->Desc, sub.Mip) && y < Height(s->Desc, sub.Mip) && offset % 4 == 0 &&
                            offset % format.Bytes == 0,
                        "Invalid texel copy region");
                Range(d->Desc.Bytes, offset, format.Stencil() ? 8 : format.Bytes);
                Recorded.push_back([s, d, sub, x, y, offset, format](Execution& e) {
                    e.Use(*s,
                          sub,
                          {VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL});
                    e.Use(*d, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT});
                    e.FlushBarriers();
                    for (auto aspect :
                         {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT})
                        if (format.Aspects & aspect)
                        {
                            VkBufferImageCopy region {};
                            region.bufferOffset     = offset + (aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 4 : 0);
                            region.imageSubresource = {VkImageAspectFlags(aspect), sub.Mip, sub.Face, 1};
                            region.imageOffset      = {int32_t(x), int32_t(y), 0};
                            region.imageExtent      = {1, 1, 1};
                            vkCmdCopyImageToBuffer(
                                e.Native, s->Native, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->Native, 1, &region);
                        }
                    e.Use(*d, HostRead);
                });
                Mark(d);
            }
            void GenerateMips(const Handle<Texture>& texture) override
            {
                Outside();
                auto t = Cast<VKTexture>(texture, Owner);
                auto f = Info(t->Desc.PixelFormat);
                Require(Has(t->Desc.Usage, TextureUsage::CopySource) &&
                            Has(t->Desc.Usage, TextureUsage::CopyDestination) && t->Desc.MipLevels > 1 && !f.Depth() &&
                            t->Desc.PixelFormat != Format::R32Uint,
                        "Texture cannot generate mips");
                VkFormatProperties properties;
                vkGetPhysicalDeviceFormatProperties(Owner->Native.Physical, f.Native, &properties);
                const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                Require((properties.optimalTilingFeatures & required) == required, "Texture format cannot filter mips");
                Recorded.push_back([t](Execution& e) {
                    for (uint32_t face = 0;
                         face < (t->Desc.Dimension == TextureDimension::Cube ? 6u : t->Desc.ArrayLayers);
                         ++face)
                        for (uint32_t mip = 1; mip < t->Desc.MipLevels; ++mip)
                        {
                            e.Use(*t,
                                  {mip - 1, face},
                                  {VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL});
                            e.Use(*t,
                                  {mip, face},
                                  {VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL});
                            e.FlushBarriers();
                            VkImageBlit region {};
                            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, face, 1};
                            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
                            region.srcOffsets[1]  = {int32_t(Width(t->Desc, mip - 1)),
                                                     int32_t(Height(t->Desc, mip - 1)),
                                                     int32_t(Depth(t->Desc, mip - 1))};
                            region.dstOffsets[1]  = {int32_t(Width(t->Desc, mip)),
                                                     int32_t(Height(t->Desc, mip)),
                                                     int32_t(Depth(t->Desc, mip))};
                            vkCmdBlitImage(e.Native,
                                           t->Native,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           t->Native,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           1,
                                           &region,
                                           VK_FILTER_LINEAR);
                        }
                });
            }
        };
        void State::BeginWindowFrame()
        {
            CheckThread();
            auto& frame = Frames[FrameIndex];
            Check(vkWaitForFences(Native.Logical, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
            for (auto& list : frame.Lists)
                list->Retire();
            frame.Lists.clear();
            frame.UIImages.clear();
            Drain();
            for (auto it = UITextures.begin(); it != UITextures.end();)
                if (it->second.expired())
                    it = UITextures.erase(it);
                else
                    ++it;
            for (auto it = DescriptorCache.begin(); it != DescriptorCache.end();)
                if (it->second.expired())
                    it = DescriptorCache.erase(it);
                else
                    ++it;
            Check(vkResetCommandPool(Native.Logical, frame.Pool, 0));
        }
        void State::FinishWindowFrame()
        {
            Require(!InFrame, "Present inside an active scene frame");
            auto& frame = Frames[FrameIndex];
            frame.Lists = std::move(Submitted);
            Submitted.clear();
            frame.UIImages = TakeImGuiTextures();
            Check(vkResetFences(Native.Logical, 1, &frame.Fence));
            VkSubmitInfo2 tail {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            Check(vkQueueSubmit2(Native.Queue, 1, &tail, frame.Fence));
            FrameIndex = (FrameIndex + 1) % Frames.size();
        }
        void State::Retire()
        {
            for (auto& frame : Frames)
            {
                for (auto& list : frame.Lists)
                    list->Retire();
                frame.Lists.clear();
                frame.UIImages.clear();
            }
            for (auto& list : Submitted)
                list->Retire();
            Submitted.clear();
            for (auto it = DescriptorCache.begin(); it != DescriptorCache.end();)
                if (it->second.expired())
                    it = DescriptorCache.erase(it);
                else
                    ++it;
            Drain();
            // Reset only after all submitted command buffers are known complete.
            for (auto& frame : Frames)
                Check(vkResetCommandPool(Native.Logical, frame.Pool, 0));
            Check(vkResetCommandPool(Native.Logical, Standalone.Pool, 0));
        }
        class VKDevice final : public Device
        {
            DeviceDesc             Desc;
            std::shared_ptr<State> Owner;

        public:
            VKDevice(DeviceDesc desc, const VulkanDeviceOptions& options) : Desc(desc)
            {
                Require(desc.FramesInFlight > 0, "FramesInFlight must be positive");
                Owner = std::make_shared<State>(desc, options);
            }
            ~VKDevice() override
            {
                // Destructors cannot throw. Normal WaitIdle reports device loss.
                vkDeviceWaitIdle(Owner->Native.Logical);
                Owner->Retire();
                Owner->Alive = false;
            }
            const Context&        GetContext() const { return Owner->Native; }
            const State&          GetState() const { return *Owner; }
            std::shared_ptr<State> GetOwner() const { return Owner; }
            const DeviceDesc&     GetDesc() const override { return Desc; }
            CoordinateConventions GetCoordinateConventions() const override
            {
                return {ClipYDirection::Down, DepthRange::ZeroToOne, FramebufferOrigin::UpperLeft};
            }
            DeviceStats GetStats() const override
            {
                Owner->CheckThread();
                auto stats         = Owner->Stats;
                stats.TextureBytes = Owner->Native.TextureAllocationBytes;
                stats.BufferBytes  = Owner->Native.BufferAllocationBytes;
                return stats;
            }
            size_t GetBufferBindingAlignment(BindingType type) const override
            {
                switch (type)
                {
                    case BindingType::UniformBuffer:
                        return Owner->Native.Properties.limits.minUniformBufferOffsetAlignment;
                    case BindingType::StorageBuffer:
                    case BindingType::ReadOnlyStorageBuffer:
                        return Owner->Native.Properties.limits.minStorageBufferOffsetAlignment;
                    default:
                        throw std::invalid_argument("Binding type is not a buffer");
                }
            }
            ComputeLimits GetComputeLimits() const override
            {
                Owner->CheckThread();
                const auto& l = Owner->Native.Properties.limits;
                return {{l.maxComputeWorkGroupCount[0], l.maxComputeWorkGroupCount[1], l.maxComputeWorkGroupCount[2]},
                        {l.maxComputeWorkGroupSize[0], l.maxComputeWorkGroupSize[1], l.maxComputeWorkGroupSize[2]},
                        l.maxComputeWorkGroupInvocations,
                        l.maxComputeSharedMemorySize};
            }
            IndirectCapabilities GetIndirectCapabilities() const override
            {
                Owner->CheckThread();
                VkPhysicalDeviceVulkan12Features f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                VkPhysicalDeviceFeatures2        f {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f.pNext = &f12;
                vkGetPhysicalDeviceFeatures2(Owner->Native.Physical, &f);
                return {bool(f.features.multiDrawIndirect),
                        bool(f12.drawIndirectCount),
                        Owner->Native.Properties.limits.maxDrawIndirectCount};
            }
            bool SupportsVertexStorageWrites() const override
            {
                VkPhysicalDeviceFeatures f {};
                vkGetPhysicalDeviceFeatures(Owner->Native.Physical, &f);
                return f.vertexPipelineStoresAndAtomics;
            }
            bool SupportsFragmentStorageWrites() const override
            {
                VkPhysicalDeviceFeatures f {};
                vkGetPhysicalDeviceFeatures(Owner->Native.Physical, &f);
                return f.fragmentStoresAndAtomics;
            }
            std::unordered_map<size_t, std::vector<std::pair<std::string, std::weak_ptr<VKComputePipeline>>>>
                                    ComputeCache;
            Handle<ComputePipeline> CreateComputePipeline(const ComputePipelineDesc& desc) override
            {
                Owner->CheckThread();
                Require(Cast<VKShader>(desc.Program, Owner)->Compute, "Compute pipeline requires a compute program");
                for (const auto& layout : desc.GroupLayouts)
                    Cast<VKLayout>(layout, Owner);
                // Use the same canonical, field-wise key as graphics; padding is never hashed.
                PipelineDesc keyDesc;
                keyDesc.Program           = desc.Program;
                keyDesc.GroupLayouts      = desc.GroupLayouts;
                keyDesc.PushConstantBytes = desc.PushConstantBytes;
                auto  key                 = Detail::PipelineKey(keyDesc);
                auto& bucket              = ComputeCache[std::hash<std::string> {}(key)];
                for (auto it = bucket.begin(); it != bucket.end();)
                    if (auto pipeline = it->second.lock())
                    {
                        if (it->first == key)
                            return pipeline;
                        ++it;
                    }
                    else
                        it = bucket.erase(it);
                auto pipeline = std::make_shared<VKComputePipeline>(Owner, desc);
                bucket.emplace_back(std::move(key), pipeline);
                return pipeline;
            }
            Handle<Buffer> CreateBuffer(const BufferDesc& d) override
            {
                Owner->CheckThread();
                return std::make_shared<VKBuffer>(Owner, d);
            }
            Handle<TimestampQueryPool> CreateTimestampQueryPool(uint32_t count) override
            {
                Owner->CheckThread();
                if (!Owner->Native.TimestampValidBits)
                    return {};
                return std::make_shared<VKQueries>(Owner, count);
            }
            bool SupportsSampleRateShading() const override
            {
                Owner->CheckThread();
                VkPhysicalDeviceFeatures features {};
                vkGetPhysicalDeviceFeatures(Owner->Native.Physical, &features);
                return features.sampleRateShading;
            }
            uint32_t GetSupportedSampleCounts(const TextureDesc& d) const override
            {
                Owner->CheckThread();
                return SupportedSamples(Owner->Native, d);
            }
            Handle<Texture> CreateTexture(const TextureDesc& d) override
            {
                Owner->CheckThread();
                return std::make_shared<VKTexture>(Owner, d);
            }
            Handle<Sampler> CreateSampler(const SamplerDesc& d) override
            {
                Owner->CheckThread();
                return std::make_shared<VKSampler>(Owner, d);
            }
            Handle<Shader> CreateShader(const Kans::ShaderData& data) override
            {
                Owner->CheckThread();
                return std::make_shared<VKShader>(Owner, data);
            }
            Handle<BindGroupLayout> CreateBindGroupLayout(const BindGroupLayoutDesc& desc) override
            {
                Owner->CheckThread();
                auto canonical = desc;
                std::sort(
                    canonical.Entries.begin(), canonical.Entries.end(), [](auto a, auto b) { return a.Slot < b.Slot; });
                std::string key(reinterpret_cast<const char*>(&canonical.Group), sizeof(canonical.Group));
                for (auto entry : canonical.Entries)
                {
                    key.append(reinterpret_cast<const char*>(&entry.Slot), sizeof(entry.Slot));
                    key.append(reinterpret_cast<const char*>(&entry.Type), sizeof(entry.Type));
                }
                if (auto layout = LayoutCache[key].lock())
                    return layout;
                auto layout      = std::make_shared<VKLayout>(Owner, std::move(canonical));
                LayoutCache[key] = layout;
                return layout;
            }
            Handle<BindGroup> CreateBindGroup(const BindGroupDesc& desc) override
            {
                Owner->CheckThread();
                const auto start = std::chrono::steady_clock::now();
                auto       group = std::make_shared<VKGroup>(Owner, desc);
                Owner->GroupCreationNanoseconds += uint64_t(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                        .count());
                return group;
            }
            std::unordered_map<std::string, std::weak_ptr<VKLayout>>                                   LayoutCache;
            std::unordered_map<size_t, std::vector<std::pair<std::string, std::weak_ptr<VKPipeline>>>> Cache;
            Handle<Pipeline> CreatePipeline(const PipelineDesc& desc) override
            {
                Owner->CheckThread();
                ValidatePipeline(Owner, desc);
                auto  key    = Detail::PipelineKey(desc);
                auto& bucket = Cache[std::hash<std::string> {}(key)];
                for (auto it = bucket.begin(); it != bucket.end();)
                    if (auto pipeline = it->second.lock())
                    {
                        if (it->first == key)
                            return pipeline;
                        ++it;
                    }
                    else
                        it = bucket.erase(it);
                auto pipeline = std::make_shared<VKPipeline>(Owner, desc);
                bucket.emplace_back(std::move(key), pipeline);
                return pipeline;
            }
            Handle<CommandList>     CreateCommandList() override
            {
                Owner->CheckThread();
                return std::make_shared<Commands>(Owner);
            }
            void BeginFrame() override
            {
                Owner->CheckThread();
                Require(!Owner->InFrame, "Frame is already active");
                auto& frame = Owner->Frames[Owner->FrameIndex];
                Check(vkWaitForFences(Owner->Native.Logical, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
                Owner->InFrame = true;
            }
            void Submit(const Handle<CommandList>& commands) override
            {
                Owner->CheckThread();
                auto list = std::dynamic_pointer_cast<Commands>(commands);
                Require(list && list->Owner == Owner && !list->Sealed && !list->Rendering,
                        "Invalid command submission");
                list->Sealed = true;
                Owner->Submitted.push_back(list);
                Execution execution(Owner);
                try
                {
                    for (const auto& command : list->Recorded)
                        command(execution);
                    execution.Submit();
                    list->Temporary = std::move(execution.Temporary);
                }
                catch (...)
                {
                    // A packed transfer may already have submitted a prefix.
                    // Keep every staging allocation alive until explicit retirement.
                    list->Temporary = std::move(execution.Temporary);
                    throw;
                }
            }
            void EndFrame() override
            {
                Owner->CheckThread();
                Require(Owner->InFrame, "No active frame");
                if (Owner->Native.Options.AsynchronousWindowFrames)
                {
                    Owner->InFrame = false;
                    return;
                }
                auto& frame = Owner->Frames[Owner->FrameIndex];
                Check(vkResetFences(Owner->Native.Logical, 1, &frame.Fence));
                VkSubmitInfo2 tail {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                Check(vkQueueSubmit2(Owner->Native.Queue, 1, &tail, frame.Fence));
                Check(vkWaitForFences(Owner->Native.Logical, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
                Owner->Retire();
                Owner->InFrame    = false;
                Owner->FrameIndex = (Owner->FrameIndex + 1) % Desc.FramesInFlight;
            }
            void WaitIdle() override
            {
                Owner->CheckThread();
                ++Owner->WaitIdleCalls;
                Check(vkDeviceWaitIdle(Owner->Native.Logical));
                Owner->Retire();
            }
            uintptr_t GetImGuiTextureID(const Handle<Texture>& texture, const SamplerDesc* sampling) const override
            {
                Owner->CheckThread();
                auto image = Cast<VKTexture>(texture, Owner);
                Require(image->SampledView && image->Desc.Dimension == TextureDimension::Texture2D &&
                            image->Desc.ArrayLayers == 1 && image->Desc.Samples == 1 &&
                            !Info(image->Desc.PixelFormat).Depth(),
                        "ImGui requires a sampled 2D colour texture");
                SamplerDesc desc = sampling ? *sampling : SamplerDesc {};
                auto        same = [&](const auto& view) {
                    const auto& d = view.Desc;
                    return d.Min == desc.Min && d.Mag == desc.Mag && d.Mip == desc.Mip && d.U == desc.U &&
                           d.V == desc.V && d.W == desc.W && d.MinLod == desc.MinLod && d.MaxLod == desc.MaxLod &&
                           d.CompareEnabled == desc.CompareEnabled && d.Compare == desc.Compare;
                };
                auto found = std::find_if(image->UIViews.begin(), image->UIViews.end(), same);
                if (found == image->UIViews.end())
                {
                    auto sampler     = std::make_shared<VKSampler>(Owner, desc);
                    auto [pool, set] = Owner->AllocateSet(Owner->UITextureLayout);
                    VkDescriptorImageInfo info {
                        sampler->Native, image->SampledView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet          = set;
                    write.descriptorCount = 1;
                    write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    write.pImageInfo      = &info;
                    vkUpdateDescriptorSets(Owner->Native.Logical, 1, &write, 0, nullptr);
                    auto [samplerPool, samplerSet] = Owner->AllocateSet(Owner->UISamplerLayout);
                    write.dstSet                   = samplerSet;
                    write.descriptorType           = VK_DESCRIPTOR_TYPE_SAMPLER;
                    vkUpdateDescriptorSets(Owner->Native.Logical, 1, &write, 0, nullptr);
                    image->UIViews.push_back({desc, sampler, set, samplerSet});
                    auto previous        = std::move(image->Memory.Delete);
                    image->Memory.Delete = [previous = std::move(previous),
                                            device   = Owner->Native.Logical,
                                            pool,
                                            set,
                                            samplerPool,
                                            samplerSet] {
                        vkFreeDescriptorSets(device, pool, 1, &set);
                        vkFreeDescriptorSets(device, samplerPool, 1, &samplerSet);
                        previous();
                    };
                    found                                               = image->UIViews.end() - 1;
                    Owner->UITextures[reinterpret_cast<uintptr_t>(set)] = image;
                }
                RetainImGuiTexture(texture);
                return reinterpret_cast<uintptr_t>(found->Set);
            }
        };
    } // namespace
    const VulkanDetail::Context& GetVulkanContext(Device& device)
    {
        return dynamic_cast<VKDevice&>(device).GetContext();
    }
    void BeginVulkanDeviceWindowFrame(Device& device)
    {
        dynamic_cast<VKDevice&>(device).GetOwner()->BeginWindowFrame();
    }
    void FinishVulkanDeviceWindowFrame(Device& device)
    {
        dynamic_cast<VKDevice&>(device).GetOwner()->FinishWindowFrame();
    }
    Handle<Texture> RenderVulkanImGuiOffscreen(Device& device, ImDrawData* data, VkRenderPass pass)
    {
        auto        owner = dynamic_cast<VKDevice&>(device).GetOwner();
        TextureDesc desc;
        desc.Width                    = uint32_t(data->DisplaySize.x * data->FramebufferScale.x);
        desc.Height                   = uint32_t(data->DisplaySize.y * data->FramebufferScale.y);
        desc.Usage                    = TextureUsage::RenderTarget | TextureUsage::Sampled | TextureUsage::CopySource;
        auto                    image = std::make_shared<VKTexture>(owner, desc);
        VkFramebuffer           framebuffer;
        VkFramebufferCreateInfo create {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        create.renderPass      = pass;
        create.attachmentCount = 1;
        create.pAttachments    = &image->Views[0];
        create.width           = desc.Width;
        create.height          = desc.Height;
        create.layers          = 1;
        Check(vkCreateFramebuffer(owner->Native.Logical, &create, nullptr, &framebuffer));
        auto previous        = std::move(image->Memory.Delete);
        image->Memory.Delete = [previous = std::move(previous), logical = owner->Native.Logical, framebuffer] {
            vkDestroyFramebuffer(logical, framebuffer, nullptr);
            previous();
        };
        Execution execution(owner);
        execution.Use(*image,
                      {},
                      {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        execution.FlushBarriers();
        VkClearValue clear {};
        clear.color.float32[3] = 1;
        VkRenderPassBeginInfo begin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass        = pass;
        begin.framebuffer       = framebuffer;
        begin.renderArea.extent = {desc.Width, desc.Height};
        begin.clearValueCount   = 1;
        begin.pClearValues      = &clear;
        vkCmdBeginRenderPass(execution.Native, &begin, VK_SUBPASS_CONTENTS_INLINE);
        FlipVulkanImGuiRenderTargetUVs(device, data);
        ImGui_ImplVulkan_RenderDrawData(data, execution.Native);
        FlipVulkanImGuiRenderTargetUVs(device, data);
        vkCmdEndRenderPass(execution.Native);
        execution.Submit();
        auto retained = std::make_shared<Commands>(owner);
        retained->Temporary.push_back(image);
        owner->Submitted.push_back(retained);
        return image;
    }
    void FlipVulkanImGuiRenderTargetUVs(Device& device, ImDrawData* data)
    {
        auto owner = dynamic_cast<VKDevice&>(device).GetOwner();
        for (int i = 0; i < data->CmdListsCount; ++i)
        {
            auto*              list = data->CmdLists[i];
            std::set<unsigned> flipped;
            for (const auto& command : list->CmdBuffer)
            {
                if (command.UserCallback)
                    continue;
                auto found = owner->UITextures.find(static_cast<uintptr_t>(command.GetTexID()));
                if (found == owner->UITextures.end())
                    continue;
                auto image = found->second.lock();
                if (!image || !Has(image->Desc.Usage, TextureUsage::RenderTarget))
                    continue;
                for (unsigned index = 0; index < command.ElemCount; ++index)
                {
                    unsigned vertex = command.VtxOffset + list->IdxBuffer[command.IdxOffset + index];
                    if (flipped.insert(vertex).second)
                        list->VtxBuffer[vertex].uv.y = 1 - list->VtxBuffer[vertex].uv.y;
                }
            }
        }
    }
    namespace
    {
        // The stock 1.92 backend binds images at set 0 and samplers at set 1.
        // Insert callbacks at draw boundaries so existing Image/AddImage callers retain
        // their RHI sampler (including mip and address modes), then restore font filtering.
        void SetImGuiSampler(const ImDrawList* list, const ImDrawCmd* command)
        {
            if (!command->UserCallbackData)
            {
                ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear(list, command);
                return;
            }
            auto* render = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
            auto  set    = reinterpret_cast<VkDescriptorSet>(command->UserCallbackData);
            vkCmdBindDescriptorSets(
                render->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render->PipelineLayout, 1, 1, &set, 0, nullptr);
        }
    } // namespace
    void PrepareVulkanImGuiTextures(Device& device, ImDrawData* data)
    {
        // Resolve dynamic atlas references before inspecting draw-command texture IDs.
        if (data->Textures)
            for (auto* texture : *data->Textures)
                if (texture->Status != ImTextureStatus_OK)
                    ImGui_ImplVulkan_UpdateTexture(texture);
        auto                owner = dynamic_cast<VKDevice&>(device).GetOwner();
        Execution           execution(owner);
        std::set<uintptr_t> seen;
        for (int i = 0; i < data->CmdListsCount; ++i)
            for (const auto& command : data->CmdLists[i]->CmdBuffer)
            {
                if (command.UserCallback)
                    continue;
                auto id = static_cast<uintptr_t>(command.GetTexID());
                if (!seen.insert(id).second)
                    continue;
                auto found = owner->UITextures.find(id);
                if (found == owner->UITextures.end())
                    continue; // ImGui-owned font descriptor.
                auto image = found->second.lock();
                Require(bool(image), "Expired ImGui texture ID");
                RetainImGuiTexture(image);
                for (uint32_t mip = 0; mip < image->Desc.MipLevels; ++mip)
                    execution.Use(*image,
                                  {mip, 0},
                                  {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
        execution.Submit();
        for (auto* list : data->CmdLists)
        {
            // A viewport may be prepared more than once by a caller.
            if (std::any_of(list->CmdBuffer.begin(), list->CmdBuffer.end(), [](const ImDrawCmd& cmd) {
                    return cmd.UserCallback == SetImGuiSampler;
                }))
                continue;
            ImVector<ImDrawCmd> commands;
            commands.reserve(list->CmdBuffer.Size * 2);
            // Sampler state survives between draw lists; establish it at each list start.
            bool customSampler = true;
            for (const auto& command : list->CmdBuffer)
            {
                if (!command.UserCallback && command.ElemCount)
                {
                    VkDescriptorSet sampler = VK_NULL_HANDLE;
                    auto            found   = owner->UITextures.find(static_cast<uintptr_t>(command.GetTexID()));
                    if (found != owner->UITextures.end())
                        if (auto image = found->second.lock())
                            for (const auto& view : image->UIViews)
                                if (reinterpret_cast<uintptr_t>(view.Set) == command.GetTexID())
                                    sampler = view.SamplerSet;
                    if (sampler || customSampler)
                    {
                        ImDrawCmd callback;
                        callback.UserCallback     = SetImGuiSampler;
                        callback.UserCallbackData = reinterpret_cast<void*>(sampler);
                        commands.push_back(callback);
                    }
                    customSampler = sampler != VK_NULL_HANDLE;
                }
                else if (command.UserCallback)
                {
                    // Callbacks may reset render state; rebind on the next image draw.
                    customSampler = true;
                }
                commands.push_back(command);
            }
            list->CmdBuffer.swap(commands);
        }
    }
    std::unique_ptr<Device> CreateVulkanWindowDevice(Window& window, const DeviceDesc& desc)
    {
        VulkanDeviceOptions options;
        options.NativeWindow             = window.GetNativeWindow();
        options.AsynchronousWindowFrames = true;
        const char* force                = std::getenv("KANS_VULKAN_REQUIRE_CPU");
        options.RequireCpuDevice         = force && std::strcmp(force, "1") == 0;
#if defined(KS_DEBUG) || !defined(NDEBUG)
        options.Validation                = true;
        options.SynchronizationValidation = true;
#endif
        return CreateVulkanDevice(desc, options);
    }
    std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, const VulkanDeviceOptions& options)
    {
        return std::make_unique<VKDevice>(desc, options);
    }
    std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc)
    {
        VulkanDeviceOptions options;
#if defined(KS_DEBUG) || !defined(NDEBUG)
        options.Validation = true;
#endif
        const char* allow        = std::getenv("KANS_VULKAN_ALLOW_NON_CPU");
        options.RequireCpuDevice = !allow || std::strcmp(allow, "1") != 0;
        const char* force        = std::getenv("KANS_VULKAN_REQUIRE_CPU");
        if (force && std::strcmp(force, "1") == 0)
            options.RequireCpuDevice = true;
        return CreateVulkanDevice(desc, options);
    }
} // namespace Kans::RenderHardware

namespace Kans::RenderHardware::VulkanTestProbes
{
    void SetBarrierObserver(const Device& device, BarrierObserver observer)
    {
        auto owner = dynamic_cast<const VKDevice&>(device).GetOwner();
        owner->CheckThread();
        owner->Barriers = std::move(observer);
    }
    void SetBufferWriteObserver(const Device& device, BufferWriteObserver observer)
    {
        auto owner = dynamic_cast<const VKDevice&>(device).GetOwner();
        owner->CheckThread();
        owner->BufferWrites = std::move(observer);
    }
    void SetPipelineFailure(const Device& device, std::string nameSubstring)
    {
        auto owner = dynamic_cast<const VKDevice&>(device).GetOwner();
        owner->CheckThread();
        owner->FailPipeline = std::move(nameSubstring);
    }
    Diagnostics Inspect(const Device& device)
    {
        const auto& state   = dynamic_cast<const VKDevice&>(device).GetState();
        const auto& context = state.Native;
        return {context.Properties.deviceType,
                context.Properties.apiVersion,
                context.Options.Validation,
                context.Options.SynchronizationValidation,
                state.PipelineCreations,
                state.DescriptorAllocations,
                state.DescriptorPools.size(),
                state.GroupCreationNanoseconds,
                state.WaitIdleCalls,
                state.BufferCreations,
                state.PipelineAttempts};
    }
    void EmitValidationError(const Device& device, const char* message)
    {
        const auto& context = dynamic_cast<const VKDevice&>(device).GetContext();
        auto        submit  = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
            vkGetInstanceProcAddr(context.Instance, "vkSubmitDebugUtilsMessageEXT"));
        VulkanDetail::Require(submit != nullptr, "Debug message entry point is missing");
        VkDebugUtilsMessengerCallbackDataEXT data {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.pMessage = message;
        submit(context.Instance,
               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
               &data);
    }
} // namespace Kans::RenderHardware::VulkanTestProbes
