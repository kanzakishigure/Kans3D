#pragma once
#include <set>
#include "Kans3D/Animation/Animation.h"
#include "Kans3D/Core/UUID.h"
#include "Kans3D/VFX/ParticleSystemComponent.h"

namespace Kans
{
    class VfxSystemAsset;
    // Tangents are derivatives (value/second). Bezier uses controls V0 + out*dt/3, V1 - in*dt/3.
    enum class TimelineInterpolation : uint8_t
    {
        Constant,
        Linear,
        Bezier
    };
    struct TimelineKey
    {
        double                Time = 0;
        glm::vec4             Value {0}, InTangent {0}, OutTangent {0};
        TimelineInterpolation Interpolation = TimelineInterpolation::Linear;
    };
    struct TimelineCurve
    {
        std::vector<TimelineKey> Keys;
        glm::vec4                Sample(double time, glm::vec4 fallback = {}) const;
    };
    struct TimelineRange
    {
        double Start = 0, End = 0;
        bool   Contains(double t) const { return t >= Start && t < End; }
    };
    enum class TimelineBindingKind : uint8_t
    {
        Possessable,
        Spawnable
    };
    struct TimelineBinding
    {
        std::string              ID, Name;
        TimelineBindingKind      Kind = TimelineBindingKind::Possessable;
        AssetID                  Prefab, Skeleton; // Skeleton is optional for plain clip evaluation without Animator.
        JointTransform           SpawnTransform;
        TimelineRange            Lifetime {-1, 0}; // Start=-1 derives first track start; End=0 means timeline length.
        std::vector<std::string> ExpectedComponents;
        // Spawnable root component, since model prefabs contain geometry rather than VFX components.
        std::optional<ParticleSystemComponent> Particles;
    };
    struct TimelineTransformTrack
    {
        std::string   Binding;
        bool          Relative = true;
        TimelineCurve Position, Rotation, Scale; // Rotation: Euler radians.
    };
    struct TimelinePropertyTrack
    {
        std::string   Binding, Path;
        TimelineCurve Curve;
    };
    struct TimelineActivationTrack
    {
        std::string                Binding;
        std::vector<TimelineRange> Ranges;
    };
    struct TimelineAnimationSection
    {
        AssetID Clip;
        double  Start = 0, End = 0, ClipIn = 0, ClipOut = 0, Speed = 1, BlendIn = 0, BlendOut = 0;
        // ClipOut=0 uses source duration; timeline End is explicit. Trim is clamped, never loops.
    };
    struct TimelineAnimationTrack
    {
        std::string                           Binding, Slot; // Empty: "Timeline" if present, otherwise first AN4 slot.
        std::vector<TimelineAnimationSection> Clips;
    };
    enum class TimelineParticleOnEnd : uint8_t
    {
        Finish,
        Kill
    };
    struct TimelineParticleSection
    {
        double                Start = 0, End = 0;
        TimelineParticleOnEnd OnEnd = TimelineParticleOnEnd::Finish;
    };
    struct TimelineParticleParameter
    {
        std::string   Name;
        Vfx::Type     Type = Vfx::Type::Float;
        TimelineCurve Curve;
    };
    struct TimelineParticleTrack
    {
        std::string                            Binding;
        bool                                   DesiredAge        = false;
        float                                  FixedTick         = 1.f / 60;
        double                                 MaxSimulationTime = 10;
        std::vector<TimelineParticleSection>   Sections;
        std::vector<TimelineParticleParameter> Parameters;
    };
    struct TimelineEvent
    {
        double      Time = 0;
        std::string Name, Payload, Binding, Method; // Method(string payload), optional bound script call.
    };
    struct TimelineCameraCut
    {
        double      Start = 0, End = 0;
        std::string Binding;
    };
    struct TimelineData
    {
        double                               Length = 0, FrameRate = 30;
        std::vector<TimelineBinding>         Bindings;
        std::vector<TimelineTransformTrack>  Transforms;
        std::vector<TimelinePropertyTrack>   Properties;
        std::vector<TimelineActivationTrack> Activations;
        std::vector<TimelineAnimationTrack>  Animations;
        std::vector<TimelineParticleTrack>   Particles;
        std::vector<TimelineEvent>           Events;
        std::vector<TimelineCameraCut>       CameraCuts;
    };
    struct TimelineActiveSections
    {
        double Time = 0;
        // Outer index is the track; inner indices identify its half-open [Start, End) sections.
        // Curves on transform/property tracks have no sections and are evaluated at every time.
        // Particle tails may remain visible after a section stops emitting.
        std::vector<std::vector<size_t>> Activations, Animations, Particles;
        std::vector<size_t>              CameraCuts;
    };
    // Same clamping as Evaluate; loop=true wraps as Advance does, including exact Length -> 0.
    TimelineActiveSections TimelineSectionsAt(const TimelineData&, double time, bool loop = false);
    using TimelineAsset = AnimationDataAsset<TimelineData, AssetType::Timeline>;
    AnimationValidation ValidateTimeline(const TimelineData&);
    AnimationValidation ValidateTimelineParticleParameters(const TimelineParticleTrack&, const VfxSystemAsset&);
    TimelineRange       TimelineBindingRange(const TimelineData&, const TimelineBinding&);
    enum class TimelineWrapMode : uint8_t
    {
        Hold,
        Reset
    };
    struct TimelinePlayerComponent
    {
        AssetID                     Timeline;
        std::map<std::string, UUID> Bindings; // Stable binding ID -> scene UUID.
        bool                        PlayOnStart = true, Loop = false;
        double                      Speed    = 1;
        TimelineWrapMode            WrapMode = TimelineWrapMode::Hold;
        Ref<const TimelineAsset>    Source;
        Ref<class TimelinePlayer>   Player;
        TimelinePlayerComponent() = default;
        TimelinePlayerComponent(const TimelinePlayerComponent& other);
        TimelinePlayerComponent& operator=(const TimelinePlayerComponent& other);
        TimelinePlayerComponent(TimelinePlayerComponent&&) noexcept            = default;
        TimelinePlayerComponent& operator=(TimelinePlayerComponent&&) noexcept = default;
    };
    bool operator==(const TimelinePlayerComponent&, const TimelinePlayerComponent&);
    class Scene;
    struct TimelineBoundState;
    class TimelinePlayer
    {
    public:
        TimelinePlayer(Scene&, UUID owner, Ref<const TimelineAsset>);
        ~TimelinePlayer();
        TimelinePlayer(const TimelinePlayer&)            = delete;
        TimelinePlayer& operator=(const TimelinePlayer&) = delete;
        bool            Play();
        void            Pause();
        void            Stop();
        bool            Seek(double time);
        bool            Evaluate(double time); // Silent, including when playing. Does not change the playback cursor.
        void            Advance(double seconds);
        bool   CaptureBoundState(); // Idempotent until restore/Stop. Captures before PrepareAnimator mutates data.
        void   RestoreBoundState();
        bool   SetBinding(const std::string& nameOrID, UUID entity);
        double Time() const { return m_Time; }
        bool   IsPlaying() const { return m_Playing; }
        const std::set<size_t>&          FiredEventIndices() const { return m_Fired; }
        const std::optional<AssetError>& Error() const { return m_Error; }
        using EventSink = std::function<void(const TimelineEvent&)>;
        uint64_t Subscribe(EventSink);
        uint64_t SubscribeFinished(std::function<void()>);
        void     Unsubscribe(uint64_t token);
        // CPU fixtures/editor can supply already loaded immutable assets.
        std::map<AssetID, Ref<const AnimationClipAsset>> Clips;
        std::map<AssetID, Ref<const SkeletonAsset>>      Skeletons;
        std::map<AssetID, Ref<const VfxSystemAsset>>     ParticleSystems;

    private:
        TimelinePlayerComponent*                  Component() const;
        bool                                      Fail(std::string);
        bool                                      CaptureEntity(UUID);
        bool                                      EvaluateAnimation(const TimelineAnimationTrack&, UUID, double);
        bool                                      EvaluateParticles(const TimelineParticleTrack&, UUID, double);
        void                                      RemoveSpawned();
        void                                      RestoreCameras();
        void                                      CrossEvents(double from, double to, bool includeZero = false);
        void                                      Finish();
        Scene&                                    m_Scene;
        UUID                                      m_Owner;
        Ref<const TimelineAsset>                  m_Source;
        std::unique_ptr<TimelineBoundState>       m_State;
        std::map<std::string, UUID>               m_Spawned;
        std::set<size_t>                          m_Fired;
        double                                    m_Time    = 0;
        bool                                      m_Playing = false, m_Completed = false, m_Entered = false;
        uint64_t                                  m_NextToken = 1;
        std::map<uint64_t, EventSink>             m_Events;
        std::map<uint64_t, std::function<void()>> m_Finished;
        std::optional<AssetError>                 m_Error;
    };
} // namespace Kans
