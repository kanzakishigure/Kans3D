#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>
#include "Kans3D/Timeline/Timeline.h"
#include "Kans3D/VFX/VfxCompiler.h"
#include "UI/NodeCanvas.h"

// ImGui-free editing of a .vfxsystem (or of one .vfxemitter shown as a system with that emitter):
// the document with its undo history, emitter handles, the emitter stacks with inheritance, module
// inputs (values, parameter links, dynamic inputs, curves and gradients), simulation stages, event
// handlers, renderers, parameters, the system overview graph and compile diagnostics mapped onto
// stack rows. VfxSystemEditorPanel draws it.
namespace Kans::VfxSystemEditing
{
    using Vfx::Emitter;
    using Vfx::EmitterHandle;
    using Vfx::Stage;
    using Vfx::System;
    using Vfx::Type;

    // Success, or a message saying why an edit was refused. `Value` carries a new GUID or name.
    struct Result
    {
        bool        Ok = true;
        std::string Value, Message;
        static Result Done(std::string value = {}) { return {true, std::move(value), {}}; }
        static Result Refused(std::string message) { return {false, {}, std::move(message)}; }
        explicit      operator bool() const { return Ok; }
    };

    // ---- Module library: built-ins and project scripts by ID, with their names ----
    struct ModuleInfo
    {
        AssetID               Id;
        std::string           Name; // The file name without extension.
        std::filesystem::path File;
        bool                  Builtin = false;
        Vfx::Module           Data;
    };
    class ModuleLibrary
    {
    public:
        // Reads the built-in root and the project's .vfxmodule products (their asset ID and source
        // file); unreadable files are skipped.
        void Load(const std::vector<std::pair<AssetID, std::filesystem::path>>& project = {});
        void Add(ModuleInfo info);
        // Asked for scripts the library does not hold (may throw).
        void SetFallback(Vfx::ModuleResolver fallback) { m_Fallback = std::move(fallback); }
        const ModuleInfo* Find(AssetID id) const;
        std::string       NameOf(AssetID id) const; // "Missing <id>" when unknown.
        std::vector<const ModuleInfo*> All() const;
        // A resolver that stays valid (a snapshot) while the library changes; safe on other threads.
        Vfx::ModuleResolver Resolver() const;

    private:
        std::shared_ptr<std::map<AssetID, ModuleInfo>> m_Modules = std::make_shared<std::map<AssetID, ModuleInfo>>();
        Vfx::ModuleResolver                            m_Fallback;
    };

    // What edits and queries need besides the document.
    struct Context
    {
        const ModuleLibrary*  Modules = nullptr;
        Vfx::EmitterResolver  Emitters; // Parents and referenced emitters (may throw).
        Vfx::ModuleResolver   ResolveModule() const;
        Vfx::EmitterResolver  ResolveEmitter() const;
    };

    // ---- Document ----
    enum class Mode
    {
        System,
        Emitter // One .vfxemitter, edited as a system holding that emitter inline.
    };
    inline constexpr const char* EmitterModeHandle = "emitter";
    class SystemDocument
    {
    public:
        // Loads a .vfxsystem or .vfxemitter; the reason when it cannot be read.
        std::optional<std::string> Open(const std::filesystem::path& file);
        void          Reset(System system, std::filesystem::path file = {}, Mode mode = Mode::System, bool readOnly = false);
        System&       Data() { return m_System; }
        const System& Data() const { return m_System; }
        Mode          GetMode() const { return m_Mode; }
        const std::filesystem::path& File() const { return m_File; }
        bool                         ReadOnly() const { return m_ReadOnly; }

        // Canonical YAML as Save writes it (the emitter alone in emitter mode).
        std::string Canonical() const;
        bool        Commit(); // Records an undo step when something changed; false when unchanged.
        bool        Undo();
        bool        Redo();
        bool        CanUndo() const { return !m_Undo.empty(); }
        bool        CanRedo() const { return !m_Redo.empty(); }
        // When the step Undo (Redo) would apply was committed (NextEditStamp); nothing without one.
        std::optional<uint64_t> UndoStamp() const;
        std::optional<uint64_t> RedoStamp() const;
        size_t      Revision() const { return m_Revision; }
        bool        IsDirty() const;
        std::optional<std::string> Save();

