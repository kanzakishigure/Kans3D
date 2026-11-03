#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>
#include "Kans3D/Editor/EditorAssetChoice.h"
#include "Gizmo/Handles.h"
#include "Gizmo/ModalTransform.h"
#include "Gizmo/TransformGizmo.h"
#include "Kans3D/Editor/EditorCamera.h"
#include "Physics/RagdollEditing.h"

namespace Kans
{
    class AssetManager;

    // Edits a .ragdoll asset (in the manner of UE's Physics Asset Editor): a skeleton tree marking
    // the bones with bodies and constraints, a preview of the model (or bare skeleton) with the body
    // primitives, the constraint limit cones and their handles, and the details of the selected
    // body and constraint. Edits are steps of the session's own undo stack; Simulate drops the
    // ragdoll in a private preview scene; Save writes the file and the editor rebuilds it.
    class RagdollEditorPanel
    {
    public:
        enum class Selection
        {
            Body,
            Constraint
        };

        void SetAssetManager(AssetManager* assets) { m_Assets = assets; }
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }
        // Renders the preview scene through the camera at a size, with the transform gizmo's handles,
        // and returns the image to show (0: none); without one the overlay draws on a plain backdrop.
        using PreviewRenderer =
            std::function<uintptr_t(const Ref<Scene>&, const EditorCamera&, glm::uvec2, const std::vector<GizmoDraw>&)>;
        void SetPreviewRenderer(PreviewRenderer render) { m_Render = std::move(render); }
        // After a save: rebuild the file.
        void SetSavedCallback(std::function<void(const std::filesystem::path&)> saved) { m_Saved = std::move(saved); }

        // Opens a .ragdoll; `source` supplies the skeleton and model to preview (looked up through the
        // asset manager by the asset's skeleton when not given). False with the reason in the status.
        bool Open(const std::filesystem::path& file, std::optional<RagdollEditing::PreviewSource> source = {});
        // The generation dialog for a new ragdoll of `source`'s skeleton; Create writes
        // "<stem>.ragdoll" in `directory` and opens it, then calls `created` with the file.
        void BeginCreate(const std::filesystem::path&                      directory,
                         const std::string&                                stem,
                         const RagdollEditing::PreviewSource&              source,
                         Ref<const MeshAsset>                              mesh,
                         std::function<void(const std::filesystem::path&)> created);
        bool IsOpen() const { return m_Session.IsOpen() || m_Creating; }
        // Asks before dropping unsaved changes; `closed` runs once it is closed.
        void RequestClose(std::function<void()> closed = {});

        // Its own window; nothing when closed.
        void Draw();
        // The panel's contents in the current window (the screenshot test draws them directly).
        void DrawContents();
        bool HasFocus() const { return m_Focused; }
        // Whether a handle, the gizmo or a modal G/R is changing the data.
        bool IsTransforming() const { return m_Handles.IsDragging() || m_Gizmo.IsDragging() || m_Modal.IsActive(); }
        const Gizmo::HandleContext&  Handles() const { return m_Handles; }
        const Gizmo::ModalTransform& Modal() const { return m_Modal; }
        static constexpr Gizmo::HandleId GizmoHandleId = 0x7261676467697a6dull;
        bool Save();
        bool Undo();
        bool Redo();

        RagdollEditing::RagdollSession& Session() { return m_Session; }
        RagdollEditing::RagdollPreview& Preview() { return m_Preview; }
        EditorCamera&                   Camera() { return m_Camera; }
        const std::string&              Status() const { return m_Status; }
        void                            Select(const std::string& bone, Selection kind = Selection::Body);
        // Options of the generation dialog.
        RagdollGenerationOptions& Options() { return m_Options; }

    private:
        void DrawToolbar();
        void DrawTree();
        void DrawViewport(ImVec2 size);
        void DrawDetails();
        void DrawGenerate();
        void DrawClosePrompt();
        void Frame();
        bool ToScreen(glm::vec3 point, ImVec2& out) const;
        // The skeleton (from the preview) and its asset ID.
        const SkeletonData* Skeleton() const { return m_Preview.GetSkeleton(); }
        // Starts an edit gesture (a drag or a field) from the committed data.
        void Edited(const RagdollData& data);

        AssetManager*                  m_Assets = nullptr;
        PreviewRenderer                m_Render;
        std::vector<EditorAssetChoice> m_Choices;
        std::function<void(const std::filesystem::path&)> m_Saved;

        RagdollEditing::RagdollSession m_Session;
        RagdollEditing::RagdollPreview m_Preview;
        RagdollEditing::PreviewSource  m_Source;
        Ref<const MeshAsset>           m_Mesh;
        EditorCamera                   m_Camera;
        ImVec2                         m_Min {}, m_Max {};
        bool                           m_Focused = false, m_Framed = false, m_WindowOpen = true;

        std::vector<std::string> m_Selected; // Bones; the first is the primary.
        Selection                m_Kind      = Selection::Body;
        int                      m_Primitive = 0;
        int                      m_GizmoOperation = 0; // 0 none, 1 move primitive, 2 rotate primitive, 3 rotate joint frame.

        // The preview's handles (primitive sizes, limits), which share hover with the gizmo.
        Gizmo::HandleContext m_Handles;
        bool                 m_GizmoWasUsing = false;
        // Modal G and R on the selected primitive: the data and its transform when it began.
        Gizmo::ModalTransform m_Modal;
        RagdollData           m_ModalStart;
        glm::mat4             m_ModalStartWorld {1};
        bool                  m_ModalWasActive = false;
        // The move/rotate gizmo, the data and handle transform when its drag began, and the
        // handles for the next preview frame.
        Gizmo::TransformGizmo  m_Gizmo;
        RagdollData            m_GizmoStart;
        glm::mat4              m_GizmoStartWorld {1};
        std::vector<GizmoDraw> m_GizmoDraws;

        // Generation (new asset, or Regenerate).
        RagdollGenerationOptions                          m_Options;
        bool                                              m_Creating = false, m_ShowGenerate = false;
        std::filesystem::path                             m_CreateDirectory;
        std::string                                       m_CreateStem;
        std::function<void(const std::filesystem::path&)> m_Created;
        bool                                              m_SimulateRequested = false;
        std::string                                       m_Status;
    };
} // namespace Kans
