#include "kspch.h"
#include "StaticBatching.h"
#include <cmath>
#include <limits>
#include <tuple>
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "RenderFrame.h"

namespace Kans
{
    bool StaticBatchKey::operator<(const StaticBatchKey& other) const
    {
        return std::tie(Cell, Material, CastShadows, ReceiveShadows) <
               std::tie(other.Cell, other.Material, other.CastShadows, other.ReceiveShadows);
    }

    bool StaticBatchSection::operator==(const StaticBatchSection& other) const
    {
        return Mesh == other.Mesh && Source == other.Source && Section == other.Section && Entity == other.Entity &&
               InstanceRoot == other.InstanceRoot && Transform == other.Transform &&
               WorldBounds.Min == other.WorldBounds.Min && WorldBounds.Max == other.WorldBounds.Max &&
               Material == other.Material && CastShadows == other.CastShadows && ReceiveShadows == other.ReceiveShadows;
    }

    StaticBatchGroups GroupStaticBatchSections(const std::vector<StaticBatchSection>& sections)
    {
        constexpr double  cellSize = 16;
        StaticBatchGroups groups;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const auto& section = sections[i];
            if (!section.Source || !section.WorldBounds.IsValid())
                continue;
            const auto& mesh = section.Source->GetData();
            if (mesh.VertexFormat != MeshVertexFormat::PositionNormalUVColorTangent || !mesh.Skin.empty() ||
                !mesh.MorphTargets.empty() || section.Section >= mesh.Sections.size())
                continue;

            const auto center = (glm::dvec3(section.WorldBounds.Min) + glm::dvec3(section.WorldBounds.Max)) * .5;
            const auto cell   = glm::floor(center / cellSize);
            if (!glm::all(glm::lessThanEqual(glm::abs(cell), glm::dvec3(std::numeric_limits<int32_t>::max()))))
                continue;
            groups[{{int32_t(cell.x), int32_t(cell.y), int32_t(cell.z)},
                    section.Material,
                    section.CastShadows,
                    section.ReceiveShadows}]
                .push_back(i);
        }
        for (auto it = groups.begin(); it != groups.end();)
        {
            auto& members = it->second;
            if (members.size() < 2)
            {
                it = groups.erase(it);
                continue;
            }
            std::stable_sort(members.begin(), members.end(), [&](size_t a, size_t b) {
                const auto& l = sections[a];
                const auto& r = sections[b];
                return std::tie(l.Entity, l.InstanceRoot, l.Mesh, l.Section) <
                       std::tie(r.Entity, r.InstanceRoot, r.Mesh, r.Section);
            });
            ++it;
        }
        return groups;
    }

    MeshData BakeStaticBatch(const std::vector<StaticBatchSection>& sections)
    {
        MeshData           baked;
        constexpr uint32_t unmapped   = std::numeric_limits<uint32_t>::max();
        uint64_t           indexCount = 0;
        for (const auto& member : sections)
        {
            const auto& data = member.Source->GetData();
            const auto& part = data.Sections[member.Section];
            indexCount += part.IndexCount;
            if (indexCount > unmapped || size_t(part.FirstIndex) + part.IndexCount > data.Indices.size() ||
                part.IndexCount % 3 != 0)
                return {};
        }
        baked.Indices.reserve(size_t(indexCount));
        std::vector<uint32_t> remap;
        for (const auto& member : sections)
        {
            const auto&     data = member.Source->GetData();
            const auto&     part = data.Sections[member.Section];
            const glm::mat3 linear(member.Transform);
            const float     determinant = glm::determinant(linear);
            if (!std::isfinite(determinant) || determinant == 0)
                return {};
            const auto  normalMatrix = glm::transpose(glm::inverse(linear));
            const float handedness   = determinant < 0 ? -1.f : 1.f;
            remap.assign(data.Vertices.size(), unmapped);
            for (uint32_t j = 0; j < part.IndexCount; ++j)
            {
                const auto source = data.Indices[size_t(part.FirstIndex) + j];
                if (source >= remap.size())
                    return {};
                auto& index = remap[source];
                if (index == unmapped)
                {
                    auto        vertex   = data.Vertices[source];
                    const auto& p        = vertex.Position;
                    const auto& n        = vertex.Normal;
                    const auto& t        = vertex.Tangent;
                    const auto  position = glm::vec3(member.Transform * glm::vec4(p[0], p[1], p[2], 1));
                    const auto  normal   = normalMatrix * glm::vec3(n[0], n[1], n[2]);
                    const auto  tangent  = linear * glm::vec3(t[0], t[1], t[2]);
                    vertex.Position      = {position.x, position.y, position.z};
                    vertex.Normal        = {normal.x, normal.y, normal.z};
                    vertex.Tangent       = {tangent.x, tangent.y, tangent.z, t[3] * handedness};
                    index                = uint32_t(baked.Vertices.size());
                    baked.Vertices.push_back(vertex);
                    baked.LocalBounds.Expand(position);
                }
                baked.Indices.push_back(index);
            }
        }
        if (!baked.Indices.empty())
        {
            baked.Material = sections.front().Material;
            baked.Sections.push_back({0, uint32_t(baked.Indices.size()), 0, baked.LocalBounds});
        }
        return baked;
    }

    struct StaticBatchCache
    {
        uint64_t World = 0;
        struct Entry
        {
            std::vector<StaticBatchSection> Members;
            MeshKey                         Key;
            std::shared_ptr<const GpuMesh>  Mesh;
        };
        std::map<StaticBatchKey, Entry> Batches;
    };

    namespace
    {
        std::shared_ptr<const GpuMesh> UploadBatch(const MeshData&                                data,
                                                   const std::shared_ptr<RenderHardware::Device>& device)
        {
            using namespace RenderHardware;
            auto mesh                = std::make_shared<GpuMesh>();
            mesh->Device             = device;
            mesh->Material           = data.Material;
            mesh->LocalBounds        = data.LocalBounds;
            mesh->Sections           = data.Sections;
            mesh->VertexCount        = uint32_t(data.Vertices.size());
            mesh->IndexCount         = uint32_t(data.Indices.size());
            const size_t vertexBytes = data.Vertices.size() * sizeof(MeshVertexData);
            const size_t indexBytes  = data.Indices.size() * sizeof(uint32_t);
            mesh->Vertices           = device->CreateBuffer({vertexBytes, BufferUsage::Vertex, MemoryUsage::Upload});
            mesh->Indices            = device->CreateBuffer({indexBytes, BufferUsage::Index, MemoryUsage::Upload});
            mesh->Vertices->Write(0, data.Vertices.data(), vertexBytes);
            mesh->Indices->Write(0, data.Indices.data(), indexBytes);
            return mesh;
        }

        RenderWorld::DrawProxy ShadowDraw(const ViewShaderDraw& draw)
        {
            return {draw.Mesh,
                    draw.Material,
                    draw.Section,
                    draw.Transform,
                    draw.LocalBounds,
                    draw.WorldBounds,
                    draw.EntityID,
                    draw.InstanceRoot,
                    draw.CastShadows,
                    draw.ReceiveShadows,
                    draw.SkinPose,
                    draw.Parameters,
                    draw.RenderCustomDepth,
                    draw.CustomStencil,
                    draw.RenderMobility,
                    draw.SectionCount};
        }
    } // namespace

    StaticBatchStatistics RenderFrame::PrepareStaticBatches(size_t worldDrawCount)
    {
        StaticBatchStatistics stats;
        auto&                 cache = (DisplayFrame ? DisplayFrame->Output : Output).m_StaticBatches;
        if (cache && cache->World != World.GetIdentity())
            cache.reset();
        if (!View.BatchStaticMeshes || !View.ExtractWorld || !View.Highlights.Isolate.empty() ||
            View.DebugMode != SceneViewMode::Lit || !Assets())
            return stats;
        const auto worldEnd = View.Draws.begin() + worldDrawCount;
        if (std::none_of(
                View.Draws.begin(), worldEnd, [](const auto& draw) { return draw.RenderMobility == Mobility::Static; }))
        {
            cache.reset();
            return stats;
        }

        const double                            start = RenderClockMilliseconds();
        std::map<AssetID, bool>                 materials;
        std::map<MeshKey, Ref<const MeshAsset>> meshes;
        std::vector<StaticBatchSection>         candidates;
        std::vector<size_t>                     candidateDraws;
        for (size_t i = 0; i < worldDrawCount; ++i)
        {
            const auto& draw = View.Draws[i];
            if (draw.RenderMobility != Mobility::Static || draw.RenderCustomDepth ||
                draw.Stencil != ModelStencilMode::Disabled || View.MeshResources.count(draw.Mesh) ||
                !World.IsStaticShadowCaster(ShadowDraw(draw)))
                continue;

            auto [material, newMaterial] = materials.try_emplace(draw.Material, false);
            if (newMaterial)
            {
                bool standard = true;
                if (draw.Material.IsValid() && !View.MaterialResources.count(draw.Material))
                {
                    const auto source = Assets()->GetAssetManager().Request<MaterialDataAsset>(draw.Material);
                    standard          = source.IsReady() && IsStandardShader(source.Asset->GetData().Shader);
                }
                const auto* bindings = standard ? GetBindings(draw.Material, false) : nullptr;
                const auto* shader   = bindings ? GetShader(draw.Material, bindings->Shader) : nullptr;
                // Object space and independent sort order are observable for custom and transparent materials.
                material->second = shader && shader->Domain == ShaderDomain::Surface &&
                                   IsStandardShader(bindings->Shader) &&
                                   !IsTransparentMaterial(bindings->Render.AlphaMode);
            }
            if (!material->second)
                continue;

            auto [mesh, newMesh] = meshes.try_emplace(draw.Mesh);
            if (newMesh)
            {
                if (draw.Mesh.Runtime)
                    mesh->second = draw.Mesh.Runtime->Source;
                else if (draw.Mesh.Asset.IsValid())
                {
                    const auto source = Assets()->GetAssetManager().Request<MeshAsset>(draw.Mesh.Asset);
                    if (source.IsReady())
                        mesh->second = source.Asset;
                }
            }
            if (!mesh->second)
                continue;
            candidates.push_back({draw.Mesh,
                                  mesh->second,
                                  draw.Transform,
                                  draw.WorldBounds,
                                  draw.EntityID,
                                  draw.InstanceRoot,
                                  draw.Section,
                                  draw.Material,
                                  draw.CastShadows,
                                  draw.ReceiveShadows});
            candidateDraws.push_back(i);
        }

        const auto                  groups = GroupStaticBatchSections(candidates);
        std::vector<bool>           replaced(worldDrawCount);
        std::vector<ViewShaderDraw> batches;
        if (!cache)
            cache = std::make_shared<StaticBatchCache>();
        cache->World = World.GetIdentity();
        for (auto it = cache->Batches.begin(); it != cache->Batches.end();)
            if (groups.count(it->first))
                ++it;
            else
                it = cache->Batches.erase(it);
        for (const auto& [key, indices] : groups)
        {
            auto&      entry = cache->Batches[key];
            const bool unchanged =
                entry.Members.size() == indices.size() &&
                std::equal(indices.begin(), indices.end(), entry.Members.begin(), [&](size_t index, const auto& old) {
                    return candidates[index] == old;
                });
            if (!unchanged)
            {
                std::vector<StaticBatchSection> members;
                members.reserve(indices.size());
                for (auto index : indices)
                    members.push_back(candidates[index]);

                const auto baked = BakeStaticBatch(members);
                if (baked.Indices.empty())
                    entry = {std::move(members), {}, {}};
                else
                {
                    entry = {std::move(members), MeshKey::MakeStaticBatch(), UploadBatch(baked, GetDevice())};
                    ++stats.Rebuilt;
                }
            }
            if (!entry.Mesh)
                continue;

            auto draw         = View.Draws[candidateDraws[indices.front()]];
            draw.Mesh         = entry.Key;
            draw.Section      = 0;
            draw.SectionCount = uint32_t(indices.size());
            draw.Transform    = glm::mat4(1);
            draw.LocalBounds = draw.WorldBounds = entry.Mesh->LocalBounds;
            draw.EntityID = draw.InstanceRoot = UUID(0);
            View.MeshResources.emplace(entry.Key, entry.Mesh);
            batches.push_back(std::move(draw));
            for (auto index : indices)
                replaced[candidateDraws[index]] = true;
            ++stats.Batches;
            stats.Sections += indices.size();
            stats.BufferBytes += uint64_t(entry.Mesh->VertexCount) * sizeof(MeshVertexData) +
                                 uint64_t(entry.Mesh->IndexCount) * sizeof(uint32_t);
        }

        if (!batches.empty())
        {
            BatchedShadowDraws.emplace();
            auto&  shadowDraws = *BatchedShadowDraws;
            size_t next        = 0;
            for (size_t i = 0; i < View.Draws.size(); ++i)
            {
                if (i < worldDrawCount)
                {
                    if (replaced[i])
                        continue;
                    if (View.Draws[i].CastShadows)
                        shadowDraws.push_back(ShadowDraw(View.Draws[i]));
                }
                if (next != i)
                    View.Draws[next] = std::move(View.Draws[i]);
                ++next;
            }
            View.Draws.resize(next);
            for (auto& draw : batches)
            {
                if (draw.CastShadows)
                    shadowDraws.push_back(ShadowDraw(draw));
                View.Draws.push_back(std::move(draw));
            }
        }
        stats.PrepareMilliseconds = RenderClockMilliseconds() - start;
        return stats;
    }
} // namespace Kans
