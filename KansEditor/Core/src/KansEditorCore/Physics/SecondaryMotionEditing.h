#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Physics/ClothAsset.h"
#include "Kans3D/Physics/ClothComponent.h"
#include "Kans3D/Physics/ClothSolver.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Entity.h"
#include "KansEditorCore/Physics/PhysicsEditing.h"

namespace Kans
{
    class DebugDraw;
    class Scene;
} // namespace Kans

// The secondary-motion editor's logic (spring bones, mesh cloth and motion colliders), kept apart
// from ImGui so it can be tested headless: chain resolution and picking, chain curves, motion
// collider handles and gizmo lines, cloth painting, presets and the edit-mode preview.
namespace Kans::SecondaryMotionEditing
{
    // ---- Spring chains ----

    // The bones a chain walks, as the engine resolves them: from EndBone up to RootBone when an end
    // is set (nothing unless the end is the root or below it), else down the first children from the
    // root; the walk stops before the first excluded bone. The last bone is only the tail of the one
    // before it unless the chain has an end extension, so `Simulated` joints come first.
    struct ResolvedChain
    {
        std::vector<Entity> Path;
        size_t              Simulated = 0;
    };
    ResolvedChain ResolveChain(const Scene& scene, const SpringBoneChain& chain);

    // "Pick from Hierarchy": the next selected entity becomes the chain's root, the one after it
    // the end (the root itself or one of its descendants).
    class ChainPicker
    {
    public:
        enum class Result
        {
            Ignored,  // Not picking, or the owner itself was clicked.
            RootSet,  // Waiting for the end bone.
            Done,     // Root and end were written to the chain.
            Rejected, // The end is not below the root; still waiting for the end.
        };
        void        Begin(UUID owner, size_t chain);
        void        Cancel();
        bool        IsActive() const { return m_Owner != UUID(0); }
        bool        IsPicking(UUID owner, size_t chain) const { return m_Owner == owner && m_Chain == chain; }
        UUID        Owner() const { return m_Owner; }
        size_t      Chain() const { return m_Chain; }
        bool        HasRoot() const { return m_Root != UUID(0); }
        const char* Prompt() const;
        // Feeds a picked entity; on Done, writes RootBone and EndBone of `chain`.
        Result Pick(const Scene& scene, Entity picked, SpringBoneChain& chain);

    private:
        UUID   m_Owner = 0, m_Root = 0;
        size_t m_Chain = 0;
    };

    // Feeds a selection change to an active picker: picks it for the owner's chain (writing the
    // component when the end is picked) and returns the owner, to select again; null when the
    // selection was not a pick. A picker whose owner or chain is gone stops.
    Entity FeedChainPicker(ChainPicker& picker, Scene& scene, Entity selected, ChainPicker::Result* result = nullptr);

    // ---- Chain curves (0 = root, 1 = tip; a curve replaces the base value) ----

    // Two linear keys at 0 and 1, both `value`.
    void ResetCurve(TimelineCurve& curve, glm::vec4 value);
    // Adds a key at `time` with the curve's value there; its index.
    size_t InsertCurveKey(TimelineCurve& curve, double time, glm::vec4 fallback);
    // Moves key `index`: the first and last keys stay at 0 and 1, the others stay strictly between
    // their neighbours.
    void MoveCurveKey(TimelineCurve& curve, size_t index, double time, glm::vec4 value);
    // Removes an inner key (the ends stay); false when not removed.
    bool RemoveCurveKey(TimelineCurve& curve, size_t index);

    // ---- Motion colliders ----

    // Planes: one handle on the plane that moves it along its normal.
    std::vector<PhysicsEditing::ShapeHandle> Handles(const Physics::Plane& plane);
    Physics::Plane                           DragHandle(const Physics::Plane& start, int id, float distance);
    std::vector<PhysicsEditing::ShapeHandle> Handles(const Physics::MotionColliderShape& shape);
    Physics::MotionColliderShape             DragHandle(const Physics::MotionColliderShape& start, int id, float distance);
    // Inside is only honoured for spheres and capsules.
    bool SupportsInside(const Physics::MotionColliderShape& shape);
    // Half the side of the grid a plane is drawn with.
    inline constexpr float PlaneGridHalfSize = 0.5f;
    // The collider's wire shape: planes as a grid with their normal; Inside colliders get inward
    // ticks.
    void DrawMotionCollider(DebugDraw& draw, Scene& scene, Entity entity, glm::vec4 color);
    // Entities with a motion collider in the groups (a group is an entity and its subtree), each once.
    std::vector<Entity> GroupColliders(Scene& scene, const std::vector<UUID>& groups);
    // A spring component's chains: a sphere of the hit radius on each joint and lines along the bones.
    void DrawSpringChains(DebugDraw& draw, Scene& scene, Entity owner, glm::vec4 color);
    // What the viewport shows for a selected spring-bone or cloth entity: its chains and every
    // collider of its collider groups.
    void
    DrawSecondaryMotion(DebugDraw& draw, Scene& scene, Entity selected, const std::unordered_set<UUID>& hidden = {});
    inline constexpr glm::vec4 ColliderColor {1.0f, 0.45f, 0.75f, 1.0f};
    inline constexpr glm::vec4 ChainColor {0.35f, 0.85f, 1.0f, 1.0f};

