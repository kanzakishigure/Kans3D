#include "kspch.h"
#include <functional>
#include <set>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/Math/Math.h"
#include "Kans3D/Physics/ClothSolver.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include "Entity.h"

namespace Kans
{
    namespace
    {
        bool Same(const TransformComponent& a, const TransformComponent& b)
        {
            return a.GetTransform() == b.GetTransform();
        }
        bool SameOverrides(const std::map<std::string, PrefabNodeOverride>& a,
                           const std::map<std::string, PrefabNodeOverride>& b)
        {
            if (a.size() != b.size())
                return false;
            auto x = a.begin(), y = b.begin();
            for (; x != a.end(); ++x, ++y)
                if (x->first != y->first || x->second.Removed != y->second.Removed ||
                    x->second.Materials != y->second.Materials || x->second.UIScreen != y->second.UIScreen ||
                    x->second.UIWorld != y->second.UIWorld || x->second.RigidBody != y->second.RigidBody ||
                    x->second.BoxCollider != y->second.BoxCollider ||
                    x->second.SphereCollider != y->second.SphereCollider ||
                    x->second.CapsuleCollider != y->second.CapsuleCollider ||
                    x->second.MeshCollider != y->second.MeshCollider || x->second.GlobalWind != y->second.GlobalWind ||
                    x->second.ForceField != y->second.ForceField || x->second.Ragdoll != y->second.Ragdoll ||
                    x->second.Particles != y->second.Particles || x->second.Vehicle != y->second.Vehicle ||
                    x->second.CharacterController != y->second.CharacterController ||
                    x->second.Joint != y->second.Joint || x->second.SpringBone != y->second.SpringBone ||
                    x->second.Cloth != y->second.Cloth || x->second.MotionCollider != y->second.MotionCollider ||

                    x->second.RenderCustomDepth != y->second.RenderCustomDepth ||
                    x->second.CustomStencil != y->second.CustomStencil ||
                    x->second.LocalTransform.has_value() != y->second.LocalTransform.has_value() ||
                    (x->second.LocalTransform && !Same(*x->second.LocalTransform, *y->second.LocalTransform)))
                    return false;
            return true;
        }
        TransformComponent Transform(const std::array<float, 16>& values)
        {
            TransformComponent result;
            const auto         matrix = glm::transpose(glm::make_mat4(values.data()));
            Math::DecomposeTransform(matrix, result.Position, result.Rotation, result.Scale);
            result.ImportedMatrix   = matrix;
            result.ImportedPosition = result.Position;
            result.ImportedRotation = result.Rotation;
            result.ImportedScale    = result.Scale;
            return result;
        }
        AssetID Material(const std::vector<AssetID>& materials, size_t slot)
        {
            return slot < materials.size() ? materials[slot] : AssetID {};
        }
    } // namespace

    UUID Scene::PrefabEntityUUID(UUID instance, const std::string& key)
    {
        // FNV-1a, with explicit little-endian root bytes and a domain separator.
        uint64_t value = 14695981039346656037ull;
        auto     byte  = [&](uint8_t b) { value = (value ^ b) * 1099511628211ull; };
        for (auto c : std::string("Kans.PrefabEntity.v1"))
            byte(uint8_t(c));
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(uint8_t(uint64_t(instance) >> shift));
        for (unsigned char c : key)
            byte(c);
        return UUID(value ? value : 1);
    }

    Entity Scene::InstantiatePrefab(AssetID prefab, const glm::vec3& position, UUID parent)
    {
        Entity root = CreateEntity("Prefab");
        root.PatchComponent<TransformComponent>([&](auto& transform) { transform.Position = position; });
        SetParent(root, GetEntityByUUID(parent), {}, false);
        root.AddComponent<PrefabInstanceComponent>().Prefab = prefab;
        return root;
    }

    Entity Scene::GetPrefabInstanceRoot(Entity entity) const
    {
        if (!entity)
            return {};
        if (entity.HasComponent<PrefabOwnedComponent>())
            return GetEntityByUUID(entity.GetComponent<PrefabOwnedComponent>().Instance);
        for (Entity current = entity; current; current = GetParent(current))
            if (current.HasComponent<PrefabInstanceComponent>())
                return current;
        return {};
    }

