#pragma once
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Asset/Runtime/MaterialBindings.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "Kans3D/Asset/Runtime/TextureData.h"
#include "Kans3D/Core/Math/Bounds.h"
#include "Kans3D/Renderer/Resource/Texture.h"
#include "Kans3D/Renderer/Shading/GpuShader.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include "MeshKey.h"

namespace Kans
{
    struct ShadowResources;
    struct GpuMesh
    {
        std::shared_ptr<RenderHardware::Device> Device;
        RenderHardware::Handle<RenderHardware::Buffer> Vertices, Indices, NormalLines, Skin, NormalSkin, BindPalette;
        RenderHardware::Handle<RenderHardware::Buffer> SmoothNormals, BindSmoothNormals;
        // Original mesh-local streams for submissions without an animator palette.
        RenderHardware::Handle<RenderHardware::Buffer> BindVertices, BindNormalLines;
        std::vector<RenderHardware::VertexBinding>     VertexBindings {{0, 64, false}};
        std::vector<RenderHardware::VertexAttribute>   Attributes {{0, 0, 0, RenderHardware::VertexFormat::Float3},
                                                                   {1, 0, 12, RenderHardware::VertexFormat::Float3},
                                                                   {2, 0, 24, RenderHardware::VertexFormat::Float2},
                                                                   {3, 0, 32, RenderHardware::VertexFormat::Float4},
                                                                   {4, 0, 48, RenderHardware::VertexFormat::Float4}};
        RenderHardware::Handle<RenderHardware::Buffer> MorphDeltas;
        std::vector<ShapeKey>                          MorphTargets;
        uint32_t                                       VertexCount     = 0;
        bool                                           HasSkeletonSkin = false;
        uint32_t         IndexCount = 0;
        AssetID          Material;
        AssetID          AuthoredMaterial;
        Bounds           LocalBounds; // In mesh space.
        std::vector<MeshSection>                       Sections;
        std::vector<MeshMaterialSlot>                  MaterialSlots;
    };

    struct GpuMaterial
    {
        AssetID Shader;
    };

    // Material data packed for its Shader: set-1 uniform blocks and textures.
    struct GpuMaterialBindings
    {
        mutable RenderHardware::Handle<RenderHardware::BindGroup>            Group;
        mutable std::vector<RenderHardware::Handle<RenderHardware::Texture>> Images;
        // Replaced descriptor snapshots may still belong to unsubmitted command lists.
        mutable std::vector<std::weak_ptr<RenderHardware::BindGroup>> RetiredGroups;
        uint64_t                                                      SnapshotFrame = UINT64_MAX;
        AssetID Shader; // Actual program, including Standard while a custom Shader is unavailable.
        MaterialRenderState Render;
        std::vector<std::string> DisabledPasses;
        bool                     PassEnabled(const std::string& name) const
        {
            return std::find(DisabledPasses.begin(), DisabledPasses.end(), name) == DisabledPasses.end();
        }
        std::vector<std::pair<uint32_t, GpuUniformBuffer>> Blocks;
        std::vector<std::pair<uint32_t, AssetID>>          Textures;
        struct ParameterSlot
        {
            std::string Name;
            size_t      Block = 0, Offset = 0;
            uint32_t    Components = 0;
        };
        std::vector<MaterialUniformBlock>                  Defaults;
        std::vector<ParameterSlot>                         ParameterSlots;
        GpuMaterialBindings                                Snapshot() const
        {
            GpuMaterialBindings copy;
            copy.Group    = Group;
            copy.Images   = Images;
            copy.Shader   = Shader;
            copy.Render   = Render;
            copy.DisabledPasses = DisabledPasses;
            copy.Textures = Textures;
            copy.Defaults       = Defaults;
            copy.ParameterSlots = ParameterSlots;
            for (const auto& block : Blocks)
                copy.Blocks.emplace_back(block.first, block.second.Snapshot());
            return copy;
        }
    };

    struct ModelInstance
    {
        AssetID   Mesh;
        glm::mat4 Transform {1.0f}; // Model-space transform of the node.
        uint32_t  Section = 0;
        AssetID   Part;                // Stable pre-section part identity for scene/editor overrides.
        bool      AllSections = false; // Unmapped/procedural models draw all mesh sections.
    };

