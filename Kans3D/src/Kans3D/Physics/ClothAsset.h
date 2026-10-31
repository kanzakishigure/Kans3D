#pragma once
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Runtime/MeshData.h"
namespace Kans
{
    struct ClothDistance
    {
        uint32_t A = 0, B = 0;
        float    Rest = 0;
    };
    enum class ClothMap
    {
        MaxDistance,
        BackstopDistance,
        BackstopRadius,
        StretchMultiplier,
        BendMultiplier
    };
    struct ClothData
    {
        AssetID  Mesh;
        uint32_t Section = 0, VertexCount = 0;
        float    WeldEpsilon  = 1e-5f;
        uint64_t TopologyHash = 0;
        // Indexed by source render vertex. UINT32_MAX denotes a vertex outside this section.
        std::vector<uint32_t>  RenderToSim, Representatives, Triangles;
        std::vector<glm::vec3> RestPositions;
        std::vector<float>     MaxDistance, BackstopDistance, BackstopRadius, StretchMultiplier, BendMultiplier;
        // Derived at creation/load or after painting; stable sorted edge order.
        std::vector<ClothDistance> Stretch, Bend, Tethers;
        std::vector<float>         Areas;
    };
    class ClothAsset final : public Asset
    {
    public:
        explicit ClothAsset(ClothData data) : m_Data(std::move(data)) {}
        static AssetType GetStaticType() { return AssetType::Cloth; }
        AssetType        GetAssetType() const override { return GetStaticType(); }
        const ClothData& GetData() const { return m_Data; }

    private:
        ClothData m_Data;
    };
    // ImportedClothMaxDistance is optional; absence pins every particle. Seams take the minimum map value.
    ClothData  CreateClothForSection(AssetID mesh, const MeshData&, uint32_t section, float weldEpsilon = 1e-5f);
    void       RebuildClothConstraints(ClothData&);
    void       WriteClothMap(ClothData&, ClothMap, const std::vector<float>&);
    void       ValidateClothData(const ClothData&); // Throws; called before publication and by the YAML decoder.
    bool       ValidateClothBinding(const ClothData&, AssetID, const MeshData&, uint32_t section);
    YAML::Node EncodeClothNode(const ClothData&);
    ClothData  DecodeClothNode(const YAML::Node&);
} // namespace Kans
