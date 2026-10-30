#include "kspch.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include <PxPhysicsAPI.h>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <characterkinematic/PxBoxController.h>
#include <characterkinematic/PxCapsuleController.h>
#include <characterkinematic/PxControllerManager.h>
#include <cooking/PxCooking.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxRigidBodyExt.h>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Physics/PhysicsMaterial.h"
#include "Kans3D/Physics/PhysicsSerialization.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/Resource/MeshKey.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Script/ScriptEngine.h"

namespace Kans
{
    namespace
    {
        using namespace physx;
        PxVec3        Px(glm::vec3 v) { return {v.x, v.y, v.z}; }
        glm::vec3     Gl(PxVec3 v) { return {v.x, v.y, v.z}; }
        PxQuat        Px(glm::quat v) { return {v.x, v.y, v.z, v.w}; }
        glm::quat     Gl(PxQuat v) { return {v.w, v.x, v.y, v.z}; }
        PxTransform   Pose(const Physics::WorldShape& w) { return {Px(w.Position), Px(w.Rotation)}; }
        PxFilterFlags Filter(PxFilterObjectAttributes a,
                             PxFilterData             x,
                             PxFilterObjectAttributes b,
                             PxFilterData             y,
                             PxPairFlags&             flags,
                             const void*              settings,
                             PxU32                    settingsSize)
        {
            const bool passiveA =
                PxGetFilterObjectType(a) == PxFilterObjectType::eRIGID_STATIC || PxFilterObjectIsKinematic(a);
            const bool passiveB =
                PxGetFilterObjectType(b) == PxFilterObjectType::eRIGID_STATIC || PxFilterObjectIsKinematic(b);
            if (passiveA && passiveB && !x.word3 && !y.word3 &&
                (settingsSize != sizeof(bool) || !*static_cast<const bool*>(settings)))
                return PxFilterFlag::eSUPPRESS;
            if (!(x.word0 & y.word1) || !(y.word0 & x.word1))
                return PxFilterFlag::eSUPPRESS;
            flags = PxFilterObjectIsTrigger(a) || PxFilterObjectIsTrigger(b) ?
                        PxPairFlags(PxPairFlag::eTRIGGER_DEFAULT) :
                        PxPairFlags(PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT |
                                    PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_TOUCH_PERSISTS |
                                    PxPairFlag::eNOTIFY_TOUCH_LOST | PxPairFlag::eNOTIFY_CONTACT_POINTS |
                                    PxPairFlag::ePRE_SOLVER_VELOCITY | PxPairFlag::eCONTACT_EVENT_POSE);
            if (passiveA && passiveB)
                flags &= ~PxPairFlags(PxPairFlag::eSOLVE_CONTACT);
            return PxFilterFlag::eDEFAULT;
        }
        struct Errors : PxErrorCallback
        {
            void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override
            {
                if (Log::GetCoreLogger())
                {
                    if (code == PxErrorCode::eDEBUG_INFO || code == PxErrorCode::eDEBUG_WARNING ||
                        code == PxErrorCode::ePERF_WARNING)
                        CORE_WARN("PhysX [{}]: {} ({}:{})", int(code), message, file, line);
                    else
                        CORE_ERROR("PhysX [{}]: {} ({}:{})", int(code), message, file, line);
                }
                else
                    std::fprintf(stderr, "PhysX [%d]: %s (%s:%d)\n", int(code), message, file, line);
            }
        };
        struct SDK
        {
            PxDefaultAllocator                     Allocator;
            Errors                                 Error;
            PxFoundation*                          Foundation = nullptr;
            PxPhysics*                             Physics    = nullptr;
            size_t                                 Worlds = 0, Actors = 0;
            uint64_t                               Cooks = 0;
            std::map<std::string, PxConvexMesh*>   Convexes;
            std::map<std::string, PxTriangleMesh*> Triangles;
            void                                   Init()
            {
                if (Physics)
                    return;
                Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, Allocator, Error);
                if (!Foundation)
                    throw std::runtime_error("PhysX foundation creation failed");
                Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *Foundation, PxTolerancesScale(), false, nullptr);
                if (!Physics || !PxInitExtensions(*Physics, nullptr))
                    throw std::runtime_error("PhysX initialization failed");
            }
            void Release()
            {
                for (auto& [key, v] : Convexes)
                    if (v)
                        v->release();
                Convexes.clear();
                for (auto& [key, v] : Triangles)
                    if (v)
                        v->release();
                Triangles.clear();
                if (Physics)
                {
                    PxCloseExtensions();
                    Physics->release();
                    Physics = nullptr;
                }
                if (Foundation)
                {
                    Foundation->release();
                    Foundation = nullptr;
                }
            }
            ~SDK() { Release(); }
        };
        SDK& Sdk()
        {
            static SDK sdk;
            return sdk;
        }
        // Scene updates are on their owning thread. SDK creation/cooking is serialized across worlds.
        std::recursive_mutex& SdkMutex()
        {
            static std::recursive_mutex mutex;
            return mutex;
        }
        std::string CookingKey(AssetID                       id,
                               uint32_t                      section,
                               bool                          convex,
                               const std::vector<glm::vec3>& points,
                               const std::vector<uint32_t>&  indices)
        {
            // Retain exact geometry bytes, not a pointer/version counter or collision-prone hash.
            std::string key   = id.ToString() + ":" + std::to_string(section) + (convex ? ":C:" : ":T:");
            const auto  count = uint64_t(points.size());
            key.append(reinterpret_cast<const char*>(&count), sizeof(count));
            for (auto p : points)
                for (int a = 0; a < 3; ++a)
                    key.append(reinterpret_cast<const char*>(&p[a]), sizeof(float));
            if (!indices.empty())
                key.append(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint32_t));
            return key;
        }
    } // namespace
    struct PhysicsWorld::Impl : PxSimulationEventCallback, PxUserControllerHitReport, PxControllerBehaviorCallback
    {
        struct Body
        {
            PxRigidActor*      Actor      = nullptr;
            uint64_t           Generation = 0;
            PxTransform        Previous {PxIdentity}, Current {PxIdentity}, Authored {PxIdentity};
            glm::vec3          Scale {1};
            RigidBodyComponent Settings;
            bool               Explicit = false, HasTriangleMesh = false, ReconcileSleeping = false;
        };
        Scene&                  Owner;
        PhysicsSettings         Settings;
        PxScene*                World      = nullptr;
        PxDefaultCpuDispatcher* Dispatcher = nullptr;
        std::map<UUID, Body>    Bodies;
        std::set<UUID>          ChangedTransforms;
        enum DirtyReason : uint8_t
        {
            Topology   = 1,
            MeshSource = 2
        };
        struct MeshSourceStamp
        {
            AssetID              Asset;
            uint32_t             Section          = UINT32_MAX;
            uint64_t             EditableIdentity = 0;
            std::string          Digest;
            Ref<const MeshAsset> Resident;
            bool                 operator==(const MeshSourceStamp& b) const
            {
                return Asset == b.Asset && Section == b.Section && EditableIdentity == b.EditableIdentity &&
                       Digest == b.Digest && Resident == b.Resident;
            }
        };
        bool                    Initialized = false;
        std::map<UUID, uint8_t> DirtyEntities;
        std::set<UUID>          DirtyBodies;
        // Retained memberships identify old owners even after remove/destroy/reparent signals.
        std::map<UUID, UUID>              ColliderOwners;
        std::map<UUID, std::set<UUID>>    GroupColliders;
        std::map<UUID, MeshSourceStamp>   MeshSources;
        std::map<AssetID, std::set<UUID>> MeshUsers;
        std::map<UUID, glm::mat4>         ColliderLocalMatrices;
        ForceFieldSet                     Fields;
        PhysicsStatistics                 Stats;
        std::vector<std::string>          Errors;
        std::set<std::string>             Reported;
        double                            Time = 0;
        struct SharedMaterial
        {
            PxMaterial* Material = nullptr;
        };
        std::map<AssetID, SharedMaterial> Materials;
        struct Listeners
        {
            uint64_t                                                                    Next = 0;
            std::map<uint64_t, std::function<void(const PhysicsPairEvent&)>>            Collision, Trigger;
            std::map<uint64_t, std::function<void(const JointBreakEvent&)>>             JointBreak;
            std::map<uint64_t, std::function<void(UUID, const ControllerColliderHit&)>> ControllerHit;
        };
        std::shared_ptr<Listeners> Subscribers = std::make_shared<Listeners>();
        using ShapeKey                         = std::array<uint64_t, 6>; // body A/B, collider A/B, shape kind A/B
        using BodyKey                          = std::pair<UUID, UUID>;
        std::mutex                                EventMutex;
        std::map<ShapeKey, PhysicsPairEvent>      ContactPairs, TriggerPairs;
        std::map<BodyKey, PhysicsPairEvent>       PreviousContacts, PreviousTriggers;
        std::map<UUID, std::pair<PxVec3, PxVec3>> StepVelocities;
        std::map<UUID, PxTransform>               KinematicTargets;
        bool                                      Dispatching = false;
        std::vector<std::function<void()>>        Commands;
#include "Mechanisms.inl"
        static UUID     Identity(const void* p) { return UUID(uint64_t(reinterpret_cast<uintptr_t>(p))); }
        static ShapeKey Key(PxActor* a, PxActor* b, PxShape* x, PxShape* y)
        {
            ShapeKey key {uint64_t(Identity(a->userData)),
                          uint64_t(Identity(b->userData)),
                          uint64_t(Identity(x->userData)),
                          uint64_t(Identity(y->userData)),
                          x->getQueryFilterData().word2,
                          y->getQueryFilterData().word2};
            if (key[0] > key[1])
            {
                std::swap(key[0], key[1]);
                std::swap(key[2], key[3]);
                std::swap(key[4], key[5]);
            }
            return key;
        }
        static PhysicsPairEvent Pair(const ShapeKey& key)
        {
            PhysicsPairEvent e;
            e.BodyA     = key[0];
            e.BodyB     = key[1];
            e.ColliderA = key[2];
            e.ColliderB = key[3];
            return e;
        }
        void onConstraintBreak(PxConstraintInfo* info, PxU32 count) override { CollectBreaks(info, count); }
        void onWake(PxActor**, PxU32) override {}
        void onSleep(PxActor**, PxU32) override {}
        void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
        void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override
        {
            // Removed SDK objects are never dereferenced. RebuildBody already invalidates their shape pairs.
            if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
                return;
            std::lock_guard lock(EventMutex);
            for (PxU32 i = 0; i < count; ++i)
            {
                const auto& p = pairs[i];
                if (p.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1))
                    continue;
                auto key = Key(header.actors[0], header.actors[1], p.shapes[0], p.shapes[1]);
                if (p.events & PxPairFlag::eNOTIFY_TOUCH_LOST)
                {
                    ContactPairs.erase(key);
                    continue;
                }
                auto                            e       = Pair(key);
                bool                            reverse = uint64_t(Identity(header.actors[0]->userData)) != key[0];
                std::vector<PxContactPairPoint> points(p.contactCount);
                auto                            n         = p.extractContacts(points.data(), PxU32(points.size()));
                const PxContactPairVelocity*    pre       = nullptr;
                const PxContactPairPose*        eventPose = nullptr;
                PxContactPairExtraDataIterator  extra(header.extraDataStream, header.extraDataStreamSize);
                while (extra.nextItemSet())
                {
                    if (extra.contactPairIndex > i)
                        break;
                    if (extra.preSolverVelocity)
                        pre = extra.preSolverVelocity;
                    if (extra.eventPose)
                        eventPose = extra.eventPose;
                }
                const auto velocity = [&](unsigned index, PxVec3 point) {
                    auto id = Identity(header.actors[index]->userData);
                    auto b  = Bodies.find(id);
                    if (b == Bodies.end())
                        return PxVec3(0);
                    auto*  d        = b->second.Actor->is<PxRigidBody>();
                    auto   pose     = eventPose ? eventPose->globalPose[index] : b->second.Current;
                    auto   center   = d ? (pose * d->getCMassLocalPose()).p : pose.p;
                    auto   fallback = StepVelocities.find(id);
                    PxVec3 linear   = pre                              ? pre->linearVelocity[index] :
                                      fallback == StepVelocities.end() ? PxVec3(0) :
                                                                         fallback->second.first;
                    PxVec3 angular  = pre                              ? pre->angularVelocity[index] :
                                      fallback == StepVelocities.end() ? PxVec3(0) :
                                                                         fallback->second.second;
                    return linear + angular.cross(point - center);
                };
                if (n)
                    e.RelativeVelocity =
                        Gl(velocity(0, points[0].position) - velocity(1, points[0].position)) * (reverse ? -1.f : 1.f);
                for (PxU32 c = 0; c < n; ++c)
                {
                    auto normal  = Gl(points[c].normal) * (reverse ? -1.f : 1.f);
                    auto impulse = Gl(points[c].impulse) * (reverse ? -1.f : 1.f);
                    e.TotalImpulse += impulse;
                    if (e.Contacts.size() < Settings.MaxContactsPerPair)
                        e.Contacts.push_back(
                            {Gl(points[c].position), normal, impulse, points[c].separation, e.ColliderA, e.ColliderB});
                }
                ContactPairs[key] = std::move(e);
            }
        }
        void onTrigger(PxTriggerPair* pairs, PxU32 count) override
        {
            std::lock_guard lock(EventMutex);
            for (PxU32 i = 0; i < count; ++i)
            {
                auto& p = pairs[i];
                if (p.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
                    continue;
                auto key = Key(p.triggerActor, p.otherActor, p.triggerShape, p.otherShape);
                if (p.status & PxPairFlag::eNOTIFY_TOUCH_LOST)
                    TriggerPairs.erase(key);
                else
                    TriggerPairs[key] = Pair(key);
            }
        }
        void InvalidatePairs(UUID id)
        {
            for (auto* pairs : {&ContactPairs, &TriggerPairs})
                for (auto it = pairs->begin(); it != pairs->end();)
                    if (it->first[0] == uint64_t(id) || it->first[1] == uint64_t(id))
                        it = pairs->erase(it);
                    else
                        ++it;
        }
        bool Alive(UUID id) const
        {
            auto e = Owner.GetEntityByUUID(id);
            return Valid(e) && Owner.IsEntityActive(e);
        }
        void DispatchPairs(bool trigger)
        {
            const auto&                         shapes   = trigger ? TriggerPairs : ContactPairs;
            auto&                               previous = trigger ? PreviousTriggers : PreviousContacts;
            std::map<BodyKey, PhysicsPairEvent> current;
            for (const auto& [key, pair] : shapes)
            {
                if (!Alive(pair.BodyA) || !Alive(pair.BodyB) || !Alive(pair.ColliderA) || !Alive(pair.ColliderB))
                    continue;
                auto [it, inserted] = current.try_emplace({pair.BodyA, pair.BodyB}, pair);
                auto& e             = it->second;
                if (inserted)
                {
                    e.Contacts.clear();
                    e.TotalImpulse = {};
                }
                e.Colliders.emplace_back(pair.ColliderA, pair.ColliderB);
                e.TotalImpulse += pair.TotalImpulse;
                for (const auto& c : pair.Contacts)
                    if (e.Contacts.size() < Settings.MaxContactsPerPair)
                        e.Contacts.push_back(c);
            }
            std::vector<PhysicsPairEvent> events;
            for (auto& [key, e] : current)
            {
                auto old = previous.find(key);
                e.Phase  = old == previous.end() ? PhysicsEventPhase::Enter : PhysicsEventPhase::Stay;
                if (old != previous.end())
                    e.RelativeVelocity = old->second.RelativeVelocity;
                std::sort(e.Colliders.begin(), e.Colliders.end());
                e.Colliders.erase(std::unique(e.Colliders.begin(), e.Colliders.end()), e.Colliders.end());
                events.push_back(e);
            }
            for (const auto& [key, pair] : previous)
                if (!current.count(key))
                {
                    auto e  = pair;
                    e.Phase = PhysicsEventPhase::Exit;
                    e.Contacts.clear();
                    e.TotalImpulse = {};
                    e.Removed      = !Alive(e.BodyA) || !Alive(e.BodyB);
                    for (auto [a, b] : e.Colliders)
                        e.Removed |= !Alive(a) || !Alive(b) || !ColliderOwners.count(a) || !ColliderOwners.count(b);
                    events.push_back(std::move(e));
                }
            previous                        = std::move(current);
            auto&                 listeners = trigger ? Subscribers->Trigger : Subscribers->Collision;
            std::vector<uint64_t> order;
            for (auto& [id, f] : listeners)
                order.push_back(id);
            for (const auto& e : events)
            {
                // Global subscribers receive identities (including removed IDs). Per-entity scripting validates every
                // recipient.
                for (auto id : order)
                    if (auto it = listeners.find(id); it != listeners.end())
                    {
                        auto f = it->second;
                        f(e);
                    }
                ScriptEngine::OnPhysicsEvent(Owner, e, trigger);
            }
        }
        void UpdateMaterial(AssetID id, PxMaterial* material)
        {
            auto values = Settings.DefaultMaterial;
            bool loaded = false;
            if (auto* assets = Owner.GetAssetManager())
            {
                auto result = assets->Load<PhysicsMaterialAsset>(id);
                if (auto a = std::get_if<Ref<const PhysicsMaterialAsset>>(&result); a && *a && (*a)->IsValid())
                {
                    values = (*a)->GetData();
                    loaded = true;
                }
            }
            if (!loaded)
                Error(0, "Physics material unavailable: " + id.ToString());
            bool changed = material->getStaticFriction() != values.StaticFriction ||
                           material->getDynamicFriction() != values.DynamicFriction ||
                           material->getRestitution() != values.Restitution ||
                           material->getFrictionCombineMode() != PxCombineMode::Enum(values.FrictionCombine) ||
                           material->getRestitutionCombineMode() != PxCombineMode::Enum(values.RestitutionCombine);
            material->setStaticFriction(values.StaticFriction);
            material->setDynamicFriction(values.DynamicFriction);
            material->setRestitution(values.Restitution);
            material->setFrictionCombineMode(PxCombineMode::Enum(values.FrictionCombine));
            material->setRestitutionCombineMode(PxCombineMode::Enum(values.RestitutionCombine));
            if (changed)
            {
                std::set<UUID> users;
                for (auto& [bodyID, body] : Bodies)
                {
                    std::vector<PxShape*> shapes(body.Actor->getNbShapes());
                    body.Actor->getShapes(shapes.data(), PxU32(shapes.size()));
                    for (auto* shape : shapes)
                    {
                        PxMaterial* m = nullptr;
                        if (shape->getMaterials(&m, 1) && m == material)
                        {
                            users.insert(bodyID);
                            break;
                        }
                    }
                }
                auto affected = users;
                for (const auto& [key, pair] : ContactPairs)
                    if (users.count(pair.BodyA) || users.count(pair.BodyB))
                    {
                        affected.insert(pair.BodyA);
                        affected.insert(pair.BodyB);
                    }
                for (auto id : affected)
                    if (auto b = Bodies.find(id);
                        b != Bodies.end() && b->second.Settings.Type == RigidBodyType::Dynamic)
                        if (auto* d = b->second.Actor->is<PxRigidDynamic>())
                            d->wakeUp();
            }
        }
        PxMaterial* Material(const ColliderMaterial& c)
        {
            if (c.Asset.IsValid())
            {
                auto [it, inserted] = Materials.try_emplace(c.Asset);
                if (inserted)
                {
                    const auto& d = Settings.DefaultMaterial;
                    it->second.Material =
                        Sdk().Physics->createMaterial(d.StaticFriction, d.DynamicFriction, d.Restitution);
                    UpdateMaterial(c.Asset, it->second.Material);
                }
                it->second.Material->acquireReference();
                return it->second.Material;
            }
            const auto& m = c.UseDefault ? Settings.DefaultMaterial : c.Values;
            if (std::holds_alternative<AssetError>(ValidatePhysicsMaterial(m)))
                return nullptr;
            auto* material = Sdk().Physics->createMaterial(m.StaticFriction, m.DynamicFriction, m.Restitution);
            if (material)
            {
                material->setFrictionCombineMode(PxCombineMode::Enum(m.FrictionCombine));
                material->setRestitutionCombineMode(PxCombineMode::Enum(m.RestitutionCombine));
            }
            return material;
        }
        Impl(Scene& owner, PhysicsSettings settings) : Owner(owner), Settings(std::move(settings))
        {
            Settings.Validate();
            std::lock_guard lock(SdkMutex());
            auto&           sdk = Sdk();
            sdk.Init();
            Dispatcher = PxDefaultCpuDispatcherCreate(Settings.WorkerThreads);
            PxSceneDesc desc(sdk.Physics->getTolerancesScale());
            desc.gravity                 = Px(Settings.Gravity);
            desc.cpuDispatcher           = Dispatcher;
            desc.filterShader            = Filter;
            desc.filterShaderData        = &Settings.EnableKinematicPairs;
            desc.filterShaderDataSize    = sizeof(Settings.EnableKinematicPairs);
            desc.simulationEventCallback = this;
            {
                desc.kineKineFilteringMode   = PxPairFilteringMode::eKEEP;
                desc.staticKineFilteringMode = PxPairFilteringMode::eKEEP;
            }
            desc.flags |= PxSceneFlag::eENABLE_CCD;
            // PhysX requires a positive threshold; zero is the engine's valid "bounce at any speed" value.
            desc.bounceThresholdVelocity = std::max(Settings.BounceThreshold, std::numeric_limits<float>::min());
            World                        = sdk.Physics->createScene(desc);
            if (!World)
            {
                Dispatcher->release();
                throw std::runtime_error("PhysX scene creation failed");
            }
            ControllerManager = PxCreateControllerManager(*World);
            if (!ControllerManager)
                throw std::runtime_error("Controller manager creation failed");
            ++sdk.Worlds;
        }
        ~Impl()
        {
            std::lock_guard lock(SdkMutex());
            Clear();
            for (auto& [id, m] : Materials)
                m.Material->release();
            ControllerManager->release();
            World->release();
            Dispatcher->release();
            --Sdk().Worlds;
        }
        void Clear()
        {
            for (auto& [id, j] : Joints)
                ReleaseJoint(j);
            Joints.clear();
            BodyJoints.clear();
            while (!Bodies.empty())
                ReleaseBody(Bodies.begin()->first);
            Stats.ActorCount = 0;
        }
        void Error(UUID id, const std::string& message)
        {
            auto text = "Entity " + std::to_string(uint64_t(id)) + ": " + message;
            if (Reported.insert(text).second)
            {
                Errors.push_back(text);
                if (Log::GetCoreLogger())
                    CORE_ERROR("Physics: {}", text);
                else
                    std::fprintf(stderr, "Physics: %s\n", text.c_str());
            }
        }
        Entity BodyOwner(Entity entity)
        {
            for (auto e = entity; e; e = Owner.GetParent(e))
                if (e.HasComponent<RigidBodyComponent>() || e.HasComponent<CharacterControllerComponent>())
                    return e;
            return entity;
        }
        bool HasCollider(Entity e)
        {
            return e.HasComponent<BoxColliderComponent>() || e.HasComponent<SphereColliderComponent>() ||
                   e.HasComponent<CapsuleColliderComponent>() || e.HasComponent<MeshColliderComponent>();
        }
        void AddShape(Entity                    entity,
                      Body&                     body,
                      const ColliderProperties& properties,
                      Physics::Shape            descriptor,
                      std::vector<float>&       densities)
        {
            const auto w = std::visit(
                [&](const auto& s) { return Physics::TransformShape(s.Offset, Owner.GetWorldTransform(entity)); },
                descriptor);
            if (!std::isfinite(w.Scale.x + w.Scale.y + w.Scale.z) ||
                glm::min(w.Scale.x, glm::min(w.Scale.y, w.Scale.z)) < 1e-6f)
            {
                Error(entity.GetUUID(), "Collider has degenerate scale");
                return;
            }
            auto&            sdk = Sdk();
            PxGeometryHolder geometry;
            PxTransform      worldPose = Pose(w);
            bool             valid     = true;
            std::visit(
                [&](const auto& shape) {
                    using T = std::decay_t<decltype(shape)>;
                    if constexpr (std::is_same_v<T, Physics::Box>)
                        geometry.storeAny(PxBoxGeometry(Px(glm::abs(shape.HalfExtents) * w.Scale)));
                    else if constexpr (std::is_same_v<T, Physics::Sphere>)
                        geometry.storeAny(PxSphereGeometry(Physics::ScaledRadius(shape, w)));
                    else if constexpr (std::is_same_v<T, Physics::Capsule>)
                    {
                        glm::vec3 a, b;
                        float     radius;
                        Physics::CapsuleSegment(shape, w, a, b, radius);
                        glm::vec3 axis(0);
                        axis[unsigned(shape.Direction)] = 1;
                        auto rotation                   = glm::rotation(glm::vec3(1, 0, 0), axis);
                        worldPose.q                     = Px(w.Rotation * rotation);
                        geometry.storeAny(PxCapsuleGeometry(radius, glm::length(b - a) * .5f));
                    }
                    else
                    {
                        constexpr bool convex = std::is_same_v<T, Physics::ConvexMesh>;
                        if (!convex && body.Settings.Type == RigidBodyType::Dynamic)
                        {
                            Error(entity.GetUUID(), "Dynamic triangle mesh collider is unsupported; use a convex mesh");
                            valid = false;
                            return;
                        }
                        std::vector<glm::vec3> points;
                        std::vector<uint32_t>  indices;
                        AssetID                meshID       = shape.Mesh;
                        uint32_t               sectionIndex = shape.Section;
                        if constexpr (convex)
                            points = shape.Points;
                        if (points.empty())
                        {
                            Ref<const MeshAsset> mesh;
                            if (!meshID.IsValid() && entity.HasComponent<MeshComponent>())
                            {
                                const auto& component = entity.GetComponent<MeshComponent>();
                                meshID                = component.Mesh;
                                if (sectionIndex == UINT32_MAX && component.Section)
                                    sectionIndex = *component.Section;
                            }
                            if (meshID.IsValid() && Owner.GetAssetManager())
                            {
                                auto result = Owner.GetAssetManager()->Load<MeshAsset>(meshID);
                                if (auto* asset = std::get_if<Ref<const MeshAsset>>(&result))
                                    mesh = *asset;
                            }
                            const MeshData*        data = mesh ? &mesh->GetData() : nullptr;
                            Ref<const RuntimeMesh> editable;
                            if (!data && !meshID.IsValid() && entity.HasComponent<EditableMeshComponent>())
                            {
                                editable = Owner.ResolveEditableMesh(entity);
                                if (editable)
                                    data = &editable->Source->GetData();
                            }
                            if (!data)
                            {
                                Error(entity.GetUUID(), "Mesh collider source is unavailable");
                                valid = false;
                                return;
                            }
                            uint32_t first = 0, count = uint32_t(data->Indices.size());
                            if (sectionIndex != UINT32_MAX)
                            {
                                if (sectionIndex >= data->Sections.size())
                                {
                                    Error(entity.GetUUID(), "Mesh collider section out of range");
                                    valid = false;
                                    return;
                                }
                                const auto& section = data->Sections[sectionIndex];
                                first               = section.FirstIndex;
                                count               = section.IndexCount;
                            }
                            if (uint64_t(first) + count > data->Indices.size() || count % 3)
                            {
                                valid = false;
                                return;
                            }
                            // Compact the selected section so convex cooking excludes other sections' vertices.
                            std::map<uint32_t, uint32_t> remap;
                            for (uint32_t i = first; i < first + count; ++i)
                            {
                                auto index = data->Indices[i];
                                if (index >= data->Vertices.size())
                                {
                                    valid = false;
                                    return;
                                }
                                auto [it, inserted] = remap.emplace(index, uint32_t(points.size()));
                                if (inserted)
                                {
                                    auto p = data->Vertices[index].Position;
                                    points.push_back({p[0], p[1], p[2]});
                                }
                                indices.push_back(it->second);
                            }
                        }
                        auto            key = CookingKey(meshID, sectionIndex, convex, points, indices);
                        PxCookingParams params(sdk.Physics->getTolerancesScale());
                        if constexpr (convex)
                        {
                            auto          it     = sdk.Convexes.find(key);
                            PxConvexMesh* cooked = nullptr;
                            if (it != sdk.Convexes.end())
                                cooked = it->second;
                            else
                            {
                                ++sdk.Cooks;
                                PxConvexMeshDesc desc;
                                desc.points.count  = PxU32(points.size());
                                desc.points.stride = sizeof(glm::vec3);
                                desc.points.data   = points.data();
                                desc.flags         = PxConvexFlag::eCOMPUTE_CONVEX;
                                cooked = PxCreateConvexMesh(params, desc, sdk.Physics->getPhysicsInsertionCallback());
                                sdk.Convexes.emplace(std::move(key), cooked);
                            }
                            if (cooked)
                                geometry.storeAny(PxConvexMeshGeometry(cooked, PxMeshScale(Px(w.Scale))));
                            else
                            {
                                Error(entity.GetUUID(), "Convex mesh cooking failed");
                                valid = false;
                            }
                        }
                        else
                        {
                            auto            it     = sdk.Triangles.find(key);
                            PxTriangleMesh* cooked = nullptr;
                            if (it != sdk.Triangles.end())
                                cooked = it->second;
                            else
                            {
                                ++sdk.Cooks;
                                PxTriangleMeshDesc desc;
                                desc.points.count     = PxU32(points.size());
                                desc.points.stride    = sizeof(glm::vec3);
                                desc.points.data      = points.data();
                                desc.triangles.count  = PxU32(indices.size() / 3);
                                desc.triangles.stride = 3 * sizeof(uint32_t);
                                desc.triangles.data   = indices.data();
                                cooked = PxCreateTriangleMesh(params, desc, sdk.Physics->getPhysicsInsertionCallback());
                                sdk.Triangles.emplace(std::move(key), cooked);
                            }
                            if (cooked)
                                geometry.storeAny(PxTriangleMeshGeometry(cooked, PxMeshScale(Px(w.Scale))));
                            else
                            {
                                Error(entity.GetUUID(), "Triangle mesh cooking failed");
                                valid = false;
                            }
                        }
                    }
                },
                descriptor);
            if (!valid)
                return;
            auto* material = Material(properties.Material);
            if (!material)
            {
                Error(entity.GetUUID(), "Invalid collider material");
                return;
            }
            auto flags = PxShapeFlag::eSCENE_QUERY_SHAPE |
                         (properties.IsTrigger ? PxShapeFlag::eTRIGGER_SHAPE : PxShapeFlag::eSIMULATION_SHAPE);
            if (properties.IsTrigger && geometry.getType() == PxGeometryType::eTRIANGLEMESH)
            {
                material->release();
                Error(entity.GetUUID(), "Triangle meshes cannot be triggers; use convex trigger shapes");
                return;
            }
            auto* shape = sdk.Physics->createShape(geometry.any(), *material, true, flags);
            material->release();
            if (!shape)
            {
                Error(entity.GetUUID(), "Invalid collider geometry");
                return;
            }
            shape->userData = reinterpret_cast<void*>(uintptr_t(uint64_t(entity.GetUUID())));
            shape->setLocalPose(body.Authored.getInverse() * worldPose);
            uint32_t layer = properties.Layer.value_or(body.Settings.Layer);
            if (layer >= 32)
            {
                shape->release();
                Error(entity.GetUUID(), "Physics layer must be 0..31");
                return;
            }
            PxFilterData filter(1u << layer, Settings.CollisionMatrix[layer], uint32_t(descriptor.index()), 0);
            shape->setSimulationFilterData(filter);
            shape->setQueryFilterData(filter);
            const bool attached = body.Actor->attachShape(*shape);
            shape->release();
            if (!attached)
            {
                Error(entity.GetUUID(), "Collider could not be attached to body");
                return;
            }
            if (geometry.getType() == PxGeometryType::eTRIANGLEMESH)
                body.HasTriangleMesh = true;
            if (!properties.IsTrigger)
                densities.push_back(properties.Density);
        }
        bool Valid(Entity e) const
        {
            return e && Owner.m_Registry.valid(entt::entity(e)) &&
                   e.HasComponent<IDComponent, TransformComponent, RelationshipComponent>();
        }
        void Queue(Entity e, uint8_t reason, bool subtree, bool stopAtNestedBodies = false)
        {
            if (!e || !Owner.m_Registry.valid(entt::entity(e)))
                return;
            if (e.HasComponent<IDComponent>())
            {
                const auto id = e.GetUUID();
                if (e.HasComponent<JointComponent>() || Joints.count(id))
                    DirtyJoints.insert(id);
                if (Bodies.count(id) || ColliderOwners.count(id) || HasCollider(e) ||
                    (e.HasComponent<RigidBodyComponent>() || e.HasComponent<CharacterControllerComponent>()))
                    DirtyEntities[id] |= reason;
            }
            if (subtree && e.HasComponent<RelationshipComponent>())
                for (auto child : Owner.GetChildren(e))
                    if (!stopAtNestedBodies || (!child.HasComponent<RigidBodyComponent>() &&
                                                !child.HasComponent<CharacterControllerComponent>()))
                        Queue(child, reason, true, stopAtNestedBodies);
        }
        MeshSourceStamp SourceStamp(Entity e)
        {
            MeshSourceStamp stamp;
            if (!Valid(e) || !e.HasComponent<MeshColliderComponent>())
                return stamp;
            const auto& shape = e.GetComponent<MeshColliderComponent>().Shape;
            if (!shape.Points.empty())
                return stamp;
            stamp.Asset   = shape.Mesh;
            stamp.Section = shape.Section;
            if (!stamp.Asset.IsValid() && e.HasComponent<MeshComponent>())
            {
                const auto& mesh = e.GetComponent<MeshComponent>();
                stamp.Asset      = mesh.Mesh;
                if (stamp.Section == UINT32_MAX && mesh.Section)
                    stamp.Section = *mesh.Section;
            }
            if (!stamp.Asset.IsValid() && e.HasComponent<EditableMeshComponent>())
                if (auto mesh = Owner.ResolveEditableMesh(e))
                    stamp.EditableIdentity = mesh->Identity;
            if (stamp.Asset.IsValid() && Owner.GetAssetManager())
            {
                auto* assets = Owner.GetAssetManager();
                if (auto record = assets->GetRegistry()->Find(stamp.Asset, assets->GetTarget()))
                    stamp.Digest = record->Location.ContentDigest;
                stamp.Resident = assets->GetLoaded<MeshAsset>(stamp.Asset);
            }
            return stamp;
        }
        glm::mat4 ColliderLocalMatrix(Entity collider, UUID owner)
        {
            const auto body  = Owner.GetEntityByUUID(owner);
            const auto pose  = Physics::TransformShape({}, Owner.GetWorldTransform(body));
            const auto rigid = glm::translate(glm::mat4(1), pose.Position) * glm::mat4_cast(pose.Rotation);
            return glm::inverse(rigid) * Owner.GetWorldTransform(collider);
        }
        static bool SameMatrix(const glm::mat4& a, const glm::mat4& b)
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (std::abs(a[c][r] - b[c][r]) > 1e-5f)
                        return false;
            return true;
        }
        void RefreshEntity(UUID id, uint8_t reason)
        {
            ++Stats.TopologyEntitiesVisited;
            auto       e         = Owner.GetEntityByUUID(id);
            const auto source    = SourceStamp(e);
            const auto oldSource = MeshSources.find(id);
            // Render-material edits and unrelated mesh components do not change collision geometry.
            if (reason == MeshSource && (oldSource == MeshSources.end() || oldSource->second == source))
                return;
            if (oldSource != MeshSources.end())
            {
                if (oldSource->second.Asset.IsValid())
                {
                    auto users = MeshUsers.find(oldSource->second.Asset);
                    if (users != MeshUsers.end())
                    {
                        users->second.erase(id);
                        if (users->second.empty())
                            MeshUsers.erase(users);
                    }
                }
                MeshSources.erase(oldSource);
            }
            if (auto old = ColliderOwners.find(id); old != ColliderOwners.end())
            {
                const auto owner = old->second;
                DirtyBodies.insert(owner);
                auto group = GroupColliders.find(owner);
                if (group != GroupColliders.end())
                {
                    group->second.erase(id);
                    if (group->second.empty())
                        GroupColliders.erase(group);
                }
                ColliderOwners.erase(old);
                ColliderLocalMatrices.erase(id);
            }
            if (Bodies.count(id))
                DirtyBodies.insert(id);
            if (!Valid(e) || !Owner.IsEntityActive(e))
                return;
            if (e.HasComponent<RigidBodyComponent>())
                DirtyBodies.insert(id);
            if (e.HasComponent<CharacterControllerComponent>())
            {
                DirtyBodies.insert(id);
                return;
            }
            if (!HasCollider(e))
                return;
            if (BodyOwner(e).HasComponent<CharacterControllerComponent>())
                return;
            const auto owner   = BodyOwner(e).GetUUID();
            ColliderOwners[id] = owner;
            GroupColliders[owner].insert(id);
            DirtyBodies.insert(owner);
            ColliderLocalMatrices[id] = ColliderLocalMatrix(e, owner);
            if (e.HasComponent<MeshColliderComponent>())
            {
                MeshSources[id] = source;
                if (source.Asset.IsValid())
                    MeshUsers[source.Asset].insert(id);
            }
        }
        void InitializeIndex()
        {
            // The only whole-registry scan: initial world construction; subsequent changes use the dirty index.
            ++Stats.FullRebuildCount;
            for (auto handle : Owner.m_Registry.view<IDComponent, TransformComponent>())
                Queue({handle, &Owner}, Topology, false);
            Initialized = true;
        }
        void RebuildBody(UUID id)
        {
            auto       e      = Owner.GetEntityByUUID(id);
            const auto group  = GroupColliders.find(id);
            const bool wanted = Valid(e) && Owner.IsEntityActive(e) &&
                                (e.HasComponent<RigidBodyComponent>() || group != GroupColliders.end());
            if (Valid(e) && Owner.IsEntityActive(e) && e.HasComponent<CharacterControllerComponent>())
            {
                BuildController(e);
                return;
            }
            DetachBodyJoints(id);
            // Static/kinematic replacements also need sleeping contact partners to participate in narrowphase.
            for (const auto& [key, pair] : PreviousContacts)
            {
                if (pair.BodyA != id && pair.BodyB != id)
                    continue;
                auto other = Bodies.find(pair.BodyA == id ? pair.BodyB : pair.BodyA);
                if (other == Bodies.end() || other->second.Settings.Type != RigidBodyType::Dynamic)
                    continue;
                if (auto* d = other->second.Actor->is<PxRigidDynamic>(); d && d->isSleeping())
                {
                    other->second.ReconcileSleeping = true;
                    d->wakeUp();
                }
            }
            InvalidatePairs(id);
            std::optional<PhysicsBodyState> saved;
            if (auto old = Bodies.find(id); old != Bodies.end())
            {
                saved.emplace();
                const auto& b   = old->second;
                saved->Position = Gl(b.Current.p);
                saved->Rotation = Gl(b.Current.q);
                if (auto* d = b.Actor->is<PxRigidDynamic>())
                {
                    saved->LinearVelocity  = Gl(d->getLinearVelocity());
                    saved->AngularVelocity = Gl(d->getAngularVelocity());
                    saved->Sleeping        = d->isSleeping();
                }
                ReleaseBody(id);
            }
            if (!wanted)
            {
                if (saved)
                    ++Stats.RebuildCount;
                return;
            }
            ++Stats.RebuildCount;
            std::vector<Entity> colliders;
            if (group != GroupColliders.end())
                for (auto collider : group->second)
                    if (auto entity = Owner.GetEntityByUUID(collider); Valid(entity))
                        colliders.push_back(entity);
            Body b;
            b.Explicit = e.HasComponent<RigidBodyComponent>();
            if (b.Explicit)
                b.Settings = e.GetComponent<RigidBodyComponent>();
            else
                b.Settings.Type = RigidBodyType::Static;
            const auto w = Physics::TransformShape({}, Owner.GetWorldTransform(e));
            b.Scale      = w.Scale;
            auto pose    = Pose(w);
            b.Authored   = pose;
            if (saved.has_value() && !ChangedTransforms.count(id))
                pose = {Px(saved->Position), Px(saved->Rotation)};
            b.Actor = b.Settings.Type == RigidBodyType::Static ?
                          static_cast<PxRigidActor*>(Sdk().Physics->createRigidStatic(pose)) :
                          Sdk().Physics->createRigidDynamic(pose);
            if (!b.Actor)
                return;
            b.Actor->userData = reinterpret_cast<void*>(uintptr_t(uint64_t(id)));
            if (auto* dynamic = b.Actor->is<PxRigidDynamic>(); dynamic && b.Settings.Type == RigidBodyType::Kinematic)
                dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            ++Sdk().Actors;
            b.Generation = ++Stats.ActorCreationCount;
            b.Previous   = pose;
            b.Current    = pose;
            std::vector<float> densities;
            for (auto collider : colliders)
            {
                if (collider.HasComponent<BoxColliderComponent>())
                {
                    const auto& c = collider.GetComponent<BoxColliderComponent>();
                    AddShape(collider, b, c, c.Shape, densities);
                }
                if (collider.HasComponent<SphereColliderComponent>())
                {
                    const auto& c = collider.GetComponent<SphereColliderComponent>();
                    AddShape(collider, b, c, c.Shape, densities);
                }
                if (collider.HasComponent<CapsuleColliderComponent>())
                {
                    const auto& c = collider.GetComponent<CapsuleColliderComponent>();
                    AddShape(collider, b, c, c.Shape, densities);
                }
                if (collider.HasComponent<MeshColliderComponent>())
                {
                    const auto&    c     = collider.GetComponent<MeshColliderComponent>();
                    Physics::Shape shape = c.Shape;
                    if (!c.Convex)
                        shape = Physics::TriangleMesh {c.Shape.Mesh, c.Shape.Section, c.Shape.Offset};
                    AddShape(collider, b, c, std::move(shape), densities);
                }
            }
            if (auto* d = b.Actor->is<PxRigidDynamic>())
            {
                bool kinematic = b.Settings.Type == RigidBodyType::Kinematic;
                d->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, kinematic);
                d->setRigidBodyFlag(PxRigidBodyFlag::eUSE_KINEMATIC_TARGET_FOR_SCENE_QUERIES, kinematic);
                d->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD,
                                    !kinematic && b.Settings.ContinuousDetection == ContinuousDetectionMode::CCD);
                d->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD,
                                    b.Settings.ContinuousDetection == ContinuousDetectionMode::Speculative);
                d->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, b.Settings.GravityScale != 1);
                d->setLinearDamping(b.Settings.LinearDamping);
                d->setAngularDamping(b.Settings.AngularDamping);
                d->setSolverIterationCounts(Settings.SolverPositionIterations, Settings.SolverVelocityIterations);
                d->setSleepThreshold(Settings.SleepThreshold);
                PxRigidDynamicLockFlags locks;
                for (unsigned a = 0; a < 3; ++a)
                {
                    if (b.Settings.LockLinear[a])
                        locks |= PxRigidDynamicLockFlag::Enum(1u << a);
                    if (b.Settings.LockAngular[a])
                        locks |= PxRigidDynamicLockFlag::Enum(1u << (a + 3));
                }
                d->setRigidDynamicLockFlags(locks);
                // Open triangle surfaces have no volumetric inertia; kinematic mass is unused.
                if (!densities.empty() && !(kinematic && b.HasTriangleMesh))
                {
                    bool success = PxRigidBodyExt::updateMassAndInertia(*d, densities.data(), PxU32(densities.size()));
                    if (!success)
                        Error(id, "Cannot compute mass/inertia from collider densities");
                    if (!b.Settings.AutoMass)
                    {
                        const float ratio = b.Settings.Mass / d->getMass();
                        d->setMassSpaceInertiaTensor(d->getMassSpaceInertiaTensor() * ratio);
                        d->setMass(b.Settings.Mass);
                    }
                }
                else
                    d->setMass(b.Settings.Mass);
                auto com = d->getCMassLocalPose();
                com.p += Px(b.Settings.CenterOfMassOffset);
                d->setCMassLocalPose(com);
                if (!kinematic)
                {
                    d->setLinearVelocity(
                        Px(saved.has_value() ? saved->LinearVelocity : b.Settings.InitialLinearVelocity));
                    d->setAngularVelocity(
                        Px(saved.has_value() ? saved->AngularVelocity : b.Settings.InitialAngularVelocity));
                }
                World->addActor(*b.Actor);
                if (!kinematic && (saved.has_value() ? saved->Sleeping : !b.Settings.StartAwake))
                {
                    // A replacement actor needs one narrowphase pass even when its predecessor was asleep.
                    // Retain entity pair history, discover the new shape contacts, then restore resting sleep.
                    b.ReconcileSleeping =
                        saved.has_value() &&
                        std::any_of(PreviousContacts.begin(), PreviousContacts.end(), [&](const auto& pair) {
                            return pair.second.BodyA == id || pair.second.BodyB == id;
                        });
                    if (!b.ReconcileSleeping)
                        d->putToSleep();
                }
            }
            else
                World->addActor(*b.Actor);
            Bodies.emplace(id, std::move(b));
            for (auto collider : colliders)
            {
                ColliderLocalMatrices[collider.GetUUID()] = ColliderLocalMatrix(collider, id);
                // Blocking cooking may have loaded the asset: a subsequent ready notification
                // for this same revision must not rebuild a second time.
                if (collider.HasComponent<MeshColliderComponent>())
                    MeshSources[collider.GetUUID()] = SourceStamp(collider);
            }
        }
    };
    PhysicsWorld::PhysicsWorld(Scene& scene, const PhysicsSettings& settings) :
        m_Impl(std::make_unique<Impl>(scene, settings))
    {
        SynchronizeChanges();
    }
    PhysicsWorld::~PhysicsWorld() = default;
    void PhysicsWorld::NotifyTopologyChanged(Entity entity, bool includeSubtree)
    {
        m_Impl->Queue(entity, Impl::Topology, includeSubtree);
    }
    void PhysicsWorld::NotifyBodyOwnershipChanged(Entity entity)
    {
        // A nested rigid body terminates compound ownership, including its descendants.
        m_Impl->Queue(entity, Impl::Topology, true, true);
    }
    void PhysicsWorld::NotifyMeshSourceChanged(Entity entity) { m_Impl->Queue(entity, Impl::MeshSource, false); }
    void PhysicsWorld::NotifyAssetChanged(AssetID asset)
    {
        auto& i = *m_Impl;
        if (auto m = i.Materials.find(asset); m != i.Materials.end())
            i.UpdateMaterial(asset, m->second.Material);
        if (auto users = i.MeshUsers.find(asset); users != i.MeshUsers.end())
            for (auto collider : users->second)
            {
                auto old = i.MeshSources.find(collider);
                if (old == i.MeshSources.end() || !(old->second == i.SourceStamp(i.Owner.GetEntityByUUID(collider))))
                    i.DirtyEntities[collider] |= Impl::Topology;
            }
    }
    void PhysicsWorld::NotifyAssetProviderChanged()
    {
        for (auto& [id, m] : m_Impl->Materials)
            m_Impl->UpdateMaterial(id, m.Material);
        for (const auto& [asset, users] : m_Impl->MeshUsers)
            NotifyAssetChanged(asset);
    }
    void PhysicsWorld::NotifyTransformChanged(Entity e)
    {
        auto& i = *m_Impl;
        if (!e || !i.Owner.m_Registry.valid(entt::entity(e)))
            return;
        if (e.HasComponent<IDComponent>())
        {
            const auto id = e.GetUUID();
            if (i.Bodies.count(id) || i.ColliderOwners.count(id) || i.Joints.count(id) ||
                e.HasComponent<JointComponent>())
                i.ChangedTransforms.insert(id);
            if (e.HasComponent<JointComponent>())
            {
                i.DirtyJoints.insert(id);
                // Keep authored recapture pending while inactive or while a connected actor is absent.
                if (auto joint = i.Joints.find(id); joint != i.Joints.end())
                    joint->second.Captured = false;
            }
        }
        if (e.HasComponent<RelationshipComponent>())
            for (auto child : i.Owner.GetChildren(e))
                NotifyTransformChanged(child);
    }
    void PhysicsWorld::SynchronizeChanges()
    {
        if (m_Impl->Dispatching)
            return;
        std::lock_guard lock(SdkMutex());
        auto&           i = *m_Impl;
        for (auto id : i.RemovedJoints)
            i.RemoveJoint(id);
        i.RemovedJoints.clear();
        if (!i.Initialized)
            i.InitializeIndex();
        auto dirty = std::move(i.DirtyEntities);
        i.DirtyEntities.clear();
        for (const auto& [id, reason] : dirty)
            i.RefreshEntity(id, reason);
        for (auto id : i.ChangedTransforms)
        {
            auto e = i.Owner.GetEntityByUUID(id);
            if (!i.Valid(e) || !i.Owner.IsEntityActive(e))
                continue;
            if (auto actor = i.Bodies.find(id); actor != i.Bodies.end())
            {
                auto& b = actor->second;
                if (auto controller = i.Controllers.find(id); controller != i.Controllers.end())
                {
                    auto w = Physics::TransformShape({}, i.Owner.GetWorldTransform(e));
                    auto p = w.Position + controller->second.Settings.Center;
                    controller->second.Native->setPosition({p.x, p.y, p.z});
                    controller->second.State = {};
                    b.Current                = Pose(w);
                    b.Previous               = Pose(w);
                    b.Scale                  = w.Scale;
                    ++i.Stats.TeleportCount;
                    continue;
                }
                const auto w = Physics::TransformShape({}, i.Owner.GetWorldTransform(e));
                if (glm::length(w.Scale - b.Scale) > 1e-6f)
                    i.DirtyBodies.insert(id);
                if (b.Settings.Type != RigidBodyType::Kinematic)
                {
                    // Authored teleports preserve velocities and reset interpolation history.
                    b.Actor->setGlobalPose(Pose(w));
                    b.Previous = Pose(w);
                    b.Current  = Pose(w);
                    ++i.Stats.TeleportCount;
                }
                else if (auto* d = b.Actor->is<PxRigidDynamic>())
                    d->setKinematicTarget(Pose(w));
            }
            if (auto owner = i.ColliderOwners.find(id); owner != i.ColliderOwners.end() && !i.Controllers.count(id))
            {
                const auto local    = i.ColliderLocalMatrix(e, owner->second);
                auto       previous = i.ColliderLocalMatrices.find(id);
                if (previous == i.ColliderLocalMatrices.end() || !Impl::SameMatrix(local, previous->second))
                    i.DirtyBodies.insert(owner->second);
            }
        }
        auto bodies = std::move(i.DirtyBodies);
        i.DirtyBodies.clear();
        for (auto id : bodies)
            i.RebuildBody(id);
        auto joints = std::move(i.DirtyJoints);
        i.DirtyJoints.clear();
        for (auto id : joints)
            i.RefreshJoint(id);
        i.Stats.ActorCount = i.Bodies.size();
        i.ChangedTransforms.clear();
    }
    void PhysicsWorld::Step(double seconds)
    {
        SynchronizeChanges();
        auto& i = *m_Impl;
        i.StepVelocities.clear();
        for (auto& [key, pair] : i.ContactPairs)
        {
            pair.TotalImpulse = {};
            for (auto& c : pair.Contacts)
                c.Impulse = {};
        }
        i.Fields.Clear();
        for (auto h : i.Owner.m_Registry.view<GlobalWindComponent>())
            if (i.Owner.IsEntityActive({h, &i.Owner}))
                i.Fields.AddGlobalWind(i.Owner.m_Registry.get<GlobalWindComponent>(h));
        for (auto& [id, b] : i.Bodies)
        {
            if (i.Controllers.count(id))
            {
                // CCT sweeps already resolved the motion. Publish the proxy pose before broadphase;
                // trigger processing otherwise observes the preceding kinematic pose for one step.
                auto*       proxy = b.Actor->is<PxRigidDynamic>();
                PxTransform target(PxIdentity);
                if (proxy->getKinematicTarget(target))
                    proxy->setGlobalPose(target);
                continue;
            }
            b.Previous           = b.Current;
            auto* d              = b.Actor->is<PxRigidDynamic>();
            i.StepVelocities[id] = d ? std::make_pair(d->getLinearVelocity(), d->getAngularVelocity()) :
                                       std::make_pair(PxVec3(0), PxVec3(0));
            if (!d)
                continue;
            if (b.Settings.Type == RigidBodyType::Kinematic)
            {
                auto e = i.Owner.GetEntityByUUID(id);
                if (auto target = i.KinematicTargets.find(id); target != i.KinematicTargets.end())
                    d->setKinematicTarget(target->second);
                else if (e)
                {
                    auto authored = Pose(Physics::TransformShape({}, i.Owner.GetWorldTransform(e)));
                    if ((authored.p - b.Authored.p).magnitudeSquared() > 1e-12f ||
                        std::abs(authored.q.dot(b.Authored.q)) < .999999f)
                        d->setKinematicTarget(authored);
                    b.Authored = authored;
                }
                continue;
            }
            if (b.Settings.GravityScale != 1 && b.Settings.GravityScale != 0 && !d->isSleeping())
                d->addForce(Px(i.Settings.Gravity * b.Settings.GravityScale), PxForceMode::eACCELERATION, false);
            if (b.Settings.AffectedByWind)
            {
                auto       relative = i.Fields.SampleWind(Gl(b.Current.p), i.Time) - Gl(d->getLinearVelocity());
                const auto force    = .5f * b.Settings.AirDensity * b.Settings.DragCoefficient * b.Settings.WindArea *
                                      glm::length(relative) * relative;
                if (glm::dot(force, force) > 1e-10f)
                    d->addForce(Px(force));
            }
        }
        for (auto& [id, j] : i.Joints)
            if (j.Native && j.Settings.Type == JointType::Hinge && j.Settings.HingeFreeSpin &&
                j.Settings.HingeMotorEnabled)
                i.UpdateJoint(j);
        i.KinematicTargets.clear();
        i.World->simulate(float(seconds));
        i.World->fetchResults(true);
        for (auto& [id, b] : i.Bodies)
        {
            if (i.Controllers.count(id))
                continue;
            b.Current = b.Actor->getGlobalPose();
            if (b.ReconcileSleeping)
            {
                b.ReconcileSleeping = false;
                auto*      d        = b.Actor->is<PxRigidDynamic>();
                const bool touching = std::any_of(i.ContactPairs.begin(), i.ContactPairs.end(), [&](const auto& pair) {
                    return pair.second.BodyA == id || pair.second.BodyB == id;
                });
                if (d && touching && d->getLinearVelocity().magnitudeSquared() <= 2 * i.Settings.SleepThreshold &&
                    d->getAngularVelocity().magnitudeSquared() <= 2 * i.Settings.SleepThreshold)
                    d->putToSleep();
            }
        }
        i.FollowPlatforms(float(seconds));
        i.CollectJointStates();
        ++i.Stats.StepCount;
        i.Time += seconds;
        i.Dispatching = true;
        try
        {
            i.DispatchPairs(false);
            i.DispatchPairs(true);
            i.DispatchMechanisms();
        }
        catch (...)
        {
            i.Dispatching = false;
            throw;
        }
        i.Dispatching = false;
        auto commands = std::move(i.Commands);
        i.Commands.clear();
        for (auto& command : commands)
            command();
        // Commit callback-authored transforms before render interpolation can overwrite them.
        SynchronizeChanges();
    }
    void PhysicsWorld::WriteTransforms(float alpha)
    {
        auto& i = *m_Impl;
        struct Write
        {
            Entity    E;
            glm::mat4 Matrix;
            size_t    Depth;
        };
        std::vector<Write> writes;
        for (const auto& [id, b] : i.Bodies)
            if (b.Explicit && b.Settings.Type != RigidBodyType::Static)
            {
                auto e = i.Owner.GetEntityByUUID(id);
                if (!e)
                    continue;
                float  t     = b.Settings.Type == RigidBodyType::Dynamic &&
                                       b.Settings.Interpolation == PhysicsInterpolation::Interpolate ?
                                   glm::clamp(alpha, 0.f, 1.f) :
                                   1.f;
                auto   p     = glm::mix(Gl(b.Previous.p), Gl(b.Current.p), t);
                auto   q     = glm::slerp(Gl(b.Previous.q), Gl(b.Current.q), t);
                size_t depth = 0;
                for (auto parent = i.Owner.GetParent(e); parent; parent = i.Owner.GetParent(parent))
                    ++depth;
                writes.push_back(
                    {e,
                     glm::translate(glm::mat4(1), p) * glm::mat4_cast(q) * glm::scale(glm::mat4(1), b.Scale),
                     depth});
            }
        std::stable_sort(writes.begin(), writes.end(), [](const auto& a, const auto& b) { return a.Depth < b.Depth; });
        const bool previous                = i.Owner.m_PhysicsWritingTransforms;
        i.Owner.m_PhysicsWritingTransforms = true;
        for (auto& write : writes)
            i.Owner.SetWorldTransform(write.E, write.Matrix);
        i.Owner.m_PhysicsWritingTransforms = previous;
    }
    std::optional<PhysicsBodyState> PhysicsWorld::GetBodyState(UUID id) const
    {
        auto& i  = *m_Impl;
        auto  it = i.Bodies.find(id);
        if (it == i.Bodies.end())
            return {};
        const auto&      b = it->second;
        PhysicsBodyState s;
        s.Position        = Gl(b.Current.p);
        s.Rotation        = Gl(b.Current.q);
        s.CenterOfMass    = Gl(b.Current.p);
        s.ShapeCount      = b.Actor->getNbShapes();
        s.ActorGeneration = b.Generation;
        if (auto* d = b.Actor->is<PxRigidDynamic>())
        {
            s.LinearVelocity  = Gl(d->getLinearVelocity());
            s.AngularVelocity = Gl(d->getAngularVelocity());
            s.Mass            = d->getMass();
            s.CenterOfMass    = Gl((b.Current * d->getCMassLocalPose()).p);
            s.Sleeping        = d->isSleeping();
        }
        return s;
    }
    const PhysicsStatistics&        PhysicsWorld::GetStatistics() const { return m_Impl->Stats; }
    const PhysicsSettings&          PhysicsWorld::GetSettings() const { return m_Impl->Settings; }
    const ForceFieldSet&            PhysicsWorld::GetForceFields() const { return m_Impl->Fields; }
    const std::vector<std::string>& PhysicsWorld::GetErrors() const { return m_Impl->Errors; }
    void PhysicsWorld::CountDroppedTime(double seconds) { m_Impl->Stats.DroppedTime += seconds; }
    bool PhysicsWorld::Shutdown()
    {
        std::lock_guard lock(SdkMutex());
        if (Sdk().Worlds)
            return false;
        Sdk().Release();
        return true;
    }
    size_t PhysicsWorld::GetLiveWorldCount()
    {
        std::lock_guard lock(SdkMutex());
        return Sdk().Worlds;
    }
    size_t PhysicsWorld::GetLiveActorCount()
    {
        std::lock_guard lock(SdkMutex());
        return Sdk().Actors;
    }
    size_t PhysicsWorld::GetLiveMaterialCount()
    {
        std::lock_guard lock(SdkMutex());
        return Sdk().Physics ? Sdk().Physics->getNbMaterials() : 0;
    }
    uint64_t PhysicsWorld::GetCookCount()
    {
        std::lock_guard lock(SdkMutex());
        return Sdk().Cooks;
    }
} // namespace Kans

