#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Physics/PhysicsComponents.h"
#include "Kans3D/Physics/PhysicsSettings.h"
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    class DebugDraw;
    class Scene;
} // namespace Kans

// The physics editor's logic, kept apart from ImGui so it can be tested headless:
// layer choices, material precedence, the groups a joint type shows, collider and
// hinge-limit handles, the gizmo lines, .physmat files and the project's settings.
namespace Kans::PhysicsEditing
{
    // ---- Layers ----

    // A collider's layer row: index 0 inherits the body's layer, index 1 + n is layer n.
    int                     ColliderLayerIndex(const std::optional<uint32_t>& layer);
    std::optional<uint32_t> ColliderLayerFromIndex(int index);
    // "n  Name" for each of the 32 layers, for dropdowns.
    std::vector<std::string> LayerLabels(const PhysicsSettings& settings);
    // A collision mask's summary: "Everything", "Nothing", the one layer's name or "Mixed (n layers)".
    std::string MaskSummary(uint32_t mask, const PhysicsSettings& settings);
    // The layers the collision matrix shows: every layer, or those renamed from the default
    // "Layer n" (layer 0 always).
    std::vector<uint32_t> MatrixLayers(const PhysicsSettings& settings, bool all);
    // Flips whether layers `a` and `b` collide, for both of them (the matrix is symmetric).
    void ToggleLayerCollision(PhysicsSettings& settings, uint32_t a, uint32_t b);

    // ---- Collider materials ----

    // Which values a collider's contacts use: its loaded asset, else its inline values unless it
    // uses the default, else the project default. A set asset that failed to load falls back to
    // the project default (not the inline values).
    enum class MaterialSource
    {
        Asset,
        Inline,
        ProjectDefault
    };
    MaterialSource EffectiveMaterialSource(const ColliderMaterial& material, bool assetLoaded);
    const char*    MaterialSourceName(MaterialSource source);

    // ---- Joints ----

    // The setting groups a joint card shows; the common ones (bodies, breaking, mass scales) are
    // always shown.
    enum JointGroup : uint32_t
    {
        HingeLimit    = 1u << 0,
        HingeMotor    = 1u << 1,
        HingeSpring   = 1u << 2,
        SliderLimit   = 1u << 3,
        SliderDrive   = 1u << 4,
        SphericalCone = 1u << 5,
        DistanceRange = 1u << 6,
        D6Motion      = 1u << 7,
        D6Limits      = 1u << 8,
        D6Drives      = 1u << 9,
    };
    uint32_t JointGroups(JointType type);
    // The body a joint's BodyA resolves to: itself when set, else the nearest entity up from the
    // joint (itself first) with a rigid body. Empty when none.
    Entity ResolveBodyA(Scene& scene, Entity joint);
    // What an unset BodyA resolves to.
    Entity AutoBodyA(Scene& scene, Entity joint);

    // ---- Shape handles ----

    // A resize handle on a primitive: where it sits and which way it pulls, in the shape's
    // offset space (before ShapeOffset::Matrix()).
    struct ShapeHandle
    {
        int       ID = 0;
        glm::vec3 Position {0}, Direction {0};
    };
    // Boxes: one per face (ID = axis * 2 + (positive ? 1 : 0)). Spheres: one per axis direction.
    // Capsules: the two caps along the axis (IDs 0, 1) and four radial ones (IDs 2..5).
    std::vector<ShapeHandle> Handles(const Physics::Box& box);
    std::vector<ShapeHandle> Handles(const Physics::Sphere& sphere);
    std::vector<ShapeHandle> Handles(const Physics::Capsule& capsule);
    // Moves handle `id` of `start` by `distance` along its direction (offset-space units) and
    // returns the result. Box faces and capsule caps move alone (the opposite side stays put,
    // so the offset shifts); sphere and capsule radii grow from the centre. Sizes stay above
    // MinimumSize (and capsule half heights at or above zero).
    inline constexpr float MinimumSize = 0.001f;
    Physics::Box           DragHandle(const Physics::Box& start, int id, float distance);
    Physics::Sphere        DragHandle(const Physics::Sphere& start, int id, float distance);
    Physics::Capsule       DragHandle(const Physics::Capsule& start, int id, float distance);

    // ---- Hinge limit handles ----

    // The joint frame: the entity's world position and rotation, without scale.
    glm::mat4 JointFrame(const glm::mat4& world);
    // The radius the hinge-limit arc and its handles are drawn at.
    inline constexpr float LimitArcRadius = 0.6f;
    // A point on the hinge-limit arc at `degrees` about the frame's +X (0 at +Y, 90 at +Z).
    glm::vec3 HingeArcPoint(const glm::mat4& frame, float degrees);
    // The angle (degrees, -180..180) under a pointer ray on the frame's YZ plane; empty when the
    // ray is parallel to the plane or points away from it.
    std::optional<float> HingeAngleAt(const glm::mat4& frame, glm::vec3 origin, glm::vec3 direction);

    // ---- Gizmo lines ----

    // The selected collider's wire shape (any primitive on `entity`), its character controller with
    // the up direction, and its joint's frame, limits and lines to its bodies.
    void DrawColliders(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color);
    void DrawController(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color);
    void DrawJoint(DebugDraw& draw, Scene& scene, Entity entity);
    // Line colours to BodyA and BodyB (the world end is a small cross at the joint).
    inline constexpr glm::vec4 BodyAColor {1.0f, 0.55f, 0.1f, 1.0f};
    inline constexpr glm::vec4 BodyBColor {0.2f, 0.8f, 1.0f, 1.0f};

    // ---- .physmat files ----

    std::variant<PhysicsMaterialValues, AssetError> LoadPhysicsMaterialFile(const std::filesystem::path& file);
    std::optional<AssetError>                       SavePhysicsMaterialFile(const std::filesystem::path& file,
                                                                            const PhysicsMaterialValues& values);
    // Writes "NewPhysicsMaterial.physmat" (numbered when taken) with default values; the file, or the error.
    std::variant<std::filesystem::path, AssetError> CreatePhysicsMaterialFile(const std::filesystem::path& directory);

    // ---- Project settings ----

    // The project's physics settings while the editor edits them. The project file keeps them
    // (ProjectFile::Physics, which becomes ProjectConfig::Physics when the project opens) and the
    // runtime reads KansFileSystem's copy; Apply updates both, Save writes the file.
    class ProjectPhysicsSettings
    {
    public:
        std::optional<AssetError>    Load(const std::filesystem::path& projectFile);
        const PhysicsSettings&       Get() const { return m_Settings; }
        const std::filesystem::path& GetFile() const { return m_File; }
        // Validates `settings` and makes them current in both places; an invalid value leaves
        // everything unchanged and returns the reason.
        std::optional<std::string> Apply(const PhysicsSettings& settings);
        bool                       IsDirty() const { return m_Dirty; }
        // Writes the settings into the project file, keeping its other fields.
        std::optional<AssetError> Save();

    private:
        std::filesystem::path m_File;
        PhysicsSettings       m_Settings;
        bool                  m_Dirty = false;
    };
} // namespace Kans::PhysicsEditing
