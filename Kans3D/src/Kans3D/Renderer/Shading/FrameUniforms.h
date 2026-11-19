#pragma once
#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include "Kans3D/RHI/RHI.h"

namespace Kans
{
    struct UniformRange
    {
        RenderHardware::Handle<RenderHardware::Buffer> Buffer;
        size_t                                         Offset = 0;
    };

    // One view invocation owns a lease. Submitted commands retain that lease,
    // independently of cached bind groups, until their frame fence retires.
    class FrameUniforms
    {
    public:
        void End()
        {
            // A changed view/material must release its obsolete image snapshots
            // even when this slot is not selected again for several frames.
            for (auto it = m_Groups.begin(); it != m_Groups.end();)
                it = it->second.Epoch != m_Epoch ? m_Groups.erase(it) : std::next(it);
        }
        void Begin()
        {
            ++m_Epoch;
            for (auto& block : m_Blocks)
                block.Used = 0;
            m_Current = 0;
            for (auto it = m_Groups.begin(); it != m_Groups.end();)
                it = it->second.Epoch + 1 < m_Epoch ? m_Groups.erase(it) : std::next(it);
        }
        UniformRange Allocate(RenderHardware::Device& device, size_t bytes, bool gpuWritable = false)
        {
            using namespace RenderHardware;
            const auto alignment = device.GetBufferBindingAlignment(BindingType::UniformBuffer);
            if (!bytes || bytes > std::numeric_limits<uint32_t>::max() - alignment)
                throw std::invalid_argument("Uniform allocation size out of range");
            const auto aligned = (bytes + alignment - 1) / alignment * alignment;
            const auto create  = [&](bool writable) {
                auto usage = BufferUsage::Uniform | BufferUsage::CopyDestination | BufferUsage::CopySource;
                if (writable)
                    usage = usage | BufferUsage::Storage;
                m_Blocks.push_back(
                    {device.CreateBuffer({std::max(size_t(1024 * 1024), aligned), usage, MemoryUsage::Upload}),
                     0,
                     writable});
            };
            if (gpuWritable)
            {
                // Exposure patches write frame data on the GPU. Seal this allocation
                // so later host writes cannot replace its RHI producer/access state.
                size_t index = 0;
                while (index < m_Blocks.size() && (!m_Blocks[index].Writable || m_Blocks[index].Used ||
                                                   m_Blocks[index].Buffer->GetDesc().Bytes < aligned))
                    ++index;
                if (index == m_Blocks.size())
                    create(true);
                auto& block = m_Blocks[index];
                block.Used  = block.Buffer->GetDesc().Bytes;
                return {block.Buffer, 0};
            }
            while (m_Current < m_Blocks.size() &&
                   (m_Blocks[m_Current].Writable ||
                    aligned > m_Blocks[m_Current].Buffer->GetDesc().Bytes - m_Blocks[m_Current].Used))
                ++m_Current;
            if (m_Current == m_Blocks.size())
                create(false);
            auto&        block = m_Blocks[m_Current];
            UniformRange result {block.Buffer, block.Used};
            block.Used += aligned;
            return result;
        }
        RenderHardware::Handle<RenderHardware::BindGroup> Group(RenderHardware::Device&              device,
                                                                const RenderHardware::BindGroupDesc& desc)
        {
            // Every resource is strongly held by the cached group. Pointer identities
            // cannot be recycled while a key exists; stale snapshots age out on reuse.
            std::string key;
            key.push_back('D');
            const auto add = [&](const auto& value) {
                key.append(reinterpret_cast<const char*>(&value), sizeof(value));
            };
            add(desc.Layout.get());
            for (const auto& entry : desc.Entries)
            {
                add(entry.Slot);
                add(entry.BufferResource.get());
                add(entry.Offset);
                add(entry.Bytes);
                add(entry.TextureResource.get());
                add(entry.SamplerResource.get());
                add(bool(entry.Subresource));
                if (entry.Subresource)
                {
                    add(entry.Subresource->Mip);
                    add(entry.Subresource->Face);
                }
                add(entry.Aspect);
            }
            auto& cached = m_Groups[key];
            if (!cached.Group)
                cached.Group = device.CreateBindGroup(desc);
            cached.Epoch = m_Epoch;
            return cached.Group;
        }

        RenderHardware::Handle<RenderHardware::BindGroup> FindGroup(const std::string& key)
        {
            auto found = m_Groups.find(key);
            if (found == m_Groups.end())
                return {};
            found->second.Epoch = m_Epoch;
            return found->second.Group;
        }
        void CacheGroup(std::string key, const RenderHardware::Handle<RenderHardware::BindGroup>& group)
        {
            m_Groups[std::move(key)] = {group, m_Epoch};
        }

    private:
        struct Block
        {
            RenderHardware::Handle<RenderHardware::Buffer> Buffer;
            size_t                                         Used     = 0;
            bool                                           Writable = false;
        };
        struct CachedGroup
        {
            RenderHardware::Handle<RenderHardware::BindGroup> Group;
            uint64_t                                          Epoch = 0;
        };
        std::vector<Block>                           m_Blocks;
        size_t                                       m_Current = 0;
        uint64_t                                     m_Epoch   = 0;
        std::unordered_map<std::string, CachedGroup> m_Groups;
    };
} // namespace Kans
