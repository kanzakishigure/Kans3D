#include "kspch.h"
#include "ImportedModelData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "FbxSkinWeights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <assimp/cfileio.h>
#include <assimp/cimport.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <yaml-cpp/yaml.h>

namespace Kans
{
    namespace
    {
        // Assimp 6 numbering for aiTextureType_GLTF_METALLIC_ROUGHNESS; absent before Assimp 6.
        constexpr uint32_t AssimpGltfMetallicRoughness = 27;

        // Maps an Assimp texture type to the engine semantic, or 0 to ignore it.
        uint32_t EngineTextureSemantic(uint32_t assimpType)
        {
            if (assimpType >= 1 && assimpType <= static_cast<uint32_t>(MaterialTextureSemantic::Transmission))
                return assimpType;
            if (assimpType == AssimpGltfMetallicRoughness)
                return static_cast<uint32_t>(MaterialTextureSemantic::MetallicRoughness);
            return 0;
        }

        struct ModelFile
        {
            aiFile        Handle {};
            std::ifstream Input;
            const std::vector<char>* Memory   = nullptr;
            size_t        Size     = 0;
            size_t        Position = 0;
        };

        struct ModelIO
        {
            std::vector<std::filesystem::path>        Files;
            std::vector<std::filesystem::path>        MissingMaterials;
            const std::vector<std::filesystem::path>* AllowedFiles = nullptr;
            std::vector<std::filesystem::path>        DeniedFiles;
            bool                                      Failed = false;
            std::filesystem::path                     PreparedPath;
            std::vector<char>                         Prepared;
        };

        ModelFile& File(aiFile* handle) { return *reinterpret_cast<ModelFile*>(handle->UserData); }

        size_t ReadModelFile(aiFile* handle, char* buffer, size_t size, size_t count)
        {
            auto& file = File(handle);
            if (!size || !count || count > size_t(std::numeric_limits<std::streamsize>::max()) / size)
                return 0;
            if (file.Position >= file.Size)
                return 0;
            const auto bytes = std::min(size * count, file.Size - file.Position);
            if (file.Memory)
            {
                std::memcpy(buffer, file.Memory->data() + file.Position, bytes);
                file.Position += bytes;
                return bytes / size;
            }
            file.Input.read(buffer, static_cast<std::streamsize>(bytes));
            const auto read = static_cast<size_t>(file.Input.gcount());
            file.Position += read;
            return read / size;
        }

        aiReturn SeekModelFile(aiFile* handle, size_t offset, aiOrigin origin)
        {
            auto&  file = File(handle);
            size_t next = offset;
            if (origin == aiOrigin_CUR || origin == aiOrigin_END)
            {
                const size_t base = origin == aiOrigin_CUR ? file.Position : file.Size;
                const size_t sign = size_t(1) << (sizeof(size_t) * 8 - 1);
                if (offset & sign)
                {
                    const auto distance = std::numeric_limits<size_t>::max() - offset + 1;
                    if (distance > base)
                        return aiReturn_FAILURE;
                    next = base - distance;
                }
                else
                {
                    if (offset > file.Size - base)
                        return aiReturn_FAILURE;
                    next = base + offset;
                }
            }
            else if (origin != aiOrigin_SET)
                return aiReturn_FAILURE;
            if (next > file.Size || next > size_t(std::numeric_limits<std::streamoff>::max()))
                return aiReturn_FAILURE;
            if (file.Memory)
            {
                file.Position = next;
                return aiReturn_SUCCESS;
            }
            file.Input.clear();
            file.Input.seekg(static_cast<std::streamoff>(next));
            if (!file.Input)
                return aiReturn_FAILURE;
            file.Position = next;
            return aiReturn_SUCCESS;
        }

        aiFile* OpenModelFile(aiFileIO* io, const char* path, const char* mode)
        {
            auto& context = *reinterpret_cast<ModelIO*>(io->UserData);
            try
            {
                if (!path || !mode || mode[0] != 'r' || std::strchr(mode, '+'))
                    return nullptr;
                const auto normalized = std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal();
                if (context.AllowedFiles &&
                    std::find(context.AllowedFiles->begin(), context.AllowedFiles->end(), normalized) ==
                        context.AllowedFiles->end())
                {
                    if (normalized.native().size() > 64 * 1024 || context.DeniedFiles.size() >= 1024)
                    {
                        context.Failed = true;
                        return nullptr;
                    }
                    if (std::find(context.DeniedFiles.begin(), context.DeniedFiles.end(), normalized) ==
                        context.DeniedFiles.end())
                        context.DeniedFiles.push_back(normalized);
                    return nullptr;
                }
                auto file = std::make_unique<ModelFile>();
                file->Input.open(normalized, std::ios::binary | std::ios::ate);
                if (!file->Input)
                {
                    auto extension = normalized.extension().u8string();
                    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    if (extension == ".mtl")
                        context.MissingMaterials.push_back(normalized);
                    return nullptr;
                }
                const auto length = file->Input.tellg();
                if (length < 0 || uint64_t(length) > std::numeric_limits<size_t>::max())
                    return nullptr;
                file->Size = static_cast<size_t>(length);
                file->Input.seekg(0);
                if (!file->Input)
                    return nullptr;
                if (normalized == context.PreparedPath && !context.Prepared.empty())
                {
                    file->Memory = &context.Prepared;
                    file->Size   = context.Prepared.size();
                }
                file->Handle.ReadProc     = ReadModelFile;
                file->Handle.WriteProc    = [](aiFile*, const char*, size_t, size_t) -> size_t { return 0; };
                file->Handle.TellProc     = [](aiFile* f) { return File(f).Position; };
                file->Handle.FileSizeProc = [](aiFile* f) { return File(f).Size; };
                file->Handle.SeekProc     = SeekModelFile;
                file->Handle.FlushProc    = [](aiFile*) {};
                file->Handle.UserData     = reinterpret_cast<char*>(file.get());
                if (std::find(context.Files.begin(), context.Files.end(), normalized) == context.Files.end())
                    context.Files.push_back(normalized);
                auto* handle = &file->Handle;
                file.release();
                return handle;
            }
            catch (const std::exception&)
            {
                // Never unwind a C++ exception through a third-party C callback.
                context.Failed = true;
                return nullptr;
            }
        }
    } // namespace

