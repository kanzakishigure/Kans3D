#include "VfxModuleEditing.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <imgui.h>

namespace Kans::VfxModuleEditing
{
    namespace fs = std::filesystem;
    using NodeCanvas::PinRef;
    using NodeCanvas::Side;

    namespace
    {
        std::string Dump(const Module& m)
        {
            YAML::Emitter out;
            out << Vfx::EncodeModule(m);
            return std::string(out.c_str()) + "\n";
        }
        Vfx::Value Zero(Type t)
        {
            return t == Type::Int ? Vfx::Value::Int(0) : t == Type::Bool ? Vfx::Value::Bool(false) : Vfx::Value::Vector({}, t);
        }
        bool IsGridOp(const std::string& op) { return op.rfind("Grid", 0) == 0; }
        bool IsEventOp(const std::string& op)
        {
            return op == "WriteEvent" || op == "ParticleCount" || op == "ReadAttribute" || op == "ReadAttributeByID";
        }
        // The data interface a node bound by name reads, or empty.
        std::string InterfaceKind(const std::string& op)
        {
            if (IsGridOp(op))
                return "Grid";
            if (Vfx::IsVectorFieldOperation(op))
                return "VectorField";
            if (Vfx::IsMeshOperation(op))
                return "Mesh";
            if (op == "Sample")
                return "Curve";
            if (IsEventOp(op))
                return "Events";
            if (op == "SampleForceFields")
                return "ForceFields";
            return {};
        }
        std::string Lower(std::string s)
        {
            for (auto& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        bool Writable(const std::string& name)
        {
            for (const char* prefix : {"Particles.", "Emitter.", "System.", "Transient."})
                if (name.rfind(prefix, 0) == 0)
                    return true;
            return false;
        }
        std::string Identifier(const std::string& text)
        {
            std::string r;
            for (char c : text)
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    r += c;
            if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0])))
                r = "Module" + r;
            return r;
        }
        fs::path UniqueFile(const fs::path& directory, const std::string& stem)
        {
            fs::path file = directory / (stem + ".vfxmodule");
            for (int n = 1; fs::exists(file); ++n)
                file = directory / (stem + std::to_string(n) + ".vfxmodule");
            return file;
        }
        std::optional<std::string> Write(const fs::path& file, const Module& m)
        {
            std::ofstream out(file, std::ios::binary);
            if (!out)
                return "Cannot write " + file.u8string();
            out << Dump(m);
            return out ? std::nullopt : std::optional<std::string>("Cannot write " + file.u8string());
        }
        const std::vector<Vfx::Pin>& Pins(const Vfx::Node& n, Side side) { return side == Side::Input ? n.Inputs : n.Outputs; }
    } // namespace

    // ---------------------------------------------------------------------- document

    std::optional<std::string> ModuleDocument::Open(const fs::path& file)
    {
        try
        {
            auto module = Vfx::DecodeModule(YAML::LoadFile(file.string()));
            Reset(std::move(module), file, IsBuiltin(file));
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            return file.filename().u8string() + ": " + e.what();
        }
    }

    void ModuleDocument::Reset(Module module, fs::path file, bool readOnly)
    {
        m_Module   = std::move(module);
        m_File     = std::move(file);
        m_ReadOnly = readOnly;
        // Modules without layout metadata get one; it is saved with the next edit.
        AutoLayout(m_Module);
        m_Committed = m_Saved = Canonical();
        m_Undo.clear();
        m_Redo.clear();
        ++m_Revision;
    }

    std::string ModuleDocument::Canonical() const { return Dump(m_Module); }

    bool ModuleDocument::Commit()
    {
        auto text = Canonical();
        if (text == m_Committed)
            return false;
        m_Undo.push_back(std::move(m_Committed));
        if (m_Undo.size() > 200)
            m_Undo.erase(m_Undo.begin());
        m_Redo.clear();
        m_Committed = std::move(text);
        ++m_Revision;
        return true;
    }

    bool ModuleDocument::Undo()
    {
        Commit(); // A pending change becomes a step first, so undo never loses it.
        if (m_Undo.empty())
            return false;
        m_Redo.push_back(std::move(m_Committed));
        m_Committed = std::move(m_Undo.back());
        m_Undo.pop_back();
        m_Module = Vfx::DecodeModule(YAML::Load(m_Committed));
        ++m_Revision;
        return true;
    }

    bool ModuleDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_Undo.push_back(std::move(m_Committed));
        m_Committed = std::move(m_Redo.back());
        m_Redo.pop_back();
        m_Module = Vfx::DecodeModule(YAML::Load(m_Committed));
        ++m_Revision;
        return true;
    }

    bool ModuleDocument::IsDirty() const { return m_Committed != m_Saved; }

    std::optional<std::string> ModuleDocument::Save()
    {
        if (m_ReadOnly)
            return "Built-in modules are read-only; duplicate the module to edit it.";
        if (m_File.empty())
            return "The module has no file.";
        Commit();
        if (auto error = Write(m_File, m_Module))
            return error;
        m_Saved = m_Committed;
        return std::nullopt;
    }

    // ---------------------------------------------------------------------- graph

    std::string NewGuid()
    {
        static std::mt19937_64 random {std::random_device {}() ^ uint64_t(std::hash<const void*> {}(&random))};
        const uint64_t         a = random(), b = random();
        char                   text[40];
        std::snprintf(text, sizeof(text), "%08x-%04x-4%03x-%04x-%012llx", uint32_t(a >> 32), uint32_t(a >> 16) & 0xffff,
                      uint32_t(a) & 0xfff, uint32_t(b >> 48) & 0x3fff | 0x8000,
                      (unsigned long long)(b & 0xffffffffffffull));
        return text;
    }

    Vfx::Node* FindNode(Module& m, const std::string& guid)
    {
        for (auto& n : m.Nodes)
            if (n.Guid == guid)
                return &n;
        return nullptr;
    }
    const Vfx::Node* FindNode(const Module& m, const std::string& guid)
    {
        for (const auto& n : m.Nodes)
            if (n.Guid == guid)
                return &n;
        return nullptr;
    }

    std::optional<Type> PinType(const Module& m, const PinRef& pin)
    {
        if (const auto* n = FindNode(m, pin.Node))
            for (const auto& p : Pins(*n, pin.PinSide))
                if (p.Name == pin.Pin)
                    return p.Kind;
        return std::nullopt;
    }

    bool CanLink(const Module& m, const PinRef& output, const PinRef& input, std::string* reason)
    {
        const auto fail = [&](std::string why) {
            if (reason)
                *reason = std::move(why);
            return false;
        };
        if (output.PinSide != Side::Output || input.PinSide != Side::Input)
            return fail("Links go from an output to an input");
        const auto from = PinType(m, output), to = PinType(m, input);
        if (!from || !to)
            return fail("Missing pin");
        if (output.Node == input.Node)
            return fail("A node cannot link to itself");
        if (!Vfx::CanConvert(*from, *to))
            return fail(std::string("Type mismatch: ") + Vfx::TypeName(*from) + " -> " + Vfx::TypeName(*to));
        // A cycle: the output's node already depends on the input's node.
        std::set<std::string>    seen;
        std::vector<std::string> pending {output.Node};
        while (!pending.empty())
        {
            auto node = pending.back();
            pending.pop_back();
            if (node == input.Node)
                return fail("Link would create a cycle");
            if (!seen.insert(node).second)
                continue;
            for (const auto& l : m.Links)
                if (l.To == node)
                    pending.push_back(l.From);
        }
        return true;
    }

    bool Link(Module& m, const PinRef& output, const PinRef& input)
    {
        if (!CanLink(m, output, input, nullptr))
            return false;
        m.Links.erase(std::remove_if(m.Links.begin(), m.Links.end(),
                                     [&](const Vfx::Link& l) { return l.To == input.Node && l.Input == input.Pin; }),
                      m.Links.end());
        m.Links.push_back({output.Node, output.Pin, input.Node, input.Pin});
        return true;
    }

    void Unlink(Module& m, const PinRef& pin)
    {
        m.Links.erase(std::remove_if(m.Links.begin(), m.Links.end(),
                                     [&](const Vfx::Link& l) {
                                         return pin.PinSide == Side::Input ? (l.To == pin.Node && l.Input == pin.Pin) :
                                                                             (l.From == pin.Node && l.Output == pin.Pin);
                                     }),
                      m.Links.end());
    }

    std::string AddNode(Module& m, const std::string& op, Type kind, glm::vec2 position, const std::string& parameter)
    {
        Vfx::Node n;
        n.Guid      = NewGuid();
        n.Op        = op;
        n.Kind      = kind;
        n.Parameter = parameter;
        if (const auto* info = Vfx::FindOperation(op))
            Vfx::OperationPins(*info, kind, n.Inputs, n.Outputs);
        else
            n.Outputs = {{"Value", kind, {}}};
        if (op == "Constant")
            n.Constant = Zero(kind);
        if (op == "CustomGLSL")
            n.Code = "$Arg0";
        m.Nodes.push_back(n);
        m.Editor.Positions[n.Guid] = position;
        return n.Guid;
    }

    void DeleteNodes(Module& m, const std::set<std::string>& nodes)
    {
        m.Nodes.erase(std::remove_if(m.Nodes.begin(), m.Nodes.end(), [&](const Vfx::Node& n) { return nodes.count(n.Guid) > 0; }),
                      m.Nodes.end());
        m.Links.erase(std::remove_if(m.Links.begin(), m.Links.end(),
                                     [&](const Vfx::Link& l) { return nodes.count(l.From) || nodes.count(l.To); }),
                      m.Links.end());
        for (const auto& id : nodes)
        {
            m.Editor.Positions.erase(id);
            m.Editor.Collapsed.erase(id);
        }
        if (nodes.count(m.ResultNode))
            m.ResultNode.clear();
    }

    std::string InsertReroute(Module& m, size_t index, glm::vec2 position)
    {
        if (index >= m.Links.size())
            return {};
        const auto link = m.Links[index];
        const auto type = PinType(m, {link.From, link.Output, Side::Output}).value_or(Type::Float);
        auto       id   = AddNode(m, "Reroute", type, position);
        m.Links.erase(m.Links.begin() + std::ptrdiff_t(index));
        m.Links.push_back({link.From, link.Output, id, "Value"});
        m.Links.push_back({id, "Value", link.To, link.Input});
        return id;
    }

    void SetNodeKind(Module& m, const std::string& guid, Type kind)
    {
        auto* n = FindNode(m, guid);
        if (!n || n->Kind == kind)
            return;
        const auto* info = Vfx::FindOperation(n->Op);
        if (info && n->Op == "Make")
        {
            std::vector<Vfx::Pin> inputs, outputs;
            Vfx::OperationPins(*info, kind, inputs, outputs);
            n->Inputs = inputs;
            for (size_t k = 0; k < n->Outputs.size(); ++k)
                n->Outputs[k].Kind = kind;
            std::set<std::string> names;
            for (const auto& p : inputs)
                names.insert(p.Name);
            m.Links.erase(std::remove_if(m.Links.begin(), m.Links.end(),
                                         [&](const Vfx::Link& l) { return l.To == guid && !names.count(l.Input); }),
                          m.Links.end());
        }
        else
        {
            const auto follows = [&](const std::vector<Vfx::PinSpec>* specs, size_t k, const Vfx::Pin& p) {
                if (specs && k < specs->size())
                    return (*specs)[k].Rule == Vfx::PinRule::Node;
                return p.Kind == n->Kind; // Pins outside the catalog follow when they matched the old type.
            };
            for (size_t k = 0; k < n->Inputs.size(); ++k)
                if (follows(info ? &info->Inputs : nullptr, k, n->Inputs[k]))
                {
                    n->Inputs[k].Kind = kind;
                    if (n->Inputs[k].Default)
                        n->Inputs[k].Default = Zero(kind);
                }
            for (size_t k = 0; k < n->Outputs.size(); ++k)
                if (follows(info ? &info->Outputs : nullptr, k, n->Outputs[k]))
                    n->Outputs[k].Kind = kind;
        }
        if (n->Op == "Constant")
            n->Constant = Zero(kind);
        n->Kind = kind;
    }

    void Translate(Module& m, const std::vector<std::string>& nodes, const std::vector<std::string>& comments, glm::vec2 delta)
    {
        for (const auto& id : nodes)
            m.Editor.Positions[id] += delta;
        for (auto& c : m.Editor.Comments)
            if (std::find(comments.begin(), comments.end(), c.Guid) != comments.end())
                c.Position += delta;
    }

    // ---------------------------------------------------------------------- clipboard

    std::string Copy(const Module& m, const std::set<std::string>& nodes)
    {
        Module clip;
        for (const auto& n : m.Nodes)
            if (nodes.count(n.Guid))
            {
                clip.Nodes.push_back(n);
                auto at = m.Editor.Positions.find(n.Guid);
                clip.Editor.Positions[n.Guid] = at == m.Editor.Positions.end() ? glm::vec2(0) : at->second;
                if (m.Editor.Collapsed.count(n.Guid))
                    clip.Editor.Collapsed.insert(n.Guid);
            }
        for (const auto& l : m.Links)
            if (nodes.count(l.From) && nodes.count(l.To))
                clip.Links.push_back(l);
        return Dump(clip);
    }

    std::vector<std::string> Paste(Module& m, const std::string& text, glm::vec2 offset)
    {
        Module clip;
        try
        {
            clip = Vfx::DecodeModule(YAML::Load(text));
        }
        catch (const std::exception&)
        {
            return {};
        }
        std::map<std::string, std::string> fresh;
        std::vector<std::string>           created;
        for (auto n : clip.Nodes)
        {
            const auto old = n.Guid;
            n.Guid         = NewGuid();
            fresh[old]     = n.Guid;
            created.push_back(n.Guid);
            auto at                    = clip.Editor.Positions.find(old);
            m.Editor.Positions[n.Guid] = (at == clip.Editor.Positions.end() ? glm::vec2(0) : at->second) + offset;
            if (clip.Editor.Collapsed.count(old))
                m.Editor.Collapsed.insert(n.Guid);
            m.Nodes.push_back(std::move(n));
        }
        for (const auto& l : clip.Links)
            if (fresh.count(l.From) && fresh.count(l.To))
                m.Links.push_back({fresh[l.From], l.Output, fresh[l.To], l.Input});
        return created;
    }

    std::string AddComment(Module& m, const std::string& title, glm::vec2 position, glm::vec2 size)
    {
        Vfx::ModuleComment c;
        c.Guid     = NewGuid();
        c.Title    = title;
        c.Position = position;
        c.Size     = size;
        m.Editor.Comments.push_back(c);
        return c.Guid;
    }

    // ---------------------------------------------------------------------- search

    int FuzzyScore(const std::string& text, const std::string& query)
    {
        if (query.empty())
            return 0;
        const auto t = Lower(text), q = Lower(query);
        if (t.rfind(q, 0) == 0)
            return 1000 - int(t.size());
        if (auto at = t.find(q); at != std::string::npos)
            return 800 - int(at) - int(t.size());
        int    score = 500, last = -1;
        size_t k     = 0;
        for (size_t i = 0; i < t.size() && k < q.size(); ++i)
            if (t[i] == q[k])
            {
                if (last >= 0)
                    score -= int(i) - last - 1;
                if (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])) || std::isupper(static_cast<unsigned char>(text[i])))
                    score += 5; // Word starts.
                last = int(i);
                ++k;
            }
        return k == q.size() ? std::max(score, 1) : -1;
    }

    namespace
    {
        const std::vector<std::string>& CategoryOrder()
        {
            static const std::vector<std::string> order {"Math", "Vector", "Noise", "Grid", "Sampling", "Events", "Custom", "Parameters"};
            return order;
        }
        // The node type and pin of `op` that link to the dragged pin, if any.
        bool Match(const Vfx::OperationInfo& op, const SearchFilter& f, Type& kind, std::string& pin)
        {
            std::vector<Type> kinds = op.Kinds;
            // Prefer the dragged type when the op offers it.
            std::stable_partition(kinds.begin(), kinds.end(), [&](Type t) { return t == f.PinKind; });
            for (Type k : kinds)
            {
                std::vector<Vfx::Pin> inputs, outputs;
                Vfx::OperationPins(op, k, inputs, outputs);
                for (const auto& p : f.PinSide == Side::Output ? inputs : outputs)
                    if (f.PinSide == Side::Output ? Vfx::CanConvert(f.PinKind, p.Kind) : Vfx::CanConvert(p.Kind, f.PinKind))
                    {
                        kind = k;
                        pin  = p.Name;
                        return true;
                    }
            }
            return false;
        }
    } // namespace

    std::vector<ParameterEntry> Parameters(const Module& m)
    {
        std::vector<ParameterEntry> r;
        std::set<std::string>       seen;
        for (const auto& p : m.Inputs)
        {
            ParameterEntry e;
            e.Name        = p.Name;
            e.Kind        = p.Kind;
            e.ModuleInput = true;
            for (const auto& n : m.Nodes)
                if (n.Parameter == p.Name && n.Op != "Input")
                    if (auto kind = InterfaceKind(n.Op); !kind.empty())
                        e.Interface = kind;
            e.Group = e.Interface.empty() ? "Module" : e.Interface;
            r.push_back(e);
            seen.insert(p.Name);
        }
        const auto add = [&](const std::string& name, Type kind) {
            if (!seen.insert(name).second)
                return;
            ParameterEntry e;
            e.Name     = name;
            e.Kind     = kind;
            e.Group    = name.substr(0, name.find('.'));
            e.Writable = Writable(name);
            r.push_back(e);
        };
        for (const auto& [name, p] : Vfx::BuiltinParameters())
            add(name, p.Kind);
        for (const auto& n : m.Nodes)
            if ((n.Op == "Set" || n.Op == "Get") && !n.Parameter.empty())
                add(n.Parameter, n.Kind);
        static const std::vector<std::string> groups {"Particles", "Emitter", "System", "Engine", "Transient", "Module"};
        const auto rank = [&](const std::string& g) {
            auto at = std::find(groups.begin(), groups.end(), g);
            return at == groups.end() ? int(groups.size()) : int(at - groups.begin());
        };
        std::stable_sort(r.begin(), r.end(), [&](const auto& a, const auto& b) {
            return rank(a.Group) != rank(b.Group) ? rank(a.Group) < rank(b.Group) : a.Group != b.Group ? a.Group < b.Group : a.Name < b.Name;
        });
        return r;
    }

    std::vector<SearchEntry> Search(const Module& m, const std::string& query, const std::optional<SearchFilter>& filter)
    {
        std::vector<SearchEntry> r;
        const auto               accept = [&](SearchEntry e) {
            const int a = FuzzyScore(e.Label, query), b = FuzzyScore(e.Category + " " + e.Label, query);
            e.Score     = std::max(a, b - 50);
            if (!query.empty() && e.Score < 0)
                return;
            r.push_back(std::move(e));
        };
        for (const auto& op : Vfx::Operations())
        {
            if (op.Hidden || op.Category == "Parameters" || op.Name == "Call")
                continue;
            SearchEntry e;
            e.Name        = op.Name;
            e.Label       = op.Name;
            e.Category    = op.Category;
            e.Description = op.Description;
            e.NodeKind    = op.Kinds.front();
            if (filter && !Match(op, *filter, e.NodeKind, e.LinkPin))
                continue;
            accept(e);
        }
        for (const auto& p : Parameters(m))
        {
            if (!p.Interface.empty())
                continue; // Data interfaces are reached through their ops.
            const auto entry = [&](SearchEntry::Kind kind, const char* verb) {
                SearchEntry e;
                e.What        = kind;
                e.Name        = p.Name;
                e.Label       = std::string(verb) + " " + p.Name;
                e.Category    = "Parameters";
                e.Description = kind == SearchEntry::Kind::ModuleInput ? "Reads this module's input." :
                                kind == SearchEntry::Kind::Get         ? "Reads the parameter." :
                                                                         "Writes the parameter.";
                e.NodeKind    = p.Kind;
                if (filter)
                {
                    const bool linkIn = filter->PinSide == Side::Output; // The new node's input is linked.
                    if (linkIn ? (kind != SearchEntry::Kind::Set || !Vfx::CanConvert(filter->PinKind, p.Kind)) :
                                 (kind == SearchEntry::Kind::Set || !Vfx::CanConvert(p.Kind, filter->PinKind)))
                        return;
                    e.LinkPin = "Value";
                }
                accept(e);
            };
            if (p.ModuleInput)
                entry(SearchEntry::Kind::ModuleInput, "Input");
            else
            {
                entry(SearchEntry::Kind::Get, "Get");
                if (p.Writable)
                    entry(SearchEntry::Kind::Set, "Set");
            }
        }
        const auto& order = CategoryOrder();
        const auto  rank  = [&](const std::string& c) { return int(std::find(order.begin(), order.end(), c) - order.begin()); };
        std::stable_sort(r.begin(), r.end(), [&](const SearchEntry& a, const SearchEntry& b) {
            if (!query.empty() && a.Score != b.Score)
                return a.Score > b.Score;
            return rank(a.Category) < rank(b.Category);
        });
        return r;
    }

    std::string Create(Module& m, const SearchEntry& entry, glm::vec2 position, const std::optional<PinRef>& from)
    {
        std::string id;
        switch (entry.What)
        {
            case SearchEntry::Kind::Operation:
                id = AddNode(m, entry.Name, entry.NodeKind, position);
                break;
            case SearchEntry::Kind::ModuleInput:
                id = AddNode(m, "Input", entry.NodeKind, position, entry.Name);
                break;
            case SearchEntry::Kind::Get:
                id = AddNode(m, "Get", entry.NodeKind, position, entry.Name);
                break;
            case SearchEntry::Kind::Set:
                id = AddNode(m, "Set", entry.NodeKind, position, entry.Name);
                break;
        }
        if (from && !entry.LinkPin.empty())
        {
            if (from->PinSide == Side::Output)
                Link(m, *from, {id, entry.LinkPin, Side::Input});
            else
                Link(m, {id, entry.LinkPin, Side::Output}, *from);
        }
        return id;
    }

    std::string CreateParameterNode(Module& m, const ParameterEntry& p, glm::vec2 position, bool set)
    {
        if (p.ModuleInput)
        {
            static const std::map<std::string, std::pair<const char*, Type>> ops {{"Grid", {"GridGet", Type::Float}},
                                                                                  {"VectorField", {"SampleVectorField", Type::Vec3}},
                                                                                  {"Mesh", {"RandomTriangle", Type::Int}},
                                                                                  {"Curve", {"Sample", Type::Float}},
                                                                                  {"Events", {"WriteEvent", Type::Int}},
                                                                                  {"ForceFields", {"SampleForceFields", Type::Vec3}}};
            auto at = ops.find(p.Interface);
            if (at != ops.end())
                return AddNode(m, at->second.first, at->second.second, position, p.Name);
            return AddNode(m, "Input", p.Kind, position, p.Name);
        }
        return AddNode(m, set && p.Writable ? "Set" : "Get", p.Kind, position, p.Name);
    }

    // ---------------------------------------------------------------------- inputs

    std::string AddInput(Module& m, Type kind)
    {
        std::string name = "Module.NewInput";
        for (int n = 1; std::any_of(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == name; }); ++n)
            name = "Module.NewInput" + std::to_string(n);
        Vfx::Parameter p;
        p.Name    = name;
        p.Kind    = kind;
        p.Default = Zero(kind);
        m.Inputs.push_back(p);
        return name;
    }

    std::optional<std::string> RenameInput(Module& m, const std::string& from, const std::string& to)
    {
        if (from == to)
            return std::nullopt;
        if (to.rfind("Module.", 0) != 0 || to.size() <= 7 || to.find_first_of(" \t\n") != std::string::npos)
            return "Module inputs are named Module.<Name>";
        if (std::any_of(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == to; }))
            return to + " already exists";
        auto at = std::find_if(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == from; });
        if (at == m.Inputs.end())
            return "Unknown input " + from;
        at->Name = to;
        for (auto& n : m.Nodes)
            if (n.Parameter == from)
                n.Parameter = to;
        return std::nullopt;
    }

    std::vector<std::string> InputUsers(const Module& m, const std::string& name)
    {
        std::vector<std::string> r;
        for (const auto& n : m.Nodes)
            if (n.Parameter == name)
                r.push_back(n.Guid);
        return r;
    }

    void RemoveInput(Module& m, const std::string& name)
    {
        m.Inputs.erase(std::remove_if(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == name; }),
                       m.Inputs.end());
        std::set<std::string> nodes;
        for (const auto& n : m.Nodes)
            if (n.Op == "Input" && n.Parameter == name)
                nodes.insert(n.Guid);
        DeleteNodes(m, nodes);
    }

    void MoveInput(Module& m, size_t index, int direction)
    {
        const long target = long(index) + direction;
        if (index >= m.Inputs.size() || target < 0 || target >= long(m.Inputs.size()))
            return;
        std::swap(m.Inputs[index], m.Inputs[size_t(target)]);
    }

    void SetInputType(Module& m, const std::string& name, Type kind)
    {
        for (auto& p : m.Inputs)
            if (p.Name == name && p.Kind != kind)
            {
                p.Kind = kind;
                if (p.Default)
                    p.Default = Zero(kind);
                if (kind != Type::Int)
                    p.EnumNames.clear();
                p.Range.reset();
            }
        for (auto& n : m.Nodes)
            if (n.Op == "Input" && n.Parameter == name)
            {
                n.Kind = kind;
                for (auto& o : n.Outputs)
                    o.Kind = kind;
            }
    }

    // ---------------------------------------------------------------------- canvas view

    uint32_t TypeColor(Type t)
    {
        switch (t)
        {
            case Type::Float:
                return IM_COL32(140, 214, 110, 255);
            case Type::Int:
                return IM_COL32(60, 200, 180, 255);
            case Type::Bool:
                return IM_COL32(220, 70, 70, 255);
            case Type::Vec2:
                return IM_COL32(150, 220, 230, 255);
            case Type::Vec3:
                return IM_COL32(240, 200, 60, 255);
            case Type::Vec4:
                return IM_COL32(230, 140, 70, 255);
            case Type::Color:
                return IM_COL32(235, 110, 190, 255);
            case Type::Quat:
                return IM_COL32(150, 120, 230, 255);
            case Type::Position:
                return IM_COL32(250, 160, 40, 255);
            case Type::IVec3:
                return IM_COL32(80, 160, 240, 255);
        }
        return IM_COL32(200, 200, 200, 255);
    }

    uint32_t CategoryColor(const std::string& category)
    {
        static const std::map<std::string, uint32_t> colors {{"Math", IM_COL32(62, 88, 120, 255)},
                                                             {"Vector", IM_COL32(110, 96, 46, 255)},
                                                             {"Noise", IM_COL32(86, 64, 122, 255)},
                                                             {"Grid", IM_COL32(40, 104, 112, 255)},
                                                             {"Sampling", IM_COL32(44, 104, 70, 255)},
                                                             {"Events", IM_COL32(128, 72, 44, 255)},
                                                             {"Custom", IM_COL32(120, 52, 82, 255)},
                                                             {"Parameters", IM_COL32(74, 78, 86, 255)}};
        auto at = colors.find(category);
        return at == colors.end() ? IM_COL32(70, 70, 70, 255) : at->second;
    }

    std::string NodeTitle(const Vfx::Node& n)
    {
        if (n.Op == "Input" || n.Op == "Get" || n.Op == "Set")
            return n.Op + " " + Vfx::TypeName(n.Kind);
        return n.Op;
    }

    float ValueWidth(Type t)
    {
        switch (t)
        {
            case Type::Bool:
                return 18;
            case Type::Float:
            case Type::Int:
                return 56;
            case Type::Vec2:
                return 96;
            case Type::Vec3:
            case Type::Position:
            case Type::IVec3:
                return 132;
            case Type::Vec4:
            case Type::Quat:
                return 168;
            case Type::Color:
                return 40;
        }
        return 0;
    }

    NodeCanvas::Graph BuildGraph(const Module& m, const std::vector<Diagnostic>& diagnostics)
    {
        NodeCanvas::Graph     g;
        std::set<std::string> linkedIn, linkedOut;
        for (const auto& l : m.Links)
        {
            linkedIn.insert(l.To + '\n' + l.Input);
            linkedOut.insert(l.From + '\n' + l.Output);
        }
        for (const auto& n : m.Nodes)
        {
            NodeCanvas::Node v;
            v.Id       = n.Guid;
            const auto* info = Vfx::FindOperation(n.Op);
            v.Title    = NodeTitle(n);
            v.Subtitle = !n.Parameter.empty() ? n.Parameter + (n.Code.empty() || n.Op == "CustomGLSL" ? "" : " / " + n.Code) :
                         n.Op == "Constant"   ? std::string() :
                                                std::string();
            v.Color    = CategoryColor(info ? info->Category : "Custom");
            auto at    = m.Editor.Positions.find(n.Guid);
            v.Position = at == m.Editor.Positions.end() ? glm::vec2(0) : at->second;
            v.Collapsed = m.Editor.Collapsed.count(n.Guid) > 0;
            v.Reroute   = n.Op == "Reroute";
            const auto pin = [&](const Vfx::Pin& p, bool input, size_t index) {
                NodeCanvas::Pin c;
                c.Name      = p.Name;
                c.Color     = TypeColor(p.Kind);
                c.Connected = (input ? linkedIn : linkedOut).count(n.Guid + '\n' + p.Name) > 0;
                c.Tooltip   = std::string(Vfx::TypeName(p.Kind)) + (input && !p.Default && !c.Connected ? " (required)" : "");
                if (input && info && index < info->Inputs.size() && !info->Inputs[index].Enum.empty())
                    c.Tooltip += " enum";
                if (input && !c.Connected && !v.Reroute)
                    c.ValueWidth = ValueWidth(p.Kind);
                return c;
            };
            for (size_t k = 0; k < n.Inputs.size(); ++k)
                v.Inputs.push_back(pin(n.Inputs[k], true, k));
            for (size_t k = 0; k < n.Outputs.size(); ++k)
                v.Outputs.push_back(pin(n.Outputs[k], false, k));
            for (const auto& d : diagnostics)
                if (d.Node == n.Guid)
                {
                    const auto mark = d.Warning ? NodeCanvas::Badge::Warning : NodeCanvas::Badge::Error;
                    if (int(mark) > int(v.Mark))
                        v.Mark = mark;
                    v.Message += (v.Message.empty() ? "" : "\n") + (d.Pin.empty() ? "" : d.Pin + ": ") + d.Message;
                }
            g.Nodes.push_back(std::move(v));
        }
        for (const auto& l : m.Links)
        {
            NodeCanvas::Link v {l.From, l.Output, l.To, l.Input, IM_COL32(200, 200, 200, 255)};
            if (auto t = PinType(m, {l.From, l.Output, Side::Output}))
                v.Color = TypeColor(*t);
            g.Links.push_back(v);
        }
        for (const auto& c : m.Editor.Comments)
            g.Comments.push_back({c.Guid, c.Title, c.Position, c.Size, c.Color});
        return g;
    }

    void AutoLayout(Module& m)
    {
        std::vector<const Vfx::Node*> missing;
        for (const auto& n : m.Nodes)
            if (!m.Editor.Positions.count(n.Guid))
                missing.push_back(&n);
        if (missing.empty())
            return;
        const auto graph = BuildGraph(m);
        std::map<std::string, glm::vec2> sizes;
        for (const auto& v : graph.Nodes)
        {
            auto l      = NodeCanvas::Layout(v);
            sizes[v.Id] = l.Max - l.Min;
        }
        constexpr float columnGap = 80, rowGap = 24;
        if (missing.size() != m.Nodes.size())
        {
            // A few new nodes: a column right of the laid-out ones.
            float x = 0, y = 0;
            for (const auto& [id, p] : m.Editor.Positions)
                x = std::max(x, p.x + sizes[id].x);
            for (const auto* n : missing)
            {
                m.Editor.Positions[n->Guid] = {x + columnGap, y};
                y += sizes[n->Guid].y + rowGap;
            }
            return;
        }
        // Layers by longest path from the nodes nothing feeds (inputs) to the sinks (outputs).
        std::map<std::string, int>                      layer;
        std::map<std::string, std::vector<std::string>> predecessors;
        for (const auto& l : m.Links)
            if (FindNode(m, l.From) && FindNode(m, l.To))
                predecessors[l.To].push_back(l.From);
        std::set<std::string>                   visiting;
        std::function<int(const std::string&)> depth = [&](const std::string& id) {
            if (auto at = layer.find(id); at != layer.end())
                return at->second;
            if (!visiting.insert(id).second)
                return 0; // A cycle (invalid graph): break it here.
            int d = 0;
            for (const auto& p : predecessors[id])
                d = std::max(d, depth(p) + 1);
            visiting.erase(id);
            return layer[id] = d;
        };
        int layers = 0;
        for (const auto& n : m.Nodes)
            layers = std::max(layers, depth(n.Guid) + 1);
        std::vector<std::vector<std::string>> columns(static_cast<size_t>(layers));
        for (const auto& n : m.Nodes)
            columns[size_t(layer[n.Guid])].push_back(n.Guid);
        float x = 0;
        for (auto& column : columns)
        {
            // Order by the mean height of what feeds each node, so links cross less.
            std::map<std::string, float> key;
            for (size_t k = 0; k < column.size(); ++k)
            {
                float sum = 0;
                int   count = 0;
                for (const auto& p : predecessors[column[k]])
                    if (auto at = m.Editor.Positions.find(p); at != m.Editor.Positions.end())
                        sum += at->second.y + sizes[p].y * 0.5f, ++count;
                key[column[k]] = count ? sum / float(count) : float(k) * 1e-3f;
            }
            std::stable_sort(column.begin(), column.end(), [&](const auto& a, const auto& b) { return key[a] < key[b]; });
            float y = 0, width = 0;
            for (const auto& id : column)
            {
                m.Editor.Positions[id] = {x, y};
                y += sizes[id].y + rowGap;
                width = std::max(width, sizes[id].x);
            }
            x += width + columnGap;
        }
    }

    std::vector<Diagnostic> Warnings(const Module& m)
    {
        std::vector<Diagnostic> r;
        std::set<std::string>   used;
        for (const auto& l : m.Links)
            used.insert(l.From);
        for (const auto& n : m.Nodes)
        {
            const bool sink = n.Op == "Set" || n.Op == "WriteEvent" || n.Op == "GridSet" || n.Op == "GridAddFloat" ||
                              n.Guid == m.ResultNode;
            if (!sink && !used.count(n.Guid))
                r.push_back({n.Guid, "", "Result is not used", true});
        }
        return r;
    }

    // ---------------------------------------------------------------------- module test compile

    std::vector<Diagnostic> TestCompile(const Module& source, const Vfx::ModuleResolver& resolve)
    {
        using namespace Vfx;
        Module module = source;
        if (!module.Identity.IsValid())
            module.Identity = AssetID(SourceAssetID::FromString("7e57000000000001"), 1);
        // The first allowed stage; event handler modules are tested in ParticleUpdate.
        int stage = -1;
        for (int s : {0, 1, 2, 3, 4, 5, 7})
            if (module.AllowedStages & (1u << s))
            {
                stage = s;
                break;
            }
        if (stage < 0)
            stage = int(Stage::ParticleUpdate);
        bool gpu = false, gridIteration = false;
        for (const auto& n : module.Nodes)
        {
            gpu = gpu || n.Op == "CustomGLSL" || IsGridOp(n.Op) || n.Op == "MainLightDirection" || n.Op == "ColliderVelocity" ||
                  n.Parameter == "Engine.MainLightDirection";
            gridIteration = gridIteration || n.Op == "GridSet" || n.Op == "GridLightMarch" || n.Parameter == "Grid.ExecutionIndex";
        }

        Emitter emitter;
        emitter.Name   = "ModuleTest";
        emitter.Target = gpu ? SimTarget::GPU : SimTarget::CPU;
        ModuleUse use;
        use.Guid   = "module-test";
        use.Script = module.Identity;

        // Bind data interfaces for inputs that ops read by name. Each grid input gets its own
        // attribute, typed by the nodes that read or write it without naming one.
        InterfaceBinding grid;
        grid.Kind        = "Grid3D";
        grid.Resolution  = glm::uvec3(8);
        grid.WorldBounds = glm::vec3(1);
        const auto attribute = [&](const std::string& name, Type kind) {
            if (std::none_of(grid.GridAttributes.begin(), grid.GridAttributes.end(), [&](const auto& a) { return a.Name == name; }))
                grid.GridAttributes.push_back({name, kind});
        };
        const auto valueOp = [](const std::string& op) {
            return op == "GridGet" || op == "GridSet" || op == "GridSample" || op == "GridLightMarch" || op == "GridAddFloat";
        };
        for (const auto& n : module.Nodes)
            if (IsGridOp(n.Op) && !n.Code.empty())
                attribute(n.Code, n.Op == "GridAddFloat" ? Type::Vec4 : n.Kind);
        std::map<std::string, std::string> gridChannels;
        for (const auto& p : module.Inputs)
        {
            std::optional<Type> kind;
            bool                grid3 = false;
            for (const auto& n : module.Nodes)
                if (n.Parameter == p.Name && IsGridOp(n.Op))
                {
                    grid3 = true;
                    if (n.Code.empty() && valueOp(n.Op) && !kind)
                        kind = n.Op == "GridAddFloat" ? Type::Vec4 : n.Op == "GridLightMarch" ? Type::Float : n.Kind;
                }
            if (!grid3)
                continue;
            const auto name = "Test" + p.Name.substr(p.Name.find('.') + 1);
            attribute(name, kind.value_or(Type::Float));
            gridChannels[p.Name] = name;
        }
        bool usesGrid = false, reader = false;
        for (const auto& p : module.Inputs)
        {
            std::string kind, op;
            for (const auto& n : module.Nodes)
                if (n.Parameter == p.Name && n.Op != "Input" && InterfaceKind(n.Op).size())
                    kind = InterfaceKind(n.Op), op = n.Op;
            Input input;
            const bool read = std::any_of(module.Nodes.begin(), module.Nodes.end(),
                                          [&](const Node& n) { return n.Op == "Input" && n.Parameter == p.Name; });
            if (kind.empty() || read)
                continue; // Inputs read as values keep their literal default.
            input.Kind = InputKind::DataInterface;
            auto& b    = input.Interface;
            if (kind == "Grid")
            {
                b.Kind    = "Grid3D";
                b.Source  = "Emitter.TestGrid";
                b.Channel = gridChannels[p.Name];
                usesGrid  = true;
            }
            else if (kind == "VectorField")
            {
                try
                {
                    b = ImportVectorField(VectorFieldSource {}, {});
                }
                catch (const std::exception&)
                {
                    b.Kind = "VectorField";
                }
            }
            else if (kind == "Mesh")
            {
                bool skeletal = false;
                for (const auto& n : module.Nodes)
                    skeletal = skeletal || (n.Parameter == p.Name && (n.Op == "SampleSkinnedTriangle" || n.Op.find("Bone") != std::string::npos));
                b.Kind   = skeletal ? "SkeletalMesh" : "StaticMesh";
                b.Source = "Owner";
            }
            else if (kind == "Curve")
            {
                const Node* sample = nullptr;
                for (const auto& n : module.Nodes)
                    if (n.Parameter == p.Name && n.Op == "Sample")
                        sample = &n;
                const bool texture = sample && !sample->Inputs.empty() && sample->Inputs[0].Kind == Type::Vec2;
                b.Kind             = texture ? "Texture2D" : sample && sample->Kind == Type::Color ? "Gradient" : "Curve";
                if (texture)
                    b.Width = b.Height = 1, b.Values = {glm::vec4(1)};
                else
                    b.Times = {0, 1}, b.Values = {glm::vec4(0), glm::vec4(1)};
            }
            else if (kind == "Events")
            {
                std::string channel = "Test";
                for (const auto& n : module.Nodes)
                    if (n.Parameter == p.Name && !n.Interface.Channel.empty())
                        channel = n.Interface.Channel;
                b.Kind    = op == "WriteEvent" ? "Events" : "EmitterParticles";
                b.Channel = channel;
                if (op == "WriteEvent")
                {
                    if (std::none_of(emitter.Channels.begin(), emitter.Channels.end(), [&](const auto& c) { return c.Name == channel; }))
                        emitter.Channels.push_back({channel, module.EventLayouts.empty() ? channel : module.EventLayouts.front().Name, 64});
                }
                else
                {
                    b.SourceEmitter = "module-test-source";
                    reader          = true;
                }
            }
            else if (kind == "ForceFields")
                b.Kind = "ForceFields";
            use.Inputs[p.Name] = input;
        }
        if (grid.GridAttributes.empty())
            grid.GridAttributes.push_back({"Value", Type::Float});
        if (usesGrid || gridIteration)
        {
            emitter.Target                           = SimTarget::GPU;
            emitter.DataInterfaces["Emitter.TestGrid"] = grid;
        }
        // Event writers declared on nodes rather than inputs.
        for (const auto& n : module.Nodes)
            if (n.Op == "WriteEvent" && !n.Interface.Channel.empty() &&
                std::none_of(emitter.Channels.begin(), emitter.Channels.end(), [&](const auto& c) { return c.Name == n.Interface.Channel; }))
                emitter.Channels.push_back({n.Interface.Channel,
                                            module.EventLayouts.empty() ? n.Interface.Channel : module.EventLayouts.front().Name, 64});

        // Functions and dynamic inputs are tested through a wrapper module that calls them.
        Module wrapper;
        bool   wrapped = module.ScriptUsage != Usage::Module;
        if (wrapped)
        {
            wrapper.Identity      = AssetID(SourceAssetID::FromString("7e57000000000002"), 1);
            wrapper.AllowedStages = 1u << stage;
            Type result           = Type::Float;
            if (const auto* r = FindNode(module, module.ResultNode))
                for (const auto& o : r->Outputs)
                    if (o.Name == module.ResultPin)
                        result = o.Kind;
            Node value;
            value.Guid = "value";
            value.Kind = result;
            value.Outputs = {{"Value", result, {}}};
            if (module.ScriptUsage == Usage::Function)
            {
                value.Op       = "Call";
                value.Function = module.Identity;
                for (const auto& p : module.Inputs)
                    value.Inputs.push_back({p.Name, p.Kind, p.Default ? p.Default : std::optional<Value>(Zero(p.Kind))});
            }
            else
            {
                value.Op        = "Input";
                value.Parameter = "Module.Test";
                wrapper.Inputs.push_back({"Module.Test", result, Zero(result), {}, {}});
                Input dynamic;
                dynamic.Kind   = InputKind::DynamicInput;
                dynamic.Script = module.Identity;
                dynamic.Inputs = use.Inputs;
                use.Inputs     = {{"Module.Test", dynamic}};
            }
            Node set;
            set.Guid      = "set";
            set.Op        = "Set";
            set.Kind      = result;
            set.Parameter = "Transient.ModuleTest";
            set.Inputs    = {{"Value", result, {}}};
            set.Outputs   = {{"Value", result, {}}};
            wrapper.Nodes = {value, set};
            wrapper.Links = {{"value", "Value", "set", "Value"}};
            use.Script    = wrapper.Identity;
        }

        System system;
        if (stage <= int(Stage::SystemUpdate))
            system.Stages[Stage(stage)] = {use};
        else if (stage == int(Stage::SimulationStage))
        {
            SimulationStage s;
            s.Guid    = "stage";
            s.Name    = "ModuleTest";
            s.Modules = {use};
            if (gridIteration)
            {
                s.Source        = IterationSource::DataInterface;
                s.DataInterface = "Emitter.TestGrid";
            }
            emitter.SimulationStages.push_back(s);
        }
        else
            emitter.Stages[Stage(stage)] = {use};
        if (stage <= int(Stage::SystemUpdate) || emitter.Stages.empty())
            emitter.Stages[Stage::ParticleUpdate] = {};
        EmitterHandle handle;
        handle.Guid   = "module-test-emitter";
        handle.Name   = emitter.Name;
        handle.Inline = emitter;
        system.Emitters.push_back(handle);
        if (reader)
        {
            // Particle readers need another emitter to read.
            EmitterHandle other;
            other.Guid           = "module-test-source";
            other.Name           = "Source";
            other.Inline         = Emitter {};
            other.Inline->Name   = "Source";
            other.Inline->Target = emitter.Target;
            other.Inline->Stages[Stage::ParticleUpdate] = {};
            system.Emitters.push_back(other);
        }

        const auto modules = [&](AssetID id) -> Module {
            if (id == module.Identity)
                return module;
            if (wrapped && id == wrapper.Identity)
                return wrapper;
            // The built-in list, per root (tests and tools may change it).
            static std::mutex                                                lock;
            static std::map<fs::path, std::map<AssetID, fs::path>>           cache;
            std::map<AssetID, fs::path>                                      builtins;
            {
                std::lock_guard<std::mutex> guard(lock);
                auto                        root = BuiltinModuleRoot();
                auto                        at   = cache.find(root);
                if (at == cache.end())
                    at = cache.emplace(root, FindBuiltinModules()).first;
                builtins = at->second;
            }
            if (auto at = builtins.find(id); at != builtins.end())
                return DecodeModule(YAML::LoadFile(at->second.string()));
            if (resolve)
                return resolve(id);
            throw std::invalid_argument("Missing script " + id.ToString());
        };
        std::vector<Diagnostic> r;
        try
        {
            auto result = Compile(
                system, [](AssetID id) -> Emitter { throw std::invalid_argument("Missing emitter " + id.ToString()); }, modules);
            for (const auto& e : result.Errors)
            {
                const bool own = e.Asset == module.Identity;
                r.push_back({own ? e.Node : std::string(), e.Pin, e.Message, false});
            }
        }
        catch (const std::exception& e)
        {
            r.push_back({"", "", e.what(), false});
        }
        return r;
    }

    // ---------------------------------------------------------------------- files

    bool IsBuiltin(const fs::path& file)
    {
        std::error_code error;
        fs::path        root;
        try
        {
            root = fs::weakly_canonical(Vfx::BuiltinModuleRoot(), error);
        }
        catch (const std::exception&)
        {
            return false;
        }
        const auto path = fs::weakly_canonical(file, error);
        auto       rel  = path.lexically_relative(root);
        return !rel.empty() && *rel.begin() != "..";
    }

    std::variant<fs::path, std::string> CreateModuleFile(const fs::path& directory, const std::string& name, Vfx::Usage usage,
                                                         uint32_t stages)
    {
        if (stages == 0)
            return std::string("Choose at least one allowed stage");
        const auto id = Identifier(name);
        Module     m;
        m.Identity      = AssetID(SourceAssetID::Generate(), 1);
        m.ScriptUsage   = usage;
        m.AllowedStages = stages;
        m.Description   = name;
        m.Inputs.push_back({"Module.Value", Type::Float, Vfx::Value::Float(1), {}, {}});
        auto input = AddNode(m, "Input", Type::Float, {0, 0}, "Module.Value");
        if (usage == Vfx::Usage::Module)
        {
            int first = 0;
            while (!(stages & (1u << first)))
                ++first;
            const char* space = first <= 1 ? "System." : first <= 3 ? "Emitter." : "Particles.";
            auto        set   = AddNode(m, "Set", Type::Float, {260, 0}, space + id);
            m.Links.push_back({input, "Value", set, "Value"});
        }
        else
        {
            auto result = AddNode(m, "Reroute", Type::Float, {260, 20});
            m.Links.push_back({input, "Value", result, "Value"});
            m.ResultNode = result;
        }
        std::error_code error;
        fs::create_directories(directory, error);
        const auto file = UniqueFile(directory, id);
        if (auto failure = Write(file, m))
            return *failure;
        return file;
    }

    std::variant<fs::path, std::string> Duplicate(const Module& source, const fs::path& file, const fs::path& directory)
    {
        Module copy   = source;
        copy.Identity = AssetID(SourceAssetID::Generate(), 1);
        if (copy.Category == "Built-in")
            copy.Category.clear();
        std::error_code error;
        fs::create_directories(directory, error);
        const auto target = UniqueFile(directory, file.stem().u8string());
        if (auto failure = Write(target, copy))
            return *failure;
        return target;
    }
} // namespace Kans::VfxModuleEditing
