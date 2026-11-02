#include "kspch.h"
#include "RenderAssetCache.h"
#include <chrono>
#include <type_traits>
#include <unordered_set>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Asset/Runtime/MaterialBindings.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/RHI/Backend.h"

namespace Kans
{
    std::variant<GpuMesh, AssetError> UploadMesh(const MeshData& data, std::shared_ptr<RenderHardware::Device> device)
    {
        auto valid = ValidateMeshData(data);
        if (auto* error = std::get_if<AssetError>(&valid))
            return *error;
        if (!device)
        {
            if (RenderHardware::GetBackend() == RenderHardware::Backend::None)
                return AssetError {AssetErrorCode::NotImplemented, "Mesh upload is unavailable for this backend"};
            try
            {
                device = RenderHardware::GetSharedDevice();
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidArgument, e.what()};
            }
        }
        std::vector<MeshVertexData> canonical;
        const auto*                 vertices = &data.Vertices;
        if (!data.Skin.empty())
        {
            canonical               = data.Vertices;
            const auto transform    = data.SkinBindTransform;
            const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
            for (auto& vertex : canonical)
            {
                const auto p = transform * glm::vec4(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1);
                const auto n =
                    glm::normalize(normalMatrix * glm::vec3(vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]));
                const auto t    = glm::normalize(glm::mat3(transform) *
                                                 glm::vec3(vertex.Tangent[0], vertex.Tangent[1], vertex.Tangent[2]));
                vertex.Position = {p.x, p.y, p.z};
                vertex.Normal   = {n.x, n.y, n.z};
                vertex.Tangent  = {
                    t.x, t.y, t.z, vertex.Tangent[3] * (glm::determinant(glm::mat3(transform)) < 0 ? -1.f : 1.f)};
            }
            vertices = &canonical;
        }
        static_assert(sizeof(MeshVertexData) == 64 && std::is_standard_layout_v<MeshVertexData>);
        GpuMesh mesh;
        mesh.Device = device;
        mesh.VertexCount     = uint32_t(data.Vertices.size());
        mesh.HasSkeletonSkin = !data.Skin.empty();
        std::vector<MeshSkinVertex> morphSkin;
        const auto*                 skinStream = &data.Skin;
        if (data.Skin.empty() && !data.MorphTargets.empty())
        {
            morphSkin.resize(data.Vertices.size());
            skinStream = &morphSkin;
        }
        if (!data.MorphTargets.empty())
        {
            // Word 0 points to the debug-normal corner map. Each key has five words:
            // count, indices, positions, optional normals, optional tangents (absolute word offsets).
            // Indices stay integer; stream values are IEEE float bits, avoiding std430 vec3 padding.
            std::vector<uint32_t> sparse(1 + data.MorphTargets.size() * 5, 0);
            const auto            transform    = glm::mat3(data.SkinBindTransform);
            const auto            normalMatrix = glm::transpose(glm::inverse(transform));
            const auto            nonzero      = [](const auto& v) { return v[0] != 0 || v[1] != 0 || v[2] != 0; };
            auto                  append       = [&](glm::vec3 value) {
                for (int c = 0; c < 3; ++c)
                {
                    uint32_t bits;
                    std::memcpy(&bits, &value[c], sizeof(bits));
                    sparse.push_back(bits);
                }
            };
            for (size_t t = 0; t < data.MorphTargets.size(); ++t)
            {
                const auto& target = data.MorphTargets[t];
                mesh.MorphTargets.push_back({target.Name, target.DefaultWeight});
                size_t count   = 0;
                bool   normals = false, tangents = false;
                for (const auto& d : target.Deltas)
                {
                    count += nonzero(d.Position) || nonzero(d.Normal) || nonzero(d.Tangent);
                    normals |= nonzero(d.Normal);
                    tangents |= nonzero(d.Tangent);
                }
                const size_t descriptor = 1 + t * 5;
                sparse[descriptor]      = uint32_t(count);
                sparse[descriptor + 1]  = uint32_t(sparse.size());
                for (const auto& d : target.Deltas)
                    if (nonzero(d.Position) || nonzero(d.Normal) || nonzero(d.Tangent))
                        sparse.push_back(d.Vertex);
                for (int stream = 0; stream < 3; ++stream)
                {
                    if ((stream == 1 && !normals) || (stream == 2 && !tangents))
                        continue;
                    sparse[descriptor + 2 + stream] = uint32_t(sparse.size());
                    for (const auto& d : target.Deltas)
                    {
                        if (!nonzero(d.Position) && !nonzero(d.Normal) && !nonzero(d.Tangent))
                            continue;
                        const auto& raw   = stream == 0 ? d.Position : stream == 1 ? d.Normal : d.Tangent;
                        auto        value = glm::vec3(raw[0], raw[1], raw[2]);
                        if (!data.Skin.empty())
                        {
                            const auto& v = data.Vertices[d.Vertex];
                            if (stream == 0)
                                value = transform * value;
                            else if (stream == 1)
                                value = normalMatrix * value /
                                        std::max(glm::length(normalMatrix *
                                                             glm::vec3(v.Normal[0], v.Normal[1], v.Normal[2])),
                                                 1e-20f);
                            else
                                value = transform * value /
                                        std::max(glm::length(transform *
                                                             glm::vec3(v.Tangent[0], v.Tangent[1], v.Tangent[2])),
                                                 1e-20f);
                        }
                        append(value);
                    }
                }
            }
            // Two debug-normal endpoints share the original indexed corner's source vertex.
            sparse[0] = uint32_t(sparse.size());
            sparse.insert(sparse.end(), data.Indices.begin(), data.Indices.end());
            mesh.MorphDeltas = device->CreateBuffer({sparse.size() * sizeof(uint32_t),
                                                     RenderHardware::BufferUsage::Storage,
                                                     RenderHardware::MemoryUsage::Upload});
            mesh.MorphDeltas->Write(0, sparse.data(), sparse.size() * sizeof(uint32_t));
        }
        mesh.LocalBounds   = data.LocalBounds;
        mesh.Sections      = data.Sections;
        mesh.MaterialSlots = data.MaterialSlots;
        mesh.Vertices      = device->CreateBuffer({data.Vertices.size() * sizeof(MeshVertexData),
                                                   RenderHardware::BufferUsage::Vertex,
                                                   RenderHardware::MemoryUsage::Upload});
        mesh.Vertices->Write(0, vertices->data(), vertices->size() * sizeof(MeshVertexData));
        if (data.HasSmoothNormals())
        {
            auto smooth = data.SmoothNormals;
            if (!data.Skin.empty())
            {
                const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(data.SkinBindTransform)));
                for (auto& n : smooth)
                {
                    const auto transformed = glm::normalize(normalMatrix * glm::vec3(n[0], n[1], n[2]));
                    n                      = {transformed.x, transformed.y, transformed.z};
                }
                mesh.BindSmoothNormals = device->CreateBuffer({smooth.size() * sizeof(smooth[0]),
                                                               RenderHardware::BufferUsage::Vertex,
                                                               RenderHardware::MemoryUsage::Upload});
                mesh.BindSmoothNormals->Write(0, data.SmoothNormals.data(), smooth.size() * sizeof(smooth[0]));
            }
            mesh.SmoothNormals = device->CreateBuffer({smooth.size() * sizeof(smooth[0]),
                                                       RenderHardware::BufferUsage::Vertex,
                                                       RenderHardware::MemoryUsage::Upload});
            mesh.SmoothNormals->Write(0, smooth.data(), smooth.size() * sizeof(smooth[0]));
        }
        mesh.Indices = device->CreateBuffer({data.Indices.size() * sizeof(uint32_t),
                                             RenderHardware::BufferUsage::Index,
                                             RenderHardware::MemoryUsage::Upload});
        mesh.Indices->Write(0, data.Indices.data(), data.Indices.size() * sizeof(uint32_t));
        // Two endpoints per indexed vertex preserve the legacy per-triangle normal lines.
        std::vector<float> lines(data.Indices.size() * 14);
        size_t             cursor = 0;
        for (uint32_t index : data.Indices)
            for (float end : {0.0f, 1.0f})
            {
                const auto& v = (*vertices)[index];
                for (auto value : v.Position)
                    lines[cursor++] = value;
                for (auto value : v.Normal)
                    lines[cursor++] = value;
                lines[cursor++] = end;
            }
        mesh.NormalLines = device->CreateBuffer(
            {lines.size() * sizeof(float), RenderHardware::BufferUsage::Vertex, RenderHardware::MemoryUsage::Upload});
        mesh.NormalLines->Write(0, lines.data(), lines.size() * sizeof(float));
        if (!skinStream->empty())
        {
            mesh.BindVertices = device->CreateBuffer({data.Vertices.size() * sizeof(MeshVertexData),
                                                      RenderHardware::BufferUsage::Vertex,
                                                      RenderHardware::MemoryUsage::Upload});
            mesh.BindVertices->Write(0, data.Vertices.data(), data.Vertices.size() * sizeof(MeshVertexData));
            cursor = 0;
            for (uint32_t index : data.Indices)
                for (float end : {0.0f, 1.0f})
                {
                    const auto& v = data.Vertices[index];
                    for (auto value : v.Position)
                        lines[cursor++] = value;
                    for (auto value : v.Normal)
                        lines[cursor++] = value;
                    lines[cursor++] = end;
                }
            mesh.BindNormalLines = device->CreateBuffer({lines.size() * sizeof(float),
                                                         RenderHardware::BufferUsage::Vertex,
                                                         RenderHardware::MemoryUsage::Upload});
            mesh.BindNormalLines->Write(0, lines.data(), lines.size() * sizeof(float));
            mesh.Skin = device->CreateBuffer({skinStream->size() * sizeof(MeshSkinVertex),
                                              RenderHardware::BufferUsage::Vertex,
                                              RenderHardware::MemoryUsage::Upload});
            mesh.Skin->Write(0, skinStream->data(), skinStream->size() * sizeof(MeshSkinVertex));
            std::vector<MeshSkinVertex> normalSkin;
            normalSkin.reserve(data.Indices.size() * 2);
            for (auto index : data.Indices)
            {
                normalSkin.push_back((*skinStream)[index]);
                normalSkin.push_back((*skinStream)[index]);
            }
            mesh.NormalSkin = device->CreateBuffer({normalSkin.size() * sizeof(MeshSkinVertex),
                                                    RenderHardware::BufferUsage::Vertex,
                                                    RenderHardware::MemoryUsage::Upload});
            mesh.NormalSkin->Write(0, normalSkin.data(), normalSkin.size() * sizeof(MeshSkinVertex));
            mesh.VertexBindings.push_back({1, 32, false});
            mesh.Attributes.push_back({5, 1, 0, RenderHardware::VertexFormat::Uint4});
            mesh.Attributes.push_back({6, 1, 16, RenderHardware::VertexFormat::Float4});
            // Without an animator the scene still supplies the source node transform. Cancel the canonical
            // mesh-to-model upload transform when applying default morphs through the skin variant.
            const auto fallback =
                !data.Skin.empty() && !data.MorphTargets.empty() ? glm::inverse(data.SkinBindTransform) : glm::mat4(1);
            std::vector<glm::mat4> identity(std::max(size_t(1), data.JointBounds.size()), fallback);
            if (!data.MorphTargets.empty())
            {
                const auto joints = identity.size();
                size_t     active = 0;
                for (size_t t = 0; t < mesh.MorphTargets.size(); ++t)
                    if (mesh.MorphTargets[t].DefaultWeight != 0)
                    {
                        glm::mat4 item(0);
                        item[0] = glm::vec4(mesh.MorphTargets[t].DefaultWeight, float(t), 0, 0);
                        identity.push_back(item);
                        ++active;
                    }
                glm::mat4 header(0);
                header[0] =
                    glm::vec4(float(joints), float(active), float(mesh.VertexCount), float(mesh.MorphTargets.size()));
                header[3][3] = -2718;
                identity.push_back(header);
            }
            mesh.BindPalette = device->CreateBuffer({identity.size() * sizeof(glm::mat4),
                                                     RenderHardware::BufferUsage::Storage,
                                                     RenderHardware::MemoryUsage::Upload});
            mesh.BindPalette->Write(0, identity.data(), identity.size() * sizeof(glm::mat4));
        }
        mesh.IndexCount  = static_cast<uint32_t>(data.Indices.size());
        mesh.Material    = data.Material;
        mesh.AuthoredMaterial = data.AuthoredMaterial;
        return mesh;
    }

    RenderHardware::Handle<RenderHardware::Buffer>
    RenderAssetCache::GetSkinningPalette(const Ref<const SkinningPose>& pose, const GpuMesh* mesh)
    {
        if (mesh && !mesh->Skin)
            return {};
        if (!pose || pose->Palette.empty())
        {
            bool active = false;
            if (mesh)
                for (const auto& t : mesh->MorphTargets)
                    active |= t.DefaultWeight != 0;
            return mesh && active ? mesh->BindPalette : RenderHardware::Handle<RenderHardware::Buffer>();
        }
        // Preserve A1 palette sharing for meshes without morph data.
        if (mesh && mesh->MorphTargets.empty())
            mesh = nullptr;
        auto& entry = m_Palettes[{pose.get(), mesh}];
        if (entry.Source.lock() != pose || (mesh && entry.MeshSource.lock() != mesh->Vertices))
        {
            entry        = {};
            entry.Source = pose;
            if (mesh)
                entry.MeshSource = mesh->Vertices;
            if (mesh)
            {
                // Catalog topology is immutable for a prepared instance. Bind names once, never in the frame loop.
                entry.ShapeIndices.assign(mesh->MorphTargets.size(), -1);
                for (size_t t = 0; t < mesh->MorphTargets.size(); ++t)
                    for (size_t k = 0; k < pose->ShapeKeys.size(); ++k)
                        if (pose->ShapeKeys[k].Name == mesh->MorphTargets[t].Name)
                        {
                            entry.ShapeIndices[t] = int32_t(k);
                            break;
                        }
            }
            entry.Pool.reserve(4);
        }
        entry.Frame = m_Frame;
        if (entry.Version == pose->Version && (entry.Current || !entry.Packed.empty()))
            return entry.Current;
        entry.Current.reset();
        entry.Packed = pose->Palette;
        if (mesh && !mesh->MorphTargets.empty())
        {
            const auto joints = entry.Packed.size();
            size_t     active = 0;
            entry.Packed.reserve(joints + mesh->MorphTargets.size() + 1);
            for (size_t t = 0; t < mesh->MorphTargets.size(); ++t)
            {
                float      weight = mesh->MorphTargets[t].DefaultWeight;
                const auto key    = entry.ShapeIndices[t];
                if (key >= 0 && size_t(key) < pose->MorphWeights.size())
                    weight = pose->MorphWeights[key];
                if (weight == 0 || !std::isfinite(weight))
                    continue;
                glm::mat4 item(0);
                item[0] = glm::vec4(weight, float(t), 0, 0);
                entry.Packed.push_back(item);
                ++active;
            }
            if (active == 0 && !mesh->HasSkeletonSkin)
            {
                entry.Version = pose->Version;
                return {};
            }
            glm::mat4 header(0);
            header[0] =
                glm::vec4(float(joints), float(active), float(mesh->VertexCount), float(mesh->MorphTargets.size()));
            header[3][3] = -2718;
            entry.Packed.resize(joints + mesh->MorphTargets.size(), glm::mat4(0));
            entry.Packed.push_back(header);
        }
        // Allocate capacity for all targets once; active counts may change without reallocating.
        const size_t bytes =
            (pose->Palette.size() + (mesh && !mesh->MorphTargets.empty() ? mesh->MorphTargets.size() + 1 : 0)) *
            sizeof(glm::mat4);
        for (const auto& buffer : entry.Pool)
            if (buffer.use_count() == 1 && buffer->GetDesc().Bytes == bytes)
            {
                entry.Current = buffer;
                break;
            }
        if (!entry.Current)
        {
            entry.Current = GetDevice()->CreateBuffer(
                {bytes, RenderHardware::BufferUsage::Storage, RenderHardware::MemoryUsage::Upload});
            entry.Pool.push_back(entry.Current);
        }
        entry.Current->Write(0, entry.Packed.data(), bytes);
        entry.Version = pose->Version;
        return entry.Current;
    }

    std::variant<ModelInstances, AssetError> FlattenModel(const ModelData& data)
    {
        const size_t           count = data.Nodes.size();
        std::vector<glm::mat4> world(count);
        // 0 = unvisited, 1 = in progress, 2 = done.
        std::vector<uint8_t> state(count, 0);
        std::vector<size_t>  chain;
        for (size_t start = 0; start < count; ++start)
        {
            // Walk up to the first resolved ancestor, then resolve back down.
            for (int64_t node = static_cast<int64_t>(start); node >= 0; node = data.Nodes[node].Parent)
            {
                if (static_cast<size_t>(node) >= count || state[node] == 1)
                    return AssetError {AssetErrorCode::InvalidData, "Model node hierarchy is not a tree"};
                if (state[node] == 2)
                    break;
                state[node] = 1;
                chain.push_back(static_cast<size_t>(node));
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                const auto& node = data.Nodes[*it];
                // LocalTransform is row-major; glm matrices are column-major.
                const glm::mat4 local = glm::transpose(glm::make_mat4(node.LocalTransform.data()));
                world[*it]            = node.Parent < 0 ? local : world[node.Parent] * local;
                state[*it]            = 2;
            }
            chain.clear();
        }
        ModelInstances result;
        for (size_t i = 0; i < count; ++i)
            for (AssetID mesh : data.Nodes[i].Meshes)
            {
                bool mapped = false;
                for (const auto& identity : data.SectionIdentities)
                    if (identity.Mesh == mesh)
                    {
                        result.Instances.push_back({mesh, world[i], identity.Section, identity.OldMesh});
                        mapped = true;
                    }
                if (!mapped)
                    result.Instances.push_back({mesh, world[i], 0, {}, true});
            }
        return result;
    }

    GpuMaterial MakeGpuMaterial(const MaterialData& data)
    {
        GpuMaterial material;
        material.Shader = IsStandardShader(data.Shader) || !data.Bindings ? StandardShaderID() : data.Shader;
        return material;
    }

    std::variant<GpuMaterialBindings, AssetError>
    MakeGpuMaterialBindings(const MaterialData&                     material,
                            const ShaderData&                       shader,
                            std::shared_ptr<RenderHardware::Device> device)
    {
        if (!material.Bindings)
            return AssetError {AssetErrorCode::InvalidArgument, "Material has no Shader bindings"};
        auto packed = PackMaterialUniforms(material, shader.Code, *material.Bindings, 64 * 1024 * 1024, shader.Domain);
        if (auto* error = std::get_if<AssetError>(&packed))
            return std::move(*error);
        GpuMaterialBindings bindings;
        bindings.Shader = material.Shader;
        bindings.Render = material.Render;
        bindings.DisabledPasses = material.DisabledPasses;
        for (const auto& block : std::get<std::vector<MaterialUniformBlock>>(packed))
            bindings.Blocks.emplace_back(block.Binding,
                                         GpuUniformBuffer(block.Bytes.size(), block.Bytes.data(), device));
        bindings.Defaults = std::move(std::get<std::vector<MaterialUniformBlock>>(packed));
        const auto reflected =
            MaterialShaderResources(std::get<std::vector<ShaderStageReflection>>(InspectShaderProgram(shader.Code)));
        for (const auto& resource : reflected)
            for (const auto& member : resource.Members)
                if (member.Type == ShaderUniformType::Float && member.Columns == 1 && member.ArrayCount == 1 &&
                    (member.Components >= 1 && member.Components <= 4))
                    for (size_t i = 0; i < bindings.Defaults.size(); ++i)
                        if (bindings.Defaults[i].Binding == resource.Binding)
                            bindings.ParameterSlots.push_back(
                                {member.Name, i, size_t(member.Offset), member.Components});
        for (const auto& texture : material.Bindings->Textures)
            bindings.Textures.emplace_back(texture.Binding, material.Textures[texture.TextureIndex].Texture);
        return bindings;
    }

    const GpuMaterialBindings*
    RenderAssetCache::GetInstanceMaterialBindings(const GpuMaterialBindings&                   base,
                                                  const Ref<const InstanceMaterialParameters>& parameters)
    {
        if (!parameters || base.Blocks.empty())
            return &base;
        bool any = false;
        for (const auto& slot : base.ParameterSlots)
            for (const auto& value : *parameters)
                any |=
                    value.Resolved && (value.RuntimeOverride || slot.Components >= 3) && value.Parameter == slot.Name;
        if (!any)
            return &base;
        const auto baseBuffer = base.Blocks.front().second.Resource();
        auto&      entry      = m_InstanceBindings[{parameters.get(), baseBuffer.get()}];
        if (entry.Source.lock() != parameters || entry.Base.lock() != baseBuffer)
        {
            entry         = {};
            entry.Source  = parameters;
            entry.Base    = baseBuffer;
            entry.Scratch = base.Defaults;
        }
        entry.LastUsedFrame = m_Frame;
        for (size_t i = 0; i < entry.Scratch.size(); ++i)
            std::memcpy(entry.Scratch[i].Bytes.data(), base.Defaults[i].Bytes.data(), base.Defaults[i].Bytes.size());
        for (const auto& slot : base.ParameterSlots)
            for (const auto& value : *parameters)
                if (value.Resolved && (value.RuntimeOverride || slot.Components >= 3) && value.Parameter == slot.Name)
                    std::memcpy(entry.Scratch[slot.Block].Bytes.data() + slot.Offset,
                                value.Value.data(),
                                slot.Components * sizeof(float));
        // Identical bytes are immutable and can be shared by all passes, even while in flight.
        for (const auto& candidate : entry.Pool)
            if (candidate->Defaults.size() == entry.Scratch.size() &&
                std::equal(entry.Scratch.begin(),
                           entry.Scratch.end(),
                           candidate->Defaults.begin(),
                           [](const auto& a, const auto& b) { return a.Bytes == b.Bytes; }))
            {
                candidate->SnapshotFrame = m_Frame;
                return candidate.get();
            }
        GpuMaterialBindings* result = nullptr;
        for (auto& candidate : entry.Pool)
        {
            auto& retired = candidate->RetiredGroups;
            retired.erase(std::remove_if(retired.begin(), retired.end(), [](const auto& g) { return g.expired(); }),
                          retired.end());
            // A descriptor published this frame is never rewritten, including after synchronous
            // command retirement. BeginFrame waits for GPU completion before advancing m_Frame;
            // descriptor ownership also protects commands not submitted at that boundary.
            if (retired.empty() &&
                (!candidate->Group || (candidate->SnapshotFrame != m_Frame && candidate->Group.use_count() == 1)))
            {
                result = candidate.get();
                break;
            }
        }
        if (!result)
        {
            // Keep one unbound staging slot at first use. CPU-only parameter edits can then
            // reuse it without allocating, while the published snapshot remains immutable.
            const size_t count = entry.Pool.empty() ? 2 : 1;
            for (size_t n = 0; n < count; ++n)
            {
                auto instance            = CreateScope<GpuMaterialBindings>();
                instance->Shader         = base.Shader;
                instance->Render         = base.Render;
                instance->DisabledPasses = base.DisabledPasses;
                instance->Textures       = base.Textures;
                instance->Defaults       = base.Defaults;
                for (const auto& block : base.Defaults)
                    instance->Blocks.emplace_back(
                        block.Binding, GpuUniformBuffer(block.Bytes.size(), block.Bytes.data(), GetDevice()));
                if (!result)
                    result = instance.get();
                entry.Pool.push_back(std::move(instance));
            }
        }
        for (size_t i = 0; i < entry.Scratch.size(); ++i)
        {
            const auto& bytes = entry.Scratch[i].Bytes;
            result->Blocks[i].second.Update(bytes.data(), bytes.size());
            std::memcpy(result->Defaults[i].Bytes.data(), bytes.data(), bytes.size());
        }
        result->SnapshotFrame = m_Frame;
        return result;
    }

    template<typename T, typename Resource, typename Build>
    Resource*
    RenderAssetCache::Resolve(std::unordered_map<AssetID, Entry<T, Resource>>& entries, AssetID id, Build&& build)
    {
        if (!id.IsValid())
            return nullptr;
        auto& entry         = entries[id];
        entry.LastUsedFrame = m_Frame;
        auto status         = m_Assets.Request<T>(id);
        if (status.State == AssetLoadState::Ready && status.Asset != entry.Source)
        {
            entry.Source = status.Asset;
            auto built   = build(*status.Asset);
            if (auto* error = std::get_if<AssetError>(&built))
                status.Error = std::move(*error);
            else
            {
                entry.Value = CreateScope<Resource>(std::get<Resource>(std::move(built)));
                entry.ReportedError.reset();
            }
        }
        if (status.Error && (!entry.ReportedError || entry.ReportedError->Message != status.Error->Message))
        {
            CORE_ERROR("Asset {}: {}", id.ToString(), status.Error->Message);
            entry.ReportedError = status.Error;
        }
        return entry.Value.get();
    }

    const ModelInstances* RenderAssetCache::GetModel(AssetID id)
    {
        return Resolve(m_Models, id, [](const ModelAsset& asset) { return FlattenModel(asset.GetMeshData()); });
    }

    const GpuMesh* RenderAssetCache::GetMesh(const MeshKey& id)
    {
        if (id.Stream)
        {
            auto& e = m_Streams[id.Stream.get()];
            if (e.Source.lock() != id.Stream)
            {
                e        = {};
                e.Source = id.Stream;
            }
            if (e.Version == id.Stream->Version)
                return e.Current;
            const auto& d = id.Stream->Data;
            e.Current     = nullptr;
            for (auto& mesh : e.Pool)
                if (mesh->Vertices.use_count() == 1 && (!mesh->SmoothNormals || mesh->SmoothNormals.use_count() == 1) &&
                    mesh->NormalLines.use_count() == 1)
                {
                    e.Current = mesh.get();
                    break;
                }
            if (!e.Current)
            {
                auto built = UploadMesh(d, GetDevice());
                if (auto error = std::get_if<AssetError>(&built))
                {
                    CORE_ERROR("Cloth stream upload: {}", error->Message);
                    return nullptr;
                }
                e.Pool.push_back(CreateScope<GpuMesh>(std::get<GpuMesh>(std::move(built))));
                e.Current = e.Pool.back().get();
            }
            else
            {
                e.Current->Vertices->Write(0, d.Vertices.data(), d.Vertices.size() * sizeof(MeshVertexData));
                if (e.Current->SmoothNormals)
                    e.Current->SmoothNormals->Write(
                        0, d.SmoothNormals.data(), d.SmoothNormals.size() * sizeof(d.SmoothNormals[0]));
                e.Lines.resize(d.Indices.size() * 14);
                size_t cursor = 0;
                for (auto index : d.Indices)
                    for (float end : {0.f, 1.f})
                    {
                        const auto& v = d.Vertices[index];
                        for (float x : v.Position)
                            e.Lines[cursor++] = x;
                        for (float x : v.Normal)
                            e.Lines[cursor++] = x;
                        e.Lines[cursor++] = end;
                    }
                e.Current->NormalLines->Write(0, e.Lines.data(), e.Lines.size() * sizeof(float));
            }
            e.Current->LocalBounds = d.LocalBounds;
            e.Current->Sections.resize(id.Stream->Section + 1);
            e.Current->Sections[id.Stream->Section] = d.Sections[0];
            e.Version                               = id.Stream->Version;
            return e.Current;
        }
        if (!id.Runtime)
            return Resolve(
                m_Meshes, id.Asset, [&](const MeshAsset& asset) { return UploadMesh(asset.GetData(), GetDevice()); });
        if (id.Runtime->Source->GetData().Indices.empty())
            return nullptr;
        auto& entry = m_EditableMeshes[id.Runtime->Identity];
        if (!entry.Value && !entry.ReportedError)
        {
            entry.Source     = id.Runtime;
            const auto start = std::chrono::steady_clock::now();
            try
            {
                auto built = UploadMesh(id.Runtime->Source->GetData(), GetDevice());
                if (auto* error = std::get_if<AssetError>(&built))
                    entry.ReportedError = *error;
                else
                {
                    entry.Value = CreateScope<GpuMesh>(std::get<GpuMesh>(std::move(built)));
                    ++m_EditableStats.Uploads;
                    m_EditableStats.LastUploadMilliseconds =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                }
            }
            catch (const std::exception& error)
            {
                entry.ReportedError = AssetError {AssetErrorCode::InvalidData, error.what()};
            }
            if (entry.ReportedError)
                CORE_ERROR("Runtime mesh {}: {}", id.Runtime->Identity, entry.ReportedError->Message);
        }
        return entry.Value.get();
    }

    void RenderAssetCache::WarnMaterialDomain(AssetID id, bool postList)
    {
        auto& warned = postList ? m_WarnedPost : m_WarnedMesh;
        if (warned.insert(id).second)
            CORE_WARN("Material {} ignored: incompatible shader domain for {}",
                      id.ToString(),
                      postList ? "post-process list" : "mesh");
    }
    const GpuMaterial* RenderAssetCache::GetMaterial(AssetID id)
    {
        return Resolve(m_Materials, id, [](const MaterialDataAsset& asset) {
            return std::variant<GpuMaterial, AssetError>(MakeGpuMaterial(asset.GetData()));
        });
    }

    std::shared_ptr<RenderHardware::Device> RenderAssetCache::GetDevice()
    {
        if (!m_Device)
            m_Device = RenderHardware::GetSharedDevice();
        return m_Device;
    }
    void RenderAssetCache::SetTextureSampler(AssetID id, const RenderHardware::SamplerDesc& desc)
    {
        m_TextureSamplers[id] = GetDevice()->CreateSampler(desc);
        for (auto* entries : {&m_Bindings, &m_StandardBindings})
            for (auto& [key, entry] : *entries)
                if (entry.Value)
                    entry.Value->Group.reset();
    }
    Ref<Texture2D> RenderAssetCache::GetTexture(AssetID id)
    {
        auto* texture = Resolve(m_Textures, id, [&](const TextureAsset& asset) {
            auto uploaded = Texture2D::CreateFromProduct(asset.GetData(), GetDevice());
            if (std::holds_alternative<Ref<Texture2D>>(uploaded))
            {
                // Drop obsolete cached descriptors. Recorded commands still retain their snapshots.
                for (auto* entries : {&m_Bindings, &m_StandardBindings})
                    for (auto& [key, entry] : *entries)
                        if (entry.Value)
                        {
                            entry.Value->Group.reset();
                            entry.Value->Images.clear();
                        }
            }
            return uploaded;
        });
        return texture ? *texture : nullptr;
    }
    RenderHardware::Handle<RenderHardware::Texture> RenderAssetCache::GetTextureResource(AssetID id)
    {
        auto view = GetTexture(id);
        return view ? view->GetResource() : nullptr;
    }

    const GpuShader* RenderAssetCache::GetShader(AssetID id)
    {
        if (IsStandardShader(id) || id == SixWaySmokeShaderID())
        {
            const bool standard = IsStandardShader(id);
            auto&      entry    = m_Shaders[standard ? StandardShaderID() : id];
            entry.LastUsedFrame = m_Frame;
            if (!entry.Source)
                entry.Source = standard ? GetStandardShaderAsset() : GetSixWaySmokeShaderAsset();
            if (!entry.Value && !entry.ReportedError)
            {
                auto built = GpuShader::Create(entry.Source->GetData(), GetDevice());
                if (auto* error = std::get_if<AssetError>(&built))
                {
                    entry.ReportedError = *error;
                    CORE_ERROR("{} Shader: {}", standard ? "Standard" : "Kans3D/SixWaySmoke", error->Message);
                }
                else
                    entry.Value = CreateScope<GpuShader>(std::get<GpuShader>(std::move(built)));
            }
            return entry.Value.get();
        }
        const auto  status = m_Assets.Request<ShaderDataAsset>(id);
        const auto* shader = Resolve(m_Shaders, id, [&](const ShaderDataAsset& asset) {
            return GpuShader::Create(asset.GetData(), GetDevice());
        });
        // Shader failures must not expose an older program with a newer CPU layout.
        return status.IsReady() && !m_Shaders.at(id).ReportedError ? shader : nullptr;
    }

    Bounds RenderAssetCache::GetModelBounds(AssetID model)
    {
        Bounds bounds;
        if (const auto* instances = GetModel(model))
            for (const auto& instance : instances->Instances)
                if (const auto* mesh = GetMesh(instance.Mesh))
                {
                    // Preserve the former per-part bounds used for drop placement
                    // and camera focus. Transforming the union AABB first adds
                    // corners that belong to no section, even for tiny rotations.
                    const size_t first = instance.AllSections ? 0 : instance.Section;
                    const size_t end   = instance.AllSections ? mesh->Sections.size() : first + 1;
                    for (size_t i = first; i < end && i < mesh->Sections.size(); ++i)
                        bounds.Expand(mesh->Sections[i].LocalBounds.Transformed(instance.Transform));
                }
        return bounds;
    }

    const GpuMaterialBindings* RenderAssetCache::GetMaterialBindings(AssetID materialID, bool standard)
    {
        const auto*                  material = GetMaterial(materialID);
        Ref<const MaterialDataAsset> materialSource;
        if (material)
            materialSource = m_Materials.at(materialID).Source;
        if (material && !standard && !IsStandardShader(material->Shader) && GetShader(material->Shader))
            if (const auto* bindings =
                    ResolveBindings(materialID, materialSource, m_Shaders.at(material->Shader).Source, false))
                return bindings;
        if (!GetShader(StandardShaderID()))
            return nullptr;
        return ResolveBindings(materialID, materialSource, GetStandardShaderAsset(), true);
    }

    const GpuMaterialBindings* RenderAssetCache::ResolveBindings(AssetID                      materialID,
                                                                 Ref<const MaterialDataAsset> materialSource,
                                                                 Ref<const ShaderDataAsset>   shaderSource,
                                                                 bool                         standard)
    {
        auto& entry         = (standard ? m_StandardBindings : m_Bindings)[materialID];
        entry.LastUsedFrame = m_Frame;
        // A null Value with unchanged sources is a cached packing failure. The
        // separate Standard entry cannot erase that failure or its source identity.
        if (entry.Material != materialSource || entry.Shader != shaderSource)
        {
            entry.Material = materialSource;
            entry.Shader   = shaderSource;
            entry.Value.reset();
            MaterialData data = materialSource ? materialSource->GetData() : MaterialData {};
            if (standard)
            {
                // A custom recipe addresses another layout. Use the default Standard
                // material in debug views and while that Shader is unavailable.
                if (!IsStandardShader(data.Shader) && data.Bindings)
                {
                    const auto render = data.Render;
                    data = MaterialData {};
                    data.Render       = render;
                }
                auto resolved = ResolveStandardMaterial(data);
                if (auto* error = std::get_if<AssetError>(&resolved))
                {
                    CORE_ERROR("Material {}: {}", materialID.ToString(), error->Message);
                    return nullptr;
                }
                data = std::get<MaterialData>(std::move(resolved));
            }
            auto built = MakeGpuMaterialBindings(data, shaderSource->GetData(), GetDevice());
            if (auto* error = std::get_if<AssetError>(&built))
            {
                CORE_ERROR("Material {}: {}", materialID.ToString(), error->Message);
            }
            else
                entry.Value = CreateScope<GpuMaterialBindings>(std::get<GpuMaterialBindings>(std::move(built)));
        }
        return entry.Value.get();
    }

    RenderHardware::Handle<RenderHardware::BindGroup>
    RenderAssetCache::GetMaterialGroup(const GpuShader& shader, const GpuMaterialBindings& bindings, bool* result)
    {
        bool complete = true;
        RenderHardware::BindGroupDesc group;
        group.Layout = shader.MaterialLayout;
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> images;
        if (!m_MaterialSampler)
            m_MaterialSampler = GetDevice()->CreateSampler();
        for (const auto& [binding, buffer] : bindings.Blocks)
            group.Entries.push_back({binding, buffer.Resource()});
        for (const auto& [binding, unit] : shader.MaterialSamplerFallbacks())
        {
            Ref<Texture2D> texture;
            auto           sampler = m_MaterialSampler;
            for (const auto& [textureBinding, id] : bindings.Textures)
                if (textureBinding == binding && id.IsValid())
                {
                    const auto status = m_Assets.Request<TextureAsset>(id);
                    texture = GetTexture(id);
                    if (texture)
                    {
                        const auto& desc   = texture->GetSampling();
                        auto&       cached = m_ProductSamplers[{desc.Min, desc.U}];
                        if (!cached)
                            cached = GetDevice()->CreateSampler(desc);
                        sampler = cached;
                    }
                    if (auto found = m_TextureSamplers.find(id); found != m_TextureSamplers.end())
                        sampler = found->second;
                    const auto& entry = m_Textures.at(id);
                    // A failed replacement upload may retain the previous GPU texture.
                    complete &=
                        status.IsReady() && bool(texture) && !entry.ReportedError && entry.Source == status.Asset;
                }
            if (!texture)
            {
                const auto role     = shader.MaterialSamplerFallbacks().at(binding);
                auto&      fallback = m_FallbackTextures[size_t(role)];
                if (!fallback)
                {
                    // Float data represents the flat normal exactly (0.5, 0.5, 1).
                    const std::array<float, 4> pixel =
                        role == ShaderTextureFallback::Normal ? std::array<float, 4> {0.5f, 0.5f, 1, 1} :
                        role == ShaderTextureFallback::Black  ? std::array<float, 4> {0, 0, 0, 1} :
                                                                std::array<float, 4> {1, 1, 1, 1};
                    TextureData image;
                    image.Width = image.Height = 1;
                    image.Format               = TexturePixelFormat::RGBA32Float;
                    image.Usage                = TextureUsage::Data;
                    image.Mips.push_back({std::vector<std::byte>(sizeof(pixel))});
                    std::memcpy(image.Mips[0].Pixels.data(), pixel.data(), sizeof(pixel));
                    auto uploaded = Texture2D::CreateFromProduct(image, GetDevice());
                    if (auto* error = std::get_if<AssetError>(&uploaded))
                    {
                        CORE_ERROR("Material fallback texture: {}", error->Message);
                        complete = false;
                        continue;
                    }
                    fallback = std::get<Ref<Texture2D>>(std::move(uploaded));
                }
                texture = fallback;
            }
            auto image = texture->GetResource();
            images.push_back(image);
            RenderHardware::BindGroupEntry entry;
            entry.Slot            = binding;
            entry.TextureResource = image;
            entry.SamplerResource = sampler;
            group.Entries.push_back(std::move(entry));
        }
        if (result)
            *result = complete;
        if (group.Entries.size() != group.Layout->GetDesc().Entries.size())
        {
            bindings.Images = std::move(images);
            return nullptr; // Compatibility texture-only binding has no uniform blocks.
        }
        if (!bindings.Group || bindings.Images != images)
        {
            auto& retired = bindings.RetiredGroups;
            retired.erase(std::remove_if(retired.begin(), retired.end(), [](const auto& g) { return g.expired(); }),
                          retired.end());
            if (bindings.Group && bindings.Group.use_count() > 1)
                retired.push_back(bindings.Group);
            bindings.Group  = GetDevice()->CreateBindGroup(group);
            bindings.Images = std::move(images);
        }
        return bindings.Group;
    }

    void RenderAssetCache::BeginFrame(uint64_t unusedFrames)
    {
        for (auto it = m_InstanceBindings.begin(); it != m_InstanceBindings.end();)
            if (it->second.Source.expired() || it->second.Base.expired() ||
                m_Frame - it->second.LastUsedFrame > unusedFrames)
                it = m_InstanceBindings.erase(it);
            else
                ++it;

        if (m_Device)
            m_Device->WaitIdle();
        ++m_Frame;
        for (auto it = m_Palettes.begin(); it != m_Palettes.end();)
            if (it->second.Source.expired() || m_Frame - it->second.Frame > unusedFrames)
                it = m_Palettes.erase(it);
            else
                ++it;
        for (auto it = m_Streams.begin(); it != m_Streams.end();)
            if (it->second.Source.expired())
                it = m_Streams.erase(it);
            else
                ++it;
        for (auto it = m_EditableMeshes.begin(); it != m_EditableMeshes.end();)
            if (it->second.Source.expired())
            {
                it = m_EditableMeshes.erase(it);
                ++m_EditableStats.Releases;
            }
            else
                ++it;
        auto evict = [&](auto& entries) {
            for (auto it = entries.begin(); it != entries.end();)
                it = m_Frame - it->second.LastUsedFrame > unusedFrames ? entries.erase(it) : std::next(it);
        };
        evict(m_Models);
        evict(m_Meshes);
        evict(m_Materials);
        evict(m_Textures);
        evict(m_Shaders);
        evict(m_Bindings);
        evict(m_StandardBindings);
    }

    void RenderAssetCache::Clear()
    {
        m_Palettes.clear();
        m_InstanceBindings.clear();

        m_Models.clear();
        m_Meshes.clear();
        m_EditableStats.Releases += m_EditableMeshes.size();
        m_EditableMeshes.clear();
        m_Streams.clear();
        m_Materials.clear();
        m_Textures.clear();
        m_Shaders.clear();
        m_Bindings.clear();
        m_StandardBindings.clear();
        m_FallbackTextures = {};
        m_MaterialSampler.reset();
        m_TextureSamplers.clear();
        m_ProductSamplers.clear();
        m_ShadowFallback.reset();
    }
} // namespace Kans

#include "ShadowMaps.h"
namespace Kans
{
    std::shared_ptr<ShadowResources> RenderAssetCache::GetShadowFallback()
    {
        if (!m_ShadowFallback)
            m_ShadowFallback = CreateShadowFallback(*GetDevice());
        return m_ShadowFallback;
    }
} // namespace Kans
