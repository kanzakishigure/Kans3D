#pragma once
#include <array>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include "KansEditorCore/EditorPanel.h"
#include "UI/IniSection.h"
#include "KansEditorCore/Panels/ContentBrowser/AssetFileOperations.h"
#include "KansEditorCore/Panels/ContentBrowser/BrowserModel.h"
#include "KansEditorCore/Panels/ContentBrowser/ThumbnailCache.h"
#include "UI/ProgressPopup.h"

namespace Kans
{
    // Browses the project's asset folder: folder tree with favorites, grid or
    // list of the current folder, and a preview with asset details. File
    // operations keep asset identity (see Browser::AssetFileOperations).
    class ContentBrowserPanel : public EditorPanel
    {
    public:
        using PathCallback = std::function<void(const std::filesystem::path&)>;

        ContentBrowserPanel();
        void onImGuiRender() override;

        // Non-owning; the editor unbinds before destroying the service.
        void Bind(EditorAssetImportService* service);
        // Running tasks, for the status bar's short progress text. Non-owning.
        void SetProgress(const EditorProgress* progress) { m_Progress = progress; }
        // Where the progress popup sits: the panel above its status bar, if the panel was drawn
        // this frame (not closed, collapsed or behind another tab).
        std::optional<ProgressPopupHost> GetProgressHost() const;

        // Double-clicked or opened file.
        void SetOpenFileCallback(PathCallback callback) { m_OpenFile = std::move(callback); }
        // "Create > Material" in a folder.
        void SetCreateMaterialCallback(PathCallback callback) { m_CreateMaterial = std::move(callback); }
        // "Create > UI" and "Create > Timeline" in a folder.
        void SetCreateUICallback(PathCallback callback) { m_CreateUI = std::move(callback); }
        void SetCreateTimelineCallback(PathCallback callback) { m_CreateTimeline = std::move(callback); }
        void SetCreatePhysicsMaterialCallback(PathCallback callback) { m_CreatePhysicsMaterial = std::move(callback); }
        // "Create > VFX Module": asks for its name, usage and stages.
        void SetCreateVfxModuleCallback(PathCallback callback) { m_CreateVfxModule = std::move(callback); }
        // "Create > VFX System / VFX Emitter": empty, from a template or inherited from an emitter.
        void SetCreateVfxSystemCallback(PathCallback callback) { m_CreateVfxSystem = std::move(callback); }
        void SetCreateVfxEmitterCallback(PathCallback callback) { m_CreateVfxEmitter = std::move(callback); }
        // "Import" / "Reimport" of a source file.
        void SetImportCallback(PathCallback callback) { m_Import = std::move(callback); }
        // "Import Settings..." of a source file.
        void SetImportSettingsCallback(PathCallback callback) { m_ImportSettings = std::move(callback); }
        // Thumbnail of a .material file (null while it renders); set by the editor.
        using MaterialThumbnailProvider =
            std::function<Ref<Texture2D>(const std::filesystem::path&, uint32_t size)>;
        void SetMaterialThumbnailProvider(MaterialThumbnailProvider provider)
        {
            m_MaterialThumbnail = std::move(provider);
        }
        // "Create Material Instance" of a .material file.
        void SetCreateInstanceCallback(PathCallback callback) { m_CreateInstance = std::move(callback); }
        // "Create Ragdoll" of a model file (its skeleton).
        void SetCreateRagdollCallback(PathCallback callback) { m_CreateRagdoll = std::move(callback); }
        // "Create Ragdoll" of a skeleton product in the details' product list.
        void SetCreateRagdollFromSkeletonCallback(std::function<void(AssetID)> callback)
        {
            m_CreateRagdollFromSkeleton = std::move(callback);
        }
        // "Extract Materials" of an imported model.
        void SetExtractMaterialsCallback(PathCallback callback) { m_ExtractMaterials = std::move(callback); }
        void SetErrorCallback(std::function<void(const std::string&)> callback) { m_ReportError = std::move(callback); }

        const std::filesystem::path& GetCurrentFolder() const { return m_Current; }
        // The selected files and folders (absolute paths).
        std::vector<std::filesystem::path> GetSelectedFiles() const
        {
            return {m_Selection.Items().begin(), m_Selection.Items().end()};
        }
        // Re-reads folders and sources on the next frame.
        void Refresh();
        // Opens the file's folder, selects the file, scrolls to it and focuses the panel.
        void Reveal(const std::filesystem::path& file);

        // View settings and favorites are saved in imgui.ini.
        void InstallSettingsHandler();
        void UninstallSettingsHandler();
        ~ContentBrowserPanel() override { UninstallSettingsHandler(); }

    private:
        enum class View
        {
            Grid,
            List
        };
        // Per-file asset state shown as a badge and in the details.
        struct SourceStatus
        {
            SourceAssetMetadata Source;
            size_t              Products = 0;
        };

        // ---- navigation and data ----
        void                         Navigate(const std::filesystem::path& directory, bool record = true);
        void                         UpdateSources(bool force);
        const SourceStatus*          StatusOf(const std::filesystem::path& file) const;
        bool                         CanImport(const std::filesystem::path& file) const;
        void                         RebuildVisible();
        std::vector<std::filesystem::path> VisiblePaths() const;
        std::vector<std::filesystem::path> Targets(const std::filesystem::path& clicked) const;

