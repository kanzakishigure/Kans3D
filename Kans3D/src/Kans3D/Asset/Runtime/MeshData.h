#pragma once

#include <array>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Core/Math/Bounds.h"

namespace Kans
{
    struct MeshVertexData
    {
        std::array<float, 3> Position {};
        std::array<float, 3> Normal {};
        std::array<float, 2> UV {};
        std::array<float, 4> Color {1, 1, 1, 1};
        std::array<float, 4> Tangent {1, 0, 0, 1};
    };

    enum class MeshVertexFormat : uint32_t
    {
        PositionNormalUVColorTangent = 1,
        Skinned                      = 2,
    };

    struct MeshSkinVertex
    {
        std::array<uint32_t, 4> Joints {};
        std::array<float, 4>    Weights {};
    };
    // Sorted sparse deltas in source mesh space. Unlisted vertices have exactly zero delta.
    struct MorphVertexDelta
    {
        uint32_t             Vertex = 0;
        std::array<float, 3> Position {}, Normal {}, Tangent {};
    };
    struct MeshMorphTarget
    {
        std::string                   Name;
        float                         DefaultWeight = 0;
        std::vector<MorphVertexDelta> Deltas;
        // Includes zero, so untouched vertices are covered as well.
        Bounds PositionBounds;
    };
    // Inserts strongest influences in descending order; ties preserve source order.
    void InsertSkinInfluence(MeshSkinVertex&, uint32_t joint, float weight);
    bool NormalizeSkinWeights(MeshSkinVertex&);

    struct MeshSection
    {
        uint32_t FirstIndex = 0, IndexCount = 0, MaterialSlot = 0;
        Bounds   LocalBounds;
    };

    struct MeshMaterialSlot
    {
        std::string Name, ObjectKey;
        AssetID     DefaultMaterial, AuthoredMaterial;
    };

    // One geometry Product. Node instances/transforms belong to model/scene data,
    // rather than duplicating the same geometry for every source node instance.
    struct MeshData
    {
        std::vector<MeshVertexData> Vertices;
        std::vector<uint32_t>       Indices;
        // Optional welded, angle-weighted unit normals in the same space as Vertices.Normal.
        std::vector<std::array<float, 3>> SmoothNormals;
        bool                              HasSmoothNormals() const { return !SmoothNormals.empty(); }
        // Editor compatibility: first slot (or the selected legacy part view).
        // Invalid ID explicitly means no material binding.
        AssetID Material;
        // Original embedded slot, independent of the bound/remapped material.
        AssetID AuthoredMaterial;
        MeshVertexFormat              VertexFormat = MeshVertexFormat::PositionNormalUVColorTangent;
        std::vector<MeshSection>      Sections;
        std::vector<MeshMaterialSlot> MaterialSlots;
        Bounds                        LocalBounds;
        AssetID                       Skeleton;
        std::vector<MeshSkinVertex>   Skin;
        std::vector<float>            ImportedClothMaxDistance; // Optional, metres, one per render vertex.
        // Each box contains model-space bind vertices with a nonzero influence from that joint.
        std::vector<Bounds> JointBounds;
        // Source mesh -> skeleton model space. Applied once during GPU upload; base vertex bytes stay unchanged.
        glm::mat4 SkinBindTransform {1};
        std::vector<MeshMorphTarget> MorphTargets;
    };
    // Explicit construction helper for procedural single-material meshes. Importers
    // build their sections and bounds in source mesh space instead.
    void                                                   BuildSingleSection(MeshData& mesh);
    void                                                   GenerateSmoothNormals(MeshData& mesh);
    void                                                   BuildMorphBounds(MeshMorphTarget&);
    Bounds MorphedBounds(const MeshData&, const std::vector<float>& weights);
    // Import-only fallback, preserving the base mesh's authored normal offset.
    void RecomputeMorphNormals(const std::vector<MeshVertexData>&, const std::vector<uint32_t>&, MeshMorphTarget&);
    [[nodiscard]] std::variant<std::monostate, AssetError> ValidateMeshData(const MeshData& mesh);

    // CPU runtime mesh. Rendering resources are created by the renderer's asset cache.
    class MeshAsset final : public Asset
    {
    public:
        explicit MeshAsset(MeshData data) : m_Data(std::move(data)) {}
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        AssetType        GetAssetType() const override { return GetStaticType(); }
        const MeshData&  GetData() const { return m_Data; }

    private:
        MeshData m_Data;
    };
} // namespace Kans
