#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <glm/gtc/quaternion.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/UUID.h"
#include "ForceFields.h"
#include "PhysicsSettings.h"
namespace Kans
{
    class Scene;
    class Entity;
    class DebugDraw;
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
        // Process-wide SDK/cache. Shutdown fails while worlds exist; lazy init can restart it.
        static bool     Shutdown();
        static size_t   GetLiveWorldCount();
        static size_t   GetLiveActorCount();
        static uint64_t GetCookCount();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Kans
