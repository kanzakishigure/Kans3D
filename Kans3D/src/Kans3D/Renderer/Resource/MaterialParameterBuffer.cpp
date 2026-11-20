#include "kspch.h"
#include "Kans3D/Asset/Runtime/MaterialParameterCollection.h"
#include "RenderAssetCache.h"

namespace Kans
{
    RenderHardware::Handle<RenderHardware::Buffer>
    RenderAssetCache::GetMaterialParameterBuffer(const std::shared_ptr<const MaterialParameterUniforms>& parameters)
    {
        if (!parameters)
        {
            if (!m_EmptyMaterialParameters)
            {
                const MaterialParameterUniforms empty;
                m_EmptyMaterialParameters = GpuUniformBuffer(sizeof(empty), &empty, GetDevice()).Resource();
            }
            return m_EmptyMaterialParameters;
        }
        const auto found = m_MaterialParameterBuffers.find(parameters);
        if (found != m_MaterialParameterBuffers.end())
            return found->second;
        // Draws retain their bind groups and hence this buffer until GPU retirement.
        // Recycle a buffer only after its scene snapshot and all outstanding bindings are gone.
        RenderHardware::Handle<RenderHardware::Buffer> buffer;
        for (auto it = m_MaterialParameterBuffers.begin(); it != m_MaterialParameterBuffers.end();)
        {
            if (it->first.expired() && it->second.use_count() == 1)
            {
                if (!buffer)
                    buffer = std::move(it->second);
                it = m_MaterialParameterBuffers.erase(it);
            }
            else
                ++it;
        }
        if (!buffer)
            buffer = GpuUniformBuffer(sizeof(*parameters), nullptr, GetDevice()).Resource();
        buffer->Write(0, parameters.get(), sizeof(*parameters));
        m_MaterialParameterBuffers.emplace(parameters, buffer);
        return buffer;
    }
} // namespace Kans
