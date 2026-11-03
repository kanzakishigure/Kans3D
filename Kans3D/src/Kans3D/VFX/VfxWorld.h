#pragma once
#include <memory>
#include "Kans3D/Core/UUID.h"
#include "ParticleSystemComponent.h"
#include "VfxSystemAsset.h"
namespace Kans
{
    class Scene;
    class Entity;
} // namespace Kans
namespace Kans::Vfx
{
    class GpuEmitter;
    struct GpuTick
    {
        ParameterMap     Parameters;
        ExecutionContext Context;
        uint32_t         SpawnCount = 0;
    };
    struct EmitterInstance
    {
        explicit EmitterInstance(const CompiledEmitter&);
        void Reset(); // Retains attribute storage and GPU workspaces.
        ~EmitterInstance();
        EmitterInstance(EmitterInstance&&) noexcept;
        EmitterInstance&                                 operator=(EmitterInstance&&) noexcept;
        const CompiledEmitter*                           Compiled;
        ParameterMap                                     Parameters;
        std::map<std::string, std::vector<ParameterMap>> Events;
        uint64_t                                         EventsDropped = 0;
        std::vector<EmitterInstance>*                    Siblings      = nullptr;
        std::vector<std::vector<Value>>                  Attributes;
        std::vector<uint32_t>                            Alive, Free;
        std::vector<GpuTick>                             Pending;
        std::unique_ptr<GpuEmitter>                      GPU;
        uint32_t                                         NextID = 0, PendingBurst = 0, ParticleCount = 0;
        uint64_t                                         Dropped = 0, TotalSpawned = 0;
        uint64_t                                         SpawnRevision = 0, ReadbackSpawnRevision = 0;
        bool                                             HasReadback = false;
        Bounds                                           DynamicBounds;
        float                                            SpriteRadius     = 0;
        bool                                             HiddenByDistance = false;
        // CPU time of this emitter's part of the last tick (spawn, update, handlers, GPU queuing).
        double                                           TickMilliseconds = 0;
        Value                                            Read(const std::string& name, uint32_t slot) const;
        glm::vec3                                        WorldPosition(uint32_t slot, const glm::mat4& owner) const;
    };
    struct MeshSamplingSource
    {
        Ref<const MeshAsset>     Mesh;
        Ref<const SkeletonAsset> Skeleton;
        glm::mat4                Transform {1};
        std::vector<glm::mat4>   Palette;
        bool                     Valid = false;
    };
    using MeshSourceResolver = std::function<MeshSamplingSource(const InterfaceBinding&)>;
    class SystemInstance
    {
    public:
        SystemInstance(Ref<const VfxSystemAsset>,
                       const ParticleSystemComponent&,
                       const glm::mat4&   initialOwner = glm::mat4(1),
                       MeshSourceResolver meshSources  = {});
        ~SystemInstance();
        SystemInstance(const SystemInstance&)            = delete;
        SystemInstance& operator=(const SystemInstance&) = delete;
        SystemInstance(SystemInstance&&)                 = delete;
        SystemInstance& operator=(SystemInstance&&)      = delete;
        void            Activate(bool reset = true);
        void            Deactivate() { m_Spawning = false; }
        void            DeactivateImmediate();
        void            Pause() { m_Paused = true; }
        void            Resume() { m_Paused = false; }
        bool            IsActive() const { return m_Spawning && !m_Paused && !IsComplete(); }
        bool            IsComplete() const;
        bool            IsSpawning() const { return m_Spawning; } // Activated and not deactivated.
        void            Emit(const std::string& emitter, uint32_t count);
        void            Set(const std::string& name, const Value&);
        Value           Get(const std::string& name, Type) const;
        uint32_t        ParticleCount() const;
        void            Tick(float delta, const glm::mat4& owner = glm::mat4(1), glm::vec3 camera = {});
        void            SeekAge(double                             age,
                                float                              fixedTick,
                                double                             maxSimulationTime,
                                const glm::mat4&                   owner      = glm::mat4(1),
                                bool                               reset      = false,
                                const std::function<void(double)>& beforeTick = {});
        glm::vec4                               MainLightDirection {0};
        std::shared_ptr<CollisionSnapshot>      Colliders;
        std::shared_ptr<CollisionDepthSnapshot> Depth;
        PhysicsWorld*                           Physics       = nullptr;
        uint32_t                                CollisionMask = UINT32_MAX;
        std::shared_ptr<FieldSnapshot>          Fields;
        uint32_t                                FieldLayer = 0;
        double                                  FieldTime  = 0;
        double                                  Age() const { return m_Time; }
        bool                                    TimelineDesiredAge = false; // Timeline owns stepping until released.
        const Ref<const VfxSystemAsset>&        Asset() const { return m_Asset; }
        std::vector<EmitterInstance>&           Emitters() { return m_Emitters; }
        const std::vector<EmitterInstance>&     Emitters() const { return m_Emitters; }
        const glm::mat4&                        OwnerTransform() const { return m_Owner; }
        std::function<void(const std::string&)> RuntimeWarning;
        std::optional<Bounds>                   FixedBounds;
        float                                   TimeScale   = 1;
        bool                                    AutoDestroy = false;
        bool                                    Prewarm     = true; // Run the system's warmup on activation.