    private:
        void Load(const std::string& canonical);

        System                   m_System;
        Mode                     m_Mode = Mode::System;
        std::filesystem::path    m_File;
        bool                     m_ReadOnly = false;
        std::string              m_Committed, m_Saved;
        std::vector<std::string> m_Undo, m_Redo;
        std::vector<uint64_t>    m_UndoStamps, m_RedoStamps; // Parallel to m_Undo and m_Redo.
        size_t                   m_Revision = 0;
    };
    // An emitter as a one-emitter system (emitter mode), and back.
    System  WrapEmitter(Emitter emitter);
    Emitter UnwrapEmitter(const System& system);

    // ---- Emitter handles and inheritance layers ----
    enum class Layer
    {
        Own,         // Inline without a parent: edits change the emitter itself.
        InlineChild, // Inline with a parent: edits are overrides of the inline emitter.
        Reference    // A referenced emitter asset: edits are overrides on the handle.
    };
    EmitterHandle*       FindHandle(System&, const std::string& guid);
    const EmitterHandle* FindHandle(const System&, const std::string& guid);
    Layer                LayerOf(const EmitterHandle&);
    // The emitter the compiler builds (parents merged, every override applied); throws when a
    // parent or reference is missing.
    Emitter Effective(const EmitterHandle&, const Context&);
    // What the editable layer starts from: the merged parent (layered) or the emitter itself (own).
    Emitter Base(const EmitterHandle&, const Context&);

    // A stack: an emitter stage, or the modules of an event handler or simulation stage (by GUID).
    struct StackRef
    {
        Stage       Kind = Stage::ParticleUpdate;
        std::string Container;
        bool        operator==(const StackRef& b) const { return Kind == b.Kind && Container == b.Container; }
        bool        operator!=(const StackRef& b) const { return !(*this == b); }
    };
    const Vfx::Stack* FindStack(const Emitter&, const StackRef&);
    // Where a module use sits in the effective emitter.
    std::optional<StackRef> StackOf(const Emitter&, const std::string& use);
    const Vfx::ModuleUse*   FindUse(const Emitter&, const std::string& use);

    enum class Origin
    {
        Own,        // An emitter without a parent.
        Inherited,  // From the parent, unchanged.
        Overridden, // From the parent, with overridden inputs, enabled state or order.
        Added       // Added by this layer.
    };
    Origin ModuleOrigin(const EmitterHandle&, const std::string& use);
    bool   InputOverridden(const EmitterHandle&, const std::string& use, const std::string& input);

    // ---- Module inputs ----
    // A module input with how the stack edits it.
    struct InputInfo
    {
        Vfx::Parameter Spec;
        // A data-interface binding the module reads by name: Curve, Gradient, Texture2D (Sample),
        // Grid3D, VectorField, Mesh, Events, EmitterParticles or ForceFields; empty for values.
        std::string           Interface;
        Vfx::InterfaceBinding DefaultBinding; // The module's own table for Sample inputs.
        std::string           SpaceInput;     // An int input choosing this position's space.
    };
    std::vector<InputInfo> ModuleInputs(const Vfx::Module&);
    // The result type of a dynamic input or function; nothing without a result node.
    std::optional<Type> ResultType(const Vfx::Module&);
    // Inputs a new use starts with: literals for value inputs, the module's table for curves.
    std::map<std::string, Vfx::Input> DefaultInputs(const Vfx::Module&);

    // An input, or one nested in dynamic inputs: {"Module.Velocity", "Module.A"}.
    using InputPath = std::vector<std::string>;
    // The input as the use currently has it (nothing: the module default applies).
    std::optional<Vfx::Input> GetInput(const Vfx::ModuleUse&, const InputPath&);
    // The type an input path has (the nested script's input type for deeper paths).
    std::optional<Type> InputType(const Context&, const Vfx::ModuleUse&, const InputPath&);

