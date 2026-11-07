// Included inside Commands. Captures own every resource until command retirement.
void ComputeOutside()
{
    Mutable();
    if (Rendering)
        throw std::logic_error("Compute/transfer requires no rendering scope");
}
void BindComputePipeline(const Handle<ComputePipeline>& pipeline) override
{
    ComputeOutside();
    BoundCompute = Cast<VKComputePipeline>(pipeline, Owner);
    BoundPipeline.reset();
    RetainedBindings.push_back(pipeline);
}
std::shared_ptr<VKBuffer> IndirectBuffer(const Handle<Buffer>& handle, size_t offset, size_t bytes)
{
    auto buffer = Cast<VKBuffer>(handle, Owner);
    Require(Has(buffer->Desc.Usage, BufferUsage::Indirect), "Indirect buffer requires Indirect usage");
    Require(offset % 4 == 0 && offset < buffer->Desc.Bytes, "Indirect offset must be 4-byte aligned and inside buffer");
    Range(buffer->Desc.Bytes, offset, bytes);
    return buffer;
}
void PrepareCompute(const std::shared_ptr<VKBuffer>& indirect = {})
{
    ComputeOutside();
    Require(bool(BoundCompute), "Dispatch requires compute pipeline");
    auto shader = Cast<VKShader>(BoundCompute->Desc.Program, Owner);
    auto uses   = std::make_shared<ScopeUses>();
    for (const auto& layout : BoundCompute->Desc.GroupLayouts)
    {
        const auto& expected = layout->GetDesc();
        Require(Groups.count(expected.Group), "Missing compute bind group");
        const auto& actual = Groups.at(expected.Group)->Desc.Layout->GetDesc();
        Require(actual.Entries.size() == expected.Entries.size(), "Compute bind group layout mismatch");
        for (size_t i = 0; i < expected.Entries.size(); ++i)
            Require(actual.Entries[i].Slot == expected.Entries[i].Slot &&
                        actual.Entries[i].Type == expected.Entries[i].Type,
                    "Compute bind group layout mismatch");
    }
    for (const auto& binding : shader->Bindings)
    {
        const auto& entries = Groups.at(binding.Group)->Desc.Entries;
        auto        entry =
            std::find_if(entries.begin(), entries.end(), [&](const auto& v) { return v.Slot == binding.Slot; });
        Require(entry != entries.end(), "Missing compute binding");
        if (entry->BufferResource)
        {
            auto b = Cast<VKBuffer>(entry->BufferResource, Owner);
            Require((entry->Bytes ? entry->Bytes : b->Desc.Bytes - entry->Offset) >= binding.Bytes,
                    "Compute binding range too small");
            VkAccessFlags2 mask = binding.Type == BindingType::UniformBuffer ? VK_ACCESS_2_UNIFORM_READ_BIT :
                                                                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            if (binding.Type == BindingType::StorageBuffer)
                mask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            uses->Buffer(b, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mask});
        }
        if (entry->TextureResource)
        {
            auto t = Cast<VKTexture>(entry->TextureResource, Owner);
            Require((entry->Subresource && t->Desc.Dimension != TextureDimension::Texture3D ?
                         TextureDimension::Texture2D :
                         t->Desc.Dimension) == binding.Dimension,
                    "Compute image dimension mismatch");
            Require(binding.Multisampled == (t->Desc.Samples > 1), "Compute image sample count mismatch");
            Require(binding.Unsigned ==
                        (t->Desc.PixelFormat == Format::R32Uint || entry->Aspect == TextureAspect::Stencil),
                    "Compute image scalar type mismatch");
            if (binding.Type == BindingType::CombinedTextureSampler)
            {
                auto sampler = Cast<VKSampler>(entry->SamplerResource, Owner);
                Require(sampler->Comparison == binding.Comparison, "Compute sampler comparison mismatch");
            }
            const bool storage = binding.Type == BindingType::StorageTexture;
            if (storage)
                ValidateStorageFormat(binding, *t);
            Require(storage || entry->Subresource || t->Desc.ArrayLayers == 1,
                    "Array sampled image requires Subresource");
            const uint32_t firstMip = entry->Subresource ? entry->Subresource->Mip : 0;
            const uint32_t mipEnd   = storage ? firstMip + 1 : (entry->Subresource ? firstMip + 1 : t->Desc.MipLevels);
            const uint32_t face     = entry->Subresource ? entry->Subresource->Face : 0;
            for (uint32_t mip = firstMip; mip < mipEnd; ++mip)
            {
                auto&        use = uses->StorageTextures[{t.get(), t->Index({mip, face})}];
                const Access access {
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    storage ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT :
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    storage ? VK_IMAGE_LAYOUT_GENERAL :
                              (Info(t->Desc.PixelFormat).Depth() && Has(t->Desc.Usage, TextureUsage::DepthStencil) ?
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
                Require(!use.first || use.second.Layout == access.Layout,
                        "Cannot bind a subresource as both sampled and storage in one dispatch");
                use.first = t;
                use.second.Stage |= access.Stage;
                use.second.Mask |= access.Mask;
                use.second.Layout = access.Layout;
            }
        }
    }
    if (indirect)
        uses->Buffer(indirect, {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
    Recorded.push_back([pipeline = BoundCompute, groups = Groups, push = PushData, uses](Execution& e) {
        uses->Prepare(e, {});
        e.FlushBarriers();
        vkCmdBindPipeline(e.Native, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->Native);
        for (const auto& l : pipeline->Desc.GroupLayouts)
        {
            const auto  slot = l->GetDesc().Group;
            const auto& g    = groups.at(slot);
            vkCmdBindDescriptorSets(e.Native,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline->Layout,
                                    slot,
                                    1,
                                    &g->Set->Native,
                                    uint32_t(g->DynamicOffsets.size()),
                                    g->DynamicOffsets.data());
        }
        if (pipeline->Desc.PushConstantBytes)
            vkCmdPushConstants(e.Native,
                               pipeline->Layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0,
                               pipeline->Desc.PushConstantBytes,
                               push.data());
    });
}
void ExportComputeReadbacks()
{
    std::vector<std::shared_ptr<VKBuffer>> buffers;
    auto                                   shader = Cast<VKShader>(BoundCompute->Desc.Program, Owner);
    for (const auto& binding : shader->Bindings)
        if (binding.Type == BindingType::StorageBuffer)
        {
            const auto& entries = Groups.at(binding.Group)->Desc.Entries;
            const auto  entry   = std::find_if(
                entries.begin(), entries.end(), [&](const auto& value) { return value.Slot == binding.Slot; });
            auto b = Cast<VKBuffer>(entry->BufferResource, Owner);
            if (b->Desc.Memory == MemoryUsage::Readback &&
                std::find(buffers.begin(), buffers.end(), b) == buffers.end())
            {
                Mark(b);
                buffers.push_back(std::move(b));
            }
        }
    if (!buffers.empty())
        Recorded.push_back([buffers](Execution& e) {
            for (const auto& b : buffers)
            {
                const auto producer = b->Last;
                e.Use(*b, HostRead);
                e.FlushBarriers();
                b->Last = producer;
            }
        });
}
void Dispatch(uint32_t x, uint32_t y, uint32_t z) override
{
    ComputeOutside();
    const auto& limits = Owner->Native.Properties.limits;
    Require(x <= limits.maxComputeWorkGroupCount[0] && y <= limits.maxComputeWorkGroupCount[1] &&
                z <= limits.maxComputeWorkGroupCount[2],
            "Dispatch group count exceeds device limits");
    PrepareCompute();
    Recorded.push_back([=](Execution& e) { vkCmdDispatch(e.Native, x, y, z); });
    ExportComputeReadbacks();
}
void DispatchIndirect(const Handle<Buffer>& buffer, size_t offset) override
{
    ComputeOutside();
    auto args = IndirectBuffer(buffer, offset, sizeof(VkDispatchIndirectCommand));
    PrepareCompute(args);
    // GPU-produced group counts must obey GetComputeLimits(), as Vulkan cannot validate their values on the CPU.
    Recorded.push_back([args, offset](Execution& e) { vkCmdDispatchIndirect(e.Native, args->Native, offset); });
    ExportComputeReadbacks();
}
void RecordIndirect(const Handle<Buffer>& buffer,
                    size_t                offset,
                    uint32_t              count,
                    uint32_t              stride,
                    bool                  indexed,
                    const Handle<Buffer>& countBuffer = {},
                    size_t                countOffset = 0)
{
    Mutable();
    if (!Rendering)
        throw std::logic_error("Indirect draw requires a rendering scope");
    const auto size = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    Require(stride % 4 == 0 && stride >= size, "Invalid indirect stride");
    const auto&              limits = Owner->Native.Properties.limits;
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(Owner->Native.Physical, &features);
    Require(count <= limits.maxDrawIndirectCount && (count <= 1 || features.multiDrawIndirect),
            "Multi-draw indirect unavailable or count exceeds device limit");
    auto                      args = IndirectBuffer(buffer, offset, count ? size_t(count - 1) * stride + size : 0);
    std::shared_ptr<VKBuffer> counter;
    if (countBuffer)
    {
        VkPhysicalDeviceVulkan12Features f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2        f {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(Owner->Native.Physical, &f);
        if (!f12.drawIndirectCount)
            throw std::runtime_error("Indirect count unavailable");
        counter = IndirectBuffer(countBuffer, countOffset, 4);
    }
    PrepareDraw(indexed);
    Uses->Buffer(args, {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
    if (counter)
        Uses->Buffer(counter, {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT});
    ++Statistics.DrawCalls;
    ++Statistics.IndirectDraws;
    Recorded.push_back([args, offset, count, stride, indexed, counter, countOffset](Execution& e) {
        if (counter)
        {
            if (indexed)
                vkCmdDrawIndexedIndirectCount(
                    e.Native, args->Native, offset, counter->Native, countOffset, count, stride);
            else
                vkCmdDrawIndirectCount(e.Native, args->Native, offset, counter->Native, countOffset, count, stride);
        }
        else if (indexed)
            vkCmdDrawIndexedIndirect(e.Native, args->Native, offset, count, stride);
        else
            vkCmdDrawIndirect(e.Native, args->Native, offset, count, stride);
    });
}
void DrawIndirect(const Handle<Buffer>& b, size_t o, uint32_t n, uint32_t s) override
{
    RecordIndirect(b, o, n, s, false);
}
void DrawIndexedIndirect(const Handle<Buffer>& b, size_t o, uint32_t n, uint32_t s) override
{
    RecordIndirect(b, o, n, s, true);
}
void DrawIndirectCount(const Handle<Buffer>& b, size_t o, const Handle<Buffer>& c, size_t co, uint32_t n, uint32_t s)
    override
{
    Require(bool(c), "Null indirect count buffer");
    RecordIndirect(b, o, n, s, false, c, co);
}
void DrawIndexedIndirectCount(const Handle<Buffer>& b,
                              size_t                o,
                              const Handle<Buffer>& c,
                              size_t                co,
                              uint32_t              n,
                              uint32_t              s) override
{
    Require(bool(c), "Null indirect count buffer");
    RecordIndirect(b, o, n, s, true, c, co);
}
void FillBuffer(const Handle<Buffer>& handle, size_t offset, size_t bytes, uint32_t value) override
{
    ComputeOutside();
    auto b = Cast<VKBuffer>(handle, Owner);
    Require(Has(b->Desc.Usage, BufferUsage::CopyDestination) && offset % 4 == 0 && bytes && bytes % 4 == 0,
            "Invalid buffer fill usage/alignment");
    Range(b->Desc.Bytes, offset, bytes);
    Mark(b);
    Recorded.push_back([b, offset, bytes, value](Execution& e) {
        e.Use(*b, BufferClearWrite);
        e.FlushBarriers();
        vkCmdFillBuffer(e.Native, b->Native, offset, bytes, value);
        if (b->Desc.Memory == MemoryUsage::Readback)
        {
            e.Use(*b, HostRead);
            e.FlushBarriers();
            b->Last = BufferClearWrite;
        }
    });
}
void UpdateBuffer(const Handle<Buffer>& handle, size_t offset, const void* data, size_t bytes) override
{
    ComputeOutside();
    auto b = Cast<VKBuffer>(handle, Owner);
    Require(Has(b->Desc.Usage, BufferUsage::CopyDestination) && data && offset % 4 == 0 && bytes && bytes % 4 == 0 &&
                bytes <= 65536,
            "Invalid buffer update (4-byte alignment, maximum 64 KiB)");
    Range(b->Desc.Bytes, offset, bytes);
    if (Owner->BufferWrites)
        Owner->BufferWrites(b->Desc, offset, data, bytes);
    Mark(b);
    std::vector<uint32_t> copy(bytes / 4);
    std::memcpy(copy.data(), data, bytes);
    Recorded.push_back([b, offset, copy = std::move(copy)](Execution& e) {
        e.Use(*b, BufferClearWrite);
        e.FlushBarriers();
        vkCmdUpdateBuffer(e.Native, b->Native, offset, copy.size() * 4, copy.data());
        if (b->Desc.Memory == MemoryUsage::Readback)
        {
            e.Use(*b, HostRead);
            e.FlushBarriers();
            b->Last = BufferClearWrite;
        }
    });
}
void ClearStorageTexture(const Handle<Texture>& handle, const std::array<float, 4>& color, uint32_t integer) override
{
    ComputeOutside();
    auto t = Cast<VKTexture>(handle, Owner);
    Require(Has(t->Desc.Usage, TextureUsage::Storage) && !Info(t->Desc.PixelFormat).Depth(),
            "Clear requires a colour storage texture");
    Recorded.push_back([t, color, integer](Execution& e) {
        for (uint32_t face = 0; face < t->Desc.ArrayLayers * (t->Desc.Dimension == TextureDimension::Cube ? 6 : 1);
             ++face)
            for (uint32_t mip = 0; mip < t->Desc.MipLevels; ++mip)
                e.Use(*t,
                      {mip, face},
                      {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL});
        e.FlushBarriers();
        VkClearColorValue value {};
        std::copy(color.begin(), color.end(), value.float32);
        if (t->Desc.PixelFormat == Format::R32Uint)
            value.uint32[0] = integer;
        VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT,
                                       0,
                                       t->Desc.MipLevels,
                                       0,
                                       t->Desc.Dimension == TextureDimension::Cube ? 6 : t->Desc.ArrayLayers};
        vkCmdClearColorImage(e.Native, t->Native, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    });
}
