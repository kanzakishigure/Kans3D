#pragma once
#include <imgui.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <variant>
#include "Kans3D/Editor/EditorCamera.h"
#include "Kans3D/UI/UIInput.h"
#include "ClothPaintTool.h"
#include "Gizmo/GizmoTargets.h"
#include "Gizmo/TransformGizmo.h"
#include "HierarchyPanel.h"
#include "MeshEditTool.h"
#include "Physics/VehicleFieldEditing.h"
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

        void Draw();

        bool          IsHovered() const { return m_Hovered; }
        // Whether a transform gizmo drag is in progress (the editor records undo steps after it).
        bool          IsGizmoDragging() const { return m_Gizmo.IsDragging() || m_MeshTool.GetGizmo().IsDragging(); }
        Tool          GetTool() const { return m_Tool; }
        void          SetTool(Tool tool) { m_Tool = tool; }
        void          SetGizmoOrientation(Gizmo::Orientation orientation) { m_Orientation = orientation; }
        void          SetPivotMode(Gizmo::PivotMode mode) { m_PivotMode = mode; }
        const Gizmo::TransformGizmo& GetGizmo() const { return m_Gizmo; }
        bool          IsNavigating() const { return m_Navigation != Navigation::None; }
        EditorCamera& GetCamera() { return m_Camera; }
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
        // The view-axis widget: click a ball to look along it, drag to orbit.
        void DrawViewAxes();
        void DrawStatsOverlay();
        // Projects a world point to the screen; false when behind the camera.
        bool ToScreen(const glm::vec3& point, ImVec2& screen) const;
        void HandleNavigation();
        void HandleShortcuts();
        void DrawToolbar();
        void DrawGizmo();
        Gizmo::Settings GizmoSettings() const;
        void            DrawGizmoReadout(const Gizmo::TransformGizmo& gizmo);
        // Physics (ViewportPhysics.cpp): the selection's collider, controller and joint gizmos,
        // the View menu's debug toggles, and the collider and hinge-limit handles.
        void DrawPhysicsOverlay();
        void DrawPhysicsMenu();
        void PhysicsHandles(Entity selected);
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
        // Clickable overlay icons of this frame, and the view-axis widget's area.
        std::vector<std::pair<ImVec2, UUID>> m_Icons;
        ImVec2                               m_CubeMin {}, m_CubeMax {};
        // The view-axis widget: the pressed ball (-1: the background), whether the press became an
        // orbit drag, and an alignment in progress (yaw and pitch, eased over ViewAnimationSeconds).
        int       m_AxisPressed = -1;
        bool      m_AxisPressActive = false, m_AxisOrbiting = false;
        glm::vec2 m_AxisPress {0};
        struct ViewAnimation
        {
            std::pair<float, float> From, To;
            float                   Time = 0.0f;
            EditorCamera::View      Target;
        };
        std::optional<ViewAnimation> m_ViewAnimation;

        // Transform tool, orientation, pivot and snapping (the toolbar's settings).
        Tool                  m_Tool        = Tool::Move;
        Gizmo::Orientation    m_Orientation = Gizmo::Orientation::Local;
        Gizmo::PivotMode      m_PivotMode   = Gizmo::PivotMode::Median;
        bool                  m_GizmoSnap   = false;
        float                 m_GizmoSize   = 110.0f; // Axis length in pixels at scale 1.
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
        // The physics handle being dragged: its owner, which handle, the pointer where the drag
        // started, the screen offset one shape unit along the handle makes, and the shape then.
        struct PhysicsDrag
        {
            using ShapeVariant  = std::variant<std::monostate, Physics::Box, Physics::Sphere, Physics::Capsule, Physics::Plane,
                                               VehicleWheel, VehicleFieldEditing::FieldShapeValues>;
            ImGuiID      Handle = 0;
            ImVec2       Start {};
            glm::vec2    PixelsPerUnit {0.0f};
            ShapeVariant Shape;
        };
        std::optional<PhysicsDrag> m_PhysicsDrag;
        bool                       m_PhysicsHandleHot = false; // Hovered or dragged: the transform gizmo waits.
        std::function<int(Entity)> m_SelectedWheel;
        ClothPaintTool             m_ClothPaint;
    };
} // namespace Kans
