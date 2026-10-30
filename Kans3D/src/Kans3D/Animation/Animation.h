#pragma once
#include "AnimationData.h"

namespace Kans
{
    struct AnimationPose
    {
        std::vector<JointTransform> Local;
        std::vector<glm::mat4>      Model, Palette;
        std::vector<float>          MorphInfluence; // Authored-channel coverage through motions/fades.
        std::vector<float>          MorphWeights;   // Logical shape-key order from SkeletonData.
        void                        Resize(size_t joints);
    };
    double AnimationSampleTime(double time, double duration, bool loop);
    void   RestPose(const SkeletonData&, AnimationPose&);
    void   SampleAnimation(const SkeletonData&, const BoundAnimationClip&, double time, bool loop, AnimationPose&);
    void   BuildModelPose(const SkeletonData&, AnimationPose&);
    JointTransform BlendJoint(const JointTransform&, const JointTransform&, float weight);
    void           BlendAnimationPoses(const AnimationPose&,
                                       const AnimationPose&,
                                       float weight,
                                       AnimationPose&,
                                       const std::vector<float>& mask      = {},
                                       const std::vector<float>& morphMask = {});
    void           AddAnimationPose(AnimationPose&,
                                    const AnimationPose&,
                                    const AnimationPose&      reference,
                                    float                     weight,
                                    const std::vector<float>& mask      = {},
                                    const std::vector<float>& morphMask = {});
    // Caller-owned output has child count entries. Cartesian gradient-band interpolation, normalized to unity.
    void AnimationBlendWeights(const AnimationMotion&, glm::vec2 parameter, std::vector<float>& output);
    enum class AnimationEventKind : uint8_t
    {
        Clip,
        Action,
        ActionStarted,
        SectionChanged,
        ActionBlendingOut,
        ActionEnded
    };
    struct AnimationEventOccurrence
    {
        const AnimationEvent* Event        = nullptr;
        double                CrossingTime = 0;
        int64_t               Cycle        = 0;
        uint32_t              Layer        = 0;
        bool                  Backwards    = false;
        AnimationEventKind    Kind         = AnimationEventKind::Clip;
        std::string           Slot, Section;
        AssetID               Action, Clip;
        bool                  Interrupted = false;
        // Own action/lifecycle events so queued occurrences survive interruption and asset unload.
        std::shared_ptr<const AnimationEvent> OwnedEvent;
    };
    using AnimationEventSink = std::function<void(const AnimationEventOccurrence&)>;
    // Forward (from,to], backward [to,from). Equal endpoints produce no events.
    void CrossAnimationEvents(const AnimationClipData&,
                              double from,
                              double to,
                              bool   loop,
                              const AnimationEventSink&,
                              uint32_t layer = 0);
    struct RootMotionDelta
    {
        glm::vec3 Translation {0};
        glm::quat Rotation {1, 0, 0, 0};
    };
    struct AnimatorLayerStatus
    {
        uint32_t              State = 0, Target = 0;
        double                Time = 0, TargetTime = 0, FadeElapsed = 0, FadeDuration = 0;
        bool                  Transitioning = false;
        AnimationInterruption Interruption  = AnimationInterruption::None;
        float                 CrossfadeWeight() const;
    };

