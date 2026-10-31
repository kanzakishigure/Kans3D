#pragma once
#include <functional>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/UUID.h"
#include "Kans3D/Timeline/Timeline.h"
namespace Kans
{
    enum class VehicleDriveModel : uint32_t
    {
        Direct,
        Engine
    };
    enum class VehicleDifferential : uint32_t
    {
        OpenFourWheel,
        OpenFront,
        OpenRear,
        LimitedSlip,
        MultiWheel
    };
    enum class VehicleRoadQuery : uint32_t
    {
        Raycast,
        SphereSweep
    };
    struct VehicleMaterialFriction
    {
        AssetID Material;
        float   Multiplier = 1;
        bool    operator==(const VehicleMaterialFriction& b) const
        {
            return Material == b.Material && Multiplier == b.Multiplier;
        }
    };
    struct VehicleWheel
    {
        UUID                                 Visual = 0;
        uint32_t                             Axle   = 0;
        glm::vec3                            AttachPosition {0}, SuspensionDirection {0, -1, 0};
        float                                Radius = .35f, Width = .2f, Mass = 20, MomentOfInertia = 1;
        float                                SuspensionTravel = .3f, SpringStiffness = 35000, SpringDamping = 4500;
        float                                LongitudinalStiffness = 10000, LateralStiffness = 10000;
        std::array<float, 6>                 FrictionVsSlip {0, 1, .1f, 1, 1, .8f};
        std::vector<VehicleMaterialFriction> MaterialFriction;
        float                                BrakeTorque = 1500, HandbrakeTorque = 0, MaxSteerAngle = 0;
        bool                                 Driven      = true;
        float                                TorqueSplit = 1;
    };
    struct VehicleAntiRollBar
    {
        uint32_t WheelA = 0, WheelB = 1;
        float    Stiffness = 10000;
    };
    struct VehicleComponent
    {
        VehicleDriveModel               DriveModel = VehicleDriveModel::Direct;
        std::vector<VehicleWheel>       Wheels;
        std::vector<VehicleAntiRollBar> AntiRollBars;
        float                           DirectDriveTorque = 500, Ackermann = 1;
        TimelineCurve                   TorqueCurve;
        float MaxTorque = 500, MaxRpm = 6000, IdleRpm = 800, EngineMomentOfInertia = 1, EngineDamping = .15f;
        std::vector<float> GearRatios {3.5f, 2.1f, 1.4f, 1, .8f};
        float ReverseRatio = -3.5f, FinalDrive = 4, SwitchTime = .25f, UpShiftRatio = .8f, DownShiftRatio = .3f,
              ClutchStrength              = 10;
        VehicleDifferential Differential  = VehicleDifferential::OpenFourWheel;
        float               TorqueBias    = 3;
        VehicleRoadQuery    RoadQuery     = VehicleRoadQuery::Raycast;
        uint32_t            RoadLayerMask = UINT32_MAX, LowSpeedSubsteps = 3, HighSpeedSubsteps = 1;
        float               SubstepSpeedThreshold = 5;
        float               Throttle = 0, Brake = 0, Steer = 0, Handbrake = 0;
        int32_t             TargetGear       = 1;
        bool                AutomaticGearbox = true;
        void                RemapEntities(const std::function<uint64_t(uint64_t)>& map)
        {
            for (auto& w : Wheels)
                w.Visual = map(w.Visual);
        }
    };
    struct VehicleWheelState
    {
        bool      Grounded     = false;
        UUID      GroundEntity = 0;
        glm::vec3 ContactPoint {0}, ContactNormal {0};
        float     SuspensionCompression = 0, LongitudinalSlip = 0, LateralSlip = 0, AngularSpeed = 0;
        glm::vec3 Position {0};
        glm::quat Rotation {1, 0, 0, 0};
    };
    struct VehicleState
    {
        float                          ForwardSpeed = 0, EngineRpm = 0, TotalDriveTorque = 0;
        int32_t                        CurrentGear = 0;
        std::vector<VehicleWheelState> Wheels;
    };
    bool        operator==(const VehicleWheel&, const VehicleWheel&);
    bool        operator==(const VehicleAntiRollBar&, const VehicleAntiRollBar&);
    bool        operator==(const VehicleComponent&, const VehicleComponent&);
    inline bool operator!=(const VehicleComponent& a, const VehicleComponent& b) { return !(a == b); }
} // namespace Kans
