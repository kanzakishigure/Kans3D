#pragma once
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>
#include <entt.hpp>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Core/Math/Bounds.h"
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/Core/UUID.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Renderer/Resource/PostProcessSettings.h"
#include "Kans3D/UI/UIContext.h"
#include "EnvironmentSettings.h"

namespace Kans
{
    class AssetManager;
    class AssetChangeObserver;
    class RenderWorld;
    class MeshAsset;
    struct RuntimeMesh;
    struct EditableMesh;
    struct TransformComponent;
    struct PrefabNodeOverride;
    struct Bounds;
    struct MeshMaterialSlot;
    class Entity;
    class PhysicsWorld;
    struct SpringBoneJointState;
    struct SpringBoneUpdateCache;
    struct ClothRuntime;
    struct ClothUpdateCache;
    class DebugDraw;
    struct PhysicsSettings;
    struct SceneSnapshot;

    using EntityMap = std::unordered_map<UUID, Entity>;

    // How a runtime session runs the scene. Without scripts (an editor's physics-only
    // simulation), C# and native scripts are neither started nor updated; physics,
    // animation, timelines and audio run as in play.
    struct SceneRuntimeOptions
    {
        bool RunScripts = true;
    };

    // the scene to the GamePlayer system
    class Scene : public Asset
    {

    public:
        const PostProcessSettings& GetPostProcessSettings() const { return m_PostProcessSettings; }
        void SetPostProcessSettings(PostProcessSettings settings) { m_PostProcessSettings = settings.Validated(); }
        Scene(const std::string& name = "UntitledScene");
        ~Scene();

        void              OnUpdate(TimeStep ts);
        UIContext&        UI() { return m_UI; }
        const UIContext&  UI() const { return m_UI; }
        const UIViewport& GetUIViewport() const { return m_UIViewport; }
        void              UpdateUI(const UIInputState&, const UIViewport&, float seconds);

        void       OnViewportResize(uint32_t width, uint32_t height);
        glm::uvec2 GetViewportSize() const { return {m_ViewportWidth, m_ViewportHeight}; }

        Entity CreateEntity(const std::string name = std::string());
        Entity CreateEntityWithID(UUID uuid, const std::string& name, bool runtimeMap);
        void   DestroyEntity(Entity entity);
        Entity GetEntityByUUID(UUID uuid) const;

        // ---- hierarchy ----
        // Top-level entities in order.
        std::vector<Entity> GetRootEntities() const;
        std::vector<Entity> GetChildren(Entity entity) const;
        Entity              GetParent(Entity entity) const;
        // True when `ancestor` is a (transitive) parent of `entity`.
        bool IsDescendantOf(Entity entity, Entity ancestor) const;
        // Moves `entity` under `parent` (a null Entity: top level), before the
        // sibling `before` (null or not a sibling: last). With `keepWorld` the
        // local transform changes so the entity stays where it is. Returns false
        // when `parent` is the entity itself or one of its descendants.
        bool SetParent(Entity entity, Entity parent, Entity before, bool keepWorld);
        // Appends `entity` to `parent`'s children, keeping its world transform.
        bool      SetParent(Entity entity, Entity parent);
        glm::mat4 GetWorldTransform(Entity entity) const;
        // Sets the local transform so the entity ends up at `world`.
        void SetWorldTransform(Entity entity, const glm::mat4& world);
        // Copies the entity with its children (new IDs), placed right after it.
        Entity DuplicateEntity(Entity entity);

