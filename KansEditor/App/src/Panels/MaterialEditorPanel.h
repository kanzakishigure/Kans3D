#pragma once
#include <functional>
#include "Kans3D/Asset/Importer/MaterialSource.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "KansEditorCore/EditorAssetChoice.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "Kans3D/Renderer/MaterialPreviewRenderer.h"
#include "Kans3D/Renderer/Resource/Texture.h"
#include "KansEditorCore/Components/MaterialAssignment.h"

namespace Kans
{
    // Edits `.material` sources: the Shader (or engine PBR), its parameters and
    // textures. Saving writes the source and builds it so the scene updates.
    class MaterialEditorPanel
    {
    public:
        // Non-owning; the editor disconnects before destroying them.
        void Bind(EditorAssetImportService* service) { m_Service = service; }
        void SetAssetManager(AssetManager* assets) { m_Assets = assets; }
        void SetAssetChoices(std::vector<EditorAssetChoice> choices) { m_Choices = std::move(choices); }
        // Renders the live preview; owned by the editor, may be null.
        void SetPreviewRenderer(MaterialPreviewRenderer* preview)
        {
            m_Preview = preview;
            m_PreviewKey.clear();
            m_PreviewImage.reset();
            m_PreviewTexture = 0;
        }

        // "Used by": the scene's mesh slots drawn with a material, and selecting one of them.
        using UsageQuery = std::function<std::vector<MaterialUse>(AssetID)>;
        void SetUsageQuery(UsageQuery query) { m_Usage = std::move(query); }
        void SetSelectCallback(std::function<void(UUID)> select) { m_Select = std::move(select); }

        // Loads a material for editing; the editor opens the window.
        void Open(const std::filesystem::path& file);
        // Creates a new engine PBR material in `directory` and loads it.
        void Create(const std::filesystem::path& directory);
        // Contents of the window the editor's panel manager opened.
        void Draw();
        bool IsDirty() const { return m_Dirty; }
        // True while the editor window has focus: Ctrl+Z / Ctrl+Y undo material edits
        // instead of scene edits.
        bool HasFocus() const { return m_Focused; }

    private:
        friend struct MaterialEditorPanelTestAccess;
        friend struct MaterialEditorPanelThemeAccess;
        // Schema of the selected Shader; nullptr while it loads or if it is invalid.
        const MaterialSchema* Schema();
        void                  DrawParameter(const MaterialParameterInfo& parameter, const MaterialSchema& schema);
        // Parameters and textures, in the groups the Shader declares.
        void DrawParameters(const MaterialSchema& schema);
        // The edited (unsaved) material on a shape; drag to rotate.
        void DrawPreview();
        void DrawTexture(const std::string& name);
        // Alpha mode, cutoff and double-sided: how the renderer draws the material.
        void DrawRenderState();
        void DrawPasses(bool instance, const MaterialSource* parent);
        // The open scene's entities and slots using the published material; click to select.
        void DrawUsedBy();
        // The published Material of the open file, or invalid.
        AssetID PublishedMaterial() const;
        // Instances: a parent material whose values are inherited unless overridden.
        bool IsInstance() const { return m_Source.Parent.IsValid(); }
        void DrawParent();
        // The parent's effective values (what an instance inherits); null while loading or on error.
        const MaterialSource* ParentEffective();
        // Marker before an instance's value: filled when overridden (click to reset to the
        // parent's value), hollow when inherited. True when a reset was requested.
        bool OverrideMarker(bool overridden);
        // Thumbnail button for a texture (or an empty slot); shows a larger preview on hover.
        void DrawThumbnail(AssetID texture, float size);
        // Assigns a file dropped from the content browser, importing it first if needed.
        void AssignDroppedFile(const std::string& slot, const std::filesystem::path& file);
        void FinishPendingTexture();
        void SetTexture(const std::string& slot, AssetID texture);
        void Save();
        // Undo history of the edited source, as serialized snapshots. An edit is
        // recorded once the interaction that made it ends (one drag, one step).
        void        TrackEdits();
        void        Undo();
        void        Redo();
        void        ResetHistory();
        std::string ChoiceName(AssetID id) const;

        EditorAssetImportService*      m_Service = nullptr;
        UsageQuery                     m_Usage;
        std::function<void(UUID)>      m_Select;
        AssetManager*                  m_Assets = nullptr;
        std::vector<EditorAssetChoice> m_Choices;

        bool                     m_Dirty   = false;
        bool                     m_Focused = false;
        std::string              m_SavedText, m_CommittedText;
        std::vector<std::string> m_UndoStack, m_RedoStack;
        static constexpr size_t  MaxHistory = 100;
        std::filesystem::path    m_File;
        MaterialSource           m_Source;
        std::string              m_Status;
        bool                     m_Building = false;

        // GPU thumbnails of texture Products, rebuilt when the asset changes.
        struct Thumbnail
        {
            Ref<const TextureAsset> Source;
            Ref<Texture2D>          Texture;
        };
        std::unordered_map<AssetID, Thumbnail> m_Thumbnails;
        // Texture slot waiting for a dropped source to finish importing.
        std::optional<std::pair<std::string, SourceAssetID>> m_PendingTexture;
        std::string                                          m_TextureFilter;

        // Live preview: re-rendered when the source, view or shader changes, or while incomplete.
        MaterialPreviewRenderer*                        m_Preview      = nullptr;
        MaterialPreviewShape                            m_PreviewShape = MaterialPreviewShape::Sphere;
        glm::vec2                                       m_PreviewRotation {0.6f, 0.3f};
        std::string                                     m_PreviewKey;
        RenderHardware::Handle<RenderHardware::Texture> m_PreviewImage;
        uintptr_t                                       m_PreviewTexture  = 0;
        bool                                            m_PreviewComplete = false;
        std::string                                     m_PreviewError;

        // Parent cache of an instance, keyed by the parent's material and Shader assets.
        AssetID                       m_ParentFor;
        Ref<const MaterialDataAsset>  m_ParentAsset;
        Ref<const ShaderDataAsset>    m_ParentShaderAsset;
        std::optional<MaterialSource> m_ParentEffective;
        std::string                   m_ParentError;

        // Schema cache, keyed by the Shader asset it was built from.
        Ref<const ShaderDataAsset>    m_SchemaShader;
        std::optional<MaterialSchema> m_Schema;
        std::string                   m_SchemaError;
    };
} // namespace Kans