    // ---- Cloth maps ----

    const char*         MapName(ClothMap map);
    inline constexpr int MapCount = 5;
    // The values of `map` (stretch and bend multipliers read as 1 while unpainted).
    std::vector<float> MapValues(const ClothData& data, ClothMap map);
    // Which values a map accepts: backstop distance may be negative, the others not.
    float ClampMapValue(ClothMap map, float value);
    // Per-particle neighbours over the welded triangles (so seams are one particle).
    std::vector<std::vector<uint32_t>> Neighbours(const ClothData& data);
    // The section's particle values from a vertex colour channel times `scale`; welded vertices take
    // the minimum (the importer's rule).
    std::vector<float> MapFromVertexColors(const ClothData& data, const MeshData& mesh, int channel, float scale);
    // Blue (low) through green to red (high).
    glm::vec4 HeatColor(float t);

    // ---- Painting tools ----

    enum class BrushMode
    {
        Add,
        Subtract,
        Set,
        Smooth
    };
    struct Brush
    {
        float     Radius = 0.1f, Strength = 0.5f, Falloff = 0.5f, Value = 0.1f;
        BrushMode Mode = BrushMode::Add;
    };
    // The weight at `distance` from the brush centre: 1 inside (1 - falloff) of the radius, then a
    // smooth fade to 0 at the radius.
    float BrushWeight(float distance, float radius, float falloff);
    // One dab at `centre` over particles at `positions`. Add/Subtract move by Value * Strength *
    // weight, Set blends toward Value, Smooth blends toward the neighbours' average.
    void ApplyBrush(ClothMap                                  map,
                    std::vector<float>&                       values,
                    const std::vector<glm::vec3>&             positions,
                    const std::vector<std::vector<uint32_t>>& neighbours,
                    glm::vec3                                 centre,
                    const Brush&                              brush);
    // The particles connected to `seed` through neighbours of (about) the seed's value.
    std::vector<uint32_t> ConnectedRegion(const std::vector<float>&                 values,
                                          const std::vector<std::vector<uint32_t>>& neighbours,
                                          uint32_t                                  seed,
                                          float                                     tolerance = 1e-5f);
    // Values from `start` at `a` to `end` at `b` along the projection on a→b, clamped outside.
    void ApplyGradient(std::vector<float>&           values,
                       const std::vector<glm::vec3>& positions,
                       glm::vec3                     a,
                       glm::vec3                     b,
                       float                         start,
                       float                         end);
    // The triangle under a ray and its nearest particle.
    struct ParticleHit
    {
        uint32_t  Particle = 0;
        glm::vec3 Point {0};
        float     Distance = 0;
    };
    std::optional<ParticleHit> PickParticle(const ClothData&              data,
                                            const std::vector<glm::vec3>& positions,
                                            glm::vec3                     origin,
                                            glm::vec3                     direction);

    // A painting session on a working copy of a .cloth asset. Each stroke is one step of the
    // session's own undo stack: the strokes edit the asset file, not the scene, so the scene's
    // history neither records nor reverts them, and Cancel drops every step at once.
    class ClothPaintSession
    {
    public:
        void Begin(UUID entity, AssetID asset, ClothData data, std::filesystem::path file);
        bool IsActive() const { return m_Entity != UUID(0); }
        // Ends painting and drops the working copy and its steps.
        void Cancel();
        UUID                         GetEntity() const { return m_Entity; }
        AssetID                      GetAsset() const { return m_Asset; }
        const std::filesystem::path& GetFile() const { return m_File; }
        const ClothData&             GetData() const { return m_Data; }
        ClothMap                     ActiveMap = ClothMap::MaxDistance;

        // A stroke edits a copy of the active map; End writes it (rebuilding the tethers) as one step.
        void                BeginStroke();
        bool                InStroke() const { return m_Stroke.has_value(); }
        std::vector<float>& StrokeValues() { return *m_Stroke; }
        // True when the map changed; an invalid map leaves everything as it was.
        bool EndStroke();
        void CancelStroke() { m_Stroke.reset(); }
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        // Unsaved: the current step is not the one saved (undoing back to it is clean again).
        bool IsDirty() const { return CurrentStep() != m_SavedStep; }
        // Changes with every edit, undo and redo (the preview rebinds when it moves).
        uint64_t GetVersion() const { return m_Version; }
        // Writes the working copy to the .cloth file.
        std::optional<AssetError> Save();
        // The asset of the working copy (carrying the component's ID) for preview binding.
        Ref<ClothAsset> MakeAsset() const;

        // World positions of the particles on the current (skinned) pose, for picking and drawing.
        const std::vector<glm::vec3>& UpdatePose(Scene& scene, Entity entity, Ref<const MeshAsset> mesh);
        const std::vector<glm::vec3>& GetPositions() const { return m_Positions; }

