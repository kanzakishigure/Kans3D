#pragma once
#include <imgui.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <variant>
#include "KansEditorCore/EditorCamera.h"
#include "Kans3D/UI/UIInput.h"
#include "ClothPaintTool.h"
#include "Gizmo/GizmoTargets.h"
#include "Gizmo/Handles.h"
#include "Gizmo/ModalTransform.h"
#include "Gizmo/TransformGizmo.h"
#include "Gizmo/ViewCube.h"
#include "HierarchyPanel.h"
#include "MeshEditTool.h"
#include "Panels/VfxGizmos.h"
#include "Physics/VehicleFieldEditing.h"
#include "UI/DragCapture.h"
#include "ViewportClient.h"

namespace Kans
{
    // The scene view: renders the scene from the editor camera (or the scene's
    // primary camera while playing), handles camera navigation, the transform
    // gizmo, the overlay toolbars and files dropped from the content browser.
    //
    // Navigation: right drag looks around (WASD/QE fly, Shift faster, wheel
    // changes the speed), Alt+left drag orbits, middle drag pans, the wheel
    // dollies, F frames the selection. Q/W/E/R/Y pick the tool (select, move, rotate, scale,
    // all three). The transform gizmo (Gizmo/TransformGizmo) moves the selection about its
    // median point or the active entity; Esc or a right click cancels a drag, and releasing
    // it is one undo step. The axis widget in the corner aligns or orbits the view.
    //
    // Over the view, G, R and S move, rotate and scale the selection from the pointer
    // (Gizmo/ModalTransform, as in Blender), and in face or edge mode E extrudes and moves; R
    // then is not the scale tool's key there (it still is with the pointer elsewhere, or always
    // with SetRPicksScaleTool). Physics and other authored shapes are edited with handles
    // (Gizmo/Handles) that share one hover resolution with the transform gizmo.
    class ViewportPanel
    {
    public:
        enum class Tool : uint8_t
        {
            Select,
            Move,
            Rotate,
            Scale,
            Transform // Move, rotate and scale handles together.
        };
        // Shows the scene's primary camera outside play mode (timeline camera cuts in preview).
        void SetPreviewSceneCamera(bool show) { m_PreviewSceneCamera = show; }
        // Play mode: where the game image is (letterboxed at a chosen resolution) and the
        // pointer, buttons and wheel over it, for the game's UI.
        UIViewport GameViewport() const;
        void       GameInput(UIInputState& input) const;
        bool       IsFocused() const { return m_Focused; }
        // A dropped file and, for files dropped on the scene, where it should go.
        using FileDrop = std::function<void(const std::filesystem::path&, std::optional<glm::vec3>)>;
        // The published model of a dragged file (invalid when it is not one, or not imported yet).
        using ModelResolver = std::function<AssetID(const std::filesystem::path&)>;

        explicit ViewportPanel(HierarchyPanel& hierarchy) : m_Hierarchy(hierarchy) {}

        // The viewport renders `scene` through `client` (both may be null).
        void SetContext(Ref<Scene> scene, ViewportClient* client);
        void SetFileDropCallback(FileDrop callback) { m_FileDrop = std::move(callback); }
        void SetModelResolver(ModelResolver resolver) { m_ResolveModel = std::move(resolver); }
        // Adds highlights other panels ask for (material slot rows) to the selection outline.
        using HighlightSource = std::function<void(ViewHighlights&)>;
        void SetHighlightSource(HighlightSource source) { m_HighlightSource = std::move(source); }
        // A material file dropped on a model part: assign it to the part's slot under the cursor.
        using MaterialDrop = std::function<void(Entity part, uint32_t slot, const std::filesystem::path& material)>;
        void SetMaterialDropCallback(MaterialDrop callback) { m_MaterialDrop = std::move(callback); }
        // While playing, the scene renders through its primary camera unless the
        // editor camera is chosen in the toolbar.
        void SetPlaying(bool playing) { m_Playing = playing; }
        // The wheel row the Vehicle card has selected (-1: none), highlighted in the view.
        void SetWheelSelection(std::function<int(Entity)> selected) { m_SelectedWheel = std::move(selected); }
        // The handles of a selected particle entity's system (shown read-only unless the card's
        // "Edit asset in viewport" is on); null: none.
        void SetVfxHandles(VfxGizmos::SceneHandles* handles) { m_VfxHandles = handles; }
        // The toolbar's gizmo size, pick radius and snapping (other viewports' handles use them).
        Gizmo::Settings ToolbarSettings() const { return GizmoSettings(); }

        void Draw();

