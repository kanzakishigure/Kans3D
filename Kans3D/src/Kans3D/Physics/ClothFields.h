#pragma once
namespace Kans
{
    template<class C, class F>
    void ClothFields(C& c, F f)
    {
        f("Asset", c.Asset);
        f("Section", c.Section);
        f("Enabled", c.Enabled);
        f("SubstepRate", c.SubstepRate);
        f("MaxSubsteps", c.MaxSubsteps);
        f("Iterations", c.Iterations);
        f("StretchCompliance", c.StretchCompliance);
        f("BendCompliance", c.BendCompliance);
        f("TetherScale", c.TetherScale);
        f("Damping", c.Damping);
        f("VelocityDrag", c.VelocityDrag);
        f("ParticleMass", c.ParticleMass);
        f("AreaDensity", c.AreaDensity);
        f("GravityScale", c.GravityScale);
        f("WindScale", c.WindScale);
        f("Lift", c.Lift);
        f("Drag", c.Drag);
        f("Center", c.Center);
        f("InertiaScale", c.InertiaScale);
        f("LinearInertiaLimit", c.LinearInertiaLimit);
        f("AngularInertiaLimit", c.AngularInertiaLimit);
        f("TeleportDistance", c.TeleportDistance);
        f("TeleportRotation", c.TeleportRotation);
        f("ColliderGroups", c.ColliderGroups);
        f("Thickness", c.Thickness);
        f("GroundPlane", c.GroundPlane);
        f("GroundHeight", c.GroundHeight);
        f("DisableDistance", c.DisableDistance);
        f("SimulateWhenNotVisible", c.SimulateWhenNotVisible);
        f("MaxDistanceScale", c.MaxDistanceScale);
    }
} // namespace Kans
