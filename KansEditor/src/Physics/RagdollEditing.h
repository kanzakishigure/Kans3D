#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Asset/Product/ProductRecord.h"
#include "Kans3D/Physics/RagdollAsset.h"
#include "PhysicsEditing.h"
#include "UnsavedChanges.h"

namespace Kans
{
    class AssetManager;
    class DebugDraw;
    class MeshAsset;
    class Scene;
} // namespace Kans

// The ragdoll editor's logic, kept apart from ImGui so it can be tested headless: generation and
// regeneration from the editor, body primitives and their handles, the twist and swing limit
// handles, disabled collision pairs, validation, .ragdoll files, the editing session with its own
// undo stack and the isolated preview simulation.
namespace Kans::RagdollEditing
{
    // ---- Queries ----

    bool Equal(const RagdollData& a, const RagdollData& b);
    // Index of the body (or constraint) on `bone`, or -1.
    int FindBody(const RagdollData& data, const std::string& bone);
    int FindConstraint(const RagdollData& data, const std::string& bone);
    // The skeleton's joints in model space at the bind pose, without scale (the frame a body on the
    // joint uses before its shape offsets).
    std::vector<glm::mat4> BindFrames(const SkeletonData& skeleton);
    // The body the bone belongs to as the engine binds it: itself, or the nearest bone with a body
    // up its parents; -1 when none.
    int OwningBody(const RagdollData& data, const SkeletonData& skeleton, int32_t joint);

    // ---- Generation ----

    // GenerateRagdoll with the options of the editor's dialog; the reason when it fails.
    std::variant<RagdollData, std::string> Generate(const SkeletonData&              skeleton,
                                                    AssetID                          skeletonID,
                                                    const MeshData*                  mesh,
                                                    const RagdollGenerationOptions& options);
    // Generates again for `bones` (every bone when empty) and keeps the bodies, constraints and
    // collision pairs of the others as they were edited. A selected bone the new generation skips
    // loses its body; masses of the regenerated bodies come from the new generation.
    std::variant<RagdollData, std::string> Regenerate(const RagdollData&               current,
                                                      const SkeletonData&              skeleton,
                                                      AssetID                          skeletonID,
                                                      const MeshData*                  mesh,
                                                      const RagdollGenerationOptions& options,
                                                      const std::vector<std::string>& bones);

    // ---- Bodies ----

    enum class PrimitiveKind
    {
        Sphere,
        Capsule,
        Box
    };
    PrimitiveKind        KindOf(const RagdollPrimitive& primitive);
    const char*          KindName(PrimitiveKind kind);
    Physics::ShapeOffset OffsetOf(const RagdollPrimitive& primitive);
    void                 SetOffset(RagdollPrimitive& primitive, const Physics::ShapeOffset& offset);
    // A primitive of `kind` about `size` (metres) across.
    RagdollPrimitive NewPrimitive(PrimitiveKind kind, float size);
    // The primitive as another kind with the same offset and about the same extent: a capsule's
    // box spans its length and radius, a box's capsule runs along its longest axis.
    RagdollPrimitive Convert(const RagdollPrimitive& primitive, PrimitiveKind kind);
    // Adds a body (one capsule along the bone) on `bone`, with a constraint when a body is above it;
    // false when the bone already has one or is not in the skeleton.
    bool AddBody(RagdollData&             data,
                 const SkeletonData&      skeleton,
                 const std::string&       bone,
                 const RagdollConstraint& limits = {});
    // Removes the body, its constraint and its collision pairs.
    bool RemoveBody(RagdollData& data, const std::string& bone);
    // Resize handles in the primitive's offset space, as PhysicsEditing's collider handles.
    std::vector<PhysicsEditing::ShapeHandle> Handles(const RagdollPrimitive& primitive);
    RagdollPrimitive DragHandle(const RagdollPrimitive& start, int id, float distance);

    // ---- Limits ----

