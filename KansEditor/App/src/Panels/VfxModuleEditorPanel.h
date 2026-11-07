#pragma once
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>
#include "Panels/VfxModuleEditing.h"
#include "UI/NodeCanvas.h"
#include "KansEditorCore/UnsavedChanges.h"

namespace Kans
{
    class EditorAssetImportService;

    // Edits one .vfxmodule graph (in the manner of Niagara's module script editor): the module
    // inputs and a parameters browser on the left, the node canvas, the selected node's details on
    // the right and the compile output below. Edits are undo steps; the module compiles alone in a
    // test context shortly after each edit; Ctrl+S writes canonical YAML and rebuilds it. Built-in
    // modules open read-only with "Duplicate to project".
    class VfxModuleEditorPanel : public NodeCanvas::Host
    {
    public:
        using Diagnostic = VfxModuleEditing::Diagnostic;
        static constexpr double CompileDelay = 0.3; // Seconds after the last edit.

        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Files of the emitters and systems using a module file; nullopt when it is not tracked.
        using UsageQuery = std::function<std::optional<std::vector<std::string>>(const std::filesystem::path&)>;
        void SetUsageQuery(UsageQuery query) { m_Usage = std::move(query); }
        // Other scripts the module calls (project functions and dynamic inputs).
        void SetModuleResolver(Vfx::ModuleResolver resolve) { m_Resolve = std::move(resolve); }
        // Where "Duplicate to project" writes, and what opens the copy.
        void SetProjectDirectory(std::filesystem::path directory) { m_ProjectDirectory = std::move(directory); }
        void SetOpenCallback(std::function<void(const std::filesystem::path&)> open) { m_Open = std::move(open); }

        bool                         Open(const std::filesystem::path& file);
        bool                         IsOpen() const { return m_IsOpen; }
        const std::filesystem::path& GetFile() const { return m_Document.File(); }
        const std::string&           Status() const { return m_Status; }
        VfxModuleEditing::ModuleDocument& Document() { return m_Document; }
        NodeCanvas::Controller&           Canvas() { return m_Canvas; }
        NodeCanvas::Graph                 BuildGraph() const;

        // Edits on the selection; each is one undo step.
        bool Save();
        bool Undo();
        bool Redo();
        void CopySelection();
        void CutSelection();
        void PasteAt(glm::vec2 position);
        void DuplicateSelection();
        void DeleteSelection();
        void CommentSelection();
        void ToggleCollapsed(const std::string& node);

        // Node search, opened by a right click, Tab or a link dropped on empty space.
        void                                       OpenSearch(const std::optional<NodeCanvas::PinRef>& from, glm::vec2 position);
        bool                                       IsSearching() const { return m_Search.Open; }
        void                                       SetSearchQuery(std::string query) { m_Search.Query = std::move(query); }
        std::vector<VfxModuleEditing::SearchEntry> SearchResults() const;
        // Creates the entry's node (linked to the dragged pin) and closes the search; its GUID.
        std::string ChooseSearch(const VfxModuleEditing::SearchEntry& entry);

        // Validation: call each frame with the time in seconds.
        void                           Tick(double now);
        bool                           IsCompiling() const { return m_Compile.valid(); }
        void                           WaitForCompile();
        const std::vector<Diagnostic>& Diagnostics() const { return m_Diagnostics; }
        // Selects and frames the diagnostic's node and flashes its pin.
        void                           ActivateDiagnostic(size_t index);

        // Module inputs. Removing one in use waits for ConfirmRemoveInput.
        bool               RequestRemoveInput(const std::string& name);
        const std::string& PendingRemoval() const { return m_PendingRemoval; }
        void               ConfirmRemoveInput(bool remove);
        bool               RenameInput(const std::string& from, const std::string& to);

        // An editable copy of a (built-in) module in `directory`, opened through the open callback.
        std::optional<std::filesystem::path> DuplicateToProject(const std::filesystem::path& directory);

