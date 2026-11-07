#include "VfxSystemEditing.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <type_traits>
#include "KansEditorCore/EditStamp.h"
#include "Panels/VfxModuleEditing.h"

namespace Kans::VfxSystemEditing
{
    namespace fs = std::filesystem;
    using Vfx::Input;
    using Vfx::InputKind;
    using Vfx::ModuleUse;
    using Vfx::Override;

    namespace
    {
        std::string Text(const YAML::Node& n)
        {
            YAML::Emitter out;
            out << n;
            return std::string(out.c_str()) + "\n";
        }
        Vfx::Value Zero(Type t)
        {
            return t == Type::Int ? Vfx::Value::Int(0) : t == Type::Bool ? Vfx::Value::Bool(false) : Vfx::Value::Vector({}, t);
        }
        const char* StageLabel(Stage s)
        {
            static const char* labels[] = {"System Spawn",   "System Update",   "Emitter Spawn", "Emitter Update",
                                           "Particle Spawn", "Particle Update", "Event Handler", "Simulation Stage"};
            return labels[int(s)];
        }
        bool Allowed(const Vfx::Module& m, Stage s) { return (m.AllowedStages & (1u << uint32_t(s))) != 0; }
        std::string Identifier(const std::string& text)
        {
            std::string r;
            for (char c : text)
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    r += c;
            if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0])))
                r = "Vfx" + r;
            return r;
        }
        fs::path UniqueFile(const fs::path& directory, const std::string& stem, const std::string& extension)
        {
            fs::path file = directory / (stem + extension);
            for (int n = 1; fs::exists(file); ++n)
                file = directory / (stem + std::to_string(n) + extension);
            return file;
        }
        std::optional<std::string> WriteText(const fs::path& file, const std::string& text)
        {
            std::ofstream out(file, std::ios::binary);
            if (!out)
                return "Cannot write " + file.u8string();
            out << text;
            return out ? std::nullopt : std::optional<std::string>("Cannot write " + file.u8string());
        }
        // Two inputs are the same edit (literal, link, dynamic input with its nested inputs, binding).
        bool Same(const Input& a, const Input& b)
        {
            if (a.Kind != b.Kind)
                return false;
            switch (a.Kind)
            {
                case InputKind::Literal:
                    return a.Literal == b.Literal;
                case InputKind::Link:
                    return a.Link == b.Link;
                case InputKind::DataInterface:
                    return Text(Vfx::EncodeInterface(a.Interface)) == Text(Vfx::EncodeInterface(b.Interface));
                case InputKind::DynamicInput:
                {
                    if (a.Script != b.Script || a.Inputs.size() != b.Inputs.size())
                        return false;
                    for (const auto& [name, input] : a.Inputs)
                    {
                        auto other = b.Inputs.find(name);
                        if (other == b.Inputs.end() || !Same(input, other->second))
                            return false;
                    }
                    return true;
                }
            }
            return false;
        }
        bool ModuleAction(Override::Kind k)
        {
            return k == Override::Kind::SetInput || k == Override::Kind::SetEnabled || k == Override::Kind::MoveModule ||
                   k == Override::Kind::RemoveModule || k == Override::Kind::AddModule;
        }
        // A module-level override applies to a use inside the stack `ref`.
        void Place(Override& o, const StackRef& ref)
        {
            o.StageKind = ref.Kind;
            if (ref.Kind == Stage::EventHandler)
                o.HandlerGuid = ref.Container;
            if (ref.Kind == Stage::SimulationStage)
                o.SimulationStageGuid = ref.Container;
        }
        std::vector<Override>& LayerOverrides(EmitterHandle& h)
        {
            return LayerOf(h) == Layer::Reference ? h.Overrides : h.Inline->Overrides;
        }
        const std::vector<Override>& LayerOverrides(const EmitterHandle& h)
        {
            return LayerOf(h) == Layer::Reference ? h.Overrides : h.Inline->Overrides;
        }
        Vfx::Stack* MutableStack(Emitter& e, const StackRef& ref)
        {
            if (ref.Kind == Stage::EventHandler)
            {
                for (auto& h : e.Handlers)
                    if (h.Guid == ref.Container)
                        return &h.Modules;
                return nullptr;
            }
            if (ref.Kind == Stage::SimulationStage)
            {
                for (auto& s : e.SimulationStages)
                    if (s.Guid == ref.Container)
                        return &s.Modules;
                return nullptr;
            }
            return &e.Stages[ref.Kind];
        }
        // The stack `ref` when this layer owns it: the emitter's own stack, or the modules of a handler
        // or simulation stage the layer added. Nothing for inherited stacks.
        Vfx::Stack* OwnedStack(EmitterHandle& h, const StackRef& ref)
        {
            if (LayerOf(h) == Layer::Own)
                return MutableStack(*h.Inline, ref);
            for (auto& o : LayerOverrides(h))
            {
                if (ref.Kind == Stage::EventHandler && o.Action == Override::Kind::AddHandler && o.Handler.Guid == ref.Container)
                    return &o.Handler.Modules;
                if (ref.Kind == Stage::SimulationStage && o.Action == Override::Kind::AddSimulationStage &&
                    o.Simulation.Guid == ref.Container)
                    return &o.Simulation.Modules;
            }
            return nullptr;
        }
        Override* FindAdded(EmitterHandle& h, const std::string& use)
        {
            for (auto& o : LayerOverrides(h))
                if (o.Action == Override::Kind::AddModule && o.Added.Guid == use)
                    return &o;
            return nullptr;
        }
        // The use as this layer can change it directly (own or added), or nothing (inherited).
        ModuleUse* EditableUse(EmitterHandle& h, const StackRef& ref, const std::string& use)
        {
            if (auto* stack = OwnedStack(h, ref))
            {
                for (auto& u : *stack)
                    if (u.Guid == use)
                        return &u;
                return nullptr;
            }
            if (auto* added = FindAdded(h, use))
                return &added->Added;
            return nullptr;
        }
        void EraseOverrides(EmitterHandle& h, const std::function<bool(const Override&)>& match)
        {
            auto& v = LayerOverrides(h);
            v.erase(std::remove_if(v.begin(), v.end(), match), v.end());
        }
        // Runs an edit on a copy of the handle and keeps it only when the emitter still resolves.
        Result Guarded(System& s, const Context& ctx, const std::string& guid, const std::function<Result(EmitterHandle&)>& edit)
        {
            auto* h = FindHandle(s, guid);
            if (!h)
                return Result::Refused("No emitter " + guid);
            EmitterHandle copy = *h;
            Result        r;
            try
            {
                r = edit(copy);
                if (!r)
                    return r;
                Effective(copy, ctx);
            }
            catch (const std::exception& e)
            {
                return Result::Refused(e.what());
            }
            *h = std::move(copy);
            return r;
        }
        std::optional<Vfx::Input> Nested(const std::map<std::string, Input>& inputs, const InputPath& path, size_t from)
        {
            auto at = inputs.find(path[from]);
            if (at == inputs.end())
                return std::nullopt;
            if (from + 1 == path.size())
                return at->second;
            if (at->second.Kind != InputKind::DynamicInput)
                return std::nullopt;
            return Nested(at->second.Inputs, path, from + 1);
        }
        // Sets (or with nothing erases) the input at `path` below `inputs`; false when a level on the
        // way is not a dynamic input.
        bool Assign(std::map<std::string, Input>& inputs, const InputPath& path, size_t from, const std::optional<Input>& value)
        {
            if (from + 1 == path.size())
            {
                if (value)
                    inputs[path[from]] = *value;
                else
                    inputs.erase(path[from]);
                return true;
            }
            auto at = inputs.find(path[from]);
            if (at == inputs.end() || at->second.Kind != InputKind::DynamicInput)
                return false;
            return Assign(at->second.Inputs, path, from + 1, value);
        }
        std::string GridKind(const std::string& op)
        {
            if (op.rfind("Grid", 0) == 0)
                return "Grid3D";
            if (Vfx::IsVectorFieldOperation(op))
                return "VectorField";
            if (Vfx::IsMeshOperation(op))
                return "Mesh";
            if (op == "WriteEvent")
                return "Events";
            if (op == "ParticleCount" || op == "ReadAttribute" || op == "ReadAttributeByID")
                return "EmitterParticles";
            if (op == "SampleForceFields")
                return "ForceFields";
            return {};
        }
        // Every module use of an emitter with the stack holding it.
        void ForEachUse(const Emitter& e, const std::function<void(const StackRef&, const ModuleUse&)>& visit)
        {
            for (const auto& [stage, stack] : e.Stages)
                if (stage != Stage::EventHandler && stage != Stage::SimulationStage)
                    for (const auto& u : stack)
                        visit({stage, {}}, u);
            for (const auto& h : e.Handlers)
                for (const auto& u : h.Modules)
                    visit({Stage::EventHandler, h.Guid}, u);
            for (const auto& s : e.SimulationStages)
                for (const auto& u : s.Modules)
                    visit({Stage::SimulationStage, s.Guid}, u);
        }
        // Every input below a map, depth first (links inside dynamic inputs included).
        void ForEachInput(std::map<std::string, Input>& inputs, const std::function<void(Input&)>& visit)
        {
            for (auto& [name, input] : inputs)
            {
                visit(input);
                if (input.Kind == InputKind::DynamicInput)
                    ForEachInput(input.Inputs, visit);
            }
        }
        void ForEachInput(Emitter& e, const std::function<void(Input&)>& visit);
        void ForEachInput(std::vector<Override>& overrides, const std::function<void(Input&)>& visit)
        {
            for (auto& o : overrides)
            {
                ForEachInput(o.Added.Inputs, visit);
                visit(o.InputValue);
                if (o.InputValue.Kind == InputKind::DynamicInput)
                    ForEachInput(o.InputValue.Inputs, visit);
                for (auto& u : o.Handler.Modules)
                    ForEachInput(u.Inputs, visit);
                for (auto& u : o.Simulation.Modules)
                    ForEachInput(u.Inputs, visit);
            }
        }
        void ForEachInput(Emitter& e, const std::function<void(Input&)>& visit)
        {
            for (auto& [stage, stack] : e.Stages)
                for (auto& u : stack)
                    ForEachInput(u.Inputs, visit);
            for (auto& h : e.Handlers)
                for (auto& u : h.Modules)
                    ForEachInput(u.Inputs, visit);
            for (auto& s : e.SimulationStages)
                for (auto& u : s.Modules)
                    ForEachInput(u.Inputs, visit);
            ForEachInput(e.Overrides, visit);
        }
        std::string NamespaceOf(const std::string& name)
        {
            for (const char* ns : {"System", "Emitter", "Particles", "User", "Engine"})
                if (name.rfind(std::string(ns) + ".", 0) == 0)
                    return ns;
            return {};
        }
    } // namespace

    // ---------------------------------------------------------------------- library

    void ModuleLibrary::Load(const std::vector<std::pair<AssetID, fs::path>>& project)
    {
        auto modules = std::make_shared<std::map<AssetID, ModuleInfo>>();
        try
        {
            for (const auto& [id, file] : Vfx::FindBuiltinModules())
                try
                {
                    ModuleInfo info {id, file.stem().u8string(), file, true, Vfx::DecodeModule(YAML::LoadFile(file.string()))};
                    modules->emplace(id, std::move(info));
                }
                catch (const std::exception&)
                {}
        }
        catch (const std::exception&)
        {}
        for (const auto& [id, file] : project)
            try
            {
                ModuleInfo info {id, file.stem().u8string(), file, false, Vfx::DecodeModule(YAML::LoadFile(file.string()))};
                info.Data.Identity = id;
                (*modules)[id]     = std::move(info);
            }
            catch (const std::exception&)
            {}
        m_Modules = std::move(modules);
    }

    void ModuleLibrary::Add(ModuleInfo info)
    {
        auto modules = std::make_shared<std::map<AssetID, ModuleInfo>>(*m_Modules);
        info.Data.Identity = info.Id;
        (*modules)[info.Id] = std::move(info);
        m_Modules           = std::move(modules);
    }

    const ModuleInfo* ModuleLibrary::Find(AssetID id) const
    {
        auto at = m_Modules->find(id);
        return at == m_Modules->end() ? nullptr : &at->second;
    }

    std::string ModuleLibrary::NameOf(AssetID id) const
    {
        if (const auto* m = Find(id))
            return m->Name;
        return "Missing " + id.ToString();
    }

    std::vector<const ModuleInfo*> ModuleLibrary::All() const
    {
        std::vector<const ModuleInfo*> r;
        for (const auto& [id, info] : *m_Modules)
            r.push_back(&info);
        return r;
    }

    Vfx::ModuleResolver ModuleLibrary::Resolver() const
    {
        return [modules = m_Modules, fallback = m_Fallback](AssetID id) -> Vfx::Module {
            auto at = modules->find(id);
            if (at != modules->end())
                return at->second.Data;
            if (fallback)
                return fallback(id);
            throw std::invalid_argument("Missing VFX script " + id.ToString());
        };
    }

    Vfx::ModuleResolver Context::ResolveModule() const
    {
        if (Modules)
            return Modules->Resolver();
        return [](AssetID id) -> Vfx::Module { throw std::invalid_argument("Missing VFX script " + id.ToString()); };
    }

    Vfx::EmitterResolver Context::ResolveEmitter() const
    {
        if (Emitters)
            return Emitters;
        return [](AssetID id) -> Emitter { throw std::invalid_argument("Missing VFX emitter " + id.ToString()); };
    }

    // ---------------------------------------------------------------------- document

    System WrapEmitter(Emitter emitter)
    {
        System        s;
        EmitterHandle h;
        h.Guid   = EmitterModeHandle;
        h.Name   = emitter.Name.empty() ? "Emitter" : emitter.Name;
        h.Inline = std::move(emitter);
        s.Emitters.push_back(std::move(h));
        return s;
    }

    Emitter UnwrapEmitter(const System& system)
    {
        if (system.Emitters.empty() || !system.Emitters.front().Inline)
            return {};
        Emitter e = *system.Emitters.front().Inline;
        e.Name    = system.Emitters.front().Name;
        return e;
    }

    std::optional<std::string> SystemDocument::Open(const fs::path& file)
    {
        try
        {
            auto node = YAML::LoadFile(file.string());
            if (file.extension() == ".vfxemitter")
            {
                auto emitter = Vfx::DecodeEmitter(node);
                if (emitter.Name.empty())
                    emitter.Name = file.stem().u8string();
                Reset(WrapEmitter(std::move(emitter)), file, Mode::Emitter, IsBuiltin(file));
            }
            else
                Reset(Vfx::DecodeSystem(node), file, Mode::System, IsBuiltin(file));
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            return file.filename().u8string() + ": " + e.what();
        }
    }

    void SystemDocument::Reset(System system, fs::path file, Mode mode, bool readOnly)
    {
        m_System    = std::move(system);
        m_File      = std::move(file);
        m_Mode      = mode;
        m_ReadOnly  = readOnly;
        m_Committed = m_Saved = Canonical();
        m_Undo.clear();
        m_Redo.clear();
        m_UndoStamps.clear();
        m_RedoStamps.clear();
        ++m_Revision;
    }

    std::string SystemDocument::Canonical() const
    {
        if (m_Mode == Mode::Emitter)
            return Text(Vfx::EncodeEmitter(UnwrapEmitter(m_System)));
        return Text(Vfx::EncodeSystem(m_System));
    }

    void SystemDocument::Load(const std::string& canonical)
    {
        auto node = YAML::Load(canonical);
        if (m_Mode == Mode::Emitter)
        {
            // The handle keeps its GUID and takes the emitter's name.
            m_System = WrapEmitter(Vfx::DecodeEmitter(node));
        }
        else
            m_System = Vfx::DecodeSystem(node);
    }

    bool SystemDocument::Commit()
    {
        auto text = Canonical();
        if (text == m_Committed)
            return false;
        m_Undo.push_back(std::move(m_Committed));
        m_UndoStamps.push_back(NextEditStamp());
        if (m_Undo.size() > 200)
        {
            m_Undo.erase(m_Undo.begin());
            m_UndoStamps.erase(m_UndoStamps.begin());
        }
        m_Redo.clear();
        m_RedoStamps.clear();
        m_Committed = std::move(text);
        ++m_Revision;
        return true;
    }

    bool SystemDocument::Undo()
    {
        Commit();
        if (m_Undo.empty())
            return false;
        m_Redo.push_back(std::move(m_Committed));
        m_RedoStamps.push_back(m_UndoStamps.back());
        m_Committed = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_UndoStamps.pop_back();
        Load(m_Committed);
        ++m_Revision;
        return true;
    }

    bool SystemDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_Undo.push_back(std::move(m_Committed));
        m_UndoStamps.push_back(m_RedoStamps.back());
        m_Committed = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_RedoStamps.pop_back();
        Load(m_Committed);
        ++m_Revision;
        return true;
    }

    std::optional<uint64_t> SystemDocument::UndoStamp() const
    {
        return m_UndoStamps.empty() ? std::nullopt : std::optional<uint64_t>(m_UndoStamps.back());
    }

    std::optional<uint64_t> SystemDocument::RedoStamp() const
    {
        return m_RedoStamps.empty() ? std::nullopt : std::optional<uint64_t>(m_RedoStamps.back());
    }

    bool SystemDocument::IsDirty() const { return m_Committed != m_Saved; }

    std::optional<std::string> SystemDocument::Save()
    {
        if (m_ReadOnly)
            return std::string("Built-in templates are read-only; duplicate it to edit it.");
        if (m_File.empty())
            return std::string("The document has no file.");
        Commit();
        if (auto error = WriteText(m_File, m_Committed))
            return error;
        m_Saved = m_Committed;
        return std::nullopt;
    }

    // ---------------------------------------------------------------------- handles and layers

    EmitterHandle* FindHandle(System& s, const std::string& guid)
    {
        for (auto& h : s.Emitters)
            if (h.Guid == guid)
                return &h;
        return nullptr;
    }
    const EmitterHandle* FindHandle(const System& s, const std::string& guid)
    {
        for (const auto& h : s.Emitters)
            if (h.Guid == guid)
                return &h;
        return nullptr;
    }

    Layer LayerOf(const EmitterHandle& h)
    {
        if (!h.Inline)
            return Layer::Reference;
        return h.Inline->Parent.IsValid() ? Layer::InlineChild : Layer::Own;
    }

    Emitter Effective(const EmitterHandle& h, const Context& ctx)
    {
        Emitter e = h.Inline ? *h.Inline : ctx.ResolveEmitter()(h.Reference);
        e         = Vfx::MergeEmitter(e, ctx.ResolveEmitter());
        Vfx::ApplyOverrides(e, h.Overrides);
        e.Name = h.Name;
        return e;
    }

    Emitter Base(const EmitterHandle& h, const Context& ctx)
    {
        Emitter e;
        switch (LayerOf(h))
        {
            case Layer::Own:
                e = *h.Inline;
                break;
            case Layer::InlineChild:
            {
                Emitter child = *h.Inline;
                child.Overrides.clear();
                child.SettingOverrides.clear();
                e = Vfx::MergeEmitter(child, ctx.ResolveEmitter());
                break;
            }
            case Layer::Reference:
                e = Vfx::MergeEmitter(ctx.ResolveEmitter()(h.Reference), ctx.ResolveEmitter());
                break;
        }
        e.Name = h.Name;
        return e;
    }

    const Vfx::Stack* FindStack(const Emitter& e, const StackRef& ref)
    {
        if (ref.Kind == Stage::EventHandler)
        {
            for (const auto& h : e.Handlers)
                if (h.Guid == ref.Container)
                    return &h.Modules;
            return nullptr;
        }
        if (ref.Kind == Stage::SimulationStage)
        {
            for (const auto& s : e.SimulationStages)
                if (s.Guid == ref.Container)
                    return &s.Modules;
            return nullptr;
        }
        auto at = e.Stages.find(ref.Kind);
        static const Vfx::Stack empty;
        return at == e.Stages.end() ? &empty : &at->second;
    }

    std::optional<StackRef> StackOf(const Emitter& e, const std::string& use)
    {
        std::optional<StackRef> r;
        ForEachUse(e, [&](const StackRef& ref, const ModuleUse& u) {
            if (u.Guid == use && !r)
                r = ref;
        });
        return r;
    }

    const ModuleUse* FindUse(const Emitter& e, const std::string& use)
    {
        const ModuleUse* r = nullptr;
        ForEachUse(e, [&](const StackRef&, const ModuleUse& u) {
            if (u.Guid == use && !r)
                r = &u;
        });
        return r;
    }

    Origin ModuleOrigin(const EmitterHandle& h, const std::string& use)
    {
        if (LayerOf(h) == Layer::Own)
            return Origin::Own;
        bool overridden = false;
        for (const auto& o : LayerOverrides(h))
        {
            if (o.Action == Override::Kind::AddModule && o.Added.Guid == use)
                return Origin::Added;
            if (o.Action == Override::Kind::AddHandler &&
                std::any_of(o.Handler.Modules.begin(), o.Handler.Modules.end(), [&](const auto& u) { return u.Guid == use; }))
                return Origin::Added;
            if (o.Action == Override::Kind::AddSimulationStage &&
                std::any_of(o.Simulation.Modules.begin(), o.Simulation.Modules.end(), [&](const auto& u) { return u.Guid == use; }))
                return Origin::Added;
            if (ModuleAction(o.Action) && o.Target == use)
                overridden = true;
        }
        return overridden ? Origin::Overridden : Origin::Inherited;
    }

    bool InputOverridden(const EmitterHandle& h, const std::string& use, const std::string& input)
    {
        if (LayerOf(h) == Layer::Own)
            return false;
        for (const auto& o : LayerOverrides(h))
            if (o.Action == Override::Kind::SetInput && o.Target == use && o.InputName == input)
                return true;
        return false;
    }

    // ---------------------------------------------------------------------- module inputs

    std::vector<InputInfo> ModuleInputs(const Vfx::Module& m)
    {
        std::vector<InputInfo> r;
        for (const auto& p : m.Inputs)
        {
            InputInfo info;
            info.Spec = p;
            for (const auto& n : m.Nodes)
            {
                if (n.Parameter != p.Name)
                    continue;
                if (n.Op == "Sample")
                {
                    info.Interface      = n.Interface.Kind.empty() ? "Curve" : n.Interface.Kind;
                    info.DefaultBinding = n.Interface;
                }
                else if (auto kind = GridKind(n.Op); !kind.empty() && info.Interface.empty())
                    info.Interface = kind;
            }
            r.push_back(std::move(info));
        }
        // A position input with an int "...Space" input next to it gets that as its space toggle.
        for (auto& info : r)
            if (info.Spec.Kind == Type::Position || info.Spec.Kind == Type::Vec3)
                for (const auto& other : r)
                    if (other.Spec.Kind == Type::Int && other.Spec.EnumNames.size() == 2 &&
                        (other.Spec.Name == info.Spec.Name + "Space" || (info.Spec.Kind == Type::Position && other.Spec.Name == "Module.Space")))
                        info.SpaceInput = other.Spec.Name;
        return r;
    }

    std::optional<Type> ResultType(const Vfx::Module& m)
    {
        for (const auto& n : m.Nodes)
            if (n.Guid == m.ResultNode)
            {
                for (const auto& p : n.Outputs)
                    if (p.Name == m.ResultPin)
                        return p.Kind;
                return n.Kind;
            }
        return std::nullopt;
    }

    std::map<std::string, Input> DefaultInputs(const Vfx::Module& m)
    {
        std::map<std::string, Input> r;
        for (const auto& info : ModuleInputs(m))
        {
            Input input;
            if (info.Interface == "Curve" || info.Interface == "Gradient")
            {
                input.Kind      = InputKind::DataInterface;
                input.Interface = info.DefaultBinding;
                if (input.Interface.Values.empty())
                    continue;
            }
            else if (info.Interface.empty() && info.Spec.Default)
                input.Literal = *info.Spec.Default;
            else
                continue;
            r[info.Spec.Name] = std::move(input);
        }
        return r;
    }

    std::optional<Vfx::Input> GetInput(const ModuleUse& use, const InputPath& path)
    {
        if (path.empty())
            return std::nullopt;
        return Nested(use.Inputs, path, 0);
    }

    std::optional<Type> InputType(const Context& ctx, const ModuleUse& use, const InputPath& path)
    {
        if (path.empty() || !ctx.Modules)
            return std::nullopt;
        const auto* info = ctx.Modules->Find(use.Script);
        if (!info)
            return std::nullopt;
        const Vfx::Module* module = &info->Data;
        std::map<std::string, Input> inputs = use.Inputs;
        for (size_t k = 0; k < path.size(); ++k)
        {
            auto spec = std::find_if(module->Inputs.begin(), module->Inputs.end(), [&](const auto& p) { return p.Name == path[k]; });
            if (spec == module->Inputs.end())
                return std::nullopt;
            if (k + 1 == path.size())
                return spec->Kind;
            auto at = inputs.find(path[k]);
            if (at == inputs.end() || at->second.Kind != InputKind::DynamicInput)
                return std::nullopt;
            const auto* nested = ctx.Modules->Find(at->second.Script);
            if (!nested)
                return std::nullopt;
            module = &nested->Data;
            inputs = at->second.Inputs;
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------------- stack edits

    Result AddModule(System& s, const Context& ctx, const std::string& handle, const StackRef& ref, AssetID script, size_t index)
    {
        const auto* info = ctx.Modules ? ctx.Modules->Find(script) : nullptr;
        if (!info)
            return Result::Refused("Missing module " + script.ToString());
        if (info->Data.ScriptUsage != Vfx::Usage::Module)
            return Result::Refused(info->Name + " is not a module");
        if (!Allowed(info->Data, ref.Kind))
            return Result::Refused(info->Name + " is not allowed in " + StageLabel(ref.Kind));
        ModuleUse use {VfxModuleEditing::NewGuid(), script, true, DefaultInputs(info->Data)};
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (auto* stack = OwnedStack(h, ref))
            {
                stack->insert(stack->begin() + std::min(index, stack->size()), use);
                return Result::Done(use.Guid);
            }
            const auto  effective = Effective(h, ctx);
            const auto* stack     = FindStack(effective, ref);
            if (!stack)
                return Result::Refused("No such stack");
            Override o;
            o.Action = Override::Kind::AddModule;
            Place(o, ref);
            o.Added  = use;
            o.Before = index < stack->size() ? (*stack)[index].Guid : std::string();
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done(use.Guid);
        });
    }

    Result RemoveModule(System& s, const Context& ctx, const std::string& handle, const std::string& use)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto effective = Effective(h, ctx);
            auto       ref       = StackOf(effective, use);
            if (!ref)
                return Result::Refused("No module " + use);
            if (auto* stack = OwnedStack(h, *ref))
            {
                stack->erase(std::remove_if(stack->begin(), stack->end(), [&](const auto& u) { return u.Guid == use; }), stack->end());
                return Result::Done();
            }
            if (FindAdded(h, use))
            {
                EraseOverrides(h, [&](const Override& o) { return o.Action == Override::Kind::AddModule && o.Added.Guid == use; });
                return Result::Done();
            }
            EraseOverrides(h, [&](const Override& o) { return ModuleAction(o.Action) && o.Target == use; });
            Override o;
            o.Action = Override::Kind::RemoveModule;
            Place(o, *ref);
            o.Target = use;
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    Result MoveModule(System& s, const Context& ctx, const std::string& handle, const std::string& use, const StackRef& to,
                      size_t index)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto effective = Effective(h, ctx);
            auto       from      = StackOf(effective, use);
            const auto* current  = FindUse(effective, use);
            if (!from || !current)
                return Result::Refused("No module " + use);
            const auto* info = ctx.Modules ? ctx.Modules->Find(current->Script) : nullptr;
            if (to != *from && (!info || !Allowed(info->Data, to.Kind)))
                return Result::Refused((info ? info->Name : current->Script.ToString()) + " is not allowed in " + StageLabel(to.Kind));
            if (!FindStack(effective, to))
                return Result::Refused("No such stack");
            ModuleUse moved = *current;
            // The target order without the moved use: `index` counts in it.
            std::vector<std::string> order;
            for (const auto& u : *FindStack(effective, to))
                if (u.Guid != use)
                    order.push_back(u.Guid);
            index                  = std::min(index, order.size());
            const std::string before = index < order.size() ? order[index] : std::string();
            auto* source = OwnedStack(h, *from);
            auto* target = OwnedStack(h, to);
            if (source && target)
            {
                source->erase(std::remove_if(source->begin(), source->end(), [&](const auto& u) { return u.Guid == use; }),
                              source->end());
                auto at = std::find_if(target->begin(), target->end(), [&](const auto& u) { return u.Guid == before; });
                target->insert(at, moved);
                return Result::Done();
            }
            const bool added = source || FindAdded(h, use);
            if (!added)
            {
                // An inherited use: a GUID-keyed move, into another stack when `to` differs.
                Override o;
                o.Action = Override::Kind::MoveModule;
                Place(o, *from);
                o.Target = use;
                o.Before = before;
                if (to != *from)
                {
                    o.ToStage     = to.Kind;
                    o.ToContainer = to.Container;
                }
                LayerOverrides(h).push_back(std::move(o));
                return Result::Done();
            }
            // An added use: take it out, then add it again where it goes.
            if (source)
                source->erase(std::remove_if(source->begin(), source->end(), [&](const auto& u) { return u.Guid == use; }),
                              source->end());
            else
                EraseOverrides(h, [&](const Override& o) { return o.Action == Override::Kind::AddModule && o.Added.Guid == use; });
            if (target)
            {
                auto at = std::find_if(target->begin(), target->end(), [&](const auto& u) { return u.Guid == before; });
                target->insert(at, moved);
                return Result::Done();
            }
            Override o;
            o.Action = Override::Kind::AddModule;
            Place(o, to);
            o.Added  = moved;
            o.Before = before;
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    Result DuplicateModule(System& s, const Context& ctx, const std::string& handle, const std::string& use)
    {
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        Emitter effective;
        try
        {
            effective = Effective(*h, ctx);
        }
        catch (const std::exception& e)
        {
            return Result::Refused(e.what());
        }
        auto        ref     = StackOf(effective, use);
        const auto* current = FindUse(effective, use);
        if (!ref || !current)
            return Result::Refused("No module " + use);
        const auto* stack = FindStack(effective, *ref);
        size_t      index = 0;
        while (index < stack->size() && (*stack)[index].Guid != use)
            ++index;
        ModuleUse copy = *current;
        auto      r    = AddModule(s, ctx, handle, *ref, copy.Script, index + 1);
        if (!r)
            return r;
        // The copy keeps the inputs and enabled state.
        auto guid = r.Value;
        Guarded(s, ctx, handle, [&](EmitterHandle& eh) -> Result {
            if (auto* u = EditableUse(eh, *ref, guid))
            {
                u->Inputs  = copy.Inputs;
                u->Enabled = copy.Enabled;
            }
            return Result::Done();
        });
        return Result::Done(guid);
    }

    Result SetModuleEnabled(System& s, const Context& ctx, const std::string& handle, const std::string& use, bool enabled)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto effective = Effective(h, ctx);
            auto       ref       = StackOf(effective, use);
            if (!ref)
                return Result::Refused("No module " + use);
            if (auto* u = EditableUse(h, *ref, use))
            {
                u->Enabled = enabled;
                return Result::Done();
            }
            EraseOverrides(h, [&](const Override& o) { return o.Action == Override::Kind::SetEnabled && o.Target == use; });
            const auto  base    = Base(h, ctx);
            const auto* inBase  = FindUse(base, use);
            if (inBase && inBase->Enabled == enabled)
                return Result::Done();
            Override o;
            o.Action = Override::Kind::SetEnabled;
            Place(o, *ref);
            o.Target  = use;
            o.Enabled = enabled;
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    Result ResetModule(System& s, const Context& ctx, const std::string& handle, const std::string& use)
    {
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        const auto origin = ModuleOrigin(*h, use);
        if (origin == Origin::Own || origin == Origin::Added)
            return Result::Refused("The module has no parent to reset to");
        return Guarded(s, ctx, handle, [&](EmitterHandle& eh) -> Result {
            EraseOverrides(eh, [&](const Override& o) {
                return (o.Action == Override::Kind::SetInput || o.Action == Override::Kind::SetEnabled ||
                        o.Action == Override::Kind::MoveModule) &&
                       o.Target == use;
            });
            return Result::Done();
        });
    }

    namespace
    {
        // Writes `top` as the use's whole top-level input (nothing: back to the default or parent).
        Result WriteTop(EmitterHandle& h, const Context& ctx, const StackRef& ref, const std::string& use,
                        const std::string& name, const std::optional<Input>& top)
        {
            if (auto* u = EditableUse(h, ref, use))
            {
                if (top)
                    u->Inputs[name] = *top;
                else
                    u->Inputs.erase(name);
                return Result::Done();
            }
            EraseOverrides(h, [&](const Override& o) {
                return o.Action == Override::Kind::SetInput && o.Target == use && o.InputName == name;
            });
            if (!top)
                return Result::Done();
            const auto  base   = Base(h, ctx);
            const auto* inBase = FindUse(base, use);
            if (inBase)
            {
                auto parent = inBase->Inputs.find(name);
                if (parent != inBase->Inputs.end() && Same(parent->second, *top))
                    return Result::Done(); // Equal to the parent: no override.
            }
            Override o;
            o.Action = Override::Kind::SetInput;
            Place(o, ref);
            o.Target     = use;
            o.InputName  = name;
            o.InputValue = *top;
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        }
        Result EditInput(System& s, const Context& ctx, const std::string& handle, const std::string& use, const InputPath& path,
                         const std::optional<Input>& value)
        {
            if (path.empty())
                return Result::Refused("No input");
            return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
                const auto  effective = Effective(h, ctx);
                auto        ref       = StackOf(effective, use);
                const auto* current   = FindUse(effective, use);
                if (!ref || !current)
                    return Result::Refused("No module " + use);
                std::optional<Input> top;
                if (path.size() == 1)
                    top = value;
                else
                {
                    auto at = current->Inputs.find(path[0]);
                    if (at == current->Inputs.end() || at->second.Kind != InputKind::DynamicInput)
                        return Result::Refused(path[0] + " is not a dynamic input");
                    top = at->second;
                    if (!Assign(top->Inputs, path, 1, value))
                        return Result::Refused("No nested input " + path.back());
                }
                return WriteTop(h, ctx, *ref, use, path[0], top);
            });
        }
    } // namespace

    Result SetInput(System& s, const Context& ctx, const std::string& handle, const std::string& use, const InputPath& path,
                    Vfx::Input value)
    {
        return EditInput(s, ctx, handle, use, path, value);
    }

    Result ResetInput(System& s, const Context& ctx, const std::string& handle, const std::string& use, const InputPath& path)
    {
        return EditInput(s, ctx, handle, use, path, std::nullopt);
    }

    Result LinkInput(System& s, const Context& ctx, const std::string& handle, const std::string& use, const InputPath& path,
                     const std::string& parameter)
    {
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        Emitter effective;
        try
        {
            effective = Effective(*h, ctx);
        }
        catch (const std::exception& e)
        {
            return Result::Refused(e.what());
        }
        const auto* current = FindUse(effective, use);
        if (!current)
            return Result::Refused("No module " + use);
        auto inputType = InputType(ctx, *current, path);
        if (!inputType)
            return Result::Refused("Unknown input " + (path.empty() ? std::string() : path.back()));
        auto parameterType = ParameterType(s, ctx, handle, parameter);
        if (!parameterType)
            return Result::Refused("Unknown parameter " + parameter);
        if (*parameterType != *inputType)
            return Result::Refused(parameter + " is " + Vfx::TypeName(*parameterType) + "; " + path.back() + " needs " +
                                   Vfx::TypeName(*inputType));
        Input link;
        link.Kind = InputKind::Link;
        link.Link = parameter;
        return SetInput(s, ctx, handle, use, path, link);
    }

    Result SetDynamicInput(System& s, const Context& ctx, const std::string& handle, const std::string& use,
                           const InputPath& path, AssetID script)
    {
        const auto* info = ctx.Modules ? ctx.Modules->Find(script) : nullptr;
        if (!info)
            return Result::Refused("Missing script " + script.ToString());
        if (info->Data.ScriptUsage != Vfx::Usage::DynamicInput)
            return Result::Refused(info->Name + " is not a dynamic input");
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        Emitter effective;
        try
        {
            effective = Effective(*h, ctx);
        }
        catch (const std::exception& e)
        {
            return Result::Refused(e.what());
        }
        const auto* current = FindUse(effective, use);
        if (!current)
            return Result::Refused("No module " + use);
        auto inputType = InputType(ctx, *current, path);
        auto result    = ResultType(info->Data);
        if (!inputType || !result)
            return Result::Refused("Unknown input or result type");
        if (!Vfx::CanConvert(*result, *inputType))
            return Result::Refused(info->Name + " returns " + Vfx::TypeName(*result) + "; " + path.back() + " needs " +
                                   Vfx::TypeName(*inputType));
        Input dynamic;
        dynamic.Kind   = InputKind::DynamicInput;
        dynamic.Script = script;
        dynamic.Inputs = DefaultInputs(info->Data);
        return SetInput(s, ctx, handle, use, path, dynamic);
    }

    // ---------------------------------------------------------------------- module search

    namespace
    {
        std::vector<ModuleEntry> Search(const ModuleLibrary& library, const std::function<bool(const ModuleInfo&)>& accept,
                                        const std::string& query)
        {
            std::vector<ModuleEntry> r;
            for (const auto* m : library.All())
            {
                if (!accept(*m))
                    continue;
                ModuleEntry e;
                e.Id          = m->Id;
                e.Name        = m->Name;
                e.Builtin     = m->Builtin;
                e.Category    = m->Data.Category.empty() || m->Data.Category == "Built-in" ? (m->Builtin ? "Built-in" : "Project")
                                                                                            : m->Data.Category;
                e.Description = m->Data.Description == m->Name ? std::string() : m->Data.Description;
                if (e.Description.empty())
                {
                    // A generated description: what it is and where it runs.
                    std::string stages;
                    for (int k = 0; k < 8; ++k)
                        if (m->Data.AllowedStages & (1u << k))
                            stages += (stages.empty() ? "" : ", ") + std::string(StageLabel(Stage(k)));
                    e.Description = (m->Data.ScriptUsage == Vfx::Usage::DynamicInput ? std::string("Dynamic input") : "Module") +
                                    std::string(m->Builtin ? " (built-in)" : " (project)") + " for " + stages;
                }
                if (!query.empty())
                {
                    e.Score = std::max({VfxModuleEditing::FuzzyScore(e.Name, query),
                                        VfxModuleEditing::FuzzyScore(e.Category, query) - 300,
                                        VfxModuleEditing::FuzzyScore(e.Description, query) - 400});
                    if (VfxModuleEditing::FuzzyScore(e.Name, query) < 0 && VfxModuleEditing::FuzzyScore(e.Category, query) < 0 &&
                        VfxModuleEditing::FuzzyScore(e.Description, query) < 0)
                        continue;
                }
                r.push_back(std::move(e));
            }
            std::stable_sort(r.begin(), r.end(), [&](const ModuleEntry& a, const ModuleEntry& b) {
                if (!query.empty() && a.Score != b.Score)
                    return a.Score > b.Score;
                if (a.Category != b.Category)
                    return a.Category < b.Category;
                return a.Name < b.Name;
            });
            return r;
        }
    } // namespace

    std::vector<ModuleEntry> SearchModules(const ModuleLibrary& library, Stage stage, const std::string& query)
    {
        return Search(library, [&](const ModuleInfo& m) { return m.Data.ScriptUsage == Vfx::Usage::Module && Allowed(m.Data, stage); },
                      query);
    }

    std::vector<ModuleEntry> SearchDynamicInputs(const ModuleLibrary& library, Stage stage, Type kind, const std::string& query)
    {
        return Search(library,
                      [&](const ModuleInfo& m) {
                          auto result = ResultType(m.Data);
                          return m.Data.ScriptUsage == Vfx::Usage::DynamicInput && Allowed(m.Data, stage) && result &&
                                 Vfx::CanConvert(*result, kind);
                      },
                      query);
    }

    // ---------------------------------------------------------------------- curves and gradients

    TimelineCurve CurveFromBinding(const Vfx::InterfaceBinding& b)
    {
        TimelineCurve c;
        for (size_t k = 0; k < b.Times.size() && k < b.Values.size(); ++k)
        {
            TimelineKey key;
            key.Time  = b.Times[k];
            key.Value = b.Values[k];
            c.Keys.push_back(key);
        }
        return c;
    }

    Vfx::InterfaceBinding BindingFromCurve(const TimelineCurve& c)
    {
        Vfx::InterfaceBinding b;
        b.Kind = "Curve";
        for (const auto& key : c.Keys)
        {
            b.Times.push_back(float(key.Time));
            b.Values.push_back(glm::vec4(key.Value.x));
        }
        return b;
    }

    namespace
    {
        template<class Stop, class V, class Get>
        V Piecewise(const std::vector<Stop>& stops, float t, bool fixed, Get get)
        {
            if (stops.empty())
                return V(1);
            if (t <= stops.front().Time)
                return get(stops.front());
            for (size_t k = 1; k < stops.size(); ++k)
                if (t < stops[k].Time)
                {
                    if (fixed)
                        return get(stops[k - 1]);
                    const float span = stops[k].Time - stops[k - 1].Time;
                    const float f    = span > 0 ? (t - stops[k - 1].Time) / span : 1.0f;
                    return get(stops[k - 1]) + (get(stops[k]) - get(stops[k - 1])) * f;
                }
            return get(stops.back());
        }
    } // namespace

    glm::vec4 Gradient::Evaluate(float time) const
    {
        const bool fixed = Mode == Blend::Fixed;
        const auto color = Piecewise<ColorStop, glm::vec3>(Colors, time, fixed, [](const ColorStop& s) { return s.Color; });
        const auto alpha = Piecewise<AlphaStop, float>(Alphas, time, fixed, [](const AlphaStop& s) { return s.Alpha; });
        return {color, alpha};
    }

    void Gradient::Sort()
    {
        std::stable_sort(Colors.begin(), Colors.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
        std::stable_sort(Alphas.begin(), Alphas.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
    }

    Vfx::InterfaceBinding Gradient::Bake() const
    {
        Gradient g = *this;
        g.Sort();
        std::vector<float> times;
        for (const auto& c : g.Colors)
            times.push_back(c.Time);
        for (const auto& a : g.Alphas)
            times.push_back(a.Time);
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        Vfx::InterfaceBinding b;
        b.Kind = "Gradient";
        for (size_t k = 0; k < times.size(); ++k)
        {
            // Fixed: a step at every stop, written as the value before it then the value from it.
            if (g.Mode == Blend::Fixed && k > 0)
            {
                b.Times.push_back(times[k]);
                b.Values.push_back(g.Evaluate(times[k - 1]));
            }
            b.Times.push_back(times[k]);
            b.Values.push_back(g.Evaluate(times[k]));
        }
        return b;
    }

    Gradient Gradient::FromBinding(const Vfx::InterfaceBinding& b)
    {
        Gradient g;
        g.Colors.clear();
        g.Alphas.clear();
        const size_t n = std::min(b.Times.size(), b.Values.size());
        for (size_t k = 1; k < n; ++k)
            if (b.Times[k] == b.Times[k - 1])
                g.Mode = Blend::Fixed;
        for (size_t k = 0; k < n; ++k)
        {
            // One stop per distinct time: the value from that time on.
            if (k + 1 < n && b.Times[k + 1] == b.Times[k])
                continue;
            g.Colors.push_back({b.Times[k], glm::vec3(b.Values[k])});
            g.Alphas.push_back({b.Times[k], b.Values[k].w});
        }
        if (g.Colors.empty())
            return Gradient {};
        // Drop stops that change nothing: equal to the previous one (fixed), or on the line between
        // their neighbours (linear).
        const auto lift     = [](auto v) {
            if constexpr (std::is_same_v<decltype(v), float>)
                return glm::vec4(v);
            else
                return glm::vec4(v, 0.0f);
        };
        const auto simplify = [&](auto& stops, auto get) {
            for (size_t k = 1; k + 1 < stops.size();)
            {
                bool redundant;
                if (g.Mode == Blend::Fixed)
                    redundant = glm::all(glm::lessThan(glm::abs(lift(get(stops[k])) - lift(get(stops[k - 1]))), glm::vec4(1e-5f)));
                else
                {
                    const float span = stops[k + 1].Time - stops[k - 1].Time;
                    const float f    = span > 0 ? (stops[k].Time - stops[k - 1].Time) / span : 0;
                    const auto  line = get(stops[k - 1]) + (get(stops[k + 1]) - get(stops[k - 1])) * f;
                    redundant = glm::all(glm::lessThan(glm::abs(lift(get(stops[k])) - lift(line)), glm::vec4(1e-5f)));
                }
                if (redundant)
                    stops.erase(stops.begin() + k);
                else
                    ++k;
            }
            if (g.Mode == Blend::Fixed && stops.size() == 2 &&
                glm::all(glm::lessThan(glm::abs(lift(get(stops[1])) - lift(get(stops[0]))), glm::vec4(1e-5f))))
                stops.pop_back();
        };
        simplify(g.Colors, [](const ColorStop& s) { return s.Color; });
        simplify(g.Alphas, [](const AlphaStop& s) { return s.Alpha; });
        return g;
    }

    // ---------------------------------------------------------------------- simulation stages

    Result AddSimulationStage(System& s, const Context& ctx, const std::string& handle, const std::string& name,
                              Vfx::IterationSource source, const std::string& dataInterface, uint32_t iterations)
    {
        Vfx::SimulationStage stage;
        stage.Guid          = VfxModuleEditing::NewGuid();
        stage.Name          = name.empty() ? "SimulationStage" : name;
        stage.Source        = source;
        stage.DataInterface = source == Vfx::IterationSource::DataInterface ? dataInterface : std::string();
        stage.Iterations    = iterations;
        if (auto error = Vfx::SimulationStageError(stage))
            return Result::Refused(*error);
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
                h.Inline->SimulationStages.push_back(stage);
            else
            {
                Override o;
                o.Action     = Override::Kind::AddSimulationStage;
                o.StageKind  = Stage::SimulationStage;
                o.Simulation = stage;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done(stage.Guid);
        });
    }

    Result SetSimulationStage(System& s, const Context& ctx, const std::string& handle, const Vfx::SimulationStage& settings)
    {
        if (auto error = Vfx::SimulationStageError(settings))
            return Result::Refused(*error);
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto apply = [&](Vfx::SimulationStage& stage) {
                stage.Name          = settings.Name;
                stage.Source        = settings.Source;
                stage.DataInterface = settings.Source == Vfx::IterationSource::DataInterface ? settings.DataInterface : std::string();
                stage.Iterations    = settings.Iterations;
                stage.Enabled       = settings.Enabled;
            };
            if (LayerOf(h) == Layer::Own)
            {
                for (auto& stage : h.Inline->SimulationStages)
                    if (stage.Guid == settings.Guid)
                    {
                        apply(stage);
                        return Result::Done();
                    }
                return Result::Refused("No simulation stage " + settings.Guid);
            }
            auto& overrides = LayerOverrides(h);
            for (auto& o : overrides)
                if ((o.Action == Override::Kind::AddSimulationStage || o.Action == Override::Kind::ReplaceSimulationStage) &&
                    o.Simulation.Guid == settings.Guid)
                {
                    apply(o.Simulation);
                    return Result::Done();
                }
            // An inherited stage: replaced by a copy of the parent's, placed before the module
            // overrides that edit its stack so they still apply.
            const auto base = Base(h, ctx);
            for (const auto& stage : base.SimulationStages)
                if (stage.Guid == settings.Guid)
                {
                    Override o;
                    o.Action     = Override::Kind::ReplaceSimulationStage;
                    o.StageKind  = Stage::SimulationStage;
                    o.Target     = stage.Guid;
                    o.Simulation = stage;
                    apply(o.Simulation);
                    overrides.insert(overrides.begin(), std::move(o));
                    return Result::Done();
                }
            return Result::Refused("No simulation stage " + settings.Guid);
        });
    }

    Result RemoveSimulationStage(System& s, const Context& ctx, const std::string& handle, const std::string& guid)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
            {
                auto& v = h.Inline->SimulationStages;
                auto  n = v.size();
                v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; }), v.end());
                return v.size() < n ? Result::Done() : Result::Refused("No simulation stage " + guid);
            }
            bool added = false;
            for (const auto& o : LayerOverrides(h))
                added = added || (o.Action == Override::Kind::AddSimulationStage && o.Simulation.Guid == guid);
            EraseOverrides(h, [&](const Override& o) {
                return o.SimulationStageGuid == guid || (o.Action >= Override::Kind::AddSimulationStage &&
                                                         (o.Target == guid || o.Simulation.Guid == guid));
            });
            if (!added)
            {
                Override o;
                o.Action    = Override::Kind::RemoveSimulationStage;
                o.StageKind = Stage::SimulationStage;
                o.Target    = guid;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done();
        });
    }

    Result MoveSimulationStage(System& s, const Context& ctx, const std::string& handle, const std::string& guid, int direction)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto effective = Effective(h, ctx);
            const auto& v        = effective.SimulationStages;
            auto at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; });
            if (at == v.end())
                return Result::Refused("No simulation stage " + guid);
            const long index = long(at - v.begin()) + direction;
            if (index < 0 || index >= long(v.size()))
                return Result::Refused("Already at the end");
            if (LayerOf(h) == Layer::Own)
            {
                auto& own = h.Inline->SimulationStages;
                std::swap(own[size_t(at - v.begin())], own[size_t(index)]);
                return Result::Done();
            }
            // The stage goes before the one after its new place (or to the end).
            std::vector<std::string> order;
            for (const auto& x : v)
                if (x.Guid != guid)
                    order.push_back(x.Guid);
            Override o;
            o.Action    = Override::Kind::MoveSimulationStage;
            o.StageKind = Stage::SimulationStage;
            o.Target    = guid;
            o.Before    = size_t(index) < order.size() ? order[size_t(index)] : std::string();
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    // ---------------------------------------------------------------------- events

    std::vector<std::string> EventChannels(const System& s, const Context& ctx, const std::string& handle)
    {
        std::vector<std::string> r;
        if (const auto* h = FindHandle(s, handle))
            try
            {
                for (const auto& c : Effective(*h, ctx).Channels)
                    r.push_back(c.Name);
            }
            catch (const std::exception&)
            {}
        return r;
    }

    std::vector<std::string> EventLayouts(const System& s, const Context& ctx, const std::string& handle)
    {
        std::vector<std::string> r;
        const auto*              h = FindHandle(s, handle);
        if (!h || !ctx.Modules)
            return r;
        try
        {
            ForEachUse(Effective(*h, ctx), [&](const StackRef&, const ModuleUse& u) {
                if (const auto* m = ctx.Modules->Find(u.Script))
                    for (const auto& l : m->Data.EventLayouts)
                        if (std::find(r.begin(), r.end(), l.Name) == r.end())
                            r.push_back(l.Name);
            });
        }
        catch (const std::exception&)
        {}
        return r;
    }

    Result AddEventChannel(System& s, const Context& ctx, const std::string& handle, const std::string& layout)
    {
        auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        if (LayerOf(*h) != Layer::Own)
            return Result::Refused("Event channels belong to the parent emitter; add them there");
        auto& channels = h->Inline->Channels;
        if (std::any_of(channels.begin(), channels.end(), [&](const auto& c) { return c.Name == layout; }))
            return Result::Refused("The channel " + layout + " exists");
        channels.push_back({layout, layout, 1024});
        return Result::Done(layout);
    }

    Result AddEventHandler(System& s, const Context& ctx, const std::string& handle, const std::string& source,
                           const std::string& channel, Vfx::EventMode mode, uint32_t spawnNumber)
    {
        if (!FindHandle(s, source))
            return Result::Refused("No source emitter " + source);
        Vfx::EventHandler handler;
        handler.Guid          = VfxModuleEditing::NewGuid();
        handler.SourceEmitter = source;
        handler.Channel       = channel;
        handler.Mode          = mode;
        handler.SpawnNumber   = spawnNumber;
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
                h.Inline->Handlers.push_back(handler);
            else
            {
                Override o;
                o.Action    = Override::Kind::AddHandler;
                o.StageKind = Stage::EventHandler;
                o.Handler   = handler;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done(handler.Guid);
        });
    }

    Result SetEventHandler(System& s, const Context& ctx, const std::string& handle, const Vfx::EventHandler& settings)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto apply = [&](Vfx::EventHandler& x) {
                x.SourceEmitter     = settings.SourceEmitter;
                x.Channel           = settings.Channel;
                x.Enabled           = settings.Enabled;
                x.Mode              = settings.Mode;
                x.SpawnNumber       = settings.SpawnNumber;
                x.MaxEventsPerFrame = settings.MaxEventsPerFrame;
            };
            if (LayerOf(h) == Layer::Own)
            {
                for (auto& x : h.Inline->Handlers)
                    if (x.Guid == settings.Guid)
                    {
                        apply(x);
                        return Result::Done();
                    }
                return Result::Refused("No event handler " + settings.Guid);
            }
            for (auto& o : LayerOverrides(h))
                if (o.Action == Override::Kind::AddHandler && o.Handler.Guid == settings.Guid)
                {
                    apply(o.Handler);
                    return Result::Done();
                }
            // Inherited: only the counts and the enabled state can be overridden.
            const auto base = Base(h, ctx);
            auto at = std::find_if(base.Handlers.begin(), base.Handlers.end(), [&](const auto& x) { return x.Guid == settings.Guid; });
            if (at == base.Handlers.end())
                return Result::Refused("No event handler " + settings.Guid);
            if (at->SourceEmitter != settings.SourceEmitter || at->Channel != settings.Channel || at->Mode != settings.Mode)
                return Result::Refused("An inherited handler keeps its parent's source, channel and mode");
            EraseOverrides(h, [&](const Override& o) {
                return o.StageKind == Stage::EventHandler && o.HandlerGuid.empty() && o.Target == settings.Guid &&
                       (o.Action == Override::Kind::SetInput || o.Action == Override::Kind::SetEnabled);
            });
            const auto count = [&](const char* name, uint32_t value, uint32_t parent) {
                if (value == parent)
                    return;
                Override o;
                o.Action             = Override::Kind::SetInput;
                o.StageKind          = Stage::EventHandler;
                o.Target             = settings.Guid;
                o.InputName          = name;
                o.InputValue.Literal = Vfx::Value::Int(int32_t(std::min<uint32_t>(value, INT32_MAX)));
                LayerOverrides(h).push_back(std::move(o));
            };
            count("SpawnNumber", settings.SpawnNumber, at->SpawnNumber);
            count("MaxEventsPerFrame", settings.MaxEventsPerFrame, at->MaxEventsPerFrame);
            if (settings.Enabled != at->Enabled)
            {
                Override o;
                o.Action    = Override::Kind::SetEnabled;
                o.StageKind = Stage::EventHandler;
                o.Target    = settings.Guid;
                o.Enabled   = settings.Enabled;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done();
        });
    }

    Result RemoveEventHandler(System& s, const Context& ctx, const std::string& handle, const std::string& guid)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
            {
                auto& v = h.Inline->Handlers;
                auto  n = v.size();
                v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; }), v.end());
                return v.size() < n ? Result::Done() : Result::Refused("No event handler " + guid);
            }
            bool added = false;
            for (const auto& o : LayerOverrides(h))
                added = added || (o.Action == Override::Kind::AddHandler && o.Handler.Guid == guid);
            EraseOverrides(h, [&](const Override& o) {
                return o.HandlerGuid == guid || (o.StageKind == Stage::EventHandler && (o.Target == guid || o.Handler.Guid == guid));
            });
            if (!added)
            {
                Override o;
                o.Action    = Override::Kind::RemoveHandler;
                o.StageKind = Stage::EventHandler;
                o.Target    = guid;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done();
        });
    }

    Result ConnectEmitters(System& s, const Context& ctx, const std::string& source, const std::string& target)
    {
        if (source == target)
            return Result::Refused("An emitter cannot handle its own events here");
        const auto* from = FindHandle(s, source);
        if (!from || !FindHandle(s, target))
            return Result::Refused("No such emitter");
        auto channels = EventChannels(s, ctx, source);
        if (channels.empty())
        {
            auto layouts = EventLayouts(s, ctx, source);
            if (layouts.empty())
                return Result::Refused(from->Name + " writes no events; add an event generator module (for example GenerateDeathEvent) first");
            auto added = AddEventChannel(s, ctx, source, layouts.front());
            if (!added)
                return added;
            channels = {added.Value};
        }
        return AddEventHandler(s, ctx, target, source, channels.front());
    }

    // ---------------------------------------------------------------------- renderers

    Result AddRenderer(System& s, const Context& ctx, const std::string& handle, Vfx::RendererKind kind)
    {
        Vfx::Renderer r;
        r.Guid = VfxModuleEditing::NewGuid();
        r.Kind = kind;
        if (kind == Vfx::RendererKind::Mesh)
            r.FacingMode = Vfx::Facing::Default;
        if (kind == Vfx::RendererKind::Ribbon)
            r.CastShadows = false;
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
                h.Inline->Renderers.push_back(r);
            else
            {
                Override o;
                o.Action = Override::Kind::AddRenderer;
                o.Render = r;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done(r.Guid);
        });
    }

    Result SetRenderer(System& s, const Context& ctx, const std::string& handle, const Vfx::Renderer& renderer)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
            {
                for (auto& x : h.Inline->Renderers)
                    if (x.Guid == renderer.Guid)
                    {
                        x = renderer;
                        return Result::Done();
                    }
                return Result::Refused("No renderer " + renderer.Guid);
            }
            for (auto& o : LayerOverrides(h))
                if ((o.Action == Override::Kind::AddRenderer || o.Action == Override::Kind::ReplaceRenderer) &&
                    o.Render.Guid == renderer.Guid)
                {
                    o.Render = renderer;
                    return Result::Done();
                }
            Override o;
            o.Action = Override::Kind::ReplaceRenderer;
            o.Target = renderer.Guid;
            o.Render = renderer;
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    Result RemoveRenderer(System& s, const Context& ctx, const std::string& handle, const std::string& guid)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
            {
                auto& v = h.Inline->Renderers;
                auto  n = v.size();
                v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; }), v.end());
                return v.size() < n ? Result::Done() : Result::Refused("No renderer " + guid);
            }
            bool added = false;
            for (const auto& o : LayerOverrides(h))
                added = added || (o.Action == Override::Kind::AddRenderer && o.Render.Guid == guid);
            if (added)
            {
                // Moves placed before the renderer that goes away keep their place before its successor.
                std::vector<std::string> after;
                bool                     found = false;
                for (const auto& x : Effective(h, ctx).Renderers)
                {
                    if (found)
                        after.push_back(x.Guid);
                    found = found || x.Guid == guid;
                }
                for (auto& o : LayerOverrides(h))
                    if (o.Action == Override::Kind::MoveRenderer && o.Before == guid)
                    {
                        auto next = std::find_if(after.begin(), after.end(), [&](const auto& g) { return g != o.Target; });
                        o.Before  = next != after.end() ? *next : std::string();
                    }
            }
            EraseOverrides(h, [&](const Override& o) {
                return (o.Action == Override::Kind::AddRenderer || o.Action == Override::Kind::ReplaceRenderer ||
                        o.Action == Override::Kind::RemoveRenderer || (added && o.Action == Override::Kind::MoveRenderer)) &&
                       (o.Render.Guid == guid || o.Target == guid);
            });
            if (!added)
            {
                Override o;
                o.Action = Override::Kind::RemoveRenderer;
                o.Target = guid;
                LayerOverrides(h).push_back(std::move(o));
            }
            return Result::Done();
        });
    }

    Result MoveRenderer(System& s, const Context& ctx, const std::string& handle, const std::string& guid, int direction)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) == Layer::Own)
            {
                auto& v  = h.Inline->Renderers;
                auto  at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; });
                if (at == v.end())
                    return Result::Refused("No renderer " + guid);
                const long index = long(at - v.begin()) + direction;
                if (index < 0 || index >= long(v.size()))
                    return Result::Refused("Already at the end");
                std::swap(*at, v[size_t(index)]);
                return Result::Done();
            }
            // A child: one MoveRenderer override keyed by GUID, placing it before its new successor.
            const auto effective = Effective(h, ctx);
            const auto& v        = effective.Renderers;
            auto at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; });
            if (at == v.end())
                return Result::Refused("No renderer " + guid);
            const long index = long(at - v.begin()) + direction;
            if (index < 0 || index >= long(v.size()))
                return Result::Refused("Already at the end");
            std::vector<std::string> order;
            for (const auto& x : v)
                if (x.Guid != guid)
                    order.push_back(x.Guid);
            Override o;
            o.Action = Override::Kind::MoveRenderer;
            o.Target = guid;
            o.Before = size_t(index) < order.size() ? order[size_t(index)] : std::string();
            LayerOverrides(h).push_back(std::move(o));
            return Result::Done();
        });
    }

    // ---------------------------------------------------------------------- asset references

    AssetType BindingAssetType(const std::string& kind)
    {
        if (kind == "VectorField")
            return AssetType::VectorField;
        if (kind == "Mesh" || kind == "StaticMesh" || kind == "SkeletalMesh")
            return AssetType::StaticMesh;
        if (kind == "Texture2D")
            return AssetType::Texture;
        if (kind == "Curve" || kind == "Gradient")
            return AssetType::VfxModule;
        return AssetType::None;
    }

    std::string AssetTypeLabel(AssetType type)
    {
        switch (type)
        {
            case AssetType::VectorField:
                return "Vector Field";
            case AssetType::StaticMesh:
                return "Static Mesh";
            case AssetType::Mesh:
                return "Mesh";
            case AssetType::Texture:
                return "Texture";
            case AssetType::Material:
                return "Material";
            case AssetType::VfxModule:
                return "VFX Module";
            case AssetType::VfxEmitter:
                return "VFX Emitter";
            case AssetType::VfxSystem:
                return "VFX System";
            case AssetType::None:
                return "None";
            default:
                return "asset type " + std::to_string(int(type));
        }
    }

    std::optional<std::string> AssetTypeMismatch(AssetType expected, AssetType got)
    {
        if (expected == got)
            return std::nullopt;
        return "Expected a " + AssetTypeLabel(expected) + " asset, not a " + AssetTypeLabel(got);
    }

    Result PickInputAsset(System& s, const Context& ctx, const std::string& handle, const std::string& use,
                          const InputPath& path, AssetID asset, AssetType type)
    {
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        if (path.empty() || !ctx.Modules)
            return Result::Refused("No input");
        Emitter effective;
        try
        {
            effective = Effective(*h, ctx);
        }
        catch (const std::exception& e)
        {
            return Result::Refused(e.what());
        }
        const auto* current = FindUse(effective, use);
        if (!current)
            return Result::Refused("No module " + use);
        // The input's kind: the use's script for the top level, the dynamic input's below it.
        const ModuleInfo* module = ctx.Modules->Find(current->Script);
        std::string       kind;
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (!module)
                return Result::Refused("Missing module script");
            const auto inputs = ModuleInputs(module->Data);
            auto at = std::find_if(inputs.begin(), inputs.end(), [&](const auto& x) { return x.Spec.Name == path[i]; });
            if (at == inputs.end())
                return Result::Refused("No input " + path[i]);
            if (i + 1 == path.size())
            {
                kind = at->Interface;
                break;
            }
            const auto nested = GetInput(*current, InputPath(path.begin(), path.begin() + long(i) + 1));
            if (!nested || nested->Kind != InputKind::DynamicInput)
                return Result::Refused("No input " + path[i + 1]);
            module = ctx.Modules->Find(nested->Script);
        }
        const auto expected = BindingAssetType(kind);
        if (expected == AssetType::None)
            return Result::Refused(path.back() + " takes no asset");
        if (asset.IsValid())
            if (auto mismatch = AssetTypeMismatch(expected, type))
                return Result::Refused(*mismatch);
        const auto existing = GetInput(*current, path);
        Input      input;
        input.Kind = InputKind::DataInterface;
        if (existing && existing->Kind == InputKind::DataInterface)
            input.Interface = existing->Interface;
        else
        {
            const auto inputs = ModuleInputs(module->Data);
            auto at = std::find_if(inputs.begin(), inputs.end(), [&](const auto& x) { return x.Spec.Name == path.back(); });
            input.Interface      = at->DefaultBinding;
            input.Interface.Kind = kind == "Mesh" ? "StaticMesh" : kind;
        }
        input.Interface.Asset = asset;
        if (kind == "Mesh")
            input.Interface.Source = "Asset";
        return SetInput(s, ctx, handle, use, path, std::move(input));
    }

    // ---------------------------------------------------------------------- emitter properties

    EmitterSettings SettingsOf(const Emitter& e)
    {
        return {e.Target, e.SimulationSpace, e.Capacity, e.Deterministic, e.FixedBounds, e.CullDistance, e.DistancePolicy};
    }

    namespace
    {
        bool SameBounds(const std::optional<Bounds>& a, const std::optional<Bounds>& b)
        {
            return a.has_value() == b.has_value() && (!a || (a->Min == b->Min && a->Max == b->Max));
        }
    } // namespace

    Result SetEmitterSettings(System& s, const Context& ctx, const std::string& handle, const EmitterSettings& v)
    {
        if (v.Capacity == 0 || v.Capacity > 16777216)
            return Result::Refused("Capacity must be 1-16777216");
        if (!std::isfinite(v.CullDistance) || v.CullDistance < 0)
            return Result::Refused("Cull distance must be 0 or more");
        if (v.FixedBounds && !v.FixedBounds->IsValid())
            return Result::Refused("Fixed bounds need Min <= Max");
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            const auto layer = LayerOf(h);
            if (layer == Layer::Reference)
                return Result::Refused("Settings of a referenced emitter are edited in its own asset");
            auto&      e    = *h.Inline;
            const auto base = SettingsOf(layer == Layer::Own ? e : Base(h, ctx));
            const auto set  = [&](const char* name, bool differs) {
                if (layer == Layer::InlineChild)
                {
                    if (differs)
                        e.SettingOverrides.insert(name);
                    else
                        e.SettingOverrides.erase(name);
                }
            };
            set("Target", v.Target != base.Target);
            set("Space", v.SimulationSpace != base.SimulationSpace);
            set("Capacity", v.Capacity != base.Capacity);
            set("Deterministic", v.Deterministic != base.Deterministic);
            set("FixedBounds", !SameBounds(v.FixedBounds, base.FixedBounds));
            set("CullDistance", v.CullDistance != base.CullDistance);
            set("DistancePolicy", v.DistancePolicy != base.DistancePolicy);
            e.Target          = v.Target;
            e.SimulationSpace = v.SimulationSpace;
            e.Capacity        = v.Capacity;
            e.Deterministic   = v.Deterministic;
            e.FixedBounds     = v.FixedBounds;
            e.CullDistance    = v.CullDistance;
            e.DistancePolicy  = v.DistancePolicy;
            return Result::Done();
        });
    }

    std::set<std::string> OverriddenSettings(const EmitterHandle& h)
    {
        return LayerOf(h) == Layer::InlineChild ? h.Inline->SettingOverrides : std::set<std::string> {};
    }

    Result ResetEmitterSetting(System& s, const Context& ctx, const std::string& handle, const std::string& setting)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) != Layer::InlineChild)
                return Result::Refused("Only child emitters override settings");
            h.Inline->SettingOverrides.erase(setting);
            const auto base = Base(h, ctx);
            auto&      e    = *h.Inline;
            if (setting == "Target")
                e.Target = base.Target;
            else if (setting == "Space")
                e.SimulationSpace = base.SimulationSpace;
            else if (setting == "Capacity")
                e.Capacity = base.Capacity;
            else if (setting == "Deterministic")
                e.Deterministic = base.Deterministic;
            else if (setting == "FixedBounds")
                e.FixedBounds = base.FixedBounds;
            else if (setting == "CullDistance")
                e.CullDistance = base.CullDistance;
            else if (setting == "DistancePolicy")
                e.DistancePolicy = base.DistancePolicy;
            return Result::Done();
        });
    }

    // ---------------------------------------------------------------------- emitters

    Emitter EmptyEmitter()
    {
        Emitter e;
        e.Name     = "Emitter";
        e.Target   = Vfx::SimTarget::CPU;
        const auto use = [](const char* name) {
            ModuleUse u;
            u.Guid   = VfxModuleEditing::NewGuid();
            u.Script = Vfx::BuiltinModuleID(name);
            return u;
        };
        try
        {
            e.Stages[Stage::EmitterUpdate]  = {use("EmitterState")};
            e.Stages[Stage::ParticleSpawn]  = {use("InitializeParticle")};
            e.Stages[Stage::ParticleUpdate] = {use("ParticleState"), use("SolveForcesAndVelocity")};
        }
        catch (const std::exception&)
        {
            e.Stages.clear(); // Without the built-ins the emitter starts empty.
        }
        Vfx::Renderer sprite;
        sprite.Guid = VfxModuleEditing::NewGuid();
        e.Renderers.push_back(sprite);
        return e;
    }

    std::string UniqueEmitterName(const System& s, const std::string& base)
    {
        const std::string stem = base.empty() ? "Emitter" : base;
        std::string       name = stem;
        for (int n = 2; std::any_of(s.Emitters.begin(), s.Emitters.end(), [&](const auto& h) { return h.Name == name; }); ++n)
            name = stem + std::to_string(n);
        return name;
    }

    std::string AddEmitter(System& s, const std::string& name, std::optional<Emitter> source)
    {
        EmitterHandle h;
        h.Guid         = VfxModuleEditing::NewGuid();
        h.Name         = UniqueEmitterName(s, name.empty() && source ? source->Name : name);
        h.Inline       = source ? std::move(*source) : EmptyEmitter();
        h.Inline->Name = h.Name;
        s.Emitters.push_back(std::move(h));
        return s.Emitters.back().Guid;
    }

    std::string AddInheritedEmitter(System& s, const std::string& name, AssetID parent)
    {
        EmitterHandle h;
        h.Guid           = VfxModuleEditing::NewGuid();
        h.Name           = UniqueEmitterName(s, name);
        h.Inline         = Emitter {};
        h.Inline->Name   = h.Name;
        h.Inline->Parent = parent;
        s.Emitters.push_back(std::move(h));
        return s.Emitters.back().Guid;
    }

    Result DuplicateEmitter(System& s, const std::string& handle)
    {
        const auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        EmitterHandle copy = *h;
        copy.Guid          = VfxModuleEditing::NewGuid();
        copy.Name          = UniqueEmitterName(s, h->Name);
        if (copy.Inline)
            copy.Inline->Name = copy.Name;
        auto at = std::find_if(s.Emitters.begin(), s.Emitters.end(), [&](const auto& x) { return x.Guid == handle; });
        s.Emitters.insert(at + 1, copy);
        return Result::Done(copy.Guid);
    }

    Result DeleteEmitter(System& s, const std::string& handle)
    {
        auto n = s.Emitters.size();
        s.Emitters.erase(std::remove_if(s.Emitters.begin(), s.Emitters.end(), [&](const auto& h) { return h.Guid == handle; }),
                         s.Emitters.end());
        return s.Emitters.size() < n ? Result::Done() : Result::Refused("No emitter " + handle);
    }

    Result RenameEmitter(System& s, const std::string& handle, const std::string& name)
    {
        auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        if (name.empty())
            return Result::Refused("An emitter needs a name");
        for (const auto& other : s.Emitters)
            if (other.Guid != handle && other.Name == name)
                return Result::Refused("Another emitter is called " + name);
        h->Name = name;
        if (h->Inline)
            h->Inline->Name = name;
        return Result::Done(name);
    }

    Result MoveEmitter(System& s, const std::string& handle, int direction)
    {
        auto at = std::find_if(s.Emitters.begin(), s.Emitters.end(), [&](const auto& h) { return h.Guid == handle; });
        if (at == s.Emitters.end())
            return Result::Refused("No emitter " + handle);
        const long index = long(at - s.Emitters.begin()) + direction;
        if (index < 0 || index >= long(s.Emitters.size()))
            return Result::Refused("Already at the end");
        std::swap(*at, s.Emitters[size_t(index)]);
        return Result::Done();
    }

    Result SetEmitterEnabled(System& s, const std::string& handle, bool enabled)
    {
        auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        h->Enabled = enabled;
        return Result::Done();
    }

    // ---------------------------------------------------------------------- parameters

    namespace
    {
        struct Known
        {
            std::map<std::string, ParameterInfo> Map;
            ParameterInfo& At(const std::string& name, Type kind)
            {
                auto at = Map.find(name);
                if (at == Map.end())
                {
                    ParameterInfo p;
                    p.Name      = name;
                    p.Namespace = NamespaceOf(name);
                    p.Kind      = kind;
                    at          = Map.emplace(name, std::move(p)).first;
                }
                return at->second;
            }
        };
        void AddUnique(std::vector<std::string>& v, const std::string& s)
        {
            if (std::find(v.begin(), v.end(), s) == v.end())
                v.push_back(s);
        }
        // Writes and reads of one use: Set and Get nodes of its script, and its linked inputs.
        void Scan(Known& known, const Context& ctx, const ModuleUse& use, bool enabled)
        {
            if (!ctx.Modules || !enabled)
                return;
            const auto* info = ctx.Modules->Find(use.Script);
            if (!info)
                return;
            std::function<void(const Vfx::Module&, const std::map<std::string, Input>&, int)> scan;
            scan = [&](const Vfx::Module& m, const std::map<std::string, Input>& inputs, int depth) {
                if (depth > 16)
                    return;
                for (const auto& n : m.Nodes)
                {
                    if ((n.Op != "Set" && n.Op != "Get") || n.Parameter.find("$Usage") != std::string::npos ||
                        NamespaceOf(n.Parameter).empty())
                        continue;
                    auto& p = known.At(n.Parameter, n.Kind);
                    AddUnique(n.Op == "Set" ? p.Writers : p.Readers, info->Name);
                }
                for (const auto& [name, input] : inputs)
                {
                    if (input.Kind == InputKind::Link && !NamespaceOf(input.Link).empty())
                    {
                        auto spec = std::find_if(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == name; });
                        AddUnique(known.At(input.Link, spec != m.Inputs.end() ? spec->Kind : Type::Float).Readers, info->Name);
                    }
                    if (input.Kind == InputKind::DynamicInput)
                        if (const auto* nested = ctx.Modules->Find(input.Script))
                            scan(nested->Data, input.Inputs, depth + 1);
                }
            };
            scan(info->Data, use.Inputs, 0);
        }
        Known Collect(const System& s, const Context& ctx, const std::string& handle)
        {
            Known known;
            for (const auto& [name, p] : Vfx::BuiltinParameters())
            {
                auto& info    = known.At(name, p.Kind);
                info.Default  = p.Default;
                info.ReadOnly = name.rfind("Engine.", 0) == 0;
            }
            known.At("Transient.SpawnIndex", Type::Int);
            for (const auto& p : s.UserParameters)
            {
                auto& info   = known.At(p.Name, p.Kind);
                info.Kind    = p.Kind;
                info.Default = p.Default;
            }
            for (const auto& [stage, stack] : s.Stages)
                for (const auto& u : stack)
                    Scan(known, ctx, u, u.Enabled);
            if (const auto* h = FindHandle(s, handle))
                try
                {
                    const auto e = Effective(*h, ctx);
                    for (const auto& p : e.Parameters)
                    {
                        auto& info   = known.At(p.Name, p.Kind);
                        info.Kind    = p.Kind;
                        info.Default = p.Default;
                    }
                    for (const auto& p : e.Attributes)
                    {
                        auto& info   = known.At(p.Name, p.Kind);
                        info.Kind    = p.Kind;
                        info.Default = p.Default;
                    }
                    ForEachUse(e, [&](const StackRef&, const ModuleUse& u) { Scan(known, ctx, u, u.Enabled); });
                    for (const auto& r : e.Renderers)
                        for (const auto& [binding, parameter] : r.Bindings)
                            if (auto at = known.Map.find(parameter); at != known.Map.end())
                                AddUnique(at->second.Readers, std::string(r.Kind == Vfx::RendererKind::Sprite ? "Sprite" :
                                                                          r.Kind == Vfx::RendererKind::Mesh   ? "Mesh" :
                                                                          r.Kind == Vfx::RendererKind::Ribbon ? "Ribbon" :
                                                                                                                "Light") +
                                                                          " renderer");
                    for (const auto& stage : e.SimulationStages)
                        if (!stage.Enabled.empty())
                            if (auto at = known.Map.find(stage.Enabled); at != known.Map.end())
                                AddUnique(at->second.Readers, stage.Name + " (stage)");
                }
                catch (const std::exception&)
                {}
            return known;
        }
    } // namespace

    std::vector<ParameterInfo> ListParameters(const System& s, const Context& ctx, const std::string& handle)
    {
        auto                       known = Collect(s, ctx, handle);
        std::vector<ParameterInfo> r;
        for (auto& [name, p] : known.Map)
        {
            if (p.Namespace.empty())
                continue;
            // Renderers and stage conditions read their bindings' defaults on purpose; modules do not.
            const bool moduleReads = std::any_of(p.Readers.begin(), p.Readers.end(), [](const std::string& r) {
                return !(r.size() >= 9 && r.compare(r.size() - 9, 9, " renderer") == 0) &&
                       !(r.size() >= 7 && r.compare(r.size() - 7, 7, "(stage)") == 0);
            });
            if (p.Namespace == "Particles" && p.Writers.empty() && moduleReads && name != "Particles.UniqueID" &&
                name != "Particles.Alive")
                p.Warning = "Read but never written: it keeps its default";
            r.push_back(p);
        }
        return r;
    }

    std::optional<Type> ParameterType(const System& s, const Context& ctx, const std::string& handle, const std::string& name)
    {
        auto known = Collect(s, ctx, handle);
        auto at    = known.Map.find(name);
        if (at == known.Map.end())
            return std::nullopt;
        return at->second.Kind;
    }

    Result AddUserParameter(System& s, Type kind, const std::string& name)
    {
        std::string base = name.empty() ? "User.NewParameter" : name;
        if (base.rfind("User.", 0) != 0)
            base = "User." + base;
        std::string unique = base;
        for (int n = 2; std::any_of(s.UserParameters.begin(), s.UserParameters.end(), [&](const auto& p) { return p.Name == unique; }); ++n)
            unique = base + std::to_string(n);
        s.UserParameters.push_back({unique, kind, Zero(kind), {}, {}});
        return Result::Done(unique);
    }

    Result RemoveUserParameter(System& s, const std::string& name)
    {
        auto& v = s.UserParameters;
        auto  n = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& p) { return p.Name == name; }), v.end());
        return v.size() < n ? Result::Done() : Result::Refused("No user parameter " + name);
    }

    Result RenameUserParameter(System& s, const std::string& from, const std::string& to)
    {
        if (to.rfind("User.", 0) != 0 || to.size() <= 5)
            return Result::Refused("User parameters are named User.<Name>");
        for (char c : to.substr(5))
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return Result::Refused("Use letters, digits and underscores");
        if (std::any_of(s.UserParameters.begin(), s.UserParameters.end(), [&](const auto& p) { return p.Name == to; }))
            return Result::Refused(to + " exists");
        auto at = std::find_if(s.UserParameters.begin(), s.UserParameters.end(), [&](const auto& p) { return p.Name == from; });
        if (at == s.UserParameters.end())
            return Result::Refused("No user parameter " + from);
        at->Name = to;
        const auto rename = [&](Input& input) {
            if (input.Kind == InputKind::Link && input.Link == from)
                input.Link = to;
        };
        const auto renameText = [&](std::string& text) {
            if (text == from)
                text = to;
        };
        for (auto& [stage, stack] : s.Stages)
            for (auto& u : stack)
                ForEachInput(u.Inputs, rename);
        for (auto& h : s.Emitters)
        {
            ForEachInput(h.Overrides, rename);
            if (h.Inline)
            {
                ForEachInput(*h.Inline, rename);
                for (auto& stage : h.Inline->SimulationStages)
                    renameText(stage.Enabled);
                for (auto& r : h.Inline->Renderers)
                    for (auto& [binding, parameter] : r.Bindings)
                        renameText(parameter);
            }
            for (auto* overrides : {&h.Overrides, h.Inline ? &h.Inline->Overrides : nullptr})
                if (overrides)
                    for (auto& o : *overrides)
                    {
                        renameText(o.Simulation.Enabled);
                        for (auto& [binding, parameter] : o.Render.Bindings)
                            renameText(parameter);
                    }
        }
        return Result::Done(to);
    }

    Result SetUserParameterDefault(System& s, const std::string& name, const Vfx::Value& value)
    {
        for (auto& p : s.UserParameters)
            if (p.Name == name)
            {
                if (value.Kind != p.Kind)
                    return Result::Refused("The default must be " + std::string(Vfx::TypeName(p.Kind)));
                p.Default = value;
                return Result::Done();
            }
        return Result::Refused("No user parameter " + name);
    }

    // ---------------------------------------------------------------------- data interfaces

    Result AddDataInterface(System& s, const Context& ctx, const std::string& handle, const std::string& kind)
    {
        auto* h = FindHandle(s, handle);
        if (!h)
            return Result::Refused("No emitter " + handle);
        if (LayerOf(*h) != Layer::Own)
            return Result::Refused("Data interfaces belong to the parent emitter; add them there");
        Vfx::InterfaceBinding b;
        b.Kind = kind;
        if (kind == "Grid3D")
        {
            b.Resolution     = glm::uvec3(32);
            b.WorldBounds    = glm::vec3(4);
            b.GridAttributes = {{"Density", Type::Float}};
        }
        else if (kind == "Curve")
        {
            b.Times  = {0, 1};
            b.Values = {glm::vec4(0), glm::vec4(1)};
        }
        else if (kind == "StaticMesh" || kind == "SkeletalMesh")
            b.Source = "Asset";
        std::string stem = kind == "Grid3D" ? "Grid" : kind == "VectorField" ? "Field" : kind == "Curve" ? "Curve" : "Mesh";
        std::string name = "Emitter." + stem;
        for (int n = 2; h->Inline->DataInterfaces.count(name); ++n)
            name = "Emitter." + stem + std::to_string(n);
        h->Inline->DataInterfaces[name] = b;
        (void)ctx;
        return Result::Done(name);
    }

    Result SetDataInterface(System& s, const Context& ctx, const std::string& handle, const std::string& name,
                            const Vfx::InterfaceBinding& binding)
    {
        return Guarded(s, ctx, handle, [&](EmitterHandle& h) -> Result {
            if (LayerOf(h) != Layer::Own || !h.Inline->DataInterfaces.count(name))
                return Result::Refused("No data interface " + name + " on this emitter");
            if (binding.Kind == "Grid3D" &&
                (glm::any(glm::lessThan(binding.Resolution, glm::uvec3(1))) ||
                 glm::any(glm::greaterThan(binding.Resolution, glm::uvec3(128)))))
                return Result::Refused("Grid resolution must be 1-128 per axis");
            if (binding.Kind == "Grid3D" && glm::any(glm::lessThanEqual(binding.WorldBounds, glm::vec3(0))))
                return Result::Refused("Grid bounds must be positive");
            h.Inline->DataInterfaces[name] = binding;
            return Result::Done();
        });
    }

    Result RemoveDataInterface(System& s, const Context& ctx, const std::string& handle, const std::string& name)
    {
        auto* h = FindHandle(s, handle);
        if (!h || LayerOf(*h) != Layer::Own || !h->Inline->DataInterfaces.erase(name))
            return Result::Refused("No data interface " + name + " on this emitter");
        (void)ctx;
        return Result::Done();
    }

    // ---------------------------------------------------------------------- overview

    std::vector<Dependency> Dependencies(const System& s, const Context& ctx)
    {
        std::vector<Dependency> r;
        for (const auto& h : s.Emitters)
        {
            Emitter e;
            try
            {
                e = Effective(h, ctx);
            }
            catch (const std::exception&)
            {
                continue;
            }
            for (const auto& handler : e.Handlers)
                if (FindHandle(s, handler.SourceEmitter))
                    r.push_back({handler.SourceEmitter, h.Guid, "Event", handler.Channel});
            std::function<void(const std::map<std::string, Input>&)> readers = [&](const std::map<std::string, Input>& inputs) {
                for (const auto& [name, input] : inputs)
                {
                    if (input.Kind == InputKind::DataInterface && input.Interface.Kind == "EmitterParticles" &&
                        FindHandle(s, input.Interface.SourceEmitter) &&
                        std::none_of(r.begin(), r.end(), [&](const Dependency& d) {
                            return d.From == input.Interface.SourceEmitter && d.To == h.Guid && d.Kind == "Reader";
                        }))
                        r.push_back({input.Interface.SourceEmitter, h.Guid, "Reader", "Particles"});
                    if (input.Kind == InputKind::DynamicInput)
                        readers(input.Inputs);
                }
            };
            ForEachUse(e, [&](const StackRef&, const ModuleUse& u) { readers(u.Inputs); });
        }
        return r;
    }

    std::string Summary(const Emitter& e, const ModuleLibrary& library)
    {
        std::string spawn;
        for (const auto& [stage, stack] : e.Stages)
            for (const auto& u : stack)
            {
                if (!u.Enabled)
                    continue;
                const auto* m = library.Find(u.Script);
                if (!m)
                    continue;
                const auto value = [&](const char* input, float fallback) {
                    auto at = u.Inputs.find(input);
                    if (at != u.Inputs.end() && at->second.Kind == InputKind::Literal)
                        return at->second.Literal.Kind == Type::Int ? float(at->second.Literal.Integer) : at->second.Literal.Data.x;
                    if (at != u.Inputs.end())
                        return -1.0f; // Linked or dynamic.
                    for (const auto& p : m->Data.Inputs)
                        if (p.Name == input && p.Default)
                            return p.Default->Kind == Type::Int ? float(p.Default->Integer) : p.Default->Data.x;
                    return fallback;
                };
                const auto number = [](float v) {
                    std::ostringstream out;
                    out << v;
                    return out.str();
                };
                std::string part;
                if (m->Name == "SpawnRate")
                {
                    const float rate = value("Module.Rate", 10);
                    part             = rate < 0 ? "Spawn rate (bound)" : "Spawn " + number(rate) + "/s";
                }
                else if (m->Name == "SpawnBurstInstantaneous")
                {
                    const float count = value("Module.Count", 0);
                    part              = count < 0 ? "Burst (bound)" : "Burst " + number(count);
                }
                else if (m->Name == "SpawnFromOtherEmitter")
                    part = "Spawn from emitter";
                if (!part.empty())
                    spawn += (spawn.empty() ? "" : " + ") + part;
            }
        if (!e.Handlers.empty())
            spawn += (spawn.empty() ? "" : " + ") + std::to_string(e.Handlers.size()) + (e.Handlers.size() == 1 ? " event" : " events");
        if (spawn.empty())
            spawn = "No spawn";
        const size_t renderers = e.Renderers.size();
        return spawn + " \xC2\xB7 " + std::to_string(renderers) + (renderers == 1 ? " renderer" : " renderers");
    }

    std::map<std::string, glm::vec2> OverviewLayout(const System& s)
    {
        std::map<std::string, glm::vec2> r;
        r[SystemNode] = {0, 0};
        float x       = 240;
        for (const auto& h : s.Emitters)
        {
            r[h.Guid] = {x, 0};
            x += 300;
        }
        return r;
    }

    NodeCanvas::Graph BuildOverview(const System& s, const Context& ctx, const std::map<std::string, glm::vec2>& positions,
                                    const std::vector<StackDiagnostic>& diagnostics)
    {
        NodeCanvas::Graph g;
        const auto        layout = OverviewLayout(s);
        const auto        place  = [&](const std::string& id) {
            auto at = positions.find(id);
            return at != positions.end() ? at->second : layout.at(id);
        };
        NodeCanvas::Node system;
        system.Id       = SystemNode;
        system.Title    = "System";
        system.Subtitle = std::to_string(s.Emitters.size()) + (s.Emitters.size() == 1 ? " emitter" : " emitters");
        system.Color    = 0xff6a4a3a;
        system.Position = place(SystemNode);
        {
            std::ostringstream warmup;
            warmup << "Warmup " << s.WarmupTime << " s \xC2\xB7 Seed " << s.Seed;
            system.Body = {warmup.str(), std::to_string(s.UserParameters.size()) + " user parameters"};
        }
        for (const auto& d : diagnostics)
            if (d.Emitter.empty() && !d.Warning)
            {
                system.Mark    = NodeCanvas::Badge::Error;
                system.Message = d.Message;
            }
        g.Nodes.push_back(std::move(system));
        const auto deps = Dependencies(s, ctx);
        for (const auto& h : s.Emitters)
        {
            NodeCanvas::Node n;
            n.Id       = h.Guid;
            n.Title    = (h.Enabled ? "" : "(off) ") + h.Name;
            n.Position = place(h.Guid);
            Emitter e;
            bool    resolved = true;
            try
            {
                e = Effective(h, ctx);
            }
            catch (const std::exception& ex)
            {
                resolved  = false;
                n.Mark    = NodeCanvas::Badge::Error;
                n.Message = ex.what();
            }
            const bool gpu = e.Target == Vfx::SimTarget::GPU;
            n.Color        = !h.Enabled ? 0xff4a4a4a : gpu ? 0xff7a5a2e : 0xff3e6a3a; // GPU blue, CPU green (ABGR).
            n.Subtitle     = std::string(gpu ? "GPU" : "CPU") + " \xC2\xB7 " + (e.SimulationSpace == Vfx::Space::World ? "World" : "Local") +
                         (LayerOf(h) == Layer::Own ? "" : LayerOf(h) == Layer::InlineChild ? " \xC2\xB7 Inherited" : " \xC2\xB7 Referenced");
            if (resolved && ctx.Modules)
                n.Body = {Summary(e, *ctx.Modules)};
            size_t errors = 0, warnings = 0;
            for (const auto& d : diagnostics)
                if (d.Emitter == h.Guid)
                {
                    (d.Warning ? warnings : errors)++;
                    if (n.Message.empty() || (!d.Warning && n.Mark != NodeCanvas::Badge::Error))
                        n.Message = d.Message;
                    n.Mark = !d.Warning ? NodeCanvas::Badge::Error : n.Mark == NodeCanvas::Badge::Error ? n.Mark : NodeCanvas::Badge::Warning;
                }
            n.Body.push_back(errors ? std::to_string(errors) + (errors == 1 ? " error" : " errors") :
                             warnings ? std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings") :
                                        std::string("Compiles"));
            n.Accent = errors ? 0xff4d56e8 : warnings ? 0xff3cb4e6 : 0xff5ec27a;
            NodeCanvas::Pin in, out;
            in.Name = out.Name = "Events";
            in.Tooltip  = "Event handlers and particle readers fed by other emitters";
            out.Tooltip = "Drag onto another emitter to add an event handler there";
            in.Color = out.Color = 0xffd0a040;
            in.Connected  = std::any_of(deps.begin(), deps.end(), [&](const Dependency& d) { return d.To == h.Guid; });
            out.Connected = std::any_of(deps.begin(), deps.end(), [&](const Dependency& d) { return d.From == h.Guid; });
            n.Inputs      = {in};
            n.Outputs     = {out};
            g.Nodes.push_back(std::move(n));
        }
        for (const auto& d : deps)
            g.Links.push_back({d.From, "Events", d.To, "Events", d.Kind == "Event" ? 0xffd0a040u : 0xffc0c0c0u});
        return g;
    }

    // ---------------------------------------------------------------------- diagnostics

    std::vector<Vfx::Diagnostic> CompileSystem(const System& s, const Context& ctx, AssetID asset)
    {
        try
        {
            return Vfx::Compile(s, ctx.ResolveEmitter(), ctx.ResolveModule(), asset).Errors;
        }
        catch (const std::exception& e)
        {
            Vfx::Diagnostic d;
            d.Message = e.what();
            return {d};
        }
    }

    StackDiagnostic Locate(const Vfx::Diagnostic& d, const System& s, const Context& ctx)
    {
        StackDiagnostic r;
        r.Emitter = d.Emitter;
        r.Message = d.Message;
        r.Use     = d.Use;
        if (d.StageKind)
            r.Stack = StackRef {*d.StageKind, d.Container};
        if (d.StageKind == Stage::EventHandler)
            r.Handler = d.Container;
        if (d.StageKind == Stage::SimulationStage)
            r.SimulationStage = d.Container;
        const auto* h = FindHandle(s, d.Emitter);
        Emitter     e;
        if (h)
            try
            {
                e = Effective(*h, ctx);
            }
            catch (const std::exception&)
            {
                h = nullptr;
            }
        if (h && !d.Node.empty())
        {
            for (const auto& renderer : e.Renderers)
                if (renderer.Guid == d.Node)
                    r.Renderer = renderer.Guid;
            for (const auto& handler : e.Handlers)
                if (handler.Guid == d.Node)
                    r.Handler = handler.Guid, r.Stack = StackRef {Stage::EventHandler, handler.Guid};
            for (const auto& stage : e.SimulationStages)
                if (stage.Guid == d.Node)
                    r.SimulationStage = stage.Guid, r.Stack = StackRef {Stage::SimulationStage, stage.Guid};
        }
        if (!r.Use.empty() && ctx.Modules)
        {
            // The scope says which input of the use a dynamic input belongs to.
            std::vector<std::string> scope;
            std::stringstream        in(d.Scope);
            for (std::string part; std::getline(in, part, '/');)
                scope.push_back(part);
            const ModuleUse* use = h ? FindUse(e, r.Use) : nullptr;
            if (!use)
                for (const auto& [stage, stack] : s.Stages)
                    for (const auto& u : stack)
                        if (u.Guid == r.Use)
                            use = &u;
            if (use)
            {
                const auto* info     = ctx.Modules->Find(use->Script);
                const auto  hasInput = [](const Vfx::Module& m, const std::string& name) {
                    return std::any_of(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == name; });
                };
                if (scope.size() >= 2 && info && hasInput(info->Data, scope[1]))
                {
                    r.Input  = scope[1];
                    r.Nested = d.Pin;
                }
                else if (scope.size() <= 1 && d.Node.empty() && info && hasInput(info->Data, d.Pin))
                    r.Input = d.Pin;
            }
        }
        return r;
    }

    // ---------------------------------------------------------------------- files

    bool IsBuiltin(const fs::path& file) { return VfxModuleEditing::IsBuiltin(file); }

    std::vector<Template> Templates()
    {
        std::vector<Template> r;
        try
        {
            for (const auto& f : fs::directory_iterator(Vfx::BuiltinModuleRoot()))
                if (f.path().extension() == ".vfxemitter")
                    r.push_back({f.path().stem().u8string(), f.path()});
        }
        catch (const std::exception&)
        {}
        std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
        return r;
    }

    std::variant<fs::path, std::string> CreateSystemFile(const fs::path& directory, const std::string& name,
                                                         const std::vector<fs::path>& templates)
    {
        System s;
        try
        {
            for (const auto& t : templates)
            {
                auto e = Vfx::DecodeEmitter(YAML::LoadFile(t.string()));
                if (e.Parent.IsValid())
                    return "The template " + t.stem().u8string() + " has a parent";
                AddEmitter(s, t.stem().u8string(), std::move(e));
            }
        }
        catch (const std::exception& e)
        {
            return std::string(e.what());
        }
        std::error_code error;
        fs::create_directories(directory, error);
        const auto file = UniqueFile(directory, Identifier(name), ".vfxsystem");
        if (auto failure = WriteText(file, Text(Vfx::EncodeSystem(s))))
            return *failure;
        return file;
    }

    std::variant<fs::path, std::string> CreateEmitterFile(const fs::path& directory, const std::string& name, AssetID parent)
    {
        Emitter e;
        if (parent.IsValid())
            e.Parent = parent;
        else
            e = EmptyEmitter();
        e.Name = Identifier(name);
        std::error_code error;
        fs::create_directories(directory, error);
        const auto file = UniqueFile(directory, e.Name, ".vfxemitter");
        if (auto failure = WriteText(file, Text(Vfx::EncodeEmitter(e))))
            return *failure;
        return file;
    }

    std::variant<fs::path, std::string> Duplicate(const SystemDocument& document, const fs::path& directory)
    {
        std::error_code error;
        fs::create_directories(directory, error);
        const auto& source = document.File();
        const auto  file   = UniqueFile(directory, source.stem().u8string(), source.extension().u8string());
        if (auto failure = WriteText(file, document.Canonical()))
            return *failure;
        return file;
    }
} // namespace Kans::VfxSystemEditing
