#include "ComponentEditors.h"
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Animation/DeformationComponents.h"
#include "Kans3D/Physics/PhysicsComponents.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/Timeline.h"
#include "Kans3D/UI/UIComponents.h"
#include "UI/EditorIcons.h"

namespace Kans
{
    namespace
    {
        // The override a prefab part carries, if any.
        const PrefabNodeOverride* PartOverride(Entity entity, Scene& scene)
        {
            if (!entity.HasComponent<PrefabOwnedComponent>())
                return nullptr;
            const auto& owned = entity.GetComponent<PrefabOwnedComponent>();
            Entity      root  = scene.GetEntityByUUID(owned.Instance);
            if (!root || !root.HasComponent<PrefabInstanceComponent>())
                return nullptr;
            const auto& overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
            const auto  found     = overrides.find(owned.NodeKey);
            return found != overrides.end() ? &found->second : nullptr;
        }

        bool IsPrefabPart(Entity entity) { return entity.HasComponent<PrefabOwnedComponent>(); }

    } // namespace

    void ComponentEditors::DefaultDirectionalLight(DirLightComponent& light)
    {
        light.Direction         = {0.0f, -1.0f, 0.0f};
        light.Diffuse_Intensity = light.Specular_Intensity = light.Ambient_Intensity = glm::vec3(1.0f);
    }
    void ComponentEditors::DefaultPointLight(PointLightComponent& light)
    {
        light.Diffuse_Intensity = light.Specular_Intensity = light.Ambient_Intensity = glm::vec3(1.0f);
    }