        // Asks before dropping unsaved changes; `closed` runs once closed.
        void RequestClose(std::function<void()> closed = {});
        bool IsAskingToClose() const { return m_Closing.Asking(); }
        void ResolveClose(UnsavedChanges::Choice choice);

        // Its own window (nothing once closed), or the contents in the current window.
        void Draw();
        void DrawContents();
        bool HasFocus() const { return m_Focused; }
        std::string WindowName() const;

        // NodeCanvas::Host
        bool ReadOnly() const override { return m_Document.ReadOnly(); }
        bool CanConnect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input, std::string* reason) override;
        void Connect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input) override;
        void Disconnect(const NodeCanvas::PinRef& pin) override;
        void DisconnectLink(size_t link) override;
        void MoveLinks(const NodeCanvas::PinRef& from, const NodeCanvas::PinRef& to) override;
        void InsertReroute(size_t link, glm::vec2 position) override;
        void Translate(const std::vector<std::string>& nodes, const std::vector<std::string>& comments, glm::vec2 delta) override;
        void ResizeComment(const std::string& id, glm::vec2 position, glm::vec2 size) override;
        void EndEdit() override;
        void RequestSearch(const std::optional<NodeCanvas::PinRef>& from, glm::vec2 position) override;
        void ContextMenu(const NodeCanvas::Hit& hit, glm::vec2 position) override;

    private:
        void Edited(); // Commits an undo step.
        void DrawToolbar();
        void DrawInputs();
        void DrawParameters();
        void DrawDetails();
        void DrawOutput();
        void DrawSearch();
        void DrawContextMenu();
        void DrawPrompts();
        void Shortcuts();
        // Value editor for an input pin's default or a constant; true when changed.
        bool ValueEditor(const char* id, Vfx::Value& value, Vfx::Type kind, float width, const std::vector<std::string>* names);

        EditorAssetImportService*                          m_Service = nullptr;
        UsageQuery                                         m_Usage;
        Vfx::ModuleResolver                                m_Resolve;
        std::filesystem::path                              m_ProjectDirectory;
        std::function<void(const std::filesystem::path&)> m_Open;

        VfxModuleEditing::ModuleDocument m_Document;
        NodeCanvas::Controller           m_Canvas;
        bool                             m_IsOpen = false, m_Focused = false, m_WindowOpen = true, m_Framed = false;
        bool                             m_ValueEdit = false; // An inline value is being edited.
        std::string                      m_Status;

        struct SearchState
        {
            bool                              Open = false, Focus = false;
            std::optional<NodeCanvas::PinRef> From;
            glm::vec2                         Position {0};
            std::string                       Query;
            int                               Highlight = 0;
        } m_Search;
        NodeCanvas::Hit m_ContextHit;
        glm::vec2       m_ContextPosition {0};
        bool            m_ContextRequested = false;

        // Compile: debounced after edits, run in the background.
        size_t                                       m_SeenRevision = size_t(-1), m_CompiledRevision = size_t(-1);
        size_t                                       m_PendingRevision = 0;
        double                                       m_EditTime = 0;
        std::future<std::vector<Diagnostic>>         m_Compile;
        Vfx::Module                                  m_CompileModule;
        std::vector<Diagnostic>                      m_Diagnostics;

        std::string    m_PendingRemoval;
        bool           m_RemovalRequested = false;
        std::string    m_RenameFrom;
        char           m_RenameBuffer[128] {};
        UnsavedChanges m_Closing;
        bool           m_ClosePromptRequested = false;
    };

    // The "New VFX Module" dialog: a name, a usage and the allowed stages.
    class VfxModuleCreateDialog
    {
    public:
        void Open(const std::filesystem::path& directory);
        // The created file once the dialog creates it.
        std::optional<std::filesystem::path> Draw();

    private:
        std::filesystem::path m_Directory;
        bool                  m_Requested = false;
        char                  m_Name[96]  = "NewModule";
        int                   m_Usage     = 0;
        uint32_t              m_Stages    = 1u << uint32_t(Vfx::Stage::ParticleUpdate);
        std::string           m_Error;
    };
} // namespace Kans
