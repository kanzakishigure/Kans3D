#include "kspch.h"

#include <set>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/Math/Math.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Components.h"
#include "Entity.h"
#include "Scene.h"

#include "Kans3D/Scene/ScriptableEntity.h"
#include "Kans3D/Script/ScriptEngine.h"

namespace Kans
{

    Scene::Scene(const std::string& name) : m_Name(name)
    {
        m_RenderWorld = std::make_unique<RenderWorld>(*this);
        auto physics  = [&](auto* type) {
            using T = std::remove_pointer_t<decltype(type)>;
            if constexpr (std::is_same_v<T, BoxColliderComponent> || std::is_same_v<T, SphereColliderComponent> ||
                          std::is_same_v<T, CapsuleColliderComponent> || std::is_same_v<T, MeshColliderComponent>)
            {
                m_Registry.on_construct<T>().template connect<&Scene::OnPhysicsComponentChanged>(this);
                m_Registry.on_update<T>().template connect<&Scene::OnPhysicsComponentChanged>(this);
                m_Registry.on_destroy<T>().template connect<&Scene::OnPhysicsComponentChanged>(this);
            }
            else if constexpr (std::is_same_v<T, RigidBodyComponent>)
            {
                m_Registry.on_construct<T>().template connect<&Scene::OnPhysicsBodyOwnershipChanged>(this);
                m_Registry.on_update<T>().template connect<&Scene::OnPhysicsComponentChanged>(this);
                m_Registry.on_destroy<T>().template connect<&Scene::OnPhysicsBodyOwnershipChanged>(this);
            }
            else if constexpr (std::is_same_v<T, ActiveComponent> || std::is_same_v<T, RelationshipComponent>)
            {
                m_Registry.on_construct<T>().template connect<&Scene::OnPhysicsHierarchyChanged>(this);
                m_Registry.on_update<T>().template connect<&Scene::OnPhysicsHierarchyChanged>(this);
                m_Registry.on_destroy<T>().template connect<&Scene::OnPhysicsHierarchyChanged>(this);
            }
            else if constexpr (std::is_same_v<T, MeshComponent> || std::is_same_v<T, EditableMeshComponent>)
            {
                m_Registry.on_construct<T>().template connect<&Scene::OnPhysicsMeshChanged>(this);
                m_Registry.on_update<T>().template connect<&Scene::OnPhysicsMeshChanged>(this);
                m_Registry.on_destroy<T>().template connect<&Scene::OnPhysicsMeshChanged>(this);
            }
            else
            {
                m_Registry.on_construct<T>().template connect<&Scene::OnPhysicsPrefabChanged>(this);
                m_Registry.on_update<T>().template connect<&Scene::OnPhysicsPrefabChanged>(this);
                m_Registry.on_destroy<T>().template connect<&Scene::OnPhysicsPrefabChanged>(this);
            }
        };
        physics(static_cast<RigidBodyComponent*>(nullptr));
        physics(static_cast<ActiveComponent*>(nullptr));
        physics(static_cast<RelationshipComponent*>(nullptr));
        physics(static_cast<BoxColliderComponent*>(nullptr));
        physics(static_cast<SphereColliderComponent*>(nullptr));
        physics(static_cast<CapsuleColliderComponent*>(nullptr));
        physics(static_cast<MeshColliderComponent*>(nullptr));
        physics(static_cast<MeshComponent*>(nullptr));
        physics(static_cast<EditableMeshComponent*>(nullptr));
        physics(static_cast<GlobalWindComponent*>(nullptr));
        m_Registry.on_update<TransformComponent>().connect<&Scene::OnPhysicsTransformChanged>(this);
        m_Registry.on_update<RelationshipComponent>().connect<&Scene::OnPhysicsTransformChanged>(this);

        auto connect = [&](auto* type) {
            using T = std::remove_pointer_t<decltype(type)>;
            m_Registry.on_construct<T>().template connect<&Scene::OnRenderComponentChanged>(this);
            m_Registry.on_update<T>().template connect<&Scene::OnRenderComponentChanged>(this);
            m_Registry.on_destroy<T>().template connect<&Scene::OnRenderComponentChanged>(this);
        };
        m_Registry.on_construct<ModelPoseComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<ModelPoseComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_destroy<ModelPoseComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<AnimatorComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_construct<ActiveComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<ActiveComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_destroy<ActiveComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_construct<IKComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<IKComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_destroy<IKComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_construct<ShapeKeysComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<ShapeKeysComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_destroy<ShapeKeysComponent>().connect<&Scene::OnTransformChanged>(this);
        connect(static_cast<MeshComponent*>(nullptr));
        connect(static_cast<TextComponent*>(nullptr));
        connect(static_cast<UIScreenComponent*>(nullptr));
        connect(static_cast<UIWorldComponent*>(nullptr));
        m_Registry.on_construct<EditableMeshComponent>().connect<&Scene::OnEditableMeshChanged>(this);
        m_Registry.on_update<EditableMeshComponent>().connect<&Scene::OnEditableMeshChanged>(this);
        m_Registry.on_destroy<EditableMeshComponent>().connect<&Scene::OnEditableMeshRemoved>(this);
        connect(static_cast<SpriteRendererComponent*>(nullptr));
        connect(static_cast<DirLightComponent*>(nullptr));
        connect(static_cast<PointLightComponent*>(nullptr));
        connect(static_cast<PrefabOwnedComponent*>(nullptr));
        m_Registry.on_construct<TransformComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<TransformComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_destroy<TransformComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_update<RelationshipComponent>().connect<&Scene::OnTransformChanged>(this);
        m_Registry.on_construct<PrefabInstanceComponent>().connect<&Scene::OnPrefabChanged>(this);
        m_Registry.on_update<PrefabInstanceComponent>().connect<&Scene::OnPrefabChanged>(this);
        m_Registry.on_destroy<PrefabInstanceComponent>().connect<&Scene::OnPrefabChanged>(this);
    }

