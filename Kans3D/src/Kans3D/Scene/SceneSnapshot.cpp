#include "kspch.h"
#include "SceneSnapshot.h"
#include <algorithm>
#include "Kans3D/Physics/PhysicsEquality.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "Entity.h"
#include "Scene.h"

namespace Kans
{
    namespace
    {
        bool Same(const TransformComponent& a, const TransformComponent& b)
        {
            return a.Position == b.Position && a.Rotation == b.Rotation && a.Scale == b.Scale &&
                   a.GetTransform() == b.GetTransform();
        }
        bool Same(const RelationshipComponent& a, const RelationshipComponent& b)
        {
            return a.Parent == b.Parent && a.Children == b.Children;
        }
        bool Same(const CameraComponent& a, const CameraComponent& b)
        {
            auto& x = const_cast<SceneCamera&>(a.SceneCamera);
            auto& y = const_cast<SceneCamera&>(b.SceneCamera);
            return a.Primary == b.Primary && a.FixedAspectRatio == b.FixedAspectRatio &&
                   x.GetProjectionType() == y.GetProjectionType() &&
                   x.GetPerspectiveVerticalFOV() == y.GetPerspectiveVerticalFOV() &&
                   x.GetPerspectiveNearClip() == y.GetPerspectiveNearClip() &&
                   x.GetPerspectiveFarClip() == y.GetPerspectiveFarClip() &&
                   x.GetOrthographicSize() == y.GetOrthographicSize() &&
                   x.GetOrthographicNearClip() == y.GetOrthographicNearClip() &&
                   x.GetOrthographicFarClip() == y.GetOrthographicFarClip() &&
                   x.GetPostProcessOverride() == y.GetPostProcessOverride() &&
                   x.GetAntiAliasing() == y.GetAntiAliasing() && x.GetUpscaling() == y.GetUpscaling() &&
                   // A free aspect ratio follows the viewport; resizing it is not an edit.
                   (!a.FixedAspectRatio || x.GetAspectRatio() == y.GetAspectRatio());
        }
        bool Same(const SpriteRendererComponent& a, const SpriteRendererComponent& b) { return a == b; }
        bool Same(const ProductModelComponent& a, const ProductModelComponent& b)
        {
            return a.Model == b.Model && a.Overrides.size() == b.Overrides.size() &&
                   std::equal(
                       a.Overrides.begin(), a.Overrides.end(), b.Overrides.begin(), [](const auto& x, const auto& y) {
                           return x.Authored == y.Authored && x.Replacement == y.Replacement;
                       });
        }
        bool Same(const UIScreenComponent& a, const UIScreenComponent& b) { return a == b; }
        bool Same(const UIWorldComponent& a, const UIWorldComponent& b) { return a == b; }
        bool Same(const TextComponent& a, const TextComponent& b) { return a == b; }
        bool Same(const MeshComponent& a, const MeshComponent& b) { return a == b; }
        bool Same(const EditableMeshComponent& a, const EditableMeshComponent& b) { return a == b; }
        bool Same(const PrefabAttachmentComponent& a, const PrefabAttachmentComponent& b)
        {
            return a.Instance == b.Instance && a.NodeKey == b.NodeKey;
        }
        bool Same(const PrefabInstanceComponent& a, const PrefabInstanceComponent& b)
        {
            if (a.Prefab != b.Prefab || a.DrawOrder != b.DrawOrder ||
                a.LegacyPending.has_value() != b.LegacyPending.has_value() ||
                (a.LegacyPending && !Same(*a.LegacyPending, *b.LegacyPending)) ||
                a.Overrides.size() != b.Overrides.size())
                return false;
            auto x = a.Overrides.begin(), y = b.Overrides.begin();
            for (; x != a.Overrides.end(); ++x, ++y)
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
        bool Same(const DirLightComponent& a, const DirLightComponent& b)
        {
            return a.Direction == b.Direction && a.Diffuse_Intensity == b.Diffuse_Intensity &&
                   a.Specular_Intensity == b.Specular_Intensity && a.Ambient_Intensity == b.Ambient_Intensity;
        }
        bool Same(const PointLightComponent& a, const PointLightComponent& b)
        {
            return a.Diffuse_Intensity == b.Diffuse_Intensity && a.Specular_Intensity == b.Specular_Intensity &&
                   a.Ambient_Intensity == b.Ambient_Intensity;
        }
        bool Same(const AudioSourceComponent& a, const AudioSourceComponent& b) { return a == b; }
        bool Same(const AudioListenerComponent& a, const AudioListenerComponent& b) { return a == b; }
        bool Same(const MaterialParameterDriversComponent& a, const MaterialParameterDriversComponent& b)
        {
            return a.Bindings.size() == b.Bindings.size() &&
                   std::equal(
                       a.Bindings.begin(), a.Bindings.end(), b.Bindings.begin(), [](const auto& x, const auto& y) {
                           return x.Parameter == y.Parameter && x.SourceEntity == y.SourceEntity &&
                                  x.Quantity == y.Quantity;
                       });
        }
        bool Same(const AnimatorComponent& a, const AnimatorComponent& b) { return a == b; }
        bool Same(const TimelinePlayerComponent& a, const TimelinePlayerComponent& b) { return a == b; }
        bool Same(const ActiveComponent& a, const ActiveComponent& b) { return a == b; }
        bool Same(const IKComponent& a, const IKComponent& b) { return a == b; }
        bool Same(const ShapeKeysComponent& a, const ShapeKeysComponent& b) { return a == b; }
        bool Same(const RigidBodyComponent& a, const RigidBodyComponent& b) { return a == b; }
        bool Same(const BoxColliderComponent& a, const BoxColliderComponent& b) { return a == b; }
        bool Same(const SphereColliderComponent& a, const SphereColliderComponent& b) { return a == b; }
        bool Same(const CapsuleColliderComponent& a, const CapsuleColliderComponent& b) { return a == b; }
        bool Same(const MeshColliderComponent& a, const MeshColliderComponent& b) { return a == b; }
        bool Same(const ForceFieldComponent& a, const ForceFieldComponent& b) { return a == b; }
        bool Same(const RagdollComponent& a, const RagdollComponent& b) { return a == b; }
        bool Same(const ParticleSystemComponent& a, const ParticleSystemComponent& b) { return a == b; }
        bool Same(const VehicleComponent& a, const VehicleComponent& b) { return a == b; }
        bool Same(const WindZoneComponent& a, const WindZoneComponent& b) { return a == b; }
        bool Same(const CharacterControllerComponent& a, const CharacterControllerComponent& b) { return a == b; }
        bool Same(const JointComponent& a, const JointComponent& b) { return a == b; }
        bool Same(const ClothComponent& a, const ClothComponent& b) { return a == b; }
        bool Same(const SpringBoneComponent& a, const SpringBoneComponent& b) { return a == b; }
        bool Same(const MotionColliderComponent& a, const MotionColliderComponent& b) { return a == b; }
        bool Same(const ScriptComponent& a, const ScriptComponent& b) { return a.ClassName == b.ClassName; }

        template<typename T>
        bool Same(const std::optional<T>& a, const std::optional<T>& b)
        {
            return a.has_value() == b.has_value() && (!a || Same(*a, *b));
        }

        template<typename T>
        void Capture(Entity entity, std::optional<T>& slot)
        {
            if (entity.HasComponent<T>())
                slot = entity.GetComponent<T>();
        }

        template<typename T>
        void Restore(Entity entity, const std::optional<T>& slot)
        {
            if (slot)
                entity.AddComponent<T>(*slot);
        }
    } // namespace

