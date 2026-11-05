#include "kspch.h"
#include "RagdollAsset.h"
#include <set>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include "Kans3D/Asset/Loader/Loader.h"
#include "PhysicsSerialization.h"
namespace Kans
{
    namespace
    {
        void Check(bool b, const char* message)
        {
            if (!b)
                throw std::invalid_argument(message);
        }
        std::vector<glm::mat4> BindPose(const SkeletonData& s)
        {
            std::vector<glm::mat4> m(s.Joints.size());
            for (size_t i = 0; i < m.size(); ++i)
            {
                auto p = s.Joints[i].Parent;
                Check(p < int32_t(i), "Skeleton must be topologically sorted");
                m[i] = (p < 0 ? glm::mat4(1) : m[p]) * s.Joints[i].BindLocal.Matrix();
            }
            return m;
        }
    } // namespace
    uint64_t RagdollSkeletonSignature(const SkeletonData& s)
    {
        uint64_t h = 14695981039346656037ull;
        for (const auto& b : s.Joints)
        {
            for (unsigned char c : b.Name)
            {
                h ^= c;
                h *= 1099511628211ull;
            }
            h ^= uint32_t(b.Parent);
            h *= 1099511628211ull;
        }
        return h;
    }
    float RagdollVolume(const RagdollPrimitive& primitive)
    {
        return std::visit(
            [](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                float v = 0;
                if constexpr (std::is_same_v<T, Physics::Box>)
                    v = 8 * s.HalfExtents.x * s.HalfExtents.y * s.HalfExtents.z;
                else if constexpr (std::is_same_v<T, Physics::Sphere>)
                    v = 4.f / 3 * glm::pi<float>() * s.Radius * s.Radius * s.Radius;
                else
                    v = glm::pi<float>() * s.Radius * s.Radius * (2 * s.HalfHeight + 4.f / 3 * s.Radius);
                return v * std::abs(s.Offset.Scale.x * s.Offset.Scale.y * s.Offset.Scale.z);
            },
            primitive);
    }
    RagdollData
    GenerateRagdoll(const SkeletonData& s, AssetID id, const MeshData* mesh, const RagdollGenerationOptions& o)
    {
        Check(o.MinimumBoneLength >= 0 && o.TotalMass > 0 && o.RadiusFraction > 0 && o.LeafLengthFactor > 0,
              "Invalid ragdoll generation options");
        auto        bind = BindPose(s);
        RagdollData d;
        d.Skeleton          = id;
        d.SkeletonSignature = RagdollSkeletonSignature(s);
        std::vector<int32_t> owners(s.Joints.size(), -1);
        std::vector<float>   lengths(s.Joints.size());
        for (size_t i = 0; i < s.Joints.size(); ++i)
        {
            const auto& bone   = s.Joints[i];
            auto        parent = bone.Parent;
            owners[i]          = parent < 0 ? -1 : owners[parent];
            if (std::find(o.SkipBones.begin(), o.SkipBones.end(), bone.Name) != o.SkipBones.end())
                continue;
            glm::vec3 head(bind[i][3]), tail;
            int32_t   child = -1;
            for (size_t j = i + 1; j < s.Joints.size(); ++j)
                if (s.Joints[j].Parent == int32_t(i) &&
                    (child < 0 ||
                     glm::distance(head, glm::vec3(bind[j][3])) > glm::distance(head, glm::vec3(bind[child][3]))))
                    child = int32_t(j);
            if (child >= 0)
                tail = glm::vec3(bind[child][3]);
            else
            {
                auto  axis = parent < 0 ? glm::vec3(bind[i][1]) : head - glm::vec3(bind[parent][3]);
                float n    = glm::length(axis);
                if (n < 1e-8f)
                    axis = {0, 1, 0};
                else
                    axis /= n;
                tail = head + axis * (parent < 0 ? o.MinimumBoneLength * 2 : lengths[parent] * o.LeafLengthFactor);
            }
            float length = glm::length(tail - head);
            lengths[i]   = length;
            bool merge   = length < o.MinimumBoneLength;
            if (merge && owners[i] < 0)
                continue;
            if (!merge)
            {
                owners[i] = int32_t(d.Bodies.size());
                RagdollBody b;
                b.Bone = bone.Name;
                d.Bodies.push_back(b);
            }
            float radius = std::max(.001f, length * o.RadiusFraction);
            if (mesh && mesh->Skin.size() == mesh->Vertices.size())
            {
                float      fitted = 0;
                bool       found  = false;
                const auto axis   = length > 1e-8f ? (tail - head) / length : glm::vec3(0, 1, 0);
                for (size_t v = 0; v < mesh->Skin.size(); ++v)
                {
                    const auto& skin     = mesh->Skin[v];
                    size_t      dominant = 0;
                    for (size_t k = 1; k < skin.Weights.size(); ++k)
                        if (skin.Weights[k] > skin.Weights[dominant])
                            dominant = k;
                    if (skin.Joints[dominant] != i || skin.Weights[dominant] < .5f)
                        continue;
                    const auto& pos = mesh->Vertices[v].Position;
                    auto        p   = glm::vec3(mesh->SkinBindTransform * glm::vec4(pos[0], pos[1], pos[2], 1)) - head;
                    fitted          = std::max(fitted, glm::length(p - axis * glm::dot(p, axis)));
                    found           = true;
                }
                if (found)
                    radius = std::max(.001f, fitted);
            }
            auto&   body      = d.Bodies[owners[i]];
            int32_t ownerBone = s.FindJoint(body.Bone);
            // The body's frame as the runtime places it: the bone's position and rotation without its
            // scale, so offsets stay in metres under a scaled root (a centimetre FBX's 0.01).
            const auto rigid   = Physics::TransformShape({}, bind[ownerBone]);
            auto       inverse = glm::inverse(glm::translate(glm::mat4(1), rigid.Position) * glm::mat4_cast(rigid.Rotation));
            auto    a = glm::vec3(inverse * glm::vec4(head, 1)), b = glm::vec3(inverse * glm::vec4(tail, 1));
            Physics::ShapeOffset offset;
            offset.Position = (a + b) * .5f;
            if (glm::length(b - a) > 1e-8f)
                offset.Rotation = glm::rotation(glm::vec3(0, 1, 0), glm::normalize(b - a));
            if (o.PreferBox)
            {
                Physics::Box shape;
                shape.HalfExtents = {radius, std::max(.001f, length * .5f), radius};
                shape.Offset      = offset;
                body.Shapes.push_back(shape);
            }
            else
            {
                Physics::Capsule shape;
                shape.Radius     = radius;
                shape.HalfHeight = std::max(0.f, length * .5f - radius);
                shape.Offset     = offset;
                body.Shapes.push_back(shape);
            }
        }
        float volume = 0;
        for (auto& b : d.Bodies)
        {
            b.Mass = 0;
            for (auto& p : b.Shapes)
                b.Mass += RagdollVolume(p);
            volume += b.Mass;
        }
        for (auto& b : d.Bodies)
        {
            b.Mass = o.TotalMass * b.Mass / volume;
            auto j = s.FindJoint(b.Bone), parent = s.Joints[j].Parent;
            while (parent >= 0 && (owners[parent] < 0 || owners[parent] == owners[j]))
                parent = s.Joints[parent].Parent;
            if (parent >= 0)
            {
                auto c = o.DefaultLimits;
                c.Bone = b.Bone;
                d.Constraints.push_back(c);
            }
        }
        ValidateRagdollData(d);
        return d;
    }
    void ValidateRagdollData(const RagdollData& d)
    {
        Check(!d.Bodies.empty() && d.Bodies.size() <= 64, "Ragdoll requires 1..64 articulation links");
        Check(d.DefaultLayer < 32, "Invalid ragdoll default collision layer");
        std::set<std::string> names, constraints;
        for (const auto& b : d.Bodies)
        {
            Check(!b.Bone.empty() && names.insert(b.Bone).second, "Duplicate/empty ragdoll bone");
            Check(!b.Shapes.empty() && std::isfinite(b.Mass) && b.Mass > 0 && std::isfinite(b.Density) &&
                      b.Density > 0 && b.LinearDamping >= 0 && b.AngularDamping >= 0 && (!b.Layer || *b.Layer < 32),
                  "Invalid ragdoll body properties");
            for (const auto& p : b.Shapes)
                std::visit(
                    [](const auto& shape) {
                        using T = std::decay_t<decltype(shape)>;
                        if constexpr (std::is_same_v<T, Physics::Sphere>)
                        {
                            SphereColliderComponent c;
                            c.Shape = shape;
                            PhysicsYaml::Encode(c);
                        }
                        else if constexpr (std::is_same_v<T, Physics::Capsule>)
                        {
                            CapsuleColliderComponent c;
                            c.Shape = shape;
                            PhysicsYaml::Encode(c);
                        }
                        else
                        {
                            BoxColliderComponent c;
                            c.Shape = shape;
                            PhysicsYaml::Encode(c);
                        }
                    },
                    p);
        }
        for (const auto& c : d.Constraints)
        {
            Check(names.count(c.Bone) && constraints.insert(c.Bone).second,
                  "Unknown/duplicate ragdoll constraint bone");
            Check(c.TwistLow <= c.TwistHigh && c.TwistLow > -180 && c.TwistHigh < 180 && c.SwingY > 0 &&
                      c.SwingY < 180 && c.SwingZ > 0 && c.SwingZ < 180 && c.LimitStiffness >= 0 &&
                      c.LimitDamping >= 0 && c.DriveStrength >= 0 && c.DriveDamping >= 0,
                  "Invalid ragdoll joint limits/drive");
        }
        for (const auto& pair : d.DisabledCollisionPairs)
            Check(pair.first != pair.second && names.count(pair.first) && names.count(pair.second),
                  "Unknown ragdoll disabled collision pair");
    }
    RagdollBinding BindRagdoll(const RagdollData& d, const SkeletonData& s, AssetID id)
    {
        RagdollBinding out;
        try
        {
            ValidateRagdollData(d);
            Check(!d.Skeleton.IsValid() || d.Skeleton == id, "Ragdoll skeleton asset mismatch");
            Check(!d.SkeletonSignature || d.SkeletonSignature == RagdollSkeletonSignature(s),
                  "Ragdoll skeleton topology mismatch");
            for (const auto& b : d.Bodies)
            {
                int32_t j = s.FindJoint(b.Bone);
                if (j < 0)
                    throw std::invalid_argument("Ragdoll missing bone: " + b.Bone);
                out.Bones.push_back(uint32_t(j));
            }
            uint32_t roots = 0;
            for (auto bone : out.Bones)
            {
                int32_t p = s.Joints[bone].Parent, body = -1;
                while (p >= 0)
                {
                    auto it = std::find(out.Bones.begin(), out.Bones.end(), uint32_t(p));
                    if (it != out.Bones.end())
                    {
                        body = int32_t(it - out.Bones.begin());
                        break;
                    }
                    p = s.Joints[p].Parent;
                }
                out.Parents.push_back(body);
                roots += body < 0;
                if (body >= 0)
                    Check(std::any_of(d.Constraints.begin(),
                                      d.Constraints.end(),
                                      [&](const auto& c) { return c.Bone == s.Joints[bone].Name; }),
                          "Missing ragdoll constraint for simulated child");
            }
            Check(roots == 1, "Ragdoll has disconnected roots; one articulation requires one root");
        }
        catch (const std::exception& e)
        {
            out.Error = e.what();
        }
        return out;
    }
    YAML::Node EncodeRagdollNode(const RagdollData& d)
    {
        ValidateRagdollData(d);
        YAML::Node n;
        n["Version"]                = 1;
        n["Skeleton"]               = d.Skeleton.ToString();
        n["SkeletonSignature"]      = d.SkeletonSignature;
        n["DefaultLayer"]           = d.DefaultLayer;
        n["Bodies"]                 = YAML::Node(YAML::NodeType::Sequence);
        n["Constraints"]            = YAML::Node(YAML::NodeType::Sequence);
        n["DisabledCollisionPairs"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& b : d.Bodies)
        {
            YAML::Node node;
            node["Bone"]           = b.Bone;
            node["Mass"]           = b.Mass;
            node["Density"]        = b.Density;
            node["UseDensity"]     = b.UseDensity;
            node["LinearDamping"]  = b.LinearDamping;
            node["AngularDamping"] = b.AngularDamping;
            node["Material"]       = b.Material.ToString();
            if (b.Layer)
                node["Layer"] = *b.Layer;
            for (const auto& p : b.Shapes)
            {
                YAML::Node shape;
                shape["Type"] = p.index();
                shape["Data"] = std::visit(
                    [](const auto& s) {
                        using T = std::decay_t<decltype(s)>;
                        if constexpr (std::is_same_v<T, Physics::Sphere>)
                        {
                            SphereColliderComponent c;
                            c.Shape = s;
                            return PhysicsYaml::Encode(c);
                        }
                        else if constexpr (std::is_same_v<T, Physics::Capsule>)
                        {
                            CapsuleColliderComponent c;
                            c.Shape = s;
                            return PhysicsYaml::Encode(c);
                        }
                        else
                        {
                            BoxColliderComponent c;
                            c.Shape = s;
                            return PhysicsYaml::Encode(c);
                        }
                    },
                    p);
                node["Shapes"].push_back(shape);
            }
            n["Bodies"].push_back(node);
        }
        for (const auto& c : d.Constraints)
        {
            YAML::Node node;
            node["Bone"] = c.Bone;
            BoxColliderComponent frame;
            frame.Shape.Offset     = c.Frame;
            node["Frame"]          = PhysicsYaml::Encode(frame);
            node["TwistLow"]       = c.TwistLow;
            node["TwistHigh"]      = c.TwistHigh;
            node["SwingY"]         = c.SwingY;
            node["SwingZ"]         = c.SwingZ;
            node["LimitStiffness"] = c.LimitStiffness;
            node["LimitDamping"]   = c.LimitDamping;
            node["DriveStrength"]  = c.DriveStrength;
            node["DriveDamping"]   = c.DriveDamping;
            n["Constraints"].push_back(node);
        }
        for (const auto& p : d.DisabledCollisionPairs)
        {
            YAML::Node pair;
            pair.push_back(p.first);
            pair.push_back(p.second);
            n["DisabledCollisionPairs"].push_back(pair);
        }
        return n;
    }
    RagdollData DecodeRagdollNode(const YAML::Node& n)
    {
        Check(n.IsMap() && n["Version"].as<int>() == 1, "Invalid ragdoll version");
        RagdollData d;
        d.Skeleton          = AssetID::FromString(n["Skeleton"].as<std::string>());
        d.SkeletonSignature = n["SkeletonSignature"].as<uint64_t>(0);
        d.DefaultLayer      = n["DefaultLayer"].as<uint32_t>(0);
        Check(n["Bodies"].IsSequence() && n["Bodies"].size() <= 64, "Ragdoll exceeds 64 articulation links");
        for (auto node : n["Bodies"])
        {
            RagdollBody b;
            b.Bone           = node["Bone"].as<std::string>();
            b.Mass           = node["Mass"].as<float>();
            b.Density        = node["Density"].as<float>();
            b.UseDensity     = node["UseDensity"].as<bool>();
            b.LinearDamping  = node["LinearDamping"].as<float>();
            b.AngularDamping = node["AngularDamping"].as<float>();
            b.Material       = AssetID::FromString(node["Material"].as<std::string>());
            if (node["Layer"])
                b.Layer = node["Layer"].as<uint32_t>();
            for (auto p : node["Shapes"])
            {
                switch (p["Type"].as<int>())
                {
                    case 0: {
                        SphereColliderComponent c;
                        PhysicsYaml::Decode(p["Data"], c);
                        b.Shapes.push_back(c.Shape);
                        break;
                    }
                    case 1: {
                        CapsuleColliderComponent c;
                        PhysicsYaml::Decode(p["Data"], c);
                        b.Shapes.push_back(c.Shape);
                        break;
                    }
                    case 2: {
                        BoxColliderComponent c;
                        PhysicsYaml::Decode(p["Data"], c);
                        b.Shapes.push_back(c.Shape);
                        break;
                    }
                    default:
                        throw std::invalid_argument("Unknown ragdoll primitive");
                }
            }
            d.Bodies.push_back(b);
        }
        for (auto node : n["Constraints"])
        {
            RagdollConstraint c;
            c.Bone = node["Bone"].as<std::string>();
            BoxColliderComponent f;
            PhysicsYaml::Decode(node["Frame"], f);
            c.Frame          = f.Shape.Offset;
            c.TwistLow       = node["TwistLow"].as<float>();
            c.TwistHigh      = node["TwistHigh"].as<float>();
            c.SwingY         = node["SwingY"].as<float>();
            c.SwingZ         = node["SwingZ"].as<float>();
            c.LimitStiffness = node["LimitStiffness"].as<float>();
            c.LimitDamping   = node["LimitDamping"].as<float>();
            c.DriveStrength  = node["DriveStrength"].as<float>();
            c.DriveDamping   = node["DriveDamping"].as<float>();
            d.Constraints.push_back(c);
        }
        for (auto p : n["DisabledCollisionPairs"])
            d.DisabledCollisionPairs.emplace_back(p[0].as<std::string>(), p[1].as<std::string>());
        ValidateRagdollData(d);
        return d;
    }
    std::variant<Ref<RagdollAsset>, AssetError> Loader<RagdollAsset>::Load(const AssetLoadContext& c) const
    {
        if (!c.ID.IsValid() || c.Type != AssetType::Ragdoll || !c.Data || c.Size > 16 * 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid ragdoll product"};
        try
        {
            auto a = CreateRef<RagdollAsset>(
                DecodeRagdollNode(YAML::Load(std::string(reinterpret_cast<const char*>(c.Data), c.Size))));
            a->assetID = c.ID;
            return a;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
