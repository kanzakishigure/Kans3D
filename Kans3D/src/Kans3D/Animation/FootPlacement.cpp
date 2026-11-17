#include "kspch.h"
#include "FootPlacement.h"
#include <glm/gtx/quaternion.hpp>
#include "Kans3D/Physics/PhysicsWorld.h"
namespace Kans
{
    namespace
    {
        glm::vec3 Point(const glm::mat4& world, const AnimationPose& p, int j)
        {
            return glm::vec3(world * p.Model[j][3]);
        }
        glm::quat Rotation(const glm::mat4& m) { return JointTransform::FromMatrix(m).Rotation; }
        bool      Resolve(const IKTarget&                   t,
                          const SkeletonData&               s,
                          const AnimationPose&              p,
                          const glm::mat4&                  w,
                          const Animator::IKEntityResolver& resolve,
                          IKTarget&                         out)
        {
            glm::mat4 m(1);
            if (!t.Joint.empty())
            {
                int j = s.FindJoint(t.Joint);
                if (j < 0)
                    return false;
                m = w * p.Model[j];
            }
            else if (t.Entity)
            {
                if (!resolve || !resolve(t.Entity, m))
                    return false;
            }
            else
            {
                out = t;
                return true;
            }
            out.Position = glm::vec3(m[3]);
            out.Rotation = Rotation(m);
            return true;
        }
    } // namespace
    std::vector<TwoBoneIK> PlaceFeet(const SkeletonData&               s,
                                     const IKComponent&                component,
                                     const FootPlacementIK&            f,
                                     float                             weight,
                                     AnimationPose&                    pose,
                                     const AnimationPose&              animated,
                                     const glm::mat4&                  world,
                                     const Animator::IKEntityResolver& resolver,
                                     const IKEvaluationContext&        context,
                                     FootPlacementState&               state)
    {
        const float dt = std::isfinite(context.DeltaTime) && context.DeltaTime > 0 ? float(context.DeltaTime) : 0;
        if (context.Reset || dt <= 0 || state.Legs.size() != f.Legs.size())
            state = {};
        state.Legs.resize(f.Legs.size());
        state.Probes.clear();
        std::vector<TwoBoneIK> result;
        float                  pelvis = 0;
        if (state.BindModel.empty())
        {
            AnimationPose bind;
            RestPose(s, bind);
            for (size_t j = 0; j < s.Joints.size(); ++j)
                bind.Local[j] = s.Joints[j].BindLocal;
            BuildModelPose(s, bind);
            state.BindModel = std::move(bind.Model);
        }
        for (size_t i = 0; i < f.Legs.size(); ++i)
        {
            const auto& leg     = f.Legs[i];
            auto&       history = state.Legs[i];
            int         hip = s.FindJoint(leg.Hip), knee = s.FindJoint(leg.Knee), ankle = s.FindJoint(leg.Ankle),
                        toe = s.FindJoint(leg.Toe);
            if (hip < 0 || knee < 0 || ankle < 0)
                continue;
            TwoBoneIK ik;
            ik.Root          = leg.Hip;
            ik.Middle        = leg.Knee;
            ik.End           = leg.Ankle;
            ik.PoleDirection = leg.PoleDirection;
            ik.PoleEntity    = leg.PoleEntity;
            ik.Name          = leg.DrivenConstraint;
            IKTarget incoming;
            incoming.Position = Point(world, animated, ankle);
            incoming.Rotation = Rotation(world * animated.Model[ankle]);
            if (!leg.DrivenConstraint.empty())
            {
                const TwoBoneIK* driven = nullptr;
                for (const auto& c : component.Constraints)
                    if (c.Name == leg.DrivenConstraint && c.Enabled)
                        driven = std::get_if<TwoBoneIK>(&c.Data);
                if (!driven || !Resolve(driven->Target, s, animated, world, resolver, incoming))
                    continue;
                ik      = *driven;
                ik.Name = leg.DrivenConstraint;
            }
            ik.PoleDirection = leg.PoleDirection;
            ik.PoleEntity    = leg.PoleEntity;
            ik.Target        = incoming;
            ik.Target.Entity = 0;
            ik.Target.Joint.clear();
            const glm::vec3 up(0, 1, 0);
            auto            heel     = incoming.Position - up * leg.FootHeight;
            auto            toePoint = heel;
            if (toe >= 0)
            {
                // The bind vector follows the animated target, even when the FK leg was never baked.
                auto offset    = glm::vec3(glm::inverse(state.BindModel[ankle]) * state.BindModel[toe][3]);
                auto direction = incoming.Rotation * offset;
                if (glm::length2(direction) > 1e-12f)
                    toePoint = heel + glm::normalize(direction) * leg.FootLength;
            }
            bool         toePivot = toe >= 0 && toePoint.y < heel.y;
            bool         hit      = false;
            float        offsetY  = -f.MaxStepDown;
            glm::vec3    normal(0, 1, 0);
            const size_t probeStart = state.Probes.size();
            for (int point = 0; point < (toe >= 0 ? 2 : 1); ++point)
            {
                const auto                sole = point ? toePoint : heel;
                FootPlacementState::Probe probe;
                probe.Start = sole + up * f.RayStartHeight;
                probe.End   = sole - up * f.MaxStepDown;
                if (context.Physics)
                {
                    QueryFilter filter;
                    filter.LayerMask    = f.GroundLayerMask;
                    filter.IgnoreEntity = context.IgnoreEntity;
                    filter.Triggers     = QueryTriggerInteraction::Ignore;
                    if (auto h = context.Physics->Raycast(probe.Start, -up, f.RayStartHeight + f.MaxStepDown, filter);
                        h && h->Normal.y > .01f)
                    {
                        probe.HasHit = true;
                        probe.Hit    = h->Point;
                        probe.Normal = h->Normal;
                        float delta  = h->Point.y - sole.y;
                        if (delta >= -f.MaxStepDown && (!hit || delta > offsetY))
                        {
                            hit     = true;
                            offsetY = delta;
                            normal  = h->Normal;
                        }
                    }
                }
                state.Probes.push_back(probe);
            }
            const bool contact = hit && offsetY >= -f.ContactBand;
            if (contact && toe >= 0)
            {
                const auto& heelProbe = state.Probes[probeStart];
                const auto& toeProbe  = state.Probes[probeStart + 1];
                const float heelGap   = heelProbe.HasHit ? heel.y - heelProbe.Hit.y : INFINITY;
                const float toeGap    = toeProbe.HasHit ? toePoint.y - toeProbe.Hit.y : INFINITY;
                toePivot              = toeGap < heelGap;
            }
            const auto pivot = toePivot ? toePoint : heel;
            if (contact)
            {
                ik.Target.Position.y += std::max(-f.MaxStepDown, offsetY) * weight;
                // Add terrain tilt to the authored rotation, retaining deliberate heel/toe rolls on flat ground.
                auto        swing = glm::rotation(up, glm::normalize(normal));
                const float angle = 2 * std::acos(std::clamp(std::abs(swing.w), 0.f, 1.f));
                const float rotationWeight =
                    f.RotationWeight * weight * (angle > f.RotationLimit && angle > 0 ? f.RotationLimit / angle : 1);
                ik.Target.Rotation =
                    BlendAnimationRotations(glm::quat(1, 0, 0, 0), swing, rotationWeight, true) * incoming.Rotation;
                ik.RotationWeight = 1;
                pelvis            = std::min(pelvis, offsetY);
            }
            const auto  animatedPivot    = history.Locked ? (history.ToePivot ? toePoint : heel) : pivot;
            const auto  previousPivot    = toePivot ? history.PreviousToe : history.PreviousHeel;
            const auto  animatedHeel     = Point(world, animated, ankle) - up * leg.FootHeight;
            const auto  animatedToe      = toe >= 0 ? Point(world, animated, toe) - up * leg.FootHeight : animatedHeel;
            const auto  animatedContact  = toePivot ? animatedToe : animatedHeel;
            const auto  previousAnimated = toePivot ? history.PreviousAnimatedToe : history.PreviousAnimatedHeel;
            const float animatedSpeed =
                history.Previous && dt > 0 ? glm::distance(animatedContact, previousAnimated) / dt : INFINITY;
            const auto& contactProbe = state.Probes[probeStart + (toePivot ? 1 : 0)];
            const bool  animatedHeight =
                contactProbe.HasHit && std::abs(animatedContact.y - contactProbe.Hit.y) <= f.ContactBand;
            const float speed  = history.Previous && dt > 0 ? glm::distance(pivot, previousPivot) / dt : INFINITY;
            const bool canLock = f.Locking && leg.LockWeight > 0 && weight > 0 && contact &&
                                 std::abs(offsetY) <= f.ContactBand && animatedHeight && animatedSpeed <= f.LockSpeed &&
                                 speed <= f.LockSpeed;
            const bool tooFar = history.Locked && glm::distance(animatedPivot, history.AnimatedPin) > f.LockMaxDistance;
            if (history.Locked && (!canLock || tooFar))
            {
                history.Locked  = false;
                history.Release = f.ReleaseTime;
            }
            if (f.LockYaw && (history.Locked || history.Release > 0))
            {
                const auto difference = history.PinRotation * glm::inverse(ik.Target.Rotation);
                glm::quat  yaw(difference.w, 0, difference.y, 0);
                if (glm::dot(yaw, yaw) > 1e-12f)
                {
                    const float fade =
                        history.Locked ? 1.f : (f.ReleaseTime > 0 ? history.Release / f.ReleaseTime : 0.f);
                    ik.Target.Rotation =
                        BlendAnimationRotations(
                            glm::quat(1, 0, 0, 0), glm::normalize(yaw), fade * leg.LockWeight * weight, true) *
                        ik.Target.Rotation;
                }
            }
            auto correctedPivot = animatedPivot + (ik.Target.Position - incoming.Position);
            if (toe >= 0 && (history.Locked ? history.ToePivot : toePivot))
                correctedPivot +=
                    (ik.Target.Rotation * glm::inverse(incoming.Rotation)) * (toePoint - heel) - (toePoint - heel);
            if (!history.Locked && canLock && !tooFar && history.Release <= 0)
            {
                history.Locked      = true;
                history.ToePivot    = toePivot;
                history.AnimatedPin = pivot;
                history.Pin         = correctedPivot;
                history.PinRotation = ik.Target.Rotation;
            }
            if (history.Locked)
            {
                history.ReleaseOffset = history.Pin - correctedPivot;
                ik.Target.Position += history.ReleaseOffset * leg.LockWeight * weight;
            }
            else if (history.Release > 0 && f.ReleaseTime > 0)
            {
                history.Release = std::max(0.f, history.Release - std::max(0.f, dt));
                ik.Target.Position +=
                    history.ReleaseOffset * (history.Release / f.ReleaseTime) * leg.LockWeight * weight;
            }
            history.PreviousAnimatedHeel = animatedHeel;
            history.PreviousAnimatedToe  = animatedToe;
            history.PreviousHeel         = heel;
            history.PreviousToe          = toePoint;
            history.Previous             = dt > 0;
            for (size_t p = probeStart; p < state.Probes.size(); ++p)
            {
                state.Probes[p].Target = ik.Target.Position;
                state.Probes[p].Locked = history.Locked;
            }
            if (contact)
            {
                const auto  h = Point(world, animated, hip), k = Point(world, animated, knee),
                            a           = Point(world, animated, ankle);
                const float reach       = glm::distance(h, k) + glm::distance(k, a);
                auto        delta       = ik.Target.Position - h;
                const float horizontal2 = delta.x * delta.x + delta.z * delta.z;
                const float highestHip  = ik.Target.Position.y + std::sqrt(std::max(0.f, reach * reach - horizontal2));
                pelvis                  = std::min(pelvis, highestHip - h.y);
            }
            result.push_back(ik);
        }
        pelvis = std::clamp(pelvis, -f.PelvisMaxLower, f.PelvisMaxRaise);
        if (dt > 0)
        {
            // Exact critically damped integration for a constant target over this frame.
            const float w = std::max(0.f, f.PelvisAdjustSpeed), x = state.Pelvis - pelvis;
            const float c = state.PelvisVelocity + w * x, e = std::exp(-w * dt);
            state.Pelvis         = pelvis + (x + c * dt) * e;
            state.PelvisVelocity = (state.PelvisVelocity - w * c * dt) * e;
        }
        else
        {
            state.Pelvis         = pelvis;
            state.PelvisVelocity = 0;
        }
        int p = s.FindJoint(f.Pelvis);
        if (p >= 0)
        {
            int        parent      = s.Joints[p].Parent;
            const auto parentWorld = parent < 0 ? world : world * pose.Model[parent];
            pose.Local[p].Translation +=
                glm::vec3(glm::inverse(parentWorld) * glm::vec4(0, state.Pelvis * weight, 0, 0));
            BuildModelPose(s, pose);
        }
        return result;
    }
} // namespace Kans
