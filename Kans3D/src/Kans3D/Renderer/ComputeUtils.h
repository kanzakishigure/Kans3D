#pragma once
#include <cstring>
#include <limits>
#include <type_traits>
#include "Kans3D/Renderer/Resource/InternalShader.h"

namespace Kans::ComputeUtils
{
    inline RenderHardware::Handle<RenderHardware::ComputePipeline>
    CreatePipeline(RenderHardware::Device&                                              device,
                   const std::string&                                                   builtin,
                   std::vector<RenderHardware::Handle<RenderHardware::BindGroupLayout>> layouts)
    {
        auto program = CreateBuiltinComputeShader(device, builtin);
        return device.CreateComputePipeline({program, std::move(layouts), program->GetPushConstantBytes()});
    }
    // One-dimensional kernels. Reject multi-dimensional local sizes rather than
    // silently processing the wrong linear range in a kernel using GlobalInvocationID.x.
    inline uint32_t GroupCount(size_t elements, const RenderHardware::Shader& shader)
    {
        const auto local = shader.GetWorkgroupSize();
        if (!local[0] || local[1] != 1 || local[2] != 1)
            throw std::invalid_argument("GroupCount requires a 1D compute kernel");
        const auto groups = elements / local[0] + (elements % local[0] != 0);
        if (groups > UINT32_MAX)
            throw std::invalid_argument("Compute group count overflow");
        return uint32_t(groups);
    }
    template<class T>
    class GpuArray
    {
        static_assert(std::is_trivially_copyable_v<T>);
        RenderHardware::Device*                        m_Device;
        RenderHardware::Handle<RenderHardware::Buffer> m_Buffer;
        size_t                                         m_Count;

    public:
        GpuArray(RenderHardware::Device&     device,
                 size_t                      count,
                 RenderHardware::BufferUsage extra = RenderHardware::BufferUsage(0)) : m_Device(&device), m_Count(count)
        {
            if (!count || count > std::numeric_limits<size_t>::max() / sizeof(T))
                throw std::invalid_argument("Invalid GPU array size");
            using namespace RenderHardware;
            m_Buffer = device.CreateBuffer(
                {count * sizeof(T),
                 BufferUsage::Storage | BufferUsage::CopySource | BufferUsage::CopyDestination | extra,
                 MemoryUsage::DeviceLocal});
        }
        const RenderHardware::Handle<RenderHardware::Buffer>& Resource() const { return m_Buffer; }
        size_t                                                Size() const { return m_Count; }
        void Upload(RenderHardware::CommandList& commands, const std::vector<T>& data, size_t first = 0)
        {
            if (first > m_Count || data.size() > m_Count - first)
                throw std::invalid_argument("GPU array upload out of range");
            if (data.empty())
                return;
            using namespace RenderHardware;
            auto staging =
                m_Device->CreateBuffer({data.size() * sizeof(T), BufferUsage::CopySource, MemoryUsage::Upload});
            staging->Write(0, data.data(), data.size() * sizeof(T));
            commands.CopyBuffer(staging, 0, m_Buffer, first * sizeof(T), data.size() * sizeof(T));
        }
        // Synchronous convenience for tests/tools. Production users can CopyBuffer
        // into their own readback ring and inspect it after normal frame retirement.
        std::vector<T> Readback() const
        {
            using namespace RenderHardware;
            auto output =
                m_Device->CreateBuffer({m_Count * sizeof(T), BufferUsage::CopyDestination, MemoryUsage::Readback});
            auto commands = m_Device->CreateCommandList();
            commands->CopyBuffer(m_Buffer, 0, output, 0, m_Count * sizeof(T));
            m_Device->Submit(commands);
            m_Device->WaitIdle();
            const auto     bytes = output->Read(0, m_Count * sizeof(T));
            std::vector<T> data(m_Count);
            std::memcpy(data.data(), bytes.data(), bytes.size());
            return data;
        }
    };
} // namespace Kans::ComputeUtils
