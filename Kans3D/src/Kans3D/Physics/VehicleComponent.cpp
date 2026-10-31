#include "VehicleComponent.h"
namespace Kans
{
    bool operator==(const VehicleAntiRollBar& a, const VehicleAntiRollBar& b)
    {
        return std::tie(a.WheelA, a.WheelB, a.Stiffness) == std::tie(b.WheelA, b.WheelB, b.Stiffness);
    }
    bool operator==(const VehicleWheel& a, const VehicleWheel& b)
    {
        return std::tie(a.Visual,
                        a.Axle,
                        a.AttachPosition,
                        a.SuspensionDirection,
                        a.Radius,
                        a.Width,
                        a.Mass,
                        a.MomentOfInertia,
                        a.SuspensionTravel,
                        a.SpringStiffness,
                        a.SpringDamping,
                        a.LongitudinalStiffness,
                        a.LateralStiffness,
                        a.FrictionVsSlip,
                        a.MaterialFriction,
                        a.BrakeTorque,
                        a.HandbrakeTorque,
                        a.MaxSteerAngle,
                        a.Driven,
                        a.TorqueSplit) == std::tie(b.Visual,
                                                   b.Axle,
                                                   b.AttachPosition,
                                                   b.SuspensionDirection,
                                                   b.Radius,
                                                   b.Width,
                                                   b.Mass,
                                                   b.MomentOfInertia,
                                                   b.SuspensionTravel,
                                                   b.SpringStiffness,
                                                   b.SpringDamping,
                                                   b.LongitudinalStiffness,
                                                   b.LateralStiffness,
                                                   b.FrictionVsSlip,
                                                   b.MaterialFriction,
                                                   b.BrakeTorque,
                                                   b.HandbrakeTorque,
                                                   b.MaxSteerAngle,
                                                   b.Driven,
                                                   b.TorqueSplit);
    }
    bool operator==(const VehicleComponent& a, const VehicleComponent& b)
    {
        return std::tie(a.DriveModel,
                        a.Wheels,
                        a.AntiRollBars,
                        a.DirectDriveTorque,
                        a.Ackermann,
                        a.MaxTorque,
                        a.MaxRpm,
                        a.IdleRpm,
                        a.EngineMomentOfInertia,
                        a.EngineDamping,
                        a.GearRatios,
                        a.ReverseRatio,
                        a.FinalDrive,
                        a.SwitchTime,
                        a.UpShiftRatio,
                        a.DownShiftRatio,
                        a.ClutchStrength,
                        a.Differential,
                        a.TorqueBias,
                        a.RoadQuery,
                        a.RoadLayerMask,
                        a.LowSpeedSubsteps,
                        a.HighSpeedSubsteps,
                        a.SubstepSpeedThreshold,
                        a.Throttle,
                        a.Brake,
                        a.Steer,
                        a.Handbrake,
                        a.TargetGear,
                        a.AutomaticGearbox) == std::tie(b.DriveModel,
                                                        b.Wheels,
                                                        b.AntiRollBars,
                                                        b.DirectDriveTorque,
                                                        b.Ackermann,
                                                        b.MaxTorque,
                                                        b.MaxRpm,
                                                        b.IdleRpm,
                                                        b.EngineMomentOfInertia,
                                                        b.EngineDamping,
                                                        b.GearRatios,
                                                        b.ReverseRatio,
                                                        b.FinalDrive,
                                                        b.SwitchTime,
                                                        b.UpShiftRatio,
                                                        b.DownShiftRatio,
                                                        b.ClutchStrength,
                                                        b.Differential,
                                                        b.TorqueBias,
                                                        b.RoadQuery,
                                                        b.RoadLayerMask,
                                                        b.LowSpeedSubsteps,
                                                        b.HighSpeedSubsteps,
                                                        b.SubstepSpeedThreshold,
                                                        b.Throttle,
                                                        b.Brake,
                                                        b.Steer,
                                                        b.Handbrake,
                                                        b.TargetGear,
                                                        b.AutomaticGearbox) &&
               [&] {
                   if (a.TorqueCurve.Keys.size() != b.TorqueCurve.Keys.size())
                       return false;
                   for (size_t i = 0; i < a.TorqueCurve.Keys.size(); ++i)
                   {
                       auto& x = a.TorqueCurve.Keys[i];
                       auto& y = b.TorqueCurve.Keys[i];
                       if (std::tie(x.Time, x.Value, x.InTangent, x.OutTangent, x.Interpolation) !=
                           std::tie(y.Time, y.Value, y.InTangent, y.OutTangent, y.Interpolation))
                           return false;
                   }
                   return true;
               }();
    }
} // namespace Kans