    private:
        struct MeshState
        {
            Ref<const MeshAsset>     Mesh;
            Ref<const SkeletonAsset> Skeleton;
            InterfaceBinding         Baked;
            glm::mat4                PreviousTransform {1};
            std::vector<glm::mat4>   PreviousPalette;
            bool                     Initialized = false, Cached = false, Warned = false;
        };
        MeshSourceResolver               m_MeshSources;
        std::map<std::string, MeshState> m_MeshStates;
        void                         PrepareMeshTables(ExecutionContext&, float, const glm::mat4&, const glm::mat4&);
        void                         Reset();
        void                         Step(float, const glm::mat4&, glm::vec3);
        Ref<const VfxSystemAsset>    m_Asset;
        std::vector<EmitterInstance> m_Emitters;
        ParameterMap                 m_Parameters;
        uint32_t                     m_Seed, m_Entropy = 0;
        uint64_t                         m_LastImpulse = 0;
        double                           m_Time        = 0;
        bool                         m_Spawning = false, m_Paused = false;
        bool                             m_ScrubCapWarned = false;
        glm::mat4                    m_Owner {1};
    };
    // The overrides of `parameters` that the system declares with the same type; the names of the
    // others (unknown, renamed or retyped) with the reason go to `dropped`.
    ParameterMap MatchUserParameters(const CompiledSystem&,
                                     const ParameterMap&       parameters,
                                     std::vector<std::string>* dropped = nullptr);
    class VfxWorld
    {
    public:
        explicit VfxWorld(Scene& scene) : m_Scene(scene) {}
        glm::vec4                                              MainLightDirection {0};
        void                                    Tick(float delta, bool preview = false, glm::vec3 camera = {});
        std::shared_ptr<CollisionDepthSnapshot> Depth;
        std::function<void(const std::string&)> RuntimeWarning;
        bool                                    Bind(Entity, Ref<const VfxSystemAsset>);
        SystemInstance*                         Find(UUID);
        const std::map<UUID, std::unique_ptr<SystemInstance>>& Instances() const { return m_Instances; }
        void                                                   Remove(UUID);
        void                                                   Clear();
        void                                                   Reset(UUID);
        void                                                   NotifyAssetChanged(AssetID);
        void                                                   Start();
        bool IsPlaying() const { return m_Playing; } // Started for play mode (not the edit-mode preview).
        Entity                                                 Spawn(AssetID, glm::vec3, glm::quat, bool);

    private:
        void                                            GatherColliders(CollisionSnapshot&, float delta);
        std::map<std::pair<UUID, uint32_t>, glm::vec3>  m_PreviousColliderCenters;
        std::shared_ptr<CollisionGpuStorage>            m_CollisionGPU;
        bool                                            m_CollisionCapWarned = false;
        std::shared_ptr<FieldGpuStorage>                m_FieldGPU;
        double                                          m_FieldTime = 0;
        Scene&                                          m_Scene;
        std::map<UUID, std::unique_ptr<SystemInstance>> m_Instances;
        std::map<UUID, ParticleSystemComponent>         m_Configurations;
        std::map<UUID, Ref<const VfxSystemAsset>>       m_Overrides;
        std::set<UUID>                                  m_AutoDestroy;
        bool                                            m_Playing = false;
    };
} // namespace Kans::Vfx
