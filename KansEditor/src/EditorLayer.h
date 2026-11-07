#pragma once
#include <array>
#include <deque>
#include <future>
#include <set>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Project/Project.h"
#include <Kans3D.h>

#include "KansEditorCore/EditorCamera.h"
#include "KansEditorCore/EditorResources.h"
#include "Kans3D/Renderer/MaterialPreviewRenderer.h"
#include "Kans3D/Scene/ScenePlaySession.h"
#include "Kans3D/Script/ScriptBuild.h"
#include "Launcher/ProjectBrowser.h"
#include "Panels/AnimatorGraphPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/FrameGraphPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/PhysicsMaterialPanel.h"
#include "Panels/VfxModuleEditorPanel.h"
#include "Panels/VfxSystemEditorPanel.h"
#include "Panels/ProductImportPanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/PropertiesPanel.h"
#include "Panels/RagdollEditorPanel.h"
#include "Panels/TimelinePanel.h"
#include "Panels/UIDesignerPanel.h"
#include "Panels/ViewportPanel.h"
#include "SceneHistory.h"
#include "UndoRouting.h"
#include "UI/ConsolePanel.h"
#include "UI/PanelManager.h"
#include "UI/SceneFileDialog.h"
#include "UnsavedChanges.h"
#include "ViewportClient.h"
namespace Kans
{
    // Identities of the editor panels (see PanelManager).
    namespace PanelID
    {
        inline constexpr const char* Viewport       = "Viewport";
        inline constexpr const char* Hierarchy      = "Hierarchy";
        inline constexpr const char* Properties     = "Properties";
        inline constexpr const char* ContentBrowser = "ContentBrowser";
        inline constexpr const char* Console        = "Console";
        inline constexpr const char* Stats          = "Stats";
        inline constexpr const char* AssetImporter  = "AssetImporter";
        inline constexpr const char* MaterialEditor = "MaterialEditor";
        inline constexpr const char* FrameGraph     = "FrameGraph";
        inline constexpr const char* AnimatorGraph  = "AnimatorGraph";
        inline constexpr const char* Timeline       = "Timeline";
        inline constexpr const char* UIDesigner     = "UIDesigner";
        inline constexpr const char* ProjectSettings = "ProjectSettings";
        inline constexpr const char* PhysicsMaterial = "PhysicsMaterial";
    } // namespace PanelID

    class EditorLayer : public Kans::Layer
    {

    public:
        explicit EditorLayer(std::filesystem::path projectFile);
        virtual ~EditorLayer() = default;

        virtual void OnAttach() override;
        virtual void OnDetach() override;

        void         OnUpdate(Kans::TimeStep ts) override;
        void         OnEvent(Kans::Event& e) override;
        virtual void OnImGuiRender() override;