        // Nonblocking; also called by rendering and runtime updates.
        void                 SetPrefabAssetManager(AssetManager* assets);
        RenderWorld&         GetRenderWorld() const { return *m_RenderWorld; }
        std::vector<AssetID> GetReferencedAssets() const;
        // Resolve CPU metadata, retaining prefab data while replacement assets load.
        Ref<const MeshAsset>   ResolveMesh(Entity entity) const;
        Ref<const RuntimeMesh> ResolveEditableMesh(Entity entity) const;
        uint64_t               GetEditableMeshBuildCount() const { return m_EditableMeshBuildCount; }
        void                   UpdatePrefabs();
        // Nonblocking asset preparation. Pose drives prefab skeleton entities.
        bool                           IsEntityActive(Entity entity) const;
        void                           SetEntityActive(Entity entity, bool active);
        bool                           PrepareTimeline(Entity owner);
        void                           UpdateTimelines(double seconds);
        AssetManager*                  GetAssetManager() const { return m_PrefabAssets; }
        bool                           PrepareAnimator(Entity root);
        void                           UpdateAnimations(double seconds);
        void                           UpdateModelPoses();
        Ref<const struct SkinningPose> GetSkinningPose(Entity entity) const;
        Entity                         GetDeformationRoot(Entity entity) const;
        void                           UpdateMaterialParameterDrivers();
        void                           UpdateMaterialParameterDrivers(Entity entity);
        bool                           PreviewAnimation(Entity root, size_t layer, uint32_t state, double time);
        void                           ApplyAnimatorPose(Entity root, const struct AnimationPose& pose);
        void                           ApplyModelPose(Entity root,
                                                      const struct SkeletonData&,
                                                      const struct AnimationPose&,
                                                      const std::vector<UUID>& joints,
                                                      Ref<struct SkinningPose>&,
                                                      uint64_t& version,
                                                      bool      writeTransforms = true);
        void                           ResetAnimator(Entity root);
        void                           UpdateSpringBones(double seconds);
        void                           UpdateCloth(double seconds);
        bool                           BindCloth(Entity, Ref<const class ClothAsset>, Ref<const MeshAsset> mesh = {});
        // Binds `asset` in place of the component's stored asset until cleared (null), e.g. unsaved
        // painted maps; the asset must carry the component's AssetID. False when it does not bind.
        bool SetClothSourceOverride(Entity, Ref<const class ClothAsset>);
        void                SetSecondaryMotionPreviewEnabled(bool enabled) { SetSpringBonePreviewEnabled(enabled); }
        bool                IsSecondaryMotionPreviewEnabled() const { return IsSpringBonePreviewEnabled(); }
        const ClothRuntime* GetClothState(Entity) const;
        void                           ResetSecondaryMotion(Entity scope);
        void                        ResetSecondaryMotion(); // Shared discontinuity hook for secondary-motion solvers.
        void                        SetSpringBonePreviewEnabled(bool);
        bool                        IsSpringBonePreviewEnabled() const { return m_SpringBonePreview; }
        const SpringBoneJointState* GetSpringBoneJointState(Entity, size_t chain, size_t joint) const;
        double                      GetSpringBoneInterpolationAlpha(Entity) const;
        uint64_t                    GetSpringBoneSubstepCount(Entity) const;
        glm::mat4                   GetSpringBoneRenderTransform(Entity) const;
        void                        UpdatePrefabs(AssetManager& assets)
        {
            SetPrefabAssetManager(&assets);
            UpdatePrefabs();
        }
        Entity              InstantiatePrefab(AssetID prefab, const glm::vec3& position = {}, UUID parent = 0);
        Entity              GetPrefabInstanceRoot(Entity entity) const;
        static UUID         PrefabEntityUUID(UUID instance, const std::string& nodeKey);
        std::vector<Entity> GetMeshEntities(Entity subtree) const;
        std::vector<Entity> GetMeshEntities() const;
        void                CapturePrefabOverrides();
        void   SetPrefabTransformOverride(Entity root, const std::string& key, const TransformComponent& transform);
        void   SetPrefabMaterialOverride(Entity root, const std::string& key, uint32_t slot, AssetID material);
        void   SetPrefabNodeRemoved(Entity root, const std::string& key, bool removed);
        void   ClearPrefabOverride(Entity root, const std::string& key);
        Bounds GetWorldBounds(Entity entity) const;
        // CPU-only pre-placement bounds; empty until the prefab/meshes are ready.
        Bounds                        GetPrefabBounds(AssetID prefab, const glm::mat4& transform = glm::mat4(1));
        std::vector<MeshMaterialSlot> GetMeshSlots(Entity entity) const;
        void                          UpdateBounds();

        std::optional<RaycastHit>
        Raycast(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& f = {})
        {
            return m_PhysicsWorld ? m_PhysicsWorld->Raycast(origin, direction, distance, f) : std::nullopt;
        }
        std::vector<RaycastHit>
        RaycastAll(glm::vec3 origin, glm::vec3 direction, float distance, const QueryFilter& f = {})
        {
            return m_PhysicsWorld ? m_PhysicsWorld->RaycastAll(origin, direction, distance, f) :
                                    std::vector<RaycastHit> {};
        }
#define KANS_SCENE_QUERY(Name) \
    std::optional<RaycastHit> Name##Cast(const Physics::Name&       shape, \
                                         const Physics::WorldShape& pose, \
                                         glm::vec3                  direction, \
                                         float                      distance, \
                                         const QueryFilter&         f = {}) \
    { \
        return m_PhysicsWorld ? m_PhysicsWorld->Name##Cast(shape, pose, direction, distance, f) : std::nullopt; \
    } \
    std::vector<RaycastHit> Name##CastAll(const Physics::Name&       shape, \
                                          const Physics::WorldShape& pose, \
                                          glm::vec3                  direction, \
                                          float                      distance, \
                                          const QueryFilter&         f = {}) \
    { \
        return m_PhysicsWorld ? m_PhysicsWorld->Name##CastAll(shape, pose, direction, distance, f) : \
                                std::vector<RaycastHit> {}; \
    } \
    std::vector<UUID> Overlap##Name( \
        const Physics::Name& shape, const Physics::WorldShape& pose, const QueryFilter& f = {}) \
    { \
        return m_PhysicsWorld ? m_PhysicsWorld->Overlap##Name(shape, pose, f) : std::vector<UUID> {}; \
    }
        KANS_SCENE_QUERY(Sphere)
        KANS_SCENE_QUERY(Capsule)
        KANS_SCENE_QUERY(Box)