    void Scene::CapturePrefabOverrides()
    {
        auto dirty = std::move(m_CapturePrefabNodes);
        m_CapturePrefabNodes.clear();
        for (auto handle : dirty)
        {
            if (!m_Registry.valid(handle) || !m_Registry.all_of<PrefabOwnedComponent, TransformComponent>(handle))
                continue;
            ++m_RenderWorld->m_PendingStats.PrefabNodesVisited;
            Entity      part {handle, this};
            const auto& owned = part.GetComponent<PrefabOwnedComponent>();
            auto        root  = GetEntityByUUID(owned.Instance);
            if (!root || !root.HasComponent<PrefabInstanceComponent>())
                continue;
            auto& instance = root.GetComponent<PrefabInstanceComponent>();
            auto  found    = instance.Nodes.find(owned.NodeKey);
            if (found == instance.Nodes.end())
                continue;
            auto&       state     = found->second;
            const auto& transform = part.GetComponent<TransformComponent>();
            auto&       change    = instance.Overrides[owned.NodeKey];
            if (!Same(transform, state.AppliedTransform))
            {
                if (Same(transform, state.BaseTransform))
                    change.LocalTransform.reset();
                else
                    change.LocalTransform = transform;
                state.AppliedTransform = transform;
            }
            if (part.HasComponent<MeshComponent>())
            {
                const auto& mesh  = part.GetComponent<MeshComponent>();
                const auto  count = std::max(mesh.Materials.size(), state.AppliedMesh.Materials.size());
                for (size_t slot = 0; slot < count; ++slot)
                {
                    const auto value = Material(mesh.Materials, slot);
                    if (value == Material(state.AppliedMesh.Materials, slot))
                        continue;
                    if (!value.IsValid() || value == Material(state.BaseMesh.Materials, slot))
                        change.Materials.erase(uint32_t(slot));
                    else
                        change.Materials[uint32_t(slot)] = value;
                }
                if (mesh.RenderCustomDepth != state.AppliedMesh.RenderCustomDepth)
                    change.RenderCustomDepth = mesh.RenderCustomDepth == state.BaseMesh.RenderCustomDepth ?
                                                   std::optional<bool>() :
                                                   mesh.RenderCustomDepth;
                if (mesh.CustomStencil != state.AppliedMesh.CustomStencil)
                    change.CustomStencil = mesh.CustomStencil == state.BaseMesh.CustomStencil ?
                                               std::optional<uint8_t>() :
                                               mesh.CustomStencil;
                state.AppliedMesh = mesh;
            }
            change.RigidBody   = part.HasComponent<RigidBodyComponent>() ?
                                     std::optional<RigidBodyComponent>(part.GetComponent<RigidBodyComponent>()) :
                                     std::nullopt;
            change.BoxCollider = part.HasComponent<BoxColliderComponent>() ?
                                     std::optional<BoxColliderComponent>(part.GetComponent<BoxColliderComponent>()) :
                                     std::nullopt;
            change.SphereCollider =
                part.HasComponent<SphereColliderComponent>() ?
                    std::optional<SphereColliderComponent>(part.GetComponent<SphereColliderComponent>()) :
                    std::nullopt;
            change.CapsuleCollider =
                part.HasComponent<CapsuleColliderComponent>() ?
                    std::optional<CapsuleColliderComponent>(part.GetComponent<CapsuleColliderComponent>()) :
                    std::nullopt;
            change.MeshCollider = part.HasComponent<MeshColliderComponent>() ?
                                      std::optional<MeshColliderComponent>(part.GetComponent<MeshColliderComponent>()) :
                                      std::nullopt;
            change.CharacterController =
                part.HasComponent<CharacterControllerComponent>() ?
                    std::optional<CharacterControllerComponent>(part.GetComponent<CharacterControllerComponent>()) :
                    std::nullopt;
            change.Joint      = part.HasComponent<JointComponent>() ?
                                    std::optional<JointComponent>(part.GetComponent<JointComponent>()) :
                                    std::nullopt;
            change.GlobalWind = part.HasComponent<WindZoneComponent>() ?
                                    std::optional<WindZoneComponent>(part.GetComponent<WindZoneComponent>()) :
                                    std::nullopt;
            change.ForceField = part.HasComponent<ForceFieldComponent>() ?
                                    std::optional<ForceFieldComponent>(part.GetComponent<ForceFieldComponent>()) :
                                    std::nullopt;
            change.Ragdoll    = part.HasComponent<RagdollComponent>() ?
                                    std::optional<RagdollComponent>(part.GetComponent<RagdollComponent>()) :
                                    std::nullopt;
            change.Particles =
                part.HasComponent<ParticleSystemComponent>() ?
                    std::optional<ParticleSystemComponent>(part.GetComponent<ParticleSystemComponent>()) :
                    std::nullopt;
            change.Vehicle    = part.HasComponent<VehicleComponent>() ?
                                    std::optional<VehicleComponent>(part.GetComponent<VehicleComponent>()) :
                                    std::nullopt;
            change.MotionCollider =
                part.HasComponent<MotionColliderComponent>() ?
                    std::optional<MotionColliderComponent>(part.GetComponent<MotionColliderComponent>()) :
                    std::nullopt;
            change.SpringBone = part.HasComponent<SpringBoneComponent>() ?
                                    std::optional<SpringBoneComponent>(part.GetComponent<SpringBoneComponent>()) :
                                    std::nullopt;
            change.Cloth      = part.HasComponent<ClothComponent>() ?
                                    std::optional<ClothComponent>(part.GetComponent<ClothComponent>()) :
                                    std::nullopt;
            change.UIScreen   = part.HasComponent<UIScreenComponent>() ?
                                    std::optional<UIScreenComponent>(part.GetComponent<UIScreenComponent>()) :
                                    std::nullopt;
            change.UIWorld    = part.HasComponent<UIWorldComponent>() ?
                                    std::optional<UIWorldComponent>(part.GetComponent<UIWorldComponent>()) :
                                    std::nullopt;
            if (change.Empty())
            {
                instance.Overrides.erase(owned.NodeKey);
                instance.AppliedOverrides.erase(owned.NodeKey);
            }
            else
                instance.AppliedOverrides[owned.NodeKey] = change;
        }
    }