    // ---- Stack edits (none records an undo step) ----
    // A new use of `script` with a fresh GUID and default inputs at `index` (end when past it).
    Result AddModule(System&, const Context&, const std::string& handle, const StackRef&, AssetID script,
                     size_t index = SIZE_MAX);
    Result RemoveModule(System&, const Context&, const std::string& handle, const std::string& use);
    // Moves a use to `index` of `to` (its own stack or another one its AllowedStages permits).
    Result MoveModule(System&, const Context&, const std::string& handle, const std::string& use, const StackRef& to,
                      size_t index);
    Result DuplicateModule(System&, const Context&, const std::string& handle, const std::string& use);
    Result SetModuleEnabled(System&, const Context&, const std::string& handle, const std::string& use, bool enabled);
    // Drops this layer's overrides of an inherited use (inputs, enabled state, order).
    Result ResetModule(System&, const Context&, const std::string& handle, const std::string& use);
    Result SetInput(System&, const Context&, const std::string& handle, const std::string& use, const InputPath&,
                    Vfx::Input value);
    // Back to the parent's value (layered) or the module default (own and added uses).
    Result ResetInput(System&, const Context&, const std::string& handle, const std::string& use, const InputPath&);
    // Links an input to a parameter of the same type.
    Result LinkInput(System&, const Context&, const std::string& handle, const std::string& use, const InputPath&,
                     const std::string& parameter);
    // Replaces an input with a dynamic input script whose result converts to the input's type.
    Result SetDynamicInput(System&, const Context&, const std::string& handle, const std::string& use,
                           const InputPath&, AssetID script);

    // ---- Module search ----
    struct ModuleEntry
    {
        AssetID     Id;
        std::string Name, Category, Description;
        bool        Builtin = false;
        int         Score   = 0;
    };
    // Modules allowed in `stage` (built-ins and project ones), grouped by category then name;
    // fuzzy matched on the name, category and description.
    std::vector<ModuleEntry> SearchModules(const ModuleLibrary&, Stage stage, const std::string& query);
    // Dynamic inputs whose result converts to `kind`.
    std::vector<ModuleEntry> SearchDynamicInputs(const ModuleLibrary&, Stage stage, Type kind, const std::string& query);

    // ---- Curves and gradients ----
    // A curve input's table as linear keys, and back.
    TimelineCurve         CurveFromBinding(const Vfx::InterfaceBinding&);
    Vfx::InterfaceBinding BindingFromCurve(const TimelineCurve&);
    struct Gradient
    {
        enum class Blend
        {
            Linear, // Colours and alphas blend between stops.
            Fixed   // Each stop holds until the next one.
        };
        struct ColorStop
        {
            float     Time = 0;
            glm::vec3 Color {1};
            bool      operator==(const ColorStop& b) const { return Time == b.Time && Color == b.Color; }
        };
        struct AlphaStop
        {
            float Time = 0, Alpha = 1;
            bool  operator==(const AlphaStop& b) const { return Time == b.Time && Alpha == b.Alpha; }
        };
        std::vector<ColorStop> Colors {{0, glm::vec3(1)}, {1, glm::vec3(1)}};
        std::vector<AlphaStop> Alphas {{0, 1}, {1, 1}};
        Blend                  Mode = Blend::Linear;

        glm::vec4 Evaluate(float time) const;
        // The baked table the compiler samples (exact for both modes).
        Vfx::InterfaceBinding Bake() const;
        // Stops recovered from a table: one per distinct time, redundant linear ones dropped.
        static Gradient FromBinding(const Vfx::InterfaceBinding&);
        void            Sort();
        bool operator==(const Gradient& b) const { return Colors == b.Colors && Alphas == b.Alphas && Mode == b.Mode; }
    };

    // ---- Simulation stages ----
    Result AddSimulationStage(System&, const Context&, const std::string& handle, const std::string& name,
                              Vfx::IterationSource source, const std::string& dataInterface, uint32_t iterations);
    // Changes a stage's settings (name, source, iterations, Enabled); its modules are kept. The
    // engine's own check refuses invalid settings.
    Result SetSimulationStage(System&, const Context&, const std::string& handle, const Vfx::SimulationStage& settings);
    Result RemoveSimulationStage(System&, const Context&, const std::string& handle, const std::string& guid);
    Result MoveSimulationStage(System&, const Context&, const std::string& handle, const std::string& guid, int direction);

