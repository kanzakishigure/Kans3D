#include "kspch.h"
#include "ContactTracks.h"
#include <set>
namespace Kans
{
    const ClipRetargetSettings* FindClipRetarget(const std::vector<ClipRetargetSettings>& settings,
                                                 const AnimationClipData*                 clip)
    {
        if (clip)
            for (const auto& entry : settings)
                if (entry.Clip == clip->Name)
                    return &entry;
        return nullptr;
    }
    void ApplyClipRetarget(const SkeletonData&         s,
                           const BoundAnimationClip&   binding,
                           const ClipRetargetSettings& settings,
                           double                      time,
                           AnimationPose&              pose)
    {
        const int root = s.FindJoint(settings.RootJoint);
        if (root >= 0 && binding.Clip)
        {
            auto      reference = s.Joints[root].RestLocal;
            const int track     = binding.JointTracks.at(root);
            if (track >= 0)
                reference =
                    SampleAnimationJoint(reference, binding.Clip->Tracks[track], 0, AnimationSampleEndpoint::First);
            pose.Local[root].Translation =
                reference.Translation +
                (pose.Local[root].Translation - reference.Translation) * settings.RootTranslationScale;
        }
        pose.ContactBinding  = binding;
        pose.ContactSettings = &settings;
        pose.ContactTime     = time;
    }
    namespace
    {
        size_t Samples(const BoundAnimationClip& b, const ContactAnalysisSettings& a)
        {
            if (!b.Clip || !std::isfinite(b.Clip->Duration) || b.Clip->Duration <= 0 || !std::isfinite(a.SampleRate) ||
                a.SampleRate < 1 || a.SampleRate > 240 || !std::isfinite(a.HeightBand) || a.HeightBand < 0 ||
                !std::isfinite(a.VerticalSpeed) || a.VerticalSpeed < 0 || !std::isfinite(a.MinimumDuration) ||
                a.MinimumDuration < 0 || !std::isfinite(a.MergeGap) || a.MergeGap < 0 ||
                b.Clip->Duration * a.SampleRate > 1000000)
                throw std::invalid_argument("Invalid contact analysis settings or clip duration");
            return size_t(std::ceil(b.Clip->Duration * a.SampleRate)) + 1;
        }
        glm::vec3 Point(const AnimationPose& p, int j) { return glm::vec3(p.Model[j][3]); }
    } // namespace
    void AnalyzeFootContacts(const SkeletonData& s, const BoundAnimationClip& b, ClipRetargetSettings& settings)
    {
        const size_t count = Samples(b, settings.Analysis);
        const double dt    = b.Clip->Duration / (count - 1);
        // Analyze the original FK so regenerating after estimating scale does not change window membership.
        for (auto& track : settings.Tracks)
        {
            track.Windows.clear();
            int heel = s.FindJoint(track.Heel.empty() ? track.Foot : track.Heel), toe = s.FindJoint(track.Toe);
            if (heel < 0)
                continue;
            std::array<std::vector<float>, 2> heights;
            heights[0].resize(count);
            heights[1].resize(count);
            std::array<float, 2> floor {INFINITY, INFINITY};
            AnimationPose        pose;
            for (size_t i = 0; i < count; ++i)
            {
                SampleAnimation(s, b, i * dt, false, pose);
                BuildModelPose(s, pose);
                for (int probe = 0; probe < (toe >= 0 ? 2 : 1); ++probe)
                {
                    heights[probe][i] = Point(pose, probe ? toe : heel).y;
                    floor[probe]      = std::min(floor[probe], heights[probe][i]);
                }
            }
            std::vector<FootContactWindow> windows;
            for (size_t i = 0; i < count; ++i)
            {
                bool contact[2] {};
                for (int probe = 0; probe < (toe >= 0 ? 2 : 1); ++probe)
                {
                    const size_t prev = i ? i - 1 : i, next = std::min(i + 1, count - 1);
                    const float  height = heights[probe][i];
                    // Central differences avoid a directional bias when regenerating the tracks.
                    const double speed = std::abs(heights[probe][next] - heights[probe][prev]) / ((next - prev) * dt);
                    contact[probe]     = height <= floor[probe] + settings.Analysis.HeightBand &&
                                         speed <= settings.Analysis.VerticalSpeed;
                }
                if (!contact[0] && !contact[1])
                    continue;
                const double start = i * dt, end = std::min((i + 1) * dt, b.Clip->Duration);
                if (end <= start)
                    continue;
                const bool toePivot = contact[1] && (!contact[0] || heights[1][i] < heights[0][i]);
                if (!windows.empty() && start - windows.back().End <= 1e-9)
                    windows.back().End = end;
                else
                    windows.push_back({start, end, toePivot});
            }
            // Reject short vertical turning-point noise before merging genuine stance runs.
            for (const auto& window : windows)
                if (window.End - window.Start + 1e-9 >= settings.Analysis.MinimumDuration)
                {
                    if (!track.Windows.empty() &&
                        window.Start - track.Windows.back().End <= settings.Analysis.MergeGap + 1e-9)
                        track.Windows.back().End = window.End;
                    else
                        track.Windows.push_back(window);
                }
        }
        settings.Edited = false;
    }
    glm::vec3 EstimateRootTranslationScale(const SkeletonData&         s,
                                           const BoundAnimationClip&   b,
                                           const ClipRetargetSettings& settings)
    {
        const size_t count = Samples(b, settings.Analysis);
        if (s.FindJoint(settings.RootJoint) < 0)
            return settings.RootTranslationScale;
        const double dt        = b.Clip->Duration / (count - 1);
        double       numerator = 0, denominator = 0;
        auto         unit = settings, zero = settings;
        unit.RootTranslationScale = {1, settings.RootTranslationScale.y, 1};
        zero.RootTranslationScale = {0, settings.RootTranslationScale.y, 0};
        AnimationPose p, q;
        for (const auto& track : settings.Tracks)
        {
            for (const auto& window : track.Windows)
            {
                const int joint = s.FindJoint(window.ToePivot && !track.Toe.empty() ? track.Toe :
                                              track.Heel.empty()                    ? track.Foot :
                                                                                      track.Heel);
                if (joint < 0)
                    continue;
                glm::vec3 previousFoot(0), previousRoot(0);
                bool      previous = false;
                for (double time = window.Start; time < window.End - 1e-9; time += dt)
                {
                    SampleAnimation(s, b, time, false, p);
                    q = p;
                    ApplyClipRetarget(s, b, unit, time, p);
                    ApplyClipRetarget(s, b, zero, time, q);
                    BuildModelPose(s, p);
                    BuildModelPose(s, q);
                    auto foot = Point(p, joint), root = foot - Point(q, joint);
                    if (previous)
                    {
                        auto velocity = foot - previousFoot, sensitivity = root - previousRoot;
                        velocity.y = sensitivity.y = 0;
                        numerator += glm::dot(velocity, sensitivity);
                        denominator += glm::dot(sensitivity, sensitivity);
                    }
                    previousFoot = foot;
                    previousRoot = root;
                    previous     = true;
                }
            }
        }
        if (denominator <= 1e-12)
            return settings.RootTranslationScale;
        const float scale = float(std::clamp(1 - numerator / denominator, 0.0, 4.0));
        return {scale, settings.RootTranslationScale.y, scale};
    }
} // namespace Kans
namespace Kans
{
    namespace
    {
        void CheckRetarget(const ClipRetargetSettings& c)
        {
            auto nonnegative = [](double v) { return std::isfinite(v) && v >= 0; };
            if (c.Tracks.size() > 16 || !nonnegative(c.Analysis.HeightBand) || !nonnegative(c.Analysis.VerticalSpeed) ||
                !nonnegative(c.Analysis.MinimumDuration) || !nonnegative(c.Analysis.MergeGap) ||
                !std::isfinite(c.Analysis.SampleRate) || c.Analysis.SampleRate < 1 || c.Analysis.SampleRate > 240)
                throw std::invalid_argument("Invalid clip retarget settings");
            for (int axis = 0; axis < 3; ++axis)
                if (!nonnegative(c.RootTranslationScale[axis]) || c.RootTranslationScale[axis] > 4)
                    throw std::invalid_argument("Invalid root translation scale");
            std::set<std::string> feet;
            for (const auto& track : c.Tracks)
            {
                if ((!track.Foot.empty() && !feet.insert(track.Foot).second) || track.Windows.size() > 65536)
                    throw std::invalid_argument("Invalid foot contact track");
                double end = 0;
                for (const auto& w : track.Windows)
                {
                    if (!nonnegative(w.Start) || !std::isfinite(w.End) || w.End <= w.Start || w.Start < end)
                        throw std::invalid_argument("Invalid or overlapping foot contact windows");
                    end = w.End;
                }
            }
        }
    } // namespace
    YAML::Node EncodeClipRetargets(const std::vector<ClipRetargetSettings>& settings)
    {
        if (settings.size() > 4096)
            throw std::invalid_argument("Too many clip retarget settings");
        YAML::Node            result(YAML::NodeType::Sequence);
        std::set<std::string> clips;
        for (const auto& c : settings)
        {
            CheckRetarget(c);
            if (!clips.insert(c.Clip).second)
                throw std::invalid_argument("Duplicate retarget clip name");
            YAML::Node n;
            n["Clip"]      = c.Clip;
            n["RootJoint"] = c.RootJoint;
            for (int axis = 0; axis < 3; ++axis)
                n["RootTranslationScale"].push_back(c.RootTranslationScale[axis]);
            n["Edited"]          = c.Edited;
            auto a               = n["Analysis"];
            a["HeightBand"]      = c.Analysis.HeightBand;
            a["VerticalSpeed"]   = c.Analysis.VerticalSpeed;
            a["MinimumDuration"] = c.Analysis.MinimumDuration;
            a["MergeGap"]        = c.Analysis.MergeGap;
            a["SampleRate"]      = c.Analysis.SampleRate;
            n["Tracks"]          = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& t : c.Tracks)
            {
                YAML::Node track;
                track["Foot"]    = t.Foot;
                track["Heel"]    = t.Heel;
                track["Toe"]     = t.Toe;
                track["Windows"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& w : t.Windows)
                {
                    YAML::Node window;
                    window["Start"]    = w.Start;
                    window["End"]      = w.End;
                    window["ToePivot"] = w.ToePivot;
                    track["Windows"].push_back(window);
                }
                n["Tracks"].push_back(track);
            }
            result.push_back(n);
        }
        return result;
    }
    std::vector<ClipRetargetSettings> DecodeClipRetargets(const YAML::Node& n)
    {
        std::vector<ClipRetargetSettings> result;
        if (!n || n.IsNull())
            return result;
        if (!n.IsSequence() || n.size() > 4096)
            throw std::invalid_argument("Invalid retarget settings list");
        for (const auto& node : n)
        {
            ClipRetargetSettings c;
            c.Clip = node["Clip"] ? node["Clip"].as<std::string>() : "";
            if (node["RootJoint"])
                c.RootJoint = node["RootJoint"].as<std::string>();
            if (auto scale = node["RootTranslationScale"])
            {
                if (!scale.IsSequence() || scale.size() != 3)
                    throw std::invalid_argument("Invalid root translation scale");
                for (int axis = 0; axis < 3; ++axis)
                    c.RootTranslationScale[axis] = scale[axis].as<float>();
            }
            if (node["Edited"])
                c.Edited = node["Edited"].as<bool>();
            if (auto a = node["Analysis"])
            {
                if (a["HeightBand"])
                    c.Analysis.HeightBand = a["HeightBand"].as<float>();
                if (a["VerticalSpeed"])
                    c.Analysis.VerticalSpeed = a["VerticalSpeed"].as<float>();
                if (a["MinimumDuration"])
                    c.Analysis.MinimumDuration = a["MinimumDuration"].as<double>();
                if (a["MergeGap"])
                    c.Analysis.MergeGap = a["MergeGap"].as<double>();
                if (a["SampleRate"])
                    c.Analysis.SampleRate = a["SampleRate"].as<double>();
            }
            if (auto tracks = node["Tracks"])
            {
                if (!tracks.IsSequence() || tracks.size() > 16)
                    throw std::invalid_argument("Invalid contact tracks");
                for (const auto& t : tracks)
                {
                    FootContactTrack track;
                    track.Foot = t["Foot"] ? t["Foot"].as<std::string>() : "";
                    if (t["Heel"])
                        track.Heel = t["Heel"].as<std::string>();
                    if (t["Toe"])
                        track.Toe = t["Toe"].as<std::string>();
                    if (auto windows = t["Windows"])
                    {
                        if (!windows.IsSequence() || windows.size() > 65536)
                            throw std::invalid_argument("Invalid contact windows");
                        for (const auto& w : windows)
                            track.Windows.push_back({w["Start"].as<double>(),
                                                     w["End"].as<double>(),
                                                     w["ToePivot"] ? w["ToePivot"].as<bool>() : false});
                    }
                    c.Tracks.push_back(std::move(track));
                }
            }
            result.push_back(std::move(c));
        }
        EncodeClipRetargets(result); // Shared validation for scene loading, clipboard and writing.
        return result;
    }
} // namespace Kans
