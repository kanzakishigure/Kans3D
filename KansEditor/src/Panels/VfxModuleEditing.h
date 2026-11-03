#pragma once
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/VFX/VfxCompiler.h"
#include "UI/NodeCanvas.h"

// ImGui-free editing of a .vfxmodule graph: the document with its undo history, link typing,
// node creation and search, clipboard, comments, the inputs list, automatic layout, the
// standalone "module test" compile and file creation. VfxModuleEditorPanel draws it.
namespace Kans::VfxModuleEditing
{
    using Vfx::Module;
    using Vfx::Type;

    // ---- Document ----
    class ModuleDocument
    {
    public:
        // Loads a module file; the reason when it cannot be read.
        std::optional<std::string> Open(const std::filesystem::path& file);
        void                       Reset(Module module, std::filesystem::path file = {}, bool readOnly = false);
        Module&                    Data() { return m_Module; }
        const Module&              Data() const { return m_Module; }
        const std::filesystem::path& File() const { return m_File; }
        bool                       ReadOnly() const { return m_ReadOnly; }

        // Canonical YAML of the module, as Save writes it.
        std::string Canonical() const;
        // Records an undo step when the module changed since the last one; false when unchanged.
        bool   Commit();
        bool   Undo();
        bool   Redo();
        bool   CanUndo() const { return !m_Undo.empty(); }
        bool   CanRedo() const { return !m_Redo.empty(); }
        // Bumped by every committed change, undo and redo (what the compile follows).
        size_t Revision() const { return m_Revision; }
        // Committed changes differ from the saved file.
        bool   IsDirty() const;
        // Writes canonical YAML; the reason when it fails.
        std::optional<std::string> Save();

    private:
        Module                   m_Module;
        std::filesystem::path    m_File;
        bool                     m_ReadOnly = false;
        std::string              m_Committed, m_Saved;
        std::vector<std::string> m_Undo, m_Redo;
        size_t                   m_Revision = 0;
    };

    // ---- Graph queries and edits (none records an undo step) ----
    std::string       NewGuid();
    Vfx::Node*        FindNode(Module&, const std::string& guid);
    const Vfx::Node*  FindNode(const Module&, const std::string& guid);
    std::optional<Type> PinType(const Module&, const NodeCanvas::PinRef&);
    // Link typing: both pins exist, the compiler converts the output type to the input type, no
    // self link and no cycle. `reason` explains a refusal.
    bool CanLink(const Module&, const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input, std::string* reason);
    // Links output to input; an input keeps one link, so the old one is replaced.
    bool Link(Module&, const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input);
    void Unlink(Module&, const NodeCanvas::PinRef& pin);
    // A new node of `op` with node type `kind` at `position`; returns its GUID.
    std::string AddNode(Module&, const std::string& op, Type kind, glm::vec2 position, const std::string& parameter = {});
    void        DeleteNodes(Module&, const std::set<std::string>& nodes);
    // A reroute on link `index` at `position`; returns its GUID.
    std::string InsertReroute(Module&, size_t index, glm::vec2 position);
    // Retypes a node: pins that follow the node type change with it (Make gets one pin per component).
    void        SetNodeKind(Module&, const std::string& guid, Type kind);
    void        Translate(Module&, const std::vector<std::string>& nodes, const std::vector<std::string>& comments,
                          glm::vec2 delta);

    // ---- Clipboard ----
    // The nodes, the links between them and their layout, as text.
    std::string              Copy(const Module&, const std::set<std::string>& nodes);
    // Pastes with fresh GUIDs, keeping internal links, offset by `offset`; returns the new GUIDs.
    std::vector<std::string> Paste(Module&, const std::string& clip, glm::vec2 offset);
    inline constexpr float   DuplicateOffset = 32;

    // ---- Comments ----
    std::string AddComment(Module&, const std::string& title, glm::vec2 position, glm::vec2 size);

