#pragma once
namespace Kans
{
    template<class C, class F>
    void VehicleMaterialFrictionFields(C& c, F f)
    {
        f("Material", c.Material);
        f("Multiplier", c.Multiplier);
    }
    template<class C, class F>
    void VehicleAntiRollBarFields(C& c, F f)
    {
        f("WheelA", c.WheelA);
        f("WheelB", c.WheelB);
        f("Stiffness", c.Stiffness);
    }
    template<class C, class F>
    void VehicleWheelFields(C& c, F f)
    {
        f("Visual", c.Visual);
        f("Axle", c.Axle);
        f("AttachPosition", c.AttachPosition);
        f("SuspensionDirection", c.SuspensionDirection);
        f("Radius", c.Radius);
        f("Width", c.Width);
        f("Mass", c.Mass);
        f("MomentOfInertia", c.MomentOfInertia);
        f("SuspensionTravel", c.SuspensionTravel);
        f("SpringStiffness", c.SpringStiffness);
        f("SpringDamping", c.SpringDamping);
        f("LongitudinalStiffness", c.LongitudinalStiffness);
        f("LateralStiffness", c.LateralStiffness);
        f("FrictionVsSlip", c.FrictionVsSlip);
        f("MaterialFriction", c.MaterialFriction);
        f("BrakeTorque", c.BrakeTorque);
        f("HandbrakeTorque", c.HandbrakeTorque);
        f("MaxSteerAngle", c.MaxSteerAngle);
        f("Driven", c.Driven);
        f("TorqueSplit", c.TorqueSplit);
    }
    template<class C, class F>
    void VehicleComponentFields(C& c, F f)
    {
        f("DriveModel", c.DriveModel);
        f("Wheels", c.Wheels);
        f("AntiRollBars", c.AntiRollBars);
        f("DirectDriveTorque", c.DirectDriveTorque);
        f("Ackermann", c.Ackermann);
        f("TorqueCurve", c.TorqueCurve);
        f("MaxTorque", c.MaxTorque);
        f("MaxRpm", c.MaxRpm);
        f("IdleRpm", c.IdleRpm);
        f("EngineMomentOfInertia", c.EngineMomentOfInertia);
        f("EngineDamping", c.EngineDamping);
        f("GearRatios", c.GearRatios);
        f("ReverseRatio", c.ReverseRatio);
        f("FinalDrive", c.FinalDrive);
        f("SwitchTime", c.SwitchTime);
        f("UpShiftRatio", c.UpShiftRatio);
        f("DownShiftRatio", c.DownShiftRatio);
        f("ClutchStrength", c.ClutchStrength);
        f("Differential", c.Differential);
        f("TorqueBias", c.TorqueBias);
        f("RoadQuery", c.RoadQuery);
        f("RoadLayerMask", c.RoadLayerMask);
        f("LowSpeedSubsteps", c.LowSpeedSubsteps);
        f("HighSpeedSubsteps", c.HighSpeedSubsteps);
        f("SubstepSpeedThreshold", c.SubstepSpeedThreshold);
        f("Throttle", c.Throttle);
        f("Brake", c.Brake);
        f("Steer", c.Steer);
        f("Handbrake", c.Handbrake);
        f("TargetGear", c.TargetGear);
        f("AutomaticGearbox", c.AutomaticGearbox);
    }
} // namespace Kans