    // ---- Event handlers and channels ----
    std::vector<std::string> EventChannels(const System&, const Context&, const std::string& handle);
    // Layouts the emitter's modules declare (what a channel can carry).
    std::vector<std::string> EventLayouts(const System&, const Context&, const std::string& handle);
    Result AddEventChannel(System&, const Context&, const std::string& handle, const std::string& layout);
    Result AddEventHandler(System&, const Context&, const std::string& handle, const std::string& source,
                           const std::string& channel, Vfx::EventMode mode = Vfx::EventMode::SpawnedParticles,
                           uint32_t spawnNumber = 1);
    Result SetEventHandler(System&, const Context&, const std::string& handle, const Vfx::EventHandler& settings);
    Result RemoveEventHandler(System&, const Context&, const std::string& handle, const std::string& guid);
    // An overview link from `source` to `target`: a handler on `target` for the source's first event
    // channel (one is added for a layout the source's modules write when it has none).
    Result ConnectEmitters(System&, const Context&, const std::string& source, const std::string& target);

    // ---- Renderers ----
    Result AddRenderer(System&, const Context&, const std::string& handle, Vfx::RendererKind kind);
    Result SetRenderer(System&, const Context&, const std::string& handle, const Vfx::Renderer& renderer);
    Result RemoveRenderer(System&, const Context&, const std::string& handle, const std::string& guid);
    Result MoveRenderer(System&, const Context&, const std::string& handle, const std::string& guid, int direction);

    // ---- Asset references (the stack's asset pickers) ----
    // The asset type a binding kind references: VectorField; StaticMesh for Mesh, StaticMesh and
    // SkeletalMesh; Texture for Texture2D; VfxModule (a curve module with a sample table) for Curve
    // and Gradient; None for bindings without an asset.
    AssetType   BindingAssetType(const std::string& kind);
    std::string AssetTypeLabel(AssetType);
    // Why an asset of type `got` cannot go where `expected` is needed ("Expected a Vector Field asset,
    // not a Texture"), or nothing.
    std::optional<std::string> AssetTypeMismatch(AssetType expected, AssetType got);
    // Points the asset-backed data-interface input at `path` at `asset` (of type `type`), keeping the
    // binding's other settings. Refused for inputs without an asset and for the wrong asset type;
    // layered emitters get a SetInput override, as for any input edit.
    Result PickInputAsset(System&, const Context&, const std::string& handle, const std::string& use, const InputPath&,
                          AssetID asset, AssetType type);

    // ---- Emitter properties ----
    struct EmitterSettings
    {
        Vfx::SimTarget             Target          = Vfx::SimTarget::CPU;
        Vfx::Space                 SimulationSpace = Vfx::Space::Local;
        uint32_t                   Capacity        = 1024;
        bool                       Deterministic   = true;
        std::optional<Bounds>      FixedBounds;
        float                      CullDistance   = 0;
        Vfx::CullPolicy            DistancePolicy = Vfx::CullPolicy::PauseSpawning;
    };
    EmitterSettings SettingsOf(const Emitter&);
    // Own emitters change; children record overridden settings; references are refused.
    Result SetEmitterSettings(System&, const Context&, const std::string& handle, const EmitterSettings&);
    // Child emitters: the settings this layer overrides, and a reset to the parent's value.
    std::set<std::string> OverriddenSettings(const EmitterHandle&);
    Result                ResetEmitterSetting(System&, const Context&, const std::string& handle, const std::string& setting);

    // ---- Emitters in the system ----
    // The minimal working emitter: emitter state, initialize, particle state, solver and a sprite.
    Emitter EmptyEmitter();
    std::string AddEmitter(System&, const std::string& name, std::optional<Emitter> source = {});
    std::string AddInheritedEmitter(System&, const std::string& name, AssetID parent);
    Result      DuplicateEmitter(System&, const std::string& handle);
    Result      DeleteEmitter(System&, const std::string& handle);
    Result      RenameEmitter(System&, const std::string& handle, const std::string& name);
    Result      MoveEmitter(System&, const std::string& handle, int direction);
    Result      SetEmitterEnabled(System&, const std::string& handle, bool enabled);
    std::string UniqueEmitterName(const System&, const std::string& base);