    // All material sections of a Geometry contribute in original polygon order. Assimp's
    // triangulation order within a polygon is retained by the stable sort.
    static void GenerateSourceNormals(ImportedModelData& model, const std::vector<FbxSourceGeometry>& geometries)
    {
        struct Triangle
        {
            size_t                  Mesh, Index;
            uint32_t                Polygon;
            std::array<uint32_t, 3> Points;
        };
        for (size_t g = 0; g < geometries.size(); ++g)
        {
            const auto&                               polygons = geometries[g].Polygons;
            std::map<uint32_t, std::vector<uint32_t>> incident;
            for (uint32_t p = 0; p < polygons.size(); ++p)
                for (auto cp : polygons[p])
                    incident[cp].push_back(p);
            std::vector<Triangle>          triangles;
            std::vector<size_t>            used(polygons.size(), 0);
            std::map<uint32_t, glm::dvec3> sums;
            for (size_t m = 0; m < model.Meshes.size(); ++m)
            {
                const auto& mesh = model.Meshes[m];
                if (mesh.SourceGeometry != int32_t(g))
                    continue;
                for (size_t i = 0; i < mesh.Indices.size(); i += 3)
                {
                    std::array<uint32_t, 3> cp;
                    for (size_t c = 0; c < 3; ++c)
                        cp[c] = mesh.SourceControlPoints.at(mesh.Indices[i + c]);
                    uint32_t polygon = UINT32_MAX;
                    for (auto p : incident[cp[0]])
                    {
                        const auto& points = polygons[p];
                        if (used[p] < points.size() - 2 &&
                            std::find(points.begin(), points.end(), cp[1]) != points.end() &&
                            std::find(points.begin(), points.end(), cp[2]) != points.end())
                        {
                            polygon = p;
                            ++used[p];
                            break;
                        }
                    }
                    triangles.push_back({m, i, polygon, cp});
                    if (polygon == UINT32_MAX)
                    {
                        // Material splitting can visit later polygons first. If overlapping
                        // polygons share a triangle subset, repair an earlier ambiguous match
                        // with an augmenting path rather than rejecting valid source geometry.
                        std::vector<std::vector<size_t>> owners(polygons.size());
                        for (size_t t = 0; t + 1 < triangles.size(); ++t)
                            owners[triangles[t].Polygon].push_back(t);
                        std::vector<size_t> parent(triangles.size(), size_t(-1));
                        std::vector<size_t> queue {triangles.size() - 1};
                        std::vector<bool>   visited(polygons.size(), false);
                        parent.back() = triangles.size() - 1;
                        bool matched  = false;
                        for (size_t cursor = 0; cursor < queue.size() && !matched; ++cursor)
                        {
                            const auto  current = queue[cursor];
                            const auto& points  = triangles[current].Points;
                            for (auto p : incident[points[0]])
                            {
                                const auto& candidate = polygons[p];
                                if (visited[p] ||
                                    std::find(candidate.begin(), candidate.end(), points[1]) == candidate.end() ||
                                    std::find(candidate.begin(), candidate.end(), points[2]) == candidate.end())
                                    continue;
                                visited[p] = true;
                                if (owners[p].size() < candidate.size() - 2)
                                {
                                    ++used[p];
                                    for (auto t = current;; t = parent[t])
                                    {
                                        const auto previous  = triangles[t].Polygon;
                                        triangles[t].Polygon = p;
                                        if (previous == UINT32_MAX)
                                            break;
                                        p = previous;
                                    }
                                    matched = true;
                                    break;
                                }
                                for (auto t : owners[p])
                                    if (parent[t] == size_t(-1))
                                    {
                                        parent[t] = current;
                                        queue.push_back(t);
                                    }
                            }
                        }
                        if (!matched)
                            throw std::runtime_error("FBX source triangle has no source polygon");
                    }
                }
            }
            std::stable_sort(
                triangles.begin(), triangles.end(), [](const auto& a, const auto& b) { return a.Polygon < b.Polygon; });
            for (const auto& triangle : triangles)
            {
                const auto&               mesh = model.Meshes[triangle.Mesh];
                const auto                i    = triangle.Index;
                std::array<glm::dvec3, 3> p;
                for (size_t c = 0; c < 3; ++c)
                {
                    const auto& v = mesh.Vertices.at(mesh.Indices[i + c]).Position;
                    p[c]          = {v[0], v[1], v[2]};
                }
                const auto   cross  = glm::cross(p[1] - p[0], p[2] - p[0]);
                const double length = glm::length(cross);
                if (!(length > 0))
                    continue;
                const auto normal = cross / length;
                for (size_t c = 0; c < 3; ++c)
                {
                    const auto   a = p[(c + 1) % 3] - p[c], b = p[(c + 2) % 3] - p[c];
                    const double angle  = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
                    const auto   cp     = mesh.SourceControlPoints[mesh.Indices[i + c]];
                    auto [at, inserted] = sums.emplace(cp, glm::dvec3(0));
                    at->second += normal * angle;
                }
            }
            for (auto& mesh : model.Meshes)
                if (mesh.SourceGeometry == int32_t(g))
                {
                    mesh.SmoothNormals.resize(mesh.Vertices.size());
                    for (size_t v = 0; v < mesh.Vertices.size(); ++v)
                    {
                        const auto at  = sums.find(mesh.SourceControlPoints[v]);
                        const auto sum = at == sums.end() ? glm::dvec3(0) : at->second;
                        if (glm::dot(sum, sum) <= 1e-20)
                            mesh.SmoothNormals[v] = mesh.Vertices[v].Normal;
                        else
                        {
                            const auto n          = glm::normalize(sum);
                            mesh.SmoothNormals[v] = {float(n.x), float(n.y), float(n.z)};
                        }
                    }
                }
        }
    }

    static std::variant<ImportedModelData, AssetError>
    ParseModelCpuImpl(const std::filesystem::path&              path,
                      const std::vector<std::filesystem::path>* allowedFiles,
                      std::optional<bool>                       recomputeMorphNormals,
                      bool                                      sourceNormals,
                      bool                                      singleRead = false)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            return AssetError {
                (!error || error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) ?
                    AssetErrorCode::NotFound :
                    AssetErrorCode::IoError,
                "Model source is not a readable regular file: " + path.u8string()};