    Scene::~Scene()
    {
        m_PhysicsWorld.reset();
        if (ScriptEngine::GetSceneContext() == this)
            ScriptEngine::OnRuntimeEnd();
        StopAudio();
        m_SuppressNotifications = true;
        m_Registry.clear();
    }

    void Scene::SetEnvironmentSettings(EnvironmentSettings settings)
    {
        settings.Validate();
        m_EnvironmentSettings      = std::move(settings);
        m_HasEnvironmentSettings   = true;
        m_RenderWorld->Environment = m_EnvironmentSettings;
    }

    void Scene::ClearEnvironmentSettings()
    {
        m_EnvironmentSettings    = {};
        m_HasEnvironmentSettings = false;
        m_RenderWorld->Environment.reset();
    }

    void Scene::DirtyBounds(Entity entity)
    {
        for (auto current = entity; current; current = GetParent(current))
            m_DirtyBounds.insert(current);
    }
    void Scene::DirtySubtree(Entity entity)
    {
        m_RenderWorld->Dirty(entity);
        if (!entity.HasComponent<RelationshipComponent>())
            return;
        DirtyBounds(entity);
        for (auto child : GetChildren(entity))
            DirtySubtree(child);
    }
    void Scene::OnTransformChanged(entt::registry&, entt::entity handle)
    {
        if (m_SuppressNotifications)
            return;
        Entity entity {handle, this};
        DirtySubtree(entity);
        if (!m_PhysicsWritingTransforms && !m_ReconcilingPrefab && entity.HasComponent<PrefabOwnedComponent>())
            m_CapturePrefabNodes.insert(handle);
    }
    void Scene::OnRenderComponentChanged(entt::registry&, entt::entity handle)
    {
        if (m_SuppressNotifications)
            return;
        m_RenderWorld->Dirty(handle);
        Entity entity {handle, this};
        if (entity.HasComponent<RelationshipComponent>())
            DirtyBounds(entity);
        if (!m_ReconcilingPrefab && entity.HasComponent<PrefabOwnedComponent>())
            m_CapturePrefabNodes.insert(handle);
    }
    void Scene::OnEditableMeshChanged(entt::registry& registry, entt::entity handle)
    {
        const auto  source    = m_EditableSources.find(handle);
        const auto* component = registry.try_get<EditableMeshComponent>(handle);
        if (!component || source == m_EditableSources.end() || !(component->Mesh == *source->second))
        {
            m_EditableMeshes.erase(handle);
            m_EditableSources.erase(handle);
        }
        OnRenderComponentChanged(registry, handle);
    }
    void Scene::OnEditableMeshRemoved(entt::registry& registry, entt::entity handle)
    {
        m_EditableMeshes.erase(handle);
        m_EditableSources.erase(handle);
        OnRenderComponentChanged(registry, handle);
    }
    Ref<const RuntimeMesh> Scene::ResolveEditableMesh(Entity entity) const
    {
        if (!entity.HasComponent<EditableMeshComponent>())
            return {};
        auto&       built = m_EditableMeshes[entt::entity(entity)];
        const auto& mesh  = entity.GetComponent<EditableMeshComponent>().Mesh;
        if (!built)
        {
            built                                   = CreateRef<RuntimeMesh>(Build(mesh));
            m_EditableSources[entt::entity(entity)] = std::make_shared<EditableMesh>(mesh);
            ++m_EditableMeshBuildCount;
        }
        return built;
    }
    void Scene::OnPrefabChanged(entt::registry&, entt::entity handle)
    {
        if (m_SuppressNotifications)
            return;
        m_DirtyPrefabs.insert(handle);
        if (m_Registry.all_of<IDComponent, RelationshipComponent>(handle))
            DirtySubtree({handle, this});
    }
    void Scene::SetPrefabAssetManager(AssetManager* assets)
    {
        if (m_PrefabAssets == assets)
            return;
        m_RenderWorld->m_UIRecords.clear();
        m_PrefabAssets = assets;
        m_AssetChanges = assets ? assets->ObserveChanges() : nullptr;
        if (m_PhysicsWorld)
            m_PhysicsWorld->NotifyAssetProviderChanged();
        for (auto handle : m_Registry.view<PrefabInstanceComponent>())
            m_DirtyPrefabs.insert(handle);
        for (auto handle : m_Registry.view<MeshComponent>())
            OnRenderComponentChanged(m_Registry, handle);
    }
    std::vector<AssetID> Scene::GetReferencedAssets() const
    {
        std::set<AssetID> ids;
        auto              add = [&](AssetID id) {
            if (id.IsValid())
                ids.insert(id);
        };
        for (auto handle : m_Registry.view<PrefabInstanceComponent>())
            for (const auto& [key, change] : m_Registry.get<PrefabInstanceComponent>(handle).Overrides)
            {
                if (change.UIScreen)
                    add(change.UIScreen->UI);
                if (change.UIWorld)
                    add(change.UIWorld->UI);
            }
        for (auto handle : m_Registry.view<UIScreenComponent>())
            add(m_Registry.get<UIScreenComponent>(handle).UI);
        for (auto handle : m_Registry.view<UIWorldComponent>())
            add(m_Registry.get<UIWorldComponent>(handle).UI);
        for (auto handle : m_Registry.view<SpriteRendererComponent>())
            add(m_Registry.get<SpriteRendererComponent>(handle).Texture);
        for (auto handle : m_Registry.view<TextComponent>())
            add(m_Registry.get<TextComponent>(handle).Font);
        for (auto handle : m_Registry.view<MeshComponent>())
        {
            const auto& c = m_Registry.get<MeshComponent>(handle);
            add(c.Mesh);
            for (auto id : c.Materials)
                add(id);
        }
        for (auto handle : m_Registry.view<EditableMeshComponent>())
            for (auto id : m_Registry.get<EditableMeshComponent>(handle).Materials)
                add(id);
        for (auto handle : m_Registry.view<PrefabInstanceComponent>())
        {
            const auto& c = m_Registry.get<PrefabInstanceComponent>(handle);
            add(c.Prefab);
            for (const auto& [key, o] : c.Overrides)
                for (const auto& [slot, id] : o.Materials)
                    add(id);
        }
        for (auto handle : m_Registry.view<AudioSourceComponent>())
            add(m_Registry.get<AudioSourceComponent>(handle).Clip);
        for (auto handle : m_Registry.view<AnimatorComponent>())
        {
            const auto& c = m_Registry.get<AnimatorComponent>(handle);
            add(c.Skeleton);
            add(c.Controller);
        }
        for (auto handle : m_Registry.view<IKComponent>())
            add(m_Registry.get<IKComponent>(handle).Skeleton);
        for (auto handle : m_Registry.view<ShapeKeysComponent>())
        {
            const auto& c = m_Registry.get<ShapeKeysComponent>(handle);
            add(c.Skeleton);
            add(c.Expression);
        }
        auto colliderMaterials = [&](auto tag) {
            using T = decltype(tag);
            for (auto h : m_Registry.view<T>())
                add(m_Registry.get<T>(h).Material.Asset);
        };
        colliderMaterials(BoxColliderComponent {});
        colliderMaterials(SphereColliderComponent {});
        colliderMaterials(CapsuleColliderComponent {});
        colliderMaterials(MeshColliderComponent {});
        for (auto h : m_Registry.view<MeshColliderComponent>())
            add(m_Registry.get<MeshColliderComponent>(h).Shape.Mesh);
        for (auto h : m_Registry.view<PrefabInstanceComponent>())
            for (const auto& [key, value] : m_Registry.get<PrefabInstanceComponent>(h).Overrides)
            {
                if (value.BoxCollider)
                    add(value.BoxCollider->Material.Asset);
                if (value.SphereCollider)
                    add(value.SphereCollider->Material.Asset);
                if (value.CapsuleCollider)
                    add(value.CapsuleCollider->Material.Asset);
                if (value.MeshCollider)
                {
                    add(value.MeshCollider->Shape.Mesh);
                    add(value.MeshCollider->Material.Asset);
                }
            }
        for (auto h : m_Registry.view<TimelinePlayerComponent>())
            add(m_Registry.get<TimelinePlayerComponent>(h).Timeline);
        for (const auto& entry : GetPostProcessSettings().Materials)
            add(entry.Material);
        return {ids.begin(), ids.end()};
    }
    void Scene::TrackPrefabAssets(Entity root)
    {
        auto& previous = m_PrefabDependencies[entt::entity(root)];
        for (auto id : previous)
        {
            auto found = m_PrefabAssetUsers.find(id);
            if (found != m_PrefabAssetUsers.end())
            {
                found->second.erase(root);
                if (found->second.empty())
                    m_PrefabAssetUsers.erase(found);
            }
        }
        previous.clear();
        const auto& instance = root.GetComponent<PrefabInstanceComponent>();
        if (instance.Prefab.IsValid())
            previous.push_back(instance.Prefab);
        if (instance.Source)
            for (const auto& node : instance.Source->GetEntities())
                if (node.Mesh.IsValid())
                    previous.push_back(node.Mesh);
        for (auto id : previous)
            m_PrefabAssetUsers[id].insert(root);
    }
    void Scene::PollAssetChanges()
    {
        if (!m_AssetChanges)
            return;
        for (auto id : m_AssetChanges->Drain())
        {
            m_RenderWorld->AssetChanged(id);
            if (m_PhysicsWorld)
                m_PhysicsWorld->NotifyAssetChanged(id);
            const auto found = m_PrefabAssetUsers.find(id);
            if (found != m_PrefabAssetUsers.end())
                for (auto handle : found->second)
                    m_DirtyPrefabs.insert(handle);
        }
    }
    Ref<const MeshAsset> Scene::ResolveMesh(Entity entity) const
    {
        if (entity.HasComponent<EditableMeshComponent>())
            return ResolveEditableMesh(entity)->Source;
        if (!entity.HasComponent<MeshComponent>())
            return {};
        const auto id = entity.GetComponent<MeshComponent>().Mesh;
        if (m_PrefabAssets)
        {
            auto status = m_PrefabAssets->Request<MeshAsset>(id);
            if (status.IsReady())
                return status.Asset;
        }
        if (auto root = GetPrefabInstanceRoot(entity))
        {
            const auto& sources = root.GetComponent<PrefabInstanceComponent>().MeshSources;
            auto        found   = sources.find(id);
            if (found != sources.end())
                return found->second;
        }
        const auto previous = m_RenderWorld->m_Records.find(entt::entity(entity));
        if (previous != m_RenderWorld->m_Records.end() && previous->second.Mesh &&
            previous->second.Mesh->Mesh.Mesh == id)
            return previous->second.MeshSource;
        return {};
    }