    // ---- Parameters ----
    struct ParameterInfo
    {
        std::string              Name, Namespace; // Namespace: System, Emitter, Particles, User or Engine.
        Type                     Kind = Type::Float;
        std::optional<Vfx::Value> Default;
        std::vector<std::string> Writers, Readers; // Module (or renderer) names.
        bool                     ReadOnly = false;
        std::string              Warning;
    };
    // The parameters the system and the emitter `handle` (may be empty) write, read or declare.
    std::vector<ParameterInfo> ListParameters(const System&, const Context&, const std::string& handle);
    std::optional<Type>        ParameterType(const System&, const Context&, const std::string& handle, const std::string& name);
    Result AddUserParameter(System&, Type kind, const std::string& name = {});
    Result RemoveUserParameter(System&, const std::string& name);
    // Renames a user parameter and every link, binding and stage condition naming it.
    Result RenameUserParameter(System&, const std::string& from, const std::string& to);
    Result SetUserParameterDefault(System&, const std::string& name, const Vfx::Value& value);

    // ---- Data interfaces (emitter parameters) ----
    inline constexpr const char* DataInterfaceKinds[] = {"Grid3D", "VectorField", "StaticMesh", "SkeletalMesh", "Curve"};
    Result AddDataInterface(System&, const Context&, const std::string& handle, const std::string& kind);
    Result SetDataInterface(System&, const Context&, const std::string& handle, const std::string& name,
                            const Vfx::InterfaceBinding& binding);
    Result RemoveDataInterface(System&, const Context&, const std::string& handle, const std::string& name);

    // ---- Overview ----
    struct Dependency
    {
        std::string From, To, Kind, Label; // Kind: "Event" or "Reader".
    };
    std::vector<Dependency> Dependencies(const System&, const Context&);
    // "Spawn 50/s · 3 renderers".
    std::string             Summary(const Emitter&, const ModuleLibrary&);
    inline constexpr const char* SystemNode = "__system";

    // ---- Diagnostics ----
    struct StackDiagnostic
    {
        std::string Emitter;   // Handle GUID; empty for the system.
        std::optional<StackRef> Stack;
        std::string Use, Input, Nested; // Module use GUID, its input, and an input of a dynamic input there.
        std::string Renderer, Handler, SimulationStage;
        std::string Message;
        bool        Warning = false;
    };
    // Compiles the document (its emitters resolved through the context); never throws.
    std::vector<Vfx::Diagnostic> CompileSystem(const System&, const Context&, AssetID asset = {});
    StackDiagnostic              Locate(const Vfx::Diagnostic&, const System&, const Context&);
    // The overview graph: the System pseudo-node, then one node per emitter (title, enabled, CPU or
    // GPU, compile badge, summary body) and the dependency links.
    NodeCanvas::Graph BuildOverview(const System&, const Context&, const std::map<std::string, glm::vec2>& positions,
                                    const std::vector<StackDiagnostic>& diagnostics);
    // Positions left to right in handle order, the System node first.
    std::map<std::string, glm::vec2> OverviewLayout(const System&);

    // ---- Files ----
    bool IsBuiltin(const std::filesystem::path& file);
    struct Template
    {
        std::string           Name;
        std::filesystem::path File;
    };
    // The built-in .vfxemitter templates.
    std::vector<Template> Templates();
    // A new "<name>.vfxsystem" in `directory`: empty, or with one emitter per template file.
    std::variant<std::filesystem::path, std::string>
    CreateSystemFile(const std::filesystem::path& directory, const std::string& name,
                     const std::vector<std::filesystem::path>& templates = {});
    // A new "<name>.vfxemitter": the minimal emitter, or a child of `parent` storing only overrides.
    std::variant<std::filesystem::path, std::string>
    CreateEmitterFile(const std::filesystem::path& directory, const std::string& name, AssetID parent = {});
    // An editable copy of the document's file in `directory`.
    std::variant<std::filesystem::path, std::string> Duplicate(const SystemDocument&, const std::filesystem::path& directory);
} // namespace Kans::VfxSystemEditing