    private:
        // ---- Assets and scenes (EditorLayerAssets.cpp) ----
        void                                     OpenProject();
        std::filesystem::path m_ProjectFile, m_StartupScene, m_LayoutFile;
        std::string           m_ProjectName;
        // Points ImGui at the project's layout file and loads it.
        void LoadLayout();
        // ---- Projects (EditorLayerUI.cpp) ----
        void DrawProjectMenu();
        void DrawProjectBrowser();
        // Starts an editor for `project` and closes this one.
        void SwitchProject(const std::filesystem::path& project);
        Scope<ProjectBrowser> m_ProjectBrowser;
        bool                  m_ProjectBrowserRequested = false;
        std::variant<std::monostate, AssetError> RefreshProductAssets(Ref<const AssetRegistry> snapshot = {});
        std::vector<EditorAssetChoice>           CollectAssetChoices(const AssetRegistry& registry) const;
        // Adds a dropped model to the scene, importing it first if needed.
        // `parent` (may be null) receives the placed model entity; `position`
        // (world space) is where it goes, the parent's origin otherwise.
        void PlaceAsset(const std::filesystem::path& file, Entity parent = {}, std::optional<glm::vec3> position = {});
        // The source of a model file in the project, if it is one.
        std::optional<SourceAssetID>             FindModelSource(const std::filesystem::path& file);
        void                                     PlacePendingModel();
        void                                     StartPendingPlacementBuild();
        // Builds the source of `file` (content browser "Import" / "Reimport").
        void                                     ImportSource(const std::filesystem::path& file);
        // Writes a model's embedded materials to editable .material files next to it
        // (in a Materials folder), makes the model use them and rebuilds it.
        void                                     ExtractMaterials(const std::filesystem::path& file);
        // Writes `<Name>_Instance.material` next to `file` with it as the parent, builds it and opens it.
        void                                     CreateMaterialInstance(const std::filesystem::path& file);
        // Writes `<directory>/<stem>_Instance.material` (numbered when taken) with `parent`, and
        // starts its build; the written file, or empty on failure.
        std::filesystem::path
        WriteMaterialInstance(AssetID parent, const std::filesystem::path& directory, const std::string& stem);
        // A slot row's Create Material Instance: the instance goes beside `source` (the parent's
        // .material, or the model holding it) and replaces the material on `parts` once imported.
        void CreateSlotInstance(AssetID parent, const std::filesystem::path& source, const SlotParts& parts);
        void AssignPendingSlotInstances();
        // ---- Secondary motion (EditorLayerSecondaryMotion.cpp) ----
        // Cloth card actions, the painting session's save hook and the preset folders.
        void BindSecondaryMotion();
        // Writes a .cloth for `section` of the entity's mesh (beside the mesh's source, or in the
        // Content Browser's folder), imports it and assigns it once published.
        void CreateClothAsset(Entity entity, uint32_t section, bool browserFolder);
        void AssignPendingCloth();
        // Paints the entity's cloth asset in the viewport (its .cloth source file).
        void BeginClothPaint(Entity entity);
        // Leaves painting; unsaved strokes are saved first (switching scenes, Play).
        void FinishClothPaint();
        // A .cloth file opened in the Content Browser: paint it on the selected entity when that
        // uses it, else show what it is bound to.
        void OpenClothFile(const std::filesystem::path& file);
        void DrawClothInfo();
        // The source file of an asset, or empty.
        std::filesystem::path SourceFileOf(AssetID asset);
        AssetID               FindPublished(const std::filesystem::path& file, AssetType type);
        struct PendingCloth
        {
            std::filesystem::path File;
            UUID                  Entity  = 0;
            uint32_t              Section = 0;
        };
        std::vector<PendingCloth>            m_PendingCloth;
        std::optional<std::filesystem::path> m_ClothInfo;
        std::optional<ClothData>             m_ClothInfoData;
        std::string                          m_ClothInfoError;
        // ---- Ragdolls (EditorLayerRagdoll.cpp) ----
        // The Ragdoll card's and Content Browser's actions, and the ragdoll editor's preview and save.
        void BindRagdolls();
        // A .ragdoll from the Content Browser or the card: the editor with the model it was made for.
        void OpenRagdollFile(const std::filesystem::path& file, Entity character = {});
        void OpenRagdollAsset(AssetID ragdoll);
        // The generation dialog for a skinned entity (assigned to it once imported) or a model file.
        void CreateRagdollFor(Entity character);
        void CreateRagdollForModel(const std::filesystem::path& model);
        // A skeleton product (the Content Browser's product list): the model and mesh of its import.
        void CreateRagdollForSkeleton(AssetID skeleton);
        void BeginRagdollFromImport(const std::filesystem::path& file, SourceAssetID import, AssetID skeleton);
        void AssignPendingRagdolls();
        // The preview scene rendered through the ragdoll editor's camera.
        uintptr_t RenderRagdollPreview(const Ref<Scene>& scene, const EditorCamera& camera, glm::uvec2 size,
                                       const std::vector<GizmoDraw>& gizmo);
        struct PendingRagdoll
        {
            std::filesystem::path File;
            UUID                  Entity = 0;
        };
        std::vector<PendingRagdoll> m_PendingRagdolls;
        RagdollEditorPanel          m_RagdollEditor;
        // One module graph editor per open .vfxmodule.
        std::vector<std::unique_ptr<VfxModuleEditorPanel>> m_VfxModuleEditors;
        std::vector<std::filesystem::path>                 m_PendingVfxModules;
        VfxModuleCreateDialog                              m_VfxModuleCreate;
        void                                               OpenVfxModule(const std::filesystem::path& file);
        void                                               DrawVfxModuleEditors();
        // One system and emitter editor per open .vfxsystem or .vfxemitter.
        std::vector<std::unique_ptr<VfxSystemEditorPanel>> m_VfxSystemEditors;
        std::vector<std::filesystem::path>                 m_PendingVfxSystems;
        VfxSystemCreateDialog                              m_VfxSystemCreate;
        void                                               OpenVfxSystem(const std::filesystem::path& file);
        // Project products of `type` with their source files.
        std::vector<std::pair<AssetID, std::filesystem::path>> ProjectVfxAssets(AssetType type);
        // The scene viewport's handles for particle entities, and the read-only copies of the
        // systems they show while no system editor has them open (reloaded when the file changes).
        VfxGizmos::SceneHandles m_VfxSceneHandles;
        struct VfxSceneDocument
        {
            VfxSystemEditing::SystemDocument Document;
            VfxSystemEditing::ModuleLibrary  Library;
            VfxSystemEditing::Context        Context;
            std::filesystem::file_time_type  Written {};
        };
        std::map<std::filesystem::path, std::unique_ptr<VfxSceneDocument>> m_VfxSceneDocuments;
        void BindVfxSceneHandles();
        VfxGizmos::SceneHandles::Source VfxSceneSource(AssetID system, bool forEditing);
        RagdollPreviewRenderer      m_RagdollPreview;
        // Published Material of a .material source, or invalid.
        AssetID                                  FindPublishedMaterial(const std::filesystem::path& file);
        // Content-browser thumbnail of a .material file: rendered a few per frame,
        // re-rendered when the material is rebuilt. Null until ready.
        Ref<Texture2D> MaterialThumbnail(const std::filesystem::path& file, uint32_t size);
        struct MaterialThumbnailEntry
        {
            AssetID                      Material;
            Ref<const MaterialDataAsset> Rendered;
            Ref<Texture2D>               Texture;
            uint32_t                     Size = 0;
        };
        std::unordered_map<std::string, MaterialThumbnailEntry> m_MaterialThumbnails;
        int                                                     m_ThumbnailBudget = 0;
        Scope<MaterialPreviewRenderer>                          m_MaterialPreview, m_ThumbnailRenderer;
        AssetID                                  FindPublishedModel(SourceAssetID source) const;
        std::optional<SourceAssetID>             m_PendingPlacement;
        // Instances created from slot rows, assigned to their slots when their build is published.
        std::vector<std::pair<std::filesystem::path, SlotParts>> m_PendingSlotInstances;
        UUID                                     m_PendingPlacementParent = 0;
        std::optional<glm::vec3>                 m_PendingPlacementPosition;
        // Dropped while another build ran: build it once that build completes.
        bool                                     m_PendingPlacementWaiting = false;
        // Sources imported without being asked, one build after another: assets the open
        // scene references that were never imported (e.g. the materials of a scene copied
        // into the project) and every shader program, so it can be picked in materials.
        // Each source is queued once per session, so a failing import is not retried in a loop.
        void                                     QueueMissingSceneAssets();
        void                                     QueueUnimportedShaders();
        void                                     QueueAutoImport(SourceAssetID source);
        void                                     StartNextAutoImport();
        std::deque<SourceAssetID>                m_AutoImports;
        std::set<SourceAssetID>                  m_AutoImported;
        std::variant<std::monostate, AssetError> ActivateScene(Ref<Scene> scene, std::filesystem::path path);
        std::variant<std::monostate, AssetError> NewScene();
        std::variant<std::monostate, AssetError> OpenScene(const std::filesystem::path& path);
        std::variant<std::monostate, AssetError> SaveScene(const std::filesystem::path& path);
        // Logs an error and shows it in the main toolbar.
        void Record(const std::variant<std::monostate, AssetError>& result);
        void SaveCurrentScene();
        // Runs `action` (described by `what`, e.g. "opening another scene") at once, or after
        // asking to save the scene's unsaved changes.
        void ConfirmDiscard(std::string what, std::function<void()> action);
        void DrawUnsavedPrompt();
        UnsavedChanges m_Unsaved;
        bool           m_UnsavedPromptRequested = false;
        void DrawSceneFileDialogs();

