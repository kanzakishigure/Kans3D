#pragma once
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Panels/VfxSystemEditing.h"
#include "UI/NodeCanvas.h"
#include "UnsavedChanges.h"

namespace Kans
{
    class EditorAssetImportService;

    // Edits one .vfxsystem (or one .vfxemitter as a system with that emitter), in the manner of
    // Niagara's system editor: the system overview (an emitter node graph) on top, the selected
    // emitter's stack (Emitter Spawn/Update, Particle Spawn/Update, Event Handlers, Simulation
    // Stages, Render) on the left, the parameters panel on the right and the compile output below.
    // Each edit is an undo step; the system compiles in the background shortly after edits and its
    // errors badge the overview node and the stack row they point to. Ctrl+S writes canonical YAML
    // and rebuilds it. Built-in templates open read-only with "Duplicate to project".
    class VfxSystemEditorPanel : public NodeCanvas::Host
    {
    public:
        using Result     = VfxSystemEditing::Result;
        using StackRef   = VfxSystemEditing::StackRef;
        using InputPath  = VfxSystemEditing::InputPath;
        using AssetList  = std::function<std::vector<std::pair<AssetID, std::filesystem::path>>()>;
        static constexpr double CompileDelay = 0.3; // Seconds after the last edit.

        void Bind(EditorAssetImportService* service) { m_Service = service; }
        // Project modules and emitters (product ID and source file), for the library and pickers.
        void SetProjectModules(AssetList list) { m_ProjectModules = std::move(list); }
        void SetProjectEmitters(AssetList list) { m_ProjectEmitters = std::move(list); }
        void SetEmitterResolver(Vfx::EmitterResolver resolve) { m_Context.Emitters = std::move(resolve); }
        void SetModuleFallback(Vfx::ModuleResolver resolve) { m_Library.SetFallback(std::move(resolve)); }
        // Where "Duplicate to project" writes; what opens a copy and what opens a module file.
        void SetProjectDirectory(std::filesystem::path directory) { m_ProjectDirectory = std::move(directory); }
        void SetOpenCallback(std::function<void(const std::filesystem::path&)> open) { m_Open = std::move(open); }
        void SetOpenModuleCallback(std::function<void(const std::filesystem::path&)> open) { m_OpenModule = std::move(open); }

        bool                                Open(const std::filesystem::path& file);
        bool                                IsOpen() const { return m_IsOpen; }
        const std::filesystem::path&        GetFile() const { return m_Document.File(); }
        const std::string&                  Status() const { return m_Status; }
        VfxSystemEditing::SystemDocument&   Document() { return m_Document; }
        const VfxSystemEditing::Context&    GetContext() const { return m_Context; }
        VfxSystemEditing::ModuleLibrary&    Library() { return m_Library; }
        NodeCanvas::Controller&             Overview() { return m_Overview; }
        NodeCanvas::Graph                   BuildOverview() const;

        // The emitter whose stack shows (a handle GUID, or SystemNode for the system's own settings).
        const std::string& Selected() const { return m_Selected; }
        void               Select(const std::string& emitter);

        // Applies an edit as one undo step (or, `continuous`, as part of the step that ends when no
        // widget is active any more); its refusal becomes the status line.
        Result Apply(const std::function<Result(Vfx::System&, const VfxSystemEditing::Context&)>& edit, bool continuous = false);
        // Ends a continuous edit now (one undo step).
        void   FinishEdit();
        bool   Save();
        bool   Undo();
        bool   Redo();
        std::optional<std::filesystem::path> DuplicateToProject(const std::filesystem::path& directory);

        // "+ Add module" search for a stack, inserting at `index`.
        void                                        OpenModuleSearch(const StackRef& stack, size_t index = SIZE_MAX);
        // The dynamic input picker for an input of a use.
        void                                        OpenDynamicInputSearch(const std::string& use, const InputPath& path);
        bool                                        IsSearching() const { return m_Search.Open; }
        void                                        SetSearchQuery(std::string query) { m_Search.Query = std::move(query); }
        std::vector<VfxSystemEditing::ModuleEntry>  SearchResults() const;
        Result                                      ChooseSearch(const VfxSystemEditing::ModuleEntry& entry);

        // Curve and gradient inputs open their editor in a popup.
        void OpenCurveEditor(const std::string& use, const InputPath& path);
        void OpenGradientEditor(const std::string& use, const InputPath& path);
        bool IsEditingCurve() const { return m_CurveEdit.Open && !m_CurveEdit.IsGradient; }
        bool IsEditingGradient() const { return m_CurveEdit.Open && m_CurveEdit.IsGradient; }
        // Writes the edited gradient back to its input (continuous while a stop is dragged).
        Result SetEditedGradient(const VfxSystemEditing::Gradient& gradient, bool continuous);
        Result SetEditedCurve(const TimelineCurve& curve, bool continuous);

        // Rows shown expanded (a module use GUID), and the row a diagnostic highlights.
        void Expand(const std::string& use) { m_ExpandPending.insert(use); }

        // Validation: call each frame with the time in seconds.
        void Tick(double now);
        bool IsCompiling() const { return m_Compile.valid(); }
        void WaitForCompile();
        const std::vector<VfxSystemEditing::StackDiagnostic>& Diagnostics() const { return m_Diagnostics; }
        // Selects the diagnostic's emitter, expands and scrolls to its row and highlights it.
        void ActivateDiagnostic(size_t index);
        const std::string& HighlightedUse() const { return m_Highlight.Use; }
        const std::string& HighlightedInput() const { return m_Highlight.Input; }

