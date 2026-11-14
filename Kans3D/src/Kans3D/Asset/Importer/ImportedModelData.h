#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Animation/AnimationData.h"
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"

namespace Kans
{
    using ImportedVertex = MeshVertexData;

    struct ImportedMeshData
    {
        std::string                 Name;
        uint32_t                    MaterialIndex = 0;
        std::vector<ImportedVertex> Vertices;
        std::vector<std::vector<std::array<float, 4>>> ColorSets;
        std::vector<uint32_t>       Indices;
        std::vector<MeshSkinVertex> Skin;
        std::vector<MeshMorphTarget> MorphTargets;
        // Import-only FBX identity. Never serialized into a mesh product.
        int32_t                           SourceGeometry = -1;
        std::vector<uint32_t>             SourceControlPoints;
        std::vector<std::array<float, 3>> SmoothNormals;
        uint32_t                          UVChannels    = 0;
        uint32_t                          ColorChannels = 0;
        int32_t                     Node = -1;
        glm::mat4                   BindToModel {1};
    };

    struct ImportedTextureReference
    {
        uint32_t    Semantic  = 0;
        uint32_t    UVChannel = 0;
        std::string Source;
    };

    struct ImportedMaterialData
    {
        std::string          Name;
        std::array<float, 4> BaseColor {1, 1, 1, 1};
        float                Metallic  = 0;
        float                Roughness = 1;
        // False when the source marks the material as transparent or alpha tested.
        bool                                  Opaque = true;
        std::vector<ImportedTextureReference> Textures;
        std::array<float, 3>                  EmissiveColor {0, 0, 0};
        MaterialRenderState                   Render;
    };

    struct ImportedNodeData
    {
        std::string Name;
        int64_t     Parent = -1;
        // Row-major local transform; world transforms are derived by consumers.
        std::array<float, 16> LocalTransform {};
        std::vector<uint32_t> Meshes;
    };

    struct ImportedModelData
    {
        std::vector<ImportedMeshData>     Meshes;
        std::vector<ImportedMaterialData> Materials;
        std::vector<ImportedNodeData>     Nodes;
        std::optional<SkeletonData>       Skeleton;
        std::vector<AnimationClipData>    Clips;
        // Successfully read files, including .mtl and external glTF buffers.
        std::vector<std::filesystem::path> SourceFiles;
        // Source content that was skipped, such as animations.
        std::vector<std::string> Warnings;
    };

    // Owns all returned data. No Assimp or rendering objects escape this boundary.
    // Embedded textures are reported but not imported.
    [[nodiscard]] std::variant<ImportedModelData, AssetError>
    ParseModelCpu(const std::filesystem::path& path,
                  std::optional<bool>          recomputeMorphNormals = {},
                  bool                         sourceNormals         = false);
    // Closed input IO: undeclared paths are rejected before opening them.
    [[nodiscard]] std::variant<ImportedModelData, AssetError>
    ParseModelCpu(const std::filesystem::path&              path,
                  const std::vector<std::filesystem::path>& allowedFiles,
                  std::optional<bool>                       recomputeMorphNormals = {},
                  bool                                      sourceNormals         = false);
    // Inspection avoids Assimp's validator retry for morph-only animations: one
    // Assimp read, followed by the same owned-data validation as regular parsing.
    [[nodiscard]] std::variant<ImportedModelData, AssetError> InspectModelCpu(const std::filesystem::path& path);
    // Dependency discovery reads materials and file references only. Meshes,
    // skeletons and animation data are built/validated by ParseModelCpu at import.
    [[nodiscard]] std::variant<ImportedModelData, AssetError> DiscoverModelCpuInputs(const std::filesystem::path& path);
    // Uniformly scales the model by scaling its root nodes.
    void ScaleModel(ImportedModelData& model, float scale);
    // Finds the file a model's texture reference points to. Accepts backslashes,
    // absolute paths from other machines and case mismatches by falling back to a
    // file-name search below the model directory.
    [[nodiscard]] std::variant<std::filesystem::path, AssetError>
    ResolveModelTexture(const std::filesystem::path& modelFile, const std::string& reference);
} // namespace Kans