        // ---- UI (EditorLayerUI.cpp) ----
        void RegisterPanels();
        void DrawMenuBar();
        void DrawToolbar();
        void DrawStats();
        void HandleShortcuts();
        // Records a history step once an interaction that may have edited the scene ends.
        void TrackSceneEdits();
        // `routed`: the VFX asset edited in the viewport takes part (UndoRouting); the History
        // menu lists the scene's own steps and passes false.
        void UndoScene(int steps = 1, bool routed = true);
        // Play mode: the scene runs from a snapshot that Stop restores.
        void StartPlay();
        // Simulate: physics without scripts, restored on stop like play.
        void StartSimulate();
        void StopPlay();
        void TogglePause();
        // Returns the right edge of the controls.
        float DrawPlayControls(ImVec2 position);
        void RedoScene(int steps = 1, bool routed = true);
        // The selected particle entity's system document while "Edit asset in viewport" is on and
        // its system editor is open; null otherwise.
        VfxSystemEditing::SystemDocument* ViewportVfxDocument();
        void OpenImporter(const std::filesystem::path& file);
        void OpenMaterial(const std::filesystem::path& file);
        void CreateMaterial(const std::filesystem::path& directory);
        void  OpenPhysicsMaterial(const std::filesystem::path& file);

        // Project C# scripts: <project root>/Scripts/GameScripts.csproj, built with the SDK.
        std::filesystem::path ScriptProjectPath() const;
        std::filesystem::path ScriptAssemblyPath() const;
        void                  CreateScriptProject();
        void                  BuildScripts();
        void                  PollScriptBuild();
        void                  LoadProjectScripts();
        void                  DrawScriptsMenu();

