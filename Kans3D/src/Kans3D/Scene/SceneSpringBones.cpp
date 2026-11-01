#include "kspch.h"
#include "Kans3D/Animation/SpringBoneComponent.h"
#include "Kans3D/Physics/ClothSolver.h"
#include "Kans3D/Physics/ForceFields.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Renderer/Resource/Skinning.h"
#include "Entity.h"
#include "MotionUpdateCache.h"
#include "Scene.h"
namespace Kans
{

    namespace
    {
        glm::vec3 Unit(glm::vec3 v, glm::vec3 fallback = {0, 1, 0})
        {
            return glm::dot(v, v) > 1e-12f ? glm::normalize(v) : fallback;
        }
        glm::quat Rotation(const glm::mat4& m) { return JointTransform::FromMatrix(m).Rotation; }
        bool      SameCurve(const TimelineCurve& a, const TimelineCurve& b)
        {
            if (a.Keys.size() != b.Keys.size())
                return false;
            for (size_t i = 0; i < a.Keys.size(); ++i)
            {
                const auto& x = a.Keys[i];
                const auto& y = b.Keys[i];
                if (std::tie(x.Time, x.Value, x.InTangent, x.OutTangent, x.Interpolation) !=
                    std::tie(y.Time, y.Value, y.InTangent, y.OutTangent, y.Interpolation))
                    return false;
            }
            return true;
        }
        float Sample(const TimelineCurve& c, float t, float base) { return c.Sample(t, glm::vec4(base)).x; }
        struct Node : SpringBoneJointState
        {
            TransformComponent Animated, Applied;
            Entity             BoneEntity;
            glm::mat4          OldLocal {1}, Local {1}, World {1};
            glm::vec3          Tail {0, 1, 0};
            float              Coordinate          = 0;
            bool               Written             = false;
            uint64_t           LastImpulseSequence = 0;
        };
        struct Chain
        {
            std::vector<Node> Nodes;
            glm::mat4         OldParent {1}, Parent {1}, LastRoot {1}, SolvedRoot {1};
            UUID              Root = 0, End = 0;
            float             Extension = 0;
        };
        struct Collider
        {
            UUID                           ID = 0;
            glm::mat4                      Old {1}, Current {1}, Step {1};
            const MotionColliderComponent* Data = nullptr;
        };
    } // namespace
    struct SpringBoneRuntime
    {
        std::vector<Chain>                         Chains;
        std::vector<SpringBoneUpdateCache::Entry*> Path;
        std::vector<Collider>                      Colliders;
        Physics::SubstepClock                      Clock;
        glm::mat4                                  OldCenter {1}, Center {1};
        uint64_t                                   ResetEpoch = 0, LastImpulseSequence = 0;
        bool                                       ResetPending = true, Active = false;
    };
    bool operator==(const SpringBoneChain& a, const SpringBoneChain& b)
    {
        return std::tie(a.RootBone,
                        a.EndBone,
                        a.ExcludedBones,
                        a.EndExtension,
                        a.Stiffness,
                        a.Drag,
                        a.GravityPower,
                        a.GravityDirection,
                        a.HitRadius,
                        a.RotationAxis,
                        a.AngleLimit,
                        a.StiffnessMultiplier,
                        a.DragMultiplier,
                        a.GravityMultiplier) == std::tie(b.RootBone,
                                                         b.EndBone,
                                                         b.ExcludedBones,
                                                         b.EndExtension,
                                                         b.Stiffness,
                                                         b.Drag,
                                                         b.GravityPower,
                                                         b.GravityDirection,
                                                         b.HitRadius,
                                                         b.RotationAxis,
                                                         b.AngleLimit,
                                                         b.StiffnessMultiplier,
                                                         b.DragMultiplier,
                                                         b.GravityMultiplier) &&
               SameCurve(a.StiffnessCurve, b.StiffnessCurve) && SameCurve(a.DragCurve, b.DragCurve) &&
               SameCurve(a.GravityPowerCurve, b.GravityPowerCurve) &&
               SameCurve(a.GravityDirectionCurve, b.GravityDirectionCurve) &&
               SameCurve(a.HitRadiusCurve, b.HitRadiusCurve) && SameCurve(a.RotationAxisCurve, b.RotationAxisCurve) &&
               SameCurve(a.AngleLimitCurve, b.AngleLimitCurve);
    }
    bool operator==(const SpringBoneComponent& a, const SpringBoneComponent& b)
    {
        return std::tie(a.Chains,
                        a.Center,
                        a.InertiaScale,
                        a.WindScale,
                        a.ColliderGroups,
                        a.SubstepRate,
                        a.MaxSubsteps,
                        a.TeleportDistance,
                        a.TeleportRotation,
                        a.Enabled) == std::tie(b.Chains,
                                               b.Center,
                                               b.InertiaScale,
                                               b.WindScale,
                                               b.ColliderGroups,
                                               b.SubstepRate,
                                               b.MaxSubsteps,
                                               b.TeleportDistance,
                                               b.TeleportRotation,
                                               b.Enabled);
    }
    bool operator==(const MotionColliderComponent& a, const MotionColliderComponent& b)
    {
        if (a.Inside != b.Inside || a.Shape.index() != b.Shape.index())
            return false;
        return std::visit(
            [&](const auto& x) {
                using T       = std::decay_t<decltype(x)>;
                const auto& y = std::get<T>(b.Shape);
                if constexpr (std::is_same_v<T, Physics::Plane>)
                    return x.Normal == y.Normal && x.Offset == y.Offset;
                else
                    return x == y;
            },
            a.Shape);
    }
    void SpringBoneComponent::Reset()
    {
        if (Runtime)
            Runtime->ResetPending = true;
    }
    void SpringBoneComponent::RemapEntities(const std::function<uint64_t(uint64_t)>& remap)
    {
        Center = remap(Center);
        for (auto& id : ColliderGroups)
            id = remap(id);
        for (auto& c : Chains)
        {
            c.RootBone = remap(c.RootBone);
            c.EndBone  = remap(c.EndBone);
            for (auto& id : c.ExcludedBones)
                id = remap(id);
        }
        Reset();
    }
    void Scene::OnSpringBoneRemoved(entt::registry&, entt::entity)
    {
        if (!m_SuppressNotifications)
            RestoreSpringBoneTransforms();
        m_SpringRenderRotations.clear();
    }
    glm::mat4 Scene::SpringBoneRenderLocal(Entity e) const
    {
        const auto& t = e.GetComponent<TransformComponent>();
        if (!m_SpringBonePreview || m_PhysicsWorld)
            return t.GetTransform();
        const auto found =
            std::lower_bound(m_SpringRenderRotations.begin(),
                             m_SpringRenderRotations.end(),
                             e.GetUUID(),
                             [](const auto& entry, UUID id) { return uint64_t(entry.first) < uint64_t(id); });
        if (found == m_SpringRenderRotations.end() || found->first != e.GetUUID())
            return t.GetTransform();
        JointTransform local;
        local.Translation = t.Position;
        local.Scale       = t.Scale;
        local.Rotation    = found->second;
        return local.Matrix();
    }
    glm::mat4 Scene::GetSpringBoneRenderTransform(Entity e) const
    {
        if (!m_SpringBonePreview || m_PhysicsWorld || m_SpringRenderRotations.empty())
            return GetWorldTransform(e);
        if (e.HasComponent<PrefabOwnedComponent>())
        {
            auto root     = GetPrefabInstanceRoot(e);
            auto relative = [&](auto&& self, Entity part) -> glm::mat4 {
                auto       parent = GetParent(part);
                const auto local  = SpringBoneRenderLocal(part);
                return parent && parent != root ? self(self, parent) * local : local;
            };
            return root ? GetSpringBoneRenderTransform(root) * relative(relative, e) : SpringBoneRenderLocal(e);
        }
        auto parent = GetParent(e);
        return parent ? GetSpringBoneRenderTransform(parent) * SpringBoneRenderLocal(e) : SpringBoneRenderLocal(e);
    }
    void Scene::ResetSecondaryMotion()
    {
        RestoreSpringBoneTransforms();
        m_SpringRenderRotations.clear();
        ++m_SecondaryMotionResetEpoch;
        ClearClothStreams();
    }
    void Scene::SetSpringBonePreviewEnabled(bool value)
    {
        RestoreSpringBoneTransforms();
        m_SpringBonePreview = value;
        if (!value)
            ClearClothStreams();
        m_SpringRenderRotations.clear();
        ResetSecondaryMotion();
        if (!value)
        {
            UpdateAnimations(0);
            UpdateTimelines(0);
            for (auto h : m_Registry.view<AnimatorComponent>())
            {
                auto& a = m_Registry.get<AnimatorComponent>(h);
                if (a.SkeletonSource && a.SkinPose)
                    ApplyModelPose({h, this},
                                   a.SkeletonSource->GetData(),
                                   a.PreviewPose,
                                   a.JointEntities,
                                   a.SkinPose,
                                   a.PoseVersion);
            }
            for (auto h : m_Registry.view<SpringBoneComponent>())
                if (auto runtime = m_Registry.get<SpringBoneComponent>(h).Runtime)
                    for (const auto& chain : runtime->Chains)
                        if (auto root = GetEntityByUUID(chain.Root))
                            DirtySubtree(root);
            m_RenderWorld->Synchronize();
        }
    }
    const SpringBoneJointState* Scene::GetSpringBoneJointState(Entity owner, size_t chain, size_t joint) const
    {
        if (!owner || !owner.HasComponent<SpringBoneComponent>())
            return nullptr;
        const auto& r = owner.GetComponent<SpringBoneComponent>().Runtime;
        if (!r || chain >= r->Chains.size() || joint >= r->Chains[chain].Nodes.size())
            return nullptr;
        return &r->Chains[chain].Nodes[joint];
    }
    double Scene::GetSpringBoneInterpolationAlpha(Entity owner) const
    {
        if (!owner || !owner.HasComponent<SpringBoneComponent>())
            return 0;
        const auto& c = owner.GetComponent<SpringBoneComponent>();
        return c.Runtime ? c.Runtime->Clock.Alpha(1. / std::max(c.SubstepRate, 1.f)) : 0;
    }
    uint64_t Scene::GetSpringBoneSubstepCount(Entity owner) const
    {
        if (!owner || !owner.HasComponent<SpringBoneComponent>())
            return 0;
        const auto& r = owner.GetComponent<SpringBoneComponent>().Runtime;
        return r ? r->Clock.Steps : 0;
    }
    namespace
    {
        void RestoreSpringTransforms(Scene& scene, SpringBoneRuntime& runtime)
        {
            for (auto& chain : runtime.Chains)
                for (auto& n : chain.Nodes)
                    if (n.Written)
                    {
                        if (auto e = scene.GetEntityByUUID(n.Bone); e && e.HasComponent<TransformComponent>())
                        {
                            auto& t = e.GetComponent<TransformComponent>();
                            if (t.Rotation == n.Applied.Rotation)
                            {
                                t.Rotation         = n.Animated.Rotation;
                                t.ImportedMatrix   = n.Animated.ImportedMatrix;
                                t.ImportedRotation = n.Animated.ImportedRotation;
                            }
                        }
                        n.Written = false;
                    }
        }
    } // namespace
    void Scene::RestoreSpringBoneTransforms()
    {
        for (auto h : m_Registry.view<SpringBoneComponent>())
            if (auto r = m_Registry.get<SpringBoneComponent>(h).Runtime)
                RestoreSpringTransforms(*this, *r);
    }
    void Scene::ResetSecondaryMotion(Entity scope)
    {
        if (!scope)
            return;
        ResetClothMotion(scope);
        auto within = [&](Entity e) { return e && (e == scope || IsDescendantOf(e, scope)); };
        for (auto h : m_Registry.view<SpringBoneComponent>())
        {
            auto& c = m_Registry.get<SpringBoneComponent>(h);
            if (!within({h, this}) && !std::any_of(c.Chains.begin(), c.Chains.end(), [&](const auto& chain) {
                    return within(GetEntityByUUID(chain.RootBone));
                }))
                continue;
            c.Reset();
            if (auto r = c.Runtime)
            {
                RestoreSpringTransforms(*this, *r);
                m_SpringRenderRotations.erase(std::remove_if(m_SpringRenderRotations.begin(),
                                                             m_SpringRenderRotations.end(),
                                                             [&](const auto& rotation) {
                                                                 for (const auto& chain : r->Chains)
                                                                     for (const auto& n : chain.Nodes)
                                                                         if (n.Bone == rotation.first)
                                                                             return true;
                                                                 return false;
                                                             }),
                                              m_SpringRenderRotations.end());
            }
        }
    }
    void Scene::GatherForceFields(ForceFieldSet& fields) const
    {
        fields.Clear();
        if (m_PhysicsWorld)
            fields = m_PhysicsWorld->GetForceFields();
        else
        {
            for (auto h : m_Registry.view<WindZoneComponent>())
            {
                Entity e {h, const_cast<Scene*>(this)};
                if (IsEntityActive(e))
                    fields.AddWind(e.GetComponent<WindZoneComponent>(), GetWorldTransform(e));
            }
            for (auto h : m_Registry.view<ForceFieldComponent>())
            {
                Entity e {h, const_cast<Scene*>(this)};
                if (IsEntityActive(e))
                    fields.AddField(e.GetComponent<ForceFieldComponent>(), GetWorldTransform(e));
            }
        }
    }
    void Scene::PrepareMotionSnapshot()
    {
        if (!m_SpringBoneUpdate)
            m_SpringBoneUpdate = CreateRef<SpringBoneUpdateCache>(*this);
        auto& snapshot        = *m_SpringBoneUpdate;
        m_MotionSnapshotReady = true;
        snapshot.Begin();
        GatherForceFields(snapshot.Fields);
        for (auto h : m_Registry.view<MotionColliderComponent>())
        {
            Entity e {h, this};
            auto&  entry = snapshot.Get(e.GetUUID(), e);
            if (!snapshot.Active(entry))
                continue;
            entry.ColliderIndex = uint32_t(snapshot.Colliders.size());
            snapshot.Colliders.push_back({entry.ID, e.GetComponent<MotionColliderComponent>(), &entry});
        }
    }
    void Scene::UpdateSpringBones(double seconds)
    {
        const bool playing = bool(m_PhysicsWorld), preview = !playing && m_SpringBonePreview;
        if ((!playing && !preview) ||
            (m_Registry.view<SpringBoneComponent>().empty() && m_Registry.view<ClothComponent>().empty()))
            return;
        m_SpringRenderRotations.clear();
        PrepareMotionSnapshot();
        auto&      snapshot = *m_SpringBoneUpdate;
        const bool paused   = playing && m_PhysicsPaused && !m_PhysicsSingleStep;
        for (auto h : m_Registry.view<SpringBoneComponent>())
        {
            Entity owner {h, this};
            auto&  c = owner.GetComponent<SpringBoneComponent>();
            if (!c.Runtime)
                c.Runtime = CreateRef<SpringBoneRuntime>();
            auto& r = *c.Runtime;
            if (!c.Enabled || !snapshot.Active(snapshot.Get(owner.GetUUID(), owner)))
            {
                r.Active = false;
                continue;
            }
            bool reset     = r.ResetPending || !r.Active || r.ResetEpoch != m_SecondaryMotionResetEpoch;
            r.Active       = true;
            r.ResetPending = false;
            r.ResetEpoch   = m_SecondaryMotionResetEpoch;
            r.OldCenter    = r.Center;
            auto& center   = snapshot.Get(c.Center);
            r.Center       = center.Value ? snapshot.World(center) : glm::mat4(1);
            if (r.Chains.size() != c.Chains.size())
            {
                r.Chains.resize(c.Chains.size());
                reset = true;
            }
            for (size_t ci = 0; ci < c.Chains.size(); ++ci)
            {
                const auto& setting = c.Chains[ci];
                auto&       chain   = r.Chains[ci];
                r.Path.clear();
                auto& root = snapshot.Get(setting.RootBone);
                auto& end  = snapshot.Get(setting.EndBone);
                if (root.Value)
                {
                    if (setting.EndBone != UUID(0))
                    {
                        for (auto* e = &end; e->Value; e = &snapshot.Get(e->Parent))
                        {
                            r.Path.push_back(e);
                            if (e == &root)
                                break;
                        }
                        if (r.Path.empty() || r.Path.back() != &root)
                            r.Path.clear();
                        std::reverse(r.Path.begin(), r.Path.end());
                    }
                    else
                        for (auto* e = &root; e->Value;)
                        {
                            r.Path.push_back(e);
                            const auto& children = e->Value.GetComponent<RelationshipComponent>().Children;
                            e = children.empty() ? &snapshot.Get(UUID(0)) : &snapshot.Get(children.front());
                        }
                }
                for (size_t i = 0; i < r.Path.size(); ++i)
                    if (std::find(setting.ExcludedBones.begin(), setting.ExcludedBones.end(), r.Path[i]->ID) !=
                        setting.ExcludedBones.end())
                    {
                        r.Path.resize(i);
                        break;
                    }
                const size_t count = r.Path.empty() ? 0 : r.Path.size() - (setting.EndExtension > 0 ? 0 : 1);
                if (chain.Nodes.size() != count || chain.Root != setting.RootBone || chain.End != setting.EndBone ||
                    chain.Extension != setting.EndExtension)
                    reset = true;
                chain.Root      = setting.RootBone;
                chain.End       = setting.EndBone;
                chain.Extension = setting.EndExtension;
                chain.Nodes.resize(count);
                chain.OldParent = chain.Parent;
                auto& parent    = snapshot.Get(root.Parent);
                chain.Parent    = parent.Value ? snapshot.World(parent) : glm::mat4(1);
                for (size_t i = 0; i < count; ++i)
                {
                    auto& n = chain.Nodes[i];
                    if (n.Bone != r.Path[i]->ID)
                        reset = true;
                    n.Bone       = r.Path[i]->ID;
                    n.BoneEntity = r.Path[i]->Value;
                    n.OldLocal   = n.Local;
                    n.Animated   = n.BoneEntity.GetComponent<TransformComponent>();
                    n.Local      = snapshot.Local(*r.Path[i]);
                    n.Coordinate = count > 1 ? float(i) / float(count - 1) : 0;
                    if (i + 1 < r.Path.size())
                        n.Tail = r.Path[i + 1]->Value.GetComponent<TransformComponent>().Position;
                    else
                        n.Tail = Unit(i ? n.Animated.Position : glm::vec3(0, 1, 0)) * setting.EndExtension;
                }
                const auto rootWorld = root.Value ? snapshot.World(root) : glm::mat4(1);
                if (!reset &&
                    ((c.TeleportDistance > 0 &&
                      glm::length(glm::vec3(rootWorld[3] - chain.LastRoot[3])) > c.TeleportDistance) ||
                     (c.TeleportRotation > 0 &&
                      glm::degrees(2 *
                                   std::acos(std::clamp(
                                       std::abs(glm::dot(Rotation(rootWorld), Rotation(chain.LastRoot))), 0.f, 1.f))) >
                          c.TeleportRotation)))
                    reset = true;
                chain.LastRoot = rootWorld;
            }
            snapshot.ColliderUnion.clear();
            for (auto id : c.ColliderGroups)
            {
                const auto& members = snapshot.Gather(id);
                snapshot.ColliderUnion.insert(snapshot.ColliderUnion.end(), members.begin(), members.end());
            }
            std::sort(snapshot.ColliderUnion.begin(), snapshot.ColliderUnion.end());
            snapshot.ColliderUnion.erase(std::unique(snapshot.ColliderUnion.begin(), snapshot.ColliderUnion.end()),
                                         snapshot.ColliderUnion.end());
            size_t colliderCount = 0;
            for (auto index : snapshot.ColliderUnion)
            {
                auto& shared = snapshot.Colliders[index];
                if (!shared.WorldReady)
                {
                    shared.World      = snapshot.World(*shared.Transform);
                    shared.WorldReady = true;
                }
                if (colliderCount == r.Colliders.size())
                    r.Colliders.emplace_back();
                auto& collider   = r.Colliders[colliderCount++];
                collider.Old     = collider.ID == shared.ID ? collider.Current : shared.World;
                collider.Current = shared.World;
                collider.ID      = shared.ID;
                collider.Data    = &shared.Data;
            }
            r.Colliders.resize(colliderCount);
            const double step = 1. / std::clamp(c.SubstepRate, 1.f, 1000.f);
            if (reset)
            {
                r.Clock.Reset();
                r.OldCenter = r.Center;
                for (auto& chain : r.Chains)
                {
                    chain.OldParent  = chain.Parent;
                    auto parent      = glm::inverse(r.Center) * chain.Parent;
                    chain.SolvedRoot = parent * (chain.Nodes.empty() ? glm::mat4(1) : chain.Nodes[0].Local);
                    for (auto& n : chain.Nodes)
                    {
                        n.OldLocal    = n.Local;
                        n.World       = parent * n.Local;
                        parent        = n.World;
                        n.PreviousTip = n.Tip = glm::vec3(n.World * glm::vec4(n.Tail, 1));
                        n.PreviousRotation = n.Rotation = n.RenderRotation = Rotation(n.Local);
                    }
                }
                for (auto& col : r.Colliders)
                    col.Old = col.Current;
            }
            const double   oldRemainder = r.Clock.Remainder;
            const double   frame        = std::isfinite(seconds) && seconds > 0 ? seconds : 0;
            const uint32_t steps = (reset || paused) ? 0 : r.Clock.Accumulate(frame, step, std::max(1u, c.MaxSubsteps));
            auto           inverseCenter = glm::inverse(r.Center);
            for (uint32_t s = 0; s < steps; ++s)
            {
                const float t = frame > 0 ? float(std::clamp(((s + 1) * step - oldRemainder) / frame, 0., 1.)) : 1;
                const auto  centerStep = Physics::InterpolateMotionTransform(r.OldCenter, r.Center, t);
                inverseCenter          = glm::inverse(centerStep);
                for (auto& col : r.Colliders)
                    col.Step = Physics::InterpolateMotionTransform(col.Old, col.Current, t);
                for (size_t ci = 0; ci < r.Chains.size(); ++ci)
                {
                    auto&       chain   = r.Chains[ci];
                    const auto& setting = c.Chains[ci];
                    auto parent = inverseCenter * Physics::InterpolateMotionTransform(chain.OldParent, chain.Parent, t);
                    // Transport by root motion in reference space only. Center motion is already removed.
                    const auto oldRoot = chain.SolvedRoot;
                    const auto rootLocal =
                        chain.Nodes.empty() ?
                            glm::mat4(1) :
                            Physics::InterpolateMotionTransform(chain.Nodes[0].OldLocal, chain.Nodes[0].Local, t);
                    const auto newRoot   = parent * rootLocal;
                    const auto transport = newRoot * glm::inverse(oldRoot);
                    chain.SolvedRoot     = newRoot;
                    for (auto& n : chain.Nodes)
                    {
                        const auto local = JointTransform::FromMatrix(
                            &n == &chain.Nodes.front() ? rootLocal :
                                                         Physics::InterpolateMotionTransform(n.OldLocal, n.Local, t));
                        n.World            = parent * local.Matrix();
                        const auto  origin = glm::vec3(n.World[3]);
                        const auto  target = glm::vec3(n.World * glm::vec4(n.Tail, 1));
                        const float length = glm::length(target - origin), u = n.Coordinate;
                        const auto  direction = Unit(target - origin);
                        const float inertia   = std::clamp(c.InertiaScale, 0.f, 1.f);
                        auto        tip       = glm::mix(glm::vec3(transport * glm::vec4(n.Tip, 1)), n.Tip, inertia);
                        auto        previous =
                            glm::mix(glm::vec3(transport * glm::vec4(n.PreviousTip, 1)), n.PreviousTip, inertia);
                        const float drag =
                            std::clamp(Sample(setting.DragCurve, u, setting.Drag) * setting.DragMultiplier, 0.f, 1.f);
                        auto gravity = Unit(
                            glm::vec3(setting.GravityDirectionCurve.Sample(u, glm::vec4(setting.GravityDirection, 0))),
                            glm::vec3(0));
                        gravity *= std::max(0.f, Sample(setting.GravityPowerCurve, u, setting.GravityPower)) *
                                   setting.GravityMultiplier;
                        const auto worldTip = glm::vec3(centerStep * glm::vec4(tip, 1));
                        const auto sample =
                            snapshot.Fields.Sample(worldTip,
                                                   glm::mat3(centerStep) * (tip - previous) / float(step),
                                                   r.Clock.Time,
                                                   0,
                                                   FieldSystem::SpringBones);
                        auto acceleration = glm::mat3(inverseCenter) *
                                            (gravity + c.WindScale * sample.Wind + sample.Acceleration + sample.Force);
                        for (const auto& impulse : snapshot.Fields.GetImpulses())
                            if (!impulse.Sequence || impulse.Sequence > n.LastImpulseSequence)
                            {
                                previous -= glm::mat3(inverseCenter) *
                                            impulse.Sample(worldTip, 0, FieldSystem::SpringBones) * float(step);
                                n.LastImpulseSequence = std::max(n.LastImpulseSequence, impulse.Sequence);
                            }
                        const float stiffness = std::max(
                            0.f, Sample(setting.StiffnessCurve, u, setting.Stiffness) * setting.StiffnessMultiplier);
                        auto        candidate = tip + (tip - previous) * std::pow(1 - drag, float(step * 60)) +
                                                ((target - tip) * stiffness + acceleration) * float(step * step);
                        const float hit       = std::max(0.f, Sample(setting.HitRadiusCurve, u, setting.HitRadius));
                        const int   axis      = std::clamp(
                            int(std::lround(Sample(setting.RotationAxisCurve, u, float(setting.RotationAxis)))), 0, 3);
                        const float limit = glm::radians(
                            std::clamp(Sample(setting.AngleLimitCurve, u, setting.AngleLimit), 0.f, 180.f));
                        glm::quat  delta {1, 0, 0, 0};
                        const auto worldRotation = Rotation(n.World);
                        const auto invRotation   = glm::inverse(worldRotation);
                        const auto from          = Unit(invRotation * direction);
                        // Alternating projections converge length/colliders/hinge/cone together. Infeasible constraints
                        // retain the angular constraint and length; authoring must provide a feasible volume.
                        for (int iteration = 0; iteration < (r.Colliders.empty() ? 1 : 16); ++iteration)
                        {
                            candidate = origin + Unit(candidate - origin, direction) * length;
                            for (const auto& col : r.Colliders)
                            {
                                const auto& collider   = *col.Data;
                                auto        worldPoint = glm::vec3(centerStep * glm::vec4(candidate, 1));
                                worldPoint             = Physics::ProjectMotionParticle(
                                    collider.Shape, col.Step, collider.Inside, worldPoint, hit);
                                candidate = glm::vec3(inverseCenter * glm::vec4(worldPoint, 1));
                            }
                            const auto to = Unit(invRotation * Unit(candidate - origin, direction));
                            if (axis)
                            {
                                glm::vec3 hinge(0);
                                hinge[axis - 1]  = 1;
                                const auto f     = from - hinge * glm::dot(from, hinge),
                                           v     = to - hinge * glm::dot(to, hinge);
                                float      angle = glm::dot(f, f) > 1e-12f && glm::dot(v, v) > 1e-12f ?
                                                       std::atan2(glm::dot(hinge, glm::cross(f, v)), glm::dot(f, v)) :
                                                       0;
                                delta            = glm::angleAxis(std::clamp(angle, -limit, limit), hinge);
                            }
                            else
                            {
                                delta             = glm::rotation(from, to);
                                const float angle = 2 * std::acos(std::clamp(std::abs(delta.w), 0.f, 1.f));
                                if (angle > limit && angle > 1e-6f)
                                    delta = glm::slerp(glm::quat(1, 0, 0, 0), delta, limit / angle);
                            }
                            candidate = origin + (worldRotation * delta * from) * length;
                        }
                        n.PreviousTip      = tip;
                        n.Tip              = candidate;
                        n.PreviousRotation = n.Rotation;
                        n.Rotation         = glm::normalize(local.Rotation * delta);
                        auto solved        = local;
                        solved.Rotation    = n.Rotation;
                        n.World            = parent * solved.Matrix();
                        parent             = n.World;
                    }
                }
                ++r.Clock.Steps;
                r.Clock.Time += step;
            }
            const float alpha = reset || m_PhysicsSingleStep ? 1.f : float(r.Clock.Alpha(step));
            for (auto& chain : r.Chains)
                for (auto& n : chain.Nodes)
                {
                    // Keep the last rendered pose while paused, including a single step rendered at alpha=1.
                    if (!paused || reset)
                        n.RenderRotation = glm::slerp(n.PreviousRotation, n.Rotation, alpha);
                    m_SpringRenderRotations.emplace_back(n.Bone, n.RenderRotation);
                }
        }
        std::sort(m_SpringRenderRotations.begin(), m_SpringRenderRotations.end(), [](const auto& a, const auto& b) {
            return uint64_t(a.first) < uint64_t(b.first);
        });
        if (!preview)
            for (auto h : m_Registry.view<SpringBoneComponent>())
            {
                auto& c = m_Registry.get<SpringBoneComponent>(h);
                if (!c.Enabled || !c.Runtime || !c.Runtime->Active)
                    continue;
                for (auto& chain : c.Runtime->Chains)
                    for (auto& n : chain.Nodes)
                        if (auto bone = n.BoneEntity)
                        {
                            snapshot.Get(n.Bone, bone).Simulated = true;
                            auto& transform                      = bone.GetComponent<TransformComponent>();
                            transform.Rotation                   = glm::eulerAngles(n.RenderRotation);
                            n.Applied                            = transform;
                            n.Written                            = true;
                        }
            }
        auto renderLocal = [&](SpringBoneUpdateCache::Entry& entry) -> const glm::mat4& {
            if (!(entry.Flags & SpringBoneUpdateCache::RenderLocalReady))
            {
                if (!preview)
                    entry.RenderLocal = entry.Simulated ?
                                            entry.Value.GetComponent<TransformComponent>().GetTransform() :
                                            snapshot.Local(entry);
                else
                {
                    auto found = std::lower_bound(
                        m_SpringRenderRotations.begin(),
                        m_SpringRenderRotations.end(),
                        entry.ID,
                        [](const auto& rotation, UUID id) { return uint64_t(rotation.first) < uint64_t(id); });
                    if (found == m_SpringRenderRotations.end() || found->first != entry.ID)
                        entry.RenderLocal = snapshot.Local(entry);
                    else
                    {
                        const auto&    t = entry.Value.GetComponent<TransformComponent>();
                        JointTransform local;
                        local.Translation = t.Position;
                        local.Scale       = t.Scale;
                        local.Rotation    = found->second;
                        entry.RenderLocal = local.Matrix();
                    }
                }
                entry.Flags |= SpringBoneUpdateCache::RenderLocalReady;
            }
            return entry.RenderLocal;
        };
        auto refresh = [&](auto&& self, Entity e, const glm::mat4& world) -> Bounds {
            Bounds     bounds;
            const auto record = m_RenderWorld->m_Records.find(e);
            if (record != m_RenderWorld->m_Records.end())
            {
                auto&      r       = record->second;
                const bool skinned = r.MeshSource && !r.MeshSource->GetData().Skin.empty() && GetDeformationRoot(e);
                if (!skinned)
                {
                    if (r.Mesh)
                    {
                        r.Mesh->Transform  = world;
                        r.OutlineTransform = world * glm::scale(glm::mat4(1), glm::vec3(1.007f));
                    }
                    for (auto& draw : r.Draws)
                    {
                        draw.Transform   = world;
                        draw.WorldBounds = draw.LocalBounds.Transformed(world);
                    }
                }
                if (r.Mesh && e.HasComponent<PrefabOwnedComponent>())
                {
                    auto& entry = snapshot.Get(e.GetUUID(), e);
                    auto& root  = snapshot.Get(entry.PrefabRoot);
                    if (root.Value && !skinned)
                        r.OutlineTransform = snapshot.RenderWorld(root, renderLocal) *
                                             glm::scale(glm::mat4(1), glm::vec3(1.007f)) *
                                             snapshot.RenderRelative(entry, entry.PrefabRoot, renderLocal);
                }
                for (const auto& draw : r.Draws)
                    bounds.Expand(draw.WorldBounds);
                if (r.Sprite)
                    r.Sprite->Transform = world;
                if (r.Point)
                    r.Point->Position = glm::vec3(world[3]);
            }
            for (auto id : e.GetComponent<RelationshipComponent>().Children)
                if (auto child = snapshot.Get(id).Value)
                {
                    auto&      entry       = snapshot.Get(id, child);
                    const auto childBounds = self(self, child, snapshot.RenderWorld(entry, renderLocal));
                    m_ChildBounds[entt::entity(e)].Set(child, childBounds);
                    bounds.Expand(childBounds);
                }
            e.GetComponent<BoundsComponent>().World = bounds;
            return bounds;
        };
        for (auto h : m_Registry.view<SpringBoneComponent>())
        {
            const auto& c = m_Registry.get<SpringBoneComponent>(h);
            if (!c.Runtime)
                continue;
            for (const auto& chain : c.Runtime->Chains)
                if (!chain.Nodes.empty())
                    if (auto root = snapshot.Get(chain.Root).Value)
                    {
                        auto bounds =
                            refresh(refresh, root, snapshot.RenderWorld(snapshot.Get(chain.Root, root), renderLocal));
                        for (auto child = root, parent = snapshot.Get(snapshot.Get(chain.Root).Parent).Value; parent;
                             child = parent, parent = snapshot.Get(snapshot.Get(parent.GetUUID(), parent).Parent).Value)
                        {
                            m_ChildBounds[entt::entity(parent)].Set(child, bounds);
                            bounds = m_ChildBounds[entt::entity(parent)].Aggregate();
                            for (const auto& draw : m_RenderWorld->GetEntityDraws(parent))
                                bounds.Expand(draw.WorldBounds);
                            parent.GetComponent<BoundsComponent>().World = bounds;
                        }
                    }
        }
        // Rebuild the final palette after every modifier, including timeline-driven IK.
        auto finish = [&](Entity                   root,
                          const SkeletonData&      skeleton,
                          const AnimationPose&     animated,
                          AnimationPose&           render,
                          const std::vector<UUID>& joints,
                          Ref<SkinningPose>&       skin,
                          uint64_t&                version) {
            const auto findRotation = [&](UUID id) {
                return std::lower_bound(
                    m_SpringRenderRotations.begin(),
                    m_SpringRenderRotations.end(),
                    id,
                    [](const auto& entry, UUID key) { return uint64_t(entry.first) < uint64_t(key); });
            };
            const bool changed = std::any_of(joints.begin(), joints.end(), [&](UUID id) {
                auto found = findRotation(id);
                return found != m_SpringRenderRotations.end() && found->first == id;
            });
            if (!changed)
                return;
            render = animated;
            for (size_t j = 0; j < joints.size() && j < render.Local.size(); ++j)
            {
                auto found = findRotation(joints[j]);
                if (found != m_SpringRenderRotations.end() && found->first == joints[j])
                    render.Local[j].Rotation = found->second;
            }
            if (changed)
            {
                BuildModelPose(skeleton, render);
                ApplyModelPose(root, skeleton, render, joints, skin, version, !preview);
            }
        };
        for (auto h : m_Registry.view<AnimatorComponent>())
        {
            auto& a = m_Registry.get<AnimatorComponent>(h);
            if (a.SkeletonSource && a.SkinPose)
                finish({h, this},
                       a.SkeletonSource->GetData(),
                       a.PreviewPose,
                       a.SpringPose,
                       a.JointEntities,
                       a.SkinPose,
                       a.PoseVersion);
        }
        for (auto h : m_Registry.view<ModelPoseComponent>())
        {
            auto& a = m_Registry.get<ModelPoseComponent>(h);
            if (a.SkeletonSource && a.SkinPose)
                finish({h, this},
                       a.SkeletonSource->GetData(),
                       a.Pose,
                       a.SpringPose,
                       a.JointEntities,
                       a.SkinPose,
                       a.PoseVersion);
        }
        // Cloth runs after the final bone pose. Reuse the same frame snapshot, with collider worlds refreshed
        // through the memoized rendered hierarchy so bone-attached collision shapes follow spring modifiers too.
        if (!m_Registry.view<ClothComponent>().empty())
            for (auto& collider : snapshot.Colliders)
            {
                collider.World      = snapshot.RenderWorld(*collider.Transform, renderLocal);
                collider.WorldReady = true;
            }
    }
} // namespace Kans
