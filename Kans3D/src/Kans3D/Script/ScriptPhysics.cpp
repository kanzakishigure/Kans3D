#include "kspch.h"
#include "ScriptPhysics.h"
#include <cstring>
#include "Kans3D/Physics/PhysicsMechanismFields.h"
#include "Kans3D/Physics/PhysicsSerialization.h"
#include "Kans3D/Scene/Entity.h"
#include "ScriptEngine.h"
namespace Kans::ScriptPhysics
{
    namespace
    {
        glm::vec3 Vector(const Scripting::Value& v) { return {v.X, v.Y, v.Z}; }
        void      Vector(Scripting::Value& v, glm::vec3 x)
        {
            v.X = x.x;
            v.Y = x.y;
            v.Z = x.z;
        }
        glm::vec3 Vector(const Scripting::Vector3& v) { return {v.X, v.Y, v.Z}; }
        template<class T>
        int Collider(Entity e, int property, bool write, Scripting::Value& v, const char* text)
        {
            if (!e.HasComponent<T>())
                return 0;
            auto c      = e.GetComponent<T>();
            auto number = [&](float& x) {
                if (write)
                    x = v.X;
                else
                    v.X = x;
            };
            auto vector = [&](glm::vec3& x) {
                if (write)
                    x = Vector(v);
                else
                    Vector(v, x);
            };
            switch (property)
            {
                case 0:
                    if constexpr (std::is_same_v<T, BoxColliderComponent>)
                        vector(c.Shape.HalfExtents);
                    else if constexpr (std::is_same_v<T, SphereColliderComponent> ||
                                       std::is_same_v<T, CapsuleColliderComponent>)
                        number(c.Shape.Radius);
                    else
                        return -1;
                    break;
                case 1:
                    if constexpr (std::is_same_v<T, CapsuleColliderComponent>)
                        number(c.Shape.HalfHeight);
                    else
                        return -1;
                    break;
                case 2:
                    if constexpr (std::is_same_v<T, CapsuleColliderComponent>)
                    {
                        if (write)
                        {
                            if (v.Integer < 0 || v.Integer > 2)
                                return -1;
                            c.Shape.Direction = Physics::Axis(v.Integer);
                        }
                        else
                            v.Integer = int64_t(c.Shape.Direction);
                    }
                    else
                        return -1;
                    break;
                case 3:
                    vector(c.Shape.Offset.Position);
                    break;
                case 4:
                    if (write)
                        c.Shape.Offset.Rotation = {v.W, v.X, v.Y, v.Z};
                    else
                    {
                        const auto q = c.Shape.Offset.Rotation;
                        v.X          = q.x;
                        v.Y          = q.y;
                        v.Z          = q.z;
                        v.W          = q.w;
                    }
                    break;
                case 5:
                    vector(c.Shape.Offset.Scale);
                    break;
                case 10:
                    if (write)
                        c.IsTrigger = v.Integer != 0;
                    else
                        v.Integer = c.IsTrigger;
                    break;
                case 11:
                    if (write)
                    {
                        if (v.Integer < -1 || v.Integer > 31)
                            return -1;
                        c.Layer = v.Integer < 0 ? std::nullopt : std::optional<uint32_t>(uint32_t(v.Integer));
                    }
                    else
                        v.Integer = c.Layer ? int64_t(*c.Layer) : -1;
                    break;
                case 12:
                    if (write)
                    {
                        if (!text)
                            return -1;
                        c.Material.Asset = AssetID::FromString(text);
                    }
                    else
                    {
                        v.Integer      = int64_t(c.Material.Asset.GetSourceID().GetUUID());
                        uint64_t local = c.Material.Asset.GetLocalID();
                        std::memcpy(&v.X, &local, 8);
                    }
                    break;
                case 13:
                    if constexpr (std::is_same_v<T, MeshColliderComponent>)
                    {
                        if (write)
                            c.Convex = v.Integer != 0;
                        else
                            v.Integer = c.Convex;
                    }
                    else
                        return -1;
                    break;
                case 14:
                    if constexpr (std::is_same_v<T, MeshColliderComponent>)
                    {
                        if (write)
                        {
                            if (!text)
                                return -1;
                            c.Shape.Mesh = AssetID::FromString(text);
                        }
                        else
                        {
                            v.Integer      = int64_t(c.Shape.Mesh.GetSourceID().GetUUID());
                            uint64_t local = c.Shape.Mesh.GetLocalID();
                            std::memcpy(&v.X, &local, 8);
                        }
                    }
                    else
                        return -1;
                    break;
                case 15:
                    if constexpr (std::is_same_v<T, MeshColliderComponent>)
                    {
                        if (write)
                        {
                            if (v.Integer < 0 || uint64_t(v.Integer) > UINT32_MAX)
                                return -1;
                            c.Shape.Section = uint32_t(v.Integer);
                        }
                        else
                            v.Integer = c.Shape.Section;
                    }
                    else
                        return -1;
                    break;
                default:
                    return -1;
            }
            if (write)
            {
                PhysicsYaml::Encode(c);
                e.SetComponent<T>(c);
            }
            return 1;
        }
        template<class T>
        void MechanismValue(T& x, bool write, Scripting::Value& v, int axis)
        {
            if constexpr (std::is_same_v<T, glm::vec3>)
            {
                if (write)
                    x = Vector(v);
                else
                    Vector(v, x);
            }
            else if constexpr (std::is_same_v<T, glm::quat>)
            {
                if (write)
                    x = {v.W, v.X, v.Y, v.Z};
                else
                {
                    v.X = x.x;
                    v.Y = x.y;
                    v.Z = x.z;
                    v.W = x.w;
                }
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                if (write)
                    x = v.X;
                else
                    v.X = x;
            }
            else if constexpr (std::is_same_v<T, UUID>)
            {
                if (write)
                    x = uint64_t(v.Integer);
                else
                    v.Integer = int64_t(uint64_t(x));
            }
            else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            {
                if (write)
                {
                    if constexpr (std::is_enum_v<T>)
                    {
                        const int64_t maximum =
                            std::is_same_v<T, JointType> ? 5 : (std::is_same_v<T, JointMotion> ? 2 : 1);
                        if (v.Integer < 0 || v.Integer > maximum)
                            throw std::invalid_argument("Invalid mechanism enum");
                    }
                    else if constexpr (std::is_same_v<T, uint32_t>)
                        if (v.Integer < 0 || uint64_t(v.Integer) > UINT32_MAX)
                            throw std::invalid_argument("Invalid mechanism mask/layer");
                    x = T(v.Integer);
                }
                else
                    v.Integer = int64_t(x);
            }
            else
            {
                if (axis < 0 || axis >= int(x.size()))
                    throw std::invalid_argument("Invalid joint axis");
                MechanismValue(x[axis], write, v, 0);
            }
        }
        template<class T>
        int MechanismProperty(Entity e, int mode, Scripting::Value& v)
        {
            if (!e.HasComponent<T>())
                return 0;
            auto c     = e.GetComponent<T>();
            bool found = false, write = mode & 1024;
            auto property = [&](const char*, auto& x, int index) {
                if (index == (mode & 127))
                {
                    MechanismValue(x, write, v, (mode & 1023) >> 7);
                    found = true;
                }
            };
            if constexpr (std::is_same_v<T, CharacterControllerComponent>)
                PhysicsFields::CharacterControllerComponentFields(c, property);
            else
                PhysicsFields::JointComponentFields(c, property);
            if (!found)
                return -1;
            if (write)
            {
                PhysicsYaml::Encode(c);
                e.SetComponent<T>(c);
            }
            return 1;
        }
        int32_t KANS_SCRIPT_CALL Command(uint64_t            id,
                                         int32_t             op,
                                         int32_t             mode,
                                         Scripting::Value*   v,
                                         Scripting::Vector3* second,
                                         const char*         text) noexcept
        {
            try
            {
                auto* scene = ScriptEngine::GetSceneContext();
                if (!scene || !v)
                    return 0;
                auto* world = scene->GetPhysicsWorld();
#include "ScriptP4.inl"
                if (op == 0)
                {
                    Vector(*v, scene->GetPhysicsSettings().Gravity);
                    return 1;
                }
                if (op == 1)
                {
                    if (!world)
                        return 0;
                    world->SetGravity(Vector(*v));
                    return 1;
                }
                if (op == 2)
                {
                    if (!world || v->Integer < 0 || v->Integer > 31 || mode < 0 || mode > 31)
                        return -1;
                    world->IgnoreLayerCollision(uint32_t(v->Integer), uint32_t(mode), v->X != 0);
                    return 1;
                }
                if (op == 3)
                {
                    if (!text)
                        return -1;
                    for (unsigned a = 0; a < 32; ++a)
                        if (scene->GetPhysicsSettings().LayerNames[a] == text)
                            return int32_t(a + 1);
                    return 0;
                }
                auto e = scene->GetEntityByUUID(id);
                if (!e)
                    return 0;
                if (op >= 70 && op <= 72)
                {
                    if (!e.HasComponent<SpringBoneComponent>())
                        return 0;
                    auto&      c        = e.GetComponent<SpringBoneComponent>();
                    const bool write    = mode & 1024;
                    const int  property = mode & 1023;
                    if (op == 71)
                    {
                        c.Reset();
                        return 1;
                    }
                    if (op == 70 && property == 0)
                    {
                        if (write)
                        {
                            const bool enabled = v->Integer != 0;
                            if (enabled != c.Enabled)
                                c.Reset();
                            c.Enabled = enabled;
                        }
                        else
                            v->Integer = c.Enabled;
                        return 1;
                    }
                    float* field = nullptr;
                    if (op == 70)
                    {
                        if (property == 1)
                            field = &c.InertiaScale;
                        if (property == 2)
                            field = &c.WindScale;
                    }
                    if (op == 72)
                    {
                        if (v->Integer < 0 || uint64_t(v->Integer) >= c.Chains.size())
                            return -1;
                        auto& chain = c.Chains[size_t(v->Integer)];
                        if (property == 0)
                            field = &chain.StiffnessMultiplier;
                        if (property == 1)
                            field = &chain.DragMultiplier;
                        if (property == 2)
                            field = &chain.GravityMultiplier;
                    }
                    if (!field)
                        return -1;
                    if (write)
                    {
                        if (!std::isfinite(v->X) || (op == 72 && v->X < 0) ||
                            (op == 70 && property == 1 && (v->X < 0 || v->X > 1)))
                            return -1;
                        *field = v->X;
                    }
                    else
                        v->X = *field;
                    return 1;
                }
                if (op == 74 || op == 75)
                {
                    if (!e.HasComponent<ClothComponent>())
                        return 0;
                    auto& c = e.GetComponent<ClothComponent>();
                    if (op == 75)
                    {
                        c.Reset();
                        return 1;
                    }
                    const bool write    = mode & 1024;
                    const int  property = mode & 1023;
                    if (property == 0)
                    {
                        if (write)
                        {
                            const bool enable = v->Integer != 0;
                            if (enable != c.Enabled)
                            {
                                c.Enabled = enable;
                                c.Reset();
                            }
                        }
                        else
                            v->Integer = c.Enabled;
                        return 1;
                    }
                    float* target = nullptr;
                    switch (property)
                    {
                        case 1:
                            target = &c.GravityScale;
                            break;
                        case 2:
                            target = &c.WindScale;
                            break;
                        case 3:
                            target = &c.InertiaScale;
                            break;
                        case 4:
                            target = &c.StretchCompliance;
                            break;
                        case 5:
                            target = &c.BendCompliance;
                            break;
                        case 6:
                            target = &c.Damping;
                            break;
                        case 7:
                            target = &c.VelocityDrag;
                            break;
                        case 8:
                            target = &c.MaxDistanceScale;
                            break;
                        default:
                            return -1;
                    }
                    if (write)
                    {
                        if (!std::isfinite(v->X) || (property >= 3 && v->X < 0) ||
                            ((property == 3 || property == 6) && v->X > 1))
                            return -1;
                        *target = v->X;
                    }
                    else
                        v->X = *target;
                    return 1;
                }
                if (op == 73)
                {
                    if (!e.HasComponent<MotionColliderComponent>())
                        return 0;
                    auto       c        = e.GetComponent<MotionColliderComponent>();
                    const bool write    = mode & 1024;
                    const int  property = mode & 1023;
                    if (property == 0)
                    {
                        if (write)
                        {
                            if (v->Integer < 0 || v->Integer > 3)
                                return -1;
                            if (size_t(v->Integer) != c.Shape.index())
                            {
                                if (v->Integer == 0)
                                    c.Shape = Physics::Sphere {};
                                if (v->Integer == 1)
                                    c.Shape = Physics::Capsule {};
                                if (v->Integer == 2)
                                    c.Shape = Physics::Plane {};
                                if (v->Integer == 3)
                                    c.Shape = Physics::Box {};
                            }
                        }
                        else
                            v->Integer = c.Shape.index();
                    }
                    else if (property == 1)
                    {
                        if (write)
                            c.Inside = v->Integer != 0;
                        else
                            v->Integer = c.Inside;
                    }
                    else
                    {
                        bool found = true;
                        std::visit(
                            [&](auto& shape) {
                                using T = std::decay_t<decltype(shape)>;
                                if (property == 2)
                                {
                                    if (write)
                                        shape.Offset.Position = Vector(*v);
                                    else
                                        Vector(*v, shape.Offset.Position);
                                }
                                else if (property == 3)
                                {
                                    if (write)
                                        shape.Offset.Rotation = {v->W, v->X, v->Y, v->Z};
                                    else
                                    {
                                        v->X = shape.Offset.Rotation.x;
                                        v->Y = shape.Offset.Rotation.y;
                                        v->Z = shape.Offset.Rotation.z;
                                        v->W = shape.Offset.Rotation.w;
                                    }
                                }
                                else if (property == 4)
                                {
                                    if (write)
                                        shape.Offset.Scale = Vector(*v);
                                    else
                                        Vector(*v, shape.Offset.Scale);
                                }
                                else if constexpr (std::is_same_v<T, Physics::Plane>)
                                {
                                    if (property == 8)
                                    {
                                        if (write)
                                            shape.Normal = Vector(*v);
                                        else
                                            Vector(*v, shape.Normal);
                                    }
                                    else
                                        found = false;
                                }
                                else if constexpr (std::is_same_v<T, Physics::Box>)
                                {
                                    if (property == 9)
                                    {
                                        if (write)
                                            shape.HalfExtents = Vector(*v);
                                        else
                                            Vector(*v, shape.HalfExtents);
                                    }
                                    else
                                        found = false;
                                }
                                else if (property == 5)
                                {
                                    if (write)
                                        shape.Radius = v->X;
                                    else
                                        v->X = shape.Radius;
                                }
                                else if constexpr (std::is_same_v<T, Physics::Capsule>)
                                {
                                    if (property == 6)
                                    {
                                        if (write)
                                            shape.HalfHeight = v->X;
                                        else
                                            v->X = shape.HalfHeight;
                                    }
                                    else if (property == 7)
                                    {
                                        if (write)
                                        {
                                            if (v->Integer < 0 || v->Integer > 2)
                                                throw std::invalid_argument("Invalid axis");
                                            shape.Direction = Physics::Axis(v->Integer);
                                        }
                                        else
                                            v->Integer = int64_t(shape.Direction);
                                    }
                                    else
                                        found = false;
                                }
                                else
                                    found = false;
                            },
                            c.Shape);
                        if (!found)
                            return -1;
                    }
                    if (write)
                    {
                        PhysicsYaml::Encode(c);
                        e.SetComponent<MotionColliderComponent>(c);
                    }
                    return 1;
                }
                if (op >= 40 && op <= 43)
                {
                    bool write    = bool(mode & 1024);
                    int  property = mode & 1023;
                    switch (op)
                    {
                        case 40:
                            return Collider<BoxColliderComponent>(e, property, write, *v, text);
                        case 41:
                            return Collider<SphereColliderComponent>(e, property, write, *v, text);
                        case 42:
                            return Collider<CapsuleColliderComponent>(e, property, write, *v, text);
                        case 43:
                            return Collider<MeshColliderComponent>(e, property, write, *v, text);
                    }
                }
                if (op == 50)
                    return MechanismProperty<CharacterControllerComponent>(e, mode, *v);
                if (op == 60)
                    return MechanismProperty<JointComponent>(e, mode, *v);
                if (op >= 51 && op <= 58)
                {
                    if (!world || !e.HasComponent<CharacterControllerComponent>())
                        return 0;
                    if (op == 51)
                    {
                        v->Integer =
                            int64_t(world->MoveController(id, Vector(*v), ScriptEngine::GetPhysicsDeltaTime()));
                        return 1;
                    }
                    if (op == 52)
                    {
                        v->Integer = world->ResizeController(id, v->X);
                        return 1;
                    }
                    if (op == 53)
                    {
                        v->Integer = world->CanResizeController(id, v->X);
                        return 1;
                    }
                    if (op == 54)
                    {
                        world->TeleportController(id, Vector(*v));
                        return 1;
                    }
                    world->SynchronizeChanges();
                    auto state = world->GetControllerState(id);
                    if (!state)
                        return 0;
                    if (op == 55)
                    {
                        v->Integer = int64_t(state->Flags);
                        Vector(*v, state->Velocity);
                    }
                    if (op == 56)
                    {
                        v->Integer = state->IsGrounded;
                        Vector(*v, state->GroundNormal);
                    }
                    if (op == 57)
                    {
                        v->Integer = int64_t(uint64_t(state->GroundColliderEntity));
                        Vector(*v, state->GroundPoint);
                    }
                    if (op == 58)
                        v->X = ScriptEngine::GetPhysicsDeltaTime();
                    return 1;
                }
                if (op >= 61 && op <= 65)
                {
                    if (!world || !e.HasComponent<JointComponent>())
                        return 0;
                    if (op == 61)
                    {
                        world->ResetJoint(id);
                        return 1;
                    }
                    world->SynchronizeChanges();
                    auto state = world->GetJointState(id);
                    if (!state)
                        return 0;
                    if (op == 62)
                    {
                        v->Integer = state->IsBroken;
                        v->X       = state->CurrentAngle;
                        v->Y       = state->CurrentPosition;
                    }
                    if (op == 63)
                        Vector(*v, state->CurrentForce);
                    if (op == 64)
                        Vector(*v, state->CurrentTorque);
                    return 1;
                }
                if (!e.HasComponent<RigidBodyComponent>())
                    return 0;
                bool write = bool(mode & 1024);
                if (op >= 10 && op <= 14 || op == 19)
                {
                    auto c      = e.GetComponent<RigidBodyComponent>();
                    auto number = [&](float& x) {
                        if (write)
                            x = v->X;
                        else
                            v->X = x;
                    };
                    switch (op)
                    {
                        case 10:
                            if (write)
                            {
                                if (v->Integer < 0 || v->Integer > 2)
                                    return -1;
                                c.Type = RigidBodyType(v->Integer);
                            }
                            else
                                v->Integer = int64_t(c.Type);
                            break;
                        case 11:
                            if (write)
                            {
                                c.Mass     = v->X;
                                c.AutoMass = false;
                            }
                            else
                            {
                                v->X = c.Mass;
                                if (world)
                                {
                                    world->SynchronizeChanges();
                                    if (auto state = world->GetBodyState(id); state && state->Mass > 0)
                                        v->X = state->Mass;
                                }
                            }
                            break;
                        case 12:
                            number(c.LinearDamping);
                            break;
                        case 13:
                            number(c.AngularDamping);
                            break;
                        case 14:
                            number(c.GravityScale);
                            break;
                        case 19:
                            if (write && (v->Integer < 0 || v->Integer > 63))
                                return -1;
                            if (!write)
                                v->Integer = 0;
                            for (unsigned a = 0; a < 3; ++a)
                                if (write)
                                {
                                    c.LockLinear[a]  = v->Integer & (1 << a);
                                    c.LockAngular[a] = v->Integer & (1 << (a + 3));
                                }
                                else
                                    v->Integer |=
                                        (int64_t(c.LockLinear[a]) << a) | (int64_t(c.LockAngular[a]) << (a + 3));
                            break;
                    }
                    if (write)
                    {
                        PhysicsYaml::Encode(c);
                        e.SetComponent<RigidBodyComponent>(c);
                    }
                    return 1;
                }
                if (!world)
                    return 0;
                if (op >= 15 && op <= 18)
                {
                    if (write && (op == 15 || op == 16))
                    {
                        world->SetVelocity(id, Vector(*v), op == 16);
                        return 1;
                    }
                    world->SynchronizeChanges();
                    auto state = world->GetBodyState(id);
                    if (!state)
                        return 0;
                    switch (op)
                    {
                        case 15:
                            Vector(*v, state->LinearVelocity);
                            break;
                        case 16:
                            Vector(*v, state->AngularVelocity);
                            break;
                        case 17:
                            Vector(*v, state->CenterOfMass);
                            break;
                        case 18:
                            v->Integer = state->Sleeping;
                            break;
                    }
                    return 1;
                }
                switch (op)
                {
                    case 20:
                        world->AddForce(id, Vector(*v), ForceMode(mode));
                        break;
                    case 21:
                        world->AddForce(id, Vector(*v), ForceMode(mode), true);
                        break;
                    case 22:
                        if (!second)
                            return -1;
                        world->AddForceAtPosition(id, Vector(*v), Vector(*second), ForceMode(mode));
                        break;
                    case 23:
                        world->MovePosition(id, Vector(*v));
                        break;
                    case 24:
                        world->MoveRotation(id, {v->W, v->X, v->Y, v->Z});
                        break;
                    case 25:
                        world->SetSleeping(id, mode != 0);
                        break;
                    default:
                        return -1;
                }
                return 1;
            }
            catch (...)
            {
                return -1;
            }
        }
        int32_t KANS_SCRIPT_CALL Query(const Scripting::PhysicsQueryData* q,
                                       Scripting::PhysicsHitData*         out,
                                       int32_t                            capacity,
                                       int32_t                            mode) noexcept
        {
            try
            {
                auto* scene = ScriptEngine::GetSceneContext();
                auto* world = scene ? scene->GetPhysicsWorld() : nullptr;
                if (!world || !q || capacity < 0 || q->Kind > 3 || q->Triggers > 2)
                    return 0;
                QueryFilter filter;
                filter.LayerMask        = q->LayerMask;
                filter.IgnoreEntity     = q->Ignore;
                filter.Triggers         = QueryTriggerInteraction(q->Triggers);
                filter.IncludeStatic    = q->Flags & 1;
                filter.IncludeDynamic   = q->Flags & 2;
                filter.IncludeKinematic = q->Flags & 4;
                Physics::WorldShape pose;
                pose.Position = Vector(q->Origin);
                pose.Rotation = {q->RotationW, q->RotationX, q->RotationY, q->RotationZ};
                PhysicsQueryShape shape;
                if (q->Kind == 1)
                {
                    Physics::Sphere s;
                    s.Radius = q->Radius;
                    shape    = s;
                }
                if (q->Kind == 2)
                {
                    Physics::Capsule s;
                    s.Radius     = q->Radius;
                    s.HalfHeight = q->HalfHeight;
                    s.Direction  = Physics::Axis(q->Axis);
                    shape        = s;
                }
                if (q->Kind == 3)
                {
                    Physics::Box s;
                    s.HalfExtents = Vector(q->HalfExtents);
                    shape         = s;
                }
                // ABI query modes: 0 = all hits, 1 = overlap, 2 = closest blocking hit.
                if (mode < 0 || mode > 2)
                    return -1;
                auto writeHit = [](const RaycastHit& h, Scripting::PhysicsHitData& result) {
                    result                = {};
                    result.Collider       = h.ColliderEntity;
                    result.Body           = h.BodyEntity;
                    result.Point          = {h.Point.x, h.Point.y, h.Point.z};
                    result.Normal         = {h.Normal.x, h.Normal.y, h.Normal.z};
                    result.Distance       = h.Distance;
                    result.FaceIndex      = h.FaceIndex;
                    result.InitialOverlap = h.InitialOverlap;
                    std::memcpy(
                        result.BoneName, h.BoneName.data(), std::min(h.BoneName.size(), sizeof(result.BoneName) - 1));
                };
                if (mode == 2)
                {
                    auto hit = q->Kind == 0 ? world->Raycast(pose.Position, Vector(q->Direction), q->Distance, filter) :
                                              world->Cast(shape, pose, Vector(q->Direction), q->Distance, filter);
                    if (!hit)
                        return 0;
                    if (out && capacity > 0)
                        writeHit(*hit, *out);
                    return 1;
                }
                if (mode == 1)
                {
                    if (q->Kind == 0)
                        return -1;
                    auto ids = world->Overlap(shape, pose, filter);
                    for (size_t i = 0; out && i < ids.size() && i < size_t(capacity); ++i)
                    {
                        out[i]          = {};
                        out[i].Collider = uint64_t(ids[i]);
                    }
                    return int32_t(ids.size());
                }
                auto hits = q->Kind == 0 ? world->RaycastAll(pose.Position, Vector(q->Direction), q->Distance, filter) :
                                           world->CastAll(shape, pose, Vector(q->Direction), q->Distance, filter);
                for (size_t i = 0; out && i < hits.size() && i < size_t(capacity); ++i)
                    writeHit(hits[i], out[i]);
                return int32_t(hits.size());
            }
            catch (...)
            {
                return -1;
            }
        }
    } // namespace
    void Install(Scripting::NativeApi& api)
    {
        api.PhysicsCommand = Command;
        api.PhysicsQuery   = Query;
    }
} // namespace Kans::ScriptPhysics
