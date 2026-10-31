#include "kspch.h"
#include "MeshData.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace Kans
{
    void GenerateSmoothNormals(MeshData& mesh)
    {
        // Bit keys preserve the imported representation (including signed zero). Map lookup never
        // determines summation order: triangles and corners are accumulated in source index order.
        std::map<std::array<uint32_t, 3>, size_t> groups;
        std::vector<size_t>                       membership;
        std::vector<glm::dvec3>                   sums;
        for (const auto& vertex : mesh.Vertices)
        {
            std::array<uint32_t, 3> key;
            std::memcpy(key.data(), vertex.Position.data(), sizeof(key));
            const auto [at, inserted] = groups.emplace(key, sums.size());
            if (inserted)
                sums.emplace_back(0);
            membership.push_back(at->second);
        }
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
        {
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
                const double angle = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
                sums[membership[mesh.Indices[i + c]]] += normal * angle;
            }
        }
        mesh.SmoothNormals.resize(mesh.Vertices.size());
        for (size_t v = 0; v < mesh.Vertices.size(); ++v)
        {
            const auto sum = sums[membership[v]];
            if (glm::dot(sum, sum) <= 1e-20)
                mesh.SmoothNormals[v] = mesh.Vertices[v].Normal;
            else
            {
                const auto n          = glm::normalize(sum);
                mesh.SmoothNormals[v] = {float(n.x), float(n.y), float(n.z)};
            }
        }
    }
    void BuildMorphBounds(MeshMorphTarget& target)
    {
        target.PositionBounds = {};
        target.PositionBounds.Expand(glm::vec3(0));
        for (const auto& d : target.Deltas)
            target.PositionBounds.Expand(glm::vec3(d.Position[0], d.Position[1], d.Position[2]));
    }
    Bounds MorphedBounds(const MeshData& mesh, const std::vector<float>& weights)
    {
        Bounds result = mesh.LocalBounds;
        for (size_t t = 0; t < mesh.MorphTargets.size() && t < weights.size(); ++t)
        {
            const auto& b      = mesh.MorphTargets[t].PositionBounds;
            const auto  extent = glm::max(glm::abs(b.Min), glm::abs(b.Max)) * std::abs(weights[t]);
            result.Min -= extent;
            result.Max += extent;
        }
        return result;
    }
    void RecomputeMorphNormals(const std::vector<MeshVertexData>& vertices,
                               const std::vector<uint32_t>&       indices,
                               MeshMorphTarget&                   target)
    {
        std::vector<glm::vec3> base(vertices.size()), deformed(vertices.size()), a(vertices.size()), b(vertices.size());
        for (size_t v = 0; v < vertices.size(); ++v)
            base[v] = deformed[v] =
                glm::vec3(vertices[v].Position[0], vertices[v].Position[1], vertices[v].Position[2]);
        for (const auto& d : target.Deltas)
            deformed.at(d.Vertex) += glm::vec3(d.Position[0], d.Position[1], d.Position[2]);
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const auto x = indices[i], y = indices[i + 1], z = indices[i + 2];
            const auto n = glm::cross(base[y] - base[x], base[z] - base[x]);
            const auto m = glm::cross(deformed[y] - deformed[x], deformed[z] - deformed[x]);
            for (auto v : {x, y, z})
            {
                a[v] += n;
                b[v] += m;
            }
        }
        std::vector<MorphVertexDelta> output;
        size_t                        cursor = 0;
        for (uint32_t v = 0; v < vertices.size(); ++v)
        {
            MorphVertexDelta d;
            d.Vertex = v;
            if (cursor < target.Deltas.size() && target.Deltas[cursor].Vertex == v)
                d = target.Deltas[cursor++];
            if (glm::dot(a[v], a[v]) > 1e-20f && glm::dot(b[v], b[v]) > 1e-20f)
            {
                const auto delta = glm::normalize(b[v]) - glm::normalize(a[v]);
                d.Normal         = {delta.x, delta.y, delta.z};
            }
            if (d.Position != std::array<float, 3> {} || d.Normal != std::array<float, 3> {} ||
                d.Tangent != std::array<float, 3> {})
                output.push_back(d);
        }
        target.Deltas = std::move(output);
    }
    void InsertSkinInfluence(MeshSkinVertex& vertex, uint32_t joint, float weight)
    {
        if (!std::isfinite(weight) || weight <= 0)
            return;
        for (size_t i = 0; i < 4; ++i)
            if (weight > vertex.Weights[i])
            {
                for (size_t k = 3; k > i; --k)
                {
                    vertex.Joints[k]  = vertex.Joints[k - 1];
                    vertex.Weights[k] = vertex.Weights[k - 1];
                }
                vertex.Joints[i]  = joint;
                vertex.Weights[i] = weight;
                break;
            }
    }
    bool NormalizeSkinWeights(MeshSkinVertex& vertex)
    {
        double sum = 0;
        for (float w : vertex.Weights)
            sum += w;
        if (!(sum > 0) || !std::isfinite(sum))
            return false;
        for (float& w : vertex.Weights)
            w = float(w / sum);
        return true;
    }
    void BuildSingleSection(MeshData& mesh)
    {
        mesh.LocalBounds = {};
        for (const auto& vertex : mesh.Vertices)
            mesh.LocalBounds.Expand(glm::vec3(vertex.Position[0], vertex.Position[1], vertex.Position[2]));
        mesh.Sections      = {{0, static_cast<uint32_t>(mesh.Indices.size()), 0, mesh.LocalBounds}};
        mesh.MaterialSlots = {{"", "", mesh.Material, mesh.AuthoredMaterial}};
    }

    std::variant<std::monostate, AssetError> ValidateMeshData(const MeshData& mesh)
    {
        if (!mesh.ImportedClothMaxDistance.empty())
        {
            if (mesh.ImportedClothMaxDistance.size() != mesh.Vertices.size())
                return AssetError {AssetErrorCode::InvalidData, "Invalid cloth colour map size"};
            for (float v : mesh.ImportedClothMaxDistance)
                if (!std::isfinite(v) || v < 0)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid cloth colour map value"};
        }

        const auto vertices = mesh.Vertices.size(), indices = mesh.Indices.size();
        if (!vertices || !indices || indices % 3 || vertices > std::numeric_limits<uint32_t>::max() ||
            indices > std::numeric_limits<uint32_t>::max())
            return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product geometry counts"};
        if (56ull + uint64_t(vertices) * 64 + uint64_t(indices) * 4 > 512ull * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidData, "Mesh Product exceeds size limit"};
        if (mesh.Material.GetSourceID().IsValid() != (mesh.Material.GetLocalID() != 0))
            return AssetError {AssetErrorCode::InvalidData, "Partially valid mesh material identity"};
        if (mesh.AuthoredMaterial.GetSourceID().IsValid() != (mesh.AuthoredMaterial.GetLocalID() != 0))
            return AssetError {AssetErrorCode::InvalidData, "Partially valid authored mesh material identity"};
        const auto finite = [](const auto& values) {
            return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
        };
        for (const auto& vertex : mesh.Vertices)
            if (!finite(vertex.Position) || !finite(vertex.Normal) || !finite(vertex.UV) || !finite(vertex.Color) ||
                !finite(vertex.Tangent))
                return AssetError {AssetErrorCode::InvalidData, "Non-finite Mesh Product vertex"};
        if (mesh.HasSmoothNormals())
        {
            if (mesh.SmoothNormals.size() != vertices)
                return AssetError {AssetErrorCode::InvalidData, "Invalid smooth normal stream size"};
            for (const auto& n : mesh.SmoothNormals)
            {
                const double lengthSquared = double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2];
                if (!finite(n) || std::abs(lengthSquared - 1) > 1e-5)
                    return AssetError {AssetErrorCode::InvalidData, "Smooth normals must be finite unit vectors"};
            }
        }
        for (const auto index : mesh.Indices)
            if (index >= vertices)
                return AssetError {AssetErrorCode::InvalidData, "Mesh Product index out of range"};
        const auto validBounds = [](const Bounds& bounds) {
            return bounds.IsValid() && std::isfinite(bounds.Min.x) && std::isfinite(bounds.Min.y) &&
                   std::isfinite(bounds.Min.z) && std::isfinite(bounds.Max.x) && std::isfinite(bounds.Max.y) &&
                   std::isfinite(bounds.Max.z);
        };
        std::set<std::string> morphNames;
        uint64_t              morphBytes = 0;
        for (const auto& target : mesh.MorphTargets)
        {
            if (target.Name.empty() || target.Name.size() > 4096 || target.Name.find('\0') != std::string::npos ||
                !morphNames.insert(target.Name).second || !std::isfinite(target.DefaultWeight) ||
                !validBounds(target.PositionBounds))
                return AssetError {AssetErrorCode::InvalidData, "Invalid morph target metadata"};
            int64_t previous = -1;
            for (const auto& d : target.Deltas)
            {
                if (d.Vertex >= vertices || int64_t(d.Vertex) <= previous || !finite(d.Position) || !finite(d.Normal) ||
                    !finite(d.Tangent) ||
                    glm::any(glm::lessThan(glm::vec3(d.Position[0], d.Position[1], d.Position[2]),
                                           target.PositionBounds.Min)) ||
                    glm::any(glm::greaterThan(glm::vec3(d.Position[0], d.Position[1], d.Position[2]),
                                              target.PositionBounds.Max)))
                    return AssetError {AssetErrorCode::InvalidData, "Invalid sparse morph delta"};
                previous = d.Vertex;
            }
            morphBytes += 36 + target.Name.size() + target.Deltas.size() * 40ull;
        }
        if (mesh.MorphTargets.size() > 65536 ||
            morphBytes + mesh.SmoothNormals.size() * 12ull + vertices * 64ull + indices * 4ull > 512ull * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidData, "Morph Product exceeds size limit"};
        if (mesh.VertexFormat == MeshVertexFormat::Skinned)
        {
            if (!mesh.Skeleton.IsValid() || mesh.Skin.size() != vertices || mesh.JointBounds.empty() ||
                mesh.JointBounds.size() > 65536)
                return AssetError {AssetErrorCode::InvalidData, "Invalid skin stream or skeleton"};
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(mesh.SkinBindTransform[c][r]))
                        return AssetError {AssetErrorCode::InvalidData, "Non-finite mesh bind transform"};
            if (std::abs(glm::determinant(mesh.SkinBindTransform)) < 1e-12f)
                return AssetError {AssetErrorCode::InvalidData, "Singular mesh bind transform"};
            for (const auto& box : mesh.JointBounds)
                if (box.IsValid() && !validBounds(box))
                    return AssetError {AssetErrorCode::InvalidData, "Invalid joint bounds"};
            for (const auto& skin : mesh.Skin)
            {
                double sum = 0;
                for (size_t k = 0; k < 4; ++k)
                {
                    if (skin.Joints[k] >= mesh.JointBounds.size() || !std::isfinite(skin.Weights[k]) ||
                        skin.Weights[k] < 0)
                        return AssetError {AssetErrorCode::InvalidData, "Invalid skin influence"};
                    sum += skin.Weights[k];
                }
                if (std::abs(sum - 1) > 1e-5)
                    return AssetError {AssetErrorCode::InvalidData, "Skin weights must sum to one"};
            }
        }
        else if (!mesh.Skin.empty() || mesh.Skeleton.IsValid() || !mesh.JointBounds.empty())
            return AssetError {AssetErrorCode::InvalidData, "Static mesh contains skin data"};
        if ((mesh.VertexFormat != MeshVertexFormat::PositionNormalUVColorTangent &&
             mesh.VertexFormat != MeshVertexFormat::Skinned) ||
            mesh.Sections.empty() || mesh.MaterialSlots.empty() || !validBounds(mesh.LocalBounds) ||
            mesh.Sections.size() > 1000000 || mesh.MaterialSlots.size() > 1000000)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product format, sections or bounds"};
        uint64_t size = 56ull + mesh.SmoothNormals.size() * 12ull + vertices * 64ull + indices * 4ull +
                        mesh.Sections.size() * 36ull;
        for (const auto& slot : mesh.MaterialSlots)
        {
            if (slot.Name.size() > 4096 || slot.ObjectKey.size() > 4096 || slot.Name.find('\0') != std::string::npos ||
                slot.ObjectKey.find('\0') != std::string::npos ||
                slot.DefaultMaterial.GetSourceID().IsValid() != (slot.DefaultMaterial.GetLocalID() != 0) ||
                slot.AuthoredMaterial.GetSourceID().IsValid() != (slot.AuthoredMaterial.GetLocalID() != 0))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product material slot"};
            size += 40 + slot.Name.size() + slot.ObjectKey.size();
        }
        if (size > 512ull * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidData, "Mesh Product exceeds size limit"};
        uint64_t end = 0;
        for (const auto& section : mesh.Sections)
        {
            if (!section.IndexCount || section.IndexCount % 3 || section.FirstIndex != end ||
                uint64_t(section.FirstIndex) + section.IndexCount > indices ||
                section.MaterialSlot >= mesh.MaterialSlots.size() || !validBounds(section.LocalBounds) ||
                glm::any(glm::lessThan(section.LocalBounds.Min, mesh.LocalBounds.Min)) ||
                glm::any(glm::greaterThan(section.LocalBounds.Max, mesh.LocalBounds.Max)))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Mesh Product section range, slot or bounds"};
            end += section.IndexCount;
        }
        if (end != indices)
            return AssetError {AssetErrorCode::InvalidData, "Mesh Product sections do not cover indices"};
        return std::monostate {};
    }
} // namespace Kans
