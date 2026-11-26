#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <functional>
#include "Kans3D/Geometry/Shapes.h"
#include "Kans3D/Scene/Components.h"
#include "ComponentRegistry.h"

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
    void DrawTextComponent(Entity entity, ComponentUI& ui);
    void DrawDirectionalLight(Entity entity, ComponentUI& ui);
    void DrawPointLight(Entity entity, ComponentUI& ui);
    void DrawSpotLight(Entity entity, ComponentUI& ui);
    void DrawAreaLight(Entity entity, ComponentUI& ui);
    void DrawFogVolume(Entity entity, ComponentUI& ui);
    void DrawScript(Entity entity, ComponentUI& ui);
    void DrawPrefabInstance(Entity root, ComponentUI& ui);
    void DrawMesh(Entity entity, ComponentUI& ui);
    void DrawEditableMesh(Entity entity, ComponentUI& ui);
    void DrawAnimator(Entity entity, ComponentUI& ui);
    void DrawIK(Entity entity, ComponentUI& ui);
    void DrawAimConstraint(Entity entity, ComponentUI& ui);
    void DrawShapeKeys(Entity entity, ComponentUI& ui);
    void DrawTimelinePlayer(Entity entity, ComponentUI& ui);
    // Audio (AudioComponentEditors.cpp).
    void DrawAudioSource(Entity entity, ComponentUI& ui);
    void DrawAudioListener(Entity entity, ComponentUI& ui);
    void DrawAudioReverbZone(Entity entity, ComponentUI& ui);
    // Where the audio cards drew their controls in the last frame (screen space), and what they
    // showed, for tooltips and UI tests.
    struct AudioCardBoxes
    {
        struct SourceBoxes
        {
            ImRect AttenuationAsset, AttenuationPreview, Shape[4], Size[3], ReverbZoneSend, LfeSend, PinToCenter;
            bool   PinToCenterEnabled = false, ModelOverridden = false;
        } Source;
        struct ZoneBoxes
        {
            ImRect Shape[2], Size[3], FadeDistance, Bus, SendLevel, Priority;
            bool   MissingBus = false;
        } Zone;
        std::string ListenerSlot; // "Listener 1", "Inactive — over the limit of 4"...
    };
    AudioCardBoxes& LastAudioCardBoxes();
    // Whether `bus` is a bus of `engine`'s layout (the default layout without one).
    bool HasAudioBus(const AudioEngine* engine, const std::string& bus);
    // The mixer bus a sound plays through as a grid row: a dropdown of `engine`'s buses (the default
    // layout without one). A name the layout lacks is kept and shown as missing (`missing` says what
    // that means); `inherited` (when not null) is the hint of a value another asset provides; reset
    // goes to `fallback`. True when changed.
    bool AudioBusRow(const char*        label,
                     std::string&       bus,
                     const AudioEngine* engine,
                     const char*        inherited,
                     const char*        fallback = MasterAudioBus,
                     const char*        missing  = nullptr,
                     ImRect*            box      = nullptr);
    // A send level in dB with Off (-inf) at the field's left end; reset turns it off. True when changed.
    bool AudioSendRow(const char* label, float& db, const char* tooltip = nullptr, ImRect* box = nullptr);
    // The Stats panel's Audio card: the engine, its voices and concurrency groups, timeline audio and,
    // in Play, the events triggered so far (by the name `eventName` gives; an empty name hides one).
    struct AudioStatsInput
    {
        const AudioEngine*                  Engine               = nullptr;
        size_t                              TimelineVoices       = 0;
        double                              TimelineDriftSeconds = 0;
        bool                                Playing              = false;
        std::function<std::string(AssetID)> EventName;
    };
    void DrawAudioStats(const AudioStatsInput& input);
    // Video (VideoPlayerEditors.cpp): the Video Player card and the Stats panel's Video card.
    void DrawVideoPlayer(Entity entity, ComponentUI& ui);
    void DrawVideoStats(const VideoPlayerStats& stats);
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
    // Particle System: the system with Open, playback, user parameter overrides, layers and the
    // in-editor preview (ParticleSystemEditors.cpp).
    void DrawParticleSystem(Entity entity, ComponentUI& ui);
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
    // Only entities `accept` takes are listed (and dropped); `none` names the 0 entry.
    bool        EntityField(const char*                        label,
                            uint64_t&                          id,
                            Scene*                             scene,
                            const std::function<bool(Entity)>& accept,
                            const char*                        none);
    const char* ShapeName(ShapeType type);
    // Adds a generated shape resting on the ground plane at the entity's position.
    void CreateShape(Entity entity, ShapeType type);
} // namespace Kans::ComponentEditors
