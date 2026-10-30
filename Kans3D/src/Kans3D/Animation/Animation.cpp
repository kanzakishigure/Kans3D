#include "kspch.h"
#include "Animation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <glm/gtx/quaternion.hpp>

namespace Kans
{
    namespace
    {
        JointTransform PlanarRoot(JointTransform transform)
        {
            // Imported ancestor axes/units are already converted to instance model space (Y-up).
            const auto  forward     = transform.Rotation * glm::vec3(0, 0, 1);
            const auto  right       = transform.Rotation * glm::vec3(1, 0, 0);
            const float yaw         = forward.x * forward.x + forward.z * forward.z > 1e-12f ?
                                          std::atan2(forward.x, forward.z) :
                                          std::atan2(-right.z, right.x);
            transform.Translation.y = 0;
            transform.Rotation      = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
            transform.Scale         = glm::vec3(1);
            return transform;
        }
        glm::mat4 Power(glm::mat4 base, int64_t exponent)
        {
            if (exponent < 0)
            {
                base             = glm::inverse(base);
                exponent         = -(exponent + 1);
                glm::mat4 result = base;
                while (exponent)
                {
                    if (exponent & 1)
                        result *= base;
                    base *= base;
                    exponent >>= 1;
                }
                return result;
            }
            glm::mat4 result(1);
            while (exponent)
            {
                if (exponent & 1)
                    result *= base;
                base *= base;
                exponent >>= 1;
            }
            return result;
        }
    } // namespace
    void AnimationPose::Resize(size_t joints)
    {
        Local.resize(joints);
        Model.resize(joints);
        Palette.resize(joints);
    }
    double AnimationSampleTime(double time, double duration, bool loop)
    {
        if (!std::isfinite(time) || !(duration > 0))
            return 0;
        if (!loop)
            return std::clamp(time, 0.0, duration);
        auto result = std::fmod(time, duration);
        return result < 0 ? result + duration : result;
    }
    void RestPose(const SkeletonData& skeleton, AnimationPose& pose)
    {
        pose.Resize(skeleton.Joints.size());
        for (size_t j = 0; j < pose.Local.size(); ++j)
            pose.Local[j] = skeleton.Joints[j].RestLocal;
        pose.MorphInfluence.assign(skeleton.ShapeKeys.size(), 0);
        pose.MorphWeights.resize(skeleton.ShapeKeys.size());
        for (size_t k = 0; k < pose.MorphWeights.size(); ++k)
            pose.MorphWeights[k] = skeleton.ShapeKeys[k].DefaultWeight;
    }
    void SampleAnimation(const SkeletonData&       skeleton,
                         const BoundAnimationClip& binding,
                         double                    time,
                         bool                      loop,
                         AnimationPose&            pose)
    {
        RestPose(skeleton, pose);
        if (!binding.Clip)
            return;
        time = AnimationSampleTime(time, binding.Clip->Duration, loop);
        for (size_t j = 0; j < pose.Local.size(); ++j)
            if (binding.JointTracks.at(j) >= 0)
                pose.Local[j] =
                    SampleAnimationJoint(pose.Local[j], binding.Clip->Tracks.at(binding.JointTracks[j]), time);
        for (size_t k = 0; k < pose.MorphWeights.size(); ++k)
            if (binding.MorphTracks.at(k) >= 0)
            {
                const auto& track      = binding.Clip->MorphTracks.at(binding.MorphTracks[k]).Weight;
                pose.MorphInfluence[k] = track.KeyCount() ? 1.f : 0.f;
                pose.MorphWeights[k]   = SampleAnimationTrack(track, time, pose.MorphWeights[k]);
            }
    }
    void BuildModelPose(const SkeletonData& skeleton, AnimationPose& pose)
    {
        pose.Resize(skeleton.Joints.size());
        for (size_t j = 0; j < skeleton.Joints.size(); ++j)
        {
            const auto& joint = skeleton.Joints[j];
            pose.Model[j] =
                joint.Parent < 0 ? pose.Local[j].Matrix() : pose.Model[joint.Parent] * pose.Local[j].Matrix();
            pose.Palette[j] = pose.Model[j] * joint.InverseBind;
        }
    }
    JointTransform BlendJoint(const JointTransform& a, const JointTransform& b, float weight)
    {
        weight = std::clamp(weight, 0.f, 1.f);
        return {glm::mix(a.Translation, b.Translation, weight),
                BlendAnimationRotations(a.Rotation, b.Rotation, weight, true),
                glm::mix(a.Scale, b.Scale, weight)};
    }
    void BlendAnimationPoses(const AnimationPose&      a,
                             const AnimationPose&      b,
                             float                     weight,
                             AnimationPose&            out,
                             const std::vector<float>& mask,
                             const std::vector<float>& morphMask)
    {
        if (a.Local.size() != b.Local.size() || (!mask.empty() && mask.size() != a.Local.size()))
            throw std::invalid_argument("Pose/mask size mismatch");
        if (a.MorphWeights.size() != b.MorphWeights.size() ||
            (!morphMask.empty() && morphMask.size() != a.MorphWeights.size()))
            throw std::invalid_argument("Morph pose/mask size mismatch");
        out.MorphInfluence.resize(a.MorphWeights.size());
        for (size_t k = 0; k < a.MorphWeights.size(); ++k)
            out.MorphInfluence[k] = glm::mix(k < a.MorphInfluence.size() ? a.MorphInfluence[k] : 0.f,
                                             k < b.MorphInfluence.size() ? b.MorphInfluence[k] : 0.f,
                                             std::clamp(weight * (morphMask.empty() ? 1 : morphMask[k]), 0.f, 1.f));
        out.MorphWeights.resize(a.MorphWeights.size());
        for (size_t k = 0; k < a.MorphWeights.size(); ++k)
            out.MorphWeights[k] = glm::mix(a.MorphWeights[k],
                                           b.MorphWeights[k],
                                           std::clamp(weight * (morphMask.empty() ? 1 : morphMask[k]), 0.f, 1.f));
        out.Resize(a.Local.size());
        for (size_t j = 0; j < a.Local.size(); ++j)
            out.Local[j] = BlendJoint(a.Local[j], b.Local[j], weight * (mask.empty() ? 1 : mask[j]));
    }
    void AddAnimationPose(AnimationPose&            pose,
                          const AnimationPose&      additive,
                          const AnimationPose&      reference,
                          float                     weight,
                          const std::vector<float>& mask,
                          const std::vector<float>& morphMask)
    {
        if (pose.Local.size() != additive.Local.size() || pose.Local.size() != reference.Local.size() ||
            (!mask.empty() && mask.size() != pose.Local.size()))
            throw std::invalid_argument("Additive pose/mask size mismatch");
        if (pose.MorphWeights.size() != additive.MorphWeights.size() ||
            pose.MorphWeights.size() != reference.MorphWeights.size() ||
            (!morphMask.empty() && morphMask.size() != pose.MorphWeights.size()))
            throw std::invalid_argument("Additive morph pose/mask size mismatch");
        for (size_t k = 0; k < pose.MorphWeights.size(); ++k)
            pose.MorphWeights[k] += (additive.MorphWeights[k] - reference.MorphWeights[k]) *
                                    std::clamp(weight * (morphMask.empty() ? 1 : morphMask[k]), 0.f, 1.f);
        for (size_t j = 0; j < pose.Local.size(); ++j)
        {
            const float w = std::clamp(weight * (mask.empty() ? 1 : mask[j]), 0.f, 1.f);
            const auto& a = additive.Local[j];
            const auto& r = reference.Local[j];
            auto&       p = pose.Local[j];
            p.Translation += (a.Translation - r.Translation) * w;
            p.Rotation =
                glm::normalize(p.Rotation * BlendAnimationRotations(
                                                glm::quat(1, 0, 0, 0), glm::inverse(r.Rotation) * a.Rotation, w, true));
            p.Scale *= glm::mix(glm::vec3(1), a.Scale / r.Scale, w);
        }
    }
    void AnimationBlendWeights(const AnimationMotion& motion, glm::vec2 p, std::vector<float>& out)
    {
        out.resize(motion.Children.size());
        std::fill(out.begin(), out.end(), 0);
        if (out.empty())
            return;
        if (motion.Type == AnimationMotionType::Blend1D)
        {
            if (p.x <= motion.Children.front().Threshold)
            {
                out.front() = 1;
                return;
            }
            if (p.x >= motion.Children.back().Threshold)
            {
                out.back() = 1;
                return;
            }
            for (size_t i = 1; i < out.size(); ++i)
                if (p.x < motion.Children[i].Threshold)
                {
                    const float w = (p.x - motion.Children[i - 1].Threshold) /
                                    (motion.Children[i].Threshold - motion.Children[i - 1].Threshold);
                    out[i - 1]    = 1 - w;
                    out[i]        = w;
                    return;
                }
        }
        else
        {
            float total = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                float w = 1;
                for (size_t j = 0; j < out.size(); ++j)
                    if (i != j)
                    {
                        const auto d = motion.Children[j].Position - motion.Children[i].Position;
                        w = std::min(w,
                                     std::max(0.f, 1 - glm::dot(p - motion.Children[i].Position, d) / glm::dot(d, d)));
                    }
                total += out[i] = w;
            }
            if (total > 0)
                for (auto& w : out)
                    w /= total;
            else // Far outside the convex hull: deterministically choose nearest child.
            {
                size_t nearest = 0;
                for (size_t i = 1; i < out.size(); ++i)
                    if (glm::length2(p - motion.Children[i].Position) <
                        glm::length2(p - motion.Children[nearest].Position))
                        nearest = i;
                out[nearest] = 1;
            }
        }
    }
    void CrossAnimationEvents(const AnimationClipData&  clip,
                              double                    from,
                              double                    to,
                              bool                      loop,
                              const AnimationEventSink& sink,
                              uint32_t                  layer)
    {
        if (!sink || from == to || !std::isfinite(from) || !std::isfinite(to) || clip.Events.empty())
            return;
        const bool back = to < from;
        if (!loop || clip.Duration <= 0)
        {
            for (size_t n = 0; n < clip.Events.size(); ++n)
            {
                const auto& e = clip.Events[back ? clip.Events.size() - 1 - n : n];
                if ((!back && from < e.Time && e.Time <= to) || (back && to <= e.Time && e.Time < from))
                    sink({&e, e.Time, 0, layer, back});
            }
            return;
        }
        // Clip endpoints can both carry events. Keep them distinct at the shared cycle boundary.
        const double lo = std::floor(std::min(from, to) / clip.Duration) - 1;
        const double hi = std::floor(std::max(from, to) / clip.Duration);
        if (lo < double(INT64_MIN + 1) || hi >= double(INT64_MAX))
            throw std::out_of_range("Animation event time out of range");
        const auto first = int64_t(lo), last = int64_t(hi);
        for (int64_t k = back ? last : first;; k += back ? -1 : 1)
        {
            for (size_t n = 0; n < clip.Events.size(); ++n)
            {
                const auto&  e    = clip.Events[back ? clip.Events.size() - 1 - n : n];
                const double time = double(k) * clip.Duration + e.Time;
                if ((!back && from < time && time <= to) || (back && to <= time && time < from))
                    sink({&e, time, k, layer, back});
            }
            if (k == (back ? first : last))
                break;
        }
    }
    float AnimatorLayerStatus::CrossfadeWeight() const
    {
        return !Transitioning || FadeDuration <= 0 ? 1.f : float(std::clamp(FadeElapsed / FadeDuration, 0.0, 1.0));
    }
    AnimationValidation Animator::Prepare(const SkeletonData&            skeleton,
                                          const AnimationControllerData& controller,
                                          const ClipResolver&            resolve,
                                          const ActionResolver&          resolveAction)
    {
        m_Skeleton = nullptr;
        auto valid = ValidateSkeleton(skeleton);
        if (std::holds_alternative<AssetError>(valid))
            return valid;
        valid = ValidateAnimationController(controller);
        if (std::holds_alternative<AssetError>(valid))
            return valid;
        m_Controller     = &controller;
        m_ClipResolver   = resolve;
        m_ActionResolver = resolveAction;
        m_Actions.assign(controller.Layers.size(), {});
        m_ActionStatus.assign(controller.Layers.size(), {});
        m_ActionEvents.clear();
        m_TimelinePoses.clear();
        m_ActionError.reset();
        for (size_t l = 0; l < controller.Layers.size(); ++l)
            m_ActionStatus[l].Slot = controller.Layers[l].Slot;
        m_Parameters.clear();
        for (const auto& p : controller.Parameters)
            m_Parameters.push_back(p.Default);
        m_Clips.clear();
        m_Clips.resize(controller.Motions.size());
        m_MotionPoses.resize(controller.Motions.size());
        m_Weights.resize(controller.Motions.size());
        try
        {
            for (size_t i = 0; i < controller.Motions.size(); ++i)
            {
                const auto& motion = controller.Motions[i];
                m_MotionPoses[i].Resize(skeleton.Joints.size());
                m_MotionPoses[i].MorphWeights.resize(skeleton.ShapeKeys.size());
                m_MotionPoses[i].MorphInfluence.resize(skeleton.ShapeKeys.size());
                m_Weights[i].resize(motion.Children.size());
                if (motion.Type != AnimationMotionType::Clip || (!motion.InlineClip && !motion.Clip.IsValid()))
                    continue; // Unassigned clips sample the rest pose.
                const auto* clip = motion.InlineClip ? &*motion.InlineClip : resolve ? resolve(motion.Clip) : nullptr;
                if (!clip)
                    return AssetError {AssetErrorCode::NotFound, "Animation clip is not ready"};
                valid = ValidateAnimationClip(*clip);
                if (std::holds_alternative<AssetError>(valid))
                    return valid;
                m_Clips[i] = BindAnimationClip(skeleton, *clip, motion.Mapping);
            }
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
        m_Status.assign(controller.Layers.size(), {});
        m_Work.resize(controller.Layers.size());
        for (size_t l = 0; l < m_Work.size(); ++l)
        {
            const auto& layer = controller.Layers[l];
            auto&       work  = m_Work[l];
            work.Source.Resize(skeleton.Joints.size());
            work.Target.Resize(skeleton.Joints.size());
            work.Frozen.Resize(skeleton.Joints.size());
            work.Source.MorphWeights.resize(skeleton.ShapeKeys.size());
            work.Source.MorphInfluence.resize(skeleton.ShapeKeys.size());
            work.Target.MorphWeights.resize(skeleton.ShapeKeys.size());
            work.Target.MorphInfluence.resize(skeleton.ShapeKeys.size());
            work.Frozen.MorphWeights.resize(skeleton.ShapeKeys.size());
            work.Frozen.MorphInfluence.resize(skeleton.ShapeKeys.size());
            work.MorphMask.assign(skeleton.ShapeKeys.size(),
                                  layer.Mask.empty() && layer.ShapeKeyMask.empty() ? 1.f : 0.f);
            for (const auto& key : layer.ShapeKeyMask)
            {
                const auto i = skeleton.FindShapeKey(key.Name);
                if (i < 0)
                    return AssetError {AssetErrorCode::InvalidData, "Unknown shape key mask: " + key.Name};
                work.MorphMask[i] = key.Weight;
            }
            work.UseFrozen    = false;
            work.Weight       = layer.Weight;
            m_Status[l].State = m_Status[l].Target = layer.EntryState;
            work.Mask.assign(skeleton.Joints.size(), layer.Mask.empty() && layer.ShapeKeyMask.empty() ? 1.f : 0.f);
            for (const auto& entry : layer.Mask)
            {
                const auto root = skeleton.FindJoint(entry.Joint);
                if (root < 0)
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Unknown or ambiguous avatar mask joint: " + entry.Joint};
                work.Mask[root] = entry.Weight;
                if (entry.IncludeChildren)
                    for (size_t j = root + 1; j < skeleton.Joints.size(); ++j)
                        for (auto parent = skeleton.Joints[j].Parent; parent >= 0;
                             parent      = skeleton.Joints[parent].Parent)
                            if (parent == root)
                            {
                                work.Mask[j] = entry.Weight;
                                break;
                            }
            }
        }
        m_Root = controller.RootMotionJoint.empty() ? -1 : skeleton.FindJoint(controller.RootMotionJoint);
        if (!controller.RootMotionJoint.empty() && m_Root < 0)
            return AssetError {AssetErrorCode::InvalidData, "Unknown root motion joint"};
        m_Skeleton = &skeleton;
        RestPose(skeleton, m_Rest);
        BuildModelPose(skeleton, m_Rest);
        m_Pose.Resize(skeleton.Joints.size());
        const auto count = skeleton.ShapeKeys.size();
        m_Pose.MorphWeights.resize(count);
        m_Pose.MorphInfluence.resize(count);
        m_Overrides.assign(count, 0);
        m_Offsets.assign(count, 0);
        m_HasOverride.assign(count, 0);
        m_Expression.assign(count, 0);
        m_ExpressionFrom.assign(count, 0);
        m_ExpressionTo.assign(count, 0);
        m_ExpressionElapsed = m_ExpressionDuration = 0;
        m_RootDelta                                = {};
        if (EvaluateControllerIK)
            PrepareIK();
        else
        {
            m_TwoBone.clear();
            m_LookAt.clear();
            m_IKWarnings.clear();
        }
        RebuildPose();
        return std::monostate {};
    }
    int32_t Animator::Parameter(const std::string& name) const
    {
        if (m_Controller)
            for (size_t i = 0; i < m_Controller->Parameters.size(); ++i)
                if (m_Controller->Parameters[i].Name == name)
                    return int32_t(i);
        return -1;
    }
    double Animator::Value(const std::string& name) const
    {
        auto i = Parameter(name);
        return i < 0 ? 0 : m_Parameters[i];
    }
    bool Animator::Set(const std::string& name, double value, AnimationParameterType type)
    {
        const auto i = Parameter(name);
        if (i < 0 || !std::isfinite(value) || m_Controller->Parameters[i].Type != type)
            return false;
        m_Parameters[i] = value;
        return true;
    }
    bool Animator::SetFloat(const std::string& n, float v) { return Set(n, v, AnimationParameterType::Float); }
    bool Animator::SetInt(const std::string& n, int v) { return Set(n, v, AnimationParameterType::Int); }
    bool Animator::SetBool(const std::string& n, bool v) { return Set(n, v, AnimationParameterType::Bool); }
    bool Animator::SetTrigger(const std::string& n) { return Set(n, 1, AnimationParameterType::Trigger); }
    bool Animator::ResetTrigger(const std::string& n) { return Set(n, 0, AnimationParameterType::Trigger); }
    std::optional<double> Animator::GetParameter(const std::string& name) const
    {
        auto i = Parameter(name);
        return i < 0 ? std::optional<double>() : m_Parameters[i];
    }
    bool Animator::SetLayerWeight(size_t layer, float weight)
    {
        if (layer >= m_Work.size() || !std::isfinite(weight) || weight < 0 || weight > 1)
            return false;
        m_Work[layer].Weight = weight;
        return true;
    }
    bool Animator::Play(size_t layer, uint32_t state, double time)
    {
        if (!IsPrepared() || layer >= m_Status.size() || state >= m_Controller->Layers[layer].States.size() ||
            !std::isfinite(time))
            return false;
        ++DiscontinuityVersion;
        auto& s = m_Status[layer];
        s       = {};
        s.State = s.Target      = state;
        s.Time                  = time;
        m_Work[layer].UseFrozen = false;
        RebuildPose();
        return true;
    }
    bool Animator::CrossFade(size_t layer, uint32_t state, double seconds, double time)
    {
        if (!IsPrepared() || layer >= m_Status.size() || state >= m_Controller->Layers[layer].States.size() ||
            !std::isfinite(seconds) || seconds < 0 || !std::isfinite(time))
            return false;
        if (seconds == 0)
            return Play(layer, state, time);
        auto& s = m_Status[layer];
        auto& w = m_Work[layer];
        if (s.Transitioning)
        {
            if (!w.UseFrozen)
                StatePose(layer, s.State, s.Time, w.Source);
            else
            {
                w.Source.Local          = w.Frozen.Local;
                w.Source.MorphWeights   = w.Frozen.MorphWeights;
                w.Source.MorphInfluence = w.Frozen.MorphInfluence;
            }
            StatePose(layer, s.Target, s.TargetTime, w.Target);
            BlendAnimationPoses(w.Source, w.Target, s.CrossfadeWeight(), w.Frozen);
            if (m_Root >= 0 && m_Controller->RootMotionKeepInitialOffset)
                w.FrozenRootReference = LayerRootReference(layer);
            w.UseFrozen = true;
        }
        s.Target        = state;
        s.TargetTime    = time;
        s.FadeElapsed   = 0;
        s.FadeDuration  = seconds;
        s.Transitioning = true;
        return true;
    }
    double Animator::Duration(uint32_t motion) const
    {
        if (m_Clips[motion].Clip)
            return m_Clips[motion].Clip->Duration;
        double duration = 0;
        for (const auto& child : m_Controller->Motions[motion].Children)
            duration = std::max(duration, Duration(child.Motion));
        return duration;
    }
    void Animator::SampleMotion(uint32_t motion, double time, bool loop, AnimationPose& out)
    {
        const auto& m = m_Controller->Motions[motion];
        if (m.Type == AnimationMotionType::Clip)
        {
            SampleAnimation(*m_Skeleton, m_Clips[motion], time, loop, out);
            return;
        }
        AnimationBlendWeights(m, {float(Value(m.ParameterX)), float(Value(m.ParameterY))}, m_Weights[motion]);
        float total = 0;
        // Trees use normalized phase: different clip lengths finish their cycles together.
        const double duration = Duration(motion);
        for (size_t i = 0; i < m.Children.size(); ++i)
        {
            const float weight = m_Weights[motion][i];
            if (weight <= 0)
                continue;
            const auto child = m.Children[i].Motion;
            auto&      pose  = m_MotionPoses[child];
            SampleMotion(child, duration > 0 ? time * Duration(child) / duration : 0, loop, pose);
            if (total == 0)
            {
                out.Local          = pose.Local;
                out.MorphWeights   = pose.MorphWeights;
                out.MorphInfluence = pose.MorphInfluence;
            }
            else
                BlendAnimationPoses(out, pose, weight / (total + weight), out);
            total += weight;
        }
        if (total == 0)
            RestPose(*m_Skeleton, out);
    }
    void Animator::StatePose(size_t layer, uint32_t state, double time, AnimationPose& out)
    {
        const auto& s = m_Controller->Layers[layer].States[state];
        SampleMotion(s.Motion, time, s.Loop, out);
    }
    bool Animator::EvaluateState(size_t layer, uint32_t state, double time, AnimationPose& out)
    {
        if (!IsPrepared() || layer >= m_Status.size() || state >= m_Controller->Layers[layer].States.size() ||
            !std::isfinite(time))
            return false;
        out.Resize(m_Skeleton->Joints.size());
        StatePose(layer, state, time, out);
        for (size_t k = 0; k < out.MorphWeights.size(); ++k)
            if (auto saved = BaseShapeKeyWeights.find(m_Skeleton->ShapeKeys[k].Name);
                saved != BaseShapeKeyWeights.end())
                out.MorphWeights[k] += (saved->second - m_Rest.MorphWeights[k]) * (1 - out.MorphInfluence[k]);
        ApplyAction(layer, out);
        if (m_Modifier)
            m_Modifier(*m_Skeleton, out);
        if (EvaluateControllerIK)
            EvaluateIK(out);
        BuildModelPose(*m_Skeleton, out);
        return true;
    }
    void Animator::MotionEvents(uint32_t                  motion,
                                double                    from,
                                double                    to,
                                bool                      loop,
                                const AnimationEventSink& sink,
                                uint32_t                  layer) const
    {
        if (m_Clips[motion].Clip)
        {
            CrossAnimationEvents(*m_Clips[motion].Clip, from, to, loop, sink, layer);
            return;
        }
        const auto& m = m_Controller->Motions[motion];
        // A clip state without its clip yet (or an empty blend tree) samples the rest pose and has no events.
        if (m.Type == AnimationMotionType::Clip || m.Children.empty())
            return;
        AnimationBlendWeights(m, {float(Value(m.ParameterX)), float(Value(m.ParameterY))}, m_Weights[motion]);
        // Events from the dominant child only, preventing duplicate footsteps in a blend. Ties use child order.
        size_t best       = 0;
        float  bestWeight = -1;
        // Reuse precomputed weights from the last pose rebuild (Advance rebuilds before events).
        for (size_t i = 0; i < m.Children.size(); ++i)
            if (m_Weights[motion][i] > bestWeight)
            {
                best       = i;
                bestWeight = m_Weights[motion][i];
            }
        const auto   child    = m.Children[best].Motion;
        const double duration = Duration(motion);
        const double ratio    = duration > 0 ? Duration(child) / duration : 0;
        MotionEvents(child, from * ratio, to * ratio, loop, sink, layer);
    }
    void
    Animator::ScrubEvents(size_t layer, uint32_t state, double from, double to, const AnimationEventSink& sink) const
    {
        if (!IsPrepared() || layer >= m_Status.size() || state >= m_Controller->Layers[layer].States.size())
            return;
        const auto& s = m_Controller->Layers[layer].States[state];
        MotionEvents(s.Motion, from, to, s.Loop, sink, uint32_t(layer));
    }
    JointTransform Animator::InitialRoot(uint32_t motion)
    {
        const auto& m = m_Controller->Motions[motion];
        if (m.Type == AnimationMotionType::Clip)
        {
            const auto& binding = m_Clips[motion];
            const auto  rest    = m_Rest.Local[m_Root];
            if (!binding.Clip)
                return rest;
            const auto track = binding.JointTracks[m_Root];
            return track >= 0 ?
                       SampleAnimationJoint(rest, binding.Clip->Tracks[track], 0, AnimationSampleEndpoint::First) :
                       rest;
        }
        AnimationBlendWeights(m, {float(Value(m.ParameterX)), float(Value(m.ParameterY))}, m_Weights[motion]);
        JointTransform result = m_Rest.Local[m_Root];
        float          total  = 0;
        for (size_t i = 0; i < m.Children.size(); ++i)
        {
            const float w = m_Weights[motion][i];
            if (w <= 0)
                continue;
            const auto child = InitialRoot(m.Children[i].Motion);
            result           = total == 0 ? child : BlendJoint(result, child, w / (total + w));
            total += w;
        }
        return result;
    }
    JointTransform Animator::LayerRootReference(size_t layer)
    {
        const auto& s      = m_Status[layer];
        const auto& w      = m_Work[layer];
        const auto& states = m_Controller->Layers[layer].States;
        auto        result = w.UseFrozen ? w.FrozenRootReference : InitialRoot(states[s.State].Motion);
        if (s.Transitioning)
            result = BlendJoint(result, InitialRoot(states[s.Target].Motion), s.CrossfadeWeight());
        return result;
    }
    JointTransform Animator::MotionRoot(uint32_t motion, double time, bool loop)
    {
        const auto& m = m_Controller->Motions[motion];
        if (m.Type == AnimationMotionType::Clip)
        {
            const auto& binding = m_Clips[motion];
            if (!binding.Clip)
                return {};
            const auto& clip = *binding.Clip;
            const auto  rest = m_Skeleton->Joints[m_Root].RestLocal;
            const auto  t    = binding.JointTracks[m_Root];
            if (t < 0)
                return {};
            const auto parent      = m_Skeleton->Joints[m_Root].Parent;
            const auto parentModel = parent < 0 ? glm::mat4(1) : m_Rest.Model[parent];
            auto       reference =
                m_Controller->RootMotionKeepInitialOffset ?
                    JointTransform::FromMatrix(
                        parentModel *
                        SampleAnimationJoint(rest, clip.Tracks[t], 0, AnimationSampleEndpoint::First).Matrix()) :
                    JointTransform::FromMatrix(m_Rest.Model[m_Root]);
            reference.Scale = glm::vec3(1);
            if (m_Controller->RootMotionPlanar)
                reference = PlanarRoot(reference);
            const auto inverseReference = glm::inverse(reference.Matrix());
            auto       sample           = [&](double at) {
                const auto endpoint = at <= 0             ? AnimationSampleEndpoint::First :
                                      at >= clip.Duration ? AnimationSampleEndpoint::Last :
                                                            AnimationSampleEndpoint::Interpolated;
                auto       local    = SampleAnimationJoint(rest, clip.Tracks[t], at, endpoint);
                auto       model    = JointTransform::FromMatrix(parentModel * local.Matrix());
                model.Scale         = glm::vec3(1);
                if (m_Controller->RootMotionPlanar)
                    model = PlanarRoot(model);
                // Motion is a factor in instance model space, relative to the rest joint. This
                // converts FBX ancestor unit scales/axis rotations before applying deltas to the entity.
                auto result  = JointTransform::FromMatrix(model.Matrix() * inverseReference);
                result.Scale = glm::vec3(1);
                return result;
            };
            auto result = sample(AnimationSampleTime(time, clip.Duration, loop));
            if (loop && clip.Duration > 0)
            {
                const double cycles = std::floor(time / clip.Duration);
                if (cycles < double(INT64_MIN + 1) || cycles >= double(INT64_MAX))
                    throw std::out_of_range("Root motion time out of range");
                const auto cycle = sample(clip.Duration).Matrix() * glm::inverse(sample(0).Matrix());
                result           = JointTransform::FromMatrix(Power(cycle, int64_t(cycles)) * result.Matrix());
            }
            return result;
        }
        AnimationBlendWeights(m, {float(Value(m.ParameterX)), float(Value(m.ParameterY))}, m_Weights[motion]);
        JointTransform result;
        float          total    = 0;
        const double   duration = Duration(motion);
        for (size_t i = 0; i < m.Children.size(); ++i)
        {
            const float w = m_Weights[motion][i];
            if (w <= 0)
                continue;
            const auto child = m.Children[i].Motion;
            auto       root  = MotionRoot(child, duration > 0 ? time * Duration(child) / duration : 0, loop);
            result           = total == 0 ? root : BlendJoint(result, root, w / (total + w));
            total += w;
        }
        return result;
    }
    RootMotionDelta Animator::StateRootDelta(size_t layer, uint32_t state, double from, double to)
    {
        if (m_Root < 0)
            return {};
        const auto& s = m_Controller->Layers[layer].States[state];
        const auto  a = MotionRoot(s.Motion, from, s.Loop), b = MotionRoot(s.Motion, to, s.Loop);
        const auto  inv = glm::inverse(a.Rotation);
        return {inv * (b.Translation - a.Translation), glm::normalize(inv * b.Rotation)};
    }
    bool Animator::Conditions(const AnimationTransition& transition) const
    {
        for (const auto& c : transition.Conditions)
        {
            const auto v     = Value(c.Parameter);
            bool       match = false;
            switch (c.Op)
            {
                case AnimationConditionOp::Greater:
                    match = v > c.Value;
                    break;
                case AnimationConditionOp::Less:
                    match = v < c.Value;
                    break;
                case AnimationConditionOp::Equal:
                    match = v == c.Value;
                    break;
                case AnimationConditionOp::NotEqual:
                    match = v != c.Value;
                    break;
                case AnimationConditionOp::If:
                    match = v != 0;
                    break;
                case AnimationConditionOp::IfNot:
                    match = v == 0;
                    break;
            }
            if (!match)
                return false;
        }
        return true;
    }
    bool
    Animator::Exit(const AnimationTransition& transition, size_t layer, uint32_t state, double from, double to) const
    {
        if (!transition.ExitTime)
            return true;
        const double duration = Duration(m_Controller->Layers[layer].States[state].Motion);
        if (duration <= 0)
            return true;
        from /= duration;
        to /= duration;
        const auto exit = *transition.ExitTime;
        if (exit >= 1 || !m_Controller->Layers[layer].States[state].Loop)
            return to >= from ? from <= exit && exit <= to : to <= exit && exit <= from;
        const double crossing = to >= from ? std::floor(from - exit) + 1 + exit : std::ceil(from - exit) - 1 + exit;
        return to >= from ? crossing <= to : crossing >= to;
    }
    void Animator::Consume(const AnimationTransition& transition)
    {
        for (const auto& c : transition.Conditions)
        {
            const auto i = Parameter(c.Parameter);
            if (i >= 0 && m_Controller->Parameters[i].Type == AnimationParameterType::Trigger)
                m_Parameters[i] = 0;
        }
    }
    void Animator::Advance(double seconds, const AnimationEventSink& sink, const RootMotionSink& applyMotion)
    {
        if (IsPrepared() && std::isfinite(seconds))
        {
            m_ExpressionElapsed = std::min(m_ExpressionDuration, m_ExpressionElapsed + std::abs(seconds));
            const float t       = m_ExpressionDuration > 0 ? float(m_ExpressionElapsed / m_ExpressionDuration) : 1.f;
            for (size_t k = 0; k < m_Expression.size(); ++k)
                m_Expression[k] = glm::mix(m_ExpressionFrom[k], m_ExpressionTo[k], t);
        }

        if (!IsPrepared() || !std::isfinite(seconds) || !std::isfinite(Speed))
            return;
        const double dt = seconds * Speed;
        m_RootDelta     = {};
        for (size_t l = 0; l < m_Status.size(); ++l)
        {
            auto&       s     = m_Status[l];
            auto&       work  = m_Work[l];
            const auto& layer = m_Controller->Layers[l];
            work.RootDelta    = {};
            // One transition per layer per update. Any-state has priority and obeys the active transition's
            // interruption rule.
            const auto                 permission      = s.Transitioning ? s.Interruption : AnimationInterruption::Both;
            const AnimationTransition* chosen          = nullptr;
            double                     transitionDelay = 0;
            for (int pass = 0; pass < 2 && !chosen; ++pass)
                for (const auto& t : layer.Transitions)
                {
                    if ((t.Source == -1) != (pass == 0) ||
                        (s.Transitioning && permission == AnimationInterruption::None))
                        continue;
                    uint32_t origin = s.State;
                    double   time   = s.Time;
                    if (t.Source >= 0)
                    {
                        if (uint32_t(t.Source) == s.State &&
                            (!s.Transitioning || permission == AnimationInterruption::Source ||
                             permission == AnimationInterruption::Both))
                        {
                        }
                        else if (s.Transitioning && uint32_t(t.Source) == s.Target &&
                                 (permission == AnimationInterruption::Destination ||
                                  permission == AnimationInterruption::Both))
                        {
                            origin = s.Target;
                            time   = s.TargetTime;
                        }
                        else
                            continue;
                    }
                    if ((!t.CanTransitionToSelf && t.Target == (s.Transitioning ? s.Target : s.State)) ||
                        !Conditions(t) || !Exit(t, l, origin, time, time + dt * layer.States[origin].Speed))
                        continue;
                    chosen = &t;
                    if (t.ExitTime)
                    {
                        const double duration = Duration(layer.States[origin].Motion);
                        const double advance  = dt * layer.States[origin].Speed;
                        if (duration > 0 && advance != 0)
                        {
                            const double from = time / duration, exit = *t.ExitTime;
                            const double crossing = exit >= 1 || !layer.States[origin].Loop ? exit :
                                                    advance > 0 ? std::floor(from - exit) + 1 + exit :
                                                                  std::ceil(from - exit) - 1 + exit;
                            transitionDelay       = std::clamp((crossing * duration - time) / advance, 0.0, 1.0);
                        }
                    }
                    break;
                }
            auto step = [&](double frameSeconds) {
                const double advance = frameSeconds * Speed;
                const double old     = s.Time;
                s.Time += advance * layer.States[s.State].Speed;
                auto delta = work.UseFrozen ? RootMotionDelta {} : StateRootDelta(l, s.State, old, s.Time);
                if (!s.Transitioning)
                {
                    if (sink)
                        ScrubEvents(l, s.State, old, s.Time, sink);
                }
                else
                {
                    const double previousTarget = s.TargetTime;
                    s.TargetTime += advance * layer.States[s.Target].Speed;
                    const double elapsed = s.FadeElapsed;
                    const float  before  = s.CrossfadeWeight();
                    s.FadeElapsed += std::abs(frameSeconds);
                    const float after       = s.CrossfadeWeight();
                    const float weight      = .5f * (before + after);
                    const auto  targetDelta = StateRootDelta(l, s.Target, previousTarget, s.TargetTime);
                    delta.Translation       = glm::mix(delta.Translation, targetDelta.Translation, weight);
                    delta.Rotation = BlendAnimationRotations(delta.Rotation, targetDelta.Rotation, weight, true);
                    if (sink)
                    {
                        // Dominant stream policy. Split at the exact half-fade crossing so changing
                        // the frame size neither drops nor duplicates crossings on either stream.
                        const double sourceFraction =
                            frameSeconds == 0 ?
                                (before < .5f ? 1 : 0) :
                                std::clamp((s.FadeDuration * .5 - elapsed) / std::abs(frameSeconds), 0.0, 1.0);
                        if (!work.UseFrozen && sourceFraction > 0)
                            ScrubEvents(l, s.State, old, old + (s.Time - old) * sourceFraction, sink);
                        if (sourceFraction < 1)
                            ScrubEvents(l,
                                        s.Target,
                                        previousTarget + (s.TargetTime - previousTarget) * sourceFraction,
                                        s.TargetTime,
                                        sink);
                    }
                    if (s.FadeElapsed >= s.FadeDuration)
                    {
                        s.State         = s.Target;
                        s.Time          = s.TargetTime;
                        s.Transitioning = false;
                        work.UseFrozen  = false;
                    }
                }
                work.RootDelta.Translation += work.RootDelta.Rotation * delta.Translation;
                work.RootDelta.Rotation = glm::normalize(work.RootDelta.Rotation * delta.Rotation);
                if (l == 0)
                {
                    const float weight = work.Weight * (m_Root >= 0 ? work.Mask[m_Root] : 0);
                    m_RootDelta.Translation += m_RootDelta.Rotation * (delta.Translation * weight);
                    m_RootDelta.Rotation =
                        glm::normalize(m_RootDelta.Rotation *
                                       BlendAnimationRotations(glm::quat(1, 0, 0, 0), delta.Rotation, weight, true));
                }
            };
            auto advance = [&](double frameSeconds) {
                if (s.Transitioning && std::abs(frameSeconds) > s.FadeDuration - s.FadeElapsed)
                {
                    const double untilComplete =
                        std::copysign(std::max(0.0, s.FadeDuration - s.FadeElapsed), frameSeconds);
                    step(untilComplete);
                    step(frameSeconds - untilComplete);
                }
                else
                    step(frameSeconds);
            };
            if (chosen)
            {
                advance(seconds * transitionDelay);
                CrossFade(l, chosen->Target, chosen->Duration);
                s.Interruption = chosen->Interruption;
                Consume(*chosen);
                advance(seconds * (1 - transitionDelay));
            }
            else
                advance(seconds);
        }
        for (size_t l = 0; l < m_Status.size(); ++l)
        {
            auto       delta      = m_Work[l].RootDelta;
            const bool actionRoot = m_Root >= 0 && m_ActionStatus[l].Playing && m_Actions[l].Data.RootMotion;
            AdvanceAction(l, seconds, delta);
            if (!actionRoot)
                continue;
            const float maskWeight = m_Work[l].Weight * m_Work[l].Mask[m_Root];
            if (l == 0)
            {
                m_RootDelta.Translation = delta.Translation * maskWeight;
                m_RootDelta.Rotation = BlendAnimationRotations(glm::quat(1, 0, 0, 0), delta.Rotation, maskWeight, true);
            }
            else
            {
                const auto& action   = m_Actions[l];
                const float coverage = maskWeight * action.RootCoverage;
                if (m_Controller->Layers[l].Mode == AnimationLayerMode::Override)
                {
                    m_RootDelta.Translation *= 1 - coverage;
                    m_RootDelta.Rotation =
                        BlendAnimationRotations(glm::quat(1, 0, 0, 0), m_RootDelta.Rotation, 1 - coverage, true);
                }
                m_RootDelta.Translation += action.RootDelta.Translation * maskWeight;
                m_RootDelta.Rotation = glm::normalize(
                    m_RootDelta.Rotation *
                    BlendAnimationRotations(glm::quat(1, 0, 0, 0), action.RootDelta.Rotation, maskWeight, true));
            }
        }
        auto actionEvents = std::move(m_ActionEvents);
        m_ActionEvents.clear();
        if (sink)
            for (const auto& e : actionEvents)
                sink(e);
        if (applyMotion)
            applyMotion(m_RootDelta);
        RebuildPose();
    }
    bool Animator::SetShapeKeyWeight(const std::string& name, float weight)
    {
        const auto i = IsPrepared() ? m_Skeleton->FindShapeKey(name) : -1;
        if (i < 0 || !std::isfinite(weight))
            return false;
        m_HasOverride[i] = 1;
        m_Overrides[i]   = weight;
        RebuildPose();
        return true;
    }
    bool Animator::AddShapeKeyWeight(const std::string& name, float weight)
    {
        const auto i = IsPrepared() ? m_Skeleton->FindShapeKey(name) : -1;
        if (i < 0 || !std::isfinite(weight))
            return false;
        m_Offsets[i] = weight;
        RebuildPose();
        return true;
    }
    bool Animator::ClearShapeKeyOverride(const std::string& name)
    {
        const auto i = IsPrepared() ? m_Skeleton->FindShapeKey(name) : -1;
        if (i < 0)
            return false;
        m_HasOverride[i] = 0;
        m_Offsets[i]     = 0;
        RebuildPose();
        return true;
    }
    bool Animator::SetExpression(const std::string& name, double seconds)
    {
        if (!IsPrepared() || !std::isfinite(seconds) || seconds < 0)
            return false;
        const ExpressionPreset* preset = nullptr;
        for (const auto& p : m_Controller->Expressions)
            if (p.Name == name)
            {
                preset = &p;
                break;
            }
        if (!preset && !name.empty())
            return false;
        if (preset)
            for (const auto& key : preset->Weights)
                if (m_Skeleton->FindShapeKey(key.Name) < 0)
                    return false;
        m_ExpressionFrom = m_Expression;
        std::fill(m_ExpressionTo.begin(), m_ExpressionTo.end(), 0);
        if (preset)
            for (const auto& key : preset->Weights)
                m_ExpressionTo[m_Skeleton->FindShapeKey(key.Name)] = key.Weight;
        m_ExpressionElapsed  = 0;
        m_ExpressionDuration = seconds;
        if (seconds == 0)
            m_Expression = m_ExpressionTo;
        RebuildPose();
        return true;
    }
    namespace
    {
        bool      IKFinite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool      IKFinite(glm::quat q) { return std::isfinite(q.w) && IKFinite(glm::vec3(q.x, q.y, q.z)); }
        glm::vec3 IKUnit(glm::vec3 v, glm::vec3 fallback)
        {
            const float length = glm::length(v);
            return std::isfinite(length) && length > 1e-7f ? v / length : fallback;
        }
        glm::vec3 IKPerpendicular(glm::vec3 v)
        {
            return IKUnit(glm::cross(v, std::abs(v.x) < .8f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)), {0, 0, 1});
        }
        glm::quat IKBetween(glm::vec3 from, glm::vec3 to)
        {
            from              = IKUnit(from, {0, 1, 0});
            to                = IKUnit(to, from);
            const float dot   = std::clamp(glm::dot(from, to), -1.f, 1.f);
            const auto  cross = glm::cross(from, to);
            const float sine  = glm::length(cross);
            if (sine > 1e-7f)
                return glm::angleAxis(std::atan2(sine, dot), cross / sine);
            return dot < 0 ? glm::angleAxis(glm::pi<float>(), IKPerpendicular(from)) : glm::quat(1, 0, 0, 0);
        }
        glm::quat IKFrame(glm::vec3 forward, glm::vec3 up)
        {
            forward    = IKUnit(forward, {0, 0, 1});
            auto right = IKUnit(glm::cross(up, forward), IKPerpendicular(forward));
            return glm::normalize(glm::quat_cast(glm::mat3(right, glm::cross(forward, right), forward)));
        }
    } // namespace
    void Animator::PrepareIK()
    {
        m_TwoBone.clear();
        m_LookAt.clear();
        m_IKWarnings.clear();
        auto bind = [&](const std::string& name) {
            const auto j = m_Skeleton->FindJoint(name);
            if (j < 0)
                m_IKWarnings.push_back("Missing or ambiguous IK joint: " + name);
            return j;
        };
        for (const auto& data : m_Controller->TwoBoneConstraints)
        {
            BoundTwoBone b {data, bind(data.Root), bind(data.Middle), bind(data.End)};
            if (b.Root >= 0 && b.Middle >= 0 && b.End >= 0 &&
                (m_Skeleton->Joints[b.Middle].Parent != b.Root || m_Skeleton->Joints[b.End].Parent != b.Middle))
            {
                m_IKWarnings.push_back("IK requires consecutive root-middle-end joints: " + data.Name);
                b.Root = -1;
            }
            m_TwoBone.push_back(std::move(b));
        }
        for (const auto& data : m_Controller->LookAtConstraints)
        {
            BoundLookAt b;
            b.Data = data;
            for (const auto& bone : data.Bones)
                b.Joints.push_back(bind(bone.Joint));
            m_LookAt.push_back(std::move(b));
        }
        m_IKModel.resize(m_Skeleton->Joints.size());
        m_IKRotations.resize(m_Skeleton->Joints.size());
        m_IKDirty.resize(m_Skeleton->Joints.size());
        m_IKGeneration.assign(m_Skeleton->Joints.size(), 0);
        m_IKParentGeneration.assign(m_Skeleton->Joints.size(), 0);
        m_IKChain.resize(m_Skeleton->Joints.size());
    }
    bool Animator::SetIKTarget(const std::string& name, const IKTarget& target)
    {
        if (!IKFinite(target.Position) || !IKFinite(target.Rotation) ||
            glm::dot(target.Rotation, target.Rotation) < 1e-12f)
            return false;
        for (auto& b : m_TwoBone)
            if (b.Data.Name == name)
            {
                b.Data.Target = target;
                return true;
            }
        for (auto& b : m_LookAt)
            if (b.Data.Name == name)
            {
                b.Data.Target = target;
                return true;
            }
        return false;
    }
    bool Animator::SetIKWeight(const std::string& name, float weight)
    {
        if (!std::isfinite(weight))
            return false;
        weight = std::clamp(weight, 0.f, 1.f);
        for (auto& b : m_TwoBone)
            if (b.Data.Name == name)
            {
                b.Data.Weight = weight;
                return true;
            }
        for (auto& b : m_LookAt)
            if (b.Data.Name == name)
            {
                b.Data.Weight = weight;
                return true;
            }
        return false;
    }
    bool Animator::SetIKPole(const std::string& name, glm::vec3 direction, uint64_t entity)
    {
        if (!IKFinite(direction))
            return false;
        for (auto& b : m_TwoBone)
            if (b.Data.Name == name)
            {
                b.Data.PoleDirection = direction;
                b.Data.PoleEntity    = entity;
                return true;
            }
        return false;
    }
    bool Animator::SetIKRotation(const std::string& name, glm::quat rotation, float weight)
    {
        if (!IKFinite(rotation) || glm::dot(rotation, rotation) < 1e-12f || !std::isfinite(weight))
            return false;
        for (auto& b : m_TwoBone)
            if (b.Data.Name == name)
            {
                b.Data.Target.Rotation = glm::normalize(rotation);
                b.Data.RotationWeight  = std::clamp(weight, 0.f, 1.f);
                return true;
            }
        return false;
    }
    bool Animator::ResolveIK(const IKTarget& target, glm::vec3& position, glm::quat& rotation) const
    {
        auto point = target.Position;
        auto q     = target.Rotation;
        if (target.Entity)
        {
            glm::mat4 world;
            if (!m_IKResolver || !m_IKResolver(target.Entity, world) || !std::isfinite(glm::determinant(world)) ||
                std::abs(glm::determinant(world)) < 1e-12f)
                return false;
            point = glm::vec3(world[3]);
            q     = JointTransform::FromMatrix(world).Rotation;
        }
        position = glm::vec3(m_IKInverse * glm::vec4(point, 1));
        rotation = glm::normalize(glm::inverse(m_IKWorldRotation) * q);
        return IKFinite(position) && IKFinite(rotation);
    }
    void Animator::RefreshIK(const AnimationPose& pose, int32_t joint)
    {
        // Resolve only the requested joint's ancestors. Generation stamps detect changes to any
        // ancestor, including shared/overlapping chains and non-depth-first skeleton layouts.
        size_t count = 0;
        for (auto j = joint; j >= 0; j = m_Skeleton->Joints[j].Parent)
            m_IKChain[count++] = j;
        while (count > 0)
        {
            const auto     j = m_IKChain[--count], parent = m_Skeleton->Joints[j].Parent;
            const uint64_t generation = parent < 0 ? 0 : m_IKGeneration[parent];
            if (!m_IKDirty[j] && m_IKParentGeneration[j] == generation)
                continue;
            m_IKModel[j] = parent < 0 ? pose.Local[j].Matrix() : m_IKModel[parent] * pose.Local[j].Matrix();
            m_IKRotations[j] =
                glm::normalize(parent < 0 ? pose.Local[j].Rotation : m_IKRotations[parent] * pose.Local[j].Rotation);
            m_IKDirty[j]            = 0;
            m_IKParentGeneration[j] = generation;
            ++m_IKGeneration[j];
        }
    }
    void Animator::RotateIK(AnimationPose& pose, int32_t joint, glm::quat modelRotation)
    {
        const auto parent = m_Skeleton->Joints[joint].Parent;
        pose.Local[joint].Rotation =
            glm::normalize(parent < 0 ? modelRotation : glm::inverse(m_IKRotations[parent]) * modelRotation);
        m_IKDirty[joint] = 1;
        RefreshIK(pose, joint);
    }
    void Animator::EvaluateIK(AnimationPose& pose)
    {
        if (!HasIK())
            return;
        const float determinant = glm::determinant(m_IKWorld);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
            return;
        m_IKInverse       = glm::inverse(m_IKWorld);
        m_IKWorldRotation = JointTransform::FromMatrix(m_IKWorld).Rotation;
        std::fill(m_IKDirty.begin(), m_IKDirty.end(), 1);
        for (const auto& b : m_TwoBone)
        {
            const auto& ik = b.Data;
            if (b.Root < 0 || b.Middle < 0 || b.End < 0)
                continue;
            glm::vec3 target;
            glm::quat rotation;
            if (!ResolveIK(ik.Target, target, rotation))
                continue;
            RefreshIK(pose, b.End);
            const auto root   = glm::vec3(m_IKModel[b.Root][3]);
            auto       middle = glm::vec3(m_IKModel[b.Middle][3]);
            auto       end    = glm::vec3(m_IKModel[b.End][3]);
            auto       pole   = glm::mat3(m_IKInverse) * ik.PoleDirection;
            if (ik.PoleEntity)
            {
                glm::mat4 world;
                if (!m_IKResolver || !m_IKResolver(ik.PoleEntity, world))
                    continue;
                pole = glm::vec3(m_IKInverse * world[3]) - root;
            }
            float a = glm::length(middle - root), c = glm::length(end - middle);
            if (ik.Weight > 0 && a > 1e-7f && c > 1e-7f)
            {
                const auto  oldRoot = pose.Local[b.Root].Rotation, oldMiddle = pose.Local[b.Middle].Rotation;
                const auto  oldMiddleT = pose.Local[b.Middle].Translation, oldEndT = pose.Local[b.End].Translation;
                const float distance  = glm::length(target - root);
                const auto  direction = IKUnit(target - root, IKUnit(end - root, {0, 1, 0}));
                if (ik.Stretch && distance > a + c)
                {
                    const float factor = distance / (a + c);
                    pose.Local[b.Middle].Translation *= factor;
                    pose.Local[b.End].Translation *= factor;
                    a *= factor;
                    c *= factor;
                    m_IKDirty[b.Middle] = m_IKDirty[b.End] = 1;
                    RefreshIK(pose, b.End);
                    middle = glm::vec3(m_IKModel[b.Middle][3]);
                }
                const float reach = std::clamp(distance, std::abs(a - c), a + c);
                const float x     = reach > 1e-7f ? (a * a - c * c + reach * reach) / (2 * reach) : 0;
                const float h     = std::sqrt(std::max(0.f, a * a - x * x));
                auto        bend  = IKUnit(pole - direction * glm::dot(pole, direction), IKPerpendicular(direction));
                const auto  desiredMiddle = root + x * direction + h * bend;
                const auto  desiredEnd    = root + reach * direction;
                RotateIK(pose, b.Root, IKBetween(middle - root, desiredMiddle - root) * m_IKRotations[b.Root]);
                RefreshIK(pose, b.End);
                middle = glm::vec3(m_IKModel[b.Middle][3]);
                end    = glm::vec3(m_IKModel[b.End][3]);
                RotateIK(pose, b.Middle, IKBetween(end - middle, desiredEnd - middle) * m_IKRotations[b.Middle]);
                pose.Local[b.Root].Rotation =
                    BlendAnimationRotations(oldRoot, pose.Local[b.Root].Rotation, ik.Weight, true);
                pose.Local[b.Middle].Rotation =
                    BlendAnimationRotations(oldMiddle, pose.Local[b.Middle].Rotation, ik.Weight, true);
                pose.Local[b.Middle].Translation = glm::mix(oldMiddleT, pose.Local[b.Middle].Translation, ik.Weight);
                pose.Local[b.End].Translation    = glm::mix(oldEndT, pose.Local[b.End].Translation, ik.Weight);
                m_IKDirty[b.Root] = m_IKDirty[b.Middle] = m_IKDirty[b.End] = 1;
            }
            if (ik.RotationWeight > 0)
            {
                RefreshIK(pose, b.End);
                RotateIK(pose, b.End, BlendAnimationRotations(m_IKRotations[b.End], rotation, ik.RotationWeight, true));
            }
        }
        for (const auto& b : m_LookAt)
        {
            const auto& ik = b.Data;
            if (ik.Weight <= 0)
                continue;
            glm::vec3 target;
            glm::quat ignored;
            if (!ResolveIK(ik.Target, target, ignored))
                continue;
            for (size_t k = 0; k < b.Joints.size(); ++k)
            {
                const auto  j    = b.Joints[k];
                const auto& bone = ik.Bones[k];
                if (j < 0 || bone.Weight <= 0)
                    continue;
                RefreshIK(pose, j);
                const auto offset = target - glm::vec3(m_IKModel[j][3]);
                if (glm::length2(offset) < 1e-14f)
                    continue;
                const auto  current = m_IKRotations[j];
                const auto  forward = IKUnit(current * bone.Forward, {0, 0, 1});
                const auto  desired = IKUnit(offset, forward);
                const auto  swing   = IKBetween(forward, desired);
                const float angle   = std::acos(std::clamp(glm::dot(forward, desired), -1.f, 1.f));
                const auto  clampedForward =
                    (angle > ik.AngleLimit ?
                         BlendAnimationRotations(glm::quat(1, 0, 0, 0), swing, ik.AngleLimit / angle, true) :
                         swing) *
                    forward;
                const auto solved = IKFrame(clampedForward, glm::mat3(m_IKInverse) * ik.WorldUp) *
                                    glm::inverse(IKFrame(bone.Forward, bone.Up));
                RotateIK(pose, j, BlendAnimationRotations(current, solved, ik.Weight * bone.Weight, true));
            }
        }
    }
    void Animator::RebuildPose()
    {
        if (!IsPrepared())
            return;
        m_Pose.Local        = m_Rest.Local;
        m_Pose.MorphWeights = m_Rest.MorphWeights;
        for (size_t k = 0; k < m_Skeleton->ShapeKeys.size(); ++k)
            if (auto found = BaseShapeKeyWeights.find(m_Skeleton->ShapeKeys[k].Name);
                found != BaseShapeKeyWeights.end())
                m_Pose.MorphWeights[k] = found->second;
        auto rootReference = m_Root >= 0 ? m_Rest.Local[m_Root] : JointTransform {};
        for (size_t l = 0; l < m_Status.size(); ++l)
        {
            const auto& s     = m_Status[l];
            auto&       w     = m_Work[l];
            const auto& layer = m_Controller->Layers[l];
            if (m_Root >= 0 && m_Controller->RootMotionKeepInitialOffset)
            {
                const auto  initial = LayerRootReference(l);
                const float weight  = w.Weight * w.Mask[m_Root];
                if (layer.Mode == AnimationLayerMode::Additive)
                {
                    const auto& rest = m_Rest.Local[m_Root];
                    rootReference.Translation += (initial.Translation - rest.Translation) * weight;
                    rootReference.Rotation = glm::normalize(
                        rootReference.Rotation *
                        BlendAnimationRotations(
                            glm::quat(1, 0, 0, 0), glm::inverse(rest.Rotation) * initial.Rotation, weight, true));
                }
                else
                    rootReference = BlendJoint(rootReference, initial, weight);
            }
            if (s.Transitioning && w.UseFrozen)
            {
                w.Source.Local          = w.Frozen.Local;
                w.Source.MorphWeights   = w.Frozen.MorphWeights;
                w.Source.MorphInfluence = w.Frozen.MorphInfluence;
            }
            else
                StatePose(l, s.State, s.Time, w.Source);
            if (s.Transitioning)
            {
                StatePose(l, s.Target, s.TargetTime, w.Target);
                BlendAnimationPoses(w.Source, w.Target, s.CrossfadeWeight(), w.Source);
            }
            if (layer.Mode == AnimationLayerMode::Additive)
                AddAnimationPose(m_Pose, w.Source, m_Rest, w.Weight, w.Mask, w.MorphMask);
            else
            {
                w.DrivenMorphMask.resize(w.MorphMask.size());
                for (size_t k = 0; k < w.MorphMask.size(); ++k)
                {
                    const float coverage = w.Source.MorphInfluence[k];
                    w.DrivenMorphMask[k] = w.MorphMask[k] * coverage;
                    if (coverage > 0)
                        w.Source.MorphWeights[k] =
                            (w.Source.MorphWeights[k] - m_Rest.MorphWeights[k] * (1 - coverage)) / coverage;
                }
                BlendAnimationPoses(m_Pose, w.Source, w.Weight, m_Pose, w.Mask, w.DrivenMorphMask);
            }
            ApplyAction(l, m_Pose);
        }
        if (ExtractRootMotion && m_Root >= 0)
        {
            auto& local = m_Pose.Local[m_Root];
            if (m_Controller->RootMotionPlanar)
            {
                const auto parent      = m_Skeleton->Joints[m_Root].Parent;
                const auto parentModel = parent < 0 ? glm::mat4(1) : m_Rest.Model[parent];
                const auto model       = parentModel * local.Matrix();
                const auto sampled     = PlanarRoot(JointTransform::FromMatrix(model));
                const auto reference   = PlanarRoot(JointTransform::FromMatrix(parentModel * rootReference.Matrix()));
                const auto residual    = JointTransform::FromMatrix(glm::inverse(parentModel) * reference.Matrix() *
                                                                    glm::inverse(sampled.Matrix()) * model);
                local.Translation      = residual.Translation;
                local.Rotation         = residual.Rotation;
            }
            else
            {
                local.Translation = rootReference.Translation;
                local.Rotation    = rootReference.Rotation;
            }
        }
        // Clips/blend trees -> masked layers -> additive preset -> absolute script override ->
        // script offset -> modifier -> two-bone IK -> look-at -> one final [0,1] clamp. Presets do not stop clip
        // playback.
        for (size_t k = 0; k < m_Pose.MorphWeights.size(); ++k)
        {
            m_Pose.MorphWeights[k] += m_Expression[k];
            if (m_HasOverride[k])
                m_Pose.MorphWeights[k] = m_Overrides[k];
            m_Pose.MorphWeights[k] += m_Offsets[k];
        }
        if (m_Modifier)
            m_Modifier(*m_Skeleton, m_Pose);
        if (EvaluateControllerIK)
            EvaluateIK(m_Pose);
        for (auto& weight : m_Pose.MorphWeights)
            weight = std::isfinite(weight) ? std::clamp(weight, 0.f, 1.f) : 0;
        BuildModelPose(*m_Skeleton, m_Pose);
    }
} // namespace Kans