    const EntitySnapshot* SceneSnapshot::Find(UUID id) const
    {
        const auto found = std::lower_bound(
            Entities.begin(), Entities.end(), id, [](const EntitySnapshot& e, UUID value) { return e.ID < value; });
        return found != Entities.end() && found->ID == id ? &*found : nullptr;
    }

    bool operator==(const EntitySnapshot& a, const EntitySnapshot& b)
    {
        return a.ID == b.ID && a.Tag == b.Tag && Same(a.Transform, b.Transform) &&
               Same(a.Relationship, b.Relationship) && Same(a.Camera, b.Camera) && Same(a.Sprite, b.Sprite) &&
               Same(a.Text, b.Text) && Same(a.UIScreen, b.UIScreen) && Same(a.UIWorld, b.UIWorld) &&
               Same(a.Model, b.Model) && Same(a.Editable, b.Editable) && Same(a.Prefab, b.Prefab) &&
               Same(a.PrefabParent, b.PrefabParent) && Same(a.Animator, b.Animator) && Same(a.IK, b.IK) &&
               Same(a.Timeline, b.Timeline) && Same(a.Active, b.Active) && Same(a.ShapeKeys, b.ShapeKeys) &&
               Same(a.MaterialDrivers, b.MaterialDrivers) && Same(a.DirLight, b.DirLight) &&
               Same(a.PointLight, b.PointLight) && Same(a.Script, b.Script) && Same(a.AudioSource, b.AudioSource) &&
               Same(a.AudioListener, b.AudioListener) && Same(a.RigidBody, b.RigidBody) &&
               Same(a.BoxCollider, b.BoxCollider) && Same(a.SphereCollider, b.SphereCollider) &&
               Same(a.CapsuleCollider, b.CapsuleCollider) && Same(a.MeshCollider, b.MeshCollider) &&
               Same(a.Vehicle, b.Vehicle) && Same(a.Ragdoll, b.Ragdoll) && Same(a.Particles, b.Particles) &&
               Same(a.ForceField, b.ForceField) && Same(a.GlobalWind, b.GlobalWind) &&
               Same(a.CharacterController, b.CharacterController) && Same(a.Joint, b.Joint) &&
               Same(a.SpringBone, b.SpringBone) && Same(a.Cloth, b.Cloth) && Same(a.MotionCollider, b.MotionCollider);
    }

