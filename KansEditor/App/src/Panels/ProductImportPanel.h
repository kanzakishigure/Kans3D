#pragma once
#include <functional>
#include <future>
#include "Kans3D/Animation/AnimationData.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/Panels/Import/ImportSettingsDraft.h"

namespace Kans
{
    // The import window of one source file, in steps: Inspect (what the file
    // contains, and what to leave out), Configure (the importer's settings and
    // presets, generated from its schema, plus the model's materials), Import
    // (progress) and Result (warnings, then place the model or open the
    // material). Settings are edited as a draft and saved when importing.
    class ProductImportPanel
    {
    public:
        enum class Stage
        {
            Inspect,
            Configure,
            Import,
            Result
        };
        using PathCallback = std::function<void(const std::filesystem::path&)>;

        // Non-owning; editor disconnects before destroying the controller.
        void Bind(EditorAssetImportService* service);
        // Selects the source of `path`; the editor opens the window.
        void OpenWithFile(const std::filesystem::path& path);
        // Result actions: place an imported model in the scene, open a material.
        void SetPlaceCallback(PathCallback callback) { m_Place = std::move(callback); }
        void SetOpenMaterialCallback(PathCallback callback) { m_OpenMaterial = std::move(callback); }
        // Contents of the window the editor's panel manager opened.
        void Draw();

        Stage                      GetStage() const { return m_Stage; }
        const ImportSettingsDraft& GetDraft() const { return m_Draft; }
        ImportSettingsDraft&       GetDraft() { return m_Draft; }
        const std::string&         GetError() const { return m_Error; }
        // Moves to Inspect or Configure (the steps that can be revisited) while nothing imports.
        void GoTo(Stage stage);
        // Saves the draft, then imports the selected source.
        void Import();

    private:
        void ReloadSources();
        void Select(const SourceAssetMetadata& source);
        // Follows builds of the selected source: Import while it runs, Result after.
        void                       Track(const EditorAssetImportSnapshot& status);
        const SourceAssetMetadata* Selected() const;
        std::filesystem::path      SelectedFile() const;
        bool                       IsImported() const;

        void DrawHeader(bool busy);
        void DrawSteps(bool busy);
        void DrawFooter(const EditorAssetImportSnapshot& status, bool busy);
        void DrawInspect(bool busy);
        void DrawModelContents(const SourceInventory& inventory);
        void DrawTextureContents(const InspectedTexture& texture);
        void DrawConfigure(bool busy);
        void DrawSetting(const ImportSettingDesc& setting);
        void DrawImport(const EditorAssetImportSnapshot& status);
        void DrawResult(const EditorAssetImportSnapshot& status);
        // Developer tools: importer name, raw settings, product IDs, move, delete.
        void DrawDeveloper(bool busy);
        void DrawDeletion();
        // Material slots of a model source: remap each to a project material or extract them.
        void DrawModelMaterials(bool busy);
        void LoadSlots();
        // Per-clip compression overrides with the statistics of the built clips.
        void DrawClipOverrides(bool busy);
        void Record(const std::variant<std::monostate, AssetError>& result);

        EditorAssetImportService*              m_Service = nullptr;
        PathCallback                           m_Place, m_OpenMaterial;
        Stage                                  m_Stage = Stage::Configure;
        bool                                   m_Force = false;
        SourceAssetID                          m_Selected;
        std::vector<SourceAssetMetadata>       m_Sources;
        std::string                            m_SourceFilter;
        ImportSettingsDraft                    m_Draft;
        std::string                            m_Importer;
        std::string                            m_Error;
        std::string                            m_NewKey;
        std::string                            m_NewValue;
        std::string                            m_MoveDestination;
        std::optional<SourceAssetDeletionPlan> m_DeletionPlan;
        // The last build state seen, to notice when a build of the selection ends.
        EditorAssetImportState m_LastState = EditorAssetImportState::Idle;
        bool                   m_Tracking  = false;

        // What the source contains; read on the first frame of Inspect after a change.
        std::optional<SourceInventory> m_Inventory;
        std::string                    m_InventoryError;
        bool                           m_InventoryStale = true;
        bool                           m_InventoryShown = false;

        // Material slots of the selected model, reloaded after selection changes and builds.
        std::optional<std::vector<ModelMaterialSlot>> m_Slots;
        std::string                                   m_SlotsError;
        bool                                          m_SlotsStale = true;
        std::string                                   m_ExtractDirectory;
        std::vector<std::string>                      m_ExtractReport;

        // Built clips of the selected model, decoded on a worker after LoadSlots.
        struct ClipSummary
        {
            std::string                    Name;
            AnimationCompressionStatistics Statistics;
            std::string                    Error;
        };
        std::vector<ClipSummary>                             m_Clips;
        std::optional<std::future<std::vector<ClipSummary>>> m_ClipsLoading;
    };
} // namespace Kans
