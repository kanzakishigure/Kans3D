#include "kspch.h"
#include "TransformConstraints.h"
#include <glm/gtx/euler_angles.hpp>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include "Entity.h"

namespace Kans
{
    glm::vec3 ConstraintAxisVector(ConstraintAxis axis)
    {
        glm::vec3 result(0);
        auto      index = uint32_t(axis);
        if (index < 6)
            result[index % 3] = index < 3 ? 1.f : -1.f;
        return result;
    }
    bool operator==(const TransformConstraintSource& a, const TransformConstraintSource& b)
    {
        return a.Entity == b.Entity && a.Weight == b.Weight && a.Joint == b.Joint && a.Offset == b.Offset;
    }
    bool operator==(const AimConstraintComponent& a, const AimConstraintComponent& b)
    {
        return a.Sources == b.Sources && a.Active == b.Active && a.Weight == b.Weight && a.AimAxis == b.AimAxis &&
               a.UpAxis == b.UpAxis && a.WorldUp == b.WorldUp && a.UpObject == b.UpObject &&
               a.LimitsEnabled == b.LimitsEnabled && a.MinRotation == b.MinRotation && a.MaxRotation == b.MaxRotation &&
               a.MaxAngularSpeed == b.MaxAngularSpeed;
    }
    bool operator!=(const AimConstraintComponent& a, const AimConstraintComponent& b) { return !(a == b); }
    void AimConstraintComponent::RemapEntities(const std::function<uint64_t(uint64_t)>& remap)
    {
        if (uint64_t(UpObject))
            UpObject = UUID(remap(uint64_t(UpObject)));
        for (auto& s : Sources)
            if (uint64_t(s.Entity))
                s.Entity = UUID(remap(uint64_t(s.Entity)));
    }
    namespace
    {
        bool    Finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        int32_t JointIndex(const SkeletonData& data, const std::string& name)
        {
            auto index = data.FindJoint(name);
            if (index >= 0)
                return index;
            for (size_t j = 0; j < data.Joints.size(); ++j)
                if (data.Joints[j].NodeKey == name)
                    return int32_t(j);
            return -1;
        }
        void Validate(const AimConstraintComponent& c)
        {
            bool valid = c.Sources.size() <= 256 && std::isfinite(c.Weight) && c.Weight >= 0 && c.Weight <= 1 &&
                         uint32_t(c.AimAxis) < 6 && uint32_t(c.UpAxis) < 6 &&
                         uint32_t(c.AimAxis) % 3 != uint32_t(c.UpAxis) % 3 && Finite(c.WorldUp) &&
                         glm::length(c.WorldUp) > 1e-6f && Finite(c.MinRotation) && Finite(c.MaxRotation) &&
                         glm::all(glm::lessThanEqual(c.MinRotation, c.MaxRotation)) &&
                         std::isfinite(c.MaxAngularSpeed) && c.MaxAngularSpeed >= 0;
            for (const auto& s : c.Sources)
                valid = valid && std::isfinite(s.Weight) && s.Weight >= 0 && Finite(s.Offset) && s.Joint.size() <= 1024;
            if (!valid)
                throw std::runtime_error("Invalid AimConstraintComponent");
        }
        YAML::Node Vector(glm::vec3 v)
        {
            YAML::Node n;
            for (int i = 0; i < 3; ++i)
                n.push_back(v[i]);
            return n;
        }
        glm::vec3 Vector(const YAML::Node& n, glm::vec3 fallback)
        {
            if (!n)
                return fallback;
            if (!n.IsSequence() || n.size() != 3)
                throw std::runtime_error("Invalid constraint vector");
            return {n[0].as<float>(), n[1].as<float>(), n[2].as<float>()};
        }
    } // namespace
    YAML::Node EncodeAimConstraint(const AimConstraintComponent& c)
    {
        Validate(c);
        YAML::Node n;
        n["Active"]          = c.Active;
        n["Weight"]          = c.Weight;
        n["AimAxis"]         = uint32_t(c.AimAxis);
        n["UpAxis"]          = uint32_t(c.UpAxis);
        n["WorldUp"]         = Vector(c.WorldUp);
        n["UpObject"]        = uint64_t(c.UpObject);
        n["LimitsEnabled"]   = c.LimitsEnabled;
        n["MinRotation"]     = Vector(c.MinRotation);
        n["MaxRotation"]     = Vector(c.MaxRotation);
        n["MaxAngularSpeed"] = c.MaxAngularSpeed;
        n["Sources"]         = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& s : c.Sources)
        {
            YAML::Node item;
            item["Entity"] = uint64_t(s.Entity);
            item["Weight"] = s.Weight;
            item["Joint"]  = s.Joint;
            item["Offset"] = Vector(s.Offset);
            n["Sources"].push_back(item);
        }
        return n;
    }
    AimConstraintComponent DecodeAimConstraint(const YAML::Node& n)
    {
        if (!n.IsMap())
            throw std::runtime_error("Invalid AimConstraintComponent");
        AimConstraintComponent c;
        c.Active          = n["Active"].as<bool>(true);
        c.Weight          = n["Weight"].as<float>(1);
        c.AimAxis         = ConstraintAxis(n["AimAxis"].as<uint32_t>(5));
        c.UpAxis          = ConstraintAxis(n["UpAxis"].as<uint32_t>(1));
        c.WorldUp         = Vector(n["WorldUp"], c.WorldUp);
        c.UpObject        = UUID(n["UpObject"].as<uint64_t>(0));
        c.LimitsEnabled   = n["LimitsEnabled"].as<bool>(false);
        c.MinRotation     = Vector(n["MinRotation"], c.MinRotation);
        c.MaxRotation     = Vector(n["MaxRotation"], c.MaxRotation);
        c.MaxAngularSpeed = n["MaxAngularSpeed"].as<float>(0);
        if (auto sources = n["Sources"])
        {
            if (!sources.IsSequence() || sources.size() > 256)
                throw std::runtime_error("Invalid constraint sources");
            for (const auto& item : sources)
                c.Sources.push_back({UUID(item["Entity"].as<uint64_t>(0)),
                                     item["Weight"].as<float>(1),
                                     item["Joint"].as<std::string>(""),
                                     Vector(item["Offset"], glm::vec3(0))});
        }
        Validate(c);
        return c;
    }
    glm::mat4 Scene::ConstraintLocalTransform(Entity e) const
    {
        const auto& t     = e.GetComponent<TransformComponent>();
        auto        found = m_ConstraintRotations.find(e.GetUUID());
        if (!m_ConstraintsVisible || found == m_ConstraintRotations.end() ||
            !e.HasComponent<AimConstraintComponent>() || !e.GetComponent<AimConstraintComponent>().Active)
            return t.GetTransform();
        return glm::translate(glm::mat4(1), t.Position) * glm::mat4_cast(found->second) *
               glm::scale(glm::mat4(1), t.Scale);
    }
    bool Scene::ResolveConstraintSource(const TransformConstraintSource& source, glm::mat4& world) const
    {
        auto target = GetEntityByUUID(source.Entity);
        if (!target)
            return false;
        world = GetSpringBoneRenderTransform(target);
        if (!source.Joint.empty())
        {
            auto root = GetDeformationRoot(target);
            if (!root)
                root = target;
            Ref<const SkeletonAsset> skeleton;
            const std::vector<UUID>* joints = nullptr;
            const AnimationPose*     pose   = nullptr;
            if (root.HasComponent<AnimatorComponent>())
            {
                const auto& a = root.GetComponent<AnimatorComponent>();
                skeleton      = a.SkeletonSource;
                joints        = &a.JointEntities;
                pose          = &a.PreviewPose;
            }
            else if (root.HasComponent<ModelPoseComponent>())
            {
                const auto& a = root.GetComponent<ModelPoseComponent>();
                skeleton      = a.SkeletonSource;
                joints        = &a.JointEntities;
                pose          = &a.Pose;
            }
            if (!skeleton || !joints || !pose)
                return false;
            const auto& data  = skeleton->GetData();
            const auto  index = JointIndex(data, source.Joint);
            if (index < 0)
                return false;
            const size_t j     = size_t(index);
            auto         joint = j < joints->size() ? GetEntityByUUID((*joints)[j]) : Entity {};
            if (joint)
                world = GetSpringBoneRenderTransform(joint);
            else if (auto skin = GetSkinningPose(root); skin && j < skin->Palette.size())
                world =
                    GetSpringBoneRenderTransform(root) * skin->Palette[j] * glm::inverse(data.Joints[j].InverseBind);
            else if (j < pose->Model.size())
                world = GetSpringBoneRenderTransform(root) * pose->Model[j];
            else
                return false;
        }
        world = world * glm::translate(glm::mat4(1), source.Offset);
        return Finite(glm::vec3(world[3]));
    }
    bool Scene::GetAimConstraintTarget(Entity e, glm::vec3& target) const
    {
        if (!e || !e.HasComponent<AimConstraintComponent>())
            return false;
        double     total = 0;
        glm::dvec3 position(0);
        for (const auto& source : e.GetComponent<AimConstraintComponent>().Sources)
        {
            glm::mat4 world;
            if (std::isfinite(source.Weight) && source.Weight > 0 && ResolveConstraintSource(source, world))
            {
                position += glm::dvec3(world[3]) * double(source.Weight);
                total += source.Weight;
            }
        }
        if (total <= 0)
            return false;
        target = glm::vec3(position / total);
        return Finite(target);
    }
    void Scene::ResetTransformConstraints()
    {
        for (const auto& [id, rotation] : m_ConstraintRotations)
            if (auto e = GetEntityByUUID(id))
                DirtySubtree(e);
        m_ConstraintRotations.clear();
        m_ResetConstraintSmoothing = true;
    }
    void Scene::UpdateTransformConstraints(double seconds)
    {
        m_ConstraintsVisible = true;
        std::vector<UUID>    owners;
        for (auto h : m_Registry.view<AimConstraintComponent, IDComponent>())
            owners.push_back(m_Registry.get<IDComponent>(h).ID);
        // Do not allocate a dependency traversal for scenes that do not use constraints.
        if (owners.empty() && m_ConstraintRotations.empty())
        {
            m_ConstraintWarnings.clear();
            m_ResetConstraintSmoothing = false;
            return;
        }
        auto& previous = m_PreviousConstraintRotations;
        previous.clear();
        previous.swap(m_ConstraintRotations);
        m_ConstraintVisits.clear();
        auto oldWarnings = std::move(m_ConstraintWarnings);
        m_ConstraintWarnings.clear();
        std::sort(owners.begin(), owners.end());
        // Parent edges matter even for unconstrained nodes: aiming at one's own child is a cycle.
        std::function<bool(Entity)> visit = [&](Entity e) -> bool {
            if (!e)
                return true;
            const auto id = e.GetUUID();
            const auto [state, inserted] = m_ConstraintVisits.try_emplace(id, ConstraintVisit::Visiting);
            if (!inserted)
            {
                if (state->second != ConstraintVisit::Visiting)
                    return state->second == ConstraintVisit::Valid;
                m_ConstraintWarnings.push_back("Transform constraint cycle at entity " + std::to_string(uint64_t(id)));
                return false;
            }
            bool ok = visit(GetParent(e));
            if (e.HasComponent<AimConstraintComponent>())
            {
                const auto& c = e.GetComponent<AimConstraintComponent>();
                if (c.Active && IsEntityActive(e) && c.Weight > 0)
                {
                    for (const auto& source : c.Sources)
                        if (source.Weight > 0)
                        {
                            ok = visit(GetEntityByUUID(source.Entity)) && ok;
                            // A named joint may itself have an entity constraint, independently of its model root.
                            if (!source.Joint.empty())
                            {
                                auto                     root = GetDeformationRoot(GetEntityByUUID(source.Entity));
                                Ref<const SkeletonAsset> skeleton;
                                const std::vector<UUID>* joints = nullptr;
                                if (root && root.HasComponent<AnimatorComponent>())
                                {
                                    const auto& a = root.GetComponent<AnimatorComponent>();
                                    skeleton      = a.SkeletonSource;
                                    joints        = &a.JointEntities;
                                }
                                else if (root && root.HasComponent<ModelPoseComponent>())
                                {
                                    const auto& a = root.GetComponent<ModelPoseComponent>();
                                    skeleton      = a.SkeletonSource;
                                    joints        = &a.JointEntities;
                                }
                                const auto index = skeleton ? JointIndex(skeleton->GetData(), source.Joint) : -1;
                                if (index >= 0 && joints && size_t(index) < joints->size())
                                    ok = visit(GetEntityByUUID((*joints)[size_t(index)])) && ok;
                            }
                        }
                    if (uint64_t(c.UpObject))
                        ok = visit(GetEntityByUUID(c.UpObject)) && ok;
                    if (ok)
                    {
                        glm::vec3   target;
                        const auto& t           = e.GetComponent<TransformComponent>();
                        const auto  parent      = GetParent(e);
                        const auto  parentWorld = parent ? GetSpringBoneRenderTransform(parent) : glm::mat4(1);
                        const auto  inverse     = glm::inverse(parentWorld);
                        auto        forward = ConstraintAxisVector(c.AimAxis), localUp = ConstraintAxisVector(c.UpAxis);
                        if (GetAimConstraintTarget(e, target) && glm::length(glm::cross(forward, localUp)) > 1e-6f)
                        {
                            auto direction = glm::vec3(inverse * glm::vec4(target, 1)) - t.Position;
                            auto up        = c.WorldUp;
                            if (auto object = GetEntityByUUID(c.UpObject))
                                up = glm::mat3(GetSpringBoneRenderTransform(object)) * up;
                            up = glm::mat3(inverse) * up;
                            if (Finite(direction) && Finite(up) && glm::length(direction) > 1e-6f)
                            {
                                direction = glm::normalize(direction);
                                up -= direction * glm::dot(up, direction);
                                if (glm::length(up) < 1e-6f)
                                {
                                    up = std::abs(direction.y) < .9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                                    up -= direction * glm::dot(up, direction);
                                }
                                up = glm::normalize(up);
                                const glm::mat3 localBasis(forward, localUp, glm::cross(forward, localUp));
                                const glm::mat3 targetBasis(direction, up, glm::cross(direction, up));
                                const auto      authored = glm::normalize(glm::quat(t.Rotation));
                                auto desired = glm::normalize(glm::quat_cast(targetBasis * glm::transpose(localBasis)));
                                if (c.LimitsEnabled)
                                {
                                    float pan, tilt, roll;
                                    glm::extractEulerAngleYXZ(
                                        glm::mat4_cast(glm::inverse(authored) * desired), pan, tilt, roll);
                                    auto delta = glm::clamp(
                                        glm::degrees(glm::vec3(tilt, pan, roll)), c.MinRotation, c.MaxRotation);
                                    delta   = glm::radians(delta);
                                    desired = authored * glm::quat_cast(glm::yawPitchRoll(delta.y, delta.x, delta.z));
                                }
                                desired = glm::normalize(glm::slerp(authored, desired, std::clamp(c.Weight, 0.f, 1.f)));
                                if (!m_ResetConstraintSmoothing && std::isfinite(seconds) && seconds > 0 &&
                                    std::isfinite(c.MaxAngularSpeed) && c.MaxAngularSpeed > 0)
                                {
                                    auto        last = previous.find(id);
                                    auto        from = last == previous.end() ? authored : last->second;
                                    const float angle =
                                        2.f * std::acos(std::clamp(std::abs(glm::dot(from, desired)), 0.f, 1.f));
                                    const float step = glm::radians(c.MaxAngularSpeed) * float(seconds);
                                    if (angle > step && angle > 1e-6f)
                                        desired = glm::normalize(glm::slerp(from, desired, step / angle));
                                }
                                m_ConstraintRotations[id] = desired;
                            }
                        }
                    }
                }
            }
            state->second = ok ? ConstraintVisit::Valid : ConstraintVisit::Invalid;
            return ok;
        };
        for (auto id : owners)
            visit(GetEntityByUUID(id));
        // Invalidate descendants as well, including light proxies parented below a moving head.
        for (const auto& [id, rotation] : previous)
            if (auto e = GetEntityByUUID(id))
                DirtySubtree(e);
        previous.clear();
        m_ConstraintVisits.clear();
        for (const auto& [id, rotation] : m_ConstraintRotations)
            if (auto e = GetEntityByUUID(id))
                DirtySubtree(e);
        if (oldWarnings != m_ConstraintWarnings)
            for (const auto& warning : m_ConstraintWarnings)
                CORE_WARN("{}", warning);
        m_ResetConstraintSmoothing = false;
    }
} // namespace Kans