    void Scene::ExpandPrefab(Entity root)
    {
        auto& instance = root.GetComponent<PrefabInstanceComponent>();
        if (!instance.Source)
            return;
        if (instance.Source->GetMeshData().Skeleton.IsValid() && !root.HasComponent<AnimatorComponent>() &&
            !root.HasComponent<IKComponent>() && !root.HasComponent<ShapeKeysComponent>())
        {
            root.AddComponent<AnimatorComponent>().Skeleton = instance.Source->GetMeshData().Skeleton;
            instance.GeneratedAnimatorSkeleton              = instance.Source->GetMeshData().Skeleton;
        }
        const auto& nodes = instance.Source->GetEntities();
        // Require all CPU meshes before migration: a missing material table must not
        // silently discard a pending old-part override.
        bool meshChanged = false;
        auto meshSources = instance.MeshSources;
        for (const auto& node : nodes)
            if (node.Mesh.IsValid())
            {
                if (m_PrefabAssets)
                {
                    auto status = m_PrefabAssets->Request<MeshAsset>(node.Mesh);
                    if (!status.IsReady())
                        return;
                    auto previous = instance.MeshSources.find(node.Mesh);
                    meshChanged |= previous == instance.MeshSources.end() || previous->second != status.Asset;
                    meshSources[node.Mesh] = status.Asset;
                }
                if (!meshSources.count(node.Mesh))
                    return;
            }
        instance.MeshSources = std::move(meshSources);
        for (auto it = instance.Overrides.begin(); it != instance.Overrides.end();)
            if (it->second.Empty())
                it = instance.Overrides.erase(it);
            else
                ++it;
        if (instance.Expanded && instance.Source == instance.ExpandedSource && !meshChanged &&
            !instance.LegacyPending && SameOverrides(instance.Overrides, instance.AppliedOverrides))
            return;
        std::set<UUID>    desired;
        std::vector<bool> removed(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& node   = nodes[i];
            auto        change = instance.Overrides.find(node.Key);
            removed[i]         = (change != instance.Overrides.end() && change->second.Removed) ||
                                 (node.Parent >= 0 && removed.at(node.Parent));
            if (!removed[i])
                desired.insert(PrefabEntityUUID(root.GetUUID(), node.Key));
        }
        // Check collisions before mutating anything. Never adopt an ordinary entity.
        for (auto id : desired)
            if (auto existing = GetEntityByUUID(id))
                if (!existing.HasComponent<PrefabOwnedComponent>() ||
                    existing.GetComponent<PrefabOwnedComponent>().Instance != root.GetUUID())
                {
                    CORE_ERROR("Prefab UUID collides with an existing entity: {0}", uint64_t(id));
                    return;
                }
        m_ReconcilingPrefab = true;
        std::vector<Entity> stale;
        for (auto handle : m_Registry.view<PrefabOwnedComponent>())
        {
            Entity entity {handle, this};
            if (entity.GetComponent<PrefabOwnedComponent>().Instance == root.GetUUID() &&
                !desired.count(entity.GetUUID()))
                stale.push_back(entity);
        }
        // Detach all stale nodes first so destruction cannot take retained nodes or
        // ordinary user additions with it. Attachments retain their original key.
        for (Entity entity : stale)
        {
            for (Entity child : GetChildren(entity))
            {
                if (!child.HasComponent<PrefabOwnedComponent>() && !child.HasComponent<PrefabAttachmentComponent>())
                {
                    const auto& owned = entity.GetComponent<PrefabOwnedComponent>();
                    child.AddComponent<PrefabAttachmentComponent>(owned.Instance, owned.NodeKey);
                }
                SetParent(child, root, {}, false);
            }
            SetParent(entity, root, {}, false);
        }
        for (Entity entity : stale)
            DestroyEntity(entity);
        std::map<std::string, PrefabNodeState> states;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            ++m_RenderWorld->m_PendingStats.PrefabNodesVisited;
            const auto& node = nodes[i];
            if (removed[i])
                continue;
            const auto id     = PrefabEntityUUID(root.GetUUID(), node.Key);
            Entity     entity = GetEntityByUUID(id);
            if (!entity)
            {
                entity = CreateEntityWithID(id, node.Name, false);
                entity.AddComponent<PrefabOwnedComponent>(root.GetUUID(), node.Key, i);
            }
            entity.PatchComponent<PrefabOwnedComponent>([&](auto& owned) { owned.Order = i; });
            Entity parent =
                node.Parent < 0 ? root : GetEntityByUUID(PrefabEntityUUID(root.GetUUID(), nodes.at(node.Parent).Key));
            SetParent(entity, parent, {}, false);
            auto& state         = states[node.Key];
            state.BaseTransform = Transform(node.LocalTransform);
            MeshComponent mesh {node.Mesh, node.Materials, node.Section};
            if (node.Mesh.IsValid())
            {
                const auto& data = instance.MeshSources.at(node.Mesh)->GetData();
                mesh.Materials.resize(std::max(mesh.Materials.size(), data.MaterialSlots.size()));
                for (size_t slot = 0; slot < data.MaterialSlots.size(); ++slot)
                    if (!mesh.Materials[slot].IsValid())
                        mesh.Materials[slot] = data.MaterialSlots[slot].DefaultMaterial;
                // Old precedence: part -> mesh -> authored slot -> bound material.
                if (instance.LegacyPending)
                    for (size_t section = 0; section < data.Sections.size(); ++section)
                    {
                        if (node.Section && *node.Section != section)
                            continue;
                        const auto  slot     = data.Sections[section].MaterialSlot;
                        const auto& material = data.MaterialSlots.at(slot);
                        AssetID     oldPart;
                        for (const auto& identity : instance.Source->GetMeshData().SectionIdentities)
                            if (identity.Mesh == node.Mesh && identity.Section == section)
                                oldPart = identity.OldMesh;
                        const auto chosen = instance.LegacyPending->ResolveMaterial(
                            node.Mesh, material.DefaultMaterial, material.AuthoredMaterial, oldPart);
                        if (chosen != mesh.Materials[slot])
                            instance.Overrides[node.Key].Materials[slot] = chosen;
                        else
                            instance.Overrides[node.Key].Materials.erase(slot);
                    }
            }
            state.BaseMesh = mesh;
            auto transform = state.BaseTransform;
            auto change    = instance.Overrides.find(node.Key);
            if (change != instance.Overrides.end())
            {
                if (change->second.RenderCustomDepth)
                    mesh.RenderCustomDepth = *change->second.RenderCustomDepth;
                if (change->second.CustomStencil)
                    mesh.CustomStencil = *change->second.CustomStencil;
                if (change->second.LocalTransform)
                    transform = *change->second.LocalTransform;
                for (const auto& [slot, material] : change->second.Materials)
                {
                    if (slot >= mesh.Materials.size())
                        mesh.Materials.resize(slot + 1);
                    mesh.Materials[slot] = material;
                }
            }
            if (change != instance.Overrides.end() && change->second.RigidBody)
            {
                if (!entity.HasComponent<RigidBodyComponent>())
                    entity.AddComponent<RigidBodyComponent>(*change->second.RigidBody);
                else if (entity.GetComponent<RigidBodyComponent>() != *change->second.RigidBody)
                    entity.SetComponent<RigidBodyComponent>(*change->second.RigidBody);
            }
            else if (entity.HasComponent<RigidBodyComponent>())
                entity.RemoveComponent<RigidBodyComponent>();
            if (change != instance.Overrides.end() && change->second.BoxCollider)
            {
                if (!entity.HasComponent<BoxColliderComponent>())
                    entity.AddComponent<BoxColliderComponent>(*change->second.BoxCollider);
                else if (entity.GetComponent<BoxColliderComponent>() != *change->second.BoxCollider)
                    entity.SetComponent<BoxColliderComponent>(*change->second.BoxCollider);
            }
            else if (entity.HasComponent<BoxColliderComponent>())
                entity.RemoveComponent<BoxColliderComponent>();
            if (change != instance.Overrides.end() && change->second.SphereCollider)
            {
                if (!entity.HasComponent<SphereColliderComponent>())
                    entity.AddComponent<SphereColliderComponent>(*change->second.SphereCollider);
                else if (entity.GetComponent<SphereColliderComponent>() != *change->second.SphereCollider)
                    entity.SetComponent<SphereColliderComponent>(*change->second.SphereCollider);
            }
            else if (entity.HasComponent<SphereColliderComponent>())
                entity.RemoveComponent<SphereColliderComponent>();
            if (change != instance.Overrides.end() && change->second.CapsuleCollider)
            {
                if (!entity.HasComponent<CapsuleColliderComponent>())
                    entity.AddComponent<CapsuleColliderComponent>(*change->second.CapsuleCollider);
                else if (entity.GetComponent<CapsuleColliderComponent>() != *change->second.CapsuleCollider)
                    entity.SetComponent<CapsuleColliderComponent>(*change->second.CapsuleCollider);
            }
            else if (entity.HasComponent<CapsuleColliderComponent>())
                entity.RemoveComponent<CapsuleColliderComponent>();
            if (change != instance.Overrides.end() && change->second.MeshCollider)
            {
                if (!entity.HasComponent<MeshColliderComponent>())
                    entity.AddComponent<MeshColliderComponent>(*change->second.MeshCollider);
                else if (entity.GetComponent<MeshColliderComponent>() != *change->second.MeshCollider)
                    entity.SetComponent<MeshColliderComponent>(*change->second.MeshCollider);
            }
            else if (entity.HasComponent<MeshColliderComponent>())
                entity.RemoveComponent<MeshColliderComponent>();
            if (change != instance.Overrides.end() && change->second.CharacterController)
            {
                if (!entity.HasComponent<CharacterControllerComponent>())
                    entity.AddComponent<CharacterControllerComponent>(*change->second.CharacterController);
                else if (entity.GetComponent<CharacterControllerComponent>() != *change->second.CharacterController)
                    entity.SetComponent<CharacterControllerComponent>(*change->second.CharacterController);
            }
            else if (entity.HasComponent<CharacterControllerComponent>())
                entity.RemoveComponent<CharacterControllerComponent>();
            if (change != instance.Overrides.end() && change->second.Joint)
            {
                if (!entity.HasComponent<JointComponent>())
                    entity.AddComponent<JointComponent>(*change->second.Joint);
                else if (entity.GetComponent<JointComponent>() != *change->second.Joint)
                    entity.SetComponent<JointComponent>(*change->second.Joint);
            }
            else if (entity.HasComponent<JointComponent>())
                entity.RemoveComponent<JointComponent>();
            if (change != instance.Overrides.end() && change->second.GlobalWind)
            {
                if (!entity.HasComponent<WindZoneComponent>())
                    entity.AddComponent<WindZoneComponent>(*change->second.GlobalWind);
                else if (entity.GetComponent<WindZoneComponent>() != *change->second.GlobalWind)
                    entity.SetComponent<WindZoneComponent>(*change->second.GlobalWind);
            }
            else if (entity.HasComponent<WindZoneComponent>())
                entity.RemoveComponent<WindZoneComponent>();
            if (change != instance.Overrides.end() && change->second.ForceField)
            {
                if (!entity.HasComponent<ForceFieldComponent>())
                    entity.AddComponent<ForceFieldComponent>(*change->second.ForceField);
                else if (entity.GetComponent<ForceFieldComponent>() != *change->second.ForceField)
                    entity.SetComponent<ForceFieldComponent>(*change->second.ForceField);
            }
            else if (entity.HasComponent<ForceFieldComponent>())
                entity.RemoveComponent<ForceFieldComponent>();
            if (change != instance.Overrides.end() && change->second.Ragdoll)
            {
                if (!entity.HasComponent<RagdollComponent>())
                    entity.AddComponent<RagdollComponent>(*change->second.Ragdoll);
                else if (entity.GetComponent<RagdollComponent>() != *change->second.Ragdoll)
                    entity.SetComponent<RagdollComponent>(*change->second.Ragdoll);
            }
            else if (entity.HasComponent<RagdollComponent>())
                entity.RemoveComponent<RagdollComponent>();
            if (change != instance.Overrides.end() && change->second.Particles)
            {
                if (!entity.HasComponent<ParticleSystemComponent>())
                    entity.AddComponent<ParticleSystemComponent>(*change->second.Particles);
                else if (entity.GetComponent<ParticleSystemComponent>() != *change->second.Particles)
                    entity.SetComponent<ParticleSystemComponent>(*change->second.Particles);
            }
            else if (entity.HasComponent<ParticleSystemComponent>())
                entity.RemoveComponent<ParticleSystemComponent>();
            if (change != instance.Overrides.end() && change->second.Vehicle)
            {
                if (!entity.HasComponent<VehicleComponent>())
                    entity.AddComponent<VehicleComponent>(*change->second.Vehicle);
                else if (entity.GetComponent<VehicleComponent>() != *change->second.Vehicle)
                    entity.SetComponent<VehicleComponent>(*change->second.Vehicle);
            }
            else if (entity.HasComponent<VehicleComponent>())
                entity.RemoveComponent<VehicleComponent>();
            if (change != instance.Overrides.end() && change->second.SpringBone)
            {
                if (!entity.HasComponent<SpringBoneComponent>())
                    entity.AddComponent<SpringBoneComponent>(*change->second.SpringBone);
                else if (entity.GetComponent<SpringBoneComponent>() != *change->second.SpringBone)
                    entity.SetComponent<SpringBoneComponent>(*change->second.SpringBone);
            }
            else if (entity.HasComponent<SpringBoneComponent>())
                entity.RemoveComponent<SpringBoneComponent>();
            if (change != instance.Overrides.end() && change->second.Cloth)
            {
                if (!entity.HasComponent<ClothComponent>())
                    entity.AddComponent<ClothComponent>(*change->second.Cloth);
                else if (entity.GetComponent<ClothComponent>() != *change->second.Cloth)
                    entity.SetComponent<ClothComponent>(*change->second.Cloth);
            }
            else if (entity.HasComponent<ClothComponent>())
                entity.RemoveComponent<ClothComponent>();
            if (change != instance.Overrides.end() && change->second.MotionCollider)
            {
                if (!entity.HasComponent<MotionColliderComponent>())
                    entity.AddComponent<MotionColliderComponent>(*change->second.MotionCollider);
                else if (entity.GetComponent<MotionColliderComponent>() != *change->second.MotionCollider)
                    entity.SetComponent<MotionColliderComponent>(*change->second.MotionCollider);
            }
            else if (entity.HasComponent<MotionColliderComponent>())
                entity.RemoveComponent<MotionColliderComponent>();
            if (change != instance.Overrides.end() && change->second.UIScreen)
            {
                if (!entity.HasComponent<UIScreenComponent>())
                    entity.AddComponent<UIScreenComponent>();
                entity.SetComponent<UIScreenComponent>(*change->second.UIScreen);
            }
            else if (entity.HasComponent<UIScreenComponent>())
                entity.RemoveComponent<UIScreenComponent>();
            if (change != instance.Overrides.end() && change->second.UIWorld)
            {
                if (!entity.HasComponent<UIWorldComponent>())
                    entity.AddComponent<UIWorldComponent>();
                entity.SetComponent<UIWorldComponent>(*change->second.UIWorld);
            }
            else if (entity.HasComponent<UIWorldComponent>())
                entity.RemoveComponent<UIWorldComponent>();
            entity.SetComponent<TransformComponent>(transform);
            if (node.Mesh.IsValid())
            {
                if (!entity.HasComponent<MeshComponent>())
                    entity.AddComponent<MeshComponent>();
                entity.SetComponent<MeshComponent>(mesh);
            }
            else if (entity.HasComponent<MeshComponent>())
                entity.RemoveComponent<MeshComponent>();
            state.AppliedTransform = transform;
            state.AppliedMesh      = mesh;
        }
        instance.Nodes = std::move(states);
        instance.LegacyPending.reset();
        for (auto it = instance.Overrides.begin(); it != instance.Overrides.end();)
            if (it->second.Empty())
                it = instance.Overrides.erase(it);
            else
                ++it;
        instance.AppliedOverrides = instance.Overrides;
        instance.ExpandedSource   = instance.Source;
        instance.Expanded         = true;
        m_ReconcilingPrefab       = false;
        // Reattach ordinary children only when their owning instance reconciles.
        for (auto child : GetChildren(root))
            if (child.HasComponent<PrefabAttachmentComponent>())
            {
                const auto attachment = child.GetComponent<PrefabAttachmentComponent>();
                if (Entity parent = GetEntityByUUID(PrefabEntityUUID(attachment.Instance, attachment.NodeKey)))
                    if (GetParent(child) != parent)
                        SetParent(child, parent, {}, false);
            }
    }

    void Scene::UpdatePrefabs()
    {
        PollAssetChanges();
        CapturePrefabOverrides();
        auto dirty = std::move(m_DirtyPrefabs);
        m_DirtyPrefabs.clear();
        for (auto handle : dirty)
        {
            if (!m_Registry.valid(handle) || !m_Registry.all_of<PrefabInstanceComponent>(handle))
                continue;
            ++m_RenderWorld->m_PendingStats.PrefabsVisited;
            Entity root {handle, this};
            auto&  instance = root.GetComponent<PrefabInstanceComponent>();
            if (m_PrefabAssets && instance.Prefab.IsValid())
            {
                auto status = m_PrefabAssets->Request<PrefabAsset>(instance.Prefab);
                if (status.IsReady())
                    instance.Source = status.Asset;
            }
            TrackPrefabAssets(root);
            ExpandPrefab(root);
        }
        for (auto handle : m_Registry.view<AnimatorComponent>())
            PrepareAnimator({handle, this});
        UpdateModelPoses();
        UpdateBounds();
    }

    std::vector<Entity> Scene::GetMeshEntities() const { return GetMeshEntities({}); }
    std::vector<Entity> Scene::GetMeshEntities(Entity subtree) const
    {
        std::vector<Entity> result;
        for (auto handle : m_Registry.view<TransformComponent>())
        {
            if (!m_Registry.any_of<MeshComponent, EditableMeshComponent>(handle))
                continue;
            Entity entity {handle, const_cast<Scene*>(this)};
            if (!subtree || entity == subtree || IsDescendantOf(entity, subtree))
                result.push_back(entity);
        }
        std::sort(result.begin(), result.end(), [&](Entity a, Entity b) {
            const Entity   ar = GetPrefabInstanceRoot(a), br = GetPrefabInstanceRoot(b);
            const uint64_t ao = ar ? ar.GetComponent<PrefabInstanceComponent>().DrawOrder : uint64_t(uint32_t(a));
            const uint64_t bo = br ? br.GetComponent<PrefabInstanceComponent>().DrawOrder : uint64_t(uint32_t(b));
            if (ar != br || !ar)
                return ao != bo ? ao > bo : a.GetUUID() < b.GetUUID();
            const auto ap =
                a.HasComponent<PrefabOwnedComponent>() ? a.GetComponent<PrefabOwnedComponent>().Order : SIZE_MAX;
            const auto bp =
                b.HasComponent<PrefabOwnedComponent>() ? b.GetComponent<PrefabOwnedComponent>().Order : SIZE_MAX;
            return ap != bp ? ap < bp : a.GetUUID() < b.GetUUID();
        });
        return result;
    }

    void Scene::SetPrefabTransformOverride(Entity root, const std::string& key, const TransformComponent& transform)
    {
        CapturePrefabOverrides();
        auto&      instance = root.GetComponent<PrefabInstanceComponent>();
        const auto state    = instance.Nodes.find(key);
        if (state != instance.Nodes.end() && Same(state->second.BaseTransform, transform))
            instance.Overrides[key].LocalTransform.reset();
        else
            instance.Overrides[key].LocalTransform = transform;
        if (auto part = GetEntityByUUID(PrefabEntityUUID(root.GetUUID(), key)); part && state != instance.Nodes.end())
        {
            part.SetComponent<TransformComponent>(transform);
            CapturePrefabOverrides();
        }
        else
            ExpandPrefab(root);
    }
    void Scene::SetPrefabMaterialOverride(Entity root, const std::string& key, uint32_t slot, AssetID material)
    {
        CapturePrefabOverrides();
        auto&      instance = root.GetComponent<PrefabInstanceComponent>();
        const auto state    = instance.Nodes.find(key);
        const bool reset    = !material.IsValid() || (state != instance.Nodes.end() &&
                                                      material == Material(state->second.BaseMesh.Materials, slot));
        if (reset)
            instance.Overrides[key].Materials.erase(slot);
        else
            instance.Overrides[key].Materials[slot] = material;
        if (auto part = GetEntityByUUID(PrefabEntityUUID(root.GetUUID(), key));
            part && part.HasComponent<MeshComponent>() && state != instance.Nodes.end())
        {
            const auto value = reset ? Material(state->second.BaseMesh.Materials, slot) : material;
            part.PatchComponent<MeshComponent>([&](auto& mesh) {
                if (mesh.Materials.size() <= slot)
                    mesh.Materials.resize(slot + 1);
                mesh.Materials[slot] = value;
            });
            CapturePrefabOverrides();
        }
        else
            ExpandPrefab(root);
    }
    void Scene::SetPrefabNodeRemoved(Entity root, const std::string& key, bool removed)
    {
        CapturePrefabOverrides();
        root.GetComponent<PrefabInstanceComponent>().Overrides[key].Removed = removed;
        ExpandPrefab(root);
    }
    void Scene::ClearPrefabOverride(Entity root, const std::string& key)
    {
        CapturePrefabOverrides();
        auto&      instance   = root.GetComponent<PrefabInstanceComponent>();
        const auto previous   = instance.Overrides.find(key);
        const bool structural = previous != instance.Overrides.end() && previous->second.Removed;
        instance.Overrides.erase(key);
        const auto state = instance.Nodes.find(key);
        if (auto part = GetEntityByUUID(PrefabEntityUUID(root.GetUUID(), key));
            part && state != instance.Nodes.end() && !structural)
        {
            part.SetComponent<TransformComponent>(state->second.BaseTransform);
            if (part.HasComponent<MeshComponent>())
                part.SetComponent<MeshComponent>(state->second.BaseMesh);
            CapturePrefabOverrides();
        }
        else
            ExpandPrefab(root);
    }

    void Scene::ChildBounds::Set(entt::entity child, const Bounds& bounds)
    {
        if (auto found = Values.find(child); found != Values.end())
        {
            if (bounds.IsValid())
            {
                // Animation changes values, not topology. Reuse the six tree nodes and map entry.
                for (int axis = 0; axis < 3; ++axis)
                {
                    auto lo    = Min[axis].extract(Min[axis].find(found->second.Min[axis]));
                    auto hi    = Max[axis].extract(Max[axis].find(found->second.Max[axis]));
                    lo.value() = bounds.Min[axis];
                    hi.value() = bounds.Max[axis];
                    Min[axis].insert(std::move(lo));
                    Max[axis].insert(std::move(hi));
                }
                found->second = bounds;
                return;
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                Min[axis].erase(Min[axis].find(found->second.Min[axis]));
                Max[axis].erase(Max[axis].find(found->second.Max[axis]));
            }
            Values.erase(found);
        }
        if (bounds.IsValid())
        {
            Values.emplace(child, bounds);
            for (int axis = 0; axis < 3; ++axis)
            {
                Min[axis].insert(bounds.Min[axis]);
                Max[axis].insert(bounds.Max[axis]);
            }
        }
    }
    Bounds Scene::ChildBounds::Aggregate() const
    {
        Bounds result;
        if (!Values.empty())
            for (int axis = 0; axis < 3; ++axis)
            {
                result.Min[axis] = *Min[axis].begin();
                result.Max[axis] = *Max[axis].rbegin();
            }
        return result;
    }
    void Scene::UpdateBounds()
    {
        std::vector<Entity> dirty;
        for (auto handle : m_DirtyBounds)
            if (m_Registry.valid(handle) && m_Registry.all_of<RelationshipComponent, BoundsComponent>(handle))
                dirty.push_back({handle, this});
        m_DirtyBounds.clear();
        auto depth = [&](Entity e) {
            size_t n = 0;
            for (auto p = GetParent(e); p; p = GetParent(p))
                ++n;
            return n;
        };
        std::sort(dirty.begin(), dirty.end(), [&](Entity a, Entity b) { return depth(a) > depth(b); });
        for (auto entity : dirty)
        {
            ++m_RenderWorld->m_PendingStats.BoundsUpdated;
            Bounds bounds;
            if (auto mesh = ResolveMesh(entity))
            {
                const auto selected = entity.HasComponent<MeshComponent>() ?
                                          entity.GetComponent<MeshComponent>().Section :
                                          std::optional<uint32_t>();
                auto       world    = GetWorldTransform(entity);
                Bounds     animated;
                if (!mesh->GetData().MorphTargets.empty())
                    animated = DeformedBounds(mesh->GetData());
                const auto root = GetDeformationRoot(entity);
                if ((!mesh->GetData().Skin.empty() || !mesh->GetData().MorphTargets.empty()) && root)
                {
                    const auto skin = GetSkinningPose(entity);
                    if (skin)
                    {
                        animated = DeformedBounds(mesh->GetData(), skin.get());
                        if (!mesh->GetData().Skin.empty())
                            world = GetWorldTransform(root);
                    }
                }
                const auto& sections = mesh->GetData().Sections;
                for (size_t i = 0; i < sections.size(); ++i)
                    if (!selected || *selected == i)
                    {
                        const auto runtime = entity.HasComponent<ClothComponent>() ?
                                                 entity.GetComponent<ClothComponent>().Runtime :
                                                 Ref<ClothRuntime>();
                        if (runtime && runtime->Active && runtime->BindingValid && runtime->Stream &&
                            entity.GetComponent<ClothComponent>().Enabled &&
                            entity.GetComponent<ClothComponent>().Section == i)
                            bounds.Expand(runtime->Stream->Data.LocalBounds.Transformed(runtime->Root));
                        else
                            bounds.Expand((animated.IsValid() ? animated : sections[i].LocalBounds).Transformed(world));
                    }
            }
            if (entity.HasComponent<TextComponent>())
            {
                auto layout = m_RenderWorld->GetTextLayout(entity);
                bounds.Expand(TextWorldBounds(
                    entity.GetComponent<TextComponent>(), layout->LocalBounds(), GetWorldTransform(entity)));
            }
            if (auto children = m_ChildBounds.find(entt::entity(entity)); children != m_ChildBounds.end())
                bounds.Expand(children->second.Aggregate());
            entity.GetComponent<BoundsComponent>().World = bounds;
            if (auto parent = GetParent(entity))
                m_ChildBounds[entt::entity(parent)].Set(entity, bounds);
        }
    }
    Bounds Scene::GetPrefabBounds(AssetID prefab, const glm::mat4& transform)
    {
        Bounds bounds;
        if (!m_PrefabAssets)
            return bounds;
        const auto source = m_PrefabAssets->Request<PrefabAsset>(prefab);
        if (!source.IsReady())
            return bounds;
        const auto&            nodes = source.Asset->GetEntities();
        std::vector<glm::mat4> world(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& node  = nodes[i];
            const auto  local = glm::transpose(glm::make_mat4(node.LocalTransform.data()));
            world[i]          = node.Parent < 0 ? local : world.at(node.Parent) * local;
            if (!node.Mesh.IsValid())
                continue;
            auto mesh = m_PrefabAssets->Request<MeshAsset>(node.Mesh);
            if (!mesh.IsReady())
                return {};
            const auto& sections = mesh.Asset->GetData().Sections;
            for (size_t section = 0; section < sections.size(); ++section)
                if (!node.Section || *node.Section == section)
                    bounds.Expand(sections[section].LocalBounds.Transformed(
                        transform *
                        (mesh.Asset->GetData().Skin.empty() ? world[i] : mesh.Asset->GetData().SkinBindTransform)));
        }
        return bounds;
    }
    std::vector<MeshMaterialSlot> Scene::GetMeshSlots(Entity entity) const
    {
        const auto named = [](std::vector<MeshMaterialSlot> slots) {
            for (size_t i = 0; i < slots.size(); ++i)
                if (slots[i].Name.empty())
                    slots[i].Name = "Slot " + std::to_string(i);
            return slots;
        };
        if (entity && entity.HasComponent<EditableMeshComponent>())
            return named(ResolveMesh(entity)->GetData().MaterialSlots);
        if (!entity || !entity.HasComponent<MeshComponent>())
            return {};
        const auto id = entity.GetComponent<MeshComponent>().Mesh;
        if (m_PrefabAssets)
        {
            const auto mesh = m_PrefabAssets->Request<MeshAsset>(id);
            if (mesh.IsReady())
                return named(mesh.Asset->GetData().MaterialSlots);
        }
        if (auto root = GetPrefabInstanceRoot(entity))
        {
            const auto& sources = root.GetComponent<PrefabInstanceComponent>().MeshSources;
            auto        mesh    = sources.find(id);
            if (mesh != sources.end())
                return named(mesh->second->GetData().MaterialSlots);
        }
        return {};
    }
    Bounds Scene::GetWorldBounds(Entity entity) const
    {
        return entity && entity.HasComponent<BoundsComponent>() ? entity.GetComponent<BoundsComponent>().World :
                                                                  Bounds {};
    }
} // namespace Kans
