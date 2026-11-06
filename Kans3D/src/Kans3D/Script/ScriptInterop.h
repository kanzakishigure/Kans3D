#pragma once
#include <cstdint>

#if defined(_WIN32)
#define KANS_SCRIPT_CALL __cdecl
#else
#define KANS_SCRIPT_CALL
#endif

namespace Kans::Scripting
{
    // ABI 12 appends MouseState and LoadScene; native and managed tables must change together.
    constexpr uint32_t ApiVersion = 12;
    enum class ComponentType : uint32_t
    {
#define KANS_COMPONENT(id, name) name = id,
#include "ComponentTypes.def"
#undef KANS_COMPONENT
    };
    struct Vector3
    {
        float X, Y, Z;
    };
    struct Value
    {
        float   X = 0, Y = 0, Z = 0, W = 0;
        int64_t Integer = 0;
    };
    // Path/key pointers are borrowed UTF-8 only for the duration of a call. Never engine pointers.
    struct WidgetAddress
    {
        uint64_t    Entity = 0;
        int32_t     Screen = 1, Reserved = 0;
        const char* Path = nullptr;
        const char* Key  = nullptr;
    };
    struct UIEventData
    {
        uint64_t Entity = 0;
        int32_t  Screen = 1, Type = 0, ValueKind = 0, Reserved = 0;
        Value    Payload;
        float    PositionX = 0, PositionY = 0, DeltaX = 0, DeltaY = 0;
    };
    struct AnimationEventData
    {
        uint64_t Entity = 0;
        double   Time   = 0;
        int32_t  Kind = 0, Interrupted = 0;
    };
    struct TimelineEventData
    {
        uint64_t Player = 0, Target = 0;
        double   Time     = 0;
        int32_t  Finished = 0, Reserved = 0;
    };
    struct PhysicsQueryData
    {
        uint32_t Kind = 0, LayerMask = UINT32_MAX, Flags = 7, Triggers = 0;
        uint64_t Ignore = 0;
        Vector3  Origin {}, Direction {}, HalfExtents {};
        float    Distance = 0, Radius = .5f, HalfHeight = .5f;
        float    RotationX = 0, RotationY = 0, RotationZ = 0, RotationW = 1;
        uint32_t Axis = 1;
    };
    struct PhysicsHitData
    {
        uint64_t Collider = 0, Body = 0;
        Vector3  Point {}, Normal {};
        float    Distance  = 0;
        uint32_t FaceIndex = UINT32_MAX, InitialOverlap = 0, Reserved = 0;
        char     BoneName[128] {};
    };
    struct PhysicsContactData
    {
        Vector3  Position {}, Normal {}, Impulse {};
        float    Separation = 0;
        uint64_t ColliderA = 0, ColliderB = 0;
        char     BoneName[128] {}, OtherBoneName[128] {};
    };
    struct ControllerHitData
    {
        uint64_t Collider = 0, Body = 0;
        Vector3  Point {}, Normal {}, MoveDirection {};
        float    MoveLength = 0;
    };
    struct PhysicsEventData
    {
        uint64_t                  Other = 0, Collider = 0, OtherCollider = 0;
        Vector3                   Impulse {}, RelativeVelocity {};
        const PhysicsContactData* Contacts = nullptr;
        int32_t                   Count = 0, Phase = 0, Trigger = 0, Removed = 0;
    };
    // ABI v7: UTF-8, NUL terminated strings; Cdecl callbacks; int32 booleans/status;
    // no engine pointers cross this boundary. Handles are opaque uint64 values.
    struct NativeApi
    {
        uint32_t Version = ApiVersion, Size = sizeof(NativeApi);
        void(KANS_SCRIPT_CALL* Log)(int32_t, const char*)                                 = nullptr;
        int32_t(KANS_SCRIPT_CALL* HasComponent)(uint64_t, uint32_t)                       = nullptr;
        void(KANS_SCRIPT_CALL* GetTranslation)(uint64_t, Vector3*)                        = nullptr;
        void(KANS_SCRIPT_CALL* SetTranslation)(uint64_t, const Vector3*)                  = nullptr;
        void(KANS_SCRIPT_CALL* GetRotation)(uint64_t, Vector3*)                           = nullptr;
        void(KANS_SCRIPT_CALL* SetRotation)(uint64_t, const Vector3*)                     = nullptr;
        int32_t(KANS_SCRIPT_CALL* IsKeyPressed)(int32_t)                                  = nullptr;
        void(KANS_SCRIPT_CALL* SetMaterialParameter)(uint64_t, const char*, const float*) = nullptr;
        void(KANS_SCRIPT_CALL* ClearMaterialParameter)(uint64_t, const char*)             = nullptr;
        int32_t(KANS_SCRIPT_CALL* GetMaterialParameter)(uint64_t, const char*, float*)    = nullptr;
        int32_t(KANS_SCRIPT_CALL* AddComponent)(uint64_t, uint32_t)                       = nullptr;
        int32_t(KANS_SCRIPT_CALL* DestroyEntity)(uint64_t)                                = nullptr;
        // Returns 0 for a missing target, -1 for an invalid property/value; otherwise >=1.
        // String reads return required capacity including NUL, and never truncate.
        int32_t(KANS_SCRIPT_CALL* ComponentProperty)(uint64_t, uint32_t, const char*, int32_t, Value*, char*, int32_t) =
            nullptr;
        int32_t(KANS_SCRIPT_CALL* WidgetProperty)(const WidgetAddress*, const char*, int32_t, Value*, char*, int32_t) =
            nullptr;
        int32_t(KANS_SCRIPT_CALL* ReadUIEvent)(int32_t, UIEventData*, int32_t, char*, int32_t)               = nullptr;
        int32_t(KANS_SCRIPT_CALL* UIUtility)(int32_t, const WidgetAddress*, Value*)                          = nullptr;
        int32_t(KANS_SCRIPT_CALL* AnimationCommand)(uint64_t,
                                                    int32_t,
                                                    const char*,
                                                    const char*,
                                                    const char*,
                                                    const char*,
                                                    float,
                                                    float,
                                                    float)                                                   = nullptr;
        int32_t(KANS_SCRIPT_CALL* ReadAnimationEvent)(int32_t, AnimationEventData*, int32_t, char*, int32_t) = nullptr;
        int32_t(KANS_SCRIPT_CALL* TimelineCommand)(uint64_t, int32_t, const char*, uint64_t, double*)        = nullptr;
        int32_t(KANS_SCRIPT_CALL* ReadTimelineEvent)(int32_t, TimelineEventData*, int32_t, char*, int32_t)   = nullptr;
        int32_t(KANS_SCRIPT_CALL* PhysicsCommand)(uint64_t, int32_t, int32_t, Value*, Vector3*, const char*) = nullptr;
        int32_t(KANS_SCRIPT_CALL* PhysicsQuery)(const PhysicsQueryData*, PhysicsHitData*, int32_t, int32_t)  = nullptr;
        int32_t(KANS_SCRIPT_CALL* VfxCommand)(uint64_t, int32_t, int32_t, const char*, Value*, const Value*) = nullptr;
        // Mouse queries (ScriptInput): 0 position, 1 delta, 2 scroll, 3 button held.
        int32_t(KANS_SCRIPT_CALL* MouseState)(int32_t, int32_t, Value*) = nullptr;
        // Queues a scene switch at the end of the frame: 1 queued, 0 not playing, -1 invalid path.
        int32_t(KANS_SCRIPT_CALL* LoadScene)(const char*) = nullptr;
    };
    struct ManagedApi
    {
        uint32_t Version = ApiVersion, Size = sizeof(ManagedApi);
        void(KANS_SCRIPT_CALL* Shutdown)()                                        = nullptr;
        int32_t(KANS_SCRIPT_CALL* LoadAssembly)(const char*)                      = nullptr;
        int32_t(KANS_SCRIPT_CALL* ReloadAssembly)()                               = nullptr;
        int32_t(KANS_SCRIPT_CALL* ClassCount)()                                   = nullptr;
        int32_t(KANS_SCRIPT_CALL* ClassName)(int32_t, char*, int32_t)             = nullptr;
        int32_t(KANS_SCRIPT_CALL* ClassExists)(const char*)                       = nullptr;
        uint64_t(KANS_SCRIPT_CALL* Create)(uint64_t, const char*)                 = nullptr;
        void(KANS_SCRIPT_CALL* Destroy)(uint64_t)                                 = nullptr;
        void(KANS_SCRIPT_CALL* Update)(const uint64_t*, int32_t, float)           = nullptr;
        void(KANS_SCRIPT_CALL* SetPlaying)(int32_t)                               = nullptr;
        int64_t(KANS_SCRIPT_CALL* AllocatedBytes)()                               = nullptr;
        int32_t(KANS_SCRIPT_CALL* LastUnloadCollected)()                          = nullptr;
        void(KANS_SCRIPT_CALL* FixedUpdate)(const uint64_t*, int32_t, float)      = nullptr;
        void(KANS_SCRIPT_CALL* PhysicsEvent)(uint64_t, const PhysicsEventData*)   = nullptr;
        uint32_t(KANS_SCRIPT_CALL* PhysicsCallbacks)(uint64_t)                    = nullptr;
        void(KANS_SCRIPT_CALL* ControllerHit)(uint64_t, const ControllerHitData*) = nullptr;
        void(KANS_SCRIPT_CALL* JointBreak)(uint64_t, uint64_t)                    = nullptr;
    };
    using InitializeFn = int32_t(KANS_SCRIPT_CALL*)(const NativeApi*, int32_t, ManagedApi*, int32_t);
    static_assert(sizeof(Vector3) == 12);
    static_assert(sizeof(Value) == 24);
    static_assert(sizeof(UIEventData) == 64);
    static_assert(sizeof(NativeApi) == 8 + 25 * sizeof(void*));
    static_assert(sizeof(ManagedApi) == 8 + 17 * sizeof(void*));
} // namespace Kans::Scripting
