#include "PhysicsSettings.h"
#include <cmath>
#include <stdexcept>
namespace Kans
{
    PhysicsSettings::PhysicsSettings()
    {
        CollisionMatrix.fill(UINT32_MAX);
        for (size_t i = 0; i < 32; ++i)
            LayerNames[i] = i ? "Layer " + std::to_string(i) : "Default";
    }
    void PhysicsSettings::SetLayerCollision(uint32_t a, uint32_t b, bool enabled)
    {
        if (a >= 32 || b >= 32)
            throw std::out_of_range("Physics layer must be 0..31");
        if (enabled)
        {
            CollisionMatrix[a] |= 1u << b;
            CollisionMatrix[b] |= 1u << a;
        }
        else
        {
            CollisionMatrix[a] &= ~(1u << b);
            CollisionMatrix[b] &= ~(1u << a);
        }
    }
    bool PhysicsSettings::LayersCollide(uint32_t a, uint32_t b) const
    {
        return a < 32 && b < 32 && (CollisionMatrix[a] & (1u << b)) && (CollisionMatrix[b] & (1u << a));
    }
    void PhysicsSettings::Validate()
    {
        if (!std::isfinite(FixedTimeStep) || FixedTimeStep < 1e-6 || FixedTimeStep > 1 || !MaxSubStepsPerFrame ||
            !SolverPositionIterations || SolverPositionIterations > 255 || !SolverVelocityIterations ||
            SolverVelocityIterations > 255 || !std::isfinite(Gravity.x) || !std::isfinite(Gravity.y) ||
            !std::isfinite(Gravity.z) || !std::isfinite(BounceThreshold) || BounceThreshold < 0 ||
            !std::isfinite(SleepThreshold) || SleepThreshold < 0 || WorkerThreads > 64 || !MaxContactsPerPair ||
            MaxContactsPerPair > 4096)
            throw std::invalid_argument("Invalid PhysicsSettings");
        const auto& material = DefaultMaterial;
        if (!std::isfinite(material.StaticFriction) || !std::isfinite(material.DynamicFriction) ||
            !std::isfinite(material.Restitution) || material.StaticFriction < 0 || material.DynamicFriction < 0 ||
            material.Restitution < 0 || material.Restitution > 1 || uint32_t(material.FrictionCombine) > 3 ||
            uint32_t(material.RestitutionCombine) > 3 || (uint32_t(DebugDraw) & ~15u))
            throw std::invalid_argument("Invalid default physics material/debug flags");
        for (uint32_t a = 0; a < 32; ++a)
            for (uint32_t b = a; b < 32; ++b)
                SetLayerCollision(a, b, LayersCollide(a, b));
    }
} // namespace Kans
