#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <thread>
#include <glm/glm.hpp>

namespace Kans
{
    enum class PhysicsCombineMode : uint8_t
    {
        Average,
        Minimum,
        Multiply,
        Maximum
    };
    struct PhysicsMaterialValues
    {
        float              StaticFriction = .6f, DynamicFriction = .6f, Restitution = 0;
        PhysicsCombineMode FrictionCombine    = PhysicsCombineMode::Average;
        PhysicsCombineMode RestitutionCombine = PhysicsCombineMode::Average;
    };
    enum class PhysicsDebugFlags : uint32_t
    {
        None          = 0,
        Colliders     = 1,
        CentersOfMass = 2,
        Sleeping      = 4,
        Contacts      = 8
    };
    inline PhysicsDebugFlags operator|(PhysicsDebugFlags a, PhysicsDebugFlags b)
    {
        return PhysicsDebugFlags(uint32_t(a) | uint32_t(b));
    }
    struct PhysicsSettings
    {
        glm::vec3                   Gravity {0, -9.81f, 0};
        double                      FixedTimeStep            = 1.0 / 60.0;
        uint32_t                    MaxSubStepsPerFrame      = 8;
        uint32_t                    SolverPositionIterations = 4, SolverVelocityIterations = 1;
        float                       BounceThreshold = 2.f, SleepThreshold = .005f;
        uint32_t                    WorkerThreads = std::min(std::max(std::thread::hardware_concurrency(), 1u) - 1, 4u);
        std::array<std::string, 32> LayerNames;
        std::array<uint32_t, 32>    CollisionMatrix;
        PhysicsDebugFlags           DebugDraw = PhysicsDebugFlags::None;
        PhysicsMaterialValues       DefaultMaterial;
        bool                        QueriesHitTriggers = false;
        // Enables PhysX kinematic-static and kinematic-kinematic detection (static-static remains suppressed).
        bool     EnableKinematicPairs = false;
        uint32_t MaxContactsPerPair   = 16;
        PhysicsSettings();
        void SetLayerCollision(uint32_t a, uint32_t b, bool enabled);
        bool LayersCollide(uint32_t a, uint32_t b) const;
        // Reject invalid simulation parameters; enforce symmetry by requiring both bits.
        void Validate();
    };
} // namespace Kans