    struct AnimatorActionStatus
    {
        std::string Slot, Section;
        AssetID     Action, Clip;
        double      Time    = 0;
        float       Weight  = 0;
        bool        Playing = false, BlendingOut = false;
    };
    // CPU evaluator. Prepare allocates/binds once. Advance, EvaluateState and RebuildPose reuse storage.
    // Data passed to Prepare must outlive this object. Copying the evaluator is safe while data stays alive.
    struct IKComponent;
    class Animator
    {
    public:
        using ClipResolver   = std::function<const AnimationClipData*(AssetID)>;
        using ActionResolver = std::function<const ActionData*(AssetID)>;
        AnimationValidation Prepare(const SkeletonData&,
                                    const AnimationControllerData&,
                                    const ClipResolver&   = {},
                                    const ActionResolver& = {});
        bool                PlayAction(AssetID, std::optional<std::string> slot = {}, float speedScale = 1);
        bool PlayClip(const std::string& slot, AssetID clip, double blendIn, double blendOut, double speed = 1);
        bool StopAction(const std::string& slot, double blendOut);
        // Silent absolute-time overlay, sharing AN4 slot masks, layer mode and morph coverage.
        bool SetTimelinePose(const std::string& slot, const AnimationPose&, float weight);
        void ClearTimelinePose(const std::string& slot);
        bool HasTimelinePose() const { return !m_TimelinePoses.empty(); }
        bool JumpToSection(const std::string& slot, const std::string& name);
        bool SetNextSection(const std::string& slot, const std::string& from, const std::string& to);
        bool IsActionPlaying(const std::string& slot) const;
        const std::vector<AnimatorActionStatus>& GetActions() const { return m_ActionStatus; }
        const std::optional<AssetError>&         GetActionError() const { return m_ActionError; }
        bool                                     IsPrepared() const { return m_Skeleton != nullptr; }
        bool                                     SetFloat(const std::string&, float);
        bool                                     SetInt(const std::string&, int);
        bool                                     SetBool(const std::string&, bool);
        bool                                     SetTrigger(const std::string&);
        bool                                     ResetTrigger(const std::string&);
        std::optional<double>                    GetParameter(const std::string&) const;
        bool                                     SetLayerWeight(size_t, float);
        bool                                     Play(size_t layer, uint32_t state, double time = 0);
        bool                         CrossFade(size_t layer, uint32_t state, double seconds, double time = 0);
        const std::vector<ShapeKey>& GetShapeKeys() const
        {
            static const std::vector<ShapeKey> empty;
            return m_Skeleton ? m_Skeleton->ShapeKeys : empty;
        }
        const std::vector<uint8_t>&          GetShapeKeyOverrides() const { return m_HasOverride; }
        const std::vector<float>&            GetShapeKeyOffsets() const { return m_Offsets; }
        const std::vector<float>&            GetShapeKeyOverrideWeights() const { return m_Overrides; }
        const std::vector<ExpressionPreset>& GetExpressions() const
        {
            static const std::vector<ExpressionPreset> empty;
            return m_Controller ? m_Controller->Expressions : empty;
        }
        bool SetShapeKeyWeight(const std::string&, float weight); // Absolute override until cleared.
        bool AddShapeKeyWeight(const std::string&, float weight); // Persistent additive offset.
        bool ClearShapeKeyOverride(const std::string&);
        bool SetExpression(const std::string&, double seconds);
        // Applies entity root motion before modifiers/IK, so world-space targets need only one solve.
        using RootMotionSink = std::function<void(const RootMotionDelta&)>;
        void Advance(double seconds, const AnimationEventSink& sink = {}, const RootMotionSink& applyMotion = {});
        // Independent preview: samples one state without changing parameters, playback times, triggers or events.
        bool EvaluateState(size_t layer, uint32_t state, double time, AnimationPose& output);
        // Explicit event query for editor scrubbing. Does not change playback state.
        void ScrubEvents(size_t layer, uint32_t state, double from, double to, const AnimationEventSink&) const;
        const AnimationPose&                    GetPose() const { return m_Pose; }
        AnimationPose&                          MutablePose() { return m_Pose; }
        const std::vector<AnimatorLayerStatus>& GetLayers() const { return m_Status; }
        const RootMotionDelta&                  GetRootMotion() const { return m_RootDelta; }
        const std::vector<BoundAnimationClip>&  GetBindings() const { return m_Clips; }
        // Post-sampling modifier hook: local pose/morphs -> modifiers -> model/palette -> scene entities/GPU.
        void SetPoseModifier(std::function<void(const SkeletonData&, AnimationPose&)> modifier)
        {
            m_Modifier = std::move(modifier);
        }
        // Runtime overrides are instance-local and apply on the next update/rebuild/preview (no damping).
        // Authoring edits belong in the controller + ResetAnimator.
        bool SetIKTarget(const std::string& name, const IKTarget&);
        bool SetIKWeight(const std::string& name, float);
        bool SetIKPole(const std::string& name, glm::vec3 worldDirection, uint64_t entity = 0);
        bool SetIKRotation(const std::string& name, glm::quat worldRotation, float weight);
        using IKEntityResolver = std::function<bool(uint64_t, glm::mat4& world)>;
        void SetIKEntityResolver(IKEntityResolver resolver) { m_IKResolver = std::move(resolver); }
        void SetIKWorldTransform(const glm::mat4& world) { m_IKWorld = world; }
        const std::vector<std::string>& GetIKWarnings() const { return m_IKWarnings; }
        bool                            HasIK() const { return !m_TwoBone.empty() || !m_LookAt.empty(); }
        void                            RebuildPose();
        uint64_t                        DiscontinuityVersion = 0;
        bool EvaluateControllerIK = true; // Legacy standalone CPU API only; scene uses IKComponent.
        std::map<std::string, float> BaseShapeKeyWeights;
        bool   ExtractRootMotion = false; // Removes sampled root T/R from pose; caller applies GetRootMotion().
        double Speed             = 1;