namespace Kans
{
    void PhysicsWorld::Draw(DebugDraw& draw) const
    {
        using namespace physx;
        auto& i     = *m_Impl;
        auto  flags = uint32_t(i.Settings.DebugDraw);
        if (!flags)
            return;
        if (flags & 16)
            i.DrawJoints(draw);
        if (flags & 8)
            for (const auto& [key, pair] : i.PreviousContacts)
                for (const auto& c : pair.Contacts)
                {
                    draw.Cross(c.Position, .04f, {1, .2f, .2f, 1});
                    draw.Line(c.Position, c.Position + c.Normal * .25f, {1, 1, 0, 1});
                }
        for (const auto& [id, b] : i.Bodies)
        {
            auto*           dynamic = b.Actor->is<PxRigidDynamic>();
            const glm::vec4 color =
                (flags & 4) && dynamic && dynamic->isSleeping() ? glm::vec4(.4f, .5f, 1, 1) : glm::vec4(.2f, 1, .3f, 1);
            const auto pose = b.Actor->getGlobalPose();
            if ((flags & 2) && dynamic)
                draw.Cross(Gl((pose * dynamic->getCMassLocalPose()).p), .12f, {1, .7f, .1f, 1});
            if (!(flags & 1))
                continue;
            std::vector<PxShape*> shapes(b.Actor->getNbShapes());
            b.Actor->getShapes(shapes.data(), PxU32(shapes.size()));
            for (auto* shape : shapes)
            {
                auto             p      = pose * shape->getLocalPose();
                auto             matrix = glm::translate(glm::mat4(1), Gl(p.p)) * glm::mat4_cast(Gl(p.q));
                PxGeometryHolder geometry(shape->getGeometry());
                switch (geometry.getType())
                {
                    case PxGeometryType::eBOX:
                        draw.Box(matrix, Gl(geometry.box().halfExtents), color);
                        break;
                    case PxGeometryType::eSPHERE:
                        draw.Sphere(Gl(p.p), geometry.sphere().radius, color);
                        break;
                    case PxGeometryType::eCAPSULE: {
                        const auto& g = geometry.capsule();
                        auto        d = p.q.rotate(PxVec3(g.halfHeight, 0, 0));
                        draw.Capsule(Gl(p.p - d), Gl(p.p + d), g.radius, color);
                        break;
                    }
                    case PxGeometryType::eCONVEXMESH: {
                        const auto& g    = geometry.convexMesh();
                        auto*       mesh = g.convexMesh;
                        for (PxU32 polygon = 0; polygon < mesh->getNbPolygons(); ++polygon)
                        {
                            PxHullPolygon poly;
                            mesh->getPolygonData(polygon, poly);
                            const auto* indices = mesh->getIndexBuffer() + poly.mIndexBase;
                            for (PxU32 v = 0; v < poly.mNbVerts; ++v)
                                draw.Line(Gl(p.transform(g.scale.transform(mesh->getVertices()[indices[v]]))),
                                          Gl(p.transform(g.scale.transform(
                                              mesh->getVertices()[indices[(v + 1) % poly.mNbVerts]]))),
                                          color);
                        }
                        break;
                    }
                    case PxGeometryType::eTRIANGLEMESH: {
                        const auto& g    = geometry.triangleMesh();
                        auto*       mesh = g.triangleMesh;
                        for (PxU32 triangle = 0; triangle < mesh->getNbTriangles(); ++triangle)
                        {
                            PxU32 indices[3];
                            for (unsigned v = 0; v < 3; ++v)
                                indices[v] = mesh->getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES ?
                                                 static_cast<const PxU16*>(mesh->getTriangles())[triangle * 3 + v] :
                                                 static_cast<const PxU32*>(mesh->getTriangles())[triangle * 3 + v];
                            for (unsigned v = 0; v < 3; ++v)
                                draw.Line(Gl(p.transform(g.scale.transform(mesh->getVertices()[indices[v]]))),
                                          Gl(p.transform(g.scale.transform(mesh->getVertices()[indices[(v + 1) % 3]]))),
                                          color);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
} // namespace Kans

namespace Kans
{
    namespace
    {
        bool Finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool Direction(glm::vec3& d, float distance)
        {
            if (!Finite(d) || !std::isfinite(distance) || distance <= 0)
                return false;
            double length = std::sqrt(double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z);
            if (length <= 0)
                return false;
            d = glm::vec3(double(d.x) / length, double(d.y) / length, double(d.z) / length);
            return true;
        }
        struct QueryCallback : physx::PxQueryFilterCallback
        {
            QueryFilter Filter;
            bool        HitTriggers, SingleHit;
            uint64_t    IgnoreBody = 0;
            QueryCallback(const QueryFilter& f, bool triggers, uint64_t ignore, bool singleHit = false) :
                Filter(f), HitTriggers(triggers), SingleHit(singleHit), IgnoreBody(ignore)
            {}
            physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData&,
                                                  const physx::PxShape*      shape,
                                                  const physx::PxRigidActor* actor,
                                                  physx::PxHitFlags&) override
            {
                using namespace physx;
                if (!(shape->getQueryFilterData().word0 & Filter.LayerMask))
                    return PxQueryHitType::eNONE;
                if (IgnoreBody && uint64_t(reinterpret_cast<uintptr_t>(actor->userData)) == IgnoreBody)
                    return PxQueryHitType::eNONE;
                if (uint64_t(Filter.IgnoreEntity) &&
                    uint64_t(reinterpret_cast<uintptr_t>(shape->userData)) == uint64_t(Filter.IgnoreEntity))
                    return PxQueryHitType::eNONE;
                bool trigger = shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE;
                if (trigger && !(Filter.Triggers == QueryTriggerInteraction::Collide ||
                                 (Filter.Triggers == QueryTriggerInteraction::UseGlobal && HitTriggers)))
                    return PxQueryHitType::eNONE;
                auto* dynamic = actor->is<PxRigidDynamic>();
                if (!dynamic && !Filter.IncludeStatic)
                    return PxQueryHitType::eNONE;
                if (dynamic && (dynamic->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC ? !Filter.IncludeKinematic :
                                                                                             !Filter.IncludeDynamic))
                    return PxQueryHitType::eNONE;
                return SingleHit ? PxQueryHitType::eBLOCK : PxQueryHitType::eTOUCH;
            }
            physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData&,
                                                   const physx::PxQueryHit&,
                                                   const physx::PxShape*,
                                                   const physx::PxRigidActor*) override
            {
                return SingleHit ? physx::PxQueryHitType::eBLOCK : physx::PxQueryHitType::eTOUCH;
            }
        };
        template<class Hit>
        struct QueryCollector : physx::PxHitCallback<Hit>
        {
            Hit              Buffer[64];
            std::vector<Hit> Hits;
            QueryCollector() : physx::PxHitCallback<Hit>(Buffer, 64) {}
            bool processTouches(const Hit* hits, physx::PxU32 count) override
            {
                Hits.insert(Hits.end(), hits, hits + count);
                return true;
            }
            void finalizeQuery() override
            {
                if (this->nbTouches)
                {
                    processTouches(this->touches, this->nbTouches);
                    this->nbTouches = 0;
                }
            }
        };
        template<class Hit>
        RaycastHit ConvertHit(const Hit& h)
        {
            RaycastHit out;
            out.ColliderEntity = uint64_t(reinterpret_cast<uintptr_t>(h.shape->userData));
            out.BodyEntity     = uint64_t(reinterpret_cast<uintptr_t>(h.actor->userData));
            out.Point          = Gl(h.position);
            out.Normal         = Gl(h.normal);
            out.Distance       = h.distance;
            out.FaceIndex      = h.faceIndex;
            if constexpr (std::is_same_v<Hit, physx::PxSweepHit>)
                out.InitialOverlap = h.hadInitialOverlap();
            return out;
        }
        bool QueryGeometry(const PhysicsQueryShape&   shape,
                           const Physics::WorldShape& pose,
                           physx::PxGeometryHolder&   geometry,
                           physx::PxTransform&        transform)
        {
            if (!Finite(pose.Position) || !Finite(pose.Scale) ||
                glm::any(glm::lessThanEqual(pose.Scale, glm::vec3(0))) || !std::isfinite(pose.Rotation.x) ||
                !std::isfinite(pose.Rotation.y) || !std::isfinite(pose.Rotation.z) || !std::isfinite(pose.Rotation.w) ||
                glm::length(pose.Rotation) < 1e-6f)
                return false;
            bool valid = true;
            std::visit(
                [&](const auto& s) {
                    if (!Finite(s.Offset.Position) || !Finite(s.Offset.Scale) ||
                        glm::any(glm::lessThanEqual(s.Offset.Scale, glm::vec3(0))) ||
                        !Finite({s.Offset.Rotation.x, s.Offset.Rotation.y, s.Offset.Rotation.z}) ||
                        !std::isfinite(s.Offset.Rotation.w) || glm::length(s.Offset.Rotation) < 1e-6f)
                    {
                        valid = false;
                        return;
                    }
                    auto world = Physics::TransformShape(s.Offset,
                                                         glm::translate(glm::mat4(1), pose.Position) *
                                                             glm::mat4_cast(glm::normalize(pose.Rotation)) *
                                                             glm::scale(glm::mat4(1), pose.Scale));
                    transform  = Pose(world);
                    using T    = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<T, Physics::Box>)
                    {
                        auto half = s.HalfExtents * world.Scale;
                        valid     = Finite(half) && glm::all(glm::greaterThan(half, glm::vec3(0)));
                        if (valid)
                            geometry.storeAny(physx::PxBoxGeometry(Px(half)));
                    }
                    else if constexpr (std::is_same_v<T, Physics::Sphere>)
                    {
                        float radius = Physics::ScaledRadius(s, world);
                        valid        = std::isfinite(radius) && radius > 0 && s.Radius > 0;
                        if (valid)
                            geometry.storeAny(physx::PxSphereGeometry(radius));
                    }
                    else
                    {
                        if (unsigned(s.Direction) > 2)
                        {
                            valid = false;
                            return;
                        }
                        glm::vec3 a, b;
                        float     radius;
                        Physics::CapsuleSegment(s, world, a, b, radius);
                        float half = glm::length(b - a) * .5f;
                        valid      = std::isfinite(radius) && radius > 0 && s.Radius > 0 && std::isfinite(half) &&
                                     s.HalfHeight >= 0;
                        glm::vec3 axis(0);
                        axis[unsigned(s.Direction)] = 1;
                        transform.q                 = Px(world.Rotation * glm::rotation(glm::vec3(1, 0, 0), axis));
                        if (valid)
                            geometry.storeAny(physx::PxCapsuleGeometry(radius, half));
                    }
                    valid &= transform.isValid();
                },
                shape);
            return valid;
        }
        void SortHits(std::vector<RaycastHit>& hits)
        {
            std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) {
                if (a.Distance != b.Distance)
                    return a.Distance < b.Distance;
                return uint64_t(a.ColliderEntity) < uint64_t(b.ColliderEntity);
            });
        }
    } // namespace
    std::optional<RaycastHit>
    PhysicsWorld::Raycast(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& filter)
    {
        if (!Finite(origin) || !Direction(direction, distance))
            return {};
        SynchronizeChanges();
        auto&                  i     = *m_Impl;
        auto                   owner = i.ColliderOwners.find(filter.IgnoreEntity);
        QueryCallback          callback(filter,
                                        i.Settings.QueriesHitTriggers,
                                        owner == i.ColliderOwners.end() ? uint64_t(filter.IgnoreEntity) :
                                                                          uint64_t(owner->second),
                                        true);
        physx::PxRaycastBuffer result; // Blocking hit only: no touch buffer or collection allocation.
        i.World->raycast(Px(origin),
                         Px(direction),
                         distance,
                         result,
                         physx::PxHitFlag::eDEFAULT,
                         physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                  physx::PxQueryFlag::ePREFILTER),
                         &callback);
        return result.hasBlock ? std::optional<RaycastHit>(ConvertHit(result.block)) : std::nullopt;
    }
    std::vector<RaycastHit>
    PhysicsWorld::RaycastAll(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& filter)
    {
        std::vector<RaycastHit> hits;
        if (!Finite(origin) || !Direction(direction, distance))
            return hits;
        SynchronizeChanges();
        auto&                               i     = *m_Impl;
        auto                                owner = i.ColliderOwners.find(filter.IgnoreEntity);
        QueryCallback                       callback(filter,
                                                     i.Settings.QueriesHitTriggers,
                                                     owner == i.ColliderOwners.end() ? uint64_t(filter.IgnoreEntity) :
                                                                                       uint64_t(owner->second));
        QueryCollector<physx::PxRaycastHit> collector;
        i.World->raycast(Px(origin),
                         Px(direction),
                         distance,
                         collector,
                         physx::PxHitFlag::eDEFAULT,
                         physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                  physx::PxQueryFlag::ePREFILTER),
                         &callback);
        for (const auto& h : collector.Hits)
            hits.push_back(ConvertHit(h));
        SortHits(hits);
        return hits;
    }
    std::optional<RaycastHit> PhysicsWorld::Cast(const PhysicsQueryShape&   shape,
                                                 const Physics::WorldShape& pose,
                                                 glm::vec3                  direction,
                                                 float                      distance,
                                                 const QueryFilter&         filter)
    {
        physx::PxGeometryHolder geometry;
        physx::PxTransform      transform(physx::PxIdentity);
        if (!Direction(direction, distance) || !QueryGeometry(shape, pose, geometry, transform))
            return {};
        SynchronizeChanges();
        auto&                i     = *m_Impl;
        auto                 owner = i.ColliderOwners.find(filter.IgnoreEntity);
        QueryCallback        callback(filter,
                                      i.Settings.QueriesHitTriggers,
                                      owner == i.ColliderOwners.end() ? uint64_t(filter.IgnoreEntity) :
                                                                        uint64_t(owner->second),
                                      true);
        physx::PxSweepBuffer result;
        i.World->sweep(geometry.any(),
                       transform,
                       Px(direction),
                       distance,
                       result,
                       physx::PxHitFlag::eDEFAULT | physx::PxHitFlag::ePRECISE_SWEEP,
                       physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                physx::PxQueryFlag::ePREFILTER),
                       &callback);
        if (!result.hasBlock)
            return {};
        auto hit = ConvertHit(result.block);
        if (hit.InitialOverlap)
            hit.Point = Gl(transform.p);
        return hit;
    }
    std::vector<RaycastHit> PhysicsWorld::CastAll(const PhysicsQueryShape&   shape,
                                                  const Physics::WorldShape& pose,
                                                  glm::vec3                  direction,
                                                  float                      distance,
                                                  const QueryFilter&         filter)
    {
        std::vector<RaycastHit> hits;
        physx::PxGeometryHolder geometry;
        physx::PxTransform      transform(physx::PxIdentity);
        if (!Direction(direction, distance) || !QueryGeometry(shape, pose, geometry, transform))
            return hits;
        SynchronizeChanges();
        auto&                             i     = *m_Impl;
        auto                              owner = i.ColliderOwners.find(filter.IgnoreEntity);
        QueryCallback                     callback(filter,
                                                   i.Settings.QueriesHitTriggers,
                                                   owner == i.ColliderOwners.end() ? uint64_t(filter.IgnoreEntity) :
                                                                                     uint64_t(owner->second));
        QueryCollector<physx::PxSweepHit> collector;
        i.World->sweep(geometry.any(),
                       transform,
                       Px(direction),
                       distance,
                       collector,
                       physx::PxHitFlag::eDEFAULT | physx::PxHitFlag::ePRECISE_SWEEP,
                       physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                physx::PxQueryFlag::ePREFILTER),
                       &callback);
        for (const auto& h : collector.Hits)
        {
            auto hit = ConvertHit(h);
            if (hit.InitialOverlap)
                hit.Point = Gl(transform.p);
            hits.push_back(hit);
        }
        SortHits(hits);
        return hits;
    }
    std::vector<UUID>
    PhysicsWorld::Overlap(const PhysicsQueryShape& shape, const Physics::WorldShape& pose, const QueryFilter& filter)
    {
        physx::PxGeometryHolder geometry;
        physx::PxTransform      transform(physx::PxIdentity);
        if (!QueryGeometry(shape, pose, geometry, transform))
            return {};
        SynchronizeChanges();
        auto&                               i     = *m_Impl;
        auto                                owner = i.ColliderOwners.find(filter.IgnoreEntity);
        QueryCallback                       callback(filter,
                                                     i.Settings.QueriesHitTriggers,
                                                     owner == i.ColliderOwners.end() ? uint64_t(filter.IgnoreEntity) :
                                                                                       uint64_t(owner->second));
        QueryCollector<physx::PxOverlapHit> collector;
        i.World->overlap(geometry.any(),
                         transform,
                         collector,
                         physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                  physx::PxQueryFlag::ePREFILTER),
                         &callback);
        std::set<UUID> ids;
        for (const auto& h : collector.Hits)
            ids.emplace(uint64_t(reinterpret_cast<uintptr_t>(h.shape->userData)));
        return {ids.begin(), ids.end()};
    }
    PhysicsSubscription PhysicsWorld::OnCollision(std::function<void(const PhysicsPairEvent&)> f)
    {
        auto state = m_Impl->Subscribers;
        auto id    = ++state->Next;
        state->Collision.emplace(id, std::move(f));
        return PhysicsSubscription([weak = std::weak_ptr<Impl::Listeners>(state), id] {
            if (auto s = weak.lock())
                s->Collision.erase(id);
        });
    }
    PhysicsSubscription PhysicsWorld::OnTrigger(std::function<void(const PhysicsPairEvent&)> f)
    {
        auto state = m_Impl->Subscribers;
        auto id    = ++state->Next;
        state->Trigger.emplace(id, std::move(f));
        return PhysicsSubscription([weak = std::weak_ptr<Impl::Listeners>(state), id] {
            if (auto s = weak.lock())
                s->Trigger.erase(id);
        });
    }
    void PhysicsWorld::SetGravity(glm::vec3 v)
    {
        if (!Finite(v))
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, v] { SetGravity(v); });
            return;
        }
        i.Settings.Gravity = v;
        i.World->setGravity(Px(v));
        for (auto& [id, b] : i.Bodies)
            if (auto* d = b.Actor->is<physx::PxRigidDynamic>(); d && b.Settings.Type == RigidBodyType::Dynamic)
                d->wakeUp();
    }
    void PhysicsWorld::IgnoreLayerCollision(uint32_t a, uint32_t b, bool ignore)
    {
        if (a >= 32 || b >= 32)
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, a, b, ignore] { IgnoreLayerCollision(a, b, ignore); });
            return;
        }
        i.Settings.SetLayerCollision(a, b, !ignore);
        for (auto& [id, body] : i.Bodies)
        {
            std::vector<physx::PxShape*> shapes(body.Actor->getNbShapes());
            body.Actor->getShapes(shapes.data(), physx::PxU32(shapes.size()));
            for (auto* shape : shapes)
            {
                auto f = shape->getSimulationFilterData();
                for (unsigned layer = 0; layer < 32; ++layer)
                    if (f.word0 & (1u << layer))
                    {
                        f.word1 = i.Settings.CollisionMatrix[layer];
                        if (auto c = i.Controllers.find(id); c != i.Controllers.end())
                            f.word1 &= c->second.Settings.CollisionMask;
                        break;
                    }
                shape->setSimulationFilterData(f);
                shape->setQueryFilterData(f);
            }
            i.World->resetFiltering(*body.Actor);
            i.InvalidatePairs(id);
        }
    }
    void PhysicsWorld::SetVelocity(UUID id, glm::vec3 v, bool angular)
    {
        if (!Finite(v))
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, v, angular] { SetVelocity(id, v, angular); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Dynamic)
            return;
        auto* d = it->second.Actor->is<physx::PxRigidDynamic>();
        if (angular)
            d->setAngularVelocity(Px(v));
        else
            d->setLinearVelocity(Px(v));
    }
    void PhysicsWorld::AddForce(UUID id, glm::vec3 v, ForceMode mode, bool torque)
    {
        if (!Finite(v) || unsigned(mode) > 3)
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, v, mode, torque] { AddForce(id, v, mode, torque); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Dynamic)
            return;
        auto* d = it->second.Actor->is<physx::PxRigidDynamic>();
        if (torque)
            d->addTorque(Px(v), physx::PxForceMode::Enum(mode));
        else
            d->addForce(Px(v), physx::PxForceMode::Enum(mode));
    }
    void PhysicsWorld::AddForceAtPosition(UUID id, glm::vec3 force, glm::vec3 position, ForceMode mode)
    {
        if (!Finite(force) || !Finite(position) || unsigned(mode) > 3)
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, force, position, mode] { AddForceAtPosition(id, force, position, mode); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Dynamic)
            return;
        auto* d = it->second.Actor->is<physx::PxRigidDynamic>();
        // Acceleration/velocity-change describe COM acceleration; convert to physical force/impulse before computing
        // torque.
        auto f            = Px(force);
        auto physicalMode = physx::PxForceMode::Enum(mode);
        if (mode == ForceMode::Acceleration || mode == ForceMode::VelocityChange)
        {
            f *= d->getMass();
            physicalMode = mode == ForceMode::Acceleration ? physx::PxForceMode::eFORCE : physx::PxForceMode::eIMPULSE;
        }
        physx::PxRigidBodyExt::addForceAtPos(*d, f, Px(position), physicalMode);
    }
    void PhysicsWorld::MovePosition(UUID id, glm::vec3 p)
    {
        if (!Finite(p))
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, p] { MovePosition(id, p); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Kinematic)
            return;
        auto [target, inserted] = i.KinematicTargets.try_emplace(id, it->second.Current);
        target->second.p        = Px(p);
    }
    void PhysicsWorld::MoveRotation(UUID id, glm::quat q)
    {
        if (!std::isfinite(q.w) || !Finite({q.x, q.y, q.z}) || glm::length(q) < 1e-6f)
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, q] { MoveRotation(id, q); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Kinematic)
            return;
        auto [target, inserted] = i.KinematicTargets.try_emplace(id, it->second.Current);
        target->second.q        = Px(glm::normalize(q));
    }
    void PhysicsWorld::SetSleeping(UUID id, bool sleep)
    {
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, sleep] { SetSleeping(id, sleep); });
            return;
        }
        SynchronizeChanges();
        auto it = i.Bodies.find(id);
        if (it == i.Bodies.end() || it->second.Settings.Type != RigidBodyType::Dynamic)
            return;
        auto* d = it->second.Actor->is<physx::PxRigidDynamic>();
        if (sleep)
            d->putToSleep();
        else
            d->wakeUp();
    }
    bool PhysicsWorld::DeferMutation(std::function<void()> command)
    {
        if (!m_Impl->Dispatching)
            return false;
        m_Impl->Commands.push_back(std::move(command));
        return true;
    }
    size_t PhysicsWorld::GetSharedMaterialCount() const { return m_Impl->Materials.size(); }
} // namespace Kans