        void RequestClose(std::function<void()> closed = {});
        bool IsAskingToClose() const { return m_Closing.Asking(); }
        void ResolveClose(UnsavedChanges::Choice choice);

        void        Draw();
        void        DrawContents();
        bool        HasFocus() const { return m_Focused; }
        std::string WindowName() const;

        // NodeCanvas::Host: emitter links create event handlers; the rest only moves nodes.
        bool ReadOnly() const override { return m_Document.ReadOnly(); }
        bool CanConnect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input, std::string* reason) override;
        void Connect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input) override;
        void Disconnect(const NodeCanvas::PinRef&) override;
        void DisconnectLink(size_t) override;
        void MoveLinks(const NodeCanvas::PinRef&, const NodeCanvas::PinRef&) override {}
        void InsertReroute(size_t, glm::vec2) override {}
        void Translate(const std::vector<std::string>& nodes, const std::vector<std::string>& comments, glm::vec2 delta) override;
        void ResizeComment(const std::string&, glm::vec2, glm::vec2) override {}
        void RequestSearch(const std::optional<NodeCanvas::PinRef>& from, glm::vec2 position) override;
        void ContextMenu(const NodeCanvas::Hit& hit, glm::vec2 position) override;

    private:
        void Edited();
        void Refresh(); // Reloads the module library.
        void DrawToolbar();
        void DrawOverview(float height);
        void DrawStack();
        void DrawSystemProperties();
        void DrawEmitterProperties(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective);
        void DrawStackSection(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective, const StackRef& ref,
                              const char* title);
        void DrawModuleRow(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective, const StackRef& ref,
                           const Vfx::ModuleUse& use, size_t index);
        void DrawInputs(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective, const StackRef& ref,
                        const Vfx::ModuleUse& use, const Vfx::Module& module, const InputPath& prefix, int depth);
        void DrawHandlers(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective);
        void DrawSimulationStages(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective);
        void DrawRenderers(const Vfx::EmitterHandle& handle, const Vfx::Emitter& effective);
        void DrawParameters();
        void DrawOutput();
        void DrawSearch();
        void DrawCurvePopup();
        void DrawMenus();
        void DrawPrompts();
        void Shortcuts();
        bool ValueEditor(const char* id, Vfx::Value& value, Vfx::Type kind, const Vfx::Parameter* spec);
        std::string Handle() const; // The emitter of the stack shown ("" for the system).

        EditorAssetImportService*                          m_Service = nullptr;
        AssetList                                          m_ProjectModules, m_ProjectEmitters;
        std::filesystem::path                              m_ProjectDirectory;
        std::function<void(const std::filesystem::path&)> m_Open, m_OpenModule;

        VfxSystemEditing::SystemDocument m_Document;
        VfxSystemEditing::ModuleLibrary  m_Library;
        VfxSystemEditing::Context        m_Context;
        NodeCanvas::Controller           m_Overview;
        std::map<std::string, glm::vec2> m_Positions;
        std::string                      m_Selected;
        bool        m_IsOpen = false, m_Focused = false, m_WindowOpen = true, m_Framed = false, m_ValueEdit = false;
        std::string m_Status;

        struct SearchState
        {
            bool        Open = false, Focus = false, Dynamic = false;
            StackRef    Stack;
            size_t      Index = SIZE_MAX;
            std::string Use;
            InputPath   Path;
            Vfx::Type   Kind = Vfx::Type::Float;
            std::string Query;
            int         Highlight = 0;
        } m_Search;
        struct CurveState
        {
            bool                       Open = false, Requested = false, IsGradient = false;
            std::string                Use;
            InputPath                  Path;
            VfxSystemEditing::Gradient Gradient;
            TimelineCurve              Curve;
        } m_CurveEdit;
        struct Highlight
        {
            std::string Use, Input;
            double      Until  = 0;
            bool        Scroll = false;
        } m_Highlight;
        std::set<std::string> m_ExpandPending;
        // Context menus requested while drawing.
        std::string m_MenuEmitter, m_MenuUse;
        bool        m_EmitterMenuRequested = false, m_UseMenuRequested = false, m_AddEmitterRequested = false;
        std::string m_RenameEmitter, m_RenameParameter;
        char        m_RenameBuffer[128] {};
        // Drag-and-drop of module rows.
        std::string m_DragUse;

        // Compile: debounced after edits, run in the background.
        size_t                                       m_SeenRevision = size_t(-1), m_CompiledRevision = size_t(-1);
        size_t                                       m_PendingRevision = 0;
        double                                       m_EditTime = 0, m_Now = 0;
        std::future<std::vector<Vfx::Diagnostic>>    m_Compile;
        std::vector<VfxSystemEditing::StackDiagnostic> m_Diagnostics;

        UnsavedChanges m_Closing;
        bool           m_ClosePromptRequested = false;
    };

    // The "New VFX System" and "New VFX Emitter" dialogs.
    class VfxSystemCreateDialog
    {
    public:
        using AssetList = VfxSystemEditorPanel::AssetList;
        void OpenSystem(const std::filesystem::path& directory);
        void OpenEmitter(const std::filesystem::path& directory, AssetList parents = {});
        // The created file once the dialog creates it.
        std::optional<std::filesystem::path> Draw();

    private:
        std::filesystem::path                                  m_Directory;
        bool                                                   m_Requested = false, m_Emitter = false;
        char                                                   m_Name[96]  = "NewSystem";
        int                                                    m_Choice    = 0; // 0: empty; then templates or parents.
        std::vector<VfxSystemEditing::Template>                m_Templates;
        std::vector<std::pair<AssetID, std::filesystem::path>> m_Parents;
        std::string                                            m_Error;
    };
} // namespace Kans