    // Valid authored limits: twist within (-180, 180) and low <= high, swings within (0, 180).
    inline constexpr float MaxTwist = 179.0f, MinSwing = 0.5f, MaxSwing = 179.0f;
    // The constraint's world frame on a body at `boneFrame` (+X twist, +Y swing 1, +Z swing 2).
    glm::mat4 ConstraintFrame(const glm::mat4& boneFrame, const RagdollConstraint& constraint);
    // A point on the twist arc at `degrees` about the frame's +X (0 at +Y, 90 at +Z).
    glm::vec3 TwistPoint(const glm::mat4& frame, float degrees, float radius);
    // The twist axis (+X) swung by `degrees` about +Y (axis 1) or +Z (axis 2).
    glm::vec3 SwingPoint(const glm::mat4& frame, int axis, float degrees, float radius);
    // The twist angle (degrees) under a pointer ray, on the frame's YZ plane.
    std::optional<float> TwistAngleAt(const glm::mat4& frame, glm::vec3 origin, glm::vec3 direction);
    // The swing (degrees, 0..180) from +X under a pointer ray, on the plane the swing about `axis`
    // sweeps (XZ for axis 1, XY for axis 2).
    std::optional<float> SwingAngleAt(const glm::mat4& frame, int axis, glm::vec3 origin, glm::vec3 direction);
    // Writes a dragged limit, clamped to the valid range (a twist end never passes the other).
    void SetTwistLimit(RagdollConstraint& constraint, bool high, float degrees);
    void SetSwingLimit(RagdollConstraint& constraint, int axis, float degrees);

    // ---- Collision pairs ----

    bool IsCollisionDisabled(const RagdollData& data, const std::string& a, const std::string& b);
    // Adds the pair once (either order counts); never a body with itself or a bone without a body.
    bool DisableCollision(RagdollData& data, const std::string& a, const std::string& b);
    bool EnableCollision(RagdollData& data, const std::string& a, const std::string& b);

    // ---- Validation ----

    // What the engine says about the data: ValidateRagdollData's message, then BindRagdoll's against
    // the skeleton when there is one; empty when the ragdoll binds.
    std::string Problem(const RagdollData& data, const SkeletonData* skeleton, AssetID skeletonID);

    // ---- Drawing ----

    inline constexpr glm::vec4 BodyColor {0.8f, 0.35f, 1.0f, 1.0f};
    inline constexpr glm::vec4 SelectedBodyColor {1.0f, 0.85f, 0.3f, 1.0f};
    inline constexpr glm::vec4 LimitColor {1.0f, 0.7f, 0.0f, 1.0f};
    // A body's primitives at `boneFrame`.
    void DrawBody(DebugDraw& draw, const glm::mat4& boneFrame, const RagdollBody& body, glm::vec4 color);
    // The constraint's twist arc and elliptical swing cone at `frame`.
    void DrawLimits(DebugDraw& draw, const glm::mat4& frame, const RagdollConstraint& constraint, float radius);
    // The limit radius of the scene view's ragdoll debug draw (metres).
    inline constexpr float SceneLimitRadius = 0.15f;
    struct SceneRagdollCounts
    {
        int Bodies = 0, Joints = 0;
    };
    // The scene view's Ragdolls debug flag outside play: every entity whose ragdoll binds to its
    // Animator's skeleton (the asset through the scene's asset manager, never blocking, or the
    // code-bound source) draws its bodies' primitives at the current animated pose (the bind pose
    // before one is evaluated) and, for each body joined to a parent body, the constraint frame and
    // its limit cone. Nothing without the flag, and nothing while the scene runs: the physics world
    // draws its own ragdolls then.
    SceneRagdollCounts DrawSceneRagdolls(DebugDraw& draw, const Scene& scene, PhysicsDebugFlags flags);

    // ---- Files ----

    std::variant<RagdollData, AssetError>           LoadRagdollFile(const std::filesystem::path& file);
    std::optional<AssetError>                       SaveRagdollFile(const std::filesystem::path& file, const RagdollData& data);
    // "<stem>.ragdoll" in `directory`, numbered when taken.
    std::variant<std::filesystem::path, AssetError> CreateRagdollFile(const std::filesystem::path& directory,
                                                                      const std::string&           stem,
                                                                      const RagdollData&           data);

    // ---- Session ----

    // A .ragdoll being edited. Edits change a working copy; each finished edit is one step of the
    // session's own undo stack (the asset is a file, not part of the scene, so the scene history
    // neither records nor reverts it). During a drag the data changes freely and Commit() makes the
    // whole drag one step. Save writes the file; the editor then rebuilds it.
    class RagdollSession
    {
    public:
        void Begin(std::filesystem::path file, RagdollData data);
        void Close();
        bool IsOpen() const { return m_Open; }
        const std::filesystem::path& GetFile() const { return m_File; }
        const RagdollData&           GetData() const { return m_Data; }