    private:
        struct Step
        {
            ClothMap           Map = ClothMap::MaxDistance;
            std::vector<float> Before, After;
            uint64_t           ID = 0;
        };
        uint64_t CurrentStep() const { return m_Undo.empty() ? 0 : m_Undo.back().ID; }
        bool Write(ClothMap map, const std::vector<float>& values);

        UUID                              m_Entity = 0;
        AssetID                           m_Asset;
        std::filesystem::path             m_File;
        ClothData                         m_Data;
        std::optional<std::vector<float>> m_Stroke;
        std::vector<Step>                 m_Undo, m_Redo;
        uint64_t                          m_Version = 0, m_SavedStep = 0, m_NextStep = 0;
        ClothRuntime                      m_Pose;
        uint64_t                          m_PoseVersion = UINT64_MAX;
        std::vector<glm::vec3>            m_Positions;
    };

    // A .cloth file for `section` of `mesh`: "<stem>.cloth" in `directory`, numbered when taken.
    std::variant<std::filesystem::path, AssetError> CreateClothFile(const std::filesystem::path& directory,
                                                                    const std::string&           stem,
                                                                    AssetID                      meshID,
                                                                    const MeshData&              mesh,
                                                                    uint32_t                     section);
    std::variant<ClothData, AssetError>             LoadClothFile(const std::filesystem::path& file);
    std::optional<AssetError> SaveClothFile(const std::filesystem::path& file, const ClothData& data);
    // The binding problem the engine reports, or empty when the asset fits the mesh section.
    std::string BindingProblem(const ClothData& data, AssetID meshID, const MeshData& mesh, uint32_t section);
    // "Section n  (slot name)" for each section of the mesh.
    std::vector<std::string> SectionLabels(const MeshData& mesh);

    // ---- Presets ----

    // A spring preset: the chain's simulation values and curves, and the component's inertia, wind
    // and substeps (applied when the preset is applied to the whole component).
    struct SpringPreset
    {
        SpringBoneChain Chain;
        float           InertiaScale = 1, WindScale = 1, SubstepRate = 60;
        uint32_t        MaxSubsteps = 8;
    };
    enum class PresetOrigin
    {
        BuiltIn,
        Project
    };
    struct MotionPreset
    {
        std::string                  Name;
        PresetOrigin                 Origin = PresetOrigin::Project;
        std::filesystem::path        File;
        std::optional<SpringPreset>  Spring;
        std::optional<ClothSettings> Cloth;
        // The menu label: project presets that share a built-in's name say so.
        std::string Label;
    };
    inline constexpr const char* PresetExtension = ".motionpreset";
    // The editor's built-in presets (Resources/Presets/SecondaryMotion) and the project's
    // (<project>/Settings/Presets).
    std::filesystem::path BuiltInPresetDirectory();
    std::filesystem::path ProjectPresetDirectory(const std::filesystem::path& projectRoot);

    std::variant<MotionPreset, AssetError> LoadPresetFile(const std::filesystem::path& file, PresetOrigin origin);
    std::optional<AssetError>              SavePresetFile(const std::filesystem::path& file, const MotionPreset& preset);
    // Built-ins first (in their file's Order), then the project's by name. A project preset never
    // replaces a built-in of the same name: both are listed, the project one labelled "(project)".
    // Files that fail to load are skipped and reported in `errors`.
    std::vector<MotionPreset> ListPresets(const std::filesystem::path& builtInDirectory,
                                          const std::filesystem::path& projectDirectory,
                                          std::vector<std::string>*    errors = nullptr);
    // Project preset names are file names that are not a built-in's name (any case).
    std::optional<std::string> PresetNameProblem(const std::string&               name,
                                                 const std::vector<MotionPreset>& existing);
    // Writes a new project preset; the file, or why not (a bad or taken name).
    std::variant<std::filesystem::path, AssetError> SaveProjectPreset(const std::filesystem::path&     projectDirectory,
                                                                      const std::string&               name,
                                                                      MotionPreset                     preset,
                                                                      const std::vector<MotionPreset>& existing);
    std::optional<AssetError> RenameProjectPreset(const MotionPreset&              preset,
                                                  const std::string&               name,
                                                  const std::vector<MotionPreset>& existing);
    std::optional<AssetError> DeleteProjectPreset(const MotionPreset& preset);

    // Applying sets simulation values and curves only: bones, colliders, collider groups, the
    // centre, the asset, the section and painted maps never change.
    void          ApplySpringPreset(SpringBoneChain& chain, const SpringPreset& preset);
    void          ApplySpringPreset(SpringBoneComponent& component, const SpringPreset& preset);
    void          ApplyClothPreset(ClothSettings& settings, const ClothSettings& preset);
    SpringPreset  CaptureSpringPreset(const SpringBoneComponent& component, size_t chain);
    ClothSettings CaptureClothPreset(const ClothSettings& settings);

    // ---- Edit-mode preview ----

    // Turns the scene's secondary-motion preview on or off (off restores the animated pose).
    void SetPreview(Scene& scene, bool enabled);
    // One edit-mode frame of the preview: springs and cloth on the current animated pose.
    void StepPreview(Scene& scene, double seconds);
} // namespace Kans::SecondaryMotionEditing
