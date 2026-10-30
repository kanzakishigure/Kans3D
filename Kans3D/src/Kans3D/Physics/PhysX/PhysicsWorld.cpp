#include "kspch.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include <PxPhysicsAPI.h>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <cooking/PxCooking.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxRigidBodyExt.h>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/Resource/MeshKey.h"
#include "Kans3D/Scene/Entity.h"

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
                             const void*,
                             PxU32)
        {
            if (!(x.word0 & y.word1) || !(y.word0 & x.word1))
                return PxFilterFlag::eSUPPRESS;
            flags = PxFilterObjectIsTrigger(a) || PxFilterObjectIsTrigger(b) ?
                        PxPairFlags(PxPairFlag::eTRIGGER_DEFAULT) :
                        PxPairFlags(PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT);
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
    struct PhysicsWorld::Impl
    {
        struct Body
        {
            PxRigidActor*      Actor      = nullptr;
            uint64_t           Generation = 0;
            PxTransform        Previous {PxIdentity}, Current {PxIdentity}, Authored {PxIdentity};
            glm::vec3          Scale {1};
            RigidBodyComponent Settings;
            bool               Explicit = false, HasTriangleMesh = false;
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
        Impl(Scene& owner, PhysicsSettings settings) : Owner(owner), Settings(std::move(settings))
        {
            Settings.Validate();
            std::lock_guard lock(SdkMutex());
            auto&           sdk = Sdk();
            sdk.Init();
            Dispatcher = PxDefaultCpuDispatcherCreate(Settings.WorkerThreads);
            PxSceneDesc desc(sdk.Physics->getTolerancesScale());
            desc.gravity       = Px(Settings.Gravity);
            desc.cpuDispatcher = Dispatcher;
            desc.filterShader  = Filter;
            desc.flags |= PxSceneFlag::eENABLE_CCD;
            desc.bounceThresholdVelocity = Settings.BounceThreshold;
            World                        = sdk.Physics->createScene(desc);
            if (!World)
            {
                Dispatcher->release();
                throw std::runtime_error("PhysX scene creation failed");
            }
            ++sdk.Worlds;
        }
        ~Impl()
        {
            std::lock_guard lock(SdkMutex());
            Clear();
            World->release();
            Dispatcher->release();
            --Sdk().Worlds;
        }
        void Clear()
        {
            for (auto& [id, b] : Bodies)
            {
                b.Actor->release();
                --Sdk().Actors;
            }
            Bodies.clear();
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
                if (e.HasComponent<RigidBodyComponent>())
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
            const auto& m = properties.Material.UseDefault ? Settings.DefaultMaterial : properties.Material.Values;
            auto*       material = sdk.Physics->createMaterial(m.StaticFriction, m.DynamicFriction, m.Restitution);
            if (!material)
            {
                Error(entity.GetUUID(), "Invalid collider material");
                return;
            }
            material->setFrictionCombineMode(PxCombineMode::Enum(m.FrictionCombine));
            material->setRestitutionCombineMode(PxCombineMode::Enum(m.RestitutionCombine));
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
            shape->setLocalPose(body.Authored.getInverse() * worldPose);
            uint32_t layer = properties.Layer.value_or(body.Settings.Layer);
            if (layer >= 32)
            {
                shape->release();
                Error(entity.GetUUID(), "Physics layer must be 0..31");
                return;
            }
            PxFilterData filter(1u << layer, Settings.CollisionMatrix[layer], 0, 0);
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
                if (Bodies.count(id) || ColliderOwners.count(id) || HasCollider(e) ||
                    e.HasComponent<RigidBodyComponent>())
                    DirtyEntities[id] |= reason;
            }
            if (subtree && e.HasComponent<RelationshipComponent>())
                for (auto child : Owner.GetChildren(e))
                    if (!stopAtNestedBodies || !child.HasComponent<RigidBodyComponent>())
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
            if (!HasCollider(e))
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
            // The only whole-registry scan: initial world construction (settings are immutable during play).
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
                b.Actor->release();
                --Sdk().Actors;
                Bodies.erase(old);
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
                    d->putToSleep();
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
            if (i.Bodies.count(id) || i.ColliderOwners.count(id))
                i.ChangedTransforms.insert(id);
        }
        if (e.HasComponent<RelationshipComponent>())
            for (auto child : i.Owner.GetChildren(e))
                NotifyTransformChanged(child);
    }
    void PhysicsWorld::SynchronizeChanges()
    {
        std::lock_guard lock(SdkMutex());
        auto&           i = *m_Impl;
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
                auto&      b = actor->second;
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
            }
            if (auto owner = i.ColliderOwners.find(id); owner != i.ColliderOwners.end())
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
        i.Stats.ActorCount = i.Bodies.size();
        i.ChangedTransforms.clear();
    }
    void PhysicsWorld::Step(double seconds)
    {
        SynchronizeChanges();
        auto& i = *m_Impl;
        i.Fields.Clear();
        for (auto h : i.Owner.m_Registry.view<GlobalWindComponent>())
            if (i.Owner.IsEntityActive({h, &i.Owner}))
                i.Fields.AddGlobalWind(i.Owner.m_Registry.get<GlobalWindComponent>(h));
        for (auto& [id, b] : i.Bodies)
        {
            b.Previous = b.Current;
            auto* d    = b.Actor->is<PxRigidDynamic>();
            if (!d)
                continue;
            if (b.Settings.Type == RigidBodyType::Kinematic)
            {
                auto e = i.Owner.GetEntityByUUID(id);
                if (e)
                    d->setKinematicTarget(Pose(Physics::TransformShape({}, i.Owner.GetWorldTransform(e))));
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
        i.World->simulate(float(seconds));
        i.World->fetchResults(true);
        for (auto& [id, b] : i.Bodies)
            b.Current = b.Actor->getGlobalPose();
        ++i.Stats.StepCount;
        i.Time += seconds;
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
            if (b.Explicit && b.Settings.Type == RigidBodyType::Dynamic)
            {
                auto e = i.Owner.GetEntityByUUID(id);
                if (!e)
                    continue;
                float t =
                    b.Settings.Interpolation == PhysicsInterpolation::Interpolate ? glm::clamp(alpha, 0.f, 1.f) : 1.f;
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
        s.ShapeCount      = b.Actor->getNbShapes();
        s.ActorGeneration = b.Generation;
        if (auto* d = b.Actor->is<PxRigidDynamic>())
        {
            s.LinearVelocity  = Gl(d->getLinearVelocity());
            s.AngularVelocity = Gl(d->getAngularVelocity());
            s.Mass            = d->getMass();
            s.CenterOfMass    = Gl(d->getCMassLocalPose().p);
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