        // Replaces the working copy without a step (a drag or a field being dragged).
        void Change(const RagdollData& data);
        // Makes the changes since the last step one step; false when nothing changed.
        bool Commit(const std::string& label = "Edit ragdoll");
        // One immediate step: `edit` changes a copy and returns whether it did.
        bool Apply(const std::string& label, const std::function<bool(RagdollData&)>& edit);
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        std::vector<std::string> UndoLabels() const;
        // Unsaved: the current step is not the one saved (undoing back to it is clean again).
        bool IsDirty() const { return CurrentStep() != m_SavedStep || !Equal(m_Data, m_Committed); }
        // Changes with every change, undo and redo.
        uint64_t GetVersion() const { return m_Version; }
        std::optional<AssetError> Save();
        // Closing: at once when nothing is unsaved; otherwise the editor asks (IsAskingToClose) until
        // ResolveClose: Save closes once the file is written (by `save` when given, true on success),
        // Discard closes, Cancel keeps it open.
        void RequestClose(std::function<void()> closed);
        bool IsAskingToClose() const { return m_Closing.Asking(); }
        void ResolveClose(UnsavedChanges::Choice choice, const std::function<bool()>& save = {});

    private:
        struct Step
        {
            RagdollData Before, After;
            std::string Label;
            uint64_t    ID = 0;
        };
        uint64_t CurrentStep() const { return m_Undo.empty() ? 0 : m_Undo.back().ID; }

        bool                  m_Open = false;
        std::filesystem::path m_File;
        RagdollData           m_Data, m_Committed;
        std::vector<Step>     m_Undo, m_Redo;
        uint64_t              m_Version = 0, m_SavedStep = 0, m_NextStep = 0;
        UnsavedChanges        m_Closing;
    };

    // ---- Preview ----

    // What the preview shows: the model (its skinned meshes) or the bare skeleton.
    struct PreviewSource
    {
        AssetID                  Skeleton;
        Ref<const SkeletonAsset> SkeletonSource;
        AssetID                  Model;      // Invalid: the bare skeleton.
        AssetID                  Controller; // The animated pose that pose matching follows.
    };

    // ---- Create Ragdoll ----

    // What Create Ragdoll generates from: the skeleton and model to preview, and the skinned mesh that
    // fits the bodies' radii (none when the import has no mesh on the skeleton).
    struct CreateSource
    {
        PreviewSource        Source;
        Ref<const MeshAsset> Mesh;
    };
    // From one import's products (`target` only): its skeleton, or `skeleton` when valid (a skeleton
    // product), its model, and the first mesh skinned to that skeleton; loaded through `assets`.
    std::variant<CreateSource, std::string> SourceFromImport(const std::vector<ProductRecord>& products,
                                                             SourceAssetID                     import,
                                                             AssetID                           skeleton,
                                                             AssetManager&                     assets,
                                                             const std::string&                target);
    // From an entity's Animator skeleton (loaded when the Animator has not yet): the controller it
    // plays, its prefab's model, and the skinned mesh under it on that skeleton, else the mesh and
    // model of the skeleton's import. The reason when the entity has no skeleton to use.
    std::variant<CreateSource, std::string> SourceFromEntity(const Scene&                      scene,
                                                             Entity                            entity,
                                                             AssetManager*                     assets,
                                                             const std::vector<ProductRecord>& products,
                                                             const std::string&                target);

    // The ragdoll simulated in a private scene with a ground plane: Simulate drops it from the
    // current pose, Reset puts it back. It works on a copy of the data and never touches the edited
    // scene or the asset.
    class RagdollPreview
    {
    public:
        RagdollPreview();
        ~RagdollPreview();
        void              SetSource(const PreviewSource& source, AssetManager* assets);
        const Ref<Scene>& GetScene() const { return m_Scene; }
        bool              HasSkeleton() const;
        const SkeletonData* GetSkeleton() const;

        void Simulate(const RagdollData& data);
        void Reset();
        bool IsSimulating() const { return m_Simulating; }
        void Step(double seconds);
        // Drives toward the animated pose (needs a controller on the source).
        bool CanPoseMatch() const { return m_Source.Controller.IsValid(); }
        void SetPoseMatching(bool on);
        bool IsPoseMatching() const { return m_PoseMatching; }
        // The body's world frame while simulating (the bind frame otherwise); empty without a body.
        std::optional<glm::mat4> BodyFrame(const std::string& bone) const;
        float                    GetGroundHeight() const { return m_Ground; }
        // Errors the preview's physics reported.
        std::vector<std::string> GetErrors() const;

    private:
        void Build();

        PreviewSource          m_Source;
        AssetManager*          m_Assets = nullptr;
        Ref<Scene>             m_Scene;
        UUID                   m_Character = 0;
        std::vector<glm::mat4> m_Bind;
        float                  m_Ground = 0.0f;
        bool                   m_Simulating = false, m_PoseMatching = false;
    };
} // namespace Kans::RagdollEditing
