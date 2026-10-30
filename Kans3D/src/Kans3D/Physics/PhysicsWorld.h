#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/UUID.h"
#include "ForceFields.h"
#include "PhysicsSettings.h"
#include "Shapes.h"
namespace Kans
{
    class Scene;
    class Entity;
    class DebugDraw;
    enum class QueryTriggerInteraction : uint32_t
    {
        UseGlobal,
        Ignore,
        Collide
    };
    struct QueryFilter
    {
        uint32_t                LayerMask     = UINT32_MAX;
        QueryTriggerInteraction Triggers      = QueryTriggerInteraction::UseGlobal;
        UUID                    IgnoreEntity  = 0; // The entity and every collider belonging to its body are excluded.
        bool                    IncludeStatic = true, IncludeDynamic = true, IncludeKinematic = true;
    };
    struct RaycastHit
    {
        UUID      ColliderEntity = 0, BodyEntity = 0;
        glm::vec3 Point {0}, Normal {0};
        float     Distance       = 0;
        uint32_t  FaceIndex      = UINT32_MAX;
        bool      InitialOverlap = false;
    };
    enum class PhysicsEventPhase : uint32_t
    {
        Enter,
        Stay,
        Exit
    };
    struct PhysicsContactPoint
    {
        glm::vec3 Position {0}, Normal {0}, Impulse {0};
        float     Separation = 0;
        UUID      ColliderA = 0, ColliderB = 0;
    };
    struct PhysicsPairEvent
    {
        PhysicsEventPhase Phase = PhysicsEventPhase::Enter;
        UUID              BodyA = 0, BodyB = 0, ColliderA = 0, ColliderB = 0;
        // All touching collider pairs, even when contact storage has reached its cap.
        std::vector<std::pair<UUID, UUID>> Colliders;
        std::vector<PhysicsContactPoint>   Contacts;
        glm::vec3 TotalImpulse {0}, RelativeVelocity {0}; // A relative to B; normals point toward A.
        bool      Removed =
            false; // Exit with a destroyed/deactivated body or removed collider; IDs remain valid identities.
    };
    // Disconnects on destruction, and remains safe after the world has been destroyed.
    class PhysicsSubscription
    {
    public:
        PhysicsSubscription() = default;
        explicit PhysicsSubscription(std::function<void()> disconnect) : Disconnect(std::move(disconnect)) {}
        PhysicsSubscription(const PhysicsSubscription&)            = delete;
        PhysicsSubscription& operator=(const PhysicsSubscription&) = delete;
        PhysicsSubscription(PhysicsSubscription&& other) noexcept : Disconnect(std::move(other.Disconnect))
        {
            other.Disconnect = {};
        }
        PhysicsSubscription& operator=(PhysicsSubscription&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                Disconnect       = std::move(other.Disconnect);
                other.Disconnect = {};
            }
            return *this;
        }
        ~PhysicsSubscription() { Reset(); }
        void Reset()
        {
            if (Disconnect)
            {
                auto f     = std::move(Disconnect);
                Disconnect = {};
                f();
            }
        }

