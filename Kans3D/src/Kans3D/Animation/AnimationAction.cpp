#include "kspch.h"
#include <limits>
#include "Animation.h"
namespace Kans
{
    namespace
    {
        double RemainingActionTime(const ActionData& data, size_t section, double time, double rate)
        {
            if (rate <= 0)
                return std::numeric_limits<double>::infinity();
            const auto* current  = &data.Sections[section];
            double      duration = std::max(0.0, current->End - time);
            for (size_t visited = 0; !current->Next.empty(); ++visited)
            {
                // A repeated section means this route has no end until SetNextSection changes it.
                if (visited >= data.Sections.size())
                    return std::numeric_limits<double>::infinity();
                auto next = std::find_if(
                    data.Sections.begin(), data.Sections.end(), [&](const auto& s) { return s.Name == current->Next; });
                if (next == data.Sections.end())
                    return std::numeric_limits<double>::infinity();
                current = &*next;
                duration += current->End - current->Start;
            }
            return duration / rate;
        }
    } // namespace

    int32_t Animator::SlotLayer(const std::string& slot) const
    {
        if (IsPrepared() && !slot.empty())
            for (size_t l = 0; l < m_ActionStatus.size(); ++l)
                if (m_ActionStatus[l].Slot == slot)
                    return int32_t(l);
        return -1;
    }
    bool Animator::IsActionPlaying(const std::string& slot) const
    {
        const auto l = SlotLayer(slot);
        return l >= 0 && m_ActionStatus[l].Playing;
    }
    bool Animator::PlayAction(AssetID id, std::optional<std::string> slot, float speed)
    {
        const auto* data = IsPrepared() && m_ActionResolver ? m_ActionResolver(id) : nullptr;
        if (!data)
        {
            m_ActionError = AssetError {AssetErrorCode::NotFound, "Action asset is not ready: " + id.ToString()};
            return false;
        }
        return StartAction(id, *data, slot.value_or(data->Slot), speed);
    }
    bool Animator::PlayClip(const std::string& slot, AssetID clip, double in, double out, double speed)
    {
        ActionData a;
        a.Clip     = clip;
        a.Slot     = slot;
        a.BlendIn  = in;
        a.BlendOut = out;
        a.Speed    = speed;
        return StartAction({}, std::move(a), slot, 1);
    }
    bool Animator::StartAction(AssetID id, ActionData data, const std::string& slot, double speed)
    {
        const auto l = SlotLayer(slot);
        if (l < 0 || !std::isfinite(speed) || speed <= 0 || !std::isfinite(data.Speed * speed))
        {
            m_ActionError =
                AssetError {AssetErrorCode::InvalidArgument, "Action: unknown slot or invalid speed: " + slot};
            return false;
        }
        const auto* clip = m_ClipResolver ? m_ClipResolver(data.Clip) : nullptr;
        if (!clip)
        {
            m_ActionError = AssetError {AssetErrorCode::NotFound, "Action: unknown clip " + data.Clip.ToString()};
            return false;
        }
        auto valid = ValidateAnimationClip(*clip);
        if (!std::holds_alternative<AssetError>(valid))
            valid = ValidateAction(data, clip);
        if (auto error = std::get_if<AssetError>(&valid))
        {
            m_ActionError = *error;
            return false;
        }
        data.Speed *= speed;
        if (data.Sections.empty())
            data.Sections.push_back({"", 0, clip->Duration, ""});
        ActionWork next;
        next.Data    = std::move(data);
        next.Binding = BindAnimationClip(*m_Skeleton, *clip);
        if (m_ActionStatus[l].Playing)
        {
            SampleAction(l);
            next.Frozen       = m_Actions[l].Pose;
            next.FrozenWeight = ActionWeight(l);
            next.HasFrozen    = true;
            FinishAction(l, true);
        }
        if (next.Data.BlendIn == 0)
            ++DiscontinuityVersion;
        m_Actions[l]  = std::move(next);
        auto& status  = m_ActionStatus[l];
        status        = {slot,
                         m_Actions[l].Data.Sections[0].Name,
                         id,
                         m_Actions[l].Data.Clip,
                         m_Actions[l].Data.Sections[0].Start,
                         0,
                         true,
                         false};
        status.Weight = ActionWeight(l);
        ActionEvent(l, AnimationEventKind::ActionStarted);
        ActionEvent(l, AnimationEventKind::SectionChanged);
        m_ActionError.reset();
        RebuildPose();
        return true;
    }
    float Animator::ActionWeight(size_t l) const
    {
        const auto& a = m_Actions[l];
        const auto& s = m_ActionStatus[l];
        if (!s.Playing)
            return 0;
        const float in       = a.Data.BlendIn > 0 ? float(std::clamp(a.Age / a.Data.BlendIn, 0.0, 1.0)) : 1;
        const float entering = a.HasFrozen ? glm::mix(a.FrozenWeight, 1.f, in) : in;
        if (a.Stopping)
            return a.StopDuration > 0 ? a.StopWeight * float(std::max(0.0, 1 - a.StopElapsed / a.StopDuration)) : 0;
        const double rate = a.Data.Speed * std::max(0.0, Speed);
        const float  out =
            a.Data.BlendOut > 0 ?
                float(std::clamp(RemainingActionTime(a.Data, a.Section, s.Time, rate) / a.Data.BlendOut, 0.0, 1.0)) :
                1;
        return std::min(entering, out);
    }
    void Animator::SampleAction(size_t l)
    {
        auto& a = m_Actions[l];
        SampleAnimation(*m_Skeleton, a.Binding, m_ActionStatus[l].Time, false, a.Pose);
        if (a.HasFrozen && a.Data.BlendIn > 0 && a.Age < a.Data.BlendIn)
        {
            const float t      = float(a.Age / a.Data.BlendIn);
            const float weight = a.FrozenWeight * (1 - t) + t;
            if (weight > 0)
                BlendAnimationPoses(a.Frozen, a.Pose, t / weight, a.Pose);
        }
    }
    bool Animator::SetTimelinePose(const std::string& slot, const AnimationPose& pose, float weight)
    {
        if (!IsPrepared() || pose.Local.size() != m_Skeleton->Joints.size() || !std::isfinite(weight))
            return false;
        auto l = SlotLayer(slot);
        // Older controllers have no authored slots. A runtime alias on the base layer
        // lets the default track use AN4 blending without editing the controller asset.
        if (l < 0 && slot == "Timeline" && IsPrepared() && !m_ActionStatus.empty() &&
            std::all_of(m_ActionStatus.begin(), m_ActionStatus.end(), [](const auto& s) { return s.Slot.empty(); }))
        {
            m_ActionStatus[0].Slot = "Timeline";
            l                      = 0;
        }
        if (l < 0)
            return false;
        m_TimelinePoses[size_t(l)] = {pose, std::clamp(weight, 0.f, 1.f)};
        RebuildPose();
        return true;
    }
    void Animator::ClearTimelinePose(const std::string& slot)
    {
        auto l = SlotLayer(slot);
        if (l >= 0)
        {
            m_TimelinePoses.erase(size_t(l));
            if (m_Controller->Layers[size_t(l)].Slot.empty() && !m_ActionStatus[size_t(l)].Playing)
                m_ActionStatus[size_t(l)].Slot.clear();
            RebuildPose();
        }
    }
    void Animator::ApplyAction(size_t l, AnimationPose& pose)
    {
        if (auto found = m_TimelinePoses.find(l); found != m_TimelinePoses.end())
        {
            auto& t = found->second;
            auto& w = m_Work[l];
            if (m_Controller->Layers[l].Mode == AnimationLayerMode::Additive)
                AddAnimationPose(pose, t.Pose, m_Rest, w.Weight * t.Weight, w.Mask, w.MorphMask);
            else
            {
                auto sample = t.Pose;
                auto mask   = w.MorphMask;
                for (size_t k = 0; k < mask.size(); ++k)
                {
                    const float coverage = sample.MorphInfluence[k];
                    mask[k] *= coverage;
                    if (coverage > 0)
                        sample.MorphWeights[k] =
                            (sample.MorphWeights[k] - m_Rest.MorphWeights[k] * (1 - coverage)) / coverage;
                }
                BlendAnimationPoses(pose, sample, w.Weight * t.Weight, pose, w.Mask, mask);
            }
            return;
        }
        auto& status = m_ActionStatus[l];
        if (!status.Playing)
            return;
        status.Weight = ActionWeight(l);
        SampleAction(l);
        auto&       a      = m_Actions[l];
        auto&       w      = m_Work[l];
        const float weight = w.Weight * status.Weight;
        if (m_Controller->Layers[l].Mode == AnimationLayerMode::Additive)
            AddAnimationPose(pose, a.Pose, m_Rest, weight, w.Mask, w.MorphMask);
        else
        {
            w.DrivenMorphMask.resize(w.MorphMask.size());
            for (size_t k = 0; k < w.MorphMask.size(); ++k)
            {
                const float coverage = a.Pose.MorphInfluence[k];
                w.DrivenMorphMask[k] = w.MorphMask[k] * coverage;
                if (coverage > 0)
                    a.Pose.MorphWeights[k] =
                        (a.Pose.MorphWeights[k] - m_Rest.MorphWeights[k] * (1 - coverage)) / coverage;
            }
            BlendAnimationPoses(pose, a.Pose, weight, pose, w.Mask, w.DrivenMorphMask);
        }
    }
    void Animator::ActionEvent(size_t l, AnimationEventKind kind, const AnimationEvent* event, bool interrupted)
    {
        static const char* names[] = {"", "", "ActionStarted", "SectionChanged", "ActionBlendingOut", "ActionEnded"};
        const auto&        s       = m_ActionStatus[l];
        AnimationEventOccurrence e;
        e.OwnedEvent = std::make_shared<AnimationEvent>(event ? *event : AnimationEvent {s.Time, names[int(kind)], ""});
        e.Event      = e.OwnedEvent.get();
        e.Kind       = kind;
        e.CrossingTime = event ? event->Time : s.Time;
        e.Cycle        = m_Actions[l].Cycle;
        e.Layer        = uint32_t(l);
        e.Slot         = s.Slot;
        e.Section      = s.Section;
        e.Action       = s.Action;
        e.Clip         = s.Clip;
        e.Interrupted  = interrupted;
        m_ActionEvents.push_back(std::move(e));
    }
    void Animator::FinishAction(size_t l, bool interrupted)
    {
        if (!m_ActionStatus[l].Playing)
            return;
        if (!interrupted && ActionWeight(l) > 0)
            ++DiscontinuityVersion;
        ActionEvent(l, AnimationEventKind::ActionEnded, nullptr, interrupted);
        m_ActionStatus[l].Playing = false;
        m_ActionStatus[l].Weight  = 0;
    }
    bool Animator::StopAction(const std::string& slot, double duration)
    {
        const auto l = SlotLayer(slot);
        if (l < 0 || !m_ActionStatus[l].Playing || !std::isfinite(duration) || duration < 0)
            return false;
        auto&       a      = m_Actions[l];
        auto&       s      = m_ActionStatus[l];
        const float weight = ActionWeight(l);
        if (!s.BlendingOut)
            ActionEvent(l, AnimationEventKind::ActionBlendingOut);
        s.BlendingOut  = true;
        a.Stopping     = true;
        a.StopDuration = duration;
        a.StopElapsed  = 0;
        a.StopWeight   = weight;
        if (duration == 0)
        {
            ++DiscontinuityVersion;
            FinishAction(l);
        }
        RebuildPose();
        return true;
    }
    bool Animator::JumpToSection(const std::string& slot, const std::string& name)
    {
        const auto l = SlotLayer(slot);
        if (l < 0 || !m_ActionStatus[l].Playing || m_Actions[l].Stopping)
            return false;
        auto& a = m_Actions[l];
        auto& s = m_ActionStatus[l];
        for (size_t i = 0; i < a.Data.Sections.size(); ++i)
            if (a.Data.Sections[i].Name == name)
            {
                ++DiscontinuityVersion;
                a.Section     = i;
                s.Section     = name;
                s.Time        = a.Data.Sections[i].Start;
                a.Entered     = true;
                a.AtEnd       = false;
                s.BlendingOut = false;
                ActionEvent(l, AnimationEventKind::SectionChanged);
                RebuildPose();
                return true;
            }
        return false;
    }
    bool Animator::SetNextSection(const std::string& slot, const std::string& from, const std::string& to)
    {
        const auto l = SlotLayer(slot);
        if (l < 0 || !m_ActionStatus[l].Playing)
            return false;
        auto& a = m_Actions[l];
        if (!to.empty() && std::none_of(a.Data.Sections.begin(), a.Data.Sections.end(), [&](const auto& section) {
                return section.Name == to;
            }))
            return false;
        for (auto& section : a.Data.Sections)
            if (section.Name == from)
            {
                section.Next = to;
                if (a.Data.Sections[a.Section].Name == from && !to.empty() && !a.Stopping)
                    m_ActionStatus[l].BlendingOut = false;
                return true;
            }
        return false;
    }
    JointTransform Animator::ActionRoot(size_t l, double time)
    {
        const auto& a = m_Actions[l];
        if (m_Root < 0 || a.Binding.JointTracks[m_Root] < 0)
            return {};
        const auto  parent      = m_Skeleton->Joints[m_Root].Parent;
        const auto  parentModel = parent < 0 ? glm::mat4(1) : m_Rest.Model[parent];
        const auto& track       = a.Binding.Clip->Tracks[a.Binding.JointTracks[m_Root]];
        auto        planar      = [](JointTransform value) {
            const auto  forward = value.Rotation * glm::vec3(0, 0, 1);
            const auto  right   = value.Rotation * glm::vec3(1, 0, 0);
            const float yaw     = forward.x * forward.x + forward.z * forward.z > 1e-12f ?
                                      std::atan2(forward.x, forward.z) :
                                      std::atan2(-right.z, right.x);
            value.Translation.y = 0;
            value.Rotation      = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
            return value;
        };
        auto sample = [&](double at) {
            auto model = JointTransform::FromMatrix(
                parentModel * SampleAnimationJoint(m_Rest.Local[m_Root],
                                                   track,
                                                   at,
                                                   at <= 0 ? AnimationSampleEndpoint::First :
                                                   at >= a.Binding.Clip->Duration ?
                                                             AnimationSampleEndpoint::Last :
                                                             AnimationSampleEndpoint::Interpolated)
                                  .Matrix());
            model.Scale = glm::vec3(1);
            if (m_Controller->RootMotionPlanar)
                model = planar(model);
            return model;
        };
        auto reference =
            m_Controller->RootMotionKeepInitialOffset ? sample(0) : JointTransform::FromMatrix(m_Rest.Model[m_Root]);
        reference.Scale = glm::vec3(1);
        if (m_Controller->RootMotionPlanar)
            reference = planar(reference);
        return JointTransform::FromMatrix(sample(time).Matrix() * glm::inverse(reference.Matrix()));
    }
    void Animator::AdvanceAction(size_t l, double seconds, RootMotionDelta& base)
    {
        auto& a        = m_Actions[l];
        auto& s        = m_ActionStatus[l];
        a.RootDelta    = {};
        a.RootCoverage = 0;
        if (!s.Playing || seconds < 0)
            return;
        const bool      root      = a.Data.RootMotion && m_Root >= 0;
        const double    rate      = a.Data.Speed * std::max(0.0, Speed);
        double          remaining = seconds, weightedTime = 0;
        RootMotionDelta motion;
        while (s.Playing)
        {
            const auto   section        = a.Data.Sections[a.Section];
            const double untilEnd       = !a.AtEnd && rate > 0 ? std::max(0.0, (section.End - s.Time) / rate) :
                                                                 std::numeric_limits<double>::infinity();
            const double untilActionEnd = RemainingActionTime(a.Data, a.Section, s.Time, rate);
            if (!a.Stopping && untilActionEnd <= a.Data.BlendOut + 1e-12 && !s.BlendingOut)
            {
                s.BlendingOut = true;
                ActionEvent(l, AnimationEventKind::ActionBlendingOut);
            }
            double step = std::min(remaining, untilEnd);
            if (a.Stopping)
                step = std::min(step, std::max(0.0, a.StopDuration - a.StopElapsed));
            else if (untilActionEnd > a.Data.BlendOut + 1e-12)
                step = std::min(step, untilActionEnd - a.Data.BlendOut);
            if (a.Age < a.Data.BlendIn)
                step = std::min(step, a.Data.BlendIn - a.Age);
            // Split where entering and exiting linear ramps meet; root-motion integration stays step invariant.
            if (!a.Stopping && a.Data.BlendOut > 0 && a.Data.BlendIn > a.Age)
            {
                const double startWeight = a.HasFrozen ? a.FrozenWeight : 0;
                const double slope       = (1 - startWeight) / a.Data.BlendIn;
                const double intersection =
                    (untilActionEnd / a.Data.BlendOut - startWeight - slope * a.Age) / (slope + 1 / a.Data.BlendOut);
                if (intersection > 1e-12)
                    step = std::min(step, intersection);
            }
            const double from   = s.Time;
            const float  before = ActionWeight(l);
            if (!a.AtEnd)
                s.Time = std::min(section.End, s.Time + step * rate);
            a.Age += step;
            if (a.Stopping)
                a.StopElapsed += step;
            const float  after  = ActionWeight(l);
            const double weight = .5 * (before + after);
            if (root)
            {
                weightedTime += step * weight;
                const auto first = ActionRoot(l, from), last = ActionRoot(l, s.Time);
                double     movingWeight = weight;
                if (a.HasFrozen && a.Data.BlendIn > 0 && a.Age - step < a.Data.BlendIn)
                {
                    // The interrupted pose is frozen; only the incoming stream moves the root.
                    const double t     = std::clamp((a.Age - step * .5) / a.Data.BlendIn, 0.0, 1.0);
                    const double total = glm::mix(double(a.FrozenWeight), 1.0, t);
                    if (total > 0)
                        movingWeight *= t / total;
                }
                const auto rotation = glm::normalize(glm::inverse(first.Rotation) * last.Rotation);
                motion.Translation += motion.Rotation *
                                      (glm::inverse(first.Rotation) * (last.Translation - first.Translation)) *
                                      float(movingWeight);
                motion.Rotation =
                    glm::normalize(motion.Rotation *
                                   BlendAnimationRotations(glm::quat(1, 0, 0, 0), rotation, float(movingWeight), true));
            }
            struct Crossing
            {
                const AnimationEvent* Event;
                AnimationEventKind    Kind;
            };
            std::vector<Crossing> events;
            auto                  collect = [&](const auto& stream, AnimationEventKind kind) {
                for (const auto& event : stream)
                    if ((event.Time > from || (a.Entered && event.Time == from)) && event.Time <= s.Time)
                        events.push_back({&event, kind});
            };
            if (!a.AtEnd)
            {
                collect(a.Binding.Clip->Events, AnimationEventKind::Clip);
                collect(a.Data.Events, AnimationEventKind::Action);
                std::stable_sort(events.begin(), events.end(), [](const auto& x, const auto& y) {
                    return x.Event->Time < y.Event->Time;
                });
                for (const auto& event : events)
                    ActionEvent(l, event.Kind, event.Event);
                a.Entered = false;
            }
            remaining = std::max(0.0, remaining - step);
            if (!a.Stopping && !s.BlendingOut &&
                RemainingActionTime(a.Data, a.Section, s.Time, rate) <= a.Data.BlendOut + 1e-12)
            {
                s.BlendingOut = true;
                ActionEvent(l, AnimationEventKind::ActionBlendingOut);
            }
            if (a.Stopping && a.StopElapsed >= a.StopDuration - 1e-12)
            {
                FinishAction(l);
                break;
            }
            if (!a.AtEnd && s.Time >= section.End - 1e-12)
            {
                s.Time = section.End;
                if (a.Stopping)
                    a.AtEnd = true;
                else if (section.Next.empty())
                {
                    if (!s.BlendingOut)
                    {
                        s.BlendingOut = true;
                        ActionEvent(l, AnimationEventKind::ActionBlendingOut);
                    }
                    FinishAction(l);
                    break;
                }
                else
                {
                    for (size_t i = 0; i < a.Data.Sections.size(); ++i)
                        if (a.Data.Sections[i].Name == section.Next)
                        {
                            if (a.Data.Sections[i].Start != section.End)
                                ++DiscontinuityVersion;
                            a.Section = i;
                            s.Section = section.Next;
                            s.Time    = a.Data.Sections[i].Start;
                            ++a.Cycle;
                            a.Entered = true;
                            ActionEvent(l, AnimationEventKind::SectionChanged);
                            break;
                        }
                }
            }
            if (remaining <= 0)
                break;
        }
        s.Weight = ActionWeight(l);
        if (root && seconds > 0)
        {
            a.RootDelta    = motion;
            a.RootCoverage = float(weightedTime / seconds);
            if (m_Controller->Layers[l].Mode == AnimationLayerMode::Override)
            {
                const float keep = float(1 - weightedTime / seconds);
                base.Translation = base.Translation * keep + motion.Translation;
                base.Rotation    = glm::normalize(
                    BlendAnimationRotations(glm::quat(1, 0, 0, 0), base.Rotation, keep, true) * motion.Rotation);
            }
            else
            {
                base.Translation += motion.Translation;
                base.Rotation = glm::normalize(base.Rotation * motion.Rotation);
            }
        }
    }
} // namespace Kans
