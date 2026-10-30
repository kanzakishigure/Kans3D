#pragma once
#include <functional>
#include "Kans3D/Physics/SecondaryMotion.h"
#include "Kans3D/Timeline/Timeline.h"
namespace Kans
{
    enum class SpringRotationAxis : uint8_t
    {
        All,
        X,
        Y,
        Z
    };
    struct SpringBoneChain
    {
        UUID               RootBone = 0, EndBone = 0;
        std::vector<UUID>  ExcludedBones;
        float              EndExtension = 0;
        float              Stiffness = 40, Drag = .15f, GravityPower = 9.81f, HitRadius = .02f;
        glm::vec3          GravityDirection {0, -1, 0};
        SpringRotationAxis RotationAxis = SpringRotationAxis::All;
        float              AngleLimit   = 180; // Degrees; 180 disables the cone.
        // Existing timeline curves: normalized chain coordinate replaces time. Empty = base value.
        TimelineCurve StiffnessCurve, DragCurve, GravityPowerCurve, GravityDirectionCurve, HitRadiusCurve,
            RotationAxisCurve, AngleLimitCurve;
        float StiffnessMultiplier = 1, DragMultiplier = 1, GravityMultiplier = 1;
    };
    struct SpringBoneSettings
    {
        std::vector<SpringBoneChain> Chains;
        UUID                         Center       = 0;
        float                        InertiaScale = 1, WindScale = 1;
        std::vector<UUID>            ColliderGroups;
        float                        SubstepRate = 60;
        uint32_t                     MaxSubsteps = 8;
        float TeleportDistance = 2, TeleportRotation = 90; // World units / degrees; zero disables.
        bool  Enabled = true;
    };
    struct SpringBoneRuntime;
    struct SpringBoneComponent : SpringBoneSettings
    {
        Ref<SpringBoneRuntime> Runtime; // Instance-local, never serialized.
        SpringBoneComponent() = default;
        SpringBoneComponent(const SpringBoneComponent& c) : SpringBoneSettings(c) {}
        SpringBoneComponent& operator=(const SpringBoneComponent& c)
        {
            if (this != &c)
            {
                SpringBoneSettings::operator=(c);
                Runtime.reset();
            }
            return *this;
        }
        SpringBoneComponent(SpringBoneComponent&&) noexcept            = default;
        SpringBoneComponent& operator=(SpringBoneComponent&&) noexcept = default;
        void                 Reset();
        void                 RemapEntities(const std::function<uint64_t(uint64_t)>&);
    };
    struct SpringBoneColliderComponent
    {
        Physics::MotionColliderShape Shape  = Physics::Sphere {};
        bool                         Inside = false;
    };
    bool        operator==(const SpringBoneChain&, const SpringBoneChain&);
    bool        operator==(const SpringBoneComponent&, const SpringBoneComponent&);
    bool        operator==(const SpringBoneColliderComponent&, const SpringBoneColliderComponent&);
    inline bool operator!=(const SpringBoneComponent& a, const SpringBoneComponent& b) { return !(a == b); }
    inline bool operator!=(const SpringBoneColliderComponent& a, const SpringBoneColliderComponent& b)
    {
        return !(a == b);
    }
    // Read-only diagnostics use center-space positions. RenderRotation is local, after interpolation.
    struct SpringBoneJointState
    {
        UUID      Bone = 0;
        glm::vec3 PreviousTip {0}, Tip {0};
        glm::quat PreviousRotation {1, 0, 0, 0}, Rotation {1, 0, 0, 0}, RenderRotation {1, 0, 0, 0};
    };
} // namespace Kans
