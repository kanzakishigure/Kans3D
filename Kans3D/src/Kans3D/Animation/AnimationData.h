#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    struct JointTransform
    {
        glm::vec3             Translation {0};
        glm::quat             Rotation {1, 0, 0, 0};
        glm::vec3             Scale {1};
        glm::mat4             Matrix() const;
        static JointTransform FromMatrix(const glm::mat4& matrix);
    };
    struct SkeletonJoint
    {
        std::string    Name, Path, NodeKey;
        int32_t        Parent = -1; // Parent precedes child; includes non-deforming ancestors.
        JointTransform RestLocal, BindLocal;
        glm::mat4      InverseBind {1}; // Model space -> joint bind space.
    };
    struct ShapeKey
    {
        std::string Name;
        float       DefaultWeight = 0;
    };
    struct ShapeKeyWeight
    {
        std::string Name;
        float       Weight = 1;
    };
    struct SkeletonData
    {
        std::vector<SkeletonJoint> Joints;
        std::vector<ShapeKey>      ShapeKeys; // Model-wide logical names, shared by every mesh.
        int32_t                    FindShapeKey(const std::string&) const;
        int32_t                    FindJoint(const std::string& nameOrPath) const;
    };
    enum class TrackInterpolation : uint8_t
    {
        Step,
        Linear,
        Spherical
    };
    enum class AnimationSampleEndpoint : uint8_t
    {
        Interpolated,
        First,
        Last
    };
    template<class T>
    struct AnimationKey
    {
        double Time = 0;
        T      Value {};
    };
    template<class T>
    struct AnimationTrack
    {
        TrackInterpolation           Interpolation = TrackInterpolation::Linear;
        std::vector<AnimationKey<T>> Keys; // Mutable authoring data (Off and controller inline clips).
        // Products v4/v5: separate times/values, bit-exact uniform times and value runs.
        // Logical intervals are retained even for constants: mix/normalize can round differently
        // at different weights, so collapsing intervals would not be lossless.
        uint32_t              PackedCount  = 0;
        uint32_t              TimeEncoding = 0; // 0 v4 explicit, 1 Start+i*Interval, 2 Start+i/Interval,
                                                // 3 shared full table, 4/5 shared table + u16/u32 indices.
        double                Start = 0, Interval = 0;
        std::vector<double>   Times;
        std::vector<T>        Values;
        std::vector<uint32_t> RunEnds; // Exclusive logical end of each value run; empty = one value/key.
        std::shared_ptr<const std::vector<double>> TimeTable;
        std::vector<uint16_t>                      TimeIndices16;
        std::vector<uint32_t>                      TimeIndices32;
        // Exact authored endpoints preserve root cycles after lossy constant fitting.
        std::optional<std::array<T, 2>> RootEndpoints;

        size_t KeyCount() const { return PackedCount ? PackedCount : Keys.size(); }
        double KeyTime(size_t i) const
        {
            if (!PackedCount)
                return Keys[i].Time;
            if (TimeEncoding >= 3)
                return (*TimeTable)[TimeEncoding == 4 ? TimeIndices16[i] : TimeEncoding == 5 ? TimeIndices32[i] : i];
            return TimeEncoding == 1 ? Start + double(i) * Interval :
                   TimeEncoding == 2 ? Start + double(i) / Interval :
                                       Times[i];
        }
        T KeyValue(size_t i) const
        {
            if (!PackedCount)
                return Keys[i].Value;
            if (Values.size() == 1)
                return Values[0];
            return Values[RunEnds.empty() ?
                              i :
                              size_t(std::upper_bound(RunEnds.begin(), RunEnds.end(), i) - RunEnds.begin())];
        }
        AnimationKey<T> KeyAt(size_t i) const { return {KeyTime(i), KeyValue(i)}; }
        size_t          ResidentBytes() const
        {
            return Keys.capacity() * sizeof(AnimationKey<T>) + Times.capacity() * sizeof(double) +
                   Values.capacity() * sizeof(T) + RunEnds.capacity() * sizeof(uint32_t) +
                   TimeIndices16.capacity() * sizeof(uint16_t) + TimeIndices32.capacity() * sizeof(uint32_t);
        }
    };
    inline glm::quat BlendAnimationRotations(glm::quat a, glm::quat b, float weight, bool spherical)
    {
        if (glm::dot(a, b) < 0)
            b = -b;
        return glm::normalize(spherical ? glm::slerp(a, b, weight) : a * (1 - weight) + b * weight);
    }
    template<class T>
    T InterpolateAnimationValues(const T& a, const T& b, float weight, TrackInterpolation mode)
    {
        if constexpr (std::is_same_v<T, glm::quat>)
            return BlendAnimationRotations(a, b, weight, mode == TrackInterpolation::Spherical);
        else
            return glm::mix(a, b, weight);
    }
    template<class T>
    T SampleAnimationTrack(const AnimationTrack<T>& track, double time, const T& rest)
    {
        const size_t count = track.KeyCount();
        if (!count)
            return rest;
        if (time <= track.KeyTime(0))
            return track.KeyValue(0);
        if (time >= track.KeyTime(count - 1))
            return track.KeyValue(count - 1);
        size_t upper;
        if (!track.PackedCount)
            upper = size_t(std::upper_bound(track.Keys.begin(),
                                            track.Keys.end(),
                                            time,
                                            [](double t, const auto& key) { return t < key.Time; }) -
                           track.Keys.begin());
        else if (track.TimeEncoding == 1 || track.TimeEncoding == 2)
        {
            const double index =
                track.TimeEncoding == 1 ? (time - track.Start) / track.Interval : (time - track.Start) * track.Interval;
            upper = size_t(std::clamp(index, 0.0, double(count - 1))) + 1;
            while (upper < count && track.KeyTime(upper) <= time)
                ++upper;
            while (upper > 0 && track.KeyTime(upper - 1) > time)
                --upper;
        }
        else if (track.TimeEncoding == 0)
            upper = size_t(std::upper_bound(track.Times.begin(), track.Times.end(), time) - track.Times.begin());
        else if (track.TimeEncoding == 3)
            upper = size_t(std::upper_bound(track.TimeTable->begin(), track.TimeTable->end(), time) -
                           track.TimeTable->begin());
        else if (track.TimeEncoding == 4)
            upper = size_t(std::upper_bound(track.TimeIndices16.begin(),
                                            track.TimeIndices16.end(),
                                            time,
                                            [&](double t, uint16_t index) { return t < (*track.TimeTable)[index]; }) -
                           track.TimeIndices16.begin());
        else
            upper = size_t(std::upper_bound(track.TimeIndices32.begin(),
                                            track.TimeIndices32.end(),
                                            time,
                                            [&](double t, uint32_t index) { return t < (*track.TimeTable)[index]; }) -
                           track.TimeIndices32.begin());
        const auto a = track.KeyAt(upper - 1);
        if (track.Interpolation == TrackInterpolation::Step)
            return a.Value;
        const auto  b = track.KeyAt(upper);
        const float w = float((time - a.Time) / (b.Time - a.Time));
        return InterpolateAnimationValues(a.Value, b.Value, w, track.Interpolation);
    }
    struct JointAnimationTrack
    {
        std::string               Joint;
        AnimationTrack<glm::vec3> Translation, Scale;
        AnimationTrack<glm::quat> Rotation {TrackInterpolation::Spherical, {}};
    };
    JointTransform SampleAnimationJoint(const JointTransform&,
                                        const JointAnimationTrack&,
                                        double                  time,
                                        AnimationSampleEndpoint endpoint = AnimationSampleEndpoint::Interpolated);
    struct AnimationEvent
    {
        double      Time = 0;
        std::string Name, Payload;
    };
    struct MorphAnimationTrack
    {
        std::string           ShapeKey;
        AnimationTrack<float> Weight;
    };
    enum class AnimationCompressionMode : uint32_t
    {
        Off,
        Lossless,
        Optimal,
        Custom
    };
    struct AnimationCompressionSettings
    {
        AnimationCompressionMode Mode     = AnimationCompressionMode::Off;
        float                    Position = .0001f, Shell = .03f, Morph = .001f;
    };
    struct AnimationCompressionStatistics
    {
        AnimationCompressionMode Mode              = AnimationCompressionMode::Off;
        uint64_t                 UncompressedBytes = 0, CompressedBytes = 0;
        float                    MaxPositionError = 0, MaxMorphError = 0;
        double                   MaxPositionTime = 0;
        std::string              Joint, ShapeKey;
    };
    struct AnimationClipData
    {
        std::string                      Name, SourceName;
        double                           Duration = 0, FrameRate = 30;
        std::vector<JointAnimationTrack> Tracks;
        std::vector<MorphAnimationTrack> MorphTracks;
        std::vector<AnimationEvent>      Events; // Sorted by time; duplicates are distinct events.
        AnimationCompressionStatistics   Compression;
        std::vector<std::shared_ptr<const std::vector<double>>> TimeTables;
        const AnimationCompressionStatistics&                   GetCompressionStatistics() const { return Compression; }
        size_t                                                  ResidentBytes() const;
    };
    struct ActionSection
    {
        std::string Name;
        double      Start = 0, End = 0;
        std::string Next;
    };
    struct ActionData
    {
        AssetID                     Clip;
        std::string                 Slot;
        double                      BlendIn = .2, BlendOut = .2, Speed = 1;
        bool                        RootMotion = true;
        std::vector<ActionSection>  Sections;
        std::vector<AnimationEvent> Events;
    };
    // With a clip, also checks section/event bounds. Asset import resolves the clip before encoding.
    std::variant<std::monostate, AssetError> ValidateAction(const ActionData&, const AnimationClipData* = nullptr);
    struct AnimationNameMapping
    {
        std::string                        StripPrefix;
        std::map<std::string, std::string> Names; // Source name -> target name/path; applied after prefix stripping.
        std::string                        Map(const std::string& source) const;
    };
    struct BoundAnimationClip
    {
        const AnimationClipData* Clip = nullptr;
        std::vector<int32_t>     JointTracks, MorphTracks;
        std::vector<std::string> MissingJoints;
    };
    BoundAnimationClip
    BindAnimationClip(const SkeletonData&, const AnimationClipData&, const AnimationNameMapping& = {});

    enum class AnimationParameterType : uint8_t
    {
        Float,
        Int,
        Bool,
        Trigger
    };
    struct AnimationParameter
    {
        std::string            Name;
        AnimationParameterType Type    = AnimationParameterType::Float;
        double                 Default = 0;
    };
    enum class AnimationMotionType : uint8_t
    {
        Clip,
        Blend1D,
        Blend2D
    };
    struct BlendTreeChild
    {
        uint32_t  Motion    = 0;
        float     Threshold = 0;
        glm::vec2 Position {0};
    };
    struct AnimationMotion
    {
        AnimationMotionType              Type = AnimationMotionType::Clip;
        AssetID                          Clip;
        std::optional<AnimationClipData> InlineClip;
        AnimationNameMapping             Mapping;
        std::string                      ParameterX, ParameterY;
        std::vector<BlendTreeChild>      Children;
    };
    struct AnimationState
    {
        std::string Name;
        uint32_t    Motion = 0;
        double      Speed  = 1;
        bool        Loop   = true;
        // Node position in the editor's graph; not used at runtime.
        std::optional<glm::vec2> EditorPosition;
    };
    enum class AnimationConditionOp : uint8_t
    {
        Greater,
        Less,
        Equal,
        NotEqual,
        If,
        IfNot
    };
    struct AnimationCondition
    {
        std::string          Parameter;
        AnimationConditionOp Op    = AnimationConditionOp::If;
        double               Value = 0;
    };
    enum class AnimationInterruption : uint8_t
    {
        None,
        Source,
        Destination,
        Both
    };
    struct AnimationTransition
    {
        int32_t  Source = -1; // -1 = any state. List order is priority; any-state transitions are considered first.
        uint32_t Target = 0;
        std::vector<AnimationCondition> Conditions;
        std::optional<double>           ExitTime; // Normalized source time. <1 repeats each loop; >=1 is absolute.
        double                          Duration            = 0.2; // Seconds, independent of state speed.
        AnimationInterruption           Interruption        = AnimationInterruption::None;
        bool                            CanTransitionToSelf = false;
    };
    enum class AnimationLayerMode : uint8_t
    {
        Override,
        Additive
    };
    struct AvatarMaskEntry
    {
        std::string Joint;
        float       Weight          = 1;
        bool        IncludeChildren = false;
    };
    struct AnimationLayer
    {
        std::string        Name;
        float              Weight = 1;
        AnimationLayerMode Mode   = AnimationLayerMode::Override;
        std::vector<ShapeKeyWeight>
            ShapeKeyMask; // Nonempty masks jointly select channels; a face-only mask selects no joints.
        std::vector<AvatarMaskEntry>
                    Mask; // Both masks empty = all channels; otherwise unlisted channels have zero weight.
        std::string Slot; // Empty = no action slot. Unique within the controller.
        uint32_t    EntryState = 0;
        std::vector<AnimationState>      States;
        std::vector<AnimationTransition> Transitions;
    };
    struct ExpressionPreset
    {
        std::string                 Name;
        std::vector<ShapeKeyWeight> Weights;
    };
    // All points, directions and rotations are world space. Entity=0 uses the explicit value.
    // An unresolved entity disables that constraint for the current evaluation.
    struct IKTarget
    {
        uint64_t  Entity = 0;
        glm::vec3 Position {0};
        glm::quat Rotation {1, 0, 0, 0};
        std::string Joint; // Same skeleton, current pose. Takes precedence over Entity.
    };
    struct TwoBoneIK
    {
        std::string Name, Root, Middle, End;
        IKTarget    Target;
        uint64_t    PoleEntity = 0;
        glm::vec3   PoleDirection {0, 0, 1};
        float       Weight = 1, RotationWeight = 0;
        bool        Stretch = false;
    };
    struct LookAtBone
    {
        std::string Joint;
        float       Weight = 1;
        glm::vec3   Forward {0, 0, 1}, Up {0, 1, 0};
        std::string AimChild; // Optional bind-pose child defines forward; swing only, retaining twist.
    };
    struct LookAtIK
    {
        std::string             Name;
        std::vector<LookAtBone> Bones; // Proximal to distal; applied in this order.
        IKTarget                Target;
        glm::vec3               WorldUp {0, 1, 0};
        float                   Weight = 1, AngleLimit = 3.14159265f; // Radians from the incoming pose's forward axis.
    };
    struct FootContactWindow
    {
        double Start = 0, End = 0; // Clip seconds, half-open [Start, End).
        bool   ToePivot = false;
    };
    struct FootContactTrack
    {
        std::string                    Foot, Heel, Toe; // Heel is optional; otherwise Foot is the heel probe.
        std::vector<FootContactWindow> Windows;
    };
    struct ContactAnalysisSettings
    {
        float  HeightBand = .06f, VerticalSpeed = .15f;
        double MinimumDuration = .1, MergeGap = .05, SampleRate = 60;
    };
    // Scene-owned, per clip name. Imported clip data is always immutable.
    struct ClipRetargetSettings
    {
        std::string                   Clip, RootJoint;
        glm::vec3                     RootTranslationScale {1};
        ContactAnalysisSettings       Analysis;
        std::vector<FootContactTrack> Tracks;
        bool                          Edited = false;
    };
    struct CopyTransformIK
    {
        std::string Name, Source, Joint;
        bool        CopyRotation = true, CopyLocation = false, WorldSpace = true;
        float       Weight = 1;
    };
    struct FootPlacementLeg
    {
        std::string Hip, Knee, Ankle, Toe;
        std::string DrivenConstraint; // Named TwoBoneIK later in the list; empty solves this leg here.
        float       FootHeight = .08f, FootLength = .22f;
        uint64_t    PoleEntity = 0;
        glm::vec3   PoleDirection {0, 0, 1};
        float       LockWeight = 1; // Timeline: IK.<constraint>.Legs.<index>.LockWeight
    };
    enum class FootContactSource : uint8_t
    {
        Velocity,
        Tracks
    };
    struct FootPlacementIK
    {
        std::string                   Name;
        std::vector<FootPlacementLeg> Legs;
        std::string                   Pelvis;
        uint32_t                      GroundLayerMask = UINT32_MAX;
        float                         RayStartHeight = .5f, MaxStepDown = .3f, ContactBand = .06f;
        float                         PelvisAdjustSpeed = 12, PelvisMaxLower = .4f, PelvisMaxRaise = .02f;
        float                         RotationWeight = 1, RotationLimit = .6f;
        FootContactSource             ContactSource = FootContactSource::Velocity;
        float                         TrackMaxReach = .9f; // Maximum pin correction as a fraction of leg reach.
        bool                          Locking = true, LockYaw = false;
        float                         LockSpeed = .05f, LockMaxDistance = .03f, ReleaseTime = .12f;
        float                         Weight = 1;
    };
    struct AnimationControllerData
    {
        std::vector<AnimationParameter> Parameters;
        std::vector<AnimationMotion>    Motions; // Index-based acyclic graph; nested blend trees allowed.
        std::vector<AnimationLayer>     Layers;
        std::string                     RootMotionJoint;
        bool RootMotionPlanar            = false; // Model-space Y-up: entity receives XZ translation and yaw only.
        bool RootMotionKeepInitialOffset = false; // Retain each clip's first-frame root placement in the pose.
        // Legacy controller payloads only; scene authoring uses separate deformation components/assets.
        std::vector<ExpressionPreset> Expressions;
        std::vector<TwoBoneIK>        TwoBoneConstraints;
        std::vector<LookAtIK>         LookAtConstraints;
    };
    using AnimationValidation = std::variant<std::monostate, AssetError>;
    AnimationValidation ValidateSkeleton(const SkeletonData&);
    AnimationValidation ValidateAnimationClip(const AnimationClipData&);
    AnimationValidation ValidateAnimationController(const AnimationControllerData&);

    template<class T, AssetType Type>
    class AnimationDataAsset final : public Asset
    {
    public:
        explicit AnimationDataAsset(T data) : m_Data(std::move(data)) {}
        static AssetType GetStaticType() { return Type; }
        AssetType        GetAssetType() const override { return Type; }
        const T&         GetData() const { return m_Data; }

    private:
        T m_Data;
    };
    using ActionAsset        = AnimationDataAsset<ActionData, AssetType::Action>;
    using SkeletonAsset      = AnimationDataAsset<SkeletonData, AssetType::Skeleton>;
    using AnimationClipAsset = AnimationDataAsset<AnimationClipData, AssetType::AnimationClip>;
    struct ExpressionData
    {
        std::vector<ExpressionPreset> Presets;
    };
    using ExpressionAsset          = AnimationDataAsset<ExpressionData, AssetType::Expression>;
    using AnimationControllerAsset = AnimationDataAsset<AnimationControllerData, AssetType::AnimationController>;
} // namespace Kans
