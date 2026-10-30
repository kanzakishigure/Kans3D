#pragma once
#include <tuple>
#include "PhysicsComponents.h"
namespace Kans::Physics
{
    inline bool operator==(const ShapeOffset& a, const ShapeOffset& b)
    {
        return std::tie(a.Position, a.Rotation, a.Scale) == std::tie(b.Position, b.Rotation, b.Scale);
    }
    inline bool operator!=(const ShapeOffset& a, const ShapeOffset& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans::Physics
{
    inline bool operator==(const Sphere& a, const Sphere& b)
    {
        return std::tie(a.Radius, a.Offset) == std::tie(b.Radius, b.Offset);
    }
    inline bool operator!=(const Sphere& a, const Sphere& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans::Physics
{
    inline bool operator==(const Capsule& a, const Capsule& b)
    {
        return std::tie(a.Radius, a.HalfHeight, a.Direction, a.Offset) ==
               std::tie(b.Radius, b.HalfHeight, b.Direction, b.Offset);
    }
    inline bool operator!=(const Capsule& a, const Capsule& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans::Physics
{
    inline bool operator==(const Box& a, const Box& b)
    {
        return std::tie(a.HalfExtents, a.Offset) == std::tie(b.HalfExtents, b.Offset);
    }
    inline bool operator!=(const Box& a, const Box& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans::Physics
{
    inline bool operator==(const ConvexMesh& a, const ConvexMesh& b)
    {
        return std::tie(a.Mesh, a.Section, a.Points, a.Offset) == std::tie(b.Mesh, b.Section, b.Points, b.Offset);
    }
    inline bool operator!=(const ConvexMesh& a, const ConvexMesh& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans::Physics
{
    inline bool operator==(const TriangleMesh& a, const TriangleMesh& b)
    {
        return std::tie(a.Mesh, a.Section, a.Offset) == std::tie(b.Mesh, b.Section, b.Offset);
    }
    inline bool operator!=(const TriangleMesh& a, const TriangleMesh& b) { return !(a == b); }
} // namespace Kans::Physics
namespace Kans
{
    inline bool operator==(const PhysicsMaterialValues& a, const PhysicsMaterialValues& b)
    {
        return std::tie(a.StaticFriction, a.DynamicFriction, a.Restitution, a.FrictionCombine, a.RestitutionCombine) ==
               std::tie(b.StaticFriction, b.DynamicFriction, b.Restitution, b.FrictionCombine, b.RestitutionCombine);
    }
    inline bool operator!=(const PhysicsMaterialValues& a, const PhysicsMaterialValues& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const ColliderMaterial& a, const ColliderMaterial& b)
    {
        return std::tie(a.Asset, a.UseDefault, a.Values) == std::tie(b.Asset, b.UseDefault, b.Values);
    }
    inline bool operator!=(const ColliderMaterial& a, const ColliderMaterial& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const ColliderProperties& a, const ColliderProperties& b)
    {
        return std::tie(a.IsTrigger, a.Density, a.Material, a.Layer) ==
               std::tie(b.IsTrigger, b.Density, b.Material, b.Layer);
    }
    inline bool operator!=(const ColliderProperties& a, const ColliderProperties& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const RigidBodyComponent& a, const RigidBodyComponent& b)
    {
        return std::tie(a.Type,
                        a.Mass,
                        a.AutoMass,
                        a.CenterOfMassOffset,
                        a.LinearDamping,
                        a.AngularDamping,
                        a.GravityScale,
                        a.LockLinear,
                        a.LockAngular,
                        a.ContinuousDetection,
                        a.Interpolation,
                        a.InitialLinearVelocity,
                        a.InitialAngularVelocity,
                        a.StartAwake,
                        a.AffectedByWind,
                        a.WindArea,
                        a.DragCoefficient,
                        a.AirDensity,
                        a.Layer) == std::tie(b.Type,
                                             b.Mass,
                                             b.AutoMass,
                                             b.CenterOfMassOffset,
                                             b.LinearDamping,
                                             b.AngularDamping,
                                             b.GravityScale,
                                             b.LockLinear,
                                             b.LockAngular,
                                             b.ContinuousDetection,
                                             b.Interpolation,
                                             b.InitialLinearVelocity,
                                             b.InitialAngularVelocity,
                                             b.StartAwake,
                                             b.AffectedByWind,
                                             b.WindArea,
                                             b.DragCoefficient,
                                             b.AirDensity,
                                             b.Layer);
    }
    inline bool operator!=(const RigidBodyComponent& a, const RigidBodyComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const BoxColliderComponent& a, const BoxColliderComponent& b)
    {
        return std::tie(a.IsTrigger, a.Density, a.Material, a.Layer, a.Shape) ==
               std::tie(b.IsTrigger, b.Density, b.Material, b.Layer, b.Shape);
    }
    inline bool operator!=(const BoxColliderComponent& a, const BoxColliderComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const SphereColliderComponent& a, const SphereColliderComponent& b)
    {
        return std::tie(a.IsTrigger, a.Density, a.Material, a.Layer, a.Shape) ==
               std::tie(b.IsTrigger, b.Density, b.Material, b.Layer, b.Shape);
    }
    inline bool operator!=(const SphereColliderComponent& a, const SphereColliderComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const CapsuleColliderComponent& a, const CapsuleColliderComponent& b)
    {
        return std::tie(a.IsTrigger, a.Density, a.Material, a.Layer, a.Shape) ==
               std::tie(b.IsTrigger, b.Density, b.Material, b.Layer, b.Shape);
    }
    inline bool operator!=(const CapsuleColliderComponent& a, const CapsuleColliderComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const MeshColliderComponent& a, const MeshColliderComponent& b)
    {
        return std::tie(a.IsTrigger, a.Density, a.Material, a.Layer, a.Convex, a.Shape) ==
               std::tie(b.IsTrigger, b.Density, b.Material, b.Layer, b.Convex, b.Shape);
    }
    inline bool operator!=(const MeshColliderComponent& a, const MeshColliderComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const GlobalWindComponent& a, const GlobalWindComponent& b)
    {
        return std::tie(a.Direction, a.Strength, a.TurbulenceAmplitude, a.TurbulenceFrequency) ==
               std::tie(b.Direction, b.Strength, b.TurbulenceAmplitude, b.TurbulenceFrequency);
    }
    inline bool operator!=(const GlobalWindComponent& a, const GlobalWindComponent& b) { return !(a == b); }
} // namespace Kans
namespace Kans
{
    inline bool operator==(const PhysicsSettings& a, const PhysicsSettings& b)
    {
        return std::tie(a.Gravity,
                        a.FixedTimeStep,
                        a.MaxSubStepsPerFrame,
                        a.SolverPositionIterations,
                        a.SolverVelocityIterations,
                        a.BounceThreshold,
                        a.SleepThreshold,
                        a.WorkerThreads,
                        a.LayerNames,
                        a.CollisionMatrix,
                        a.DebugDraw,
                        a.DefaultMaterial) == std::tie(b.Gravity,
                                                       b.FixedTimeStep,
                                                       b.MaxSubStepsPerFrame,
                                                       b.SolverPositionIterations,
                                                       b.SolverVelocityIterations,
                                                       b.BounceThreshold,
                                                       b.SleepThreshold,
                                                       b.WorkerThreads,
                                                       b.LayerNames,
                                                       b.CollisionMatrix,
                                                       b.DebugDraw,
                                                       b.DefaultMaterial);
    }
    inline bool operator!=(const PhysicsSettings& a, const PhysicsSettings& b) { return !(a == b); }
} // namespace Kans
