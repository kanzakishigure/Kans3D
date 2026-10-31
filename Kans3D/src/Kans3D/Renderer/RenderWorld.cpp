#include "kspch.h"
#include "RenderWorld.h"
#include <stdexcept>
#include "Kans3D/Physics/ClothSolver.h"
#include "Kans3D/Scene/Entity.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans
{
    AssetManager* RenderWorld::GetAssetManager() const { return m_Scene ? m_Scene->m_PrefabAssets : nullptr; }
    void          RenderWorld::SetAssetManager(AssetManager* assets)
    {
        if (m_Scene)
            m_Scene->SetPrefabAssetManager(assets);
    }
    bool RenderWorld::Order::operator<(const Order& b) const
    {
        if (Root != b.Root || !Root)
            return Submission != b.Submission ? Submission > b.Submission : EntityID < b.EntityID;
        return Part != b.Part ? Part < b.Part : EntityID < b.EntityID;
    }
    RenderWorld::Record RenderWorld::ExtractEntity(Entity entity) const
    {
        Record record;
        if (!m_Scene->IsEntityActive(entity))
            return record;
        const auto id   = entity.GetUUID();
        const auto root = m_Scene->GetPrefabInstanceRoot(entity);
        record.Sort = {root ? root.GetComponent<PrefabInstanceComponent>().DrawOrder : uint64_t(uint32_t(entity)),
                       root ? root.GetUUID() : UUID(0),
                       id,
                       entity.HasComponent<PrefabOwnedComponent>() ? entity.GetComponent<PrefabOwnedComponent>().Order :
                                                                     SIZE_MAX};
        const auto world = m_Scene->GetSpringBoneRenderTransform(entity);
        if (entity.HasComponent<MeshComponent>() || entity.HasComponent<EditableMeshComponent>())
        {
            CORE_ASSERT(!(entity.HasComponent<MeshComponent>() && entity.HasComponent<EditableMeshComponent>()),
                        "Mesh components are mutually exclusive");
            MeshComponent component;
            if (entity.HasComponent<EditableMeshComponent>())
            {
                const auto& editable        = entity.GetComponent<EditableMeshComponent>();
                component.Materials         = editable.Materials;
                component.CastShadows       = editable.CastShadows;
                component.RenderCustomDepth = editable.RenderCustomDepth;
                component.CustomStencil     = editable.CustomStencil;
                component.ReceiveShadows    = editable.ReceiveShadows;
                record.Key                  = MeshKey(m_Scene->ResolveEditableMesh(entity));
            }
            else
            {
                component  = entity.GetComponent<MeshComponent>();
                record.Key = MeshKey(component.Mesh);
            }
            record.Mesh             = ViewMeshSubmission {component, world, id, record.Sort.Root};
            record.OutlineTransform = world * glm::scale(glm::mat4(1), glm::vec3(1.007f));
            if (root && entity.HasComponent<PrefabOwnedComponent>())
            {
                std::function<glm::mat4(Entity)> relative = [&](Entity part) {
                    auto parent = m_Scene->GetParent(part);
                    auto local  = m_Scene->SpringBoneRenderLocal(part);
                    return parent && parent != root ? relative(parent) * local : local;
                };
                record.OutlineTransform = m_Scene->GetSpringBoneRenderTransform(root) *
                                          glm::scale(glm::mat4(1), glm::vec3(1.007f)) * relative(entity);
            }
            record.MeshSource = m_Scene->ResolveMesh(entity);
            if (record.MeshSource)
            {
                const auto&             data = record.MeshSource->GetData();
                Ref<const SkinningPose> skin;
                auto                    geometryWorld = world;
                Bounds                  animatedBounds;
                auto                    poseRoot = m_Scene->GetDeformationRoot(entity);
                if ((!data.Skin.empty() || !data.MorphTargets.empty()) && poseRoot)
                {
                    skin = m_Scene->GetSkinningPose(entity);
                    if (skin && !data.Skin.empty())
                    {
                        geometryWorld           = m_Scene->GetSpringBoneRenderTransform(poseRoot);
                        animatedBounds          = DeformedBounds(data, skin.get());
                        record.Mesh->Transform  = geometryWorld;
                        record.OutlineTransform = geometryWorld * glm::scale(glm::mat4(1), glm::vec3(1.007f));
                    }
                }
                if (!data.MorphTargets.empty())
                    animatedBounds = DeformedBounds(data, skin.get());
                for (size_t i = 0; i < data.Sections.size(); ++i)
                {
                    if (component.Section && *component.Section != i)
                        continue;
                    const auto& section     = data.Sections[i];
                    const auto  slot        = section.MaterialSlot;
                    const auto  material    = slot < component.Materials.size() && component.Materials[slot].IsValid() ?
                                                  component.Materials[slot] :
                                                  data.MaterialSlots.at(slot).DefaultMaterial;
                    const auto  bindBounds  = !skin || data.Skin.empty() ?
                                                  section.LocalBounds :
                                                  section.LocalBounds.Transformed(data.SkinBindTransform);
                    const auto  localBounds = animatedBounds.IsValid() ? animatedBounds : bindBounds;
                    record.Draws.push_back({record.Key,
                                            material,
                                            uint32_t(i),
                                            geometryWorld,
                                            localBounds,
                                            localBounds.Transformed(geometryWorld),
                                            id,
                                            record.Sort.Root,
                                            component.CastShadows,
                                            component.ReceiveShadows,
                                            skin,
                                            {},
                                            component.RenderCustomDepth,
                                            component.CustomStencil});
                }
            }
        }
        if (entity.HasComponent<ClothComponent>())
        {
            const auto& c = entity.GetComponent<ClothComponent>();
            const auto  r = c.Runtime;
            if (c.Enabled && r && r->Active && r->BindingValid && r->Stream)
                for (auto& draw : record.Draws)
                    if (draw.Section == c.Section)
                    {
                        draw.Mesh = MeshKey(Ref<const DynamicVertexStream>(r->Stream));
                        draw.SkinPose.reset();
                        draw.Transform   = r->Root;
                        draw.LocalBounds = r->Stream->Data.LocalBounds;
                        draw.WorldBounds = draw.LocalBounds.Transformed(draw.Transform);
                    }
        }
        if (entity.HasComponent<DirLightComponent>())
        {
            const auto& light  = entity.GetComponent<DirLightComponent>();
            record.Directional = {light.Direction,
                                  light.Diffuse_Intensity,
                                  light.Specular_Intensity,
                                  light.Ambient_Intensity,
                                  light.Shadows,
                                  id};
        }
        if (entity.HasComponent<PointLightComponent>())
        {
            const auto& light = entity.GetComponent<PointLightComponent>();
            record.Point      = {glm::vec3(world[3]),
                                 light.Diffuse_Intensity,
                                 light.Specular_Intensity,
                                 light.Ambient_Intensity,
                                 light.Shadows,
                                 id};
        }
        if (entity.HasComponent<SpriteRendererComponent>())
        {
            const auto& sprite = entity.GetComponent<SpriteRendererComponent>();
            record.Sprite      = {world,
                                  {},
                                  sprite.Color,
                                  sprite.TilingFactor,
                                  sprite.UVRect,
                                  sprite.FlipX,
                                  sprite.FlipY,
                                  sprite.SortingOrder,
                                  sprite.Texture};
        }
        for (auto parent = m_Scene->GetParent(entity); parent; parent = m_Scene->GetParent(parent))
            record.Ancestors.push_back(parent.GetUUID());
        return record;
    }
    const std::vector<RenderWorld::DrawProxy>& RenderWorld::GetEntityDraws(entt::entity entity) const
    {
        static const std::vector<DrawProxy> empty;
        const auto                          found = m_Records.find(entity);
        return found == m_Records.end() ? empty : found->second.Draws;
    }
    bool RenderWorld::MatchesSubtree(UUID entity, UUID instanceRoot, UUID subtree) const
    {
        if (!subtree)
            return false;
        if (entity == subtree || instanceRoot == subtree)
            return true;
        if (m_Scene)
            for (auto parent = m_Scene->GetEntityByUUID(entity); parent; parent = m_Scene->GetParent(parent))
                if (parent.GetUUID() == subtree)
                    return true;
        return false;
    }
    bool RenderWorld::IsIsolated(const ViewHighlights& highlights, UUID entity, UUID instanceRoot, uint32_t slot) const
    {
        if (highlights.Isolate.empty())
            return true;
        for (const auto& [subtree, isolated] : highlights.Isolate)
            if (isolated == slot && MatchesSubtree(entity, instanceRoot, subtree))
                return true;
        return false;
    }
    std::vector<RenderWorld::DrawProxy> RenderWorld::CollectDraws(UUID subtree) const
    {
        if (!m_Scene)
        {
            std::vector<DrawProxy> result;
            for (const auto& draw : m_ProceduralDraws)
                if (!subtree || MatchesSubtree(draw.EntityID, draw.InstanceRoot, subtree))
                    result.push_back(draw);
            return result;
        }
        std::vector<DrawProxy> result;
        for (const auto& [order, handle] : m_Order)
        {
            const auto& record = m_Records.at(handle);
            if (!subtree || order.EntityID == subtree ||
                std::find(record.Ancestors.begin(), record.Ancestors.end(), subtree) != record.Ancestors.end())
            {
                const auto first = result.size();
                result.insert(result.end(), record.Draws.begin(), record.Draws.end());
                auto root = m_Scene->GetEntityByUUID(order.Root ? order.Root : order.EntityID);
                if (root && root.HasComponent<MaterialParameterDriversComponent>())
                    for (size_t i = first; i < result.size(); ++i)
                    {
                        const auto& drivers = root.GetComponent<MaterialParameterDriversComponent>();
                        uint32_t slot  = record.MeshSource ?
                                             record.MeshSource->GetData().Sections.at(result[i].Section).MaterialSlot :
                                             0;
                        auto     found = drivers.SlotValues.find(slot);
                        result[i].Parameters = found == drivers.SlotValues.end() ? drivers.Values : found->second;
                    }
            }
        }
        return result;
    }
    void RenderWorld::AssetChanged(AssetID asset)
    {
        const auto found = m_MeshUsers.find(asset);
        if (found != m_MeshUsers.end())
            for (auto handle : found->second)
                if (m_Scene->m_Registry.valid(handle))
                    m_Scene->OnRenderComponentChanged(m_Scene->m_Registry, handle);
    }
    std::shared_ptr<TextLayoutCache> RenderWorld::GetTextLayout(Entity entity) const
    {
        auto&                record = m_TextRecords[entt::entity(entity)];
        const auto&          c      = entity.GetComponent<TextComponent>();
        Ref<const FontAsset> font;
        bool                 missing = c.Font.IsValid();
        if (missing && GetAssetManager())
        {
            auto status = GetAssetManager()->Request<FontAsset>(c.Font);
            if (status.IsReady())
            {
                font    = status.Asset;
                missing = false;
            }
            else if (status.State == AssetLoadState::Loading)
                missing = false;
        }
        if (missing && !record.WarnedMissing)
        {
            CORE_WARN("Text entity {}: missing font {}; using engine defaults",
                      uint64_t(entity.GetUUID()),
                      c.Font.ToString());
            record.WarnedMissing = true;
        }
        auto& fonts = FontSystem::Default();
        if (auto* assets = GetAssetManager())
            fonts.SetResolver([assets](AssetID id) -> Ref<const FontAsset> {
                auto status = assets->Request<FontAsset>(id);
                return status.IsReady() ? status.Asset : Ref<const FontAsset>();
            });
        record.Layout->Update(c, font);
        fonts.SetResolver({});
        return record.Layout;
    }
    std::vector<ViewTextDraw> RenderWorld::CollectTexts() const
    {
        std::vector<ViewTextDraw> result;
        if (!m_Scene)
            return result;
        for (auto handle : m_Scene->m_Registry.view<TextComponent, TransformComponent, IDComponent>())
        {
            Entity entity {handle, m_Scene};
            if (!m_Scene->IsEntityActive(entity))
                continue;
            const auto& c      = entity.GetComponent<TextComponent>();
            auto        layout = GetTextLayout(entity);
            auto        world  = m_Scene->GetSpringBoneRenderTransform(entity);
            auto        root   = m_Scene->GetPrefabInstanceRoot(entity);
            result.push_back({c,
                              world,
                              entity.GetUUID(),
                              root ? root.GetUUID() : UUID(0),
                              layout,
                              TextWorldBounds(c, layout->LocalBounds(), world)});
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.EntityID < b.EntityID; });
        return result;
    }
    std::shared_ptr<UIInstance> RenderWorld::GetUIInstance(Entity entity, bool world) const
    {
        auto assets = GetAssetManager();
        if (!assets || !entity ||
            (world ? !entity.HasComponent<UIWorldComponent>() : !entity.HasComponent<UIScreenComponent>()))
            return {};
        auto id = world ? entity.GetComponent<UIWorldComponent>().UI : entity.GetComponent<UIScreenComponent>().UI;
        if (!id.IsValid())
            return {};
        auto& r = m_UIRecords[{entt::entity(entity), world}];
        if (!r.Instance || r.ID != id)
        {
            r.ID       = id;
            r.Instance = std::make_shared<UIInstance>(id, [assets](AssetID id) -> Ref<const UIAsset> {
                auto s = assets->Request<UIAsset>(id);
                return s.IsReady() ? s.Asset : Ref<const UIAsset>();
            });
        }
        auto refreshed = r.Instance->Refresh();
        return r.Instance;
    }
    std::vector<ViewUI> RenderWorld::CollectUIs(const RenderCamera& camera) const
    {
        std::vector<ViewUI> result;
        if (!m_Scene)
            return result;
        for (auto it = m_UIRecords.begin(); it != m_UIRecords.end();)
            if (!m_Scene->m_Registry.valid(it->first.first) ||
                (it->first.second ? !m_Scene->m_Registry.all_of<UIWorldComponent>(it->first.first) :
                                    !m_Scene->m_Registry.all_of<UIScreenComponent>(it->first.first)))
                it = m_UIRecords.erase(it);
            else
                ++it;
        for (auto handle : m_Scene->m_Registry.view<UIScreenComponent, IDComponent>())
        {
            Entity e {handle, m_Scene};
            auto&  c = e.GetComponent<UIScreenComponent>();
            if (!c.Visible || !m_Scene->IsEntityActive(e))
                continue;
            if (auto instance = GetUIInstance(e))
                result.push_back({instance, glm::mat4(1), true, true, true, c.SortOrder, e.GetUUID(), {}});
        }
        for (auto handle : m_Scene->m_Registry.view<UIWorldComponent, TransformComponent, IDComponent>())
        {
            Entity e {handle, m_Scene};
            auto&  c = e.GetComponent<UIWorldComponent>();
            if (!c.Visible || !m_Scene->IsEntityActive(e))
                continue;
            if (auto instance = GetUIInstance(e, true))
            {
                auto          ref = instance->Data() ? instance->Data()->ReferenceResolution : glm::vec2(1920, 1080);
                TextComponent style;
                style.Billboard = c.Billboard;
                auto transform =
                    TextWorldTransform(
                        style, m_Scene->GetSpringBoneRenderTransform(e), camera.Position, camera.viewMatrix) *
                    glm::translate(glm::mat4(1), glm::vec3(-c.WorldSize.x * .5f, c.WorldSize.y * .5f, 0)) *
                    glm::scale(glm::mat4(1), glm::vec3(c.WorldSize.x / ref.x, -c.WorldSize.y / ref.y, 1));
                Bounds bounds;
                bounds.Expand({0, 0, -.001f});
                bounds.Expand({ref.x, ref.y, .001f});
                result.push_back({instance,
                                  transform,
                                  false,
                                  c.AlwaysOnTop,
                                  c.DoubleSided,
                                  c.SortOrder,
                                  e.GetUUID(),
                                  bounds.Transformed(transform)});
            }
        }
        std::stable_sort(result.begin(), result.end(), [](auto& a, auto& b) {
            return a.SortOrder != b.SortOrder ? a.SortOrder < b.SortOrder : a.EntityID < b.EntityID;
        });
        return result;
    }
    void RenderWorld::Synchronize()
    {
        if (!m_Scene)
            return;
        m_Scene->UpdatePrefabs();
        for (auto it = m_TextRecords.begin(); it != m_TextRecords.end();)
            if (!m_Scene->m_Registry.valid(it->first) || !m_Scene->m_Registry.all_of<TextComponent>(it->first))
                it = m_TextRecords.erase(it);
            else
                ++it;
        for (auto handle : m_Scene->m_Registry.view<TextComponent>())
        {
            auto previous = m_TextRecords.find(handle);
            auto revision = previous == m_TextRecords.end() ? 0 : previous->second.Layout->Revision();
            auto layout   = GetTextLayout({handle, m_Scene});
            if (layout->Revision() != revision)
                m_Scene->DirtyBounds({handle, m_Scene});
        }
        m_Scene->UpdateBounds();
        m_Scene->UpdateMaterialParameterDrivers();
        m_Stats           = m_PendingStats;
        m_PendingStats    = {};
        using SpriteGroup = decltype(m_Scene->m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>));
        // Create the legacy owning group lazily at the first sprite extraction.
        const auto sprites = [&] {
            if (!m_Dirty.empty() &&
                (!m_SpriteOrder.empty() || !m_Scene->m_Registry.view<SpriteRendererComponent>().empty()))
                return m_Scene->m_Registry.group<TransformComponent>(entt::get<SpriteRendererComponent>);
            return SpriteGroup {};
        }();
        std::unordered_set<size_t> spritePositions;
        for (auto handle : m_Dirty)
        {
            ++m_Stats.EntitiesVisited;
            auto         old      = m_Records.find(handle);
            const size_t oldCount = old == m_Records.end() ? 0 : old->second.Draws.size();
            const bool   valid    = m_Scene->m_Registry.valid(handle) &&
                                    m_Scene->m_Registry.all_of<TransformComponent, IDComponent>(handle);
            Record       next;
            if (valid)
                next = ExtractEntity({handle, m_Scene});
            const bool   renderable = next.Mesh || next.Point || next.Directional || next.Sprite;
            const size_t nextCount  = next.Draws.size();
            auto         countProxy = [&](bool before, bool after) {
                m_Stats.Created += !before && after;
                m_Stats.Updated += before && after;
                m_Stats.Removed += before && !after;
            };
            countProxy(old != m_Records.end() && old->second.Point.has_value(), next.Point.has_value());
            countProxy(old != m_Records.end() && old->second.Directional.has_value(), next.Directional.has_value());
            countProxy(old != m_Records.end() && old->second.Sprite.has_value(), next.Sprite.has_value());
            // Section identity survives material/transform/mesh-data changes.
            size_t before = 0, after = 0;
            while (before < oldCount || after < nextCount)
            {
                if (before < oldCount && after < nextCount &&
                    old->second.Draws[before].Section == next.Draws[after].Section)
                {
                    ++m_Stats.Updated;
                    ++before;
                    ++after;
                }
                else if (after == nextCount ||
                         (before < oldCount && old->second.Draws[before].Section < next.Draws[after].Section))
                {
                    ++m_Stats.Removed;
                    ++before;
                }
                else
                {
                    ++m_Stats.Created;
                    ++after;
                }
            }
            const auto oldSpriteIndex = old != m_Records.end() ? old->second.SpriteIndex : SIZE_MAX;
            if (next.Sprite || (valid && m_Scene->m_Registry.all_of<SpriteRendererComponent>(handle)))
                next.SpriteIndex = sprites.storage<TransformComponent>().index(handle);
            if (oldSpriteIndex != next.SpriteIndex ||
                (old != m_Records.end() && bool(old->second.Sprite) != bool(next.Sprite)))
            {
                if (oldSpriteIndex != SIZE_MAX)
                    spritePositions.insert(oldSpriteIndex);
                if (next.SpriteIndex != SIZE_MAX)
                    spritePositions.insert(next.SpriteIndex);
            }
            if (old != m_Records.end())
            {
                if (old->second.Mesh)
                {
                    m_Order.erase(old->second.Sort);
                    auto users = m_MeshUsers.find(old->second.Mesh->Mesh.Mesh);
                    if (users != m_MeshUsers.end())
                    {
                        users->second.erase(handle);
                        if (users->second.empty())
                            m_MeshUsers.erase(users);
                    }
                }
                m_Records.erase(old);
                m_Lights.erase(handle);
            }
            if (renderable)
            {
                if (next.Mesh)
                {
                    m_Order.emplace(next.Sort, handle);
                    m_MeshUsers[next.Mesh->Mesh.Mesh].insert(handle);
                }
                if (next.Point || next.Directional)
                    m_Lights.insert(handle);
                m_Records.emplace(handle, std::move(next));
            }
        }
        m_Dirty.clear();
        // A group insertion/removal swaps only the affected packed positions.
        // Preserve its reverse-packed execution order without scanning all sprites.
        std::vector<std::pair<size_t, entt::entity>> replacements;
        for (auto index : spritePositions)
        {
            if (index < sprites.size())
            {
                const auto handle = sprites.rbegin()[index];
                auto       found  = m_Records.find(handle);
                if (found != m_Records.end() && found->second.Sprite)
                {
                    auto& record = found->second;
                    m_SpriteOrder.erase(record.SpriteIndex);
                    record.SpriteIndex = index;
                    replacements.emplace_back(index, handle);
                    ++m_Stats.SpriteOrderVisits;
                }
            }
            m_SpriteOrder.erase(index);
        }
        for (auto [index, handle] : replacements)
            m_SpriteOrder[index] = handle;
        if (m_Validate)
            Validate();
    }
    void RenderWorld::CollectLights(SceneInfo& info) const
    {
        if (!m_Scene)
        {
            info.dirLight            = m_ProceduralDirectional.value_or(DirLight {});
            info.HasDirectionalLight = m_ProceduralDirectional.has_value();
            info.pointLights         = m_ProceduralPointLights;
            info.pointLight          = info.pointLights.empty() ? PointLight {} : info.pointLights.front();
            return;
        }
        info.dirLight   = {};
        info.pointLight = {};
        info.pointLights.clear();
        info.HasDirectionalLight = false;
        uint64_t directionalID   = UINT64_MAX;
        struct Candidate
        {
            PointLight Light;
            float      Distance;
            uint64_t   ID;
        };
        std::vector<Candidate> candidates;
        for (auto handle : m_Lights)
        {
            const auto& record = m_Records.at(handle);
            const auto  id     = uint64_t(record.Sort.EntityID);
            if (record.Directional && (!info.HasDirectionalLight || id < directionalID))
            {
                info.dirLight            = *record.Directional;
                info.HasDirectionalLight = true;
                directionalID            = id;
            }
            if (record.Point)
            {
                const auto  delta    = record.Point->Position - info.sceneCamera.Position;
                const float distance = glm::dot(delta, delta);
                candidates.push_back(
                    {*record.Point, std::isfinite(distance) ? distance : std::numeric_limits<float>::max(), id});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.Distance != b.Distance ? a.Distance < b.Distance : a.ID < b.ID;
        });
        for (size_t i = 0; i < std::min(candidates.size(), size_t(EnginePointLightLimit)); ++i)
            info.pointLights.push_back(candidates[i].Light);
        if (!info.pointLights.empty())
            info.pointLight = info.pointLights.front();
    }
    void RenderWorld::Extract(RenderView& output, const RenderResource& settings) const
    {
        if (m_Scene)
            output.Scene.PostProcess = m_Scene->GetPostProcessSettings();
        CollectLights(output.Scene);
        output.Texts = CollectTexts();
        output.Draws.clear();
        output.Meshes.clear();
        output.Geometry.clear();
        output.Sprites.clear();
        output.Grids.clear();
        output.ImmediateOrder.clear();
        for (const auto& proxy : m_ProceduralDraws)
            output.Draws.push_back({proxy.Mesh,
                                    proxy.Transform,
                                    proxy.Material,
                                    output.DebugMode,
                                    ModelStencilMode::Disabled,
                                    proxy.LocalBounds,
                                    {},
                                    false,
                                    proxy.Section,
                                    proxy.EntityID,
                                    proxy.InstanceRoot,
                                    proxy.WorldBounds,
                                    true,
                                    proxy.CastShadows,
                                    proxy.ReceiveShadows,
                                    proxy.SkinPose,
                                    proxy.Parameters,
                                    proxy.RenderCustomDepth,
                                    proxy.CustomStencil});
        const auto& state = settings.Piplinestate;
        for (const auto& [order, handle] : m_Order)
        {
            const auto& record     = m_Records.at(handle);
            auto        submission = *record.Mesh;
            submission.Stencil = state.EnableStencil ? ModelStencilMode::WriteReference : ModelStencilMode::Disabled;
            output.Meshes.push_back(submission);
            for (const auto& proxy : record.Draws)
                output.Draws.push_back({proxy.Mesh,
                                        proxy.Transform,
                                        proxy.Material,
                                        output.DebugMode,
                                        submission.Stencil,
                                        proxy.LocalBounds,
                                        {},
                                        false,
                                        proxy.Section,
                                        proxy.EntityID,
                                        proxy.InstanceRoot,
                                        proxy.WorldBounds,
                                        true,
                                        proxy.CastShadows,
                                        proxy.ReceiveShadows,
                                        proxy.SkinPose,
                                        proxy.Parameters,
                                        proxy.RenderCustomDepth,
                                        proxy.CustomStencil});
            if (auto root =
                    m_Scene->GetEntityByUUID(submission.InstanceRoot ? submission.InstanceRoot : submission.EntityID);
                root && root.HasComponent<MaterialParameterDriversComponent>())
                for (size_t i = output.Draws.size() - record.Draws.size(); i < output.Draws.size(); ++i)
                {
                    const auto& drivers = root.GetComponent<MaterialParameterDriversComponent>();
                    uint32_t slot = record.MeshSource ?
                                        record.MeshSource->GetData().Sections.at(output.Draws[i].Section).MaterialSlot :
                                        0;
                    auto     found             = drivers.SlotValues.find(slot);
                    output.Draws[i].Parameters = found == drivers.SlotValues.end() ? drivers.Values : found->second;
                }
            const auto& mesh = submission.Mesh;
            if (std::any_of(
                    record.Draws.begin(), record.Draws.end(), [](const auto& draw) { return bool(draw.Mesh.Stream); }))
            {
                for (const auto& proxy : record.Draws)
                {
                    if (state.EnableStencil)
                        output.Geometry.push_back({proxy.Transform * glm::scale(glm::mat4(1), glm::vec3(1.007f)),
                                                   "StencilShader",
                                                   ModelGeometryMode::StencilOutline,
                                                   proxy.Mesh,
                                                   proxy.Section,
                                                   submission.EntityID,
                                                   proxy.SkinPose});
                    if (state.EnableOutline)
                        output.Geometry.push_back({proxy.Transform,
                                                   "OutLineShader",
                                                   ModelGeometryMode::BackFaces,
                                                   proxy.Mesh,
                                                   proxy.Section,
                                                   submission.EntityID,
                                                   proxy.SkinPose});
                    if (state.EnableDebugNormal)
                        output.Geometry.push_back({proxy.Transform,
                                                   "DebugNormalShader",
                                                   ModelGeometryMode::Default,
                                                   proxy.Mesh,
                                                   proxy.Section,
                                                   submission.EntityID,
                                                   proxy.SkinPose});
                }
            }
            else
            {
                if (state.EnableStencil)
                    output.Geometry.push_back(
                        {record.OutlineTransform,
                         "StencilShader",
                         ModelGeometryMode::StencilOutline,
                         record.Key,
                         mesh.Section,
                         submission.EntityID,
                         record.Draws.empty() ? Ref<const SkinningPose>() : record.Draws.front().SkinPose});
                if (state.EnableOutline)
                    output.Geometry.push_back(
                        {submission.Transform,
                         "OutLineShader",
                         ModelGeometryMode::BackFaces,
                         record.Key,
                         mesh.Section,
                         submission.EntityID,
                         record.Draws.empty() ? Ref<const SkinningPose>() : record.Draws.front().SkinPose});
                if (state.EnableDebugNormal)
                    output.Geometry.push_back(
                        {submission.Transform,
                         "DebugNormalShader",
                         ModelGeometryMode::Default,
                         record.Key,
                         mesh.Section,
                         submission.EntityID,
                         record.Draws.empty() ? Ref<const SkinningPose>() : record.Draws.front().SkinPose});
            }
        }
        // Phase 3 groups shell kinds across parts sharing the same submission order.
        std::unordered_map<UUID, uint64_t> submissions;
        for (const auto& [order, handle] : m_Order)
            submissions[order.EntityID] = order.Submission;
        std::stable_sort(output.Geometry.begin(), output.Geometry.end(), [&](const auto& a, const auto& b) {
            const auto ao = submissions.at(a.EntityID), bo = submissions.at(b.EntityID);
            if (ao != bo)
                return ao > bo;
            auto rank = [](const auto& d) {
                return d.Mode == ModelGeometryMode::StencilOutline ? 0 : d.Mode == ModelGeometryMode::BackFaces ? 1 : 2;
            };
            return rank(a) < rank(b);
        });
        for (auto sprite = m_SpriteOrder.rbegin(); sprite != m_SpriteOrder.rend(); ++sprite)
            if (auto found = m_Records.find(sprite->second); found != m_Records.end() && found->second.Sprite)
                output.Sprites.push_back(*found->second.Sprite);
        if (!output.Sprites.empty())
            output.ImmediateOrder.push_back({ViewImmediateKind::Sprites, 0});
        if (state.EnableGridShader)
        {
            output.Grids.push_back({});
        }
    }
    void RenderWorld::Validate() const
    {
        if (!m_Scene)
            return;
#ifndef NDEBUG
        auto stale = [&](UUID id) {
            throw std::logic_error("RenderWorld stale entity " + std::to_string(uint64_t(id)));
        };
        size_t count = 0;
        for (auto handle : m_Scene->m_Registry.view<TransformComponent, IDComponent>())
        {
            const auto expected = ExtractEntity({handle, m_Scene});
            if (!expected.Mesh && !expected.Point && !expected.Directional && !expected.Sprite)
                continue;
            ++count;
            const auto found = m_Records.find(handle);
            if (found == m_Records.end())
                stale(expected.Sort.EntityID);
            const auto& actual = found->second;
            if (expected.Mesh.has_value() != actual.Mesh.has_value() ||
                expected.Sprite.has_value() != actual.Sprite.has_value() ||
                expected.Point.has_value() != actual.Point.has_value() ||
                expected.Directional.has_value() != actual.Directional.has_value() || expected.Sort < actual.Sort ||
                actual.Sort < expected.Sort || expected.Ancestors != actual.Ancestors ||
                expected.Draws.size() != actual.Draws.size())
                stale(expected.Sort.EntityID);
            if (expected.Key != actual.Key)
                stale(expected.Sort.EntityID);
            if (m_Scene->m_Registry.all_of<EditableMeshComponent>(handle))
            {
                const auto snapshot = m_Scene->m_EditableSources.find(handle);
                if (snapshot == m_Scene->m_EditableSources.end() ||
                    !(*snapshot->second == m_Scene->m_Registry.get<EditableMeshComponent>(handle).Mesh))
                    stale(expected.Sort.EntityID);
            }
            if (expected.Mesh)
            {
                const auto& a = *actual.Mesh;
                const auto& e = *expected.Mesh;
                if (a.Transform != e.Transform || a.Mesh.Mesh != e.Mesh.Mesh || a.Mesh.Materials != e.Mesh.Materials ||
                    a.Mesh.Section != e.Mesh.Section || actual.OutlineTransform != expected.OutlineTransform)
                    stale(e.EntityID);
                for (size_t i = 0; i < expected.Draws.size(); ++i)
                {
                    const auto& x = expected.Draws[i];
                    const auto& y = actual.Draws[i];
                    if (x.Mesh != y.Mesh || x.Material != y.Material || x.Section != y.Section ||
                        x.LocalBounds.Min != y.LocalBounds.Min || x.LocalBounds.Max != y.LocalBounds.Max ||
                        x.WorldBounds.Min != y.WorldBounds.Min || x.WorldBounds.Max != y.WorldBounds.Max ||
                        x.RenderCustomDepth != y.RenderCustomDepth || x.CustomStencil != y.CustomStencil ||
                        x.CastShadows != y.CastShadows || x.ReceiveShadows != y.ReceiveShadows)
                        stale(e.EntityID);
                }
            }
            if (expected.Sprite &&
                (actual.Sprite->Transform != expected.Sprite->Transform ||
                 actual.Sprite->TextureAsset != expected.Sprite->TextureAsset ||
                 actual.Sprite->Color != expected.Sprite->Color ||
                 actual.Sprite->TilingFactor != expected.Sprite->TilingFactor ||
                 actual.Sprite->UVRect != expected.Sprite->UVRect || actual.Sprite->FlipX != expected.Sprite->FlipX ||
                 actual.Sprite->FlipY != expected.Sprite->FlipY ||
                 actual.Sprite->SortingOrder != expected.Sprite->SortingOrder))
                stale(expected.Sort.EntityID);
            if (expected.Point && (actual.Point->Position != expected.Point->Position ||
                                   actual.Point->Diffuse_Intensity != expected.Point->Diffuse_Intensity ||
                                   actual.Point->Specular_Intensity != expected.Point->Specular_Intensity ||
                                   actual.Point->Ambient_Intensity != expected.Point->Ambient_Intensity ||
                                   actual.Point->Shadows != expected.Point->Shadows))
                stale(expected.Sort.EntityID);
            if (expected.Directional &&
                (actual.Directional->Dirction != expected.Directional->Dirction ||
                 actual.Directional->Diffuse_Intensity != expected.Directional->Diffuse_Intensity ||
                 actual.Directional->Specular_Intensity != expected.Directional->Specular_Intensity ||
                 actual.Directional->Ambient_Intensity != expected.Directional->Ambient_Intensity ||
                 actual.Directional->Shadows != expected.Directional->Shadows))
                stale(expected.Sort.EntityID);
        }
        const auto sprites =
            m_Scene->m_Registry.group_if_exists<TransformComponent>(entt::get<SpriteRendererComponent>);
        auto actualSprite = m_SpriteOrder.rbegin();
        for (auto handle : sprites)
        {
            if (!m_Scene->IsEntityActive(Entity {handle, m_Scene}))
                continue;
            if (actualSprite == m_SpriteOrder.rend() || actualSprite->second != handle)
                stale(m_Scene->m_Registry.get<IDComponent>(handle).ID);
            ++actualSprite;
        }
        if (actualSprite != m_SpriteOrder.rend())
            stale(m_Records.at(actualSprite->second).Sort.EntityID);
        if (count != m_Records.size())
            for (const auto& [handle, record] : m_Records)
                if (!m_Scene->m_Registry.valid(handle) ||
                    !m_Scene->m_Registry.all_of<TransformComponent, IDComponent>(handle) ||
                    !m_Scene->m_Registry.any_of<MeshComponent,
                                                EditableMeshComponent,
                                                PointLightComponent,
                                                DirLightComponent,
                                                SpriteRendererComponent>(handle))
                    stale(record.Sort.EntityID);
#endif
    }
} // namespace Kans
