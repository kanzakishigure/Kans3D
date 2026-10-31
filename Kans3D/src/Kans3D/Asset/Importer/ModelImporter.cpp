#include "kspch.h"
#include "ModelImporter.h"
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Product/MeshProduct.h"
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "ImportedModelData.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

namespace Kans
{
    namespace
    {
        std::string ObjectKey(const char* type, const std::string& name)
        {
            // Length prefix prevents delimiter characters in names from aliasing.
            return std::string(type) + ":" + std::to_string(name.size()) + ":" + name;
        }

        struct StagedProduct
        {
            ProductDescriptor      Descriptor;
            std::vector<std::byte> Bytes;
        };

        struct StagingCleanup
        {
            const std::vector<std::filesystem::path>& Paths;
            size_t                                    Attempted = 0;
            bool                                      Complete  = false;
            ~StagingCleanup()
            {
                if (Complete)
                    return;
                for (size_t i = 0; i < Attempted; ++i)
                {
                    std::error_code ignored;
                    std::filesystem::remove(Paths[i], ignored);
                }
            }
        };

        // Makes repeated keys unique without using array position where possible:
        // a repeated key first gets its discriminator (for meshes, the referencing
        // node path), then an occurrence number if it is still not unique.
        void MakeUnique(std::vector<std::string>& keys, const std::vector<std::string>& discriminators)
        {
            std::map<std::string, size_t> counts;
            for (const auto& key : keys)
                ++counts[key];
            for (size_t i = 0; i < keys.size(); ++i)
                if (counts[keys[i]] > 1 && !discriminators[i].empty())
                    keys[i] += ":" + std::to_string(discriminators[i].size()) + ":" + discriminators[i];
            std::map<std::string, size_t> seen;
            for (auto& key : keys)
                if (const size_t occurrence = seen[key]++; occurrence > 0)
                    key += "#" + std::to_string(occurrence);
        }

        void AddUnique(std::vector<AssetID>& values, AssetID id)
        {
            if (id.IsValid() && std::find(values.begin(), values.end(), id) == values.end())
                values.push_back(id);
        }
    } // namespace

    namespace
    {
        // Settings: "scale" (positive number, default 1) multiplies the model size.
        enum class SmoothNormalMode
        {
            Off,
            Weld,
            Source
        };
        struct ModelSettings
        {
            AnimationNameMapping           Mapping;
            float                          Scale = 1;
            int                            ClothColorSet = -1, ClothColorChannel = 0;
            float                          ClothColorScale = 1;
            SmoothNormalMode               SmoothNormals = SmoothNormalMode::Off;
            std::optional<bool>            RecomputeMorphNormals;
            std::map<std::string, AssetID> Materials;
            bool Meshes = true, Animations = true, ImportMaterials = true, ImportSkeleton = true, MorphTargets = true;
            std::map<std::string, bool> MeshIncludes, ClipIncludes;
            bool                        IncludeMesh(const std::string& name) const
            {
                auto i = MeshIncludes.find(name);
                return Meshes && (i == MeshIncludes.end() || i->second);
            }
            bool IncludeClip(const std::string& name) const
            {
                auto i = ClipIncludes.find(name);
                return Animations && (i == ClipIncludes.end() || i->second);
            }
        };

