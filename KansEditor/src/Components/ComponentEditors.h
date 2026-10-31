#pragma once
#include <imgui.h>
#include <filesystem>
#include <functional>
#include "ComponentRegistry.h"
#include "Kans3D/Geometry/Shapes.h"
#include "Kans3D/Scene/Components.h"

namespace Kans
{
    struct SkeletonData;
    namespace SecondaryMotionEditing
    {
        struct MotionPreset;
    }
} // namespace Kans

// Drawers of the built-in components, registered in ComponentEditors.cpp.
namespace Kans::ComponentEditors
{
    inline constexpr ImVec4 ErrorColor {0.95f, 0.38f, 0.35f, 1.0f};

    // Values a light starts with (Add and Reset), also the reset targets in its editor.
    void DefaultDirectionalLight(DirLightComponent& light);
    void DefaultPointLight(PointLightComponent& light);

    void DrawTransform(Entity entity, ComponentUI& ui);
    void DrawCamera(Entity entity, ComponentUI& ui);
    void DrawSprite(Entity entity, ComponentUI& ui);
    void DrawUIScreen(Entity entity, ComponentUI& ui);
    void DrawUIWorld(Entity entity, ComponentUI& ui);
    void DrawText(Entity entity, ComponentUI& ui);
    void DrawDirectionalLight(Entity entity, ComponentUI& ui);
    void DrawPointLight(Entity entity, ComponentUI& ui);
    void DrawScript(Entity entity, ComponentUI& ui);
    void DrawPrefabInstance(Entity root, ComponentUI& ui);
    void DrawMesh(Entity entity, ComponentUI& ui);
    void DrawEditableMesh(Entity entity, ComponentUI& ui);
    void DrawAnimator(Entity entity, ComponentUI& ui);
    void DrawIK(Entity entity, ComponentUI& ui);
    void DrawShapeKeys(Entity entity, ComponentUI& ui);
    void DrawTimelinePlayer(Entity entity, ComponentUI& ui);
    // Physics (PhysicsComponentEditors.cpp).
    void DrawRigidBody(Entity entity, ComponentUI& ui);
    void DrawBoxCollider(Entity entity, ComponentUI& ui);
    void DrawSphereCollider(Entity entity, ComponentUI& ui);
    void DrawCapsuleCollider(Entity entity, ComponentUI& ui);
    void DrawMeshCollider(Entity entity, ComponentUI& ui);
    void DrawCharacterController(Entity entity, ComponentUI& ui);
    void DrawJoint(Entity entity, ComponentUI& ui);
    // Ragdolls, vehicles and fields (RagdollVehicleFieldEditors.cpp).
    void DrawRagdoll(Entity entity, ComponentUI& ui);
    void DrawVehicle(Entity entity, ComponentUI& ui);
    void DrawWindZone(Entity entity, ComponentUI& ui);
    void DrawForceField(Entity entity, ComponentUI& ui);
    // Secondary motion (SecondaryMotionEditors.cpp).
    void DrawSpringBone(Entity entity, ComponentUI& ui);
    void DrawMotionCollider(Entity entity, ComponentUI& ui);
    void DrawCloth(Entity entity, ComponentUI& ui);
    // Where the preset menus look: the editor's built-in presets and the project's own (empty: none).
    void SetMotionPresetDirectories(std::filesystem::path builtIn, std::filesystem::path project);
    // The preset menu's items for spring (or cloth) values: apply one, Save as preset (from `capture`),
    // and the project presets' Rename and Delete. True when one was applied.
    bool MotionPresetMenuItems(bool                                                                  spring,
                               const std::function<SecondaryMotionEditing::MotionPreset()>&           capture,
                               const std::function<void(const SecondaryMotionEditing::MotionPreset&)>& apply);
    // Friction, bounciness and their combine modes as grid rows; true when changed.
    bool PhysicsMaterialRows(PhysicsMaterialValues& values);
    // A joint of `skeleton` (empty = none) as a grid row; true when changed.
    bool JointCombo(const char* label, std::string& joint, const SkeletonData* skeleton);
    void DrawMaterialParameterDrivers(Entity entity, ComponentUI& ui);
    // Picks an entity of the scene (or one dragged from the Hierarchy); 0 = none. True when changed.
    bool        EntityField(const char* label, uint64_t& id, Scene* scene);
    const char* ShapeName(ShapeType type);
    // Adds a generated shape resting on the ground plane at the entity's position.
    void CreateShape(Entity entity, ShapeType type);
} // namespace Kans::ComponentEditors
