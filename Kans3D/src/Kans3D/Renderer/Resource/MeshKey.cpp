#include "kspch.h"
#include "MeshKey.h"
#include <atomic>
#include <stdexcept>

namespace Kans
{
    namespace
    {
        uint64_t NextMeshIdentity()
        {
            static std::atomic<uint64_t> next {1};
            auto                         id = next.fetch_add(1, std::memory_order_relaxed);
            if (!id)
                std::terminate();
            return id;
        }
    } // namespace
    MeshKey MeshKey::MakeStaticBatch()
    {
        MeshKey key;
        key.m_StaticBatch = NextMeshIdentity();
        return key;
    }

    RuntimeMesh::RuntimeMesh(MeshData data) :
        Identity(NextMeshIdentity()), Source(CreateRef<MeshAsset>(std::move(data)))
    {}
} // namespace Kans
