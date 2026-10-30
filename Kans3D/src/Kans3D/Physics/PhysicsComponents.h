#pragma once
#include <optional>
#include "ForceFields.h"
#include "PhysicsSettings.h"
#include "Shapes.h"
namespace Kans
{
    enum class RigidBodyType : uint8_t
    {
        Static,
        Dynamic,
        Kinematic
    };
    enum class ContinuousDetectionMode : uint8_t
    {
        Off,
        CCD,
        Speculative
    };
    enum class PhysicsInterpolation : uint8_t
    {
        None,
        Interpolate
    };
    struct RigidBodyComponent
    {
        RigidBodyType           Type     = RigidBodyType::Dynamic;
        float                   Mass     = 1;
        bool                    AutoMass = false;
        glm::vec3               CenterOfMassOffset {0};
        float                   LinearDamping = 0, AngularDamping = .05f, GravityScale = 1;
        std::array<bool, 3>     LockLinear {}, LockAngular {};
        ContinuousDetectionMode ContinuousDetection = ContinuousDetectionMode::Off;
        PhysicsInterpolation    Interpolation       = PhysicsInterpolation::Interpolate;
        glm::vec3               InitialLinearVelocity {0}, InitialAngularVelocity {0};
        bool                    StartAwake = true, AffectedByWind = false;
        // F = .5 * AirDensity * DragCoefficient * WindArea * |wind-velocity| * (wind-velocity).
        float    WindArea = 1, DragCoefficient = 1, AirDensity = 1.225f;
        uint32_t Layer = 0;
    };
    struct ColliderMaterial
    {
        // Loaded asset > inline when !UseDefault > project default. A failed asset uses project default.
        AssetID               Asset;
        bool                  UseDefault = true;
        PhysicsMaterialValues Values;
    };
    struct ColliderProperties
    {
        bool                    IsTrigger = false;
        float                   Density   = 1000;
        ColliderMaterial        Material;
        std::optional<uint32_t> Layer;
    };
    struct BoxColliderComponent : ColliderProperties
    {
        Physics::Box Shape;
    };
    struct SphereColliderComponent : ColliderProperties
    {
        Physics::Sphere Shape;
    };
    struct CapsuleColliderComponent : ColliderProperties
    {
        Physics::Capsule Shape;
    };
    struct MeshColliderComponent : ColliderProperties
    {
        bool Convex = true;
        // Invalid Mesh uses the entity's MeshComponent (or EditableMeshComponent).
        Physics::ConvexMesh Shape;
    };
} // namespace Kans
