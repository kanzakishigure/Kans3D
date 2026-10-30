#include "PhysicsSerialization.h"
#include <cmath>
#include <set>
#include <stdexcept>
#include <type_traits>
#include "PhysicsMechanismFields.h"
namespace Kans::PhysicsYaml
{
    namespace
    {
        void Require(bool value, const char* message)
        {
            if (!value)
                throw std::invalid_argument(message);
        }
        template<class T, class F>
        void FieldsPhysicsShapeOffset(T& c, F f)
        {
            f("Position", c.Position);
            f("Rotation", c.Rotation);
            f("Scale", c.Scale);
        }
        template<class T, class F>
        void FieldsPhysicsSphere(T& c, F f)
        {
            f("Radius", c.Radius);
            f("Offset", c.Offset);
        }
        template<class T, class F>
        void FieldsPhysicsCapsule(T& c, F f)
        {
            f("Radius", c.Radius);
            f("HalfHeight", c.HalfHeight);
            f("Direction", c.Direction);
            f("Offset", c.Offset);
        }
        template<class T, class F>
        void FieldsPhysicsBox(T& c, F f)
        {
            f("HalfExtents", c.HalfExtents);
            f("Offset", c.Offset);
        }
        template<class T, class F>
        void FieldsPhysicsConvexMesh(T& c, F f)
        {
            f("Mesh", c.Mesh);
            f("Section", c.Section);
            f("Points", c.Points);
            f("Offset", c.Offset);
        }
        template<class T, class F>
        void FieldsPhysicsTriangleMesh(T& c, F f)
        {
            f("Mesh", c.Mesh);
            f("Section", c.Section);
            f("Offset", c.Offset);
        }
        template<class T, class F>
        void FieldsPhysicsMaterialValues(T& c, F f)
        {
            f("StaticFriction", c.StaticFriction);
            f("DynamicFriction", c.DynamicFriction);
            f("Restitution", c.Restitution);
            f("FrictionCombine", c.FrictionCombine);
            f("RestitutionCombine", c.RestitutionCombine);
        }
        template<class T, class F>
        void FieldsColliderMaterial(T& c, F f)
        {
            f("Asset", c.Asset);
            f("UseDefault", c.UseDefault);
            f("Values", c.Values);
        }
        template<class T, class F>
        void FieldsColliderProperties(T& c, F f)
        {
            f("IsTrigger", c.IsTrigger);
            f("Density", c.Density);
            f("Material", c.Material);
            f("Layer", c.Layer);
        }
        template<class T, class F>
        void FieldsRigidBodyComponent(T& c, F f)
        {
            f("Type", c.Type);
            f("Mass", c.Mass);
            f("AutoMass", c.AutoMass);
            f("CenterOfMassOffset", c.CenterOfMassOffset);
            f("LinearDamping", c.LinearDamping);
            f("AngularDamping", c.AngularDamping);
            f("GravityScale", c.GravityScale);
            f("LockLinear", c.LockLinear);
            f("LockAngular", c.LockAngular);
            f("ContinuousDetection", c.ContinuousDetection);
            f("Interpolation", c.Interpolation);
            f("InitialLinearVelocity", c.InitialLinearVelocity);
            f("InitialAngularVelocity", c.InitialAngularVelocity);
            f("StartAwake", c.StartAwake);
            f("AffectedByWind", c.AffectedByWind);
            f("WindArea", c.WindArea);
            f("DragCoefficient", c.DragCoefficient);
            f("AirDensity", c.AirDensity);
            f("Layer", c.Layer);
        }
        template<class T, class F>
        void FieldsBoxColliderComponent(T& c, F f)
        {
            f("IsTrigger", c.IsTrigger);
            f("Density", c.Density);
            f("Material", c.Material);
            f("Layer", c.Layer);
            f("Shape", c.Shape);
        }
        template<class T, class F>
        void FieldsSphereColliderComponent(T& c, F f)
        {
            f("IsTrigger", c.IsTrigger);
            f("Density", c.Density);
            f("Material", c.Material);
            f("Layer", c.Layer);
            f("Shape", c.Shape);
        }
        template<class T, class F>
        void FieldsCapsuleColliderComponent(T& c, F f)
        {
            f("IsTrigger", c.IsTrigger);
            f("Density", c.Density);
            f("Material", c.Material);
            f("Layer", c.Layer);
            f("Shape", c.Shape);
        }
        template<class T, class F>
        void FieldsMeshColliderComponent(T& c, F f)
        {
            f("IsTrigger", c.IsTrigger);
            f("Density", c.Density);
            f("Material", c.Material);
            f("Layer", c.Layer);
            f("Convex", c.Convex);
            f("Shape", c.Shape);
        }
        template<class T, class F>
        void FieldsGlobalWindComponent(T& c, F f)
        {
            f("Direction", c.Direction);
            f("Strength", c.Strength);
            f("TurbulenceAmplitude", c.TurbulenceAmplitude);
            f("TurbulenceFrequency", c.TurbulenceFrequency);
        }
        template<class T, class F>
        void FieldsPhysicsSettings(T& c, F f)
        {
            f("Gravity", c.Gravity);
            f("FixedTimeStep", c.FixedTimeStep);
            f("MaxSubStepsPerFrame", c.MaxSubStepsPerFrame);
            f("SolverPositionIterations", c.SolverPositionIterations);
            f("SolverVelocityIterations", c.SolverVelocityIterations);
            f("BounceThreshold", c.BounceThreshold);
            f("SleepThreshold", c.SleepThreshold);
            f("WorkerThreads", c.WorkerThreads);
            f("LayerNames", c.LayerNames);
            f("CollisionMatrix", c.CollisionMatrix);
            f("DebugDraw", c.DebugDraw);
            f("DefaultMaterial", c.DefaultMaterial);
            f("QueriesHitTriggers", c.QueriesHitTriggers);
            f("EnableKinematicPairs", c.EnableKinematicPairs);
            f("MaxContactsPerPair", c.MaxContactsPerPair);
        }
        template<class F>
        void Fields(Physics::ShapeOffset& c, F f)
        {
            FieldsPhysicsShapeOffset(c, f);
        }
        template<class F>
        void Fields(const Physics::ShapeOffset& c, F f)
        {
            FieldsPhysicsShapeOffset(c, f);
        }
        template<class F>
        void Fields(Physics::Sphere& c, F f)
        {
            FieldsPhysicsSphere(c, f);
        }
        template<class F>
        void Fields(const Physics::Sphere& c, F f)
        {
            FieldsPhysicsSphere(c, f);
        }
        template<class F>
        void Fields(Physics::Capsule& c, F f)
        {
            FieldsPhysicsCapsule(c, f);
        }
        template<class F>
        void Fields(const Physics::Capsule& c, F f)
        {
            FieldsPhysicsCapsule(c, f);
        }
        template<class F>
        void Fields(Physics::Box& c, F f)
        {
            FieldsPhysicsBox(c, f);
        }
        template<class F>
        void Fields(const Physics::Box& c, F f)
        {
            FieldsPhysicsBox(c, f);
        }
        template<class F>
        void Fields(Physics::ConvexMesh& c, F f)
        {
            FieldsPhysicsConvexMesh(c, f);
        }
        template<class F>
        void Fields(const Physics::ConvexMesh& c, F f)
        {
            FieldsPhysicsConvexMesh(c, f);
        }
        template<class F>
        void Fields(Physics::TriangleMesh& c, F f)
        {
            FieldsPhysicsTriangleMesh(c, f);
        }
        template<class F>
        void Fields(const Physics::TriangleMesh& c, F f)
        {
            FieldsPhysicsTriangleMesh(c, f);
        }
        template<class F>
        void Fields(PhysicsMaterialValues& c, F f)
        {
            FieldsPhysicsMaterialValues(c, f);
        }
        template<class F>
        void Fields(const PhysicsMaterialValues& c, F f)
        {
            FieldsPhysicsMaterialValues(c, f);
        }
        template<class F>
        void Fields(ColliderMaterial& c, F f)
        {
            FieldsColliderMaterial(c, f);
        }
        template<class F>
        void Fields(const ColliderMaterial& c, F f)
        {
            FieldsColliderMaterial(c, f);
        }
        template<class F>
        void Fields(ColliderProperties& c, F f)
        {
            FieldsColliderProperties(c, f);
        }
        template<class F>
        void Fields(const ColliderProperties& c, F f)
        {
            FieldsColliderProperties(c, f);
        }
        template<class F>
        void Fields(RigidBodyComponent& c, F f)
        {
            FieldsRigidBodyComponent(c, f);
        }
        template<class F>
        void Fields(const RigidBodyComponent& c, F f)
        {
            FieldsRigidBodyComponent(c, f);
        }
        template<class F>
        void Fields(BoxColliderComponent& c, F f)
        {
            FieldsBoxColliderComponent(c, f);
        }
        template<class F>
        void Fields(const BoxColliderComponent& c, F f)
        {
            FieldsBoxColliderComponent(c, f);
        }
        template<class F>
        void Fields(SphereColliderComponent& c, F f)
        {
            FieldsSphereColliderComponent(c, f);
        }
        template<class F>
        void Fields(const SphereColliderComponent& c, F f)
        {
            FieldsSphereColliderComponent(c, f);
        }
        template<class F>
        void Fields(CapsuleColliderComponent& c, F f)
        {
            FieldsCapsuleColliderComponent(c, f);
        }
        template<class F>
        void Fields(const CapsuleColliderComponent& c, F f)
        {
            FieldsCapsuleColliderComponent(c, f);
        }
        template<class F>
        void Fields(MeshColliderComponent& c, F f)
        {
            FieldsMeshColliderComponent(c, f);
        }
        template<class F>
        void Fields(const MeshColliderComponent& c, F f)
        {
            FieldsMeshColliderComponent(c, f);
        }
        template<class F>
        void Fields(GlobalWindComponent& c, F f)
        {
            FieldsGlobalWindComponent(c, f);
        }
        template<class F>
        void Fields(const GlobalWindComponent& c, F f)
        {
            FieldsGlobalWindComponent(c, f);
        }
        template<class F>
        void Fields(PhysicsSettings& c, F f)
        {
            FieldsPhysicsSettings(c, f);
        }
        template<class F>
        void Fields(const PhysicsSettings& c, F f)
        {
            FieldsPhysicsSettings(c, f);
        }
        template<class F>
        void Fields(CharacterControllerComponent& c, F f)
        {
            PhysicsFields::CharacterControllerComponentFields(c, [&](const char* k, auto& v, int) { f(k, v); });
        }
        template<class F>
        void Fields(const CharacterControllerComponent& c, F f)
        {
            PhysicsFields::CharacterControllerComponentFields(c, [&](const char* k, auto& v, int) { f(k, v); });
        }
        template<class F>
        void Fields(JointComponent& c, F f)
        {
            PhysicsFields::JointComponentFields(c, [&](const char* k, auto& v, int) { f(k, v); });
        }
        template<class F>
        void Fields(const JointComponent& c, F f)
        {
            PhysicsFields::JointComponentFields(c, [&](const char* k, auto& v, int) { f(k, v); });
        }
        template<class T, class F>
        void FieldsSpringBoneChain(T& c, F f)
        {
            f("RootBone", c.RootBone);
            f("EndBone", c.EndBone);
            f("ExcludedBones", c.ExcludedBones);
            f("EndExtension", c.EndExtension);
            f("Stiffness", c.Stiffness);
            f("Drag", c.Drag);
            f("GravityPower", c.GravityPower);
            f("GravityDirection", c.GravityDirection);
            f("HitRadius", c.HitRadius);
            f("RotationAxis", c.RotationAxis);
            f("AngleLimit", c.AngleLimit);
            f("StiffnessCurve", c.StiffnessCurve);
            f("DragCurve", c.DragCurve);
            f("GravityPowerCurve", c.GravityPowerCurve);
            f("GravityDirectionCurve", c.GravityDirectionCurve);
            f("HitRadiusCurve", c.HitRadiusCurve);
            f("RotationAxisCurve", c.RotationAxisCurve);
            f("AngleLimitCurve", c.AngleLimitCurve);
            f("StiffnessMultiplier", c.StiffnessMultiplier);
            f("DragMultiplier", c.DragMultiplier);
            f("GravityMultiplier", c.GravityMultiplier);
        }
        template<class F>
        void Fields(SpringBoneChain& c, F f)
        {
            FieldsSpringBoneChain(c, f);
        }
        template<class F>
        void Fields(const SpringBoneChain& c, F f)
        {
            FieldsSpringBoneChain(c, f);
        }
        template<class T, class F>
        void FieldsSpringBoneComponent(T& c, F f)
        {
            f("Chains", c.Chains);
            f("Center", c.Center);
            f("InertiaScale", c.InertiaScale);
            f("WindScale", c.WindScale);
            f("ColliderGroups", c.ColliderGroups);
            f("SubstepRate", c.SubstepRate);
            f("MaxSubsteps", c.MaxSubsteps);
            f("TeleportDistance", c.TeleportDistance);
            f("TeleportRotation", c.TeleportRotation);
            f("Enabled", c.Enabled);
        }
        template<class F>
        void Fields(SpringBoneComponent& c, F f)
        {
            FieldsSpringBoneComponent(c, f);
        }
        template<class F>
        void Fields(const SpringBoneComponent& c, F f)
        {
            FieldsSpringBoneComponent(c, f);
        }
        template<class T, class F>
        void FieldsSpringBoneColliderComponent(T& c, F f)
        {
            f("Shape", c.Shape);
            f("Inside", c.Inside);
        }
        template<class F>
        void Fields(SpringBoneColliderComponent& c, F f)
        {
            FieldsSpringBoneColliderComponent(c, f);
        }
        template<class F>
        void Fields(const SpringBoneColliderComponent& c, F f)
        {
            FieldsSpringBoneColliderComponent(c, f);
        }
        template<class T, class F>
        void FieldsPhysicsPlane(T& c, F f)
        {
            f("Normal", c.Normal);
            f("Offset", c.Offset);
        }
        template<class F>
        void Fields(Physics::Plane& c, F f)
        {
            FieldsPhysicsPlane(c, f);
        }
        template<class F>
        void Fields(const Physics::Plane& c, F f)
        {
            FieldsPhysicsPlane(c, f);
        }
        template<class T, class F>
        void FieldsTimelineCurve(T& c, F f)
        {
            f("Keys", c.Keys);
        }
        template<class F>
        void Fields(TimelineCurve& c, F f)
        {
            FieldsTimelineCurve(c, f);
        }
        template<class F>
        void Fields(const TimelineCurve& c, F f)
        {
            FieldsTimelineCurve(c, f);
        }
        template<class T, class F>
        void FieldsTimelineKey(T& c, F f)
        {
            f("Time", c.Time);
            f("Value", c.Value);
            f("InTangent", c.InTangent);
            f("OutTangent", c.OutTangent);
            f("Interpolation", c.Interpolation);
        }
        template<class F>
        void Fields(TimelineKey& c, F f)
        {
            FieldsTimelineKey(c, f);
        }
        template<class F>
        void Fields(const TimelineKey& c, F f)
        {
            FieldsTimelineKey(c, f);
        }
        template<class T>
        YAML::Node Write(const T&);
        template<class T>
        void Read(const YAML::Node&, T&);
        template<class T>
        YAML::Node Write(const std::optional<T>& value)
        {
            return value ? Write(*value) : YAML::Node(YAML::NodeType::Null);
        }
        template<class T>
        void Read(const YAML::Node& n, std::optional<T>& v)
        {
            if (n.IsNull())
                v.reset();
            else
            {
                T item {};
                Read(n, item);
                v = item;
            }
        }
        template<class T, size_t N>
        YAML::Node Write(const std::array<T, N>& v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : v)
                n.push_back(Write(x));
            return n;
        }
        template<class T, size_t N>
        void Read(const YAML::Node& n, std::array<T, N>& v)
        {
            Require(n.IsSequence() && n.size() == N, "Invalid physics array");
            for (size_t i = 0; i < N; ++i)
                Read(n[i], v[i]);
        }
        template<class T>
        YAML::Node Write(const std::vector<T>& v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : v)
                n.push_back(Write(x));
            return n;
        }
        template<class T>
        void Read(const YAML::Node& n, std::vector<T>& v)
        {
            Require(n.IsSequence() && n.size() <= 1000000, "Invalid physics vector");
            v.resize(n.size());
            for (size_t i = 0; i < v.size(); ++i)
                Read(n[i], v[i]);
        }
        template<class T>
        YAML::Node Write(const T& value)
        {
            YAML::Node n;
            if constexpr (std::is_same_v<T, Physics::MotionColliderShape>)
            {
                n["Type"] = uint32_t(value.index());
                n["Data"] = std::visit([](const auto& shape) { return Write(shape); }, value);
            }
            else if constexpr (std::is_enum_v<T>)
                n = uint32_t(value);
            else if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
            {
                if constexpr (std::is_floating_point_v<T>)
                    Require(std::isfinite(value), "Nonfinite physics value");
                n = value;
            }
            else if constexpr (std::is_same_v<T, AssetID>)
                n = value.ToString();
            else if constexpr (std::is_same_v<T, UUID>)
                n = uint64_t(value);
            else if constexpr ((std::is_same_v<T, glm::vec3> || std::is_same_v<T, glm::vec4>))
            {
                n = YAML::Node(YAML::NodeType::Sequence);
                for (int i = 0; i < (std::is_same_v<T, glm::vec3> ? 3 : 4); ++i)
                    n.push_back(Write(value[i]));
            }
            else if constexpr (std::is_same_v<T, glm::quat>)
            {
                n = YAML::Node(YAML::NodeType::Sequence);
                for (int i = 0; i < 4; ++i)
                    n.push_back(Write(value[i]));
            }
            else
            {
                n = YAML::Node(YAML::NodeType::Map);
                Fields(value, [&](const char* key, const auto& v) { n[key] = Write(v); });
            }
            return n;
        }
        template<class T>
        void Read(const YAML::Node& n, T& value)
        {
            if constexpr (std::is_same_v<T, Physics::MotionColliderShape>)
            {
                Require(n.IsMap() && n.size() == 2 && n["Type"] && n["Data"], "Invalid motion collider shape");
                const auto type = n["Type"].as<uint32_t>();
                Require(type <= 2, "Invalid motion collider type");
                if (type == 0)
                {
                    Physics::Sphere s;
                    Read(n["Data"], s);
                    value = s;
                }
                if (type == 1)
                {
                    Physics::Capsule s;
                    Read(n["Data"], s);
                    value = s;
                }
                if (type == 2)
                {
                    Physics::Plane s;
                    Read(n["Data"], s);
                    value = s;
                }
            }
            else if constexpr (std::is_enum_v<T>)
            {
                auto     v   = n.as<uint32_t>();
                uint32_t max = 2;
                if constexpr (std::is_same_v<T, SpringRotationAxis>)
                    max = 3;
                if constexpr (std::is_same_v<T, PhysicsCombineMode>)
                    max = 3;
                if constexpr (std::is_same_v<T, PhysicsInterpolation>)
                    max = 1;
                if constexpr (std::is_same_v<T, PhysicsDebugFlags>)
                    max = 31;
                if constexpr (std::is_same_v<T, JointType>)
                    max = 5;
                if constexpr (std::is_same_v<T, ControllerShape> || std::is_same_v<T, NonWalkableMode>)
                    max = 1;
                Require(v <= max, "Invalid physics enum");
                value = T(v);
            }
            else if constexpr (std::is_arithmetic_v<T>)
            {
                value = n.as<T>();
                if constexpr (std::is_floating_point_v<T>)
                    Require(std::isfinite(value), "Nonfinite physics value");
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                value = n.as<std::string>();
                Require(value.size() <= 4096, "Physics string too long");
            }
            else if constexpr (std::is_same_v<T, AssetID>)
            {
                value = AssetID::FromString(n.as<std::string>());
            }
            else if constexpr (std::is_same_v<T, UUID>)
                value = n.as<uint64_t>();
            else if constexpr (std::is_same_v<T, glm::vec3> || std::is_same_v<T, glm::vec4> ||
                               std::is_same_v<T, glm::quat>)
            {
                const size_t count = std::is_same_v<T, glm::vec3> ? 3 : 4;
                Require(n.IsSequence() && n.size() == count, "Invalid physics vector");
                for (size_t i = 0; i < count; ++i)
                    Read(n[i], value[int(i)]);
                if constexpr (std::is_same_v<T, glm::quat>)
                {
                    Require(glm::dot(value, value) > 1e-12f, "Invalid physics rotation");
                }
            }
            else
            {
                Require(n.IsMap(), "Invalid physics mapping");
                std::set<std::string> allowed, seen;
                Fields(value, [&](const char* key, auto& v) {
                    allowed.insert(key);
                    if (n[key])
                        Read(n[key], v);
                });
                for (auto entry : n)
                {
                    auto key = entry.first.as<std::string>();
                    Require(allowed.count(key) && seen.insert(key).second, "Unknown/duplicate physics field");
                }
            }
        }
        void Validate(const PhysicsMaterialValues& m)
        {
            Require(m.StaticFriction >= 0 && m.DynamicFriction >= 0 && m.Restitution >= 0 && m.Restitution <= 1,
                    "Invalid physics material");
        }
        void Validate(const RigidBodyComponent& c)
        {
            Require(c.Mass > 0 && c.Layer < 32 && c.LinearDamping >= 0 && c.AngularDamping >= 0 && c.WindArea >= 0 &&
                        c.DragCoefficient >= 0 && c.AirDensity >= 0,
                    "Invalid rigid body");
        }
        void Validate(const GlobalWindComponent& c)
        {
            Require(c.TurbulenceFrequency >= 0 && c.TurbulenceAmplitude >= 0, "Invalid wind");
        }
        template<class T>
        void ValidateCollider(const T& c)
        {
            Require(c.Density > 0 && (!c.Layer || *c.Layer < 32), "Invalid collider density/layer");
            Validate(c.Material.Values);
            using S = std::decay_t<decltype(c.Shape)>;
            if constexpr (std::is_same_v<S, Physics::Sphere> || std::is_same_v<S, Physics::Capsule>)
                Require(c.Shape.Radius > 0, "Invalid collider radius");
            if constexpr (std::is_same_v<S, Physics::Capsule>)
                Require(c.Shape.HalfHeight >= 0, "Invalid capsule height");
            if constexpr (std::is_same_v<S, Physics::Box>)
                Require(c.Shape.HalfExtents.x > 0 && c.Shape.HalfExtents.y > 0 && c.Shape.HalfExtents.z > 0,
                        "Invalid box extents");
        }
        void Validate(const BoxColliderComponent& c) { ValidateCollider(c); }
        void Validate(const SphereColliderComponent& c) { ValidateCollider(c); }
        void Validate(const CapsuleColliderComponent& c) { ValidateCollider(c); }
        void Validate(const MeshColliderComponent& c) { ValidateCollider(c); }
        void Validate(const CharacterControllerComponent& c)
        {
            Require(uint32_t(c.Shape) <= 1 && uint32_t(c.NonWalkable) <= 1 && c.Layer < 32 && c.Radius > 0 &&
                        c.Height > 0 && c.HalfExtents.x > 0 && c.HalfExtents.y > 0 && c.HalfExtents.z > 0 &&
                        c.SlopeLimit >= 0 && c.SlopeLimit <= 90 && c.StepOffset >= 0 &&
                        c.StepOffset <=
                            (c.Shape == ControllerShape::Capsule ? c.Height + 2 * c.Radius : 2 * c.HalfExtents.y) &&
                        c.SkinWidth > 0 && c.MinMoveDistance >= 0 && glm::length(c.UpDirection) > 1e-6f && c.Mass > 0 &&
                        c.PushForceScale >= 0 && c.SnapDistance >= 0,
                    "Invalid character controller");
        }
        void Validate(const JointComponent& c)
        {
            Require(uint32_t(c.Type) <= 5 && c.BreakForce >= 0 && c.BreakTorque >= 0 && c.InvMassScaleA >= 0 &&
                        c.InvMassScaleB >= 0 && c.InvInertiaScaleA >= 0 && c.InvInertiaScaleB >= 0 &&
                        c.HingeLowerAngle <= c.HingeUpperAngle && c.HingeLowerAngle > -360 && c.HingeUpperAngle < 360 &&
                        c.SliderLowerLimit <= c.SliderUpperLimit && c.DistanceMin >= 0 &&
                        c.DistanceMax >= c.DistanceMin && c.DistanceTolerance >= 0 && c.SphericalYAngle > 0 &&
                        c.SphericalYAngle < 180 && c.SphericalZAngle > 0 && c.SphericalZAngle < 180 &&
                        c.D6TwistLower <= c.D6TwistUpper && c.D6TwistLower > -360 && c.D6TwistUpper < 360 &&
                        c.D6SwingY > 0 && c.D6SwingY < 180 && c.D6SwingZ > 0 && c.D6SwingZ < 180 &&
                        glm::all(glm::lessThanEqual(c.D6LinearLower, c.D6LinearUpper)) &&
                        glm::length(c.DriveRotation) > 1e-6f,
                    "Invalid joint limits");
            PhysicsFields::JointComponentFields(c, [&](const char* key, const auto& v, int) {
                using T = std::decay_t<decltype(v)>;
                const std::string name(key);
                if constexpr (std::is_same_v<T, float>)
                    if (name.find("Stiffness") != std::string::npos || name.find("Damping") != std::string::npos ||
                        name.find("MaxForce") != std::string::npos)
                        Require(v >= 0, "Negative joint spring/drive value");
            });
            for (auto v : c.D6Motion)
                Require(uint32_t(v) <= 2, "Invalid D6 motion");
            for (auto* values : {&c.DriveStiffness, &c.DriveDamping, &c.DriveMaxForce})
                for (auto v : *values)
                    Require(v >= 0, "Negative D6 drive value");
        }
        void ValidateCurve(const TimelineCurve& c)
        {
            double last = -1;
            for (const auto& key : c.Keys)
            {
                Require(key.Time >= 0 && key.Time <= 1 && key.Time > last && uint32_t(key.Interpolation) <= 2,
                        "Invalid spring curve coordinate");
                last = key.Time;
            }
        }
        void Validate(const SpringBoneComponent& c)
        {
            Require(c.InertiaScale >= 0 && c.InertiaScale <= 1 && c.SubstepRate >= 1 && c.SubstepRate <= 1000 &&
                        c.MaxSubsteps > 0 && c.MaxSubsteps <= 1024 && c.TeleportDistance >= 0 &&
                        c.TeleportRotation >= 0 && c.TeleportRotation <= 180,
                    "Invalid spring settings");
            for (const auto& chain : c.Chains)
            {
                Require(chain.EndExtension >= 0 && chain.Stiffness >= 0 && chain.Drag >= 0 && chain.Drag <= 1 &&
                            chain.GravityPower >= 0 && chain.HitRadius >= 0 && uint32_t(chain.RotationAxis) <= 3 &&
                            chain.AngleLimit >= 0 && chain.AngleLimit <= 180 && chain.StiffnessMultiplier >= 0 &&
                            chain.DragMultiplier >= 0 && chain.GravityMultiplier >= 0,
                        "Invalid spring chain");
                for (auto curve : {&chain.StiffnessCurve,
                                   &chain.DragCurve,
                                   &chain.GravityPowerCurve,
                                   &chain.GravityDirectionCurve,
                                   &chain.HitRadiusCurve,
                                   &chain.RotationAxisCurve,
                                   &chain.AngleLimitCurve})
                    ValidateCurve(*curve);
            }
        }
        void Validate(const SpringBoneColliderComponent& c)
        {
            std::visit(
                [&](const auto& s) {
                    using T = std::decay_t<decltype(s)>;
                    Require(glm::dot(s.Offset.Rotation, s.Offset.Rotation) > 1e-12f && s.Offset.Scale.x > 0 &&
                                s.Offset.Scale.y > 0 && s.Offset.Scale.z > 0,
                            "Invalid motion collider offset");
                    if constexpr (std::is_same_v<T, Physics::Plane>)
                        Require(glm::dot(s.Normal, s.Normal) > 1e-12f, "Invalid plane normal");
                    else
                        Require(s.Radius > 0, "Invalid motion collider radius");
                    if constexpr (std::is_same_v<T, Physics::Capsule>)
                        Require(s.HalfHeight >= 0 && uint32_t(s.Direction) <= 2, "Invalid motion capsule");
                },
                c.Shape);
        }
        void Validate(const PhysicsSettings& c)
        {
            auto copy = c;
            copy.Validate();
            Validate(c.DefaultMaterial);
        }
    } // namespace
    YAML::Node Encode(const SpringBoneComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, SpringBoneComponent& c)
    {
        SpringBoneComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const SpringBoneColliderComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, SpringBoneColliderComponent& c)
    {
        SpringBoneColliderComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const CharacterControllerComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, CharacterControllerComponent& c)
    {
        CharacterControllerComponent next;
        Read(n, next);
        Validate(next);
        c = next;
    }
    YAML::Node Encode(const JointComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, JointComponent& c)
    {
        JointComponent next;
        Read(n, next);
        Validate(next);
        c = next;
    }
    YAML::Node Encode(const RigidBodyComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, RigidBodyComponent& c)
    {
        RigidBodyComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const BoxColliderComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, BoxColliderComponent& c)
    {
        BoxColliderComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const SphereColliderComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, SphereColliderComponent& c)
    {
        SphereColliderComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const CapsuleColliderComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, CapsuleColliderComponent& c)
    {
        CapsuleColliderComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const MeshColliderComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, MeshColliderComponent& c)
    {
        MeshColliderComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const GlobalWindComponent& c)
    {
        Validate(c);
        return Write(c);
    }
    void Decode(const YAML::Node& n, GlobalWindComponent& c)
    {
        GlobalWindComponent next;
        Read(n, next);
        Validate(next);
        c = std::move(next);
    }
    YAML::Node Encode(const PhysicsSettings& c)
    {
        auto validated = c;
        validated.Validate();
        Validate(validated);
        return Write(validated);
    }
    void Decode(const YAML::Node& n, PhysicsSettings& c)
    {
        PhysicsSettings next;
        Read(n, next);
        Validate(next);
        next.Validate();
        c = std::move(next);
    }
} // namespace Kans::PhysicsYaml
