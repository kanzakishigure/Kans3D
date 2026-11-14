#pragma once

#include <array>
#include <optional>
#include <variant>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "MaterialBindingRecipeData.h"

namespace Kans
{
    // Engine texture roles stored in Material Products. Values 1-21 follow Assimp's
    // stable aiTextureType numbering; other Assimp types are mapped by the importer.
    enum class MaterialTextureSemantic : uint32_t
    {
        Diffuse           = 1,
        Specular          = 2,
        Ambient           = 3,
        Emissive          = 4,
        Height            = 5,
        Normal            = 6,
        Shininess         = 7,
        Opacity           = 8,
        Displacement      = 9,
        Lightmap          = 10,
        Reflection        = 11,
        BaseColor         = 12,
        NormalCamera      = 13,
        EmissionColor     = 14,
        Metalness         = 15,
        DiffuseRoughness  = 16,
        AmbientOcclusion  = 17,
        Unknown           = 18,
        Sheen             = 19,
        Clearcoat         = 20,
        Transmission      = 21,
        MetallicRoughness = 22, // glTF: roughness in G, metalness in B.
    };
    constexpr uint32_t MaxMaterialTextureSemantic = 22;

    struct MaterialTextureBinding
    {
        uint32_t Semantic  = 0;
        uint32_t UVChannel = 0;
        AssetID  Texture;
    };

    enum class MaterialAlphaMode : uint32_t
    {
        Opaque   = 0,
        Mask     = 1,
        Blend    = 2,
        Dithered = 3,
        // 4 and 5 are the published shader-only alpha-to-coverage modes.
        Additive      = 6,
        Premultiplied = 7,
    };

    inline bool IsTransparentMaterial(MaterialAlphaMode mode)
    {
        return mode == MaterialAlphaMode::Blend || mode == MaterialAlphaMode::Additive ||
               mode == MaterialAlphaMode::Premultiplied;
    }

    // Describes content stability, independently of the reconstruction technique.
    enum class MaterialTemporalResponse : uint32_t
    {
        Default,
        Reactive,
        Composition
    };

    // Render state belongs to the material, independently of its Shader.
    struct MaterialRenderState
    {
        MaterialAlphaMode AlphaMode      = MaterialAlphaMode::Opaque;
        float             AlphaCutoff    = 0.5f;
        bool              DoubleSided    = false;
        bool              ReceiveShadows = true;
        MaterialTemporalResponse TemporalResponse = MaterialTemporalResponse::Default;
    };

    [[nodiscard]] bool IsValidMaterialRenderState(const MaterialRenderState& state);

    struct InstanceMaterialParameter
    {
        std::string          Parameter;
        std::array<float, 4> Value {};
        bool                 Resolved = false;
        // Runtime overrides also accept scalar/vec2 float declarations; drivers remain vec3/vec4.
        bool RuntimeOverride = false;
    };
    using InstanceMaterialParameters = std::vector<InstanceMaterialParameter>;

    struct MaterialData
    {
        std::string                         Name;
        std::array<float, 4>                BaseColor {1, 1, 1, 1};
        float                               Metallic  = 0;
        float                               Roughness = 1;
        std::vector<MaterialTextureBinding> Textures;
        // Empty in old Products; resolves to the reserved engine Standard Shader.
        AssetID Shader;
        // Absent in KMAT v1/v2; an explicit (possibly empty) recipe is persisted in v3.
        std::optional<MaterialBindingRecipe> Bindings;
        MaterialRenderState                  Render;
        std::vector<std::string>             DisabledPasses;
    };

    [[nodiscard]] std::variant<std::monostate, AssetError> ValidateMaterialData(const MaterialData& data);

    struct ModelNodeData
    {
        std::string Name;
        int64_t     Parent = -1;
        // Row-major local transform, matching imported node data.
        std::array<float, 16> LocalTransform {};
        std::vector<AssetID>  Meshes;
        std::string           Key; // Stable source-node identity, independent of array position.
    };

    struct ModelSectionIdentity
    {
        std::string ObjectKey;
        AssetID     OldMesh, Mesh;
        uint32_t    Section = 0;
    };

    struct PrefabNodeData
    {
        std::string             Key, Name;
        int64_t                 Parent = -1;
        std::array<float, 16>   LocalTransform {};
        AssetID                 Mesh;
        std::optional<uint32_t> Section;   // A source part may select one shared-mesh section.
        std::vector<AssetID>    Materials; // Empty entries use mesh defaults.
    };

    struct ModelData
    {
        std::vector<ModelNodeData>        Nodes;
        std::vector<ModelSectionIdentity> SectionIdentities;
        std::vector<PrefabNodeData>       Entities;
        AssetID                           Skeleton;
        std::vector<AssetID>              AnimationClips;
    };

    // Builds the prefab entity list while retaining the stage-2 model view.
    std::vector<PrefabNodeData> BuildPrefabEntities(const ModelData& data);

    class ModelAsset final : public Asset
    {
    public:
        explicit ModelAsset(ModelData data);
        static AssetType GetStaticType() { return AssetType::Model; }
        AssetType        GetAssetType() const override { return GetStaticType(); }
        // Compatibility view for the unchanged editor Parts list: old part IDs.
        const ModelData& GetData() const { return m_EditorData; }
        // Canonical product: nodes reference shared meshes, identities name sections.
        const ModelData&                   GetMeshData() const { return m_Data; }
        const std::vector<PrefabNodeData>& GetEntities() const { return m_Data.Entities; }

    private:
        ModelData m_Data, m_EditorData;
    };

    using PrefabAsset = ModelAsset;

    // CPU runtime material. The existing renderer MaterialAsset remains a legacy
    // rendering wrapper until the renderer bridge is migrated to this data.
    class MaterialDataAsset final : public Asset
    {
    public:
        explicit MaterialDataAsset(MaterialData data) : m_Data(std::move(data)) {}
        static AssetType    GetStaticType() { return AssetType::Material; }
        AssetType           GetAssetType() const override { return GetStaticType(); }
        const MaterialData& GetData() const { return m_Data; }

    private:
        MaterialData m_Data;
    };
} // namespace Kans