        // ---- drawing ----
        void DrawToolbar();
        void DrawBreadcrumbs(float width);
        void DrawFilterRow();
        void DrawStatusBar();
        void DrawTree();
        void DrawTreeNode(const std::filesystem::path& directory, const std::string& label);
        void DrawItems();
        void DrawGrid();
        void DrawList();
        void DrawPreview();
        void DrawContextMenus();
        void DrawDeleteDialog();
        void DrawIcon(const Browser::Entry& entry, ImVec2 min, ImVec2 max);
        void DrawStatusBadge(const Browser::Entry& entry, ImVec2 corner);
        void DrawRenameField(const Browser::Entry& entry, float width);
        void DrawItemTooltip(const Browser::Entry& entry);
        // Selection, context menu, drag and drop for the last drawn item.
        void HandleItem(const Browser::Entry& entry);
        void AcceptDrop(const std::filesystem::path& directory);
        void HandleKeyboard(int columns);

        // ---- actions ----
        void Open(const Browser::Entry& entry);
        // Index of m_ScrollTo in the listing, or -1; ScrolledTo scrolls to the last item and clears it.
        int  ScrollTarget() const;
        void ScrolledTo();
        void StartRename(const std::filesystem::path& item);
        void CommitRename();
        void RequestDelete(std::vector<std::filesystem::path> items);
        void Duplicate(const std::vector<std::filesystem::path>& items);
        void CopyToClipboard(const std::vector<std::filesystem::path>& items, bool cut);
        void Paste(const std::filesystem::path& directory);
        void MoveInto(const std::vector<std::filesystem::path>& items, const std::filesystem::path& directory);
        void NewFolder(const std::filesystem::path& parent);
        void ToggleFavorite(const std::filesystem::path& directory);
        bool IsFavorite(const std::filesystem::path& directory) const;
        void Report(const AssetError& error);
        void SelectAfterChange(const std::vector<std::filesystem::path>& paths);

        // ---- callbacks and services ----
        PathCallback m_OpenFile, m_CreateMaterial, m_Import, m_ImportSettings, m_ExtractMaterials;
        PathCallback              m_CreateUI, m_CreateTimeline, m_CreatePhysicsMaterial, m_CreateRagdoll;
        std::function<void(AssetID)> m_CreateRagdollFromSkeleton;
        PathCallback              m_CreateVfxModule, m_CreateVfxSystem, m_CreateVfxEmitter;
        MaterialThumbnailProvider m_MaterialThumbnail;
        PathCallback              m_CreateInstance;
        std::function<void(const std::string&)> m_ReportError;
        EditorAssetImportService*                m_Service = nullptr;
        const EditorProgress*                    m_Progress = nullptr;
        ProgressPopupHost                        m_ProgressHost;
        int                                      m_ProgressHostFrame = -1;
        Browser::AssetFileOperations             m_Operations;
        Browser::DirectoryCache                  m_Cache;
        Browser::ThumbnailCache                  m_Thumbnails;

        // ---- location ----
        std::filesystem::path              m_Root;
        std::filesystem::path              m_Current;
        std::vector<std::filesystem::path> m_Back, m_Forward;
        bool                               m_RevealCurrent = true; // Opens the tree down to the current folder.

        // ---- view ----
        View                   m_View       = View::Grid;
        float                  m_IconSize   = 88.0f;
        bool                   m_ShowPreview = true;
        float                  m_ColumnHeight = 0.0f; // Height of the folder, item and preview columns.
        Browser::SortKey       m_SortKey    = Browser::SortKey::Name;
        bool                   m_Ascending  = true;
        Browser::Filter        m_Filter;
        std::array<char, 256>  m_Search {};
        std::vector<Browser::Entry> m_Visible;
        int                    m_Columns = 1;

        // ---- selection and editing ----
        Browser::Selection                 m_Selection;
        std::filesystem::path              m_Renaming;
        std::array<char, 256>              m_RenameBuffer {};
        bool                               m_RenameFocus = false;
        // Bytes to select when the rename field opens: the name without its
        // extension (the whole name for folders); -1 once applied.
        int m_RenameSelection = -1;
        std::filesystem::path                   m_ScrollTo; // Scrolled into view once it is listed (Reveal).
        bool                                    m_FocusRequested = false;
        std::vector<std::filesystem::path> m_Clipboard;
        bool                               m_ClipboardCut = false;
        std::vector<std::filesystem::path> m_DragItems;
        std::vector<std::filesystem::path> m_MenuTargets; // Items the open context menu acts on.
        std::filesystem::path              m_MenuFolder;  // Folder for background / tree menus.
        bool                               m_OpenItemMenu = false, m_OpenFolderMenu = false;
        std::vector<std::filesystem::path> m_DeleteTargets;
        std::optional<Browser::DeletionPreview> m_DeletePreview;
        bool                               m_OpenDeleteDialog = false;

        // ---- favorites (stored relative to the asset folder) ----
        std::vector<std::string> m_Favorites;
        std::string              m_SavedDirectory;
        IniSection               m_Settings;

        // ---- asset state ----
        std::unordered_map<std::string, SourceStatus> m_Sources; // By normalized absolute path.
        Ref<const AssetRegistry>                      m_Registry;
        uint64_t                                      m_SourcesGeneration = ~uint64_t(0);
        double                                        m_SourcesTime       = 0.0;
        bool                                          m_SourcesDirty      = true;

        // ---- preview ----
        std::filesystem::path              m_PreviewPath;
        std::string                        m_PreviewText;
        std::optional<std::vector<SourceAssetMetadata>> m_Dependents;
    };
} // namespace Kans
