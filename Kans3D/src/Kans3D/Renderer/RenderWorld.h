#pragma once
#include <map>
#include <unordered_set>
#include <entt.hpp>
#include "DebugDraw.h"
#include "RenderView.h"

namespace Kans
{
    class Entity;
    namespace Vfx
    {
        class VfxWorld;
    }
    // Persistent CPU data, owned by Scene. No device, camera or frame resources.
    class RenderWorld
    {
    public:
        struct DrawProxy
        {
            MeshKey   Mesh;
            AssetID   Material;
            uint32_t  Section = 0;
            glm::mat4 Transform {1};
            Bounds    LocalBounds, WorldBounds;
            UUID      EntityID = 0, InstanceRoot = 0;
            bool      CastShadows = true, ReceiveShadows = true;
            Ref<const SkinningPose> SkinPose;
            Ref<const InstanceMaterialParameters> Parameters;
            bool                                  RenderCustomDepth = false;
            uint8_t                               CustomStencil     = 0;
        };
        struct Stats
        {
            size_t Created = 0, Updated = 0, Removed = 0;
            size_t EntitiesVisited = 0, PrefabsVisited = 0, PrefabNodesVisited = 0, BoundsUpdated = 0,
                   SpriteOrderVisits = 0;
        };
        explicit RenderWorld(Scene& scene) : m_Scene(&scene) {}
        RenderWorld() = default; // Dedicated procedural worlds (e.g. material preview).
        void SetProceduralDraws(std::vector<DrawProxy> draws) { m_ProceduralDraws = std::move(draws); }
        void SetProceduralLights(std::vector<PointLight> points, std::optional<DirLight> directional = {})
        {
            m_ProceduralPointLights = std::move(points);
            m_ProceduralDirectional = std::move(directional);
        }
        Vfx::VfxWorld*                   GetVfxWorld() const;
        AssetManager*                 GetAssetManager() const;
        void                          SetAssetManager(AssetManager* assets);
        void                          Synchronize();
        std::shared_ptr<TextLayoutCache> GetTextLayout(Entity entity) const;
        std::vector<ViewTextDraw>        CollectTexts(const std::unordered_set<UUID>& hidden = {}) const;
        std::shared_ptr<UIInstance>      GetUIInstance(Entity entity, bool world = false) const;
        std::vector<ViewUI> CollectUIs(const RenderCamera&, const std::unordered_set<UUID>& hidden = {}) const;
        void                          Extract(RenderView& output, const RenderResource& settings) const;
        // Reuses a flattened snapshot from this frame's resource preparation.
        void Extract(RenderView& output, const RenderResource& settings, const std::unordered_set<UUID>& hidden) const;
        void CollectLights(SceneInfo& output, const std::unordered_set<UUID>& hidden = {}) const;
        void                             CacheVfxLight(const SceneInfo& info) const;
        std::vector<DrawProxy> CollectDraws(UUID subtree = 0, const std::unordered_set<UUID>& hidden = {}) const;
        // Expands explicitly hidden roots to descendants in one traversal.
        std::unordered_set<UUID>      ResolveHiddenEntities(const std::unordered_set<UUID>& hidden) const;
        bool                          MatchesSubtree(UUID entity, UUID instanceRoot, UUID subtree) const;
        // Whether a part using `slot` survives the view's isolate list.
        bool IsIsolated(const ViewHighlights& highlights, UUID entity, UUID instanceRoot, uint32_t slot) const;
        const Stats&                  GetStats() const { return m_Stats; }
        const std::vector<DrawProxy>& GetEntityDraws(entt::entity entity) const;
        void                          SetValidationEnabled(bool enabled) { m_Validate = enabled; }
        // Debug-only full reference extraction. Throws with the stale UUID.
        void                  Validate() const;
        DebugDraw                          Debug;
        RenderResource        Settings;
        std::filesystem::path              EnvironmentPath; // View/editor default for legacy scenes.
        std::filesystem::path              EnvironmentRoot; // Project directory; empty means current working directory.
        std::optional<EnvironmentSettings> Environment;     // Mirrored by Scene's setter; absent inherits defaults.

    private:
        struct Order
        {
            uint64_t Submission = 0;
            UUID     Root = 0, EntityID = 0;
            size_t   Part = SIZE_MAX;
            bool     operator<(const Order& other) const;
        };
        struct Record
        {
            Order                             Sort;
            std::optional<ViewMeshSubmission> Mesh;
            MeshKey                           Key;
            Ref<const MeshAsset>              MeshSource; // Retain the last ready version while reimport loads.
            glm::mat4                         OutlineTransform {1};
            std::vector<DrawProxy>            Draws;
            std::optional<DirLight>           Directional;
            std::optional<PointLight>         Point;
            std::optional<ViewSprite>         Sprite;
            size_t                            SpriteIndex = SIZE_MAX;
            std::vector<UUID>                 Ancestors;
        };
        Record                                   ExtractEntity(Entity entity) const;
        void                                     Dirty(entt::entity entity) { m_Dirty.insert(entity); }
        void                                     AssetChanged(AssetID asset);
        Scene*                                                        m_Scene = nullptr;
        std::vector<DrawProxy>                                        m_ProceduralDraws;
        std::vector<PointLight>                                       m_ProceduralPointLights;
        std::optional<DirLight>                                       m_ProceduralDirectional;
        std::unordered_set<entt::entity>         m_Dirty;
        std::unordered_map<entt::entity, Record> m_Records;
        std::map<Order, entt::entity>            m_Order;
        std::map<size_t, entt::entity>           m_SpriteOrder;
        std::unordered_set<entt::entity>         m_Lights;
        std::unordered_map<AssetID, std::unordered_set<entt::entity>> m_MeshUsers;
        Stats                                                         m_Stats, m_PendingStats;
        bool                                                          m_Validate = false;
        struct TextRecord
        {
            std::shared_ptr<TextLayoutCache> Layout        = std::make_shared<TextLayoutCache>();
            bool                             WarnedMissing = false;
        };
        mutable std::unordered_map<entt::entity, TextRecord> m_TextRecords;
        struct UIRecord
        {
            AssetID                     ID;
            std::shared_ptr<UIInstance> Instance;
        };
        mutable std::map<std::pair<entt::entity, bool>, UIRecord> m_UIRecords;
        friend class Scene;
    };
} // namespace Kans