        bool          IsHovered() const { return m_Hovered; }
        // Whether a transform gizmo drag is in progress (the editor records undo steps after it).
        // Whether a transform is in progress: a gizmo or handle drag, or a modal G/R/S (the editor
        // records undo steps after it and holds back its own shortcuts).
        bool IsGizmoDragging() const
        {
            return m_Gizmo.IsDragging() || m_MeshTool.GetGizmo().IsDragging() || m_Modal.IsActive() || m_Handles.IsDragging() ||
                   (m_VfxHandles && m_VfxHandles->IsDragging());
        }
        // The "input captured by drag" gate (UI/DragCapture.h): this viewport's shortcuts and the
        // editor's both ask it before acting on a key.
        DragCapture Capture() const;
        bool        CapturesKey(ImGuiKey key, bool ctrl) const { return Capture().Captures(key, ctrl); }
        bool                         IsModal() const { return m_Modal.IsActive(); }
        const Gizmo::ModalTransform& GetModal() const { return m_Modal; }
        const Gizmo::HandleContext&  GetHandles() const { return m_Handles; }
        MeshEditTool&                GetMeshTool() { return m_MeshTool; }
        // The industry-standard choice for R: the scale tool even over the view (G and S stay modal).
        void SetRPicksScaleTool(bool on) { m_RPicksScaleTool = on; }
        bool RPicksScaleTool() const { return m_RPicksScaleTool; }
        // The overlay toolbar as last drawn: the left groups' right edge, the right group's left
        // edge, and how far it collapsed (0: full, 1: tight, 2: some groups in the overflow menu).
        struct ToolbarLayout
        {
            float LeftEnd = 0.0f, RightStart = 0.0f;
            int   Level = 0;
        };
        ToolbarLayout GetToolbarLayout() const { return m_ToolbarLayout; }
        // The handle ID the transform gizmo takes part in hover resolution under.
        static constexpr Gizmo::HandleId GizmoHandleId = 0x7472616E73666F72ull;
        Tool          GetTool() const { return m_Tool; }
        void          SetTool(Tool tool) { m_Tool = tool; }
        void          SetGizmoOrientation(Gizmo::Orientation orientation) { m_Orientation = orientation; }
        void          SetPivotMode(Gizmo::PivotMode mode) { m_PivotMode = mode; }
        void          SetGizmoSnap(bool snap) { m_GizmoSnap = snap; }
        const Gizmo::TransformGizmo& GetGizmo() const { return m_Gizmo; }
        bool          IsNavigating() const { return m_Navigation != Navigation::None; }
        EditorCamera& GetCamera() { return m_Camera; }
        // The view cube: its cells as last drawn, hover and press.
        const Gizmo::ViewCube& GetViewCube() const { return m_ViewCube; }
        void          FocusSelection();
        // Cloth painting (ClothPaintTool): the editor starts it from the Cloth card and routes
        // Ctrl+S / Ctrl+Z / Ctrl+Y to it while it runs.
        ClothPaintTool& GetClothPaint() { return m_ClothPaint; }
        bool            IsPaintingCloth() const { return m_ClothPaint.IsActive(); }

    private:
        enum class Navigation
        {
            None,
            Look,
            Orbit,
            Pan
        };

        void Resize(ImVec2 size);
        // The camera the scene is shown through (the primary camera while playing).
        bool      UsesSceneCamera() const;
        glm::mat4 ViewProjection() const;
        // Draws of every model entity, with IDs indexing `entities` (ID - 1).
        void                  PickAt(ImVec2 mouse);
        // The entity a click at `mouse` would select (icons first, then meshes).
        Entity EntityAt(ImVec2 mouse);
        MeshEditTool::View EditView();
        void Render();
        // Icons for lights and cameras, camera frustums and light directions.
        void DrawSceneOverlays();
        // The view cube: click a face, edge or corner to look from it, drag to orbit.
        void      DrawViewCube();
        glm::vec2 ViewCubeCentre() const;
        void DrawStatsOverlay();
        // Projects a world point to the screen; false when behind the camera.
        bool ToScreen(const glm::vec3& point, ImVec2& screen) const;
        void HandleNavigation();
        void HandleShortcuts();
        void DrawToolbar();
        void DrawGizmo();
        Gizmo::Settings GizmoSettings() const;
        Gizmo::View     GizmoView() const;
        void            DrawGizmoReadout(const std::string& text, glm::vec2 at);
        // Modal G/R/S: starts one from the keys over the view, or runs the one in progress
        // (adding its guide lines to `draws`); true while one runs this frame.
        bool UpdateModal(std::vector<GizmoDraw>& draws);
        bool StartModal(Gizmo::Operation op, bool extrude);
        void DrawModalStatus();
        // The handle frame: input for this frame's handles, before any panel declares them.
        void BeginHandles();
        // Physics (ViewportPhysics.cpp): the selection's collider, controller and joint gizmos,
        // the View menu's debug toggles, and the collider and hinge-limit handles.
        void DrawPhysicsOverlay();
        void DrawPhysicsMenu();
        void PhysicsHandles(Entity selected);
        // The selected particle entity's VFX handles (declared with the others), and their update
        // and draws once the hover is resolved.
        void VfxHandles(Entity selected);
        void EndVfxHandles(std::vector<GizmoDraw>& draws);
        // A world segment on the overlay, clipped at the near plane.
        void OverlaySegment(ImDrawList* draw, glm::vec3 a, glm::vec3 b, ImU32 color, float thickness) const;
        void AcceptDrop();
        // The surface under `mouse` (screen position), with its entity and material slot.
        ScenePickHit SurfaceAt(ImVec2 mouse);
        // A dragged material: lights up the part slot under the cursor, assigns it on drop.
        void DragMaterial(const std::filesystem::path& file, bool delivered);
        // Where a model dragged to `mouse` lands (resting on the surface under it).
        glm::vec3 DropPosition(ImVec2 mouse, AssetID model);

