#pragma once

#include <array>
#include <map>
#include <vector>
#include "Kans3D/Renderer/Resource/MeshKey.h"

namespace Kans
{
    struct StaticBatchKey
    {
        std::array<int32_t, 3> Cell;
        AssetID                Material;
        bool                   CastShadows, ReceiveShadows;

        bool operator<(const StaticBatchKey& other) const;
    };

    struct StaticBatchSection
    {
        MeshKey              Mesh;
        Ref<const MeshAsset> Source;
        glm::mat4            Transform {1};
        Bounds               WorldBounds;
        UUID                 Entity = 0, InstanceRoot = 0;
        uint32_t             Section = 0;
        AssetID              Material;
        bool                 CastShadows = true, ReceiveShadows = true;

        // Mesh assets are immutable. A new source object for the same asset ID is a reimport.
        bool operator==(const StaticBatchSection& other) const;
    };

    using StaticBatchGroups = std::map<StaticBatchKey, std::vector<size_t>>;

    StaticBatchGroups GroupStaticBatchSections(const std::vector<StaticBatchSection>& sections);
    MeshData          BakeStaticBatch(const std::vector<StaticBatchSection>& sections);
} // namespace Kans