    // Flattened node hierarchy of a Model asset.
    struct ModelInstances
    {
        std::vector<ModelInstance> Instances;
    };

    // Render-thread cache from runtime CPU assets to GPU resources.
    //
    // Assets are requested from the AssetManager without blocking. While a newer
    // version of an asset is loading, the last uploaded resource stays in use.
    // Resources are rebuilt when the AssetManager returns a different CPU object.
    class RenderAssetCache
    {
    public:
        explicit RenderAssetCache(AssetManager& assets, std::shared_ptr<RenderHardware::Device> device = {}) :
            m_Device(std::move(device)), m_Assets(assets)
        {}
        std::shared_ptr<RenderHardware::Device>         GetDevice();
        RenderHardware::Handle<RenderHardware::Texture> GetTextureResource(AssetID id);
        // Sampling belongs to an independent RHI sampler, never mutable native image state.
        void SetTextureSampler(AssetID id, const RenderHardware::SamplerDesc& desc);
        RenderHardware::Handle<RenderHardware::BindGroup>
        GetMaterialGroup(const GpuShader& shader, const GpuMaterialBindings& bindings, bool* complete = nullptr);

        const GpuMaterialBindings* GetInstanceMaterialBindings(const GpuMaterialBindings&                   base,
                                                               const Ref<const InstanceMaterialParameters>& parameters);

        // Evicts resources not used for `unusedFrames` frames.
        void BeginFrame(uint64_t unusedFrames = 600);
        void Clear();
        std::shared_ptr<ShadowResources> GetShadowFallback();

        const ModelInstances* GetModel(AssetID id);
        const GpuMesh*        GetMesh(const MeshKey& id);
        RenderHardware::Handle<RenderHardware::Buffer> GetSkinningPalette(const Ref<const SkinningPose>&,
                                                                          const GpuMesh* mesh = nullptr);
        const GpuMaterial*    GetMaterial(AssetID id);
        Ref<Texture2D>        GetTexture(AssetID id);
        const GpuShader*      GetShader(AssetID id);
        // Model-space bounds of a model's meshes; empty until the model and its
        // meshes are uploaded.
        Bounds GetModelBounds(AssetID model);
        // Packed material parameters, using Standard for missing/pending/failed
        // custom Shaders. `standard` forces Standard for viewport debug views.
        const GpuMaterialBindings* GetMaterialBindings(AssetID material, bool standard = false);

        struct EditableMeshStatistics
        {
            uint64_t Uploads = 0, Releases = 0;
            double   LastUploadMilliseconds = 0;
        };
        const EditableMeshStatistics& GetEditableMeshStatistics() const { return m_EditableStats; }
        size_t                        GetEditableMeshCount() const { return m_EditableMeshes.size(); }
        void                          WarnMaterialDomain(AssetID id, bool postList);
        AssetManager& GetAssetManager() { return m_Assets; }

    private:
        template<typename T, typename Resource>
        struct Entry
        {
            Ref<const T>              Source;
            Scope<Resource>           Value;
            uint64_t                  LastUsedFrame = 0;
            std::optional<AssetError> ReportedError;
        };
        template<typename T, typename Resource, typename Build>
        Resource* Resolve(std::unordered_map<AssetID, Entry<T, Resource>>& entries, AssetID id, Build&& build);

        std::unordered_set<AssetID> m_WarnedPost, m_WarnedMesh;
        struct BindingsEntry
        {
            Ref<const MaterialDataAsset>  Material;
            Ref<const ShaderDataAsset>    Shader;
            Scope<GpuMaterialBindings>    Value;
            uint64_t                      LastUsedFrame = 0;
        };

        const GpuMaterialBindings* ResolveBindings(AssetID                      materialID,
                                                   Ref<const MaterialDataAsset> materialSource,
                                                   Ref<const ShaderDataAsset>   shaderSource,
                                                   bool                         standard);