        HierarchyPanel&    m_Hierarchy;
        HighlightSource    m_HighlightSource;
        MaterialDrop       m_MaterialDrop;
        // The part slot a dragged material would go to, lit up in the next frame.
        std::optional<std::pair<UUID, uint32_t>> m_DropSlot;
        // Vertex/edge/face editing of editable meshes.
        MeshEditTool       m_MeshTool;
        Ref<Scene>         m_Scene;
        ViewportClient*    m_Client = nullptr;
        FileDrop           m_FileDrop;
        ModelResolver      m_ResolveModel;
        // The file being dragged over the viewport and its model, resolved once per drag.
        std::filesystem::path m_DragFile;
        AssetID               m_DragModel;
        EditorCamera       m_Camera;

        ImVec2     m_Min {}, m_Max {};
        glm::uvec2 m_Size {0, 0};
        bool       m_Hovered = false, m_Focused = false, m_ImageHovered = false;
        Navigation m_Navigation = Navigation::None;
        bool       m_Playing = false, m_UseEditorCameraInPlay = false, m_PreviewSceneCamera = false;
        int        m_GameResolution = 0; // Index into the game resolutions; 0 fits the panel.
        ImVec2     m_ImageMin {0, 0}, m_ImageSize {1, 1};

        SceneViewMode m_ViewMode  = SceneViewMode::Lit;
        GridSettings  m_Grid;
        bool          m_ShowIcons = true, m_ShowStats = false;
        // Clickable overlay icons of this frame, and the view cube's area.
        std::vector<std::pair<ImVec2, UUID>> m_Icons;
        ImVec2                               m_CubeMin {}, m_CubeMax {};
        // The view cube in the top right corner, ViewCubeHalf pixels (at scale 1) to a face.
        static constexpr float ViewCubeHalf = 27.0f;
        Gizmo::ViewCube        m_ViewCube;

        // Transform tool, orientation, pivot and snapping (the toolbar's settings).
        Tool                  m_Tool        = Tool::Move;
        Gizmo::Orientation    m_Orientation = Gizmo::Orientation::Local;
        Gizmo::PivotMode      m_PivotMode   = Gizmo::PivotMode::Median;
        bool                  m_GizmoSnap   = false;
        float                 m_GizmoSize   = 85.0f; // Axis length in pixels at scale 1.
        Gizmo::TransformGizmo m_Gizmo;
        Gizmo::GizmoTargets   m_GizmoTargets;
        // A right click that cancelled a drag; it does not start looking around too.
        bool                  m_SwallowRightClick = false;
        // Last chosen orthographic view, for the camera button's label.
        const char* m_ViewName = "Perspective";
        // The right-hand toolbar's measured width, to right-align it.
        float m_RightToolbarWidth = 300.0f;
        float m_SnapMove   = 0.5f;
        float m_SnapRotate = 15.0f;
        float m_SnapScale  = 0.1f;
        // Physics debug drawing this viewport adds (editor-local; neither the scene nor the project
        // saves it). Outside play, colliders and joints are drawn from their components.
        PhysicsDebugFlags m_PhysicsView = PhysicsDebugFlags::None;
        // Handles of this viewport (physics shapes, wheels, fields, joint limits) and the hover
        // resolution they share with the transform gizmo.
        Gizmo::HandleContext       m_Handles;
        Gizmo::Input               m_HandleInput;
        VfxGizmos::SceneHandles*   m_VfxHandles = nullptr;
        bool                       m_VfxDeclared = false;
        // Modal G/R/S and what it transforms; whether one ran this frame (its keys and clicks
        // then do nothing else).
        enum class ModalTarget : uint8_t
        {
            None,
            Entities,
            Mesh
        };
        Gizmo::ModalTransform      m_Modal;
        ModalTarget                m_ModalTarget = ModalTarget::None;
        bool                       m_ModalThisFrame = false, m_RPicksScaleTool = false, m_HandlesWereDragging = false;
        // The toolbar's widths at each collapse level when last drawn there (0: not yet).
        float                      m_ToolbarWidths[3] = {0.0f, 0.0f, 0.0f};
        ToolbarLayout              m_ToolbarLayout;
        std::function<int(Entity)> m_SelectedWheel;
        ClothPaintTool             m_ClothPaint;
    };
} // namespace Kans
