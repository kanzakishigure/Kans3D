#include "kspch.h"
#include "ClothAsset.h"
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include "Kans3D/Asset/Loader/Loader.h"
namespace Kans
{
    namespace
    {
        void Check(bool ok, const char* message)
        {
            if (!ok)
                throw std::invalid_argument(message);
        }
        glm::vec3 Position(const MeshVertexData& v) { return {v.Position[0], v.Position[1], v.Position[2]}; }
        uint64_t  MeshHash(const MeshData& m, uint32_t section)
        {
            uint64_t h   = 1469598103934665603ull;
            auto     add = [&](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
            add(uint32_t(m.VertexFormat));
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                {
                    uint32_t bits;
                    std::memcpy(&bits, &m.SkinBindTransform[column][row], 4);
                    add(bits);
                }
            const auto& s = m.Sections.at(section);
            add(s.FirstIndex);
            add(s.IndexCount);
            for (uint32_t i = s.FirstIndex; i < s.FirstIndex + s.IndexCount; ++i)
            {
                const auto index = m.Indices.at(i);
                add(index);
                for (float f : m.Vertices.at(index).Position)
                {
                    uint32_t b;
                    std::memcpy(&b, &f, 4);
                    add(b);
                }
                if (index < m.Skin.size())
                    for (unsigned j = 0; j < 4; ++j)
                    {
                        add(m.Skin[index].Joints[j]);
                        uint32_t b;
                        std::memcpy(&b, &m.Skin[index].Weights[j], 4);
                        add(b);
                    }
            }
            return h;
        }
        template<class T>
        YAML::Node Array(const std::vector<T>& v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : v)
                n.push_back(x);
            return n;
        }
        template<class T>
        std::vector<T> ReadArray(const YAML::Node& n)
        {
            Check(n.IsSequence() && n.size() <= 4000000, "Invalid cloth array");
            std::vector<T> out;
            out.reserve(n.size());
            for (auto x : n)
                out.push_back(x.as<T>());
            return out;
        }
    } // namespace
    ClothData CreateClothForSection(AssetID id, const MeshData& mesh, uint32_t section, float epsilon)
    {
        Check(section < mesh.Sections.size(), "Cloth section does not exist");
        Check(std::isfinite(epsilon) && epsilon > 0, "Invalid cloth weld epsilon");
        ClothData d;
        d.Mesh         = id;
        d.Section      = section;
        d.VertexCount  = uint32_t(mesh.Vertices.size());
        d.WeldEpsilon  = epsilon;
        d.TopologyHash = MeshHash(mesh, section);
        d.RenderToSim.resize(mesh.Vertices.size(), UINT32_MAX);
        using Cell = std::tuple<int64_t, int64_t, int64_t>;
        std::map<Cell, std::vector<uint32_t>> cells;
        const auto&                           s = mesh.Sections[section];
        std::set<uint32_t>                    used;
        for (uint32_t i = s.FirstIndex; i < s.FirstIndex + s.IndexCount; ++i)
            used.insert(mesh.Indices.at(i));
        for (auto vertex : used)
        {
            const auto p = Position(mesh.Vertices.at(vertex));
            Check(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Invalid cloth position");
            const auto q = glm::floor(glm::dvec3(p) / double(epsilon));
            Check(glm::all(glm::lessThan(glm::abs(q), glm::dvec3(1e15))), "Cloth weld coordinate overflow");
            const int64_t x = int64_t(q.x), y = int64_t(q.y), z = int64_t(q.z);
            uint32_t      match = UINT32_MAX;
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b)
                    for (int c = -1; c <= 1; ++c)
                        if (auto it = cells.find({x + a, y + b, z + c}); it != cells.end())
                            for (auto n : it->second)
                                if (glm::dot(p - d.RestPositions[n], p - d.RestPositions[n]) <= epsilon * epsilon)
                                    match = std::min(match, n);
            const float imported = mesh.ImportedClothMaxDistance.empty() ? 0 : mesh.ImportedClothMaxDistance.at(vertex);
            if (match == UINT32_MAX)
            {
                match = uint32_t(d.RestPositions.size());
                d.RestPositions.push_back(p);
                d.Representatives.push_back(vertex);
                d.MaxDistance.push_back(std::max(0.f, imported));
                cells[{x, y, z}].push_back(match);
            }
            else
                d.MaxDistance[match] = std::min(d.MaxDistance[match], std::max(0.f, imported));
            d.RenderToSim[vertex] = match;
        }
        for (uint32_t i = s.FirstIndex; i < s.FirstIndex + s.IndexCount; i += 3)
        {
            const auto a = d.RenderToSim[mesh.Indices.at(i)], b = d.RenderToSim[mesh.Indices.at(i + 1)],
                       c = d.RenderToSim[mesh.Indices.at(i + 2)];
            if (a != b && b != c && c != a)
                d.Triangles.insert(d.Triangles.end(), {a, b, c});
        }
        d.BackstopDistance.resize(d.RestPositions.size());
        d.BackstopRadius.resize(d.RestPositions.size());
        ValidateClothData(d);
        RebuildClothConstraints(d);
        return d;
    }
    void ValidateClothData(const ClothData& d)
    {
        const auto n = d.RestPositions.size();
        Check(n > 0 && n <= 1000000 && d.VertexCount <= 4000000 && d.RenderToSim.size() == d.VertexCount &&
                  d.Representatives.size() == n,
              "Invalid cloth weld map sizes");
        Check(std::isfinite(d.WeldEpsilon) && d.WeldEpsilon > 0, "Invalid cloth epsilon");
        Check(!d.Triangles.empty() && d.Triangles.size() % 3 == 0, "Invalid cloth triangles");
        for (const auto& p : d.RestPositions)
            Check(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Nonfinite cloth position");
        for (auto i : d.RenderToSim)
            Check(i == UINT32_MAX || i < n, "Invalid cloth weld index");
        for (size_t i = 0; i < n; ++i)
            Check(d.Representatives[i] < d.VertexCount && d.RenderToSim[d.Representatives[i]] == i,
                  "Invalid cloth representative");
        for (auto i : d.Triangles)
            Check(i < n, "Invalid cloth triangle index");
        for (const auto* map :
             {&d.MaxDistance, &d.BackstopDistance, &d.BackstopRadius, &d.StretchMultiplier, &d.BendMultiplier})
        {
            const bool optional = map == &d.StretchMultiplier || map == &d.BendMultiplier;
            Check(map->size() == n || (optional && map->empty()), "Invalid cloth painted map size");
            for (float v : *map)
                Check(std::isfinite(v) && (map == &d.BackstopDistance || v >= 0), "Invalid cloth painted value");
        }
    }
    void RebuildClothConstraints(ClothData& d)
    {
        ValidateClothData(d);
        d.Stretch.clear();
        d.Bend.clear();
        d.Tethers.clear();
        d.Areas.assign(d.RestPositions.size(), 0);
        std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
        for (size_t i = 0; i < d.Triangles.size(); i += 3)
        {
            const auto  a = d.Triangles[i], b = d.Triangles[i + 1], c = d.Triangles[i + 2];
            const float area = glm::length(glm::cross(d.RestPositions[b] - d.RestPositions[a],
                                                      d.RestPositions[c] - d.RestPositions[a])) /
                               6;
            for (auto v : {a, b, c})
                d.Areas[v] += area;
            for (unsigned k = 0; k < 3; ++k)
            {
                auto x = d.Triangles[i + k], y = d.Triangles[i + (k + 1) % 3];
                edges[std::minmax(x, y)].push_back(d.Triangles[i + (k + 2) % 3]);
            }
        }
        std::vector<std::vector<std::pair<uint32_t, float>>> graph(d.RestPositions.size());
        for (const auto& [edge, opposites] : edges)
        {
            auto [a, b]      = edge;
            const float rest = glm::length(d.RestPositions[b] - d.RestPositions[a]);
            d.Stretch.push_back({a, b, rest});
            graph[a].push_back({b, rest});
            graph[b].push_back({a, rest});
            if (opposites.size() == 2 && opposites[0] != opposites[1])
                d.Bend.push_back({opposites[0],
                                  opposites[1],
                                  glm::length(d.RestPositions[opposites[0]] - d.RestPositions[opposites[1]])});
        }
        // Multi-source Dijkstra. Tuple ordering fixes ties independently of hash or thread order.
        using Item = std::tuple<float, uint32_t, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        std::vector<float>    distance(d.RestPositions.size(), std::numeric_limits<float>::infinity());
        std::vector<uint32_t> root(d.RestPositions.size(), UINT32_MAX);
        for (uint32_t i = 0; i < distance.size(); ++i)
            if (d.MaxDistance[i] == 0)
            {
                distance[i] = 0;
                root[i]     = i;
                queue.emplace(0, i, i);
            }
        while (!queue.empty())
        {
            auto [cost, source, v] = queue.top();
            queue.pop();
            if (cost != distance[v] || source != root[v])
                continue;
            for (auto [next, length] : graph[v])
                if (cost + length < distance[next] || (cost + length == distance[next] && source < root[next]))
                {
                    distance[next] = cost + length;
                    root[next]     = source;
                    queue.emplace(distance[next], source, next);
                }
        }
        for (uint32_t i = 0; i < distance.size(); ++i)
            if (d.MaxDistance[i] > 0 && root[i] != UINT32_MAX)
                d.Tethers.push_back({i, root[i], distance[i]});
    }
    void WriteClothMap(ClothData& d, ClothMap map, const std::vector<float>& values)
    {
        auto  next   = d;
        auto* target = &next.MaxDistance;
        switch (map)
        {
            case ClothMap::MaxDistance:
                break;
            case ClothMap::BackstopDistance:
                target = &next.BackstopDistance;
                break;
            case ClothMap::BackstopRadius:
                target = &next.BackstopRadius;
                break;
            case ClothMap::StretchMultiplier:
                target = &next.StretchMultiplier;
                break;
            case ClothMap::BendMultiplier:
                target = &next.BendMultiplier;
                break;
        }
        *target = values;
        RebuildClothConstraints(next);
        d = std::move(next);
    }
    bool ValidateClothBinding(const ClothData& d, AssetID id, const MeshData& m, uint32_t section)
    {
        return d.Mesh == id && d.Section == section && d.VertexCount == m.Vertices.size() &&
               section < m.Sections.size() && d.TopologyHash == MeshHash(m, section);
    }
    YAML::Node EncodeClothNode(const ClothData& d)
    {
        ValidateClothData(d);
        YAML::Node n;
        n["Version"]         = 1;
        n["Mesh"]            = d.Mesh.ToString();
        n["Section"]         = d.Section;
        n["VertexCount"]     = d.VertexCount;
        n["WeldEpsilon"]     = d.WeldEpsilon;
        n["TopologyHash"]    = d.TopologyHash;
        n["RenderToSim"]     = Array(d.RenderToSim);
        n["Representatives"] = Array(d.Representatives);
        n["Triangles"]       = Array(d.Triangles);
        YAML::Node p(YAML::NodeType::Sequence);
        for (auto v : d.RestPositions)
        {
            YAML::Node a;
            for (int i = 0; i < 3; ++i)
                a.push_back(v[i]);
            p.push_back(a);
        }
        n["RestPositions"]     = p;
        n["MaxDistance"]       = Array(d.MaxDistance);
        n["BackstopDistance"]  = Array(d.BackstopDistance);
        n["BackstopRadius"]    = Array(d.BackstopRadius);
        n["StretchMultiplier"] = Array(d.StretchMultiplier);
        n["BendMultiplier"]    = Array(d.BendMultiplier);
        return n;
    }
    ClothData DecodeClothNode(const YAML::Node& n)
    {
        Check(n.IsMap() && n["Version"].as<int>() == 1, "Invalid cloth version");
        const std::set<std::string> allowed = {"Version",
                                               "Mesh",
                                               "Section",
                                               "VertexCount",
                                               "WeldEpsilon",
                                               "TopologyHash",
                                               "RenderToSim",
                                               "Representatives",
                                               "Triangles",
                                               "RestPositions",
                                               "MaxDistance",
                                               "BackstopDistance",
                                               "BackstopRadius",
                                               "StretchMultiplier",
                                               "BendMultiplier"};
        std::set<std::string>       seen;
        for (auto item : n)
        {
            auto k = item.first.as<std::string>();
            Check(allowed.count(k) && seen.insert(k).second, "Unknown/duplicate cloth field");
        }
        ClothData d;
        d.Mesh            = AssetID::FromString(n["Mesh"].as<std::string>());
        d.Section         = n["Section"].as<uint32_t>();
        d.VertexCount     = n["VertexCount"].as<uint32_t>();
        d.WeldEpsilon     = n["WeldEpsilon"].as<float>();
        d.TopologyHash    = n["TopologyHash"].as<uint64_t>();
        d.RenderToSim     = ReadArray<uint32_t>(n["RenderToSim"]);
        d.Representatives = ReadArray<uint32_t>(n["Representatives"]);
        d.Triangles       = ReadArray<uint32_t>(n["Triangles"]);
        Check(n["RestPositions"].IsSequence() && n["RestPositions"].size() <= 1000000, "Invalid cloth positions");
        for (auto v : n["RestPositions"])
        {
            Check(v.IsSequence() && v.size() == 3, "Invalid cloth position");
            d.RestPositions.emplace_back(v[0].as<float>(), v[1].as<float>(), v[2].as<float>());
        }
        d.MaxDistance      = ReadArray<float>(n["MaxDistance"]);
        d.BackstopDistance = ReadArray<float>(n["BackstopDistance"]);
        d.BackstopRadius   = ReadArray<float>(n["BackstopRadius"]);
        if (n["StretchMultiplier"])
            d.StretchMultiplier = ReadArray<float>(n["StretchMultiplier"]);
        if (n["BendMultiplier"])
            d.BendMultiplier = ReadArray<float>(n["BendMultiplier"]);
        RebuildClothConstraints(d);
        return d;
    }
    std::variant<Ref<ClothAsset>, AssetError> Loader<ClothAsset>::Load(const AssetLoadContext& c) const
    {
        if (!c.ID.IsValid() || c.Type != AssetType::Cloth || !c.Data || c.Size > 128 * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid cloth product"};
        try
        {
            auto a = CreateRef<ClothAsset>(
                DecodeClothNode(YAML::Load(std::string(reinterpret_cast<const char*>(c.Data), c.Size))));
            a->assetID = c.ID;
            return a;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }

} // namespace Kans