    // ---- Search ----
    struct SearchEntry
    {
        enum class Kind
        {
            Operation,
            ModuleInput, // An Input node of a Module.* input.
            Get,
            Set
        };
        Kind        What = Kind::Operation;
        std::string Name; // Op or parameter.
        std::string Label, Category, Description;
        Type        NodeKind = Type::Float;
        std::string LinkPin; // The new node's pin linked to the dragged pin.
        int         Score = 0;
    };
    // The dragged pin a search links to.
    struct SearchFilter
    {
        NodeCanvas::Side PinSide = NodeCanvas::Side::Output;
        Type             PinKind = Type::Float;
    };
    // Ops grouped by category, then the module's inputs and parameters; with a filter only entries
    // with a pin that links to the dragged pin. Fuzzy: every query letter in order.
    std::vector<SearchEntry> Search(const Module&, const std::string& query, const std::optional<SearchFilter>& filter = {});
    int                      FuzzyScore(const std::string& text, const std::string& query); // -1: no match.
    // Creates the entry's node at `position`, linked to `from` when given; returns its GUID.
    std::string Create(Module&, const SearchEntry&, glm::vec2 position, const std::optional<NodeCanvas::PinRef>& from = {});

    // ---- Parameters browser ----
    struct ParameterEntry
    {
        std::string Group; // Particles, Emitter, System, Engine, Transient, Module, or a data interface.
        std::string Name;
        Type        Kind = Type::Float;
        bool        Writable = false, ModuleInput = false;
        std::string Interface; // Data-interface kind of a module input bound by name (Grid, VectorField...).
    };
    std::vector<ParameterEntry> Parameters(const Module&);
    // A Get (or Set), or an Input node for a module input; a data interface makes its default op.
    std::string CreateParameterNode(Module&, const ParameterEntry&, glm::vec2 position, bool set);

    // ---- Module inputs ----
    std::string AddInput(Module&, Type kind);
    // Renames an input and every node bound to it; the reason when the name is invalid or taken.
    std::optional<std::string> RenameInput(Module&, const std::string& from, const std::string& to);
    // Nodes bound to the input.
    std::vector<std::string> InputUsers(const Module&, const std::string& name);
    // Removes the input with its Input nodes and their links.
    void RemoveInput(Module&, const std::string& name);
    void MoveInput(Module&, size_t index, int direction);
    void SetInputType(Module&, const std::string& name, Type kind);

    // ---- Canvas view of the module ----
    struct Diagnostic
    {
        std::string Node, Pin, Message;
        bool        Warning = false;
    };
    uint32_t           TypeColor(Type);
    uint32_t           CategoryColor(const std::string& category);
    std::string        NodeTitle(const Vfx::Node&);
    NodeCanvas::Graph  BuildGraph(const Module&, const std::vector<Diagnostic>& diagnostics = {});
    // Width of an inline value editor for an unconnected input of this type (0: none).
    float              ValueWidth(Type);
    // Positions every node without one: layers from inputs to outputs, columns left to right.
    void               AutoLayout(Module&);
    // Warnings the editor adds: nodes whose result reaches nothing.
    std::vector<Diagnostic> Warnings(const Module&);

    // ---- Module test compile ----
    // Compiles the module alone: a minimal system whose one emitter uses it in its first allowed
    // stage, with data interfaces bound for its named inputs. Errors of other assets are kept with
    // an empty node. `resolve` finds other scripts (functions, dynamic inputs).
    std::vector<Diagnostic> TestCompile(const Module&, const Vfx::ModuleResolver& resolve = {});

    // ---- Files ----
    bool IsBuiltin(const std::filesystem::path& file);
    // A new module "<name>.vfxmodule" in `directory` with one Input and one Set node (a result
    // reroute for functions and dynamic inputs).
    std::variant<std::filesystem::path, std::string>
    CreateModuleFile(const std::filesystem::path& directory, const std::string& name, Vfx::Usage usage, uint32_t stages);
    // An editable copy with a new asset ID in `directory`.
    std::variant<std::filesystem::path, std::string> Duplicate(const Module&, const std::filesystem::path& source,
                                                               const std::filesystem::path& directory);
} // namespace Kans::VfxModuleEditing
