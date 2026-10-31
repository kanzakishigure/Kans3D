#pragma once
#include <cfloat>
#include <limits>
#include <optional>
#include "Kans3D/Core/UUID.h"
#include "ForceFields.h"
#include "PhysicsSettings.h"
#include "Shapes.h"
namespace Kans
{

    enum class ControllerShape : uint8_t
    {
        Capsule,
        Box
    };
    enum class NonWalkableMode : uint8_t
    {
        PreventClimbing,
        ForceSliding
    };
    // Height is the capsule's cylindrical section (total height = Height + 2 * Radius).
    // Sizes/Center are in world units; rotation/scale do not rotate UpDirection.
    struct CharacterControllerComponent
    {
        ControllerShape Shape  = ControllerShape::Capsule;
        float           Radius = .5f;
        float           Height = 1;
        glm::vec3       HalfExtents {.5f, 1, .5f};
        glm::vec3       Center {0};
        uint32_t        Layer           = 0;
        uint32_t        CollisionMask   = UINT32_MAX;
        float           SlopeLimit      = 45;
        NonWalkableMode NonWalkable     = NonWalkableMode::PreventClimbing;
        float           StepOffset      = .3f;
        float           SkinWidth       = .05f;
        float           MinMoveDistance = .001f;
        glm::vec3       UpDirection {0, 1, 0};
        bool            PushRigidBodies = false;
        float           Mass            = 80;
        float           PushForceScale  = 1;
        float           SnapDistance    = 0;
        bool            FollowPlatforms = true;
    };
    enum class JointType : uint8_t
    {
        Fixed,
        Hinge,
        Slider,
        Spherical,
        Distance,
        D6
    };
    enum class JointMotion : uint8_t
    {
        Locked,
        Limited,
        Free
    };
    // All authored angles are degrees; motor angular velocities are radians/second.
    // Joint local +X is hinge/slider/twist; +Y/+Z define swing.
    struct JointComponent
    {
        JointType                  Type                    = JointType::Fixed;
        UUID                       BodyA                   = 0;
        UUID                       BodyB                   = 0;
        bool                       EnableCollision         = false;
        float                      BreakForce              = std::numeric_limits<float>::max();
        float                      BreakTorque             = std::numeric_limits<float>::max();
        float                      InvMassScaleA           = 1;
        float                      InvMassScaleB           = 1;
        float                      InvInertiaScaleA        = 1;
        float                      InvInertiaScaleB        = 1;
        bool                       HingeLimitEnabled       = false;
        float                      HingeLowerAngle         = -90;
        float                      HingeUpperAngle         = 90;
        float                      HingeLimitStiffness     = 0;
        float                      HingeLimitDamping       = 0;
        bool                       HingeMotorEnabled       = false;
        float                      HingeMotorVelocity      = 0;
        float                      HingeMotorMaxForce      = std::numeric_limits<float>::max();
        bool                       HingeFreeSpin           = false;
        bool                       HingeSpringEnabled      = false;
        float                      HingeTargetAngle        = 0;
        float                      HingeSpringStiffness    = 0;
        float                      HingeSpringDamping      = 0;
        bool                       SliderLimitEnabled      = false;
        float                      SliderLowerLimit        = -1;
        float                      SliderUpperLimit        = 1;
        float                      SliderLimitStiffness    = 0;
        float                      SliderLimitDamping      = 0;
        bool                       SliderDriveEnabled      = false;
        float                      SliderTargetPosition    = 0;
        float                      SliderTargetVelocity    = 0;
        float                      SliderDriveStiffness    = 0;
        float                      SliderDriveDamping      = 0;
        float                      SliderDriveMaxForce     = std::numeric_limits<float>::max();
        bool                       SphericalLimitEnabled   = false;
        float                      SphericalYAngle         = 45;
        float                      SphericalZAngle         = 45;
        float                      SphericalLimitStiffness = 0;
        float                      SphericalLimitDamping   = 0;
        bool                       DistanceMinEnabled      = false;
        bool                       DistanceMaxEnabled      = true;
        float                      DistanceMin             = 0;
        float                      DistanceMax             = 1;
        float                      DistanceStiffness       = 0;
        float                      DistanceDamping         = 0;
        float                      DistanceTolerance       = .025f;
        std::array<JointMotion, 6> D6Motion                = {};
        glm::vec3                  D6LinearLower {-1};
        glm::vec3                  D6LinearUpper {1};
        float                      D6LinearStiffness  = 0;
        float                      D6LinearDamping    = 0;
        float                      D6TwistLower       = -45;
        float                      D6TwistUpper       = 45;
        float                      D6SwingY           = 45;
        float                      D6SwingZ           = 45;
        float                      D6AngularStiffness = 0;
        float                      D6AngularDamping   = 0;
        glm::vec3                  DrivePosition {0};
        glm::quat                  DriveRotation {1, 0, 0, 0};
        glm::vec3                  DriveLinearVelocity {0};
        glm::vec3                  DriveAngularVelocity {0};
        std::array<float, 6>       DriveStiffness = {};
        std::array<float, 6>       DriveDamping   = {};
        std::array<float, 6>       DriveMaxForce  = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
        bool                       SlerpDrive     = false;
    };
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
        bool                    StartAwake = true, AffectedByWind = false, AffectedByFields = true;
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