        std::variant<ModelSettings, AssetError> ParseModelSettings(const std::map<std::string, std::string>& settings)
        {
            auto compression = ParseAnimationCompressionSettings(settings);
            if (auto* error = std::get_if<AssetError>(&compression))
                return *error;
            ModelSettings parsed;
            for (const auto& [key, value] : settings)
            {
                if (key.compare(0, 8, "include.") == 0)
                {
                    if (value != "true" && value != "false")
                        return AssetError {AssetErrorCode::InvalidArgument, "Invalid model include boolean: " + key};
                    bool enabled = value == "true";
                    if (key == "include.meshes")
                        parsed.Meshes = enabled;
                    else if (key == "include.animations")
                        parsed.Animations = enabled;
                    else if (key == "include.materials")
                        parsed.ImportMaterials = enabled;
                    else if (key == "include.skeleton")
                        parsed.ImportSkeleton = enabled;
                    else if (key == "include.morph_targets")
                        parsed.MorphTargets = enabled;
                    else if (key.compare(0, 13, "include.mesh.") == 0 && key.size() > 13)
                        parsed.MeshIncludes[key.substr(13)] = enabled;
                    else if (key.compare(0, 13, "include.clip.") == 0 && key.size() > 13)
                        parsed.ClipIncludes[key.substr(13)] = enabled;
                    else
                        return AssetError {AssetErrorCode::InvalidArgument, "Unknown model include key: " + key};
                    continue;
                }
                if (key.compare(0, 9, "material.") == 0 && key.compare(9, 9, "material:") == 0)
                {
                    if (key.size() <= 18)
                        return AssetError {AssetErrorCode::InvalidArgument, "Empty material slot key"};
                    if (value.empty())
                        continue;
                    const auto id = AssetID::FromString(value);
                    if (!id.IsValid() || id.ToString() != value)
                        return AssetError {AssetErrorCode::InvalidArgument, "Invalid material remap AssetID: " + value};
                    parsed.Materials.emplace(key.substr(9), id);
                    continue;
                }
                if (key == "animation.compression" || key.compare(0, 22, "animation.compression.") == 0 ||
                    key == "animation.error.position" || key.compare(0, 25, "animation.error.position.") == 0 ||
                    key == "animation.error.shell" || key.compare(0, 22, "animation.error.shell.") == 0 ||
                    key == "animation.error.morph" || key.compare(0, 22, "animation.error.morph.") == 0)
                    continue;
                if (key == "animation.strip_prefix")
                {
                    parsed.Mapping.StripPrefix = value;
                    continue;
                }
                if (key.compare(0, 14, "animation.map.") == 0)
                {
                    if (key.size() == 14)
                        return AssetError {AssetErrorCode::InvalidArgument, "Empty animation mapping name"};
                    if (!value.empty())
                        parsed.Mapping.Names[key.substr(14)] = value;
                    continue;
                }
                if (key == "cloth.max_distance.color_set")
                {
                    char*      end = nullptr;
                    const auto n   = std::strtol(value.c_str(), &end, 10);
                    if (value.empty() || *end || n < -1 || n > 7)
                        return AssetError {AssetErrorCode::InvalidArgument,
                                           "Cloth colour set must be -1 (off) or 0..7"};
                    parsed.ClothColorSet = int(n);
                    continue;
                }
                if (key == "cloth.max_distance.channel")
                {
                    const auto channel = std::string("RGBA").find(value);
                    if (value.size() != 1 || channel == std::string::npos)
                        return AssetError {AssetErrorCode::InvalidArgument, "Cloth colour channel must be R/G/B/A"};
                    parsed.ClothColorChannel = int(channel);
                    continue;
                }
                if (key == "cloth.max_distance.scale")
                {
                    char*       end = nullptr;
                    const float n   = std::strtof(value.c_str(), &end);
                    if (value.empty() || *end || !std::isfinite(n) || n < 0)
                        return AssetError {AssetErrorCode::InvalidArgument, "Invalid cloth distance scale"};
                    parsed.ClothColorScale = n;
                    continue;
                }
                if (key == "mesh.smooth_normals")
                {
                    if (value != "true" && value != "false" && value != "weld" && value != "source")
                        return AssetError {AssetErrorCode::InvalidArgument,
                                           "mesh.smooth_normals must be false, weld, source, or true"};
                    parsed.SmoothNormals = value == "source" ? SmoothNormalMode::Source :
                                           value == "false"  ? SmoothNormalMode::Off :
                                                               SmoothNormalMode::Weld;
                    continue;
                }
                if (key == "morph.recompute_normals")
                {
                    if (value == "auto")
                        continue;
                    if (value != "true" && value != "false")
                        return AssetError {AssetErrorCode::InvalidArgument,
                                           "morph.recompute_normals must be auto, true or false"};
                    parsed.RecomputeMorphNormals = value == "true";
                    continue;
                }
                if (key != "scale")
                    return AssetError {AssetErrorCode::InvalidArgument, "Unknown model import setting: " + key};
                char* end = nullptr;
                parsed.Scale = std::strtof(value.c_str(), &end);
                if (value.empty() || *end || !std::isfinite(parsed.Scale) || parsed.Scale <= 0.0f)
                    return AssetError {AssetErrorCode::InvalidArgument, "Model scale must be a positive number"};
            }
            return parsed;
        }
    } // namespace