namespace Kans
{
    CollisionFlags PhysicsWorld::MoveController(UUID id, glm::vec3 displacement, float dt)
    {
        if (!Finite(displacement) || !std::isfinite(dt) || dt <= 0)
            return CollisionFlags::None;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, displacement, dt] { MoveController(id, displacement, dt); });
            return CollisionFlags::None;
        }
        SynchronizeChanges();
        auto flags    = i.MoveCharacter(id, displacement, dt);
        i.Dispatching = true;
        try
        {
            i.DispatchMechanisms();
        }
        catch (...)
        {
            i.Dispatching = false;
            throw;
        }
        i.Dispatching = false;
        auto commands = std::move(i.Commands);
        i.Commands.clear();
        for (auto& command : commands)
            command();
        SynchronizeChanges();
        return flags;
    }
    std::optional<CharacterControllerState> PhysicsWorld::GetControllerState(UUID id) const
    {
        auto c = m_Impl->Controllers.find(id);
        return c == m_Impl->Controllers.end() ? std::nullopt : std::optional<CharacterControllerState>(c->second.State);
    }
    bool PhysicsWorld::CanResizeController(UUID id, float height)
    {
        SynchronizeChanges();
        auto& i = *m_Impl;
        auto  c = i.Controllers.find(id);
        if (c == i.Controllers.end())
            return false;
        physx::PxGeometryHolder g;
        physx::PxTransform      pose(physx::PxIdentity);
        if (!i.ControllerGeometry(c->second, height, g, pose))
            return false;
        Impl::ControllerFilter filter(i, id, c->second.Settings);
        physx::PxOverlapBuffer hit;
        i.World->overlap(g.any(),
                         pose,
                         hit,
                         physx::PxQueryFilterData(physx::PxQueryFlag::eSTATIC | physx::PxQueryFlag::eDYNAMIC |
                                                  physx::PxQueryFlag::ePREFILTER | physx::PxQueryFlag::eANY_HIT),
                         &filter);
        return !hit.hasBlock;
    }
    bool PhysicsWorld::ResizeController(UUID id, float height)
    {
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, height] { ResizeController(id, height); });
            return false;
        }
        if (!CanResizeController(id, height))
            return false;
        auto& c    = i.Controllers.at(id);
        auto  next = c.Settings;
        if (next.Shape == ControllerShape::Capsule)
            next.Height = height;
        else
            next.HalfExtents.y = height * .5f;
        try
        {
            PhysicsYaml::Encode(next);
        }
        catch (...)
        {
            return false;
        }
        c.Native->resize(next.Shape == ControllerShape::Capsule ? height : height * .5f);
        c.Settings = next;
        i.Owner.GetEntityByUUID(id).SetComponent<CharacterControllerComponent>(next);
        i.ControllerPose(id, c);
        SynchronizeChanges();
        return true;
    }
    void PhysicsWorld::TeleportController(UUID id, glm::vec3 position)
    {
        if (!Finite(position))
            return;
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id, position] { TeleportController(id, position); });
            return;
        }
        SynchronizeChanges();
        auto c = i.Controllers.find(id);
        if (c == i.Controllers.end())
            return;
        auto center = position + c->second.Settings.Center;
        c->second.Native->setPosition({center.x, center.y, center.z});
        c->second.State = {};
        i.ControllerPose(id, c->second);
        ++i.Stats.TeleportCount;
    }
    std::optional<JointState> PhysicsWorld::GetJointState(UUID id) const
    {
        auto j = m_Impl->Joints.find(id);
        return j == m_Impl->Joints.end() ? std::nullopt : std::optional<JointState>(j->second.State);
    }
    void PhysicsWorld::ResetJoint(UUID id)
    {
        auto& i = *m_Impl;
        if (i.Dispatching)
        {
            i.Commands.push_back([this, id] { ResetJoint(id); });
            return;
        }
        SynchronizeChanges();
        auto j = i.Joints.find(id);
        if (j == i.Joints.end())
            return;
        i.ReleaseJoint(j->second);
        j->second.State.IsBroken      = false;
        j->second.State.CurrentForce  = {};
        j->second.State.CurrentTorque = {};
        i.DirtyJoints.insert(id);
        SynchronizeChanges();
    }
    void PhysicsWorld::NotifyJointRemoved(UUID id)
    {
        m_Impl->RemovedJoints.insert(id);
        m_Impl->DirtyJoints.insert(id);
    }
    PhysicsSubscription PhysicsWorld::OnControllerHit(std::function<void(UUID, const ControllerColliderHit&)> f)
    {
        auto state = m_Impl->Subscribers;
        auto id    = ++state->Next;
        state->ControllerHit.emplace(id, std::move(f));
        return PhysicsSubscription([weak = std::weak_ptr<Impl::Listeners>(state), id] {
            if (auto s = weak.lock())
                s->ControllerHit.erase(id);
        });
    }
    PhysicsSubscription PhysicsWorld::OnJointBreak(std::function<void(const JointBreakEvent&)> f)
    {
        auto state = m_Impl->Subscribers;
        auto id    = ++state->Next;
        state->JointBreak.emplace(id, std::move(f));
        return PhysicsSubscription([weak = std::weak_ptr<Impl::Listeners>(state), id] {
            if (auto s = weak.lock())
                s->JointBreak.erase(id);
        });
    }
} // namespace Kans
