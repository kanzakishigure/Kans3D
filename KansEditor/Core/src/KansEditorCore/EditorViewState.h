#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include "Kans3D/Renderer/RenderView.h"
#include "KansEditorCore/EditorCamera.h"
#include "KansEditorCore/Gizmo/GizmoTargets.h"
#include "KansEditorCore/Gizmo/TransformGizmo.h"

namespace Kans
{
    // The scene view's camera as the editor restores it: the navigation state and the name of the
    // axis view it was last set to (the camera button's label).
    struct EditorViewCamera
    {
        EditorCamera::State Camera;
        std::string         View = "Perspective";
    };
    bool        operator==(const EditorViewCamera& a, const EditorViewCamera& b);
    inline bool operator!=(const EditorViewCamera& a, const EditorViewCamera& b) { return !(a == b); }

    // The scene view's display options (its View menu) and toolbar settings. Editor preferences:
    // the grid, outline, stencil and debug-normal switches live in the scene's RenderWorld while
    // editing, but are never written into the scene file.
    struct ViewportOptions
    {
        // View menu.
        bool          AmbientOcclusionBuffer = false;
        int           ReflectionDebug        = 0; // 0 off, 1 radiance, 2 hit mask.
        SceneViewMode ViewMode               = SceneViewMode::Lit;
        bool          ShowIcons = true, ShowStats = false;
        float         GizmoSize       = 85.0f; // Pixels at scale 1.
        bool          RPicksScaleTool = false;
        bool          Grid            = true;
        GridSettings  GridLook;
        bool          Outline = false, Stencil = false, DebugNormals = false;
        uint32_t      PhysicsView  = 0; // PhysicsDebugFlags.
        bool          ShowAllAudio = false, ShowOcclusionRays = false;
        bool          EditorCameraInPlay = false;
        int           GameResolution     = 0; // Index into the viewport's list; 0 fits the panel.
        // Toolbar.
        uint8_t            Tool        = 1; // ViewportPanel::Tool (Move).
        Gizmo::Orientation Orientation = Gizmo::Orientation::Local;
        Gizmo::PivotMode   Pivot       = Gizmo::PivotMode::Median;
        bool               Snap        = false;
        float              SnapMove = 0.5f, SnapRotate = 15.0f, SnapScale = 0.1f;
    };
    bool        operator==(const ViewportOptions& a, const ViewportOptions& b);
    inline bool operator!=(const ViewportOptions& a, const ViewportOptions& b) { return !(a == b); }

    // Per-project editor view state in <project>/Cache/Editor/ViewState.yaml: the viewport options
    // (one set for the project) and the editor camera of each scene, keyed like the Hierarchy's
    // eye column (EditorVisibilityStore::SceneKey). A missing, unreadable or partly broken file
    // gives defaults for what cannot be read; unknown keys are ignored.
    //
    // Track is called every frame with the live camera and options. It records the camera under
    // the bound scene except while the scene plays (Play and Simulate keep the Edit camera saved
    // before them), and writes the file once nothing has changed for DebounceSeconds, so a crash
    // loses at most the last second. Flush writes pending changes at once (scene switch, close).
    class EditorViewStateStore
    {
    public:
        static constexpr double DebounceSeconds = 1.0;

        explicit EditorViewStateStore(std::filesystem::path file = {});
        // Reads the file, replacing what this store holds.
        void                   Load();
        const ViewportOptions& Options() const { return m_Options; }
        // Binds a scene by its key (empty: an unsaved scene, which keeps no camera) and returns its
        // saved camera, if any.
        std::optional<EditorViewCamera> BindScene(std::string key);
        const std::string&              SceneKey() const { return m_Key; }
        // Save As: the bound scene's camera is kept under the new key too.
        void                            RenameScene(std::string key);
        std::optional<EditorViewCamera> Camera(const std::string& key) const;
        void Track(const EditorViewCamera& camera, const ViewportOptions& options, bool playing, double seconds);
        bool IsDirty() const { return m_Dirty; }
        // Writes pending changes now; false when nothing could be written.
        bool Flush();

    private:
        bool                                    Save();
        std::filesystem::path                   File() const;
        std::filesystem::path                   m_File;
        ViewportOptions                         m_Options;
        std::map<std::string, EditorViewCamera> m_Cameras;
        std::string                             m_Key;
        bool                                    m_Dirty      = false;
        double                                  m_LastChange = 0.0;
    };
} // namespace Kans