    void RegisterBuiltinComponents(ComponentRegistry& registry)
    {
        using namespace ComponentEditors;

        // Registration order is the Properties panel's section order.
        auto& transform     = registry.Register<TransformComponent>("Transform", ICON_FA_ARROWS_MOVE, "General");
        transform.Add       = {}; // Every entity has one.
        transform.CanRemove = {};
        transform.Draw      = DrawTransform;
        transform.Overridden = [](Entity e, Scene& scene) {
            const auto* change = PartOverride(e, scene);
            return change && change->LocalTransform.has_value();
        };
        transform.Revert = [](Entity e, Scene& scene) {
            // Clear the part's override, then restore its material replacements.
            const auto* change    = PartOverride(e, scene);
            const auto  materials = change ? change->Materials : std::map<uint32_t, AssetID> {};
            const auto& owned     = e.GetComponent<PrefabOwnedComponent>();
            Entity      root      = scene.GetEntityByUUID(owned.Instance);
            scene.ClearPrefabOverride(root, owned.NodeKey);
            for (const auto& [slot, material] : materials)
                scene.SetPrefabMaterialOverride(root, owned.NodeKey, slot, material);
        };

        auto& text    = registry.Register<TextComponent>("Text", ICON_FA_FONT, "Rendering");
        text.IconRank = 6;
        text.Add      = [](Entity e) { e.AddComponent<TextComponent>().Text = "Text"; };
        // Reset keeps the string and the font: it restores the style.
        text.Reset = [](Entity e) {
            e.PatchComponent<TextComponent>([](auto& c) {
                TextComponent style;
                style.Text = c.Text;
                style.Font = c.Font;
                c          = style;
            });
        };
        text.Draw        = DrawText;
        text.CreateName  = "Text";
        text.CreateOrder = 5;
        text.Create      = text.Add;

        auto& camera    = registry.Register<CameraComponent>("Camera", ICON_FA_VIDEO, "Rendering");
        camera.IconRank = 1;
        camera.Draw     = DrawCamera;
        camera.CreateName = "Camera";
        camera.CreateOrder = 1;
        // A camera created from the Hierarchy does not take over the primary view.
        camera.Create = [](Entity e) { e.AddComponent<CameraComponent>().Primary = false; };

        auto& prefab      = registry.Register<PrefabInstanceComponent>("Prefab", ICON_FA_CUBES, "Rendering");
        prefab.IconRank   = 4;
        prefab.CanRemove  = [](Entity e) { return !IsPrefabPart(e); };
        prefab.Reset      = [](Entity e) { e.PatchComponent<PrefabInstanceComponent>([](auto& p) { p.Overrides.clear(); }); };
        prefab.Copy       = {};
        prefab.Paste      = {};
        prefab.Draw       = DrawPrefabInstance;
        prefab.CreateName = "Model";
        prefab.CreateOrder = 0;
        prefab.Create     = [](Entity e) { e.AddComponent<PrefabInstanceComponent>(); };

        auto& mesh     = registry.Register<MeshComponent>("Mesh", ICON_FA_CUBE, "Rendering");
        mesh.IconRank  = 5;
        mesh.CanRemove = [](Entity e) { return !IsPrefabPart(e); };
        mesh.Reset     = {};
        mesh.Copy      = {};
        mesh.Paste     = {};
        mesh.Draw      = DrawMesh;
        // Exclusive with an editable mesh, which renders the entity itself.
        mesh.CanAdd = [](Entity e) { return !e.HasComponent<EditableMeshComponent>(); };
        mesh.Overridden = [](Entity e, Scene& scene) {
            const auto* change = PartOverride(e, scene);
            return change && !change->Materials.empty();
        };
        mesh.Revert = [](Entity e, Scene& scene) {
            const auto* change = PartOverride(e, scene);
            if (!change)
                return;
            const auto  slots = change->Materials;
            const auto& owned = e.GetComponent<PrefabOwnedComponent>();
            Entity      root  = scene.GetEntityByUUID(owned.Instance);
            for (const auto& [slot, material] : slots)
                scene.SetPrefabMaterialOverride(root, owned.NodeKey, slot, {});
        };

        auto& editable     = registry.Register<EditableMeshComponent>("Editable Mesh", ICON_FA_SHAPES, "Rendering");
        editable.IconRank  = 5;
        editable.CanAdd    = [](Entity e) { return !e.HasComponent<MeshComponent>() && !IsPrefabPart(e); };
        editable.Add       = [](Entity e) {
            EditableMeshComponent cube;
            cube.Shape = ShapeDesc {};
            RegenerateShape(cube);
            e.AddComponent<EditableMeshComponent>(std::move(cube));
        };
        editable.Reset     = {};
        editable.Draw      = DrawEditableMesh;
        editable.CreateMenu  = "3D Object";
        editable.CreateOrder = 0;
        for (int type = 0; type <= int(ShapeType::Prism); ++type)
            editable.CreateItems.push_back(
                {ShapeName(ShapeType(type)), nullptr, [type](Entity e) { CreateShape(e, ShapeType(type)); }});

        // Added with skinned prefabs (which know their skeleton); not added by hand.
        auto& animator    = registry.Register<AnimatorComponent>("Animator", ICON_FA_PERSON_RUNNING, "Animation");
        animator.IconRank = 4;
        animator.Add      = {};
        animator.Reset    = {};
        animator.Copy     = {};
        animator.Paste    = {};
        animator.Draw     = DrawAnimator;

        // On the model root, beside its Animator (or alone, on the bind pose).
        auto& ik    = registry.Register<IKComponent>("IK", ICON_FA_LINK, "Animation");
        ik.IconRank = 8;
        ik.Draw     = DrawIK;

        auto& shapeKeys    = registry.Register<ShapeKeysComponent>("Shape Keys", ICON_FA_FACE_SMILE, "Animation");
        shapeKeys.IconRank = 8;
        shapeKeys.Draw     = DrawShapeKeys;

        auto& timeline    = registry.Register<TimelinePlayerComponent>("Timeline Player", ICON_FA_FILM, "Animation");
        timeline.IconRank = 7;
        timeline.Draw     = DrawTimelinePlayer;

        auto& screen    = registry.Register<UIScreenComponent>("Screen UI", ICON_FA_DESKTOP, "UI");
        screen.IconRank = 6;
        screen.Draw     = DrawUIScreen;

        auto& world    = registry.Register<UIWorldComponent>("World UI", ICON_FA_TABLET, "UI");
        world.IconRank = 6;
        world.Draw     = DrawUIWorld;

        auto& drivers = registry.Register<MaterialParameterDriversComponent>(
            "Material Parameter Drivers", ICON_FA_SLIDERS, "Rendering");
        drivers.IconRank = 9;
        drivers.Draw     = DrawMaterialParameterDrivers;

        auto& sprite    = registry.Register<SpriteRendererComponent>("Sprite Renderer", ICON_FA_IMAGE, "Rendering");
        sprite.IconRank = 6;
        sprite.Add      = [](Entity e) { e.AddComponent<SpriteRendererComponent>(); };
        // Reset keeps the texture: it restores colour and tiling only.
        sprite.Reset = [](Entity e) {
            e.PatchComponent<SpriteRendererComponent>([](auto& c) {
                c.Color        = glm::vec4(1.0f);
                c.TilingFactor = 1.0f;
            });
        };
        sprite.Draw       = DrawSprite;
        sprite.CreateName = "Sprite";
        sprite.CreateOrder = 4;
        sprite.Create     = sprite.Add;

        auto& directional    = registry.Register<DirLightComponent>("Directional Light", ICON_FA_SUN, "Lighting");
        directional.IconRank = 2;
        directional.Add      = [](Entity e) { DefaultDirectionalLight(e.AddComponent<DirLightComponent>()); };
        directional.Reset    = [](Entity e) { e.PatchComponent<DirLightComponent>(DefaultDirectionalLight); };
        directional.Draw     = DrawDirectionalLight;
        directional.CreateName = "Directional Light";
        directional.CreateOrder = 2;
        directional.Create     = directional.Add;

        auto& point      = registry.Register<PointLightComponent>("Point Light", ICON_FA_LIGHTBULB, "Lighting");
        point.IconRank   = 3;
        point.Add        = [](Entity e) { DefaultPointLight(e.AddComponent<PointLightComponent>()); };
        point.Reset      = [](Entity e) { e.PatchComponent<PointLightComponent>(DefaultPointLight); };
        point.Draw       = DrawPointLight;
        point.CreateName = "Point Light";
        point.CreateOrder = 3;
        point.Create     = point.Add;

        // Physics: bodies, colliders, characters, joints and fields.
        auto& body    = registry.Register<RigidBodyComponent>("Rigid Body", ICON_FA_WEIGHT_HANGING, "Physics");
        body.IconRank = 10;
        body.Draw     = DrawRigidBody;

        auto& box    = registry.Register<BoxColliderComponent>("Box Collider", ICON_FA_BOX, "Physics");
        box.IconRank = 11;
        box.Draw     = DrawBoxCollider;

        auto& sphere    = registry.Register<SphereColliderComponent>("Sphere Collider", ICON_FA_BASEBALL, "Physics");
        sphere.IconRank = 11;
        sphere.Draw     = DrawSphereCollider;

        auto& capsule    = registry.Register<CapsuleColliderComponent>("Capsule Collider", ICON_FA_CAPSULES, "Physics");
        capsule.IconRank = 11;
        capsule.Draw     = DrawCapsuleCollider;

        auto& meshCollider = registry.Register<MeshColliderComponent>("Mesh Collider", ICON_FA_DRAW_POLYGON, "Physics");
        meshCollider.IconRank = 11;
        meshCollider.Draw     = DrawMeshCollider;

        auto& controller =
            registry.Register<CharacterControllerComponent>("Character Controller", ICON_FA_PERSON_WALKING, "Physics");
        controller.IconRank = 9;
        controller.Draw     = DrawCharacterController;

        auto& joint    = registry.Register<JointComponent>("Joint", ICON_FA_LINK, "Physics");
        joint.IconRank = 10;
        joint.Draw     = DrawJoint;

        auto& wind       = registry.Register<WindZoneComponent>("Global Wind", ICON_FA_WIND, "Physics");
        wind.IconRank    = 9;
        wind.Draw        = DrawGlobalWind;
        wind.CreateName  = "Global Wind";
        wind.CreateOrder = 6;
        wind.Create      = [](Entity e) { e.AddComponent<WindZoneComponent>().Strength = 1.0f; };

        // Secondary motion: spring bones and cloth on characters, and the colliders they share.
        auto& spring    = registry.Register<SpringBoneComponent>("Spring Bone", ICON_FA_BONE, "Physics");
        spring.IconRank = 9;
        spring.Draw     = DrawSpringBone;

        auto& cloth    = registry.Register<ClothComponent>("Cloth", ICON_FA_SHIRT, "Physics");
        cloth.IconRank = 9;
        cloth.Draw     = DrawCloth;

        auto& motionCollider =
            registry.Register<MotionColliderComponent>("Motion Collider", ICON_FA_BASEBALL, "Physics");
        motionCollider.IconRank    = 11;
        motionCollider.Draw        = DrawMotionCollider;
        motionCollider.CreateName  = "Motion Collider";
        motionCollider.CreateOrder = 7;
        motionCollider.Create      = [](Entity e) { e.AddComponent<MotionColliderComponent>(); };

        auto& script    = registry.Register<ScriptComponent>("Script", ICON_FA_CODE, "Scripting");
        script.IconRank = 7;
        script.Reset    = {};
        script.Draw     = DrawScript;
    }
} // namespace Kans