    void Scene::OnUpdate(TimeStep ts)
    {

        UpdatePrefabs();
        UpdateAnimations(double(ts));
        UpdateTimelines(double(ts));
        UpdatePhysics(double(ts));

        // update ScripteableEntity
        {

            m_Registry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc) {
                if (!IsEntityActive(Entity {entity, this}))
                    return;
                if (!nsc.Instance)
                {
                    nsc.Instance           = nsc.InstantiateFunction();
                    nsc.Instance->m_Entity = Entity {entity, this};
                    nsc.Instance->OnCreate();
                }

                nsc.Instance->OnUpdate(ts);
                nsc.Instance->OnDestory();
                nsc.DestoryInstanceFunction(&nsc);
            });
        }
        if (ScriptEngine::GetSceneContext() == this)
            ScriptEngine::OnUpdate(ts);
        UpdateAudio(double(ts));
    }

    void Scene::OnViewportResize(uint32_t width, uint32_t height)
    {

        m_ViewportWidth  = width;
        m_ViewportHeight = height;

        auto view = m_Registry.view<CameraComponent>();
        for (auto entity : view)
        {
            auto& cameraCMP = view.get<CameraComponent>(entity);
            if (!cameraCMP.FixedAspectRatio)
            {
                cameraCMP.SceneCamera.SetViewportSize(width, height);
            }
        }
    }

    Entity Scene::CreateEntity(const std::string name)
    {
        UUID id = UUID();
        return CreateEntityWithID(id, name, false);
    }
    Kans::Entity Scene::CreateEntityWithID(UUID uuid, const std::string& name, bool runtimeMap)
    {
        Entity entity = {m_Registry.create(), this};
        entity.AddComponent<TransformComponent>();
        entity.AddComponent<IDComponent>(uuid);
        entity.AddComponent<RelationshipComponent>();
        entity.AddComponent<BoundsComponent>();
        auto& tag = entity.AddComponent<TagComponent>();
        tag.Tag   = name.empty() ? "Entity" : name;

        CORE_ASSERT(m_EntityMap.find(uuid) == m_EntityMap.end(), nullptr);
        m_EntityMap[uuid] = entity;
        m_RootOrder.push_back(uuid);
        return entity;
    }
    void Scene::DestroyEntity(Entity entity)
    {
        if (!entity || !m_Registry.valid(entt::entity(entity)))
            return;
        if (m_PhysicsWorld && m_PhysicsWorld->DeferMutation([this, id = entity.GetUUID()] {
                if (auto e = GetEntityByUUID(id))
                    DestroyEntity(e);
            }))
            return;
        if (entity.HasComponent<TimelinePlayerComponent>())
        {
            auto player = entity.GetComponent<TimelinePlayerComponent>().Player;
            if (player)
                player->Stop();
        }
        if (!m_ReconcilingPrefab && entity.HasComponent<PrefabOwnedComponent>())
        {
            const auto owned = entity.GetComponent<PrefabOwnedComponent>();
            Entity     root  = GetEntityByUUID(owned.Instance);
            if (root && root.HasComponent<PrefabInstanceComponent>())
            {
                SetPrefabNodeRemoved(root, owned.NodeKey, true);
                UpdateBounds();
                return;
            }
        }
        const bool reconciling = m_ReconcilingPrefab;
        if (entity.HasComponent<PrefabInstanceComponent>())
            m_ReconcilingPrefab = true;
        // Children go with their parent.
        const auto children = entity.GetComponent<RelationshipComponent>().Children;
        for (UUID child : children)
            if (Entity value = GetEntityByUUID(child))
                DestroyEntity(value);
        if (ScriptEngine::GetSceneContext() == this)
            ScriptEngine::OnDestroyEntity(entity);
        DirtyBounds(entity);
        m_RenderWorld->Dirty(entity);
        m_DirtyPrefabs.erase(entity);
        m_CapturePrefabNodes.erase(entity);
        if (auto dependencies = m_PrefabDependencies.find(entt::entity(entity));
            dependencies != m_PrefabDependencies.end())
        {
            for (auto asset : dependencies->second)
                m_PrefabAssetUsers[asset].erase(entity);
            m_PrefabDependencies.erase(dependencies);
        }
        Detach(entity);
        auto& id = entity.GetComponent<IDComponent>().ID;
        if (m_PhysicsWorld && !m_SuppressNotifications)
            m_PhysicsWorld->NotifyTopologyChanged(entity);
        m_EntityMap.erase(id);
        m_ChildBounds.erase(entt::entity(entity));
        m_Registry.destroy(entity);
        m_ReconcilingPrefab = reconciling;
    }

    void Scene::Detach(Entity entity)
    {
        auto&      relationship = entity.GetComponent<RelationshipComponent>();
        const UUID id           = entity.GetUUID();
        Entity     parent       = relationship.Parent ? GetEntityByUUID(relationship.Parent) : Entity {};
        auto&      siblings     = parent ? parent.GetComponent<RelationshipComponent>().Children : m_RootOrder;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
        if (parent)
            m_ChildBounds[entt::entity(parent)].Set(entity, {});
        relationship.Parent = 0;
    }

    std::vector<Entity> Scene::GetRootEntities() const
    {
        std::vector<Entity> result;
        for (UUID id : m_RootOrder)
            if (Entity entity = GetEntityByUUID(id))
                result.push_back(entity);
        return result;
    }

    std::vector<Entity> Scene::GetChildren(Entity entity) const
    {
        std::vector<Entity> result;
        for (UUID id : entity.GetComponent<RelationshipComponent>().Children)
            if (Entity child = GetEntityByUUID(id))
                result.push_back(child);
        return result;
    }

    Entity Scene::GetParent(Entity entity) const
    {
        const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
        return parent ? GetEntityByUUID(parent) : Entity {};
    }

    bool Scene::IsDescendantOf(Entity entity, Entity ancestor) const
    {
        for (Entity parent = GetParent(entity); parent; parent = GetParent(parent))
            if (parent == ancestor)
                return true;
        return false;
    }

    bool Scene::SetParent(Entity entity, Entity parent, Entity before, bool keepWorld)
    {
        if (!m_ReconcilingPrefab && entity.HasComponent<PrefabOwnedComponent>())
            return false;
        if (parent && (parent == entity || IsDescendantOf(parent, entity)))
            return false;
        const glm::mat4 world = GetWorldTransform(entity);
        DirtyBounds(entity);
        Detach(entity);
        const UUID id       = entity.GetUUID();
        auto&      siblings = parent ? parent.GetComponent<RelationshipComponent>().Children : m_RootOrder;
        auto       at       = before ? std::find(siblings.begin(), siblings.end(), before.GetUUID()) : siblings.end();
        siblings.insert(at, id);
        entity.GetComponent<RelationshipComponent>().Parent = parent ? parent.GetUUID() : UUID(0);
        if (!m_ReconcilingPrefab && !entity.HasComponent<PrefabOwnedComponent>())
        {
            if (parent && parent.HasComponent<PrefabOwnedComponent>())
            {
                const auto& owned = parent.GetComponent<PrefabOwnedComponent>();
                if (!entity.HasComponent<PrefabAttachmentComponent>())
                    entity.AddComponent<PrefabAttachmentComponent>();
                entity.GetComponent<PrefabAttachmentComponent>() = {owned.Instance, owned.NodeKey};
            }
            else if (entity.HasComponent<PrefabAttachmentComponent>())
                entity.RemoveComponent<PrefabAttachmentComponent>();
        }
        if (keepWorld)
            SetWorldTransform(entity, world);
        m_Registry.patch<RelationshipComponent>(entity);
        return true;
    }

    bool Scene::SetParent(Entity entity, Entity parent) { return SetParent(entity, parent, {}, true); }

    glm::mat4 Scene::GetWorldTransform(Entity entity) const
    {
        const glm::mat4 local = entity.GetComponent<TransformComponent>().GetTransform();
        if (entity.HasComponent<PrefabOwnedComponent>())
        {
            const auto root     = GetPrefabInstanceRoot(entity);
            auto       relative = [&](auto&& self, Entity part) -> glm::mat4 {
                const auto parent = GetParent(part);
                const auto matrix = part.GetComponent<TransformComponent>().GetTransform();
                return parent && parent != root ? self(self, parent) * matrix : matrix;
            };
            // Preserve phase-2 multiplication grouping: rootWorld * modelSpace.
            return root ? GetWorldTransform(root) * relative(relative, entity) : local;
        }
        const Entity parent = GetParent(entity);
        return parent ? GetWorldTransform(parent) * local : local;
    }

    void Scene::UpdateMaterialParameterDrivers()
    {
        for (auto handle : m_Registry.view<MaterialParameterDriversComponent>())
            UpdateMaterialParameterDrivers(Entity {handle, this});
    }

    void Scene::UpdateMaterialParameterDrivers(Entity entity)
    {
        if (!entity || !entity.HasComponent<MaterialParameterDriversComponent>())
            return;
        auto& drivers = entity.GetComponent<MaterialParameterDriversComponent>();
        if (!drivers.Values)
            drivers.Values = CreateRef<InstanceMaterialParameters>();
        auto&  values = *drivers.Values;
        size_t count  = drivers.Bindings.size();
        for (const auto& entry : drivers.Overrides)
            if (std::none_of(drivers.Bindings.begin(), drivers.Bindings.end(), [&](const auto& binding) {
                    return binding.Parameter == entry.first;
                }))
                ++count;
        values.resize(count);
        for (size_t i = 0; i < drivers.Bindings.size(); ++i)
        {
            const auto& binding = drivers.Bindings[i];
            auto&       value   = values[i];
            if (value.Parameter != binding.Parameter)
                value.Parameter = binding.Parameter;
            const auto source     = GetEntityByUUID(binding.SourceEntity);
            value.RuntimeOverride = false;
            value.Resolved        = bool(source);
            if (!source)
                continue;
            const auto world = GetWorldTransform(source);
            if (binding.Quantity == MaterialParameterQuantity::WorldPosition)
                value.Value = {world[3].x, world[3].y, world[3].z, 1};
            else
            {
                const int   axis = binding.Quantity == MaterialParameterQuantity::WorldForward ? 2 :
                                   binding.Quantity == MaterialParameterQuantity::WorldUp      ? 1 :
                                                                                                 0;
                glm::vec3   direction(world[axis]);
                const float length = glm::length(direction);
                direction          = length > 1e-20f ? direction / length : glm::vec3(0);
                value.Value        = {direction.x, direction.y, direction.z, 0};
            }
        }
        size_t next = drivers.Bindings.size();
        for (const auto& [name, overrideValue] : drivers.Overrides)
        {
            bool found = false;
            for (size_t i = 0; i < drivers.Bindings.size(); ++i)
                if (auto& value = values[i]; value.Parameter == name)
                {
                    value.Value           = overrideValue;
                    value.Resolved        = true;
                    value.RuntimeOverride = true;
                    found                 = true;
                }
            if (!found)
            {
                auto& value = values[next++];
                if (value.Parameter != name)
                    value.Parameter = name;
                value.Value           = overrideValue;
                value.Resolved        = true;
                value.RuntimeOverride = true;
            }
        }
        for (auto it = drivers.SlotValues.begin(); it != drivers.SlotValues.end();)
        {
            if (!drivers.SlotOverrides.count(it->first))
                it = drivers.SlotValues.erase(it);
            else
                ++it;
        }
        for (const auto& [slot, overrides] : drivers.SlotOverrides)
        {
            auto& output = drivers.SlotValues[slot];
            if (!output)
                output = CreateRef<InstanceMaterialParameters>();
            *output = values;
            for (const auto& [name, v] : overrides)
            {
                auto found =
                    std::find_if(output->begin(), output->end(), [&](const auto& p) { return p.Parameter == name; });
                InstanceMaterialParameter parameter {name, v, true, true};
                if (found == output->end())
                    output->push_back(parameter);
                else
                    *found = parameter;
            }
        }
    }

    void Scene::SetWorldTransform(Entity entity, const glm::mat4& world)
    {
        const Entity    parent = GetParent(entity);
        const glm::mat4 local  = parent ? glm::inverse(GetWorldTransform(parent)) * world : world;
        entity.PatchComponent<TransformComponent>([&](auto& transform) {
            Math::DecomposeTransform(local, transform.Position, transform.Rotation, transform.Scale);
            // The existing matrix override is general local-transform storage despite
            // its historical Imported* names. It also preserves shear from inverse-parent
            // multiplication. Editing a TRS field invalidates the override in GetTransform().
            transform.ImportedMatrix   = local;
            transform.ImportedPosition = transform.Position;
            transform.ImportedRotation = transform.Rotation;
            transform.ImportedScale    = transform.Scale;
        });
    }

    namespace
    {
        template<typename T>
        void CopyComponent(Entity source, Entity target)
        {
            if (source.HasComponent<T>())
            {
                if (target.HasComponent<T>())
                    target.SetComponent<T>(source.GetComponent<T>());
                else
                    target.AddComponent<T>(source.GetComponent<T>());
            }
        }
    } // namespace

    Entity Scene::CopyEntity(Entity source, Entity parent, Entity before)
    {
        Entity copy = CreateEntity(source.GetComponent<TagComponent>().Tag);
        CopyComponent<TransformComponent>(source, copy);
        CopyComponent<CameraComponent>(source, copy);
        CopyComponent<TextComponent>(source, copy);
        CopyComponent<UIScreenComponent>(source, copy);
        CopyComponent<UIWorldComponent>(source, copy);
        CopyComponent<SpriteRendererComponent>(source, copy);
        CopyComponent<MeshComponent>(source, copy);
        CopyComponent<EditableMeshComponent>(source, copy);
        CopyComponent<AnimatorComponent>(source, copy);
        CopyComponent<IKComponent>(source, copy);
        CopyComponent<ShapeKeysComponent>(source, copy);
        CopyComponent<MaterialParameterDriversComponent>(source, copy);
        CopyComponent<TimelinePlayerComponent>(source, copy);
        CopyComponent<ActiveComponent>(source, copy);
        CopyComponent<RigidBodyComponent>(source, copy);
        CopyComponent<BoxColliderComponent>(source, copy);
        CopyComponent<SphereColliderComponent>(source, copy);
        CopyComponent<CapsuleColliderComponent>(source, copy);
        CopyComponent<MeshColliderComponent>(source, copy);
        CopyComponent<GlobalWindComponent>(source, copy);

        CopyComponent<AudioSourceComponent>(source, copy);
        CopyComponent<AudioListenerComponent>(source, copy);
        if (source.HasComponent<PrefabInstanceComponent>())
        {
            auto instance     = source.GetComponent<PrefabInstanceComponent>();
            instance.Expanded = false;
            instance.Nodes.clear();
            instance.DrawOrder = 0;
            copy.AddComponent<PrefabInstanceComponent>(std::move(instance));
        }
        CopyComponent<DirLightComponent>(source, copy);
        CopyComponent<PointLightComponent>(source, copy);
        CopyComponent<ScriptComponent>(source, copy);
        SetParent(copy, parent, before, false);
        if (copy.HasComponent<PrefabInstanceComponent>())
            ExpandPrefab(copy);
        for (Entity child : GetChildren(source))
            if (!child.HasComponent<PrefabOwnedComponent>())
                CopyEntity(child, copy, {});
        if (source.HasComponent<PrefabInstanceComponent>())
        {
            const auto& instance = source.GetComponent<PrefabInstanceComponent>();
            for (const auto& [key, state] : instance.Nodes)
                if (auto part = GetEntityByUUID(PrefabEntityUUID(source.GetUUID(), key)))
                    for (auto child : GetChildren(part))
                        if (!child.HasComponent<PrefabOwnedComponent>())
                            CopyEntity(child, GetEntityByUUID(PrefabEntityUUID(copy.GetUUID(), key)), {});
        }
        return copy;
    }

    Entity Scene::DuplicateEntity(Entity entity)
    {
        // Insert after the original: before its next sibling.
        const Entity parent   = GetParent(entity);
        const auto&  siblings = parent ? parent.GetComponent<RelationshipComponent>().Children : m_RootOrder;
        auto         at       = std::find(siblings.begin(), siblings.end(), entity.GetUUID());
        const Entity next = at != siblings.end() && at + 1 != siblings.end() ? GetEntityByUUID(*(at + 1)) : Entity {};
        auto         copy = CopyEntity(entity, parent, next);
        std::unordered_map<UUID, UUID> remap;
        auto                           pair = [&](auto&& self, Entity source, Entity target) -> void {
            remap.emplace(source.GetUUID(), target.GetUUID());
            const auto children = GetChildren(target);
            size_t     ordinary = 0;
            for (auto child : GetChildren(source))
            {
                Entity copied;
                if (child.HasComponent<PrefabOwnedComponent>())
                {
                    const auto& owner = child.GetComponent<PrefabOwnedComponent>();
                    auto        root  = remap.find(owner.Instance);
                    if (root != remap.end())
                        copied = GetEntityByUUID(PrefabEntityUUID(root->second, owner.NodeKey));
                }
                else
                {
                    while (ordinary < children.size() && children[ordinary].HasComponent<PrefabOwnedComponent>())
                        ++ordinary;
                    if (ordinary < children.size())
                        copied = children[ordinary++];
                }
                if (copied)
                    self(self, child, copied);
            }
        };
        pair(pair, entity, copy);
        for (const auto& [source, target] : remap)
            if (auto copied = GetEntityByUUID(target); copied.HasComponent<MaterialParameterDriversComponent>())
                for (auto& binding : copied.GetComponent<MaterialParameterDriversComponent>().Bindings)
                    if (auto found = remap.find(binding.SourceEntity); found != remap.end())
                        binding.SourceEntity = found->second;
        for (const auto& [source, target] : remap)
            if (auto copied = GetEntityByUUID(target); copied.HasComponent<TimelinePlayerComponent>())
                for (auto& [binding, id] : copied.GetComponent<TimelinePlayerComponent>().Bindings)
                    if (auto found = remap.find(id); found != remap.end())
                        id = found->second;
        for (const auto& [source, target] : remap)
            if (auto copied = GetEntityByUUID(target); copied.HasComponent<IKComponent>())
                copied.GetComponent<IKComponent>().RemapEntities([&](uint64_t id) {
                    auto found = remap.find(UUID(id));
                    return found == remap.end() ? id : uint64_t(found->second);
                });
        return copy;
    }

    Entity Scene::GetEntityByUUID(UUID uuid) const
    {
        if (m_EntityMap.find(uuid) != m_EntityMap.end())
        {
            return m_EntityMap.at(uuid);
        }
        return {};
    }

    void Scene::OnRuntimeStart()
    {
        if (m_PhysicsWorld)
            return;
        UpdatePrefabs();
        m_PhysicsAccumulator = 0;
        m_PhysicsPaused = m_PhysicsSingleStep = false;
        m_PhysicsWorld                        = std::make_unique<PhysicsWorld>(*this, GetPhysicsSettings());
        StartAudio();
        if (ScriptEngine::IsInitialized())
            ScriptEngine::OnRuntimeStart(this);
        std::vector<UUID> timelines;
        for (auto h : m_Registry.view<TimelinePlayerComponent>())
            timelines.push_back(m_Registry.get<IDComponent>(h).ID);
        for (auto id : timelines)
            if (auto e = GetEntityByUUID(id);
                e && e.GetComponent<TimelinePlayerComponent>().PlayOnStart && PrepareTimeline(e))
            {
                auto player = e.GetComponent<TimelinePlayerComponent>().Player;
                player->Play();
            }
    }

    void Scene::DestroyNativeScriptInstances()
    {
        m_Registry.view<NativeScriptComponent>().each([](auto& nsc) {
            if (nsc.Instance)
            {
                nsc.Instance->OnDestory();
                nsc.DestoryInstanceFunction(&nsc);
                nsc.Instance = nullptr;
            }
        });
    }

    void Scene::OnRuntimeStop()
    {
        m_PhysicsWorld.reset();
        m_PhysicsAccumulator = 0;
        m_PhysicsPaused = m_PhysicsSingleStep = false;
        GetDebugDraw().BeginFrame();
        std::vector<Ref<TimelinePlayer>> players;
        for (auto h : m_Registry.view<TimelinePlayerComponent>())
            if (auto p = m_Registry.get<TimelinePlayerComponent>(h).Player)
                players.push_back(p);
        for (auto& p : players)
            p->Stop();
        StopAudio();
        DestroyNativeScriptInstances();
        if (ScriptEngine::GetSceneContext() == this)
            ScriptEngine::OnRuntimeEnd();
    }

    Kans::Entity Scene::GetCameraEntity()
    {
        auto view = m_Registry.view<CameraComponent>();
        for (auto entity : view)
        {
            auto [cameraCMP] = view.get(entity);
            if (cameraCMP.Primary && IsEntityActive(Entity {entity, this}))
            {
                return {entity, this};
            }
        }
        return {};
    }

    // To use entt use the call back function we should implement the function advance
    template<typename T>
    void Scene::OnComponentAdd(Entity entity, T& component)
    {
        static_assert(sizeof(T) == 0);
    }

    template<>
    void Scene::OnComponentAdd<RigidBodyComponent>(Entity, RigidBodyComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<BoxColliderComponent>(Entity, BoxColliderComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<SphereColliderComponent>(Entity, SphereColliderComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<CapsuleColliderComponent>(Entity, CapsuleColliderComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<MeshColliderComponent>(Entity, MeshColliderComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<GlobalWindComponent>(Entity, GlobalWindComponent&)
    {}

    template<>
    void Scene::OnComponentAdd<AudioSourceComponent>(Entity, AudioSourceComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<AudioListenerComponent>(Entity, AudioListenerComponent&)
    {}

    template<>
    void Scene::OnComponentAdd<MaterialParameterDriversComponent>(Entity, MaterialParameterDriversComponent&)
    {}

    template<>
    void Scene::OnComponentAdd<IKComponent>(Entity, IKComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<ShapeKeysComponent>(Entity, ShapeKeysComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<TimelinePlayerComponent>(Entity, TimelinePlayerComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<ActiveComponent>(Entity entity, ActiveComponent&)
    {
        DirtySubtree(entity);
    }
    template<>
    void Scene::OnComponentAdd<ModelPoseComponent>(Entity, ModelPoseComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<AnimatorComponent>(Entity, AnimatorComponent&)
    {}

    template<>
    void Scene::OnComponentAdd<CameraComponent>(Entity entity, CameraComponent& component)
    {
        if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
            component.SceneCamera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
    }

    template<>
    void Scene::OnComponentAdd<TransformComponent>(Entity entity, TransformComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<UIScreenComponent>(Entity, UIScreenComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<UIWorldComponent>(Entity, UIWorldComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<TextComponent>(Entity, TextComponent&)
    {}
    template<>
    void Scene::OnComponentAdd<MeshComponent>(Entity entity, MeshComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<EditableMeshComponent>(Entity entity, EditableMeshComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<BoundsComponent>(Entity entity, BoundsComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<PrefabOwnedComponent>(Entity entity, PrefabOwnedComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<PrefabAttachmentComponent>(Entity entity, PrefabAttachmentComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<PrefabInstanceComponent>(Entity entity, PrefabInstanceComponent& component)
    {
        if (!component.DrawOrder)
            component.DrawOrder = m_NextDrawOrder++;
        else
            m_NextDrawOrder = std::max(m_NextDrawOrder, component.DrawOrder + 1);
    }
    template<>
    void Scene::OnComponentAdd<TagComponent>(Entity entity, TagComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<NativeScriptComponent>(Entity entity, NativeScriptComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<DirLightComponent>(Entity entity, DirLightComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<PointLightComponent>(Entity entity, PointLightComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<IDComponent>(Entity entity, IDComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<ScriptComponent>(Entity entity, ScriptComponent& component)
    {}
    template<>
    void Scene::OnComponentAdd<RelationshipComponent>(Entity entity, RelationshipComponent& component)
    {}
} // namespace Kans