        enum class SceneDialog
        {
            None,
            Open,
            Save
        };
        SceneDialog            m_SceneDialog = SceneDialog::None;
        std::filesystem::path  m_ScenePath;
        SceneFileDialog        m_SceneFileDialog;
        // Last editor error, shown in the main toolbar until dismissed.
        std::string m_SceneFileError;

        SceneHistory     m_History;
        ScenePlaySession m_Play;
        // The script build runs on a worker thread; loading happens on the main thread.
        std::future<ScriptBuild::Result> m_ScriptBuild;
        bool                             m_ScriptsLoaded = false;
        std::string                      m_ScriptStatus;
        bool         m_EditPending = false;

        Scope<EditorAssetImportService> m_AssetImports;
        Scope<AssetManager>             m_Assets;
        // Name of the source being imported, for the toolbar status.
        SourceAssetID m_StatusSource;
        std::string   m_StatusName;


        Ref<Scene>         m_ActiveScene;
        Scope<ViewportClient> m_Viewport;

        Entity m_CameraEntity;
        Entity m_SecondCameraEntity;

        PanelManager        m_Panels;
        HierarchyPanel      m_HierarchyPanel;
        PropertiesPanel     m_PropertiesPanel {m_HierarchyPanel};
        ViewportPanel       m_ViewportPanel {m_HierarchyPanel};
        ContentBrowserPanel m_ContentBrowserPanel;
        ProductImportPanel  m_ProductImportPanel;
        MaterialEditorPanel m_MaterialEditorPanel;
        FrameGraphPanel     m_FrameGraphPanel;
        AnimatorGraphPanel  m_AnimatorGraphPanel {m_HierarchyPanel};
        TimelinePanel       m_TimelinePanel {m_HierarchyPanel};
        UIDesignerPanel     m_UIDesignerPanel;
        ProjectSettingsPanel m_ProjectSettingsPanel;
        PhysicsMaterialPanel m_PhysicsMaterialPanel;
        // Keys and text typed while playing with the viewport focused, for the game's UI.
        UIInputState m_UIInput;
        // Editor view frame times (ms) for the Stats panel's graph, oldest first.
        std::array<float, 240> m_FrameTimes {};
        size_t                 m_FrameTimeCursor = 0;
        ConsolePanel        m_Console;
    };

} // namespace Kans