        std::vector<FbxSourceGeometry> sourceGeometries;
        ModelIO context;
        context.AllowedFiles = allowedFiles;
        const auto normalized    = std::filesystem::absolute(path).lexically_normal();
        auto       extensionText = path.extension().u8string();
        std::transform(extensionText.begin(), extensionText.end(), extensionText.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        if (extensionText == ".fbx" &&
            (!allowedFiles || std::find(allowedFiles->begin(), allowedFiles->end(), normalized) != allowedFiles->end()))
        {
            try
            {
                std::ifstream input(path, std::ios::binary | std::ios::ate);
                const auto    size = input.tellg();
                if (size >= 27)
                {
                    input.seekg(0);
                    char header[27] {};
                    input.read(header, 27);
                    if (std::memcmp(header, "Kaydara FBX Binary  \0\x1a\0", 23) == 0)
                    {
                        if (uint64_t(size) > 512ull * 1024 * 1024)
                            return AssetError {AssetErrorCode::InvalidData, "Binary FBX exceeds input budget"};
                        std::vector<char> bytes(size_t(size), 0);
                        input.seekg(0);
                        input.read(bytes.data(), size);
                        if (!input)
                            return AssetError {AssetErrorCode::IoError, "Cannot read binary FBX"};
                        context.Prepared     = PrepareBinaryFbx(bytes, sourceNormals ? &sourceGeometries : nullptr);
                        context.PreparedPath = normalized;
                    }
                }
            }
            catch (const std::exception& error)
            {
                return AssetError {AssetErrorCode::InvalidData, std::string("Binary FBX: ") + error.what()};
            }
        }
        std::unique_ptr<aiPropertyStore, decltype(&aiReleasePropertyStore)> properties(aiCreatePropertyStore(),
                                                                                       aiReleasePropertyStore);
        if (!context.Prepared.empty())
            aiSetImportPropertyInteger(properties.get(), AI_CONFIG_IMPORT_FBX_READ_ALL_MATERIALS, 1);
        aiFileIO io {OpenModelFile, [](aiFileIO*, aiFile* f) { delete &File(f); }, reinterpret_cast<char*>(&context)};
        // C callbacks keep STL state on our side of the DLL boundary. Serialize
        // import/error retrieval because the C error string is process-global.
        static std::mutex importMutex;
        const aiScene*    rawScene;
        std::string       parseError;
        {
            std::lock_guard<std::mutex> lock(importMutex);
            const unsigned              flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                                aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
            rawScene = aiImportFileExWithProperties(path.u8string().c_str(),
                                                    flags | (singleRead ? 0 : aiProcess_ValidateDataStructure),
                                                    &io,
                                                    properties.get());
            if (!rawScene)
            {
                parseError = aiGetErrorString();
                // Assimp's validator rejects morph-only clips by requiring a node channel. Its C API
                // also double-releases on a failed aiApplyPostProcessing, so do not revalidate that way.
                // Retry only this specific error, require real morph channels, then use the geometry,
                // hierarchy, skeleton and clip validation below (including sparse target indices/weights).
                if (!singleRead && parseError.find("aiAnimation::mNumChannels is 0") != std::string::npos)
                {
                    rawScene = aiImportFileExWithProperties(path.u8string().c_str(), flags, &io, properties.get());
                    bool hasMorphOnly = false, invalid = false;
                    if (rawScene)
                    {
                        for (unsigned i = 0; i < rawScene->mNumAnimations; ++i)
                        {
                            const auto& animation = *rawScene->mAnimations[i];
                            if (!animation.mNumChannels)
                            {
                                hasMorphOnly |= animation.mNumMorphMeshChannels > 0;
                                invalid |= animation.mNumMorphMeshChannels == 0;
                            }
                        }
                        if (!hasMorphOnly || invalid)
                        {
                            aiReleaseImport(rawScene);
                            rawScene = nullptr;
                        }
                    }
                }
            }
        }
        std::unique_ptr<const aiScene, decltype(&aiReleaseImport)> owned(rawScene, aiReleaseImport);
        const auto*                                                scene = owned.get();
        if (context.Failed)
            return AssetError {AssetErrorCode::IoError, "Model source IO callback failed"};
        if (!context.DeniedFiles.empty())
            return AssetError {AssetErrorCode::NotFound,
                               "Model input is outside the declared input set: " +
                                   context.DeniedFiles.front().u8string()};
        if (!context.MissingMaterials.empty())
            return AssetError {AssetErrorCode::NotFound,
                               "Cannot read model material input: " + context.MissingMaterials.front().u8string()};
        if (!scene || !scene->mRootNode || (!scene->mNumMeshes && !scene->mNumAnimations))
            return AssetError {AssetErrorCode::InvalidData, "Model parse failed: " + parseError};
        // Unsupported content is skipped so the static geometry still imports.
        ImportedModelData result;
        result.SourceFiles = context.Files;
        if (scene->HasTextures())
            result.Warnings.push_back(std::to_string(scene->mNumTextures) + " embedded texture(s) were not imported");
        auto extension = path.extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool gltfSource = extension == ".gltf" || extension == ".glb";
        YAML::Node              gltf;
        std::vector<YAML::Node> gltfMeshes, gltfPrimitives;
        std::vector<size_t>     gltfMeshIndices;
        if (gltfSource)
        {
            try
            {
                std::ifstream input(path, std::ios::binary);
                std::string   json;
                if (extension == ".gltf")
                    json.assign(std::istreambuf_iterator<char>(input), {});
                else
                {
                    uint32_t header[5] {};
                    input.read(reinterpret_cast<char*>(header), sizeof(header));
                    if (!input || header[0] != 0x46546c67 || header[4] != 0x4e4f534a || header[3] > 64 * 1024 * 1024)
                        throw std::runtime_error("Invalid glTF JSON chunk");
                    json.resize(header[3]);
                    input.read(json.data(), json.size());
                }
                gltf             = YAML::Load(json);
                size_t meshIndex = 0;
                for (const auto& m : gltf["meshes"])
                {
                    for (const auto& primitive : m["primitives"])
                    {
                        gltfMeshes.push_back(m);
                        gltfPrimitives.push_back(primitive);
                        gltfMeshIndices.push_back(meshIndex);
                    }
                    ++meshIndex;
                }
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
        for (uint32_t i = 0; i < scene->mNumMaterials; ++i)
        {
            const auto*          material = scene->mMaterials[i];
            ImportedMaterialData data;
            data.Name = material->GetName().C_Str();
            aiColor4D color(1, 1, 1, 1);
            if (material->Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
                material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
            data.BaseColor = {color.r, color.g, color.b, color.a};
            aiColor3D emissive(0, 0, 0);
            material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
            data.EmissiveColor = {emissive.r, emissive.g, emissive.b};
            int shading    = 0;
            material->Get(AI_MATKEY_SHADING_MODEL, shading);
            if (shading == aiShadingMode_PBR_BRDF)
            {
                material->Get(AI_MATKEY_METALLIC_FACTOR, data.Metallic);
                material->Get(AI_MATKEY_ROUGHNESS_FACTOR, data.Roughness);
            }
            else
            {
                // Phong-style material: approximate roughness from the specular exponent.
                float shininess = 0;
                if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0)
                    data.Roughness = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.04f, 1.0f);
            }
            float opacity = 1.0f;
            material->Get(AI_MATKEY_OPACITY, opacity);
            aiString   alphaMode; // glTF: OPAQUE, MASK or BLEND
            const bool gltfAlpha = material->Get("$mat.gltf.alphaMode", 0, 0, alphaMode) == AI_SUCCESS;
            if (gltfAlpha)
            {
                const std::string mode = alphaMode.C_Str();
                if (mode == "MASK")
                    data.Render.AlphaMode = MaterialAlphaMode::Mask;
                else if (mode == "BLEND")
                    data.Render.AlphaMode = MaterialAlphaMode::Blend;
                material->Get("$mat.gltf.alphaCutoff", 0, 0, data.Render.AlphaCutoff);
            }
            else
            {
                if (!std::isfinite(opacity) || opacity <= 0.0f)
                {
                    result.Warnings.push_back("material '" + data.Name +
                                              "' has unreliable non-glTF opacity; using opaque alpha 1");
                    opacity = 1.0f;
                }
                else if (opacity < 1.0f)
                    data.Render.AlphaMode = MaterialAlphaMode::Blend;
                // Assimp's legacy diffuse colour does not necessarily carry opacity.
                data.BaseColor[3] = opacity;
            }
            int doubleSided = 0;
            material->Get(AI_MATKEY_TWOSIDED, doubleSided);
            // Legacy character formats have no reliable single-sided flag; preserve their visible inner surfaces.
            data.Render.DoubleSided = !gltfSource || doubleSided != 0;
            data.Opaque             = data.Render.AlphaMode == MaterialAlphaMode::Opaque;
            if (!std::all_of(data.BaseColor.begin(), data.BaseColor.end(), [](float v) { return std::isfinite(v); }) ||
                !std::isfinite(data.Metallic) || !std::isfinite(data.Roughness) || !std::isfinite(opacity) ||
                !IsValidMaterialRenderState(data.Render) ||
                !std::all_of(
                    data.EmissiveColor.begin(), data.EmissiveColor.end(), [](float v) { return std::isfinite(v); }))
                return AssetError {AssetErrorCode::InvalidData, "Model material contains non-finite data"};
            for (uint32_t type = 1; type <= static_cast<uint32_t>(AI_TEXTURE_TYPE_MAX); ++type)
            {
                const uint32_t engineSemantic = EngineTextureSemantic(type);
                if (!engineSemantic)
                    continue;
                const auto semantic = static_cast<aiTextureType>(type);
                for (uint32_t slot = 0; slot < material->GetTextureCount(semantic); ++slot)
                {
                    aiString     source;
                    unsigned int uv = 0;
                    if (material->GetTexture(semantic, slot, &source, nullptr, &uv) != AI_SUCCESS)
                        return AssetError {AssetErrorCode::InvalidData, "Cannot read model texture reference"};
                    data.Textures.push_back({engineSemantic, uv, source.C_Str()});
                }
            }
            result.Materials.push_back(std::move(data));
        }

        for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        {
            const auto* mesh = scene->mMeshes[i];
            const std::string name = mesh->mName.C_Str();
            // The carrier participates in Assimp joining/triangulation, but is consumed only
            // as import identity. It never becomes an authored color or changes UV/tangent data.
            int sourceChannel = -1, sourceGeometry = -1;
            if (sourceNormals && !sourceGeometries.empty())
                for (unsigned channel = 0; channel < mesh->GetNumColorChannels(); ++channel)
                {
                    const auto& encoded = mesh->mColors[channel][0];
                    if (encoded.a == FbxSourceColorMarker && std::isfinite(encoded.b) && encoded.b >= 0 &&
                        encoded.b < sourceGeometries.size() && encoded.b == std::floor(encoded.b) &&
                        !sourceGeometries[size_t(encoded.b)].Polygons.empty())
                    {
                        sourceChannel  = int(channel);
                        sourceGeometry = int(encoded.b);
                    }
                }
            const auto uvChannels    = mesh->GetNumUVChannels();
            const auto colorChannels = mesh->GetNumColorChannels() - (sourceChannel >= 0 ? 1 : 0);
            if (uvChannels > 1 || colorChannels > 1)
                result.Warnings.push_back("mesh '" + name + "' keeps only its first UV and color channel");

            if (!mesh->HasPositions() || !mesh->HasNormals() || mesh->mMaterialIndex >= result.Materials.size())
                return AssetError {AssetErrorCode::InvalidData, "Model mesh has invalid geometry or material"};
            ImportedMeshData data;
            data.SourceGeometry = sourceGeometry;
            data.UVChannels     = uvChannels;
            data.ColorChannels  = colorChannels;
            data.Name          = mesh->mName.C_Str();
            data.MaterialIndex = mesh->mMaterialIndex;
            data.ColorSets.resize(AI_MAX_NUMBER_OF_COLOR_SETS);
            for (unsigned set = 0; set < AI_MAX_NUMBER_OF_COLOR_SETS; ++set)
                if (mesh->HasVertexColors(set) && int(set) != sourceChannel)
                {
                    auto& colors = data.ColorSets[set];
                    colors.reserve(mesh->mNumVertices);
                    for (unsigned v = 0; v < mesh->mNumVertices; ++v)
                    {
                        const auto c = mesh->mColors[set][v];
                        colors.push_back({c.r, c.g, c.b, c.a});
                    }
                }
            data.Vertices.reserve(mesh->mNumVertices);
            for (uint32_t vertex = 0; vertex < mesh->mNumVertices; ++vertex)
            {
                ImportedVertex value;
                const auto&    p = mesh->mVertices[vertex];
                const auto&    n = mesh->mNormals[vertex];
                value.Position   = {p.x, p.y, p.z};
                value.Normal     = {n.x, n.y, n.z};
                if (sourceChannel >= 0)
                {
                    const auto&  encoded = mesh->mColors[sourceChannel][vertex];
                    const double low = encoded.r, high = encoded.g;
                    if (!std::isfinite(low) || !std::isfinite(high) || low < 0 || low > 65535 || high < 0 ||
                        high > 65535 || low != std::floor(low) || high != std::floor(high) ||
                        encoded.b != float(sourceGeometry) || encoded.a != FbxSourceColorMarker)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid FBX source control point encoding"};
                    data.SourceControlPoints.push_back(uint32_t(low) | (uint32_t(high) << 16));
                }
                if (mesh->HasTextureCoords(0))
                    value.UV = {mesh->mTextureCoords[0][vertex].x, mesh->mTextureCoords[0][vertex].y};
                if (mesh->HasVertexColors(0) && sourceChannel != 0)
                {
                    const auto& c = mesh->mColors[0][vertex];
                    value.Color   = {c.r, c.g, c.b, c.a};
                }
                if (mesh->HasTangentsAndBitangents())
                {
                    const auto& t = mesh->mTangents[vertex];
                    const auto& b = mesh->mBitangents[vertex];
                    value.Tangent = {t.x, t.y, t.z, ((n ^ t) * b) < 0 ? -1.0f : 1.0f};
                }
                const auto finite = [](const auto& values) {
                    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
                };
                if (!finite(value.Position) || !finite(value.Normal) || !finite(value.UV) || !finite(value.Color) ||
                    !finite(value.Tangent))
                    return AssetError {AssetErrorCode::InvalidData, "Model vertex contains non-finite data"};
                data.Vertices.push_back(value);
            }
            for (uint32_t face = 0; face < mesh->mNumFaces; ++face)
            {
                const auto& triangle = mesh->mFaces[face];
                if (triangle.mNumIndices != 3)
                    return AssetError {AssetErrorCode::InvalidData, "Model contains non-triangle geometry"};
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    if (triangle.mIndices[corner] >= data.Vertices.size())
                        return AssetError {AssetErrorCode::InvalidData, "Model triangle index is out of range"};
                    data.Indices.push_back(triangle.mIndices[corner]);
                }
            }
            for (uint32_t t = 0; t < mesh->mNumAnimMeshes; ++t)
            {
                const auto& source = *mesh->mAnimMeshes[t];
                if (source.mNumVertices != mesh->mNumVertices)
                    return AssetError {AssetErrorCode::InvalidData, "Morph vertex count mismatch"};
                MeshMorphTarget target;
                target.Name = source.mName.length ? source.mName.C_Str() : "ShapeKey" + std::to_string(t);
                // Assimp FBX joins channel and shape names with a dot; Blender commonly repeats the
                // same name on both. Collapse "Smile.Smile" while preserving authored names such as "Blink.L".
                if (extension == ".fbx" && target.Name.size() % 2 == 1)
                {
                    const auto half = target.Name.size() / 2;
                    if (half && target.Name[half] == '.' &&
                        target.Name.compare(0, half, target.Name, half + 1, half) == 0)
                        target.Name.resize(half);
                }
                // FBX aiAnimMesh::mWeight describes the full blend-shape endpoint (usually 1),
                // rather than an initial expression. glTF authored defaults are restored below.
                target.DefaultWeight = extension == ".fbx" ? 0.f : source.mWeight;
                bool hasNormals      = source.mNormals != nullptr;
                if (gltfSource && i < gltfMeshes.size())
                {
                    const auto m      = gltfMeshes[i];
                    const auto extras = m["extras"];
                    const auto names  = extras ? extras["targetNames"] : YAML::Node();
                    if (names && t < names.size())
                        target.Name = names[t].as<std::string>();
                    const auto weights = m["weights"];
                    if (weights && t < weights.size())
                        target.DefaultWeight = weights[t].as<float>();
                    std::optional<float> nodeDefault;
                    for (const auto& node : gltf["nodes"])
                        if (node["mesh"] && node["mesh"].as<size_t>() == gltfMeshIndices[i])
                        {
                            const auto overrideWeights = node["weights"];
                            if (overrideWeights &&
                                (!overrideWeights.IsSequence() || overrideWeights.size() != mesh->mNumAnimMeshes))
                                return AssetError {AssetErrorCode::InvalidData, "Invalid glTF node morph weights"};
                            const float w = overrideWeights ? overrideWeights[t].as<float>() : target.DefaultWeight;
                            if (nodeDefault && std::abs(*nodeDefault - w) > 1e-6f)
                                return AssetError {AssetErrorCode::InvalidData,
                                                   "Conflicting instance defaults for shared shape key: " +
                                                       target.Name};
                            nodeDefault = w;
                        }
                    if (nodeDefault)
                        target.DefaultWeight = *nodeDefault;
                    const auto targets = gltfPrimitives[i]["targets"];
                    hasNormals         = targets && t < targets.size() && bool(targets[t]["NORMAL"]);
                }
                for (uint32_t v = 0; v < source.mNumVertices; ++v)
                {
                    MorphVertexDelta d;
                    d.Vertex = v;
                    if (source.mVertices)
                    {
                        const auto x = source.mVertices[v] - mesh->mVertices[v];
                        d.Position   = {x.x, x.y, x.z};
                    }
                    if (source.mNormals && hasNormals)
                    {
                        const auto x = source.mNormals[v] - mesh->mNormals[v];
                        d.Normal     = {x.x, x.y, x.z};
                    }
                    if (source.mTangents && mesh->mTangents)
                    {
                        const auto x = source.mTangents[v] - mesh->mTangents[v];
                        d.Tangent    = {x.x, x.y, x.z};
                    }
                    if (d.Position != std::array<float, 3> {} || d.Normal != std::array<float, 3> {} ||
                        d.Tangent != std::array<float, 3> {})
                        target.Deltas.push_back(d);
                }
                const bool unchangedNormals =
                    std::all_of(target.Deltas.begin(), target.Deltas.end(), [](const auto& d) {
                        return d.Normal == std::array<float, 3> {};
                    });
                // FBX/Assimp may fill absent target normals with the base normals, losing presence information.
                if ((!hasNormals || (!gltfSource && unchangedNormals)) && recomputeMorphNormals.value_or(!gltfSource))
                    RecomputeMorphNormals(data.Vertices, data.Indices, target);
                BuildMorphBounds(target);
                result.Warnings.push_back("mesh '" + data.Name + "' morph '" + target.Name +
                                          "': " + std::to_string(target.Deltas.size()) + "/" +
                                          std::to_string(data.Vertices.size()) + " sparse vertices, " +
                                          std::to_string(target.Deltas.size() * 40) + " delta bytes (dense " +
                                          std::to_string(data.Vertices.size() * 36) + ")");
                data.MorphTargets.push_back(std::move(target));
            }
            result.Meshes.push_back(std::move(data));
        }

        // Iterative traversal avoids consuming the call stack for deep hierarchies.
        std::vector<std::pair<const aiNode*, int64_t>> pending {{scene->mRootNode, -1}};
        while (!pending.empty())
        {
            const auto entry = pending.back();
            pending.pop_back();
            ImportedNodeData node;
            node.Name           = entry.first->mName.C_Str();
            node.Parent         = entry.second;
            const auto& m       = entry.first->mTransformation;
            node.LocalTransform = {
                m.a1, m.a2, m.a3, m.a4, m.b1, m.b2, m.b3, m.b4, m.c1, m.c2, m.c3, m.c4, m.d1, m.d2, m.d3, m.d4};
            for (float value : node.LocalTransform)
                if (!std::isfinite(value))
                    return AssetError {AssetErrorCode::InvalidData, "Model node transform is non-finite"};
            for (uint32_t i = 0; i < entry.first->mNumMeshes; ++i)
            {
                const auto mesh = entry.first->mMeshes[i];
                if (mesh >= result.Meshes.size())
                    return AssetError {AssetErrorCode::InvalidData, "Model node mesh reference is out of range"};
                node.Meshes.push_back(mesh);
            }
            const auto index = static_cast<int64_t>(result.Nodes.size());
            result.Nodes.push_back(std::move(node));
            for (uint32_t i = entry.first->mNumChildren; i > 0; --i)
                pending.emplace_back(entry.first->mChildren[i - 1], index);
        }
        bool animated = scene->HasAnimations();
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
            animated |= scene->mMeshes[i]->HasBones() || scene->mMeshes[i]->mNumAnimMeshes > 0;
        if (animated)
        {
            result.Skeleton.emplace();
            auto&                         skeleton = *result.Skeleton;
            for (const auto& m : result.Meshes)
                for (const auto& t : m.MorphTargets)
                {
                    const auto i = skeleton.FindShapeKey(t.Name);
                    if (i < 0)
                        skeleton.ShapeKeys.push_back({t.Name, t.DefaultWeight});
                    else if (std::abs(skeleton.ShapeKeys[i].DefaultWeight - t.DefaultWeight) > 1e-6f)
                        return AssetError {AssetErrorCode::InvalidData,
                                           "Conflicting defaults for shared shape key: " + t.Name};
                }
            std::vector<glm::mat4>        world(result.Nodes.size());
            std::map<std::string, size_t> paths;
            for (size_t i = 0; i < result.Nodes.size(); ++i)
            {
                const auto& node   = result.Nodes[i];
                const auto  matrix = glm::transpose(glm::make_mat4(node.LocalTransform.data()));
                world[i]           = node.Parent < 0 ? matrix : world[node.Parent] * matrix;
                SkeletonJoint joint;
                joint.Name            = node.Name.empty() ? "unnamed" : node.Name;
                joint.Path            = (node.Parent < 0 ? std::string() : skeleton.Joints[node.Parent].Path) + "/" +
                                        std::to_string(joint.Name.size()) + ":" + joint.Name;
                const auto occurrence = paths[joint.Path]++;
                if (occurrence)
                    joint.Path += "#" + std::to_string(occurrence);
                joint.Parent = int32_t(node.Parent);
                try
                {
                    joint.RestLocal = joint.BindLocal = JointTransform::FromMatrix(matrix);
                }
                catch (const std::exception& e)
                {
                    return AssetError {AssetErrorCode::InvalidData, e.what()};
                }
                joint.InverseBind = glm::inverse(world[i]);
                skeleton.Joints.push_back(std::move(joint));
                for (auto mesh : node.Meshes)
                    if (result.Meshes[mesh].Node < 0)
                    {
                        result.Meshes[mesh].Node        = int32_t(i);
                        result.Meshes[mesh].BindToModel = world[i];
                    }
            }
            std::vector<bool> bound(skeleton.Joints.size());
            auto              matrix = [](const aiMatrix4x4& m) {
                const float values[] = {
                    m.a1, m.a2, m.a3, m.a4, m.b1, m.b2, m.b3, m.b4, m.c1, m.c2, m.c3, m.c4, m.d1, m.d2, m.d3, m.d4};
                return glm::transpose(glm::make_mat4(values));
            };
            for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
            {
                const auto* mesh = scene->mMeshes[i];
                auto&       data = result.Meshes[i];
                if (!mesh->HasBones())
                    continue;
                if (data.Node < 0)
                    return AssetError {AssetErrorCode::InvalidData, "Skinned mesh has no source node"};
                const auto meshWorld = world[data.Node];
                data.Skin.resize(data.Vertices.size());
                std::vector<uint32_t> counts(data.Vertices.size());
                for (uint32_t b = 0; b < mesh->mNumBones; ++b)
                {
                    const auto* bone = mesh->mBones[b];
                    const auto  j    = skeleton.FindJoint(bone->mName.C_Str());
                    if (j < 0)
                        return AssetError {AssetErrorCode::InvalidData,
                                           "Missing or ambiguous skin bone: " + std::string(bone->mName.C_Str())};
                    const auto inverseBind = matrix(bone->mOffsetMatrix) * glm::inverse(meshWorld);
                    if (!bound[j])
                    {
                        skeleton.Joints[j].InverseBind = inverseBind;
                        bound[j]                       = true;
                    }
                    else
                    {
                        float difference = 0;
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                difference = std::max(
                                    difference, std::abs(inverseBind[c][r] - skeleton.Joints[j].InverseBind[c][r]));
                        if (difference > 0.01f)
                            return AssetError {AssetErrorCode::InvalidData,
                                               "Meshes have incompatible bind poses for joint: " +
                                                   skeleton.Joints[j].Name};
                    }
                    for (uint32_t w = 0; w < bone->mNumWeights; ++w)
                    {
                        const auto& influence = bone->mWeights[w];
                        if (influence.mVertexId >= data.Skin.size() || !std::isfinite(influence.mWeight) ||
                            influence.mWeight < 0)
                            return AssetError {AssetErrorCode::InvalidData, "Invalid source skin influence"};
                        if (influence.mWeight > 0)
                            ++counts[influence.mVertexId];
                        InsertSkinInfluence(data.Skin[influence.mVertexId], uint32_t(j), influence.mWeight);
                    }
                }
                size_t truncated = 0, unweighted = 0;
                for (size_t v = 0; v < data.Skin.size(); ++v)
                {
                    truncated += counts[v] > 4;
                    if (!NormalizeSkinWeights(data.Skin[v]))
                    {
                        ++unweighted;
                        data.Skin[v].Joints[0]  = uint32_t(data.Node);
                        data.Skin[v].Weights[0] = 1;
                    }
                }
                if (truncated)
                    result.Warnings.push_back("mesh '" + data.Name + "': " + std::to_string(truncated) +
                                              " vertices exceeded 4 influences; strongest 4 retained and renormalized");
                if (unweighted)
                    result.Warnings.push_back("mesh '" + data.Name + "': " + std::to_string(unweighted) +
                                              " unweighted vertices bound to mesh node");
            }
            // Derive bind locals from the canonical inverse binds, independently of source rest locals.
            for (size_t j = 0; j < skeleton.Joints.size(); ++j)
            {
                const auto parent = skeleton.Joints[j].Parent;
                const auto bind   = glm::inverse(skeleton.Joints[j].InverseBind);
                try
                {
                    skeleton.Joints[j].BindLocal =
                        JointTransform::FromMatrix(parent < 0 ? bind : skeleton.Joints[parent].InverseBind * bind);
                }
                catch (const std::exception& e)
                {
                    return AssetError {AssetErrorCode::InvalidData, e.what()};
                }
            }
            for (uint32_t a = 0; a < scene->mNumAnimations; ++a)
            {
                const auto&       animation = *scene->mAnimations[a];
                AnimationClipData clip;
                clip.Name       = animation.mName.length ? animation.mName.C_Str() : "Animation" + std::to_string(a);
                clip.SourceName = path.filename().u8string();
                clip.FrameRate  = animation.mTicksPerSecond > 0 ? animation.mTicksPerSecond : 30;
                clip.Duration   = animation.mDuration / clip.FrameRate;
                for (uint32_t t = 0; t < animation.mNumChannels; ++t)
                {
                    const auto&         channel = *animation.mChannels[t];
                    JointAnimationTrack track;
                    track.Joint = channel.mNodeName.C_Str();
                    for (uint32_t k = 0; k < channel.mNumPositionKeys; ++k)
                    {
                        const auto& v = channel.mPositionKeys[k];
                        track.Translation.Keys.push_back(
                            {v.mTime / clip.FrameRate, {v.mValue.x, v.mValue.y, v.mValue.z}});
                    }
                    for (uint32_t k = 0; k < channel.mNumRotationKeys; ++k)
                    {
                        const auto& v = channel.mRotationKeys[k];
                        track.Rotation.Keys.push_back(
                            {v.mTime / clip.FrameRate,
                             glm::normalize(glm::quat(v.mValue.w, v.mValue.x, v.mValue.y, v.mValue.z))});
                    }
                    for (uint32_t k = 0; k < channel.mNumScalingKeys; ++k)
                    {
                        const auto& v = channel.mScalingKeys[k];
                        track.Scale.Keys.push_back({v.mTime / clip.FrameRate, {v.mValue.x, v.mValue.y, v.mValue.z}});
                    }
                    clip.Tracks.push_back(std::move(track));
                }
                for (uint32_t c = 0; c < animation.mNumMorphMeshChannels; ++c)
                {
                    const auto&             channel     = *animation.mMorphMeshChannels[c];
                    const std::string       channelName = channel.mName.C_Str();
                    const ImportedMeshData* mesh        = nullptr;
                    for (const auto& m : result.Meshes)
                        if (m.Name == channelName || (m.Node >= 0 && result.Nodes[m.Node].Name == channelName))
                        {
                            mesh = &m;
                            break;
                        }
                    if (!mesh)
                    {
                        // Assimp FBX names morph channels "node*primitiveIndex" for split material meshes.
                        auto       nodeName  = channelName;
                        size_t     primitive = 0;
                        const auto star      = channelName.find_last_of('*');
                        if (star != std::string::npos && star + 1 < channelName.size())
                        {
                            char*      end  = nullptr;
                            const auto part = std::strtoul(channelName.c_str() + star + 1, &end, 10);
                            if (end && !*end)
                            {
                                nodeName  = channelName.substr(0, star);
                                primitive = size_t(part);
                            }
                        }
                        for (const auto& node : result.Nodes)
                            if (node.Name == nodeName && primitive < node.Meshes.size())
                            {
                                mesh = &result.Meshes[node.Meshes[primitive]];
                                break;
                            }
                    }
                    if (!mesh)
                        return AssetError {AssetErrorCode::InvalidData,
                                           "Unbound morph animation channel: " + channelName};
                    if (!channel.mNumKeys)
                        return AssetError {AssetErrorCode::InvalidData, "Empty morph animation channel"};
                    for (uint32_t k = 0; k < channel.mNumKeys; ++k)
                        for (uint32_t v = 0; v < channel.mKeys[k].mNumValuesAndWeights; ++v)
                            if (channel.mKeys[k].mValues[v] >= mesh->MorphTargets.size() ||
                                !std::isfinite(channel.mKeys[k].mWeights[v]))
                                return AssetError {AssetErrorCode::InvalidData,
                                                   "Invalid morph animation target or weight"};
                    for (uint32_t t = 0; t < mesh->MorphTargets.size(); ++t)
                    {
                        MorphAnimationTrack track;
                        track.ShapeKey = mesh->MorphTargets[t].Name;
                        for (uint32_t k = 0; k < channel.mNumKeys; ++k)
                        {
                            const auto& key   = channel.mKeys[k];
                            float       value = 0;
                            for (uint32_t v = 0; v < key.mNumValuesAndWeights; ++v)
                                if (key.mValues[v] == t)
                                    value = float(key.mWeights[v]);
                            track.Weight.Keys.push_back({key.mTime / clip.FrameRate, value});
                        }
                        auto found = std::find_if(clip.MorphTracks.begin(), clip.MorphTracks.end(), [&](const auto& v) {
                            return v.ShapeKey == track.ShapeKey;
                        });
                        if (found == clip.MorphTracks.end())
                            clip.MorphTracks.push_back(std::move(track));
                        else
                        {
                            if (found->Weight.KeyCount() != track.Weight.KeyCount())
                                return AssetError {AssetErrorCode::InvalidData, "Conflicting shared morph tracks"};
                            for (size_t k = 0; k < track.Weight.KeyCount(); ++k)
                                if (found->Weight.KeyTime(k) != track.Weight.KeyTime(k) ||
                                    std::abs(found->Weight.KeyValue(k) - track.Weight.KeyValue(k)) > 1e-6f)
                                    return AssetError {AssetErrorCode::InvalidData, "Conflicting shared morph tracks"};
                        }
                    }
                }
                auto valid = ValidateAnimationClip(clip);
                if (auto* e = std::get_if<AssetError>(&valid))
                    return *e;
                result.Clips.push_back(std::move(clip));
            }
        }
        // Assimp's node-key representation drops glTF sampler interpolation. Restore the declared contract.
        if (gltfSource && !result.Clips.empty())
        {
            try
            {
                std::ifstream input(path, std::ios::binary);
                std::string   json;
                if (extension == ".gltf")
                    json.assign(std::istreambuf_iterator<char>(input), {});
                else
                {
                    uint32_t header[5] {};
                    input.read(reinterpret_cast<char*>(header), sizeof(header));
                    if (!input || header[0] != 0x46546c67 || header[4] != 0x4e4f534a || header[3] > 64 * 1024 * 1024)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid glTF JSON chunk"};
                    json.resize(header[3]);
                    input.read(json.data(), json.size());
                }
                const auto document   = YAML::Load(json);
                const auto animations = document["animations"];
                for (size_t a = 0; a < result.Clips.size() && a < animations.size(); ++a)
                {
                    auto&      clip      = result.Clips[a];
                    const auto animation = animations[a];
                    for (const auto& channel : animation["channels"])
                    {
                        const auto node = channel["target"]["node"].as<size_t>();
                        const auto name =
                            document["nodes"][node]["name"].as<std::string>("nodes[" + std::to_string(node) + "]");
                        const auto sampler       = animation["samplers"][channel["sampler"].as<size_t>()];
                        const auto interpolation = sampler["interpolation"].as<std::string>("LINEAR");
                        const auto property      = channel["target"]["path"].as<std::string>();
                        if (property == "weights")
                        {
                            if (interpolation != "LINEAR" && interpolation != "STEP")
                                return AssetError {AssetErrorCode::InvalidData,
                                                   "Unsupported glTF morph interpolation: " + interpolation};
                            for (const auto& targetNode : result.Nodes)
                                if (targetNode.Name == name)
                                    for (auto meshIndex : targetNode.Meshes)
                                        for (const auto& key : result.Meshes[meshIndex].MorphTargets)
                                            for (auto& track : clip.MorphTracks)
                                                if (track.ShapeKey == key.Name)
                                                    track.Weight.Interpolation = interpolation == "STEP" ?
                                                                                     TrackInterpolation::Step :
                                                                                     TrackInterpolation::Linear;
                            continue;
                        }
                        if (interpolation != "LINEAR" && interpolation != "STEP")
                            return AssetError {AssetErrorCode::InvalidData,
                                               "Unsupported glTF animation interpolation: " + interpolation};
                        for (auto& track : clip.Tracks)
                            if (track.Joint == name)
                            {
                                const auto mode =
                                    interpolation == "STEP" ? TrackInterpolation::Step : TrackInterpolation::Linear;
                                if (property == "translation")
                                    track.Translation.Interpolation = mode;
                                else if (property == "scale")
                                    track.Scale.Interpolation = mode;
                                else if (property == "rotation")
                                    track.Rotation.Interpolation =
                                        mode == TrackInterpolation::Step ? mode : TrackInterpolation::Spherical;
                            }
                    }
                    double interval = std::numeric_limits<double>::max();
                    auto   rate     = [&](const auto& track) {
                        for (size_t k = 1; k < track.KeyCount(); ++k)
                            interval = std::min(interval, track.KeyTime(k) - track.KeyTime(k - 1));
                    };
                    for (const auto& track : clip.Tracks)
                    {
                        rate(track.Translation);
                        rate(track.Rotation);
                        rate(track.Scale);
                    }
                    clip.FrameRate = interval > 0 && interval < std::numeric_limits<double>::max() ? 1 / interval : 30;
                }
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData,
                                   "Cannot read glTF animation metadata: " + std::string(e.what())};
            }
        }
        // The engine works in meters. FBX records its unit as centimeters per unit.
        double unit = 0;
        if (scene->mMetaData)
        {
            float single = 0;
            if (!scene->mMetaData->Get("UnitScaleFactor", unit) && scene->mMetaData->Get("UnitScaleFactor", single))
                unit = single;
        }
        if (std::isfinite(unit) && unit > 0 && unit != 100 && !result.Nodes.empty())
            ScaleModel(result, static_cast<float>(unit / 100.0));
        if (sourceNormals)
        {
            if (sourceGeometries.empty() || std::any_of(result.Meshes.begin(),
                                                        result.Meshes.end(),
                                                        [](const auto& mesh) { return mesh.SourceGeometry < 0; }))
                result.Warnings.push_back("mesh.smooth_normals: source identity unavailable; using weld smoothing");
            try
            {
                GenerateSourceNormals(result, sourceGeometries);
            }
            catch (const std::exception& error)
            {
                return AssetError {AssetErrorCode::InvalidData, error.what()};
            }
        }
        return result;
    }

    void ScaleModel(ImportedModelData& model, float scale)
    {
        // Scaling the root node scales the whole hierarchy (row-major: scale rows 0-2).
        for (auto& node : model.Nodes)
            if (node.Parent < 0)
                for (size_t i = 0; i < 12; ++i)
                    node.LocalTransform[i] *= scale;
        if (model.Skeleton)
        {
            const auto inverseScale = glm::scale(glm::mat4(1), glm::vec3(1 / scale));
            for (auto& j : model.Skeleton->Joints)
            {
                j.InverseBind *= inverseScale;
                if (j.Parent < 0)
                {
                    j.RestLocal.Translation *= scale;
                    j.RestLocal.Scale *= scale;
                    j.BindLocal.Translation *= scale;
                    j.BindLocal.Scale *= scale;
                    for (auto& clip : model.Clips)
                        for (auto& track : clip.Tracks)
                            if (track.Joint == j.Name)
                            {
                                for (auto& k : track.Translation.Keys)
                                    k.Value *= scale;
                                for (auto& k : track.Scale.Keys)
                                    k.Value *= scale;
                            }
                }
            }
            for (auto& mesh : model.Meshes)
                mesh.BindToModel = glm::scale(glm::mat4(1), glm::vec3(scale)) * mesh.BindToModel;
        }
    }

    std::variant<ImportedModelData, AssetError>
    ParseModelCpu(const std::filesystem::path& path, std::optional<bool> recomputeMorphNormals, bool sourceNormals)
    {
        return ParseModelCpuImpl(path, nullptr, recomputeMorphNormals, sourceNormals);
    }

    std::variant<ImportedModelData, AssetError> ParseModelCpu(const std::filesystem::path&              path,
                                                              const std::vector<std::filesystem::path>& allowedFiles,
                                                              std::optional<bool> recomputeMorphNormals,
                                                              bool                sourceNormals)
    {
        if (allowedFiles.empty() || allowedFiles.size() > 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid model input set size"};
        std::vector<std::filesystem::path> normalized;
        normalized.reserve(allowedFiles.size());
        for (const auto& file : allowedFiles)
        {
            if (file.empty())
                return AssetError {AssetErrorCode::InvalidArgument, "Empty model input path"};
            normalized.push_back(std::filesystem::absolute(file).lexically_normal());
        }
        return ParseModelCpuImpl(path, &normalized, recomputeMorphNormals, sourceNormals);
    }
    std::variant<ImportedModelData, AssetError> InspectModelCpu(const std::filesystem::path& path)
    {
        return ParseModelCpuImpl(path, nullptr, {}, false, true);
    }
    std::variant<std::filesystem::path, AssetError> ResolveModelTexture(const std::filesystem::path& modelFile,
                                                                        const std::string&           reference)
    {
        namespace fs           = std::filesystem;
        std::string normalized = reference;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (normalized.empty() || normalized.size() > 4096 || normalized.find('\0') != std::string::npos)
            return AssetError {AssetErrorCode::InvalidData, "Invalid model texture reference"};
        const auto      referenced = fs::u8path(normalized);
        const auto      directory  = modelFile.parent_path();
        std::error_code error;
        // Authoring tools often store absolute paths from another machine, so an
        // absolute reference falls back to its file name next to the model.
        std::vector<fs::path> candidates;
        if (!referenced.is_absolute() && !referenced.has_root_name() && !referenced.has_root_directory())
            candidates.push_back((directory / referenced).lexically_normal());
        candidates.push_back((directory / referenced.filename()).lexically_normal());
        for (const auto& candidate : candidates)
            if (fs::is_regular_file(fs::symlink_status(candidate, error)))
                return candidate;

        // Case-insensitive file name search below the model directory (bounded).
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
            });
            return text;
        };
        const auto            wanted = lower(referenced.filename().u8string());
        std::vector<fs::path> matches;
        size_t                visited = 0;
        for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end && visited < 20000;
             it.increment(error), ++visited)
        {
            if (it.depth() > 4)
                it.disable_recursion_pending();
            if (it->is_regular_file(error) && !it->is_symlink(error) &&
                lower(it->path().filename().u8string()) == wanted)
                matches.push_back(it->path().lexically_normal());
        }
        if (matches.empty())
            return AssetError {AssetErrorCode::NotFound, "Model texture not found: " + reference};
        // Deterministic choice: the shallowest, then lexicographically first match.
        std::sort(matches.begin(), matches.end(), [](const fs::path& a, const fs::path& b) {
            const auto da = std::distance(a.begin(), a.end()), db = std::distance(b.begin(), b.end());
            return da != db ? da < db : a < b;
        });
        return matches.front();
    }
} // namespace Kans
