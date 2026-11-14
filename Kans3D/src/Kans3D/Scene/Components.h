#pragma once
#include <string.h>
#include <map>
#include <optional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Animation/DeformationComponents.h"
#include "Kans3D/Animation/SpringBoneComponent.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "Kans3D/Audio/AudioComponents.h"
#include "Kans3D/Physics/ClothComponent.h"
#include "Kans3D/Physics/PhysicsComponents.h"
#include "Kans3D/Physics/PhysicsEquality.h"
#include "Kans3D/Physics/RagdollComponent.h"
#include "Kans3D/Physics/VehicleComponent.h"
#include "Kans3D/Timeline/Timeline.h"
#include "Kans3D/VFX/ParticleSystemComponent.h"

#include "Kans3D/Core/UUID.h"
#include "Kans3D/Geometry/Shapes.h"
#include "Kans3D/Renderer/Lighting/ShadowSettings.h"
#include "Kans3D/Text/TextComponent.h"
#include "Kans3D/UI/UIComponents.h"
#include "ProductModelComponent.h"

#include "Kans3D/Renderer/Camera/SceneCamera.h"
#include "Kans3D/Renderer/Resource/Texture.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
namespace Kans
{
    class ScriptableEntity;

    struct IDComponent
    {
        UUID ID                         = 0;
        IDComponent()                   = default;
        IDComponent(const IDComponent&) = default;
    };

    struct ActiveComponent
    {
        bool Active = true;
    };
    inline bool operator==(const ActiveComponent& a, const ActiveComponent& b) { return a.Active == b.Active; }

    struct TagComponent
    {
        std::string Tag;
        TagComponent()                    = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(std::string tag) : Tag(tag) {}
        operator std::string&() { return Tag; }
    };

    // Parent/child links between entities (by UUID; 0 = no parent). Children are
    // in display order; the Scene keeps the order of top-level entities.
    struct RelationshipComponent
    {
        UUID              Parent = 0;
        std::vector<UUID> Children;
    };

    // Transform relative to the parent entity (or the world for top-level ones).
    struct TransformComponent
    {

        glm::vec3 Position = glm::vec3(0.0f);
        // rotation here is radians
        glm::vec3 Rotation = glm::vec3(0.0f);

        glm::vec3 Scale = glm::vec3(1.0f);

        TransformComponent()                          = default;
        TransformComponent(const TransformComponent&) = default;
        TransformComponent(glm::vec3 position, glm::vec3 rotation, glm::vec3 scale) :
            Position(position), Rotation(rotation), Scale(scale)
        {}

        // Imported matrices remain exact (including shear) until a TRS field is edited.
        std::optional<glm::mat4> ImportedMatrix;
        glm::vec3                ImportedPosition {0}, ImportedRotation {0}, ImportedScale {1};

