#pragma once
#include <tuple>
namespace Kans::PhysicsFields
{
    template<class T, class F>
    void CharacterControllerComponentFields(T& c, F f)
    {
        f("Shape", c.Shape, 0);
        f("Radius", c.Radius, 1);
        f("Height", c.Height, 2);
        f("HalfExtents", c.HalfExtents, 3);
        f("Center", c.Center, 4);
        f("Layer", c.Layer, 5);
        f("CollisionMask", c.CollisionMask, 6);
        f("SlopeLimit", c.SlopeLimit, 7);
        f("NonWalkable", c.NonWalkable, 8);
        f("StepOffset", c.StepOffset, 9);
        f("SkinWidth", c.SkinWidth, 10);
        f("MinMoveDistance", c.MinMoveDistance, 11);
        f("UpDirection", c.UpDirection, 12);
        f("PushRigidBodies", c.PushRigidBodies, 13);
        f("Mass", c.Mass, 14);
        f("PushForceScale", c.PushForceScale, 15);
        f("SnapDistance", c.SnapDistance, 16);
        f("FollowPlatforms", c.FollowPlatforms, 17);
    }
    template<class T, class F>
    void JointComponentFields(T& c, F f)
    {
        f("Type", c.Type, 0);
        f("BodyA", c.BodyA, 1);
        f("BodyB", c.BodyB, 2);
        f("EnableCollision", c.EnableCollision, 3);
        f("BreakForce", c.BreakForce, 4);
        f("BreakTorque", c.BreakTorque, 5);
        f("InvMassScaleA", c.InvMassScaleA, 6);
        f("InvMassScaleB", c.InvMassScaleB, 7);
        f("InvInertiaScaleA", c.InvInertiaScaleA, 8);
        f("InvInertiaScaleB", c.InvInertiaScaleB, 9);
        f("HingeLimitEnabled", c.HingeLimitEnabled, 10);
        f("HingeLowerAngle", c.HingeLowerAngle, 11);
        f("HingeUpperAngle", c.HingeUpperAngle, 12);
        f("HingeLimitStiffness", c.HingeLimitStiffness, 13);
        f("HingeLimitDamping", c.HingeLimitDamping, 14);
        f("HingeMotorEnabled", c.HingeMotorEnabled, 15);
        f("HingeMotorVelocity", c.HingeMotorVelocity, 16);
        f("HingeMotorMaxForce", c.HingeMotorMaxForce, 17);
        f("HingeFreeSpin", c.HingeFreeSpin, 18);
        f("HingeSpringEnabled", c.HingeSpringEnabled, 19);
        f("HingeTargetAngle", c.HingeTargetAngle, 20);
        f("HingeSpringStiffness", c.HingeSpringStiffness, 21);
        f("HingeSpringDamping", c.HingeSpringDamping, 22);
        f("SliderLimitEnabled", c.SliderLimitEnabled, 23);
        f("SliderLowerLimit", c.SliderLowerLimit, 24);
        f("SliderUpperLimit", c.SliderUpperLimit, 25);
        f("SliderLimitStiffness", c.SliderLimitStiffness, 26);
        f("SliderLimitDamping", c.SliderLimitDamping, 27);
        f("SliderDriveEnabled", c.SliderDriveEnabled, 28);
        f("SliderTargetPosition", c.SliderTargetPosition, 29);
        f("SliderTargetVelocity", c.SliderTargetVelocity, 30);
        f("SliderDriveStiffness", c.SliderDriveStiffness, 31);
        f("SliderDriveDamping", c.SliderDriveDamping, 32);
        f("SliderDriveMaxForce", c.SliderDriveMaxForce, 33);
        f("SphericalLimitEnabled", c.SphericalLimitEnabled, 34);
        f("SphericalYAngle", c.SphericalYAngle, 35);
        f("SphericalZAngle", c.SphericalZAngle, 36);
        f("SphericalLimitStiffness", c.SphericalLimitStiffness, 37);
        f("SphericalLimitDamping", c.SphericalLimitDamping, 38);
        f("DistanceMinEnabled", c.DistanceMinEnabled, 39);
        f("DistanceMaxEnabled", c.DistanceMaxEnabled, 40);
        f("DistanceMin", c.DistanceMin, 41);
        f("DistanceMax", c.DistanceMax, 42);
        f("DistanceStiffness", c.DistanceStiffness, 43);
        f("DistanceDamping", c.DistanceDamping, 44);
        f("DistanceTolerance", c.DistanceTolerance, 45);
        f("D6Motion", c.D6Motion, 46);
        f("D6LinearLower", c.D6LinearLower, 47);
        f("D6LinearUpper", c.D6LinearUpper, 48);
        f("D6LinearStiffness", c.D6LinearStiffness, 49);
        f("D6LinearDamping", c.D6LinearDamping, 50);
        f("D6TwistLower", c.D6TwistLower, 51);
        f("D6TwistUpper", c.D6TwistUpper, 52);
        f("D6SwingY", c.D6SwingY, 53);
        f("D6SwingZ", c.D6SwingZ, 54);
        f("D6AngularStiffness", c.D6AngularStiffness, 55);
        f("D6AngularDamping", c.D6AngularDamping, 56);
        f("DrivePosition", c.DrivePosition, 57);
        f("DriveRotation", c.DriveRotation, 58);
        f("DriveLinearVelocity", c.DriveLinearVelocity, 59);
        f("DriveAngularVelocity", c.DriveAngularVelocity, 60);
        f("DriveStiffness", c.DriveStiffness, 61);
        f("DriveDamping", c.DriveDamping, 62);
        f("DriveMaxForce", c.DriveMaxForce, 63);
        f("SlerpDrive", c.SlerpDrive, 64);
    }
} // namespace Kans::PhysicsFields