    bool operator==(const SceneSnapshot& a, const SceneSnapshot& b)
    {
        return a.PostProcess == b.PostProcess && a.Name == b.Name && a.Environment == b.Environment &&
               a.RootOrder == b.RootOrder && a.Entities == b.Entities;
    }

    SceneSnapshot Scene::Capture() const
    {
        const_cast<Scene*>(this)->UpdatePrefabs();
        SceneSnapshot snapshot;
        snapshot.Name        = m_Name;
        snapshot.PostProcess = m_PostProcessSettings;
        if (m_HasEnvironmentSettings)
            snapshot.Environment = m_EnvironmentSettings;
        snapshot.RootOrder = m_RootOrder;
        for (const auto& [id, value] : m_EntityMap)
        {
            Entity entity = value;
            if (entity.HasComponent<PrefabOwnedComponent>())
                continue;
            EntitySnapshot item;
            item.ID           = id;
            item.Tag          = entity.GetComponent<TagComponent>().Tag;
            item.Transform    = entity.GetComponent<TransformComponent>();
            item.Relationship = entity.GetComponent<RelationshipComponent>();
            Kans::Capture(entity, item.Camera);
            Kans::Capture(entity, item.Text);
            Kans::Capture(entity, item.UIScreen);
            Kans::Capture(entity, item.UIWorld);
            Kans::Capture(entity, item.Sprite);
            Kans::Capture(entity, item.Model);
            Kans::Capture(entity, item.Editable);
            Kans::Capture(entity, item.Prefab);
            Kans::Capture(entity, item.Animator);
            Kans::Capture(entity, item.Timeline);
            Kans::Capture(entity, item.Active);
            Kans::Capture(entity, item.IK);
            Kans::Capture(entity, item.ShapeKeys);
            Kans::Capture(entity, item.MaterialDrivers);
            Kans::Capture(entity, item.AudioSource);
            Kans::Capture(entity, item.AudioListener);
            Kans::Capture(entity, item.PrefabParent);
            if (item.Prefab)
            {
                if (item.Animator && item.Prefab->IsGeneratedAnimator(*item.Animator))
                    item.Animator.reset();
                item.Prefab->Expanded = false;
                item.Prefab->Nodes.clear();
            }
            const auto parent = GetParent(entity);
            if (parent && parent.HasComponent<PrefabOwnedComponent>())
            {
                const auto& owned = parent.GetComponent<PrefabOwnedComponent>();
                item.PrefabParent = PrefabAttachmentComponent {owned.Instance, owned.NodeKey};
            }
            // Attachments wait under the instance root while its products load.
            // Capture their authored parent, independent of that temporary hierarchy.
            if (item.PrefabParent)
                item.Relationship.Parent = PrefabEntityUUID(item.PrefabParent->Instance, item.PrefabParent->NodeKey);
            auto& children = item.Relationship.Children;
            children.erase(std::remove_if(children.begin(),
                                          children.end(),
                                          [&](UUID child) {
                                              auto e = GetEntityByUUID(child);
                                              return e &&
                                                     (e.HasComponent<PrefabOwnedComponent>() ||
                                                      (e.HasComponent<PrefabAttachmentComponent>() &&
                                                       e.GetComponent<PrefabAttachmentComponent>().Instance == id));
                                          }),
                           children.end());
            Kans::Capture(entity, item.DirLight);
            Kans::Capture(entity, item.PointLight);
            Kans::Capture(entity, item.Script);
            Kans::Capture(entity, item.RigidBody);
            Kans::Capture(entity, item.BoxCollider);
            Kans::Capture(entity, item.SphereCollider);
            Kans::Capture(entity, item.CapsuleCollider);
            Kans::Capture(entity, item.MeshCollider);
            Kans::Capture(entity, item.GlobalWind);
            Kans::Capture(entity, item.ForceField);
            Kans::Capture(entity, item.Ragdoll);
            Kans::Capture(entity, item.Particles);
            Kans::Capture(entity, item.Vehicle);
            Kans::Capture(entity, item.CharacterController);
            Kans::Capture(entity, item.Joint);
            Kans::Capture(entity, item.SpringBone);
            Kans::Capture(entity, item.Cloth);
            Kans::Capture(entity, item.MotionCollider);

            snapshot.Entities.push_back(std::move(item));
        }
        std::sort(snapshot.Entities.begin(), snapshot.Entities.end(), [](const auto& a, const auto& b) {
            return a.ID < b.ID;
        });
        return snapshot;
    }

