// Included by VfxCompiler.cpp; CPU and generated GPU kernels use this packed layout.
bool IsMeshOperation(const std::string& op)
{
    static const std::set<std::string> ops {"RandomTriangle",
                                            "TriangleCount",
                                            "SampleTriangle",
                                            "RandomVertex",
                                            "VertexCount",
                                            "SampleVertex",
                                            "SampleSkinnedTriangle",
                                            "RandomBone",
                                            "BoneCount",
                                            "SampleBone",
                                            "BoneByName"};
    return ops.count(op) != 0;
}
std::string MeshBindingKey(const InterfaceBinding& b)
{
    std::string key;
    auto        append = [&](const std::string& value) { key += std::to_string(value.size()) + ":" + value; };
    for (const auto& value : {b.Kind, b.Source, b.EntityPath, b.Asset.ToString(), b.Skeleton.ToString()})
        append(value);
    append(std::to_string(b.Sections.size()));
    for (auto section : b.Sections)
        append(std::to_string(section));
    append(std::to_string(b.Bones.size()));
    for (const auto& name : b.Bones)
        append(name);
    append(std::to_string(b.Region.size()));
    for (const auto& name : b.Region)
        append(name);
    return key;
}
std::shared_ptr<const MeshSamplingGeometry> InternMeshGeometry(std::vector<glm::vec4> values)
{
    static std::mutex                                             mutex;
    static std::vector<std::weak_ptr<const MeshSamplingGeometry>> entries;
    std::lock_guard<std::mutex>                                   lock(mutex);
    for (auto at = entries.begin(); at != entries.end();)
        if (auto geometry = at->lock())
        {
            if (geometry->Values == values)
                return geometry;
            ++at;
        }
        else
            at = entries.erase(at);
    auto geometry    = std::make_shared<MeshSamplingGeometry>();
    geometry->Values = std::move(values);
    entries.push_back(geometry);
    return geometry;
}
void BakeMeshSampling(InterfaceBinding& b, const MeshData& mesh, const SkeletonData& skeleton)
{
    const bool skinned    = b.Kind == "SkeletalMesh";
    auto       validation = ValidateMeshData(mesh);
    if (auto error = std::get_if<AssetError>(&validation))
        throw std::invalid_argument("Invalid sampling mesh: " + error->Message);
    if (skinned && (mesh.Skin.size() != mesh.Vertices.size() || skeleton.Joints.empty()))
        throw std::invalid_argument("SkeletalMesh requires skin weights and a skeleton");
    b.Skeleton = skinned ? mesh.Skeleton : AssetID {};
    b.SkeletonNames.clear();
    for (const auto& j : skeleton.Joints)
        b.SkeletonNames.push_back(j.Name);
    auto resolve = [&](const std::vector<std::string>& names) {
        std::vector<uint32_t> indices;
        for (const auto& name : names)
        {
            auto index = skeleton.FindJoint(name);
            if (index < 0)
                throw std::invalid_argument("Missing bone: " + name);
            indices.push_back(uint32_t(index));
        }
        return indices;
    };
    auto bones = resolve(b.Bones), region = resolve(b.Region);
    if (bones.empty())
        for (uint32_t j = 0; j < skeleton.Joints.size(); ++j)
            bones.push_back(j);
    std::vector<glm::vec4> data(12, glm::vec4(0));
    auto                   matrix = [&](const glm::mat4& m) {
        for (int c = 0; c < 4; ++c)
            data.push_back(m[c]);
    };
    for (const auto& v : mesh.Vertices)
    {
        glm::vec4 p(v.Position[0], v.Position[1], v.Position[2], 1);
        glm::vec3 n(v.Normal[0], v.Normal[1], v.Normal[2]);
        glm::vec4 t(v.Tangent[0], v.Tangent[1], v.Tangent[2], v.Tangent[3]);
        if (skinned)
        {
            p = mesh.SkinBindTransform * p;
            n = glm::transpose(glm::inverse(glm::mat3(mesh.SkinBindTransform))) * n;
            t = glm::vec4(glm::mat3(mesh.SkinBindTransform) * glm::vec3(t), t.w);
        }
        data.push_back(p);
        data.push_back(glm::vec4(n, 0));
        data.push_back(t);
        data.emplace_back(v.UV[0], v.UV[1], 0, 0);
        data.emplace_back(v.Color[0], v.Color[1], v.Color[2], v.Color[3]);
        auto      index = size_t(&v - mesh.Vertices.data());
        glm::vec4 joints(0), weights(0);
        if (skinned)
            for (int k = 0; k < 4; ++k)
            {
                if (mesh.Skin[index].Weights[k] > 0 && mesh.Skin[index].Joints[k] >= skeleton.Joints.size())
                    throw std::invalid_argument("Skin joint outside skeleton");
                joints[k]  = float(mesh.Skin[index].Joints[k]);
                weights[k] = mesh.Skin[index].Weights[k];
            }
        data.push_back(joints);
        data.push_back(weights);
    }
    uint32_t           triangles = uint32_t(data.size());
    std::set<uint32_t> vertices;
    double             total = 0;
    for (uint32_t first = 0; first + 2 < mesh.Indices.size(); first += 3)
    {
        if (!b.Sections.empty())
        {
            bool included = false;
            for (uint32_t section : b.Sections)
            {
                if (section >= mesh.Sections.size())
                    throw std::invalid_argument("Missing mesh section");
                const auto& s = mesh.Sections[section];
                included |= first >= s.FirstIndex && first + 2 < s.FirstIndex + s.IndexCount;
            }
            if (!included)
                continue;
        }
        uint32_t a = mesh.Indices[first], c = mesh.Indices[first + 1], d = mesh.Indices[first + 2];
        if (!region.empty())
        {
            std::map<uint32_t, float> influence;
            for (auto index : {a, c, d})
                for (int k = 0; k < 4; ++k)
                    influence[mesh.Skin[index].Joints[k]] += mesh.Skin[index].Weights[k];
            auto dominant = std::max_element(
                influence.begin(), influence.end(), [](auto& x, auto& y) { return x.second < y.second; });
            if (dominant == influence.end() || std::find(region.begin(), region.end(), dominant->first) == region.end())
                continue;
        }
        glm::vec3 p(data[12 + a * 7]), q(data[12 + c * 7]), r(data[12 + d * 7]);
        total += double(glm::length(glm::cross(q - p, r - p))) * .5;
        data.emplace_back(float(a), float(c), float(d), float(total));
        vertices.insert(a);
        vertices.insert(c);
        vertices.insert(d);
    }
    uint32_t triangleCount = uint32_t(data.size()) - triangles;
    if (total > 0)
        for (uint32_t t = triangles; t < data.size(); ++t)
            data[t].w /= float(total);
    if (b.Sections.empty() && b.Region.empty())
        for (uint32_t v = 0; v < mesh.Vertices.size(); ++v)
            vertices.insert(v);
    uint32_t vertexList = uint32_t(data.size());
    for (auto v : vertices)
        data.emplace_back(float(v), 0, 0, 0);
    uint32_t boneList = uint32_t(data.size());
    for (auto bone : bones)
        data.emplace_back(float(bone), 0, 0, 0);
    uint32_t palette = 12;
    uint32_t bind    = uint32_t(data.size());
    for (const auto& j : skeleton.Joints)
        matrix(glm::inverse(j.InverseBind));
    data[0] = {float(mesh.Vertices.size()), float(triangleCount), float(skeleton.Joints.size()), float(bones.size())};
    data[1] = {12, float(triangles), float(boneList), float(palette)};
    data[2] = {float(palette + skeleton.Joints.size() * 4), float(bind), float(total), 1};
    data[3] = {0, float(skinned), float(vertexList), float(vertices.size())};
    for (int c = 0; c < 4; ++c)
        data[4 + c] = data[8 + c] = glm::mat4(1)[c];
    b.Geometry = InternMeshGeometry(std::move(data));
    b.Values.clear();
}
InterfaceBinding CachedMeshSampling(const InterfaceBinding&         binding,
                                    const Ref<const MeshAsset>&     mesh,
                                    const Ref<const SkeletonAsset>& skeleton)
{
    struct Entry
    {
        std::weak_ptr<const MeshAsset>            Mesh;
        std::weak_ptr<const SkeletonAsset>        Skeleton;
        std::string                               Key;
        std::weak_ptr<const MeshSamplingGeometry> Geometry;
        AssetID                                   SkeletonID;
        std::vector<std::string>                  Names;
    };
    static std::mutex           mutex;
    static std::vector<Entry>   entries;
    std::lock_guard<std::mutex> lock(mutex);
    auto                        geometry = binding;
    geometry.Source.clear();
    geometry.EntityPath.clear();
    auto key = MeshBindingKey(geometry);
    entries.erase(std::remove_if(entries.begin(),
                                 entries.end(),
                                 [](const auto& e) { return e.Mesh.expired() || e.Geometry.expired(); }),
                  entries.end());
    for (const auto& e : entries)
        if (e.Mesh.lock() == mesh && e.Skeleton.lock() == skeleton && e.Key == key)
        {
            auto result          = binding;
            result.Geometry      = e.Geometry.lock();
            result.Skeleton      = e.SkeletonID;
            result.SkeletonNames = e.Names;
            return result;
        }
    auto baked = binding;
    BakeMeshSampling(baked, mesh->GetData(), skeleton ? skeleton->GetData() : SkeletonData {});
    entries.push_back({mesh, skeleton, key, baked.Geometry, baked.Skeleton, baked.SkeletonNames});
    return baked;
}
MeshSamplingSnapshot MeshSamplingFrame(const InterfaceBinding&       b,
                                       const glm::mat4&              source,
                                       const glm::mat4&              previousSource,
                                       const glm::mat4&              owner,
                                       float                         delta,
                                       const std::vector<glm::mat4>& palette,
                                       const std::vector<glm::mat4>& previousPalette,
                                       bool                          valid)
{
    MeshSamplingSnapshot snapshot;
    snapshot.Geometry = b.Geometry;
    auto& data        = snapshot.Frame;
    data.resize(12, glm::vec4(0));
    if (b.Geometry)
        std::copy_n(b.Geometry->Values.begin(), 12, data.begin());
    data[2].w = float(valid && bool(b.Geometry));
    data.resize(12 + size_t(data[0].z) * 8, glm::vec4(0));
    data[3].x    = delta > 0 ? 1 / delta : 0;
    auto current = glm::inverse(owner) * source, previous = glm::inverse(owner) * previousSource;
    for (int c = 0; c < 4; ++c)
    {
        data[4 + c] = current[c];
        data[8 + c] = previous[c];
    }
    if (b.Kind == "SkeletalMesh")
    {
        size_t count = size_t(data[0].z);
        if (palette.size() != count || previousPalette.size() != count)
            data[2].w = 0;
        else
            for (size_t j = 0; j < count; ++j)
                for (int c = 0; c < 4; ++c)
                {
                    data[size_t(data[1].w) + j * 4 + c] = palette[j][c];
                    data[size_t(data[2].x) + j * 4 + c] = previousPalette[j][c];
                }
    }
    return snapshot;
}
std::vector<std::pair<const InterfaceBinding*, uint32_t>> MeshGpuBindings(const Program& p)
{
    uint32_t binding = 7;
    for (const auto& r : p.Resources)
        binding = std::max(binding, r.Binding + (r.Kind == "ParticleReader" ? 3u : 1u));
    std::vector<std::pair<const InterfaceBinding*, uint32_t>> result;
    std::set<std::string>                                     keys;
    for (const auto& table : p.Interfaces)
    {
        const auto& i = p.Code.at(table.Instruction);
        if (IsMeshOperation(i.Op) && keys.insert(MeshBindingKey(i.Interface)).second)
            result.emplace_back(&i.Interface, binding++);
    }
    return result;
}
std::vector<glm::vec4> GpuInterfaceTable(const Program& p, const ExecutionContext& context)
{
    std::vector<glm::vec4> data;
    for (const auto& table : p.Interfaces)
    {
        const auto& i = p.Code.at(table.Instruction);
        if (IsMeshOperation(i.Op))
            data.emplace_back(0);
        else
            data.insert(data.end(), i.Interface.Values.begin(), i.Interface.Values.end());
    }
    if (data.empty())
        data.emplace_back(0);
    std::map<std::string, uint32_t> meshOffsets;
    for (const auto& table : p.Interfaces)
    {
        const auto& i = p.Code.at(table.Instruction);
        if (!IsMeshOperation(i.Op))
            continue;
        auto key = MeshBindingKey(i.Interface);
        if (auto at = meshOffsets.find(key); at != meshOffsets.end())
        {
            data.at(table.ValueOffset).x = float(at->second);
            continue;
        }
        meshOffsets[key] = uint32_t(data.size());
        auto snapshot    = context.MeshTable ? context.MeshTable(i.Interface) : nullptr;
        auto fallback = snapshot ? MeshSamplingSnapshot {} :
                                   MeshSamplingFrame(i.Interface,
                                                     context.LocalToWorld,
                                                     context.LocalToWorld,
                                                     context.LocalToWorld,
                                                     0,
                                                     {},
                                                     {},
                                                     i.Interface.Kind == "StaticMesh" &&
                                                         (i.Interface.Source.empty() || i.Interface.Source == "Asset"));
        const auto& values           = snapshot ? snapshot->Frame : fallback.Frame;
        data.at(table.ValueOffset).x = float(data.size());
        data.insert(data.end(), values.begin(), values.end());
    }
    return data;
}
namespace
{
    glm::mat4 MeshMatrix(const std::vector<glm::vec4>& d, uint32_t offset)
    {
        return {d.at(offset), d.at(offset + 1), d.at(offset + 2), d.at(offset + 3)};
    }
    glm::vec3 MeshUnit(glm::vec3 n) { return glm::dot(n, n) > 1e-20f ? glm::normalize(n) : glm::vec3(0); }
    Value     EvaluateMesh(const Instruction& i, const std::vector<Value>& registers, const ExecutionContext& context)
    {
        auto                                snapshot = context.MeshTable ? context.MeshTable(i.Interface) : nullptr;
        const auto                          geometry = snapshot ? snapshot->Geometry : i.Interface.Geometry;
        static const std::vector<glm::vec4> empty;
        const auto&                         d = geometry ? geometry->Values : empty;
        auto fallback = snapshot ?
                            MeshSamplingSnapshot {} :
                            MeshSamplingFrame(i.Interface,
                                              glm::mat4(1),
                                              glm::mat4(1),
                                              glm::mat4(1),
                                              0,
                                              std::vector<glm::mat4>(d.empty() ? 0 : size_t(d[0].z), glm::mat4(1)),
                                              std::vector<glm::mat4>(d.empty() ? 0 : size_t(d[0].z), glm::mat4(1)));
        const auto& f = snapshot ? snapshot->Frame : fallback.Frame;
        Value       result = Value::Vector({}, i.Kind);
        if (i.Kind == Type::Quat)
            result.Data.w = 1;
        if (i.Op == "BoneByName")
            return Value::Int(i.Literal.Integer);
        if (d.size() < 12 || f[2].w == 0)
            return i.Op == "RandomTriangle" || i.Op == "RandomVertex" || i.Op == "RandomBone" ? Value::Int(-1) : result;
        auto        arg = [&](size_t n) { return registers.at(i.Args.at(n)); };
        const auto& op  = i.Op;
        // Keep the spawn-index domain distinct: the first batch has UniqueID == SpawnIndex.
        auto random = [&](uint32_t lane) {
            return float(Hash(context.Seed ^ Hash(context.EmitterIndex) ^
                              Hash(context.RandomID.value_or(context.UniqueID)) ^ i.RandomKey ^
                              Hash(context.SpawnIndex + 0x9e3779b9u) ^ Hash(lane) ^ Hash(context.EventIndex)) >>
                         8) *
                   (1.f / 16777216.f);
        };
        if (op == "BoneByName")
            return Value::Int(i.Literal.Integer);
        if (op == "TriangleCount")
            return Value::Int(int(d[0].y));
        if (op == "VertexCount")
            return Value::Int(int(d[3].w));
        if (op == "BoneCount")
            return Value::Int(int(d[0].w));
        if (op == "RandomTriangle")
        {
            if (d[2].z <= 0)
                return Value::Int(-1);
            uint32_t lo = 0, hi = uint32_t(d[0].y), base = uint32_t(d[1].y);
            float    u = random(0);
            while (lo < hi)
            {
                auto mid = (lo + hi) / 2;
                if (u < d[base + mid].w)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            return Value::Int(int(std::min(lo, uint32_t(d[0].y) - 1)));
        }
        if (op == "RandomVertex" || op == "RandomBone")
        {
            auto count = uint32_t(op == "RandomBone" ? d[0].w : d[3].w);
            if (!count)
                return Value::Int(-1);
            uint32_t base = uint32_t(op == "RandomBone" ? d[1].z : d[3].z);
            return Value::Int(int(d[base + std::min(uint32_t(random(0) * count), count - 1)].x));
        }
        int  index = arg(0).Integer;
        bool bone = op == "SampleBone", vertex = op == "SampleVertex";
        if (index < 0 || index >= int(bone ? d[0].z : vertex ? d[0].x : d[0].y))
            return result;
        if (i.Output == "Valid")
            return Value::Bool(true);
        auto      source = MeshMatrix(f, 4), previousSource = MeshMatrix(f, 8);
        glm::vec3 position(0), previous(0), normal(0);
        glm::vec4 tangent(0), uv(0), color(0);
        if (bone)
        {
            auto bind    = MeshMatrix(d, uint32_t(d[2].y) + index * 4);
            auto current = source * MeshMatrix(f, uint32_t(f[1].w) + index * 4) * bind;
            auto before  = previousSource * MeshMatrix(f, uint32_t(f[2].x) + index * 4) * bind;
            position     = glm::vec3(current[3]);
            previous     = glm::vec3(before[3]);
            if (i.Output == "Rotation")
            {
                glm::mat3 rotation(current);
                for (int c = 0; c < 3; ++c)
                    rotation[c] = MeshUnit(rotation[c]);
                auto q      = glm::normalize(glm::quat_cast(rotation));
                result.Data = {q.x, q.y, q.z, q.w};
                return result;
            }
        }
        else
        {
            glm::vec3 indices {float(index)};
            glm::vec3 bary(1, 0, 0);
            if (!vertex)
            {
                indices = glm::vec3(d[uint32_t(d[1].y) + index]);
                bary    = glm::vec3(arg(1).Data);
            }
            for (int c = 0; c < (vertex ? 1 : 3); ++c)
            {
                auto      offset = uint32_t(d[1].x) + uint32_t(indices[c]) * 7;
                auto      p      = d[offset];
                auto      n      = glm::vec3(d[offset + 1]);
                auto      t      = d[offset + 2];
                glm::mat4 skin(1), oldSkin(1);
                if (d[3].y != 0)
                {
                    skin = oldSkin = glm::mat4(0);
                    for (int k = 0; k < 4; ++k)
                        if (d[offset + 6][k] > 0)
                        {
                            uint32_t j = uint32_t(d[offset + 5][k]);
                            float    w = d[offset + 6][k];
                            skin += MeshMatrix(f, uint32_t(f[1].w) + j * 4) * w;
                            oldSkin += MeshMatrix(f, uint32_t(f[2].x) + j * 4) * w;
                        }
                }
                position += glm::vec3(source * skin * p) * bary[c];
                previous += glm::vec3(previousSource * oldSkin * p) * bary[c];
                normal += MeshUnit(glm::transpose(glm::inverse(glm::mat3(source * skin))) * n) * bary[c];
                tangent += glm::vec4(MeshUnit(glm::mat3(source * skin) * glm::vec3(t)), t.w) * bary[c];
                uv += d[offset + 3] * bary[c];
                color += d[offset + 4] * bary[c];
            }
        }
        if (i.Output == "Position" || i.Output == "Value")
            result.Data = glm::vec4(position, 0);
        else if (i.Output == "Normal")
            result.Data = glm::vec4(MeshUnit(normal), 0);
        else if (i.Output == "Tangent")
            result.Data = glm::vec4(MeshUnit(glm::vec3(tangent)), tangent.w);
        else if (i.Output == "UV0")
            result.Data = uv;
        else if (i.Output == "Color")
            result.Data = color;
        else if (i.Output == "Velocity" || i.Output == "MeshVelocity")
            result.Data = glm::vec4((position - previous) * f[3].x, 0);
        return result;
    }
} // namespace
