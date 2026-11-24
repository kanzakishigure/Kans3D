#include "kspch.h"
#include "DeformationComponents.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "FootPlacement.h"
namespace Kans
{
    namespace
    {
        bool Same(const IKTarget& a, const IKTarget& b)
        {
            return a.Entity == b.Entity && a.Position == b.Position && a.Rotation == b.Rotation && a.Joint == b.Joint;
        }
        bool Same(const TwoBoneIK& a, const TwoBoneIK& b)
        {
            return a.Root == b.Root && a.Middle == b.Middle && a.End == b.End && Same(a.Target, b.Target) &&
                   a.PoleEntity == b.PoleEntity && a.PoleDirection == b.PoleDirection && a.Weight == b.Weight &&
                   a.RotationWeight == b.RotationWeight && a.Stretch == b.Stretch;
        }
        bool Same(const LookAtIK& a, const LookAtIK& b)
        {
            if (!Same(a.Target, b.Target) || a.WorldUp != b.WorldUp || a.Weight != b.Weight ||
                a.AngleLimit != b.AngleLimit || a.Bones.size() != b.Bones.size())
                return false;
            for (size_t i = 0; i < a.Bones.size(); ++i)
            {
                const auto& x = a.Bones[i];
                const auto& y = b.Bones[i];
                if (x.Joint != y.Joint || x.Weight != y.Weight || x.Forward != y.Forward || x.Up != y.Up ||
                    x.AimChild != y.AimChild)
                    return false;
            }
            return true;
        }
        bool Same(const CopyTransformIK& a, const CopyTransformIK& b)
        {
            return a.Source == b.Source && a.Joint == b.Joint && a.CopyRotation == b.CopyRotation &&
                   a.CopyLocation == b.CopyLocation && a.WorldSpace == b.WorldSpace && a.Weight == b.Weight;
        }
        bool Same(const FootPlacementIK& a, const FootPlacementIK& b)
        {
            return a.Legs.size() == b.Legs.size() && a.Pelvis == b.Pelvis && a.GroundLayerMask == b.GroundLayerMask &&
                   a.RayStartHeight == b.RayStartHeight && a.MaxStepDown == b.MaxStepDown &&
                   a.ContactBand == b.ContactBand && a.PelvisAdjustSpeed == b.PelvisAdjustSpeed &&
                   a.PelvisMaxLower == b.PelvisMaxLower && a.PelvisMaxRaise == b.PelvisMaxRaise &&
                   a.RotationWeight == b.RotationWeight && a.RotationLimit == b.RotationLimit &&
                   a.Locking == b.Locking && a.LockYaw == b.LockYaw && a.LockSpeed == b.LockSpeed &&
                   a.LockMaxDistance == b.LockMaxDistance && a.ContactSource == b.ContactSource &&
                   a.TrackMaxReach == b.TrackMaxReach && a.ReleaseTime == b.ReleaseTime && a.Weight == b.Weight &&
                   std::equal(a.Legs.begin(), a.Legs.end(), b.Legs.begin(), [](const auto& x, const auto& y) {
                       return x.Hip == y.Hip && x.Knee == y.Knee && x.Ankle == y.Ankle && x.Toe == y.Toe &&
                              x.DrivenConstraint == y.DrivenConstraint && x.FootHeight == y.FootHeight &&
                              x.FootLength == y.FootLength && x.PoleEntity == y.PoleEntity &&
                              x.PoleDirection == y.PoleDirection && x.LockWeight == y.LockWeight;
                   });
        }
        bool Same(const std::vector<IKConstraint>& a, const std::vector<IKConstraint>& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                const auto& x = a[i];
                const auto& y = b[i];
                if (x.Name != y.Name || x.Enabled != y.Enabled || x.Weight != y.Weight ||
                    x.Data.index() != y.Data.index())
                    return false;
                if (!std::visit(
                        [&](const auto& data) { return Same(data, std::get<std::decay_t<decltype(data)>>(y.Data)); },
                        x.Data))
                    return false;
            }
            return true;
        }
    } // namespace
    struct IKRuntime
    {
        struct TargetOverrides
        {
            TwoBoneIK Limb;
            IKTarget  Aim;
            bool      HasLimb = false, HasAim = false;
        };

        const SkeletonData*         Skeleton = nullptr;
        std::vector<IKConstraint>   Constraints;
        std::vector<Animator>       Solvers;
        std::vector<JointTransform> Before;
        AnimationPose                      Animated, SceneInput;
        std::vector<FootPlacementState>    Feet;
        std::vector<std::vector<Animator>> LegSolvers;
        std::map<std::string, TargetOverrides> Targets;
    };
    IKComponent::IKComponent(const IKComponent& c) :
        Constraints(c.Constraints), Skeleton(c.Skeleton), SkeletonSource(c.SkeletonSource)
    {}
    IKComponent& IKComponent::operator=(const IKComponent& c)
    {
        if (this != &c)
        {
            IKComponent copy(c);
            *this = std::move(copy);
        }
        return *this;
    }
    bool operator==(const IKComponent& a, const IKComponent& b)
    {
        return a.Skeleton == b.Skeleton && Same(a.Constraints, b.Constraints);
    }
    bool operator==(const ShapeKeysComponent& a, const ShapeKeysComponent& b)
    {
        return a.Skeleton == b.Skeleton && a.Expression == b.Expression && a.Weights == b.Weights;
    }
    void IKComponent::ImportLegacy(const AnimationControllerData& c)
    {
        for (auto ik : c.TwoBoneConstraints)
        {
            Constraints.push_back({ik.Name, true, 1, ik});
        }
        for (auto ik : c.LookAtConstraints)
        {
            Constraints.push_back({ik.Name, true, 1, ik});
        }
    }
    void IKComponent::RemapEntities(const std::function<uint64_t(uint64_t)>& remap)
    {
        for (auto& c : Constraints)
            std::visit(
                [&](auto& ik) {
                    using T = std::decay_t<decltype(ik)>;
                    if constexpr (std::is_same_v<T, TwoBoneIK> || std::is_same_v<T, LookAtIK>)
                        if (ik.Target.Entity)
                            ik.Target.Entity = remap(ik.Target.Entity);
                    if constexpr (std::is_same_v<T, FootPlacementIK>)
                        for (auto& leg : ik.Legs)
                            if (leg.PoleEntity)
                                leg.PoleEntity = remap(leg.PoleEntity);
                    if constexpr (std::is_same_v<std::decay_t<decltype(ik)>, TwoBoneIK>)
                        if (ik.PoleEntity)
                            ik.PoleEntity = remap(ik.PoleEntity);
                },
                c.Data);
    }
    bool ShapeKeysComponent::ApplyPreset(const std::string& name, float weight)
    {
        if (!ExpressionSource || !std::isfinite(weight) || weight < 0 || weight > 1)
            return false;
        for (const auto& preset : ExpressionSource->GetData().Presets)
            if (preset.Name == name)
            {
                for (const auto& key : preset.Weights)
                    Weights[key.Name] = key.Weight * weight;
                return true;
            }
        return false;
    }
    void CacheSceneIKInput(const IKComponent& component, const AnimationPose& pose)
    {
        if (!component.Runtime)
            component.Runtime = CreateRef<IKRuntime>();
        component.Runtime->SceneInput = pose;
    }
    const AnimationPose& SceneIKInput(const IKComponent& component, const AnimationPose& fallback)
    {
        return component.Runtime && component.Runtime->SceneInput.Local.size() == fallback.Local.size() ?
                   component.Runtime->SceneInput :
                   fallback;
    }
    void EvaluateIKConstraints(const SkeletonData&               skeleton,
                               const IKComponent&                component,
                               AnimationPose&                    pose,
                               const glm::mat4&                  world,
                               const Animator::IKEntityResolver& resolver,
                               const IKEvaluationContext&        context)
    {
        if (pose.Local.size() != skeleton.Joints.size() || !std::isfinite(glm::determinant(world)) ||
            std::abs(glm::determinant(world)) < 1e-12f)
            return;
        if (!component.Runtime)
            component.Runtime = CreateRef<IKRuntime>();
        auto& runtime = *component.Runtime;
        auto  prepare = [&](Animator& solver, const auto& ik) {
            solver.m_Skeleton = &skeleton;
            AnimationControllerData data;
            using T = std::decay_t<decltype(ik)>;
            if constexpr (std::is_same_v<T, TwoBoneIK>)
                data.TwoBoneConstraints.push_back(ik);
            if constexpr (std::is_same_v<T, LookAtIK>)
                data.LookAtConstraints.push_back(ik);
            solver.m_Controller = &data;
            solver.PrepareIK();
            solver.m_Controller = nullptr;
        };
        if (runtime.Skeleton != &skeleton || !Same(runtime.Constraints, component.Constraints))
        {
            if (runtime.Skeleton != &skeleton || runtime.Constraints.size() != component.Constraints.size())
                runtime.Feet.clear();
            if (runtime.Feet.size() == component.Constraints.size())
                for (size_t i = 0; i < component.Constraints.size(); ++i)
                {
                    auto current  = std::get_if<FootPlacementIK>(&component.Constraints[i].Data);
                    auto previous = i < runtime.Constraints.size() ?
                                        std::get_if<FootPlacementIK>(&runtime.Constraints[i].Data) :
                                        nullptr;
                    bool preserve = current && previous && current->Legs.size() == previous->Legs.size() &&
                                    component.Constraints[i].Name == runtime.Constraints[i].Name;
                    if (preserve)
                    {
                        auto config   = *current;
                        config.Weight = previous->Weight;
                        for (size_t l = 0; l < config.Legs.size(); ++l)
                            config.Legs[l].LockWeight = previous->Legs[l].LockWeight;
                        preserve = Same(config, *previous);
                    }
                    if (!preserve)
                        runtime.Feet[i] = {};
                }
            runtime.Skeleton    = &skeleton;
            runtime.Constraints = component.Constraints;
            runtime.Targets.clear();
            runtime.Solvers.clear();
            runtime.Solvers.resize(component.Constraints.size());
            runtime.Feet.resize(component.Constraints.size());
            runtime.LegSolvers.clear();
            runtime.LegSolvers.resize(component.Constraints.size());
            for (size_t i = 0; i < component.Constraints.size(); ++i)
                std::visit([&](const auto& ik) { prepare(runtime.Solvers[i], ik); }, component.Constraints[i].Data);
        }
        BuildModelPose(skeleton, pose);
        runtime.Animated     = pose;
        const auto& animated = runtime.Animated;
        // Foot placement overrides last only for this evaluation. Keep their storage for the next frame.
        for (auto& [name, target] : runtime.Targets)
        {
            target.HasLimb = false;
            target.HasAim  = false;
        }
        auto resolveTarget = [&](const IKTarget& target, IKTarget& resolved) {
            glm::mat4 matrix(1);
            if (!target.Joint.empty())
            {
                int joint = skeleton.FindJoint(target.Joint);
                if (joint < 0)
                    return false;
                matrix = world * animated.Model[joint];
            }
            else if (target.Entity)
            {
                if (!resolver || !resolver(target.Entity, matrix))
                    return false;
            }
            else
            {
                resolved = target;
                return true;
            }
            resolved.Position = glm::vec3(matrix[3]);
            resolved.Rotation = JointTransform::FromMatrix(matrix).Rotation;
            return true;
        };
        auto solve = [&](Animator& solver, float weight) {
            solver.SetIKWorldTransform(world);
            solver.SetIKEntityResolver(resolver);
            if (weight < 1)
                runtime.Before = pose.Local;
            solver.EvaluateIK(pose);
            if (weight < 1)
                for (size_t j = 0; j < pose.Local.size(); ++j)
                    pose.Local[j] = BlendJoint(runtime.Before[j], pose.Local[j], weight);
            BuildModelPose(skeleton, pose);
        };
        const bool tracksRig = UsesContactTracks(component);
        auto       applyCopy = [&](const IKConstraint& c, const CopyTransformIK* copy) {
            const float weight = std::clamp(c.Weight, 0.f, 1.f);
            int         source = skeleton.FindJoint(copy->Source), dest = skeleton.FindJoint(copy->Joint);
            if (source < 0 || dest < 0 || !std::isfinite(copy->Weight))
                return;
            auto value = pose.Local[source];
            if (copy->WorldSpace)
            {
                int parent = skeleton.Joints[dest].Parent;
                value      = JointTransform::FromMatrix((parent < 0 ? glm::mat4(1) : glm::inverse(pose.Model[parent])) *
                                                        pose.Model[source]);
            }
            const float w = weight * std::clamp(copy->Weight, 0.f, 1.f);
            if (copy->CopyRotation)
                pose.Local[dest].Rotation = BlendAnimationRotations(pose.Local[dest].Rotation, value.Rotation, w, true);
            if (copy->CopyLocation)
                pose.Local[dest].Translation = glm::mix(pose.Local[dest].Translation, value.Translation, w);
            BuildModelPose(skeleton, pose);
        };
        for (size_t i = 0; i < component.Constraints.size(); ++i)
        {
            const auto& c = component.Constraints[i];
            if (!c.Enabled || !std::isfinite(c.Weight) || c.Weight <= 0)
            {
                runtime.Feet[i] = {};
                continue;
            }
            const float weight = std::clamp(c.Weight, 0.f, 1.f);
            if (const auto* copy = std::get_if<CopyTransformIK>(&c.Data))
            {
                if (!tracksRig)
                    applyCopy(c, copy);
            }
            else if (const auto* feet = std::get_if<FootPlacementIK>(&c.Data))
            {
                if (!std::isfinite(feet->Weight) || feet->Weight <= 0)
                {
                    runtime.Feet[i] = {};
                    continue;
                }
                auto  legs    = PlaceFeet(skeleton,
                                          component,
                                          *feet,
                                          weight * std::clamp(feet->Weight, 0.f, 1.f),
                                          pose,
                                          animated,
                                          world,
                                          resolver,
                                          context,
                                          runtime.Feet[i]);
                auto& solvers = runtime.LegSolvers[i];
                if (solvers.size() != legs.size())
                {
                    solvers.clear();
                    solvers.resize(legs.size());
                }
                for (size_t l = 0; l < legs.size(); ++l)
                {
                    IKTarget  original;
                    const int ankle   = skeleton.FindJoint(legs[l].End);
                    original.Position = glm::vec3(world * animated.Model[ankle][3]);
                    original.Rotation = JointTransform::FromMatrix(world * animated.Model[ankle]).Rotation;
                    for (const auto& candidate : component.Constraints)
                        if (candidate.Name == legs[l].Name)
                            if (const auto* limb = std::get_if<TwoBoneIK>(&candidate.Data))
                                resolveTarget(limb->Target, original);
                    // Toe aims on this ankle follow the same ground correction as the leg target.
                    // Their authored relative motion is retained, including independent toe target bones.
                    for (size_t next = i + 1; next < component.Constraints.size(); ++next)
                    {
                        const auto& candidate = component.Constraints[next];
                        const auto* aim       = std::get_if<LookAtIK>(&candidate.Data);
                        if (!aim || !std::any_of(aim->Bones.begin(), aim->Bones.end(), [&](const auto& bone) {
                                return skeleton.FindJoint(bone.Joint) == ankle && !bone.AimChild.empty();
                            }))
                            continue;
                        IKTarget target;
                        if (!resolveTarget(aim->Target, target))
                            continue;
                        const auto rotation = legs[l].Target.Rotation * glm::inverse(original.Rotation);
                        target.Position = legs[l].Target.Position + rotation * (target.Position - original.Position);
                        target.Rotation = rotation * target.Rotation;
                        target.Joint.clear();
                        target.Entity   = 0;
                        auto& override  = runtime.Targets[candidate.Name];
                        override.Aim    = target;
                        override.HasAim = true;
                    }
                    if (!legs[l].Name.empty())
                    {
                        auto& override   = runtime.Targets[legs[l].Name];
                        override.Limb    = legs[l];
                        override.HasLimb = true;
                        if (feet->ContactSource == FootContactSource::Velocity)
                            continue;
                    }
                    legs[l].Weight = 1;
                    if (solvers[l].m_TwoBone.empty() || solvers[l].m_TwoBone[0].Data.Root != legs[l].Root ||
                        solvers[l].m_TwoBone[0].Data.Middle != legs[l].Middle ||
                        solvers[l].m_TwoBone[0].Data.End != legs[l].End)
                        prepare(solvers[l], legs[l]);
                    else
                        solvers[l].m_TwoBone[0].Data = legs[l];
                    solve(solvers[l], 1);
                }
                if (feet->ContactSource == FootContactSource::Tracks)
                {
                    // Initial TwoBone solve, then pelvis reach correction, then enforce the same planted goals.
                    const int pelvis = skeleton.FindJoint(feet->Pelvis);
                    if (pelvis >= 0)
                    {
                        const int  parent      = skeleton.Joints[pelvis].Parent;
                        const auto parentWorld = parent < 0 ? world : world * pose.Model[parent];
                        pose.Local[pelvis].Translation += glm::vec3(
                            glm::inverse(parentWorld) *
                            glm::vec4(0, runtime.Feet[i].Pelvis * weight * std::clamp(feet->Weight, 0.f, 1.f), 0, 0));
                        BuildModelPose(skeleton, pose);
                        for (size_t l = 0; l < legs.size(); ++l)
                            solve(solvers[l], 1);
                    }
                }
            }
            else
            {
                auto& solver = runtime.Solvers[i];
                const auto target = runtime.Targets.find(c.Name);
                // Restore original target each frame before applying this frame's ground adjustment.
                if (const auto* limb = std::get_if<TwoBoneIK>(&c.Data); limb && !solver.m_TwoBone.empty())
                    solver.m_TwoBone[0].Data =
                        target != runtime.Targets.end() && target->second.HasLimb ? target->second.Limb : *limb;
                if (const auto* aim = std::get_if<LookAtIK>(&c.Data); aim && !solver.m_LookAt.empty())
                    solver.m_LookAt[0].Data.Target =
                        target != runtime.Targets.end() && target->second.HasAim ? target->second.Aim : aim->Target;
                solve(solver, weight);
            }
        }
        BuildModelPose(skeleton, pose);
        if (tracksRig)
            for (const auto& c : component.Constraints)
                if (c.Enabled && std::isfinite(c.Weight) && c.Weight > 0)
                    if (const auto* copy = std::get_if<CopyTransformIK>(&c.Data))
                        applyCopy(c, copy);
    }
    void DrawFootPlacementDebug(const IKComponent& component,
                                DebugDraw&         draw,
                                glm::vec4          rayColor,
                                glm::vec4          hitColor,
                                glm::vec4          lockColor)
    {
        if (!component.Runtime)
            return;
        for (const auto& feet : component.Runtime->Feet)
            for (const auto& probe : feet.Probes)
            {
                draw.Line(probe.Start, probe.End, rayColor);
                if (probe.HasHit)
                {
                    draw.Cross(probe.Hit, .025f, hitColor);
                    draw.Line(probe.Hit, probe.Hit + probe.Normal * .12f, hitColor);
                }
                draw.Cross(probe.Target, .04f, probe.Locked ? lockColor : hitColor);
            }
    }
} // namespace Kans