        glm::mat4 GetTransform() const
        {
            if (ImportedMatrix && Position == ImportedPosition && Rotation == ImportedRotation &&
                Scale == ImportedScale)
                return *ImportedMatrix;
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), Position);
            glm::mat4 rotation    = glm::toMat4(glm::quat(Rotation));
            glm::mat4 scale       = glm::scale(glm::mat4(1.0f), Scale);
            return translation * rotation * scale;
        }
    };

    enum class MaterialParameterQuantity : uint32_t
    {
        WorldPosition,
        WorldForward,
        WorldUp,
        WorldRight
    };
    struct MaterialParameterBinding
    {
        std::string               Parameter;
        UUID                      SourceEntity = 0;
        MaterialParameterQuantity Quantity     = MaterialParameterQuantity::WorldPosition;
    };
    struct MaterialParameterDriversComponent
    {
        std::vector<MaterialParameterBinding>                                         Bindings;
        Ref<InstanceMaterialParameters>                                               Values;
        std::map<uint32_t, std::vector<std::pair<std::string, std::array<float, 4>>>> SlotOverrides;
        std::map<uint32_t, Ref<InstanceMaterialParameters>>                           SlotValues;
        // Runtime only, ordered by first set. Copies/snapshots retain bindings alone.
        std::vector<std::pair<std::string, std::array<float, 4>>> Overrides;
        MaterialParameterDriversComponent() = default;
        MaterialParameterDriversComponent(const MaterialParameterDriversComponent& other) : Bindings(other.Bindings) {}
        MaterialParameterDriversComponent& operator=(const MaterialParameterDriversComponent& other)
        {
            if (this != &other)
            {
                Bindings = other.Bindings;
                Values.reset();
                Overrides.clear();
                SlotOverrides.clear();
                SlotValues.clear();
            }
            return *this;
        }
        MaterialParameterDriversComponent(MaterialParameterDriversComponent&&) noexcept            = default;
        MaterialParameterDriversComponent& operator=(MaterialParameterDriversComponent&&) noexcept = default;
    };

    struct MeshComponent
    {
        AssetID                 Mesh;
        std::vector<AssetID>    Materials;
        std::optional<uint32_t> Section; // Empty renders all sections, in mesh order.
        bool                    CastShadows = true, ReceiveShadows = true;
        bool                    RenderCustomDepth = false;
        uint8_t                 CustomStencil     = 0;
    };

    inline bool operator==(const MeshComponent& a, const MeshComponent& b)
    {
        return a.Mesh == b.Mesh && a.Materials == b.Materials && a.Section == b.Section &&
               a.CastShadows == b.CastShadows && a.ReceiveShadows == b.ReceiveShadows &&
               a.RenderCustomDepth == b.RenderCustomDepth && a.CustomStencil == b.CustomStencil;
    }

    // Mutually exclusive with MeshComponent. Callers must remove the previous
    // component explicitly; Entity::AddComponent asserts/rejects a conflict.
    struct EditableMeshComponent
    {
        EditableMesh             Mesh;
        std::optional<ShapeDesc> Shape;
        std::vector<AssetID>     Materials;
        bool                     CastShadows = true, ReceiveShadows = true;
        bool                     RenderCustomDepth = false;
        uint8_t                  CustomStencil     = 0;
    };
    bool operator==(const EditableMeshComponent& a, const EditableMeshComponent& b);
    // Explicit regeneration only. PatchComponent never interprets Shape changes.
    void RegenerateShape(EditableMeshComponent& component);
    // Material slots: one per material index the faces use, at least one, more when Materials lists more.
    size_t MaterialSlotCount(const EditableMeshComponent& component);
    // Removes a slot unless it is the last one. Its faces move to the slot before it (the first slot's
    // to the next), and later slots move down one.
    void RemoveMaterialSlot(EditableMeshComponent& component, uint32_t slot);

    struct BoundsComponent
    {
        Bounds World;
    };
    struct PrefabOwnedComponent
    {
        UUID        Instance = 0;
        std::string NodeKey;
        size_t      Order = 0;
    };
    // Retains a user child's attachment even while its prefab parent is unavailable.
    struct PrefabAttachmentComponent
    {
        UUID        Instance = 0;
        std::string NodeKey;
    };
    struct PrefabNodeOverride
    {
        std::optional<RigidBodyComponent>           RigidBody;
        std::optional<BoxColliderComponent>         BoxCollider;
        std::optional<SphereColliderComponent>      SphereCollider;
        std::optional<CapsuleColliderComponent>     CapsuleCollider;
        std::optional<MeshColliderComponent>        MeshCollider;
        std::optional<ForceFieldComponent>          ForceField;
        std::optional<RagdollComponent>             Ragdoll;
        std::optional<ParticleSystemComponent>      Particles;
        std::optional<VehicleComponent>             Vehicle;
        std::optional<WindZoneComponent>            GlobalWind;
        std::optional<CharacterControllerComponent> CharacterController;
        std::optional<ClothComponent>               Cloth;
        std::optional<SpringBoneComponent>          SpringBone;
        std::optional<MotionColliderComponent>      MotionCollider;
        std::optional<JointComponent>               Joint;
        std::optional<UIScreenComponent>            UIScreen;
        std::optional<UIWorldComponent>             UIWorld;
        std::optional<TransformComponent>           LocalTransform;
        std::map<uint32_t, AssetID>                 Materials;
        bool                                        Removed = false;
        std::optional<bool>                         RenderCustomDepth;
        std::optional<uint8_t>                      CustomStencil;
        bool                                        Empty() const
        {
            return !LocalTransform && Materials.empty() && !Removed && !RenderCustomDepth && !CustomStencil &&
                   !UIScreen && !UIWorld && !RigidBody && !BoxCollider && !SphereCollider && !CapsuleCollider &&
                   !MeshCollider && !ForceField && !Ragdoll && !Particles && !Vehicle && !GlobalWind &&
                   !CharacterController && !Joint && !Cloth && !SpringBone && !MotionCollider;
        }
    };
    struct PrefabNodeState
    {
        TransformComponent BaseTransform, AppliedTransform;
        MeshComponent      BaseMesh, AppliedMesh;
    };
    struct PrefabInstanceComponent
    {
        AssetID                                   Prefab;
        std::map<std::string, PrefabNodeOverride> Overrides;
        std::optional<ProductModelComponent>      LegacyPending;
        // Runtime state; serializers omit it, snapshots use it to rebuild atomically.
        Ref<const PrefabAsset>                    Source;
        AssetID                                   GeneratedAnimatorSkeleton;
        std::map<std::string, PrefabNodeState>    Nodes;
        std::map<AssetID, Ref<const MeshAsset>>   MeshSources;
        Ref<const PrefabAsset>                    ExpandedSource;
        std::map<std::string, PrefabNodeOverride> AppliedOverrides;
        bool                                      Expanded  = false;
        uint64_t                                  DrawOrder = 0;

        bool IsGeneratedAnimator(const AnimatorComponent& animator) const
        {
            if (!GeneratedAnimatorSkeleton.IsValid())
                return false;
            AnimatorComponent generated;
            generated.Skeleton = GeneratedAnimatorSkeleton;
            return animator == generated;
        }
    };

    struct CameraComponent
    {
        Kans::SceneCamera SceneCamera;
        bool              Primary               = true;
        bool              FixedAspectRatio      = false;
        CameraComponent()                       = default;
        CameraComponent(const CameraComponent&) = default;
    };

    struct SpriteRendererComponent
    {
        AssetID   Texture; // Invalid means white; no GPU resources in scene data.
        glm::vec4 Color {1}, UVRect {0, 0, 1, 1};
        float     TilingFactor = 1;
        bool      FlipX = false, FlipY = false;
        int       SortingOrder    = 0;
        SpriteRendererComponent() = default;
        SpriteRendererComponent(glm::vec4 color, AssetID texture = {}) : Texture(texture), Color(color) {}
    };
    inline bool operator==(const SpriteRendererComponent& a, const SpriteRendererComponent& b)
    {
        return a.Texture == b.Texture && a.Color == b.Color && a.UVRect == b.UVRect &&
               a.TilingFactor == b.TilingFactor && a.FlipX == b.FlipX && a.FlipY == b.FlipY &&
               a.SortingOrder == b.SortingOrder;
    }

    // can't be serialize
    struct NativeScriptComponent
    {
        ScriptableEntity* Instance = nullptr;

        ScriptableEntity* (*InstantiateFunction)();
        void (*DestoryInstanceFunction)(NativeScriptComponent*);

        template<typename T>
        void Bind()
        {
            InstantiateFunction     = []() { return static_cast<ScriptableEntity*>(new T()); };
            DestoryInstanceFunction = [](NativeScriptComponent* nsc) {
                delete nsc->Instance;
                nsc->Instance = nullptr;
            };
        }
    };

    struct DirLightComponent
    {
        glm::vec3      Direction {0, -1, 0};
        glm::vec3      Diffuse_Intensity {1};
        glm::vec3      Specular_Intensity {1};
        glm::vec3      Ambient_Intensity {0};
        ShadowSettings Shadows;
        DirLightComponent()                         = default;
        DirLightComponent(const DirLightComponent&) = default;
    };

    struct PointLightComponent
    {
        glm::vec3      Diffuse_Intensity {1};
        glm::vec3      Specular_Intensity {1};
        glm::vec3      Ambient_Intensity {0};
        ShadowSettings Shadows                          = ShadowSettings::PointDefaults();
        PointLightComponent()                           = default;
        PointLightComponent(const PointLightComponent&) = default;
    };

    struct ScriptComponent
    {
        ScriptComponent()                       = default;
        ScriptComponent(const ScriptComponent&) = default;
        ScriptComponent(const std::string& name) : ClassName(name) {}

        std::string ClassName;
        bool        IsLoaded = false;
    };

    // Component Type Register
    template<typename... Component>
    struct ComponentGroup
    {};
    using AllComponents = ComponentGroup<ParticleSystemComponent,
                                         ClothComponent,
                                         SpringBoneComponent,
                                         MotionColliderComponent,
                                         RigidBodyComponent,
                                         BoxColliderComponent,
                                         SphereColliderComponent,
                                         CapsuleColliderComponent,
                                         MeshColliderComponent,
                                         ForceFieldComponent,
                                         RagdollComponent,
                                         VehicleComponent,
                                         WindZoneComponent,
                                         CharacterControllerComponent,
                                         JointComponent,
                                         IDComponent,
                                         TagComponent,
                                         TransformComponent,
                                         CameraComponent,
                                         TextComponent,
                                         UIScreenComponent,
                                         UIWorldComponent,
                                         SpriteRendererComponent,
                                         NativeScriptComponent,
                                         MeshComponent,
                                         EditableMeshComponent,
                                         PrefabInstanceComponent,
                                         AnimatorComponent,
                                         TimelinePlayerComponent,
                                         ActiveComponent,
                                         IKComponent,
                                         ShapeKeysComponent,
                                         MaterialParameterDriversComponent,
                                         AudioSourceComponent,
                                         AudioListenerComponent,
                                         PrefabOwnedComponent,
                                         PrefabAttachmentComponent,
                                         BoundsComponent,
                                         DirLightComponent,
                                         PointLightComponent,
                                         ScriptComponent>;
} // namespace Kans