    private:
        friend void EvaluateIKConstraints(const SkeletonData&,
                                          const IKComponent&,
                                          AnimationPose&,
                                          const glm::mat4&,
                                          const IKEntityResolver&);
        void        PrepareIK();
        void        EvaluateIK(AnimationPose&);
        void        RefreshIK(const AnimationPose&, int32_t joint);
        void        RotateIK(AnimationPose&, int32_t joint, glm::quat modelRotation);
        bool        ResolveIK(const IKTarget&, glm::vec3&, glm::quat&) const;
        struct BoundTwoBone
        {
            TwoBoneIK Data;
            int32_t   Root = -1, Middle = -1, End = -1;
        };
        struct BoundLookAt
        {
            LookAtIK             Data;
            std::vector<int32_t> Joints;
        };
        std::vector<BoundTwoBone> m_TwoBone;
        std::vector<BoundLookAt>  m_LookAt;
        std::vector<std::string>  m_IKWarnings;
        std::vector<glm::mat4>    m_IKModel;
        std::vector<glm::quat>    m_IKRotations;
        std::vector<uint8_t>      m_IKDirty;
        std::vector<uint64_t>     m_IKGeneration, m_IKParentGeneration;
        std::vector<int32_t>      m_IKChain;
        glm::mat4                 m_IKWorld {1}, m_IKInverse {1};
        glm::quat                 m_IKWorldRotation {1, 0, 0, 0};
        IKEntityResolver          m_IKResolver;
        int32_t                   Parameter(const std::string&) const;
        bool                      Set(const std::string&, double, AnimationParameterType);
        double                    Value(const std::string&) const;
        double                    Duration(uint32_t motion) const;
        void                      SampleMotion(uint32_t motion, double time, bool loop, AnimationPose&);
        void                      StatePose(size_t layer, uint32_t state, double time, AnimationPose&);
        void MotionEvents(uint32_t motion, double from, double to, bool loop, const AnimationEventSink&, uint32_t layer)
            const;
        JointTransform  InitialRoot(uint32_t motion);
        JointTransform  LayerRootReference(size_t layer);
        JointTransform  MotionRoot(uint32_t motion, double time, bool loop);
        RootMotionDelta StateRootDelta(size_t layer, uint32_t state, double from, double to);
        bool            Conditions(const AnimationTransition&) const;
        bool            Exit(const AnimationTransition&, size_t layer, uint32_t state, double from, double to) const;
        void            Consume(const AnimationTransition&);
        struct ActionWork
        {
            RootMotionDelta    RootDelta;
            float              RootCoverage = 0;
            ActionData         Data;
            BoundAnimationClip Binding;
            AnimationPose      Pose, Frozen;
            size_t             Section = 0;
            double             Age = 0, StopElapsed = 0, StopDuration = 0;
            float              FrozenWeight = 0, StopWeight = 0;
            bool               HasFrozen = false, Stopping = false, AtEnd = false, Entered = true;
            int64_t            Cycle = 0;
        };
        int32_t SlotLayer(const std::string&) const;
        bool    StartAction(AssetID, ActionData, const std::string&, double);
        float   ActionWeight(size_t) const;
        void    SampleAction(size_t);
        void    ApplyAction(size_t, AnimationPose&);
        void    ActionEvent(size_t, AnimationEventKind, const AnimationEvent* = nullptr, bool interrupted = false);
        void    FinishAction(size_t, bool interrupted = false);
        void    AdvanceAction(size_t, double seconds, RootMotionDelta&);
        JointTransform ActionRoot(size_t, double);
        ClipResolver   m_ClipResolver;
        ActionResolver m_ActionResolver;
        struct TimelineSlotPose
        {
            AnimationPose Pose;
            float         Weight = 0;
        };
        std::map<size_t, TimelineSlotPose>    m_TimelinePoses;
        std::vector<ActionWork>               m_Actions;
        std::vector<AnimatorActionStatus>     m_ActionStatus;
        std::vector<AnimationEventOccurrence> m_ActionEvents;
        std::optional<AssetError>             m_ActionError;
        struct LayerWork
        {
            AnimationPose      Source, Target, Frozen;
            bool               UseFrozen = false;
            JointTransform     FrozenRootReference;
            std::vector<float> Mask, MorphMask, DrivenMorphMask;
            float              Weight = 1;
            RootMotionDelta    RootDelta;
        };
        const SkeletonData*                     m_Skeleton   = nullptr;
        const AnimationControllerData*          m_Controller = nullptr;
        int32_t                                 m_Root       = -1;
        std::vector<double>                     m_Parameters;
        std::vector<BoundAnimationClip>         m_Clips;
        std::vector<AnimationPose>              m_MotionPoses;
        mutable std::vector<std::vector<float>> m_Weights;
        std::vector<LayerWork>                  m_Work;
        std::vector<AnimatorLayerStatus>        m_Status;
        AnimationPose                           m_Pose, m_Rest;
        RootMotionDelta                         m_RootDelta;
        std::vector<float>                      m_Overrides, m_Offsets, m_Expression, m_ExpressionFrom, m_ExpressionTo;
        std::vector<uint8_t>                    m_HasOverride;
        double                                  m_ExpressionElapsed = 0, m_ExpressionDuration = 0;
        std::function<void(const SkeletonData&, AnimationPose&)> m_Modifier;
    };
} // namespace Kans