    std::variant<std::monostate, AssetError>
    ModelImporter::ValidateSettings(const std::map<std::string, std::string>& settings) const
    {
        auto parsed = ParseModelSettings(settings);
        if (auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        return std::monostate {};
    }

    std::variant<SourceInventory, AssetError> ModelImporter::Inspect(const ImportContext& context) const
    {
        if (!context.SourceFile.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Inspection requires an absolute source path"};
        auto settings = ParseModelSettings(context.Settings);
        if (auto* error = std::get_if<AssetError>(&settings))
            return *error;
        auto parsed = InspectModelCpu(context.SourceFile);
        if (auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        const auto&     model = std::get<ImportedModelData>(parsed);
        SourceInventory inventory;
        inventory.Warnings = model.Warnings;
        std::vector<std::string> slots;
        for (const auto& m : model.Materials)
            slots.push_back(ObjectKey("material", m.Name));
        MakeUnique(slots, std::vector<std::string>(slots.size()));
        for (size_t i = 0; i < model.Materials.size(); ++i)
        {
            InspectedMaterialSlot slot {slots[i], model.Materials[i].Name, {}};
            const auto&           remaps = std::get<ModelSettings>(settings).Materials;
            if (auto found = remaps.find(slot.Key); found != remaps.end())
                slot.SuggestedMaterial = found->second;
            inventory.Materials.push_back(slot);
            for (const auto& t : model.Materials[i].Textures)
            {
                // Assimp may expose one image as both diffuse and base color.
                // Inventory lists each source reference once per material slot.
                if (std::any_of(inventory.Textures.begin(), inventory.Textures.end(), [&](const auto& texture) {
                        return texture.MaterialSlot == slot.Key && texture.Reference == t.Source;
                    }))
                    continue;
                InspectedTextureReference texture;
                texture.MaterialSlot = slot.Key;
                texture.Reference    = t.Source;
                texture.Embedded     = !t.Source.empty() && t.Source[0] == '*';
                texture.Found        = texture.Embedded;
                if (!texture.Embedded)
                {
                    auto path = ResolveModelTexture(context.SourceFile, t.Source);
                    if (auto* found = std::get_if<std::filesystem::path>(&path))
                    {
                        texture.ResolvedPath = *found;
                        texture.Found        = true;
                    }
                    else
                    {
                        std::string normalized = t.Source;
                        std::replace(normalized.begin(), normalized.end(), '\\', '/');
                        auto referenced      = std::filesystem::u8path(normalized);
                        texture.ResolvedPath = (context.SourceFile.parent_path() /
                                                (referenced.is_absolute() ? referenced.filename() : referenced))
                                                   .lexically_normal();
                    }
                }
                inventory.Textures.push_back(std::move(texture));
            }
        }
        std::vector<std::string> meshKeys, paths(model.Meshes.size()), nodePaths(model.Nodes.size());
        for (size_t i = 0; i < model.Nodes.size(); ++i)
        {
            const auto& node = model.Nodes[i];
            nodePaths[i]     = (node.Parent >= 0 ? nodePaths.at(node.Parent) : "") + "/" + node.Name;
            for (auto m : node.Meshes)
                if (paths.at(m).empty())
                    paths[m] = nodePaths[i];
        }
        for (const auto& mesh : model.Meshes)
            meshKeys.push_back(ObjectKey("mesh", mesh.Name) + ":" + slots.at(mesh.MaterialIndex));
        MakeUnique(meshKeys, paths);
        for (size_t i = 0; i < model.Meshes.size(); ++i)
        {
            const auto&   mesh = model.Meshes[i];
            InspectedMesh item;
            item.Name             = mesh.Name;
            item.ObjectKey        = meshKeys[i];
            item.VertexCount      = mesh.Vertices.size();
            item.TriangleCount    = mesh.Indices.size() / 3;
            item.Skinned          = !mesh.Skin.empty();
            item.MorphTargetCount = uint32_t(mesh.MorphTargets.size());
            item.MaterialSlots.push_back(slots.at(mesh.MaterialIndex));
            for (const auto& target : mesh.MorphTargets)
            {
                item.MorphTargets.push_back(target.Name);
                if (std::find(inventory.MorphTargets.begin(), inventory.MorphTargets.end(), target.Name) ==
                    inventory.MorphTargets.end())
                    inventory.MorphTargets.push_back(target.Name);
            }
            inventory.Meshes.push_back(std::move(item));
        }
        if (model.Skeleton)
        {
            InspectedSkeleton skeleton;
            skeleton.JointCount = uint32_t(model.Skeleton->Joints.size());
            for (const auto& joint : model.Skeleton->Joints)
                if (joint.Parent < 0)
                {
                    skeleton.RootName = joint.Name;
                    break;
                }
            inventory.Skeleton = std::move(skeleton);
        }
        std::vector<std::string> clipKeys;
        for (const auto& clip : model.Clips)
            clipKeys.push_back(ObjectKey("clip", clip.Name));
        MakeUnique(clipKeys, std::vector<std::string>(clipKeys.size()));
        for (size_t i = 0; i < model.Clips.size(); ++i)
        {
            const auto&   clip = model.Clips[i];
            InspectedClip item {clip.Name, clipKeys[i], clip.Duration, clip.FrameRate, 0};
            for (const auto& track : clip.Tracks)
                item.KeyCount += track.Translation.KeyCount() + track.Rotation.KeyCount() + track.Scale.KeyCount();
            for (const auto& track : clip.MorphTracks)
                item.KeyCount += track.Weight.KeyCount();
            inventory.Clips.push_back(std::move(item));
        }
        return inventory;
    }

    std::variant<ImportInputs, AssetError> ModelImporter::DiscoverInputs(const ImportContext& context) const
    {
        if (!context.SourceFile.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid model import context"};
        auto settings = ParseModelSettings(context.Settings);
        if (const auto* error = std::get_if<AssetError>(&settings))
            return *error;
        auto parsed = ParseModelCpu(context.SourceFile, std::get<ModelSettings>(settings).RecomputeMorphNormals);
        if (const auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        const auto&  model = std::get<ImportedModelData>(parsed);
        ImportInputs inputs;
        inputs.Warnings = model.Warnings;
        std::vector<std::string> slots;
        for (const auto& material : model.Materials)
            slots.push_back(ObjectKey("material", material.Name));
        MakeUnique(slots, std::vector<std::string>(slots.size()));
        for (const auto& [slot, id] : std::get<ModelSettings>(settings).Materials)
        {
            if (std::find(slots.begin(), slots.end(), slot) == slots.end())
                inputs.Warnings.push_back("Unknown material remap slot: " + slot);
            else
                inputs.OptionalProducts.emplace(id, AssetType::Material);
        }
        std::set<std::filesystem::path> conflicted; // referenced with differing settings
        for (const auto& file : model.SourceFiles)
            if (file != context.SourceFile.lexically_normal())
                inputs.SourceFiles.push_back(file);
        if (std::get<ModelSettings>(settings).ImportMaterials)
            for (const auto& material : model.Materials)
                for (const auto& texture : material.Textures)
                {
                    if (!texture.Source.empty() && texture.Source[0] == '*')
                        continue; // Embedded texture; reported by the parser.
                    // A missing texture leaves the material slot empty instead of failing the model.
                    auto resolved = ResolveModelTexture(context.SourceFile, texture.Source);
                    if (const auto* error = std::get_if<AssetError>(&resolved))
                    {
                        inputs.Warnings.push_back(error->Message);
                        continue;
                    }
                    auto path = std::get<std::filesystem::path>(std::move(resolved));
                    // Suggested import settings: non-color textures must not be decoded as
                    // sRGB, and an opaque material's color texture uses its alpha as data,
                    // not coverage. A texture referenced with different needs gets none.
                    using S                                     = MaterialTextureSemantic;
                    const auto                         semantic = static_cast<S>(texture.Semantic);
                    std::map<std::string, std::string> hint;
                    if (semantic == S::Normal || semantic == S::NormalCamera || semantic == S::Height)
                        hint = {{"usage", "normal"}};
                    else if (semantic == S::MetallicRoughness || semantic == S::Metalness ||
                             semantic == S::DiffuseRoughness || semantic == S::AmbientOcclusion ||
                             semantic == S::Unknown || semantic == S::Lightmap)
                        hint = {{"usage", "data"}};
                    else if ((semantic == S::BaseColor || semantic == S::Diffuse) && material.Opaque)
                        hint = {{"alpha_mode", "opaque"}};
                    const bool first = std::find(inputs.ProductSources.begin(), inputs.ProductSources.end(), path) ==
                                       inputs.ProductSources.end();
                    if (first)
                    {
                        if (!hint.empty())
                            inputs.ProductSourceSettings[path] = hint;
                        else
                            conflicted.insert(path);
                    }
                    else if (!conflicted.count(path) && (hint.empty() || inputs.ProductSourceSettings[path] != hint))
                    {
                        inputs.ProductSourceSettings.erase(path);
                        conflicted.insert(path);
                    }
                    if (first)
                    {
                        inputs.OptionalProductSources.insert(path);
                        inputs.ProductSources.push_back(std::move(path));
                    }
                }
        return inputs;
    }

    std::variant<ImportResult, AssetError> ModelImporter::Import(const ImportContext& context) const
    {
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() || !context.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid model import context"};
        auto scale = ParseModelSettings(context.Settings);
        if (const auto* error = std::get_if<AssetError>(&scale))
            return *error;
        std::vector<std::filesystem::path> allowedFiles {context.SourceFile};
        for (const auto& input : context.SourceInputs)
            allowedFiles.push_back(input.first);
        auto parsed = ParseModelCpu(context.SourceFile,
                                    allowedFiles,
                                    std::get<ModelSettings>(scale).RecomputeMorphNormals,
                                    std::get<ModelSettings>(scale).SmoothNormals == SmoothNormalMode::Source);
        if (const auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        auto& source = std::get<ImportedModelData>(parsed);
        if (std::get<ModelSettings>(scale).Scale != 1.0f)
            ScaleModel(source, std::get<ModelSettings>(scale).Scale);
        std::vector<std::string> materialKeys;
        std::vector<std::string> meshKeys;
        for (const auto& material : source.Materials)
            materialKeys.push_back(ObjectKey("material", material.Name));
        MakeUnique(materialKeys, std::vector<std::string>(materialKeys.size()));

        // Hierarchy path of the first node referencing each mesh.
        std::vector<std::string> nodePaths(source.Nodes.size()), meshPaths(source.Meshes.size());
        for (size_t i = 0; i < source.Nodes.size(); ++i)
        {
            const auto& node = source.Nodes[i];
            nodePaths[i] = (node.Parent >= 0 && static_cast<size_t>(node.Parent) < i ? nodePaths[node.Parent] : "") +
                           "/" + node.Name;
            for (uint32_t mesh : node.Meshes)
                if (mesh < meshPaths.size() && meshPaths[mesh].empty())
                    meshPaths[mesh] = nodePaths[i];
        }
        for (const auto& mesh : source.Meshes)
        {
            // Assimp can split one named source object by material. Preserve that
            // distinction without using a traversal index as persistent identity.
            meshKeys.push_back(ObjectKey("mesh", mesh.Name) + ":" + materialKeys.at(mesh.MaterialIndex));
        }
        MakeUnique(meshKeys, meshPaths);
        // An ordered node mesh tuple is a logical geometry object. Repeated tuples
        // are instances of the same object; different tuples retain independent
        // geometry even if they share individual aiMeshes. Never sort the tuple:
        // its order is the previous per-aiMesh submission order.
        std::vector<std::vector<uint32_t>> groups;
        std::vector<size_t>                nodeGroups(source.Nodes.size(), size_t(-1));
        std::vector<std::string>           groupPaths;
        std::vector<bool>                  used(source.Meshes.size(), false);
        for (size_t i = 0; i < source.Nodes.size(); ++i)
        {
            const auto& tuple = source.Nodes[i].Meshes;
            if (tuple.empty())
                continue;
            auto found    = std::find(groups.begin(), groups.end(), tuple);
            nodeGroups[i] = static_cast<size_t>(std::distance(groups.begin(), found));
            if (found == groups.end())
            {
                groups.push_back(tuple);
                groupPaths.push_back(nodePaths[i]);
            }
            for (auto mesh : tuple)
                used.at(mesh) = true;
        }
        // Keep an identity mapping even for source geometry without a node instance.
        for (uint32_t i = 0; i < source.Meshes.size(); ++i)
            if (!used[i])
            {
                groups.push_back({i});
                groupPaths.push_back("");
            }
        std::vector<std::string> groupKeys;
        for (const auto& group : groups)
            groupKeys.push_back(ObjectKey("mesh-object", source.Meshes.at(group.front()).Name));
        MakeUnique(groupKeys, groupPaths);
        std::vector<std::string> keys {"model:root"};
        keys.insert(keys.end(), materialKeys.begin(), materialKeys.end());
        keys.insert(keys.end(), meshKeys.begin(), meshKeys.end());
        keys.insert(keys.end(), groupKeys.begin(), groupKeys.end());
        std::vector<std::string> clipKeys;
        for (const auto& clip : source.Clips)
            clipKeys.push_back(ObjectKey("clip", clip.Name));
        MakeUnique(clipKeys, std::vector<std::string>(clipKeys.size()));
        keys.insert(keys.end(), clipKeys.begin(), clipKeys.end());
        if (source.Skeleton)
            keys.push_back("skeleton:root");
        auto allocated = AllocateLocalIDs(context.PreviousLocalIDs, keys);
        if (const auto* error = std::get_if<AssetError>(&allocated))
            return *error;
        ImportResult result;
        result.LocalIDs = std::get<LocalIDMap>(std::move(allocated));
        result.Warnings = source.Warnings;
        std::vector<StagedProduct> staged;
        const auto idFor = [&](const std::string& key) { return AssetID(context.SourceID, result.LocalIDs.at(key)); };

        for (size_t i = 0; i < source.Materials.size(); ++i)
        {
            if (!std::get<ModelSettings>(scale).ImportMaterials)
                continue;
            const auto&       input = source.Materials[i];
            MaterialData      data {input.Name, input.BaseColor, input.Metallic, input.Roughness, {}};
            data.Render = input.Render;
            ProductDescriptor descriptor {result.LocalIDs.at(materialKeys[i]), AssetType::Material, {}, {}};
            descriptor.StagedFile = std::to_string(descriptor.LocalID) + ".kmaterial";
            for (const auto& texture : input.Textures)
            {
                auto resolved = ResolveModelTexture(context.SourceFile, texture.Source);
                if (std::holds_alternative<AssetError>(resolved))
                    continue; // Reported as a warning by DiscoverInputs.
                const auto& path  = std::get<std::filesystem::path>(resolved);
                const auto  found = context.ProductsBySource.find(path);
                if (found == context.ProductsBySource.end())
                    return AssetError {AssetErrorCode::NotFound,
                                       "Unresolved model texture Product: " + path.u8string()};
                if (!found->second.IsValid())
                    continue; // Optional texture build failure; the coordinator warned.
                data.Textures.push_back({texture.Semantic, texture.UVChannel, found->second});
                AddUnique(descriptor.Dependencies, found->second);
            }
            auto standard = ResolveStandardMaterial(data, input.EmissiveColor);
            if (auto* error = std::get_if<AssetError>(&standard))
                return *error;
            data         = std::get<MaterialData>(std::move(standard));
            auto encoded = EncodeMaterialProduct(data);
            if (const auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            staged.push_back({std::move(descriptor), std::get<std::vector<std::byte>>(std::move(encoded))});
        }

        if (!std::get<ModelSettings>(scale).MorphTargets && source.Skeleton)
            source.Skeleton->ShapeKeys.clear();
        std::set<std::string> warnedRemaps;
        ModelData             model;
        if (source.Skeleton && std::get<ModelSettings>(scale).ImportSkeleton)
        {
            model.Skeleton = idFor("skeleton:root");
            // Node identities use the same disambiguation as the prefab, including duplicate names.
            std::vector<std::string> jointKeys;
            for (const auto& node : source.Nodes)
                jointKeys.push_back(ObjectKey("node", node.Name));
            MakeUnique(jointKeys, nodePaths);
            for (size_t i = 0; i < jointKeys.size(); ++i)
                source.Skeleton->Joints[i].NodeKey = jointKeys[i];
            auto encoded = EncodeSkeletonProduct(*source.Skeleton);
            if (auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            staged.push_back({{model.Skeleton.GetLocalID(),
                               AssetType::Skeleton,
                               std::to_string(model.Skeleton.GetLocalID()) + ".kskeleton",
                               {}},
                              std::get<std::vector<std::byte>>(std::move(encoded))});
        }
        // The runtime names may be remapped; the compression metric still evaluates the
        // original import hierarchy and rest transforms, with those same names applied.
        auto compressionSkeleton = source.Skeleton;
        if (compressionSkeleton)
            for (auto& joint : compressionSkeleton->Joints)
            {
                joint.Name = std::get<ModelSettings>(scale).Mapping.Map(joint.Name);
                joint.Path = std::get<ModelSettings>(scale).Mapping.Map(joint.Path);
            }
        for (size_t i = 0; i < source.Clips.size(); ++i)
        {
            auto& clip = source.Clips[i];
            if (!std::get<ModelSettings>(scale).IncludeClip(clip.Name))
                continue;
            if (!std::get<ModelSettings>(scale).MorphTargets)
                clip.MorphTracks.clear();
            if (clip.Tracks.empty() && clip.MorphTracks.empty())
                continue;
            for (auto& track : clip.Tracks)
                track.Joint = std::get<ModelSettings>(scale).Mapping.Map(track.Joint);
            for (auto& track : clip.MorphTracks)
                track.ShapeKey = std::get<ModelSettings>(scale).Mapping.Map(track.ShapeKey);
            const auto id = idFor(clipKeys[i]);
            model.AnimationClips.push_back(id);
            auto compression = ParseAnimationCompressionSettings(context.Settings, clipKeys[i]);
            if (auto* error = std::get_if<AssetError>(&compression))
                return *error;
            const auto& options = std::get<AnimationCompressionSettings>(compression);
            auto encoded = options.Mode == AnimationCompressionMode::Off ?
                               EncodeAnimationClipProduct(clip) :
                           compressionSkeleton ?
                               EncodeAnimationClipProduct(clip, *compressionSkeleton, options) :
                               AnimationProductBytes(AssetError {AssetErrorCode::InvalidData,
                                                                 "Compressed clip requires its import skeleton"});
            if (auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            staged.push_back(
                {{id.GetLocalID(), AssetType::AnimationClip, std::to_string(id.GetLocalID()) + ".kanim", {}},
                 std::get<std::vector<std::byte>>(std::move(encoded))});
        }
        std::set<size_t> includedGroups;
        for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
        {
            const auto& options = std::get<ModelSettings>(scale);
            if (std::none_of(groups[groupIndex].begin(), groups[groupIndex].end(), [&](auto i) {
                    return options.IncludeMesh(source.Meshes[i].Name);
                }))
                continue;
            includedGroups.insert(groupIndex);
            MeshData          data;
            const auto        meshID = idFor(groupKeys[groupIndex]);
            ProductDescriptor descriptor {meshID.GetLocalID(), AssetType::StaticMesh, {}, {}};
            descriptor.StagedFile = std::to_string(descriptor.LocalID) + ".kmesh";
            bool skinned          = false;
            for (auto index : groups[groupIndex])
                skinned |= options.ImportSkeleton && options.IncludeMesh(source.Meshes[index].Name) &&
                           !source.Meshes[index].Skin.empty();
            if (skinned)
            {
                data.VertexFormat = MeshVertexFormat::Skinned;
                data.Skeleton     = model.Skeleton;
                data.JointBounds.resize(source.Skeleton->Joints.size());
                AddUnique(descriptor.Dependencies, model.Skeleton);
            }
            for (uint32_t inputIndex : groups[groupIndex])
            {
                const auto& input    = source.Meshes.at(inputIndex);
                if (!options.IncludeMesh(input.Name))
                    continue;
                const auto& slotKey  = materialKeys.at(input.MaterialIndex);
                const auto  material = options.ImportMaterials ? idFor(slotKey) : AssetID {};
                AssetID     bound    = material;
                const auto  remap    = std::get<ModelSettings>(scale).Materials.find(slotKey);
                if (remap != std::get<ModelSettings>(scale).Materials.end())
                {
                    const auto bytes = context.ProductBytes.find(remap->second);
                    if (bytes != context.ProductBytes.end())
                    {
                        if (std::holds_alternative<MaterialData>(
                                DecodeMaterialProduct(bytes->second.data(), bytes->second.size())))
                            bound = remap->second;
                        else if (warnedRemaps.insert(remap->first).second)
                            result.Warnings.push_back("Material remap for slot " + remap->first + " (" +
                                                      remap->second.ToString() +
                                                      ") is not a valid Material; using embedded material");
                    }
                }
                auto slot = std::find_if(data.MaterialSlots.begin(), data.MaterialSlots.end(), [&](const auto& item) {
                    return item.ObjectKey == slotKey;
                });
                const auto slotIndex = static_cast<uint32_t>(std::distance(data.MaterialSlots.begin(), slot));
                if (slot == data.MaterialSlots.end())
                    data.MaterialSlots.push_back(
                        {source.Materials.at(input.MaterialIndex).Name, slotKey, bound, material});
                const uint64_t baseVertex = data.Vertices.size(), firstIndex = data.Indices.size();
                if (baseVertex + input.Vertices.size() > UINT32_MAX || firstIndex + input.Indices.size() > UINT32_MAX)
                    return AssetError {AssetErrorCode::InvalidData, "Merged model mesh exceeds index limits"};
                const auto* vertices = &input.Vertices;
                if (skinned)
                {
                    if (data.Vertices.empty())
                        data.SkinBindTransform = input.BindToModel;
                    else if (data.SkinBindTransform != input.BindToModel)
                        return AssetError {AssetErrorCode::InvalidData,
                                           "Merged skin sections disagree about mesh bind space"};
                }
                Bounds bounds;
                for (const auto& vertex : *vertices)
                    bounds.Expand(glm::vec3(vertex.Position[0], vertex.Position[1], vertex.Position[2]));
                if (options.MorphTargets)
                    for (const auto& sourceTarget : input.MorphTargets)
                    {
                        auto found = std::find_if(data.MorphTargets.begin(),
                                                  data.MorphTargets.end(),
                                                  [&](const auto& t) { return t.Name == sourceTarget.Name; });
                        if (found == data.MorphTargets.end())
                        {
                            data.MorphTargets.push_back({sourceTarget.Name, sourceTarget.DefaultWeight, {}, {}});
                            found = data.MorphTargets.end() - 1;
                        }
                        for (auto d : sourceTarget.Deltas)
                        {
                            d.Vertex += uint32_t(baseVertex);
                            found->Deltas.push_back(d);
                        }
                        BuildMorphBounds(*found);
                    }
                if (options.ClothColorSet >= 0)
                {
                    if (size_t(options.ClothColorSet) >= input.ColorSets.size() ||
                        input.ColorSets[options.ClothColorSet].size() != vertices->size())
                        return AssetError {AssetErrorCode::InvalidData,
                                           "Cloth Max Distance colour set is missing on mesh " + input.Name};
                    for (const auto& color : input.ColorSets[options.ClothColorSet])
                    {
                        const float distance = color[options.ClothColorChannel] * options.ClothColorScale;
                        if (!std::isfinite(distance) || distance < 0)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid imported cloth Max Distance"};
                        data.ImportedClothMaxDistance.push_back(distance);
                    }
                }
                data.Vertices.insert(data.Vertices.end(), vertices->begin(), vertices->end());
                if (std::get<ModelSettings>(scale).SmoothNormals == SmoothNormalMode::Source)
                    data.SmoothNormals.insert(
                        data.SmoothNormals.end(), input.SmoothNormals.begin(), input.SmoothNormals.end());
                if (skinned)
                {
                    if (input.Skin.empty())
                    {
                        if (input.Node < 0)
                            return AssetError {AssetErrorCode::InvalidData, "Rigid skin section has no source node"};
                        MeshSkinVertex rigid;
                        rigid.Joints[0]  = uint32_t(input.Node);
                        rigid.Weights[0] = 1;
                        data.Skin.insert(data.Skin.end(), vertices->size(), rigid);
                    }
                    else
                        data.Skin.insert(data.Skin.end(), input.Skin.begin(), input.Skin.end());
                    for (size_t v = 0; v < vertices->size(); ++v)
                    {
                        const auto& p    = (*vertices)[v].Position;
                        const auto& skin = data.Skin[baseVertex + v];
                        for (size_t k = 0; k < 4; ++k)
                            if (skin.Weights[k] > 0)
                                data.JointBounds[skin.Joints[k]].Expand(
                                    glm::vec3(input.BindToModel * glm::vec4(p[0], p[1], p[2], 1)));
                    }
                }
                for (uint32_t index : input.Indices)
                    data.Indices.push_back(static_cast<uint32_t>(baseVertex + index));
                model.SectionIdentities.push_back({meshKeys[inputIndex],
                                                   idFor(meshKeys[inputIndex]),
                                                   meshID,
                                                   static_cast<uint32_t>(data.Sections.size())});
                data.Sections.push_back({static_cast<uint32_t>(firstIndex),
                                         static_cast<uint32_t>(input.Indices.size()),
                                         slotIndex,
                                         bounds});
                data.LocalBounds.Expand(bounds);
                AddUnique(descriptor.Dependencies, bound);
                AddUnique(descriptor.Dependencies, material);
            }
            data.Material         = data.MaterialSlots.front().DefaultMaterial;
            data.AuthoredMaterial = data.MaterialSlots.front().AuthoredMaterial;
            if (std::get<ModelSettings>(scale).SmoothNormals == SmoothNormalMode::Weld ||
                (std::get<ModelSettings>(scale).SmoothNormals == SmoothNormalMode::Source &&
                 data.SmoothNormals.size() != data.Vertices.size()))
                GenerateSmoothNormals(data);
            auto encoded          = EncodeMeshProduct(data);
            if (const auto* error = std::get_if<AssetError>(&encoded))
                return *error;
            staged.push_back({std::move(descriptor), std::get<std::vector<std::byte>>(std::move(encoded))});
        }

        ProductDescriptor descriptor {result.LocalIDs.at("model:root"), AssetType::Model, {}, {}};
        descriptor.StagedFile = std::to_string(descriptor.LocalID) + ".kmodel";
        if (model.Skeleton.IsValid())
            AddUnique(descriptor.Dependencies, model.Skeleton);
        for (auto clip : model.AnimationClips)
            AddUnique(descriptor.Dependencies, clip);
        for (auto index : includedGroups)
            AddUnique(descriptor.Dependencies, idFor(groupKeys[index]));
        std::vector<std::string> nodeKeys;
        for (const auto& node : source.Nodes)
            nodeKeys.push_back(ObjectKey("node", node.Name));
        MakeUnique(nodeKeys, nodePaths);
        for (size_t i = 0; i < source.Nodes.size(); ++i)
        {
            const auto&   input = source.Nodes[i];
            ModelNodeData node {input.Name, input.Parent, input.LocalTransform, {}, nodeKeys[i]};
            if (includedGroups.count(nodeGroups[i]))
            {
                const auto id = idFor(groupKeys.at(nodeGroups[i]));
                node.Meshes.push_back(id);
                AddUnique(descriptor.Dependencies, id);
            }
            model.Nodes.push_back(std::move(node));
        }
        auto encoded = EncodeModelProduct(model);
        if (const auto* error = std::get_if<AssetError>(&encoded))
            return *error;
        staged.push_back({std::move(descriptor), std::get<std::vector<std::byte>>(std::move(encoded))});

        // All parsing, identity resolution and encoding finish before any writes.
        // The coordinator owns this exclusive directory; never overwrite an
        // existing set or write to the source tree through a staging symlink.
        std::error_code error;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_empty(context.StagingDirectory, error) || error)
            return AssetError {AssetErrorCode::InvalidArgument,
                               "Model staging directory must be an empty exclusive directory"};
        std::vector<std::filesystem::path> paths;
        paths.reserve(staged.size());
        result.Products.reserve(staged.size());
        for (const auto& product : staged)
        {
            paths.push_back(context.StagingDirectory / product.Descriptor.StagedFile);
            result.Products.push_back(product.Descriptor);
        }
        StagingCleanup cleanup {paths};
        for (size_t i = 0; i < staged.size(); ++i)
        {
            const auto& product = staged[i];
            const auto& path    = paths[i];
            ++cleanup.Attempted;
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
                return AssetError {AssetErrorCode::IoError, "Cannot open staged model Product: " + path.u8string()};
            output.write(reinterpret_cast<const char*>(product.Bytes.data()),
                         static_cast<std::streamsize>(product.Bytes.size()));
            output.flush();
            output.close();
            if (!output)
                return AssetError {AssetErrorCode::IoError, "Cannot write staged model Product: " + path.u8string()};
        }
        cleanup.Complete = true;
        return result;
    }
} // namespace Kans