#undef KANS_SCENE_QUERY
        const EnvironmentSettings& GetEnvironmentSettings() const { return m_EnvironmentSettings; }
        bool                       HasEnvironmentSettings() const { return m_HasEnvironmentSettings; }
        // Validates and immediately notifies the render world, including map removal.
        void SetEnvironmentSettings(EnvironmentSettings settings);
        template<typename Fn>
        void PatchEnvironmentSettings(Fn&& patch)
        {
            auto settings = m_EnvironmentSettings;
            patch(settings);
            if (settings != m_EnvironmentSettings)
                SetEnvironmentSettings(std::move(settings));
        }
        // Restore legacy inheritance from the view/editor, including default scales.
        void ClearEnvironmentSettings();

        // ---- undo/redo (SceneSnapshot.h) ----
        SceneSnapshot Capture() const;
        // Replaces every entity with the snapshot's; UUIDs are preserved.
        void Restore(const SceneSnapshot& snapshot);

        void OnRuntimeStart(const SceneRuntimeOptions& options = {});
        // The options of the running session (defaults outside one).
        const SceneRuntimeOptions& GetRuntimeOptions() const { return m_RuntimeOptions; }
        void                   OnRuntimeSingleStep();
        PhysicsWorld*          GetPhysicsWorld() const { return m_PhysicsWorld.get(); }
        const PhysicsSettings& GetPhysicsSettings() const;
        double                 GetPhysicsInterpolationAlpha() const;
        DebugDraw&             GetDebugDraw() const;
        // Debug drawing a viewer adds to the project's PhysicsSettings::DebugDraw while the scene
        // runs (e.g. an editor viewport's toggles); it is not saved with the scene or the project.
        void                   SetPhysicsDebugView(PhysicsDebugFlags flags) { m_PhysicsDebugView = flags; }
        PhysicsDebugFlags      GetPhysicsDebugView() const { return m_PhysicsDebugView; }
        void                   OnRuntimeStop();
        void                   OnRuntimePause();
        void                   OnRuntimeResume();
        // The engine must outlive its attachment to this scene.
        void SetAudioEngine(AudioEngine* engine);
        void PlayAudioSource(Entity entity);
        void StopAudioSource(Entity entity);
        void PauseAudioSource(Entity entity);
        bool IsAudioSourcePlaying(Entity entity) const;

        Entity GetCameraEntity();

        const std::string GetName() const { return m_Name; }
        void              SetName(const std::string& name) { m_Name = name; }

    private:
        template<typename T>
        void OnComponentAdd(Entity entity, T& component);
        // Removes the entity from its parent's children or the root order.
        void              Detach(Entity entity);
        Entity            CopyEntity(Entity source, Entity parent, Entity before);
        void              DestroyNativeScriptInstances();
        bool              OwnsAudioSource(Entity entity) const;
        void              StartAudio();
        void              StopAudio();
        void              UpdateAudio(double seconds);
        void              UpdateAudioListener(double seconds);
        void              PrepareAudioSource(Entity entity);
        void              PushAudioSource(Entity entity, double seconds);
        AudioEngine*      m_AudioEngine = nullptr;
        AudioClipRegistry m_AudioClips;
        bool              m_AudioRunning = false, m_AudioPaused = false, m_WarnedMultipleListeners = false;
        UIContext         m_UI;
        UIViewport        m_UIViewport;
        UUID              m_AudioListener = 0;
        glm::vec3         m_CameraAudioPosition {0};
        bool              m_HasCameraAudioPosition = false;

    private:
        void OnPhysicsJointRemoved(entt::registry&, entt::entity);
        void OnPhysicsComponentChanged(entt::registry&, entt::entity);
        void OnPhysicsBodyOwnershipChanged(entt::registry&, entt::entity);
        void OnPhysicsHierarchyChanged(entt::registry&, entt::entity);
        void OnPhysicsMeshChanged(entt::registry&, entt::entity);
        void OnPhysicsTransformChanged(entt::registry&, entt::entity);
        void OnPhysicsPrefabChanged(entt::registry&, entt::entity);
        // P2: dispatch script OnFixedUpdate here, before PhysicsWorld::Step.
        void                                    DispatchFixedUpdate(double seconds);
        void                                    UpdatePhysics(double seconds);
        void                                    RestoreSpringBoneTransforms();
        bool                                    m_SpringBonePreview = false;
        std::vector<std::pair<UUID, glm::quat>> m_SpringRenderRotations;
        glm::mat4                               SpringBoneRenderLocal(Entity) const;
        void                                    OnSpringBoneRemoved(entt::registry&, entt::entity);
        Ref<SpringBoneUpdateCache>              m_SpringBoneUpdate;
        Ref<ClothUpdateCache>                   m_ClothUpdate;
        bool                                    m_MotionSnapshotReady = false;
        void                                    PrepareMotionSnapshot();
        void                                    ResetClothMotion(Entity);
        void                                    OnClothRemoved(entt::registry&, entt::entity);
        void                                    ClearClothStreams();
        void                                    PublishClothStreams();
        uint64_t                                m_SecondaryMotionResetEpoch = 1;
        std::unique_ptr<PhysicsWorld>           m_PhysicsWorld;
        double                                  m_PhysicsAccumulator = 0;
        bool                m_PhysicsPaused = false, m_PhysicsSingleStep = false, m_PhysicsWritingTransforms = false;
        SceneRuntimeOptions m_RuntimeOptions;
        PhysicsDebugFlags   m_PhysicsDebugView = PhysicsDebugFlags::None;
        void                OnRenderComponentChanged(entt::registry&, entt::entity entity);
        void                OnEditableMeshChanged(entt::registry&, entt::entity entity);
        mutable std::unordered_map<entt::entity, Ref<const RuntimeMesh>>              m_EditableMeshes;
        mutable std::unordered_map<entt::entity, std::shared_ptr<const EditableMesh>> m_EditableSources;
        mutable uint64_t                                                              m_EditableMeshBuildCount = 0;
        void                                 OnEditableMeshRemoved(entt::registry&, entt::entity entity);
        void                                 OnTransformChanged(entt::registry&, entt::entity entity);
        void                                 OnPrefabChanged(entt::registry&, entt::entity entity);
        void                                 DirtySubtree(Entity entity);
        void                                 DirtyBounds(Entity entity);
        void                                 TrackPrefabAssets(Entity root);
        void                                 PollAssetChanges();
        bool                                 m_SuppressNotifications = false;
        std::unique_ptr<RenderWorld>         m_RenderWorld;
        std::shared_ptr<AssetChangeObserver> m_AssetChanges;
        std::unordered_set<entt::entity>     m_DirtyPrefabs, m_CapturePrefabNodes, m_DirtyBounds;
        std::unordered_map<AssetID, std::unordered_set<entt::entity>> m_PrefabAssetUsers;
        std::unordered_map<entt::entity, std::vector<AssetID>>        m_PrefabDependencies;
        struct ChildBounds
        {
            std::unordered_map<entt::entity, Bounds> Values;
            std::array<std::multiset<float>, 3>      Min, Max;
            void                                     Set(entt::entity child, const Bounds& bounds);
            Bounds                                   Aggregate() const;
        };
        std::unordered_map<entt::entity, ChildBounds> m_ChildBounds;
        AssetManager*                                 m_PrefabAssets      = nullptr;
        uint64_t                                      m_NextDrawOrder     = 1;
        bool                                          m_ReconcilingPrefab = false;
        void                                          ExpandPrefab(Entity root);
        entt::registry                                m_Registry;
        uint32_t                                      m_ViewportWidth = 0, m_ViewportHeight = 0;
        std::string                                   m_Name;
        PostProcessSettings                           m_PostProcessSettings;
        EnvironmentSettings                           m_EnvironmentSettings;
        bool                                          m_HasEnvironmentSettings = false;
        EntityMap                                     m_EntityMap;
        std::vector<UUID>                             m_RootOrder;

    private:
        friend class Entity;
        friend class RenderWorld;
        friend class PhysicsWorld;
        friend class SceneSerializer;
        friend struct SceneClipboardAccess;
    };

} // namespace Kans
