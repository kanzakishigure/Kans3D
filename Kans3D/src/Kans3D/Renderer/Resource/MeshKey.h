#pragma once
#include "Kans3D/Asset/Runtime/MeshData.h"

namespace Kans
{
    // A separate identity domain, never encoded as an AssetID or serialized.
    struct RuntimeMesh
    {
        explicit RuntimeMesh(MeshData data);
        RuntimeMesh(const RuntimeMesh&)                          = delete;
        RuntimeMesh&               operator=(const RuntimeMesh&) = delete;
        const uint64_t             Identity;
        const Ref<const MeshAsset> Source;
    };
    struct DynamicVertexStream
    {
        MeshData Data;
        uint32_t Section = 0; // Original source section; Data contains one compact section.
        uint64_t Version = 0;
    };
    struct MeshKey
    {
        AssetID                Asset;
        Ref<const RuntimeMesh> Runtime;
        Ref<const DynamicVertexStream> Stream;
        MeshKey() = default;
        MeshKey(AssetID asset) : Asset(asset) {}
        MeshKey(SourceAssetID source, uint64_t local) : Asset(source, local) {}
        MeshKey(Ref<const DynamicVertexStream> stream) : Stream(std::move(stream)) {}
        MeshKey(Ref<const RuntimeMesh> runtime) : Runtime(std::move(runtime)) {}
        static MeshKey MakeStaticBatch();
        bool           IsValid() const { return m_StaticBatch || Stream || Runtime || Asset.IsValid(); }
        bool operator==(const MeshKey& b) const
        {
            if (m_StaticBatch || b.m_StaticBatch)
                return m_StaticBatch == b.m_StaticBatch;
            if (Stream || b.Stream)
                return Stream == b.Stream;
            return Runtime || b.Runtime ? Runtime && b.Runtime && Runtime->Identity == b.Runtime->Identity :
                                          Asset == b.Asset;
        }
        bool operator!=(const MeshKey& b) const { return !(*this == b); }
        bool operator<(const MeshKey& b) const
        {
            if (m_StaticBatch || b.m_StaticBatch)
                return m_StaticBatch < b.m_StaticBatch;
            if (Stream || b.Stream)
                return std::less<const DynamicVertexStream*>()(Stream.get(), b.Stream.get());
            if (bool(Runtime) != bool(b.Runtime))
                return bool(Runtime) < bool(b.Runtime);
            return Runtime ? Runtime->Identity < b.Runtime->Identity : Asset < b.Asset;
        }

    private:
        uint64_t m_StaticBatch = 0;
    };
} // namespace Kans