    private:
        std::function<void()> Disconnect;
    };
    enum class ForceMode : uint32_t
    {
        Force,
        Impulse,
        VelocityChange,
        Acceleration
    };
    using PhysicsQueryShape = std::variant<Physics::Sphere, Physics::Capsule, Physics::Box>;
    struct PhysicsBodyState
    {
        glm::vec3 Position {0}, LinearVelocity {0}, AngularVelocity {0}, CenterOfMass {0};
        glm::quat Rotation {1, 0, 0, 0};
        float     Mass       = 0;
        bool      Sleeping   = false;
        uint32_t  ShapeCount = 0;
        // Changes only when this body's SDK actor is created/replaced; never exposes an SDK pointer.
        uint64_t ActorGeneration = 0;
    };
    struct PhysicsStatistics
    {
        uint64_t StepCount = 0, TeleportCount = 0, RebuildCount = 0;
        uint64_t ActorCreationCount = 0, FullRebuildCount = 0, TopologyEntitiesVisited = 0;
        size_t   ActorCount  = 0;
        double   DroppedTime = 0;
    };
    class PhysicsWorld
    {
    public:
        PhysicsWorld(Scene&, const PhysicsSettings&);
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&)                              = delete;
        PhysicsWorld&                   operator=(const PhysicsWorld&) = delete;
        void                            NotifyTopologyChanged(Entity, bool includeSubtree = false);
        void                            NotifyBodyOwnershipChanged(Entity);
        void                            NotifyMeshSourceChanged(Entity);
        void                            NotifyAssetChanged(AssetID);
        void                            NotifyAssetProviderChanged();
        void                            NotifyTransformChanged(Entity);
        void                            SynchronizeChanges();
        void                            Step(double seconds);
        void                            WriteTransforms(float alpha);
        void                            Draw(DebugDraw&) const;
        std::optional<PhysicsBodyState> GetBodyState(UUID) const;
        const PhysicsStatistics&        GetStatistics() const;
        const PhysicsSettings&          GetSettings() const;
        const ForceFieldSet&            GetForceFields() const;
        const std::vector<std::string>& GetErrors() const;
        void                            CountDroppedTime(double seconds);
        // Singles return the nearest All distance; exact-distance ties may be resolved differently by PhysX.
        std::optional<RaycastHit>
        Raycast(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& = {});
        std::vector<RaycastHit>
        RaycastAll(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& = {});
        std::optional<RaycastHit> Cast(const PhysicsQueryShape&,
                                       const Physics::WorldShape&,
                                       glm::vec3 direction,
                                       float     distance,
                                       const QueryFilter& = {});
        std::vector<RaycastHit>   CastAll(const PhysicsQueryShape&,
                                          const Physics::WorldShape&,
                                          glm::vec3 direction,
                                          float     distance,
                                          const QueryFilter& = {});
        std::vector<UUID> Overlap(const PhysicsQueryShape&, const Physics::WorldShape&, const QueryFilter& = {});
#define KANS_QUERY_API(Name) \
    std::optional<RaycastHit> Name##Cast(const Physics::Name&       shape, \
                                         const Physics::WorldShape& pose, \
                                         glm::vec3                  direction, \
                                         float                      distance, \
                                         const QueryFilter&         filter = {}) \
    { \
        return Cast(shape, pose, direction, distance, filter); \
    } \
    std::vector<RaycastHit> Name##CastAll(const Physics::Name&       shape, \
                                          const Physics::WorldShape& pose, \
                                          glm::vec3                  direction, \
                                          float                      distance, \
                                          const QueryFilter&         filter = {}) \
    { \
        return CastAll(shape, pose, direction, distance, filter); \
    } \
    std::vector<UUID> Overlap##Name( \
        const Physics::Name& shape, const Physics::WorldShape& pose, const QueryFilter& filter = {}) \
    { \
        return Overlap(shape, pose, filter); \
    }
        KANS_QUERY_API(Sphere)
        KANS_QUERY_API(Capsule)
        KANS_QUERY_API(Box)
#undef KANS_QUERY_API
        PhysicsSubscription OnCollision(std::function<void(const PhysicsPairEvent&)>);
        PhysicsSubscription OnTrigger(std::function<void(const PhysicsPairEvent&)>);
        void                SetGravity(glm::vec3);
        void                IgnoreLayerCollision(uint32_t a, uint32_t b, bool ignore);
        void                SetVelocity(UUID, glm::vec3, bool angular = false);
        void                AddForce(UUID, glm::vec3, ForceMode = ForceMode::Force, bool torque = false);
        void                AddForceAtPosition(UUID, glm::vec3 force, glm::vec3 position, ForceMode = ForceMode::Force);
        void                MovePosition(UUID, glm::vec3);
        void                MoveRotation(UUID, glm::quat);
        void                SetSleeping(UUID, bool);
        size_t              GetSharedMaterialCount() const;
        bool                DeferMutation(std::function<void()>);
        // Process-wide SDK/cache. Shutdown fails while worlds exist; lazy init can restart it.
        static bool     Shutdown();
        static size_t   GetLiveWorldCount();
        static size_t   GetLiveActorCount();
        static size_t   GetLiveMaterialCount();
        static uint64_t GetCookCount();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Kans