    void Scene::Restore(const SceneSnapshot& snapshot)
    {
        m_PhysicsWorld.reset();
        StopAudio();
        DestroyNativeScriptInstances();
        if (ScriptEngine::IsInitialized() && ScriptEngine::GetSceneContext() == this)
            ScriptEngine::OnRuntimeEnd();
        for (const auto& [handle, record] : m_RenderWorld->m_Records)
            m_RenderWorld->Dirty(handle);
        m_SuppressNotifications = true;
        m_Registry.clear();
        m_RenderWorld->m_TextRecords.clear();
        m_RenderWorld->m_UIRecords.clear();
        m_EditableMeshes.clear();
        m_EditableSources.clear();
        m_SuppressNotifications = false;
        m_DirtyPrefabs.clear();
        m_CapturePrefabNodes.clear();
        m_DirtyBounds.clear();
        m_EntityMap.clear();
        m_PrefabDependencies.clear();
        m_PrefabAssetUsers.clear();
        m_ChildBounds.clear();
        m_RootOrder.clear();
        m_Name = snapshot.Name;
        SetPostProcessSettings(snapshot.PostProcess);
        if (snapshot.Environment)
            SetEnvironmentSettings(*snapshot.Environment);
        else
            ClearEnvironmentSettings();
        for (const auto& item : snapshot.Entities)
        {
            Entity entity                           = CreateEntityWithID(item.ID, item.Tag, false);
            entity.GetComponent<TagComponent>().Tag = item.Tag;
            entity.SetComponent<TransformComponent>(item.Transform);
            entity.SetComponent<RelationshipComponent>(item.Relationship);
            Kans::Restore(entity, item.Camera);
            Kans::Restore(entity, item.Text);
            Kans::Restore(entity, item.UIScreen);
            Kans::Restore(entity, item.UIWorld);
            Kans::Restore(entity, item.Sprite);
            Kans::Restore(entity, item.Model);
            Kans::Restore(entity, item.Editable);
            Kans::Restore(entity, item.Prefab);
            Kans::Restore(entity, item.IK);
            Kans::Restore(entity, item.ShapeKeys);
            Kans::Restore(entity, item.Animator);
            Kans::Restore(entity, item.Timeline);
            Kans::Restore(entity, item.Active);
            Kans::Restore(entity, item.MaterialDrivers);
            Kans::Restore(entity, item.AudioSource);
            Kans::Restore(entity, item.AudioListener);
            Kans::Restore(entity, item.PrefabParent);
            Kans::Restore(entity, item.DirLight);
            Kans::Restore(entity, item.PointLight);
            Kans::Restore(entity, item.Script);
            Kans::Restore(entity, item.RigidBody);
            Kans::Restore(entity, item.BoxCollider);
            Kans::Restore(entity, item.SphereCollider);
            Kans::Restore(entity, item.CapsuleCollider);
            Kans::Restore(entity, item.MeshCollider);
            Kans::Restore(entity, item.GlobalWind);
            Kans::Restore(entity, item.ForceField);
            Kans::Restore(entity, item.Ragdoll);
            Kans::Restore(entity, item.Particles);
            Kans::Restore(entity, item.Vehicle);
            Kans::Restore(entity, item.CharacterController);
            Kans::Restore(entity, item.Joint);
            Kans::Restore(entity, item.SpringBone);
            Kans::Restore(entity, item.Cloth);
            Kans::Restore(entity, item.MotionCollider);
        }
        m_RootOrder         = snapshot.RootOrder;
        m_ReconcilingPrefab = true;
        for (auto handle : m_Registry.view<PrefabAttachmentComponent>())
        {
            Entity entity {handle, this};
            if (!GetParent(entity))
            {
                const auto& attachment = entity.GetComponent<PrefabAttachmentComponent>();
                SetParent(entity, GetEntityByUUID(attachment.Instance), {}, false);
            }
        }
        m_ReconcilingPrefab = false;
        UpdatePrefabs();
    }
} // namespace Kans