        std::shared_ptr<RenderHardware::Device>                                      m_Device;
        RenderHardware::Handle<RenderHardware::Sampler>                              m_MaterialSampler;
        std::unordered_map<AssetID, RenderHardware::Handle<RenderHardware::Sampler>> m_TextureSamplers;
        std::map<std::pair<RenderHardware::Filter, RenderHardware::AddressMode>,
                 RenderHardware::Handle<RenderHardware::Sampler>>
                                                                                     m_ProductSamplers;
        std::array<Ref<Texture2D>, 3>                                      m_FallbackTextures;
        AssetManager&                                                      m_Assets;
        std::shared_ptr<ShadowResources>                                             m_ShadowFallback;
        struct EditableEntry
        {
            std::weak_ptr<const RuntimeMesh> Source;
            Scope<GpuMesh>                   Value;
            std::optional<AssetError>        ReportedError;
        };
        struct StreamEntry
        {
            std::weak_ptr<const DynamicVertexStream> Source;
            std::vector<Scope<GpuMesh>>              Pool;
            GpuMesh*                                 Current = nullptr;
            uint64_t                                 Version = UINT64_MAX;
            std::vector<float>                       Lines;
        };
        std::map<const DynamicVertexStream*, StreamEntry>                  m_Streams;
        std::unordered_map<uint64_t, EditableEntry>                        m_EditableMeshes;
        EditableMeshStatistics                                             m_EditableStats;
        struct PaletteEntry
        {
            std::weak_ptr<const SkinningPose>                           Source;
            std::weak_ptr<RenderHardware::Buffer>                       MeshSource;
            std::vector<glm::mat4>                                      Packed;
            std::vector<int32_t>                                        ShapeIndices;
            uint64_t                                                    Version = UINT64_MAX, Frame = 0;
            RenderHardware::Handle<RenderHardware::Buffer>              Current;
            std::vector<RenderHardware::Handle<RenderHardware::Buffer>> Pool;
        };
        std::map<std::pair<const SkinningPose*, const GpuMesh*>, PaletteEntry> m_Palettes;
        struct InstanceBindingsEntry
        {
            std::weak_ptr<const InstanceMaterialParameters> Source;
            std::weak_ptr<RenderHardware::Buffer>           Base;
            std::vector<Scope<GpuMaterialBindings>>         Pool;
            std::vector<MaterialUniformBlock>               Scratch;
            uint64_t                                        LastUsedFrame = 0;
        };
        std::map<std::pair<const InstanceMaterialParameters*, const RenderHardware::Buffer*>, InstanceBindingsEntry>
                                                                           m_InstanceBindings;
        uint64_t                                                           m_Frame = 0;
        std::unordered_map<AssetID, Entry<ModelAsset, ModelInstances>>     m_Models;
        std::unordered_map<AssetID, Entry<MeshAsset, GpuMesh>>             m_Meshes;
        std::unordered_map<AssetID, Entry<MaterialDataAsset, GpuMaterial>> m_Materials;
        std::unordered_map<AssetID, Entry<TextureAsset, Ref<Texture2D>>>   m_Textures;
        std::unordered_map<AssetID, Entry<ShaderDataAsset, GpuShader>>     m_Shaders;
        std::unordered_map<AssetID, BindingsEntry>                         m_Bindings;
        std::unordered_map<AssetID, BindingsEntry>                         m_StandardBindings;
    };

    // Uploads CPU mesh data to RHI buffers.
    [[nodiscard]] std::variant<GpuMesh, AssetError> UploadMesh(const MeshData&                         data,
                                                               std::shared_ptr<RenderHardware::Device> device = {});
    // Converts a Model hierarchy to per-mesh model-space transforms.
    [[nodiscard]] std::variant<ModelInstances, AssetError> FlattenModel(const ModelData& data);
    // Maps imported material parameters and texture semantics to the engine PBR material.
    GpuMaterial MakeGpuMaterial(const MaterialData& data);
    // Packs a material into RHI uniform buffers; group 1 is resolved lazily with its textures.
    [[nodiscard]] std::variant<GpuMaterialBindings, AssetError>
    MakeGpuMaterialBindings(const MaterialData&                     material,
                            const ShaderData&                       shader,
                            std::shared_ptr<RenderHardware::Device> device = {});
} // namespace Kans
