#include "kspch.h"
#include "VfxCompiler.h"
#include <iomanip>
#include <regex>
#include <sstream>
#include <glm/gtc/matrix_inverse.hpp>
#include <shaderc/shaderc.hpp>
namespace Kans::Vfx
{
    namespace
    {
        bool VectorType(Type t) { return t != Type::Int && t != Type::Bool; }
        bool Widen(Type from, Type to)
        {
            return from == to || (from == Type::Float && (to == Type::Vec2 || to == Type::Vec3 || to == Type::Vec4));
        }
        const std::map<std::string, uint32_t> Arity = {{"Add", 2},
                                                       {"Sub", 2},
                                                       {"Mul", 2},
                                                       {"Div", 2},
                                                       {"Dot", 2},
                                                       {"Cross", 2},
                                                       {"Normalize", 1},
                                                       {"Length", 1},
                                                       {"Lerp", 3},
                                                       {"Clamp", 3},
                                                       {"Saturate", 1},
                                                       {"Min", 2},
                                                       {"Max", 2},
                                                       {"Abs", 1},
                                                       {"Floor", 1},
                                                       {"Frac", 1},
                                                       {"Pow", 2},
                                                       {"Sin", 1},
                                                       {"Cos", 1},
                                                       {"Step", 2},
                                                       {"Smoothstep", 3},
                                                       {"Less", 2},
                                                       {"LessEqual", 2},
                                                       {"Greater", 2},
                                                       {"Equal", 2},
                                                       {"Select", 3},
                                                       {"Make", 0},
                                                       {"Break", 1},
                                                       {"Random", 0},
                                                       {"RandomRange", 2},
                                                       {"RandomVector", 0},
                                                       {"RandomUnitVector", 0},
                                                       {"Noise3D", 1},
                                                       {"Sample", 1},
                                                       {"TransformPosition", 1},
                                                       {"TransformVector", 1},
                                                       {"InverseTransformPosition", 1},
                                                       {"InverseTransformVector", 1},
                                                       {"Splat", 1},
                                                       {"ToInt", 1},
                                                       {"ToFloat", 1},
                                                       {"And", 2},
                                                       {"Or", 2},
                                                       {"Not", 1}};
        std::string                           GType(Type);
        struct Compiler
        {
            const ModuleResolver&            Resolve;
            std::map<std::string, Parameter> Known;
            std::vector<Diagnostic>&         Errors;
            Program                          Out;
            std::vector<AssetID>             Calls;
            AssetID                          Asset;
            Space                            SimulationSpace = Space::Local;
            uint32_t                         Emit(Instruction i)
            {
                if (i.Kind == Type::Position && i.Op != "Get" && i.Op != "Set")
                {
                    i.PositionSpace = Space::Local;
                    for (auto argument : i.Args)
                        if (Out.Code[argument].Kind == Type::Position &&
                            Out.Code[argument].PositionSpace == Space::World)
                            i.PositionSpace = Space::World;
                    if (i.Op == "TransformPosition")
                        i.PositionSpace = Space::World;
                    if (i.Op == "InverseTransformPosition")
                        i.PositionSpace = Space::Local;
                }
                Out.Code.push_back(std::move(i));
                return uint32_t(Out.Code.size() - 1);
            }
            uint32_t PositionTo(uint32_t value, Space target)
            {
                if (Out.Code[value].Kind != Type::Position || Out.Code[value].PositionSpace == target)
                    return value;
                Instruction transform;
                transform.Op   = target == Space::World ? "TransformPosition" : "InverseTransformPosition";
                transform.Kind = Type::Position;
                transform.Args = {value};
                return Emit(std::move(transform));
            }
            uint32_t Constant(Value v)
            {
                Instruction i;
                i.Op      = "Constant";
                i.Kind    = v.Kind;
                i.Literal = v;
                return Emit(i);
            }
            void Error(AssetID a, const std::string& n, const std::string& p, std::string m)
            {
                Errors.push_back({a, n, p, std::move(m)});
            }
            uint32_t Param(const std::string& name, Type type)
            {
                for (uint32_t i = 0; i < Out.Parameters.size(); ++i)
                    if (Out.Parameters[i].Name == name)
                        return i;
                auto k = Known.find(name);
                Out.Parameters.push_back(k == Known.end() ? Parameter {name, type, {}, {}} : k->second);
                return uint32_t(Out.Parameters.size() - 1);
            }
            uint32_t Convert(uint32_t value, Type type, AssetID asset, const std::string& node, const std::string& pin)
            {
                auto from = Out.Code[value].Kind;
                if (from == type)
                    return value;
                if (!Widen(from, type))
                {
                    Error(asset, node, pin, std::string("Type mismatch: ") + TypeName(from) + " -> " + TypeName(type));
                    return Constant(Value::Vector({}, type));
                }
                Instruction i;
                i.Op   = "Splat";
                i.Kind = type;
                i.Args = {value};
                return Emit(i);
            }
            uint32_t
            Get(const std::string& name, Type type, AssetID asset, const std::string& node, const std::string& pin)
            {
                if (name == "Engine.Owner.LocalToWorld")
                {
                    if (type != Type::Vec4)
                        Error(asset, node, pin, "Owner.LocalToWorld columns require vec4 pins");
                    Instruction matrix;
                    matrix.Op        = "OwnerMatrixColumn";
                    matrix.Kind      = Type::Vec4;
                    matrix.Parameter = pin == "Column1" ? 1 : pin == "Column2" ? 2 : pin == "Column3" ? 3 : 0;
                    Out.Reads.insert(name);
                    return Emit(std::move(matrix));
                }
                auto k = Known.find(name);
                if (k == Known.end() || !k->second.Default)
                {
                    Error(asset, node, pin, "Read of unset parameter: " + name);
                    return Constant(Value::Vector({}, type));
                }
                if (k->second.Kind != type)
                    Error(asset, node, pin, "Parameter type mismatch: " + name);
                Instruction i;
                i.Op        = "Get";
                i.Kind      = k->second.Kind;
                i.Parameter = Param(name, type);
                Out.Reads.insert(name);
                if (i.Kind == Type::Position)
                    i.PositionSpace = (name.rfind("Particles.", 0) == 0 || name.rfind("Transient.", 0) == 0) ?
                                          SimulationSpace :
                                          Space::World;
                return Emit(i);
            }
            std::string Scoped(std::string name, const std::string& scope)
            {
                auto p = name.find("$Usage");
                if (p != std::string::npos)
                    name.replace(p, 6, scope);
                return name;
            }
            bool CanWrite(const std::string& n)
            {
                auto s = Out.StageKind;
                return (n.rfind("Transient.", 0) == 0) ||
                       (n.rfind("Particles.", 0) == 0 && (s == Stage::ParticleSpawn || s == Stage::ParticleUpdate)) ||
                       (n.rfind("Emitter.", 0) == 0 && (s == Stage::EmitterSpawn || s == Stage::EmitterUpdate)) ||
                       (n.rfind("System.", 0) == 0 && (s == Stage::SystemSpawn || s == Stage::SystemUpdate));
            }
            uint32_t Inline(AssetID                                id,
                            Usage                                  expected,
                            const std::map<std::string, Input>&    inputs,
                            const std::string&                     scope,
                            const std::map<std::string, uint32_t>* arguments = nullptr)
            {
                if (std::find(Calls.begin(), Calls.end(), id) != Calls.end())
                {
                    Error(id, "", "", "Function/dynamic input recursion");
                    return Constant(Value::Float(0));
                }
                if (Calls.size() >= 64 || std::count(scope.begin(), scope.end(), '/') >= 64)
                {
                    Error(id, "", "", "VFX call depth exceeds 64");
                    return Constant(Value::Float(0));
                }
                Module m;
                try
                {
                    m = Resolve(id);
                }
                catch (const std::exception& e)
                {
                    Error(id, "", "", e.what());
                    return Constant(Value::Float(0));
                }
                if (m.ScriptUsage != expected)
                    Error(id, "", "", "Script Usage does not match call site");
                if (!(m.AllowedStages & (1u << uint32_t(Out.StageKind))))
                    Error(id, "", "", "Script not allowed in " + std::string(StageName(Out.StageKind)));
                std::map<std::string, uint32_t> resolved;
                for (const auto& p : m.Inputs)
                {
                    auto     input = inputs.find(p.Name);
                    auto     arg   = arguments ? arguments->find(p.Name) : resolved.end();
                    uint32_t v;
                    if (arguments && arg != arguments->end())
                        v = arg->second;
                    else if (input == inputs.end())
                    {
                        if (p.Default)
                            v = Constant(*p.Default);
                        else
                        {
                            Error(id, "", p.Name, "Missing module input");
                            v = Constant(Value::Vector({}, p.Kind));
                        }
                    }
                    else
                    {
                        const auto& x = input->second;
                        if (x.Kind == InputKind::Literal)
                            v = Constant(x.Literal);
                        else if (x.Kind == InputKind::Link)
                            v = Get(x.Link, p.Kind, id, "", p.Name);
                        else if (x.Kind == InputKind::DynamicInput)
                            v = Inline(x.Script, Usage::DynamicInput, x.Inputs, scope + "/" + p.Name);
                        else
                        { // A DI input is a sample table binding; it is attached to Sample nodes below.
                            v = Constant(Value::Vector({}, p.Kind));
                        }
                    }
                    resolved[p.Name] = Convert(v, p.Kind, id, "", p.Name);
                }
                for (const auto& [name, value] : inputs)
                    if (std::none_of(m.Inputs.begin(), m.Inputs.end(), [&](const auto& p) { return p.Name == name; }))
                        Error(id, "", name, "Unknown module input");
                Calls.push_back(id);
                std::map<std::string, const Node*> nodes;
                for (const auto& n : m.Nodes)
                    if (!nodes.emplace(n.Guid, &n).second)
                        Error(id, n.Guid, "", "Duplicate node GUID");
                std::map<std::pair<std::string, std::string>, Link> links;
                for (const auto& l : m.Links)
                {
                    auto to = nodes.find(l.To), from = nodes.find(l.From);
                    if (to == nodes.end() || from == nodes.end())
                    {
                        Error(id, l.To, l.Input, "Link references missing node");
                        continue;
                    }
                    if (std::none_of(to->second->Inputs.begin(), to->second->Inputs.end(), [&](const auto& p) {
                            return p.Name == l.Input;
                        }))
                        Error(id, l.To, l.Input, "Link references missing input pin");
                    if (std::none_of(from->second->Outputs.begin(), from->second->Outputs.end(), [&](const auto& p) {
                            return p.Name == l.Output;
                        }))
                        Error(id, l.From, l.Output, "Link references missing output pin");
                    if (!links.emplace(std::make_pair(l.To, l.Input), l).second)
                        Error(id, l.To, l.Input, "Multiple links to input");
                }
                // Discover writes before reads, including persistent scratch unique to each usage.
                for (const auto& n : m.Nodes)
                    if (n.Op == "Set")
                    {
                        auto name = Scoped(n.Parameter, scope);
                        auto k    = Known.find(name);
                        if (k == Known.end())
                            Known[name] = {name, n.Kind, Value::Vector({}, n.Kind), {}};
                        else if (k->second.Kind != n.Kind)
                            Error(id, n.Guid, "Value", "Parameter type mismatch: " + name);
                    }
                std::map<std::pair<std::string, std::string>, uint32_t>         cache;
                std::set<std::pair<std::string, std::string>>                   visiting;
                std::function<uint32_t(const std::string&, const std::string&)> visit;
                visit = [&](const std::string& guid, const std::string& output) -> uint32_t {
                    auto key = std::make_pair(guid, output);
                    if (auto c = cache.find(key); c != cache.end())
                        return c->second;
                    auto at = nodes.find(guid);
                    if (at == nodes.end())
                    {
                        Error(id, guid, output, "Missing graph result node");
                        return Constant(Value::Float(0));
                    }
                    const auto& n = *at->second;
                    if (!visiting.insert(key).second)
                    {
                        Error(id, guid, output, "Graph link cycle");
                        return Constant(Value::Float(0));
                    }
                    Instruction i;
                    i.Op        = n.Op;
                    i.Kind      = n.Kind;
                    i.Code      = n.Code;
                    i.Output    = output;
                    i.RandomKey = Hash(StableHash(scope) ^ StableHash(guid + "/" + output));
                    i.Interface = n.Interface;
                    auto outPin = std::find_if(
                        n.Outputs.begin(), n.Outputs.end(), [&](const auto& p) { return p.Name == output; });
                    if (outPin != n.Outputs.end())
                        i.Kind = outPin->Kind;
                    for (const auto& pin : n.Inputs)
                    {
                        auto     l = links.find({guid, pin.Name});
                        uint32_t v;
                        if (l != links.end())
                            v = visit(l->second.From, l->second.Output);
                        else if (pin.Default)
                            v = Constant(*pin.Default);
                        else
                        {
                            Error(id, guid, pin.Name, "Unconnected required input");
                            v = Constant(Value::Vector({}, pin.Kind));
                        }
                        i.Args.push_back(Convert(v, pin.Kind, id, guid, pin.Name));
                    }
                    uint32_t reg = 0;
                    if (n.Op == "Input")
                    {
                        auto p = resolved.find(n.Parameter);
                        if (p == resolved.end())
                        {
                            Error(id, guid, output, "Unknown module input: " + n.Parameter);
                            reg = Constant(Value::Vector({}, i.Kind));
                        }
                        else
                            reg = Convert(p->second, i.Kind, id, guid, output);
                    }
                    else if (n.Op == "Constant")
                        reg = Convert(Constant(n.Constant), i.Kind, id, guid, output);
                    else if (n.Op == "Get")
                        reg = Get(Scoped(n.Parameter, scope), i.Kind, id, guid, output);
                    else if (n.Op == "Set")
                    {
                        const auto name = Scoped(n.Parameter, scope);
                        if (!CanWrite(name))
                            Error(id, guid, "Value", "Cannot write " + name + " in " + StageName(Out.StageKind));
                        if (i.Args.size() != 1)
                            Error(id, guid, "Value", "Set requires one input");
                        else
                            i.Args[0] = Convert(i.Args[0], n.Kind, id, guid, "Value");
                        if (n.Kind == Type::Position && !i.Args.empty())
                        {
                            i.PositionSpace = (name.rfind("Particles.", 0) == 0 || name.rfind("Transient.", 0) == 0) ?
                                                  SimulationSpace :
                                                  Space::World;
                            i.Args[0]       = PositionTo(i.Args[0], i.PositionSpace);
                        }
                        i.Parameter = Param(name, n.Kind);
                        i.Kind      = n.Kind;
                        Out.Writes.insert(name);
                        reg = Emit(i);
                    }
                    else if (n.Op == "Call")
                    {
                        std::map<std::string, uint32_t> a;
                        for (size_t k = 0; k < n.Inputs.size(); ++k)
                            a[n.Inputs[k].Name] = i.Args[k];
                        reg = Convert(
                            Inline(n.Function, Usage::Function, {}, scope + "/" + guid, &a), i.Kind, id, guid, output);
                    }
                    else
                    {
                        auto arity = Arity.find(n.Op);
                        if (n.Op == "CustomGLSL")
                        {
                            if (Out.Target == SimTarget::CPU)
                                Error(id, guid, output, "Custom GLSL node is GPU only");
                            const bool body = i.Code.find(';') != std::string::npos;
                            if (body)
                                for (const auto& pin : n.Outputs)
                                {
                                    if (!std::regex_match(pin.Name, std::regex("[A-Za-z_][A-Za-z0-9_]*")))
                                        Error(id, guid, pin.Name, "Invalid custom GLSL output name");
                                    i.CustomDeclarations +=
                                        GType(pin.Kind) + " " + pin.Name + "=" + GType(pin.Kind) + "(0);\n";
                                }
                            // Typed names are substituted in the expression by the GPU backend.
                            for (size_t k = 0; k < n.Inputs.size(); ++k)
                            {
                                const auto marker = "$" + n.Inputs[k].Name;
                                size_t     p      = 0;
                                while ((p = i.Code.find(marker, p)) != std::string::npos)
                                {
                                    auto replace = "$" + std::to_string(k);
                                    i.Code.replace(p, marker.size(), replace);
                                    p += replace.size();
                                }
                            }
                            if (Out.Target == SimTarget::GPU)
                            {
                                auto        code    = i.Code;
                                std::string prelude = "#version 450\nlayout(local_size_x=1) in;\nvoid main(){\n";
                                for (size_t k = n.Inputs.size(); k-- > 0;)
                                {
                                    auto variable = "input" + std::to_string(k);
                                    prelude += GType(n.Inputs[k].Kind) + " " + variable + "=" +
                                               GType(n.Inputs[k].Kind) + "(0);\n";
                                    auto   marker = "$" + std::to_string(k);
                                    size_t at     = 0;
                                    while ((at = code.find(marker, at)) != std::string::npos)
                                    {
                                        code.replace(at, marker.size(), variable);
                                        at += variable.size();
                                    }
                                }
                                prelude += i.CustomDeclarations;
                                prelude += body ? code + "\n" + GType(i.Kind) + " result=" + output + ";\n}" :
                                                  GType(i.Kind) + " result=" + code + ";\n}";
                                shaderc::Compiler compiler;
                                auto              compiled =
                                    compiler.CompileGlslToSpv(prelude, shaderc_compute_shader, "CustomGLSL");
                                if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                                    Error(id, guid, output, compiled.GetErrorMessage());
                            }
                        }
                        else if (arity == Arity.end())
                            Error(id, guid, output, "Unknown graph operation: " + n.Op);
                        else
                        {
                            auto count = n.Op == "Make" ? Components(i.Kind) : arity->second;
                            if (i.Args.size() != count)
                                Error(id, guid, output, "Wrong input count for " + n.Op);
                            Validate(i, n, id);
                        }
                        if (n.Op == "Sample")
                        {
                            if (!n.Parameter.empty())
                            {
                                auto p = inputs.find(n.Parameter);
                                if (p != inputs.end() && p->second.Kind == InputKind::DataInterface)
                                    i.Interface = p->second.Interface;
                            }
                            InterfaceRegistry registry;
                            auto              entry = registry.Find(i.Interface.Kind);
                            if (!entry)
                                Error(id, guid, output, "Unknown data interface: " + i.Interface.Kind);
                            else if (!entry->Implemented)
                                Error(id, guid, output, i.Interface.Kind + " not implemented in V1a");
                            if (i.Interface.Values.empty())
                                Error(id, guid, output, "Data interface has no baked samples");
                            if (entry && entry->Result != i.Kind)
                                Error(id, guid, output, "Data interface result type mismatch");
                            if (i.Interface.Kind == "Texture2D" &&
                                (!i.Interface.Width || !i.Interface.Height ||
                                 uint64_t(i.Interface.Width) * i.Interface.Height != i.Interface.Values.size()))
                                Error(id, guid, output, "Invalid Texture2D sample table extent");
                            if ((i.Interface.Kind == "Curve" || i.Interface.Kind == "Gradient") &&
                                i.Interface.Values.size() != i.Interface.Times.size())
                                Error(id, guid, output, "Curve sample time/value count mismatch");
                        }
                        if (i.Op == "Select" && i.Kind == Type::Position && i.Args.size() == 3)
                        {
                            i.Args[1] = PositionTo(i.Args[1], SimulationSpace);
                            i.Args[2] = PositionTo(i.Args[2], SimulationSpace);
                        }
                        if ((i.Op == "TransformPosition" || i.Op == "InverseTransformPosition") && i.Args.size() == 1 &&
                            Out.Code[i.Args[0]].Kind == Type::Position)
                            reg = PositionTo(i.Args[0], i.Op == "TransformPosition" ? Space::World : Space::Local);
                        else
                            reg = Emit(i);
                    }
                    visiting.erase(key);
                    cache[key] = reg;
                    return reg;
                };
                // Node order is the parameter-map execution order. Links are expression dependencies.
                for (const auto& n : m.Nodes)
                    if (n.Op == "Set")
                        visit(n.Guid, n.Outputs.empty() ? "Value" : n.Outputs.front().Name);
                uint32_t result = Constant(Value::Float(0));
                if (expected != Usage::Module)
                    result = visit(m.ResultNode, m.ResultPin);
                // Validate disconnected expressions too, so diagnostics are collected together.
                for (const auto& n : m.Nodes)
                    if (n.Op != "Set")
                        for (const auto& p : n.Outputs)
                            visit(n.Guid, p.Name);
                Calls.pop_back();
                return result;
            }
            void Validate(const Instruction& i, const Node& n, AssetID id)
            {
                const auto t     = [&](size_t k) { return k < i.Args.size() ? Out.Code[i.Args[k]].Kind : Type::Float; };
                const auto error = [&](size_t k, const char* message) {
                    Error(id, n.Guid, k < n.Inputs.size() ? n.Inputs[k].Name : "Value", message);
                };
                if (i.Op == "Select")
                {
                    if (t(0) != Type::Bool)
                        error(0, "Select condition must be bool");
                    if (t(1) != i.Kind)
                        error(1, "Select branch type mismatch");
                    if (t(2) != i.Kind)
                        error(2, "Select branch type mismatch");
                }
                else if (i.Op == "Less" || i.Op == "LessEqual" || i.Op == "Greater" || i.Op == "Equal")
                {
                    if (i.Kind != Type::Bool)
                        error(0, "Comparison output must be bool");
                    if (t(0) != t(1) || Components(t(0)) != 1)
                        error(1, "Comparison requires matching scalar inputs");
                }
                else if (i.Op == "Dot" || i.Op == "Length")
                {
                    if (i.Kind != Type::Float || Components(t(0)) < 2)
                        error(0, "Vector reduction type mismatch");
                    if (i.Op == "Dot" && t(0) != t(1))
                        error(1, "Dot input type mismatch");
                }
                else if (i.Op == "Cross")
                {
                    if (t(0) != Type::Vec3 || t(1) != Type::Vec3 || i.Kind != Type::Vec3)
                        error(0, "Cross requires vec3");
                }
                else if (i.Op == "Make")
                {
                    if (Components(i.Kind) < 2)
                        error(0, "Make requires a vector or color output");
                    for (size_t k = 0; k < i.Args.size(); ++k)
                        if (t(k) != Type::Float)
                            error(k, "Make requires float components");
                }
                else if (i.Op == "Break")
                {
                    if (i.Kind != Type::Float || Components(t(0)) < 2)
                        error(0, "Break requires a vector and float output");
                }
                else if (i.Op == "And" || i.Op == "Or" || i.Op == "Not")
                {
                    for (size_t k = 0; k < i.Args.size(); ++k)
                        if (t(k) != Type::Bool || i.Kind != Type::Bool)
                            error(k, "Boolean operation requires bool");
                }
                else if (i.Op == "ToInt")
                {
                    if (t(0) != Type::Float || i.Kind != Type::Int)
                        error(0, "ToInt requires float -> int");
                }
                else if (i.Op == "ToFloat")
                {
                    if (t(0) != Type::Int || i.Kind != Type::Float)
                        error(0, "ToFloat requires int -> float");
                }
                else if (i.Op == "Noise3D")
                {
                    if (t(0) != Type::Vec3 || i.Kind != Type::Float)
                        error(0, "Noise3D requires vec3 -> float");
                }
                else if (i.Op.find("Transform") != std::string::npos)
                {
                    auto type = i.Op.find("Position") != std::string::npos ? Type::Position : Type::Vec3;
                    if (t(0) != type || i.Kind != type)
                        error(0, "Space transform type mismatch");
                }
                else if (i.Op == "Sample")
                {
                    auto input = i.Interface.Kind == "Texture2D" ? Type::Vec2 : Type::Float;
                    if (t(0) != input)
                        error(0, "Data interface sample coordinate type mismatch");
                }
                else if (i.Op == "Normalize")
                {
                    if (!VectorType(t(0)) || Components(t(0)) < 2 || t(0) != i.Kind)
                        error(0, "Normalize requires a floating vector");
                }
                else if (i.Op == "Splat")
                {
                    if (t(0) != Type::Float || Components(i.Kind) < 2)
                        error(0, "Splat requires float -> vector");
                }
                else if (i.Op == "Random" || i.Op == "RandomVector" || i.Op == "RandomUnitVector")
                {
                    if (!VectorType(i.Kind))
                        error(0, "Random requires floating type");
                }
                else if (i.Kind == Type::Bool ||
                         (i.Kind == Type::Int &&
                          !std::set<std::string> {"Add", "Sub", "Mul", "Div", "Min", "Max", "Abs", "Clamp"}.count(
                              i.Op)))
                    error(0, "Operation requires floating operands");
                else
                    for (size_t k = 0; k < i.Args.size(); ++k)
                    {
                        auto type = t(k);
                        if ((i.Op == "Lerp" && k == 2) || (i.Op == "Clamp" && k > 0))
                        {
                            if (type != i.Kind && type != Type::Float)
                                error(k, "Operand type mismatch");
                        }
                        else if (type != i.Kind)
                            error(k, "Operand type mismatch");
                    }
            }
        };
        std::map<std::string, Parameter> Collect(const std::map<Stage, Stack>&    stages,
                                                 const ModuleResolver&            resolve,
                                                 std::map<std::string, Parameter> known)
        {
            std::set<AssetID>                         visited;
            std::function<void(AssetID, std::string)> scan = [&](AssetID id, std::string scope) {
                if (!visited.insert(id).second)
                    return;
                try
                {
                    const auto m = resolve(id);
                    for (const auto& n : m.Nodes)
                    {
                        if (n.Op == "Set")
                        {
                            auto name = n.Parameter;
                            auto p    = name.find("$Usage");
                            if (p != std::string::npos)
                                name.replace(p, 6, scope);
                            if (!known.count(name))
                                known[name] = {name, n.Kind, Value::Vector({}, n.Kind), {}};
                            else if (!known[name].Default)
                                known[name].Default = Value::Vector({}, known[name].Kind);
                        }
                        if (n.Op == "Call")
                            scan(n.Function, scope + "/" + n.Guid);
                    }
                }
                catch (const std::exception&)
                {}
                visited.erase(id);
            };
            for (const auto& [s, stack] : stages)
                for (const auto& use : stack)
                    if (use.Enabled)
                        scan(use.Script, use.Guid);
            return known;
        }
        Program StageProgram(Stage                            stage,
                             const Stack&                     stack,
                             SimTarget                        target,
                             const ModuleResolver&            resolve,
                             std::map<std::string, Parameter> known,
                             std::vector<Diagnostic>&         errors,
                             AssetID                          asset,
                             Space                            simulationSpace = Space::Local)
        {
            Compiler c {resolve, std::move(known), errors, {}, {}, asset};
            c.SimulationSpace = simulationSpace;
            c.Out.StageKind   = stage;
            c.Out.Target      = target;
            for (const auto& use : stack)
                if (use.Enabled)
                    c.Inline(use.Script, Usage::Module, use.Inputs, use.Guid);
            // Validate the entire graph, then retain only effects and their expression dependencies.
            std::vector<bool>             live(c.Out.Code.size(), false);
            std::function<void(uint32_t)> mark = [&](uint32_t index) {
                if (live[index])
                    return;
                live[index] = true;
                for (auto argument : c.Out.Code[index].Args)
                    mark(argument);
            };
            for (uint32_t index = 0; index < c.Out.Code.size(); ++index)
                if (c.Out.Code[index].Op == "Set")
                    mark(index);
            std::vector<uint32_t>    registers(c.Out.Code.size()), parameters(c.Out.Parameters.size(), UINT32_MAX);
            std::vector<Instruction> code;
            std::vector<Parameter>   layout;
            c.Out.Reads.clear();
            c.Out.Writes.clear();
            for (uint32_t index = 0; index < c.Out.Code.size(); ++index)
                if (live[index])
                {
                    auto instruction = c.Out.Code[index];
                    for (auto& argument : instruction.Args)
                        argument = registers[argument];
                    if (instruction.Op == "Get" || instruction.Op == "Set")
                    {
                        auto original = instruction.Parameter;
                        if (parameters[original] == UINT32_MAX)
                        {
                            parameters[original] = uint32_t(layout.size());
                            layout.push_back(c.Out.Parameters[original]);
                        }
                        instruction.Parameter = parameters[original];
                        (instruction.Op == "Get" ? c.Out.Reads : c.Out.Writes)
                            .insert(layout[instruction.Parameter].Name);
                    }
                    if (instruction.Op == "OwnerMatrixColumn")
                        c.Out.Reads.insert("Engine.Owner.LocalToWorld");
                    registers[index] = uint32_t(code.size());
                    code.push_back(std::move(instruction));
                }
            c.Out.Code            = std::move(code);
            c.Out.Parameters      = std::move(layout);
            uint32_t valuesOffset = 0;
            for (uint32_t index = 0; index < c.Out.Code.size(); ++index)
                if (c.Out.Code[index].Op == "Sample")
                {
                    auto count                  = uint32_t(c.Out.Code[index].Interface.Values.size());
                    c.Out.Code[index].Parameter = uint32_t(c.Out.Interfaces.size());
                    c.Out.Interfaces.push_back({index, 6, valuesOffset, count});
                    valuesOffset += count;
                }
            return c.Out;
        }
        std::string F(float x)
        {
            std::ostringstream o;
            o << std::setprecision(9) << x;
            auto s = o.str();
            if (s.find_first_of(".eE") == std::string::npos)
                s += ".0";
            return s;
        }
        std::string GType(Type t)
        {
            if (t == Type::Color || t == Type::Quat)
                return "vec4";
            if (t == Type::Position)
                return "vec3";
            return TypeName(t);
        }
        std::string Literal(Value v)
        {
            if (v.Kind == Type::Int)
                return v.Integer == INT32_MIN ? "(-2147483647-1)" : std::to_string(v.Integer);
            if (v.Kind == Type::Bool)
                return v.Integer ? "true" : "false";
            if (v.Kind == Type::Float)
                return F(v.Data.x);
            std::string s = GType(v.Kind) + "(";
            for (uint32_t k = 0; k < Components(v.Kind); ++k)
            {
                if (k)
                    s += ",";
                s += F(v.Data[k]);
            }
            return s + ")";
        }
    } // namespace
    CompileResult
    Compile(const System& system, const EmitterResolver& emitters, const ModuleResolver& modules, AssetID asset)
    {
        CompileResult r;
        r.Product.Settings = system;
        auto known         = BuiltinParameters();
        for (const auto& p : system.UserParameters)
            known[p.Name] = p;
        known = Collect(system.Stages, modules, std::move(known));
        for (const auto& [s, stack] : system.Stages)
            r.Product.Programs[s] = StageProgram(s, stack, SimTarget::CPU, modules, known, r.Errors, asset);
        for (const auto& h : system.Emitters)
            if (h.Enabled)
            {
                CompiledEmitter ce;
                try
                {
                    auto e      = h.Inline ? *h.Inline : emitters(h.Reference);
                    ce.Settings = MergeEmitter(e, emitters);
                    ApplyOverrides(ce.Settings, h.Overrides);
                    ce.Settings.Name = h.Name;
                }
                catch (const std::exception& e)
                {
                    r.Errors.push_back({asset, h.Guid, "", e.what()});
                    continue;
                }
                auto k = known;
                for (const auto& p : ce.Settings.Attributes)
                    k[p.Name] = p;
                k = Collect(ce.Settings.Stages, modules, std::move(k));
                std::set<std::string> attributes;
                for (const auto& [s, stack] : ce.Settings.Stages)
                {
                    if (s >= Stage::EventHandler)
                    {
                        if (!stack.empty())
                            r.Errors.push_back(
                                {asset, h.Guid, "", std::string(StageName(s)) + " not implemented in V1a"});
                        continue;
                    }
                    auto p = StageProgram(s,
                                          stack,
                                          s >= Stage::ParticleSpawn ? ce.Settings.Target : SimTarget::CPU,
                                          modules,
                                          k,
                                          r.Errors,
                                          asset,
                                          ce.Settings.SimulationSpace);
                    for (const auto& a : p.Reads)
                        if (a.rfind("Particles.", 0) == 0)
                            attributes.insert(a);
                    for (const auto& a : p.Writes)
                        if (a.rfind("Particles.", 0) == 0)
                            attributes.insert(a);
                    ce.Programs[s] = std::move(p);
                }
                for (const auto& renderer : ce.Settings.Renderers)
                {
                    if (renderer.Kind != RendererKind::Sprite)
                        r.Errors.push_back({asset, renderer.Guid, "", "Renderer kind not implemented in V1a"});
                    if (renderer.Sorting > Sort::EmitterOrder)
                        r.Errors.push_back({asset, renderer.Guid, "Sort", "GPU sorting not implemented in V1a"});
                    const std::map<std::string, Type> bindingTypes = {{"Position", Type::Position},
                                                                      {"Color", Type::Color},
                                                                      {"SpriteSize", Type::Vec2},
                                                                      {"SpriteRotation", Type::Float},
                                                                      {"Velocity", Type::Vec3},
                                                                      {"SubImageIndex", Type::Float},
                                                                      {"DynamicMaterialParameter", Type::Vec4}};
                    for (const auto& [binding, type] : bindingTypes)
                    {
                        auto name = renderer.Bindings.find(binding);
                        if (name == renderer.Bindings.end())
                            r.Errors.push_back({asset, renderer.Guid, binding, "Missing sprite renderer binding"});
                        else if (auto parameter = k.find(name->second);
                                 parameter != k.end() &&
                                 (parameter->second.Kind != type || name->second.rfind("Particles.", 0) != 0))
                            r.Errors.push_back(
                                {asset, renderer.Guid, binding, "Sprite renderer binding type or namespace mismatch"});
                    }
                    for (const auto& [binding, name] : renderer.Bindings)
                    {
                        auto at = k.find(name);
                        if (at == k.end())
                            r.Errors.push_back({asset, renderer.Guid, binding, "Unknown renderer parameter: " + name});
                        else if (!at->second.Default)
                            r.Errors.push_back(
                                {asset, renderer.Guid, binding, "Read of unset renderer parameter: " + name});
                        else if (name.rfind("Particles.", 0) == 0)
                            attributes.insert(name);
                    }
                }
                // Lifecycle data is consumed by the allocator and update scheduler.
                attributes.insert("Particles.UniqueID");
                attributes.insert("Particles.Alive");
                for (const auto& a : attributes)
                {
                    auto p = k.find(a);
                    if (p != k.end())
                        ce.Attributes.push_back({a,
                                                 p->second.Kind,
                                                 p->second.Default.value_or(Value::Vector({}, p->second.Kind)),
                                                 uint32_t(ce.Attributes.size())});
                }
                for (auto stage : {Stage::ParticleSpawn, Stage::ParticleUpdate})
                    if (!ce.Programs.count(stage))
                    {
                        Program p;
                        p.StageKind        = stage;
                        p.Target           = ce.Settings.Target;
                        ce.Programs[stage] = std::move(p);
                    }
                if (r.Errors.empty())
                    for (auto& [s, p] : ce.Programs)
                        if (p.Target == SimTarget::GPU)
                            try
                            {
                                GenerateGpuProgram(p, ce.Attributes);
                            }
                            catch (const std::exception& e)
                            {
                                r.Errors.push_back({asset, h.Guid, "", e.what()});
                            }
                r.Product.Emitters.push_back(std::move(ce));
            }
        return r;
    }
    std::string FormatDiagnostics(const std::vector<Diagnostic>& errors)
    {
        std::string s;
        for (const auto& e : errors)
            s += e.Asset.ToString() + " node=" + e.Node + " pin=" + e.Pin + ": " + e.Message + "\n";
        return s;
    }
    Value Evaluate(const Instruction& i, const std::vector<Value>& r, const ExecutionContext& c)
    {
        Value      out = Value::Vector({}, i.Kind);
        const auto a   = [&](size_t k) -> Value { return k < i.Args.size() ? r.at(i.Args[k]) : Value::Float(0); };
        const auto x = a(0), y = a(1), z = a(2);
        const auto op = i.Op;
        if (op == "OwnerMatrixColumn")
            return Value::Vector(c.LocalToWorld[i.Parameter], Type::Vec4);
        if (op == "Constant")
            return i.Literal;
        if (op == "Select")
            return x.Integer ? y : z;
        if (op == "ToInt")
        {
            auto value  = x.Data.x;
            out.Integer = std::isnan(value)      ? 0 :
                          value >= 2147483648.f  ? INT32_MAX :
                          value <= -2147483648.f ? INT32_MIN :
                                                   int32_t(std::floor(value));
            return out;
        }
        if (op == "ToFloat")
        {
            out.Data = glm::vec4(float(x.Integer));
            return out;
        }
        if (op == "And" || op == "Or" || op == "Not")
        {
            out.Integer = op == "And" ? (x.Integer && y.Integer) : op == "Or" ? (x.Integer || y.Integer) : !x.Integer;
            return out;
        }
        if (op == "Less" || op == "LessEqual" || op == "Greater" || op == "Equal")
        {
            if (x.Kind == Type::Int || x.Kind == Type::Bool)
                out.Integer = op == "Less"      ? x.Integer < y.Integer :
                              op == "LessEqual" ? x.Integer <= y.Integer :
                              op == "Greater"   ? x.Integer > y.Integer :
                                                  x.Integer == y.Integer;
            else
                out.Integer = op == "Less"      ? x.Data.x < y.Data.x :
                              op == "LessEqual" ? x.Data.x <= y.Data.x :
                              op == "Greater"   ? x.Data.x > y.Data.x :
                                                  x.Data.x == y.Data.x;
            return out;
        }
        if (i.Kind == Type::Int)
        {
            auto u = uint32_t(x.Integer), v = uint32_t(y.Integer);
            if (op == "Add")
                out.Integer = int32_t(u + v);
            else if (op == "Sub")
                out.Integer = int32_t(u - v);
            else if (op == "Mul")
                out.Integer = int32_t(u * v);
            else if (op == "Div")
                out.Integer = y.Integer ? int32_t(int64_t(x.Integer) / y.Integer) : 0;
            else if (op == "Min")
                out.Integer = std::min(x.Integer, y.Integer);
            else if (op == "Max")
                out.Integer = std::max(x.Integer, y.Integer);
            else if (op == "Abs")
                out.Integer = x.Integer < 0 ? int32_t(0u - uint32_t(x.Integer)) : x.Integer;
            else if (op == "Clamp")
                out.Integer = std::min(std::max(x.Integer, y.Integer), z.Integer);
            return out;
        }
        if (op == "Splat")
        {
            out.Data = glm::vec4(x.Data.x);
            return out;
        }
        if (op == "Make")
        {
            for (uint32_t k = 0; k < Components(i.Kind); ++k)
                out.Data[k] = a(k).Data.x;
            return out;
        }
        if (op == "Break")
        {
            auto at  = i.Output == "Y" || i.Output == "G" ? 1 :
                       i.Output == "Z" || i.Output == "B" ? 2 :
                       i.Output == "W" || i.Output == "A" ? 3 :
                                                            0;
            out.Data = glm::vec4(x.Data[at]);
            return out;
        }
        if (op == "Dot" || op == "Length" || op == "Normalize")
        {
            float d = 0;
            for (uint32_t k = 0; k < Components(x.Kind); ++k)
                d += x.Data[k] * (op == "Dot" ? y.Data[k] : x.Data[k]);
            if (op == "Normalize")
                out.Data = d > 1e-20f ? x.Data / std::sqrt(d) : glm::vec4(0);
            else
                out.Data = glm::vec4(op == "Dot" ? d : std::sqrt(d));
            return out;
        }
        if (op == "Cross")
        {
            out.Data = glm::vec4(glm::cross(glm::vec3(x.Data), glm::vec3(y.Data)), 0);
            return out;
        }
        if (op.find("Transform") != std::string::npos)
        {
            auto m   = op.rfind("Inverse", 0) == 0 ? glm::inverse(c.LocalToWorld) : c.LocalToWorld;
            out.Data = glm::vec4(
                glm::vec3(m * glm::vec4(glm::vec3(x.Data), op.find("Position") != std::string::npos ? 1 : 0)), 0);
            return out;
        }
        const auto random = [&](uint32_t lane) {
            uint32_t h = Hash(c.Seed ^ Hash(c.EmitterIndex) ^ Hash(c.UniqueID) ^ i.RandomKey ^ Hash(lane));
            return float(h >> 8) * (1.f / 16777216.f);
        };
        if (op == "RandomUnitVector")
        {
            float az = random(0) * 6.28318530718f, zv = random(1) * 2 - 1, rad = std::sqrt(std::max(0.f, 1 - zv * zv));
            out.Data = {rad * std::cos(az), rad * std::sin(az), zv, 0};
            return out;
        }
        if (op == "Sample")
        {
            const auto& b = i.Interface;
            if (b.Values.empty())
                return out;
            if (b.Kind == "Texture2D")
            {
                auto u   = std::min(b.Width - 1, uint32_t(glm::clamp(x.Data.x, 0.f, 1.f) * float(b.Width)));
                auto v   = std::min(b.Height - 1, uint32_t(glm::clamp(x.Data.y, 0.f, 1.f) * float(b.Height)));
                out.Data = b.Values.at(v * b.Width + u);
            }
            else
            {
                auto at = std::upper_bound(b.Times.begin(), b.Times.end(), x.Data.x);
                if (at == b.Times.begin())
                    out.Data = b.Values.front();
                else if (at == b.Times.end())
                    out.Data = b.Values.back();
                else
                {
                    size_t k = size_t(at - b.Times.begin());
                    float  f = (x.Data.x - b.Times[k - 1]) / (b.Times[k] - b.Times[k - 1]);
                    out.Data = glm::mix(b.Values[k - 1], b.Values[k], f);
                }
            }
            return out;
        }
        if (op == "Noise3D")
        {
            auto base = glm::ivec3(glm::floor(glm::vec3(x.Data)));
            auto f    = glm::fract(glm::vec3(x.Data));
            f         = f * f * (3.f - 2.f * f);
            float v   = 0;
            for (int zc = 0; zc < 2; ++zc)
                for (int yc = 0; yc < 2; ++yc)
                    for (int xc = 0; xc < 2; ++xc)
                    {
                        auto  p = base + glm::ivec3(xc, yc, zc);
                        float n = float(Hash(uint32_t(p.x) ^ Hash(uint32_t(p.y)) ^ Hash(uint32_t(p.z))) >> 8) *
                                  (1.f / 16777216.f);
                        v += n * (xc ? f.x : 1 - f.x) * (yc ? f.y : 1 - f.y) * (zc ? f.z : 1 - f.z);
                    }
            out.Data = glm::vec4(v);
            return out;
        }
        for (uint32_t k = 0; k < Components(i.Kind); ++k)
        {
            float av = x.Data[x.Kind == Type::Float ? 0 : k], bv = y.Data[y.Kind == Type::Float ? 0 : k],
                  cv = z.Data[z.Kind == Type::Float ? 0 : k], v = 0;
            if (op == "Add")
                v = av + bv;
            else if (op == "Sub")
                v = av - bv;
            else if (op == "Mul")
                v = av * bv;
            else if (op == "Div")
                v = bv != 0 ? av / bv : 0;
            else if (op == "Lerp")
                v = av + (bv - av) * cv;
            else if (op == "Clamp")
                v = glm::clamp(av, bv, cv);
            else if (op == "Saturate")
                v = glm::clamp(av, 0.f, 1.f);
            else if (op == "Min")
                v = std::min(av, bv);
            else if (op == "Max")
                v = std::max(av, bv);
            else if (op == "Abs")
                v = std::abs(av);
            else if (op == "Floor")
                v = std::floor(av);
            else if (op == "Frac")
                v = av - std::floor(av);
            else if (op == "Pow")
                v = std::pow(av, bv);
            else if (op == "Sin")
                v = std::sin(av);
            else if (op == "Cos")
                v = std::cos(av);
            else if (op == "Step")
                v = bv >= av ? 1 : 0;
            else if (op == "Smoothstep")
            {
                float t = av != bv ? glm::clamp((cv - av) / (bv - av), 0.f, 1.f) : 0;
                v       = t * t * (3 - 2 * t);
            }
            else if (op == "Random" || op == "RandomVector")
                v = random(k);
            else if (op == "RandomRange")
                v = av + (bv - av) * random(k);
            out.Data[k] = v;
        }
        return out;
    }
    void Execute(const Program& p, ParameterMap& values, const ExecutionContext& c)
    {
        std::vector<Value> r;
        r.reserve(p.Code.size());
        for (const auto& i : p.Code)
        {
            if (i.Op == "Get")
            {
                const auto& param = p.Parameters.at(i.Parameter);
                auto        at    = values.find(param.Name);
                if (at == values.end())
                {
                    if (!param.Default)
                        throw std::logic_error("Unset VFX runtime parameter: " + param.Name);
                    r.push_back(*param.Default);
                }
                else
                    r.push_back(at->second);
            }
            else if (i.Op == "Set")
            {
                auto v                                    = r.at(i.Args.at(0));
                values[p.Parameters.at(i.Parameter).Name] = v;
                r.push_back(v);
            }
            else
                r.push_back(Evaluate(i, r, c));
        }
    }
    void GenerateGpuProgram(Program& p, const std::vector<Attribute>& attributes)
    {
        const auto attribute = [&](const std::string& n) -> const Attribute* {
            for (const auto& a : attributes)
                if (a.Name == n)
                    return &a;
            return nullptr;
        };
        const auto decode = [](Type t, std::string x) {
            if (t == Type::Int)
                return "int(" + x + ".x)";
            if (t == Type::Bool)
                return "(" + x + ".x!=0u)";
            auto v = "uintBitsToFloat(" + x + ")";
            return v + (Components(t) == 1 ? ".x" : Components(t) == 2 ? ".xy" : Components(t) == 3 ? ".xyz" : "");
        };
        const auto encode = [](Type t, std::string x) {
            if (t == Type::Int)
                return "uvec4(uint(" + x + "),0u,0u,0u)";
            if (t == Type::Bool)
                return "uvec4(" + x + "?1u:0u,0u,0u,0u)";
            return "floatBitsToUint(vec4(" + x +
                   (Components(t) == 2 ? ",0.0,0.0" :
                    Components(t) == 3 ? ",0.0" :
                                         "") +
                   "))";
        };
        std::string s     = R"glsl(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Attributes { uvec4 attributes[]; };
layout(set=0,binding=1,std430) readonly buffer Parameters { uvec4 parameters[]; };
layout(set=0,binding=2,std430) buffer Flags { uint flags[]; };
layout(set=0,binding=3,std430) buffer FreeList { uint freeList[]; };
layout(set=0,binding=4,std430) buffer Counters { uint counters[]; };
layout(set=0,binding=5,std430) readonly buffer AliveList { uint aliveList[]; };
layout(set=0,binding=6,std430) readonly buffer DataInterfaces {vec4 diValues[];};
layout(push_constant) uniform Push {uint capacity;uint seed;uint emitter;uint requested;uint worldSpace;uint kill;} pc;
uint hashV(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float randomV(uint id,uint key,uint lane){return float(hashV(pc.seed^hashV(pc.emitter)^hashV(id)^key^hashV(lane))>>8)*(1.0/16777216.0);}
float noiseV(vec3 x){ivec3 b=ivec3(floor(x));vec3 f=fract(x);f=f*f*(3.0-2.0*f);float v=0;
for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int a=0;a<2;++a){ivec3 p=b+ivec3(a,y,z);float n=float(hashV(uint(p.x)^hashV(uint(p.y))^hashV(uint(p.z)))>>8)*(1.0/16777216.0);v+=n*(a!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y)*(z!=0?f.z:1-f.z);}return v;}
void main(){uint invocation=gl_GlobalInvocationID.x;uint slot;uint uniqueID;
)glsl";
        const bool  spawn = p.StageKind == Stage::ParticleSpawn;
        if (spawn)
            s += "if(invocation>=counters[2])return;slot=freeList[counters[4]+invocation];uniqueID=counters[5]+"
                 "invocation;flags[slot]=1u;\n";
        else
            s += "if(invocation>=counters[1])return;slot=aliveList[invocation];uniqueID=uint(" +
                 decode(Type::Int,
                        "attributes[" + std::to_string(attribute("Particles.UniqueID")->Offset) +
                            "u*pc.capacity+slot]") +
                 ");\n";
        s += "mat4 owner=mat4(";
        for (int k = 0; k < 4; ++k)
        {
            if (k)
                s += ",";
            s += "uintBitsToFloat(parameters[" + std::to_string(p.Parameters.size() + k) + "])";
        }
        s += ");\n";
        for (const auto& a : attributes)
        {
            s += GType(a.Kind) + " a" + std::to_string(a.Offset) + "=" +
                 (spawn ? Literal(a.Default) :
                          decode(a.Kind, "attributes[" + std::to_string(a.Offset) + "u*pc.capacity+slot]")) +
                 ";\n";
        }
        if (spawn)
            for (const auto& entry : attributes)
                if (entry.Kind == Type::Position)
                {
                    auto a = &entry;
                    s += "if(pc.worldSpace!=0u)a" + std::to_string(a->Offset) + "=(owner*vec4(a" +
                         std::to_string(a->Offset) + ",1)).xyz;\n";
                }
        s += "a" + std::to_string(attribute("Particles.UniqueID")->Offset) + "=int(uniqueID);\n";
        s += "a" + std::to_string(attribute("Particles.Alive")->Offset) + "=true;\n";
        for (uint32_t k = 0; k < p.Parameters.size(); ++k)
        {
            const auto& v = p.Parameters[k];
            auto        a = attribute(v.Name);
            s += GType(v.Kind) + " p" + std::to_string(k) + "=" +
                 (a ? "a" + std::to_string(a->Offset) : decode(v.Kind, "parameters[" + std::to_string(k) + "]")) +
                 ";\n";
        }
        std::string customHelpers;
        for (uint32_t k = 0; k < p.Code.size(); ++k)
        {
            const auto& i   = p.Code[k];
            const auto  arg = [&](size_t a) { return a < i.Args.size() ? "r" + std::to_string(i.Args[a]) : "0.0"; };
            std::string e;
            auto        a = arg(0), b = arg(1), c = arg(2), op = i.Op;
            if (op == "OwnerMatrixColumn")
                e = "owner[" + std::to_string(i.Parameter) + "]";
            else if (op == "Get")
                e = "p" + std::to_string(i.Parameter);
            else if (op == "Set")
                e = "(p" + std::to_string(i.Parameter) + "=" + a + ")";
            else if (op == "Constant")
                e = Literal(i.Literal);
            else if (op == "Add" || op == "Sub" || op == "Mul" || op == "Div")
            {
                const auto symbol = op == "Add" ? "+" : op == "Sub" ? "-" : op == "Mul" ? "*" : "/";
                if (op == "Div" && Components(i.Kind) == 1)
                    e = "(" + b + "==" + (i.Kind == Type::Int ? "0" : "0.0") + "?" +
                        (i.Kind == Type::Int ? "0" : "0.0") + ":" + a + "/" + b + ")";
                else if (op == "Div" && Components(i.Kind) > 1)
                {
                    e = GType(i.Kind) + "(";
                    for (uint32_t component = 0; component < Components(i.Kind); ++component)
                    {
                        if (component)
                            e += ",";
                        std::string lane(1, "xyzw"[component]);
                        e += "(" + b + "." + lane + "==0.0?0.0:" + a + "." + lane + "/" + b + "." + lane + ")";
                    }
                    e += ")";
                }
                else
                    e = "(" + a + symbol + b + ")";
            }
            else if (op == "Less" || op == "LessEqual" || op == "Greater" || op == "Equal")
                e = "(" + a + (op == "Less" ? "<" : op == "LessEqual" ? "<=" : op == "Greater" ? ">" : "==") + b + ")";
            else if (op == "And" || op == "Or")
                e = "(" + a + (op == "And" ? "&&" : "||") + b + ")";
            else if (op == "Not")
                e = "(!" + a + ")";
            else if (op == "Select")
                e = "(" + a + "?" + b + ":" + c + ")";
            else if (op == "ToInt")
                e = "(isnan(" + a + ")?0:" + a + ">=2147483648.0?2147483647:" + a +
                    "<=-2147483648.0?(-2147483647-1):int(floor(" + a + ")))";
            else if (op == "ToFloat")
                e = "float(" + a + ")";
            else if (op == "Splat")
                e = GType(i.Kind) + "(" + a + ")";
            else if (op == "Make")
            {
                e = GType(i.Kind) + "(";
                for (size_t j = 0; j < i.Args.size(); ++j)
                {
                    if (j)
                        e += ",";
                    e += arg(j);
                }
                e += ")";
            }
            else if (op == "Break")
                e = a + "." +
                    (i.Output == "Y" || i.Output == "G" ? "y" :
                     i.Output == "Z" || i.Output == "B" ? "z" :
                     i.Output == "W" || i.Output == "A" ? "w" :
                                                          "x");
            else if (op == "Saturate")
                e = "clamp(" + a + "," + GType(i.Kind) + "(0)," + GType(i.Kind) + "(1))";
            else if (op == "Normalize")
                e = "(dot(" + a + "," + a + ")>1e-20?normalize(" + a + "):" + GType(i.Kind) + "(0))";
            else if (op == "Random" || op == "RandomVector" || op == "RandomRange" || op == "RandomUnitVector")
            {
                const auto random = [&](uint32_t lane) {
                    return "randomV(uniqueID," + std::to_string(i.RandomKey) + "u," + std::to_string(lane) + "u)";
                };
                if (op == "RandomUnitVector")
                {
                    auto az = random(0) + "*6.28318530718", zv = "(" + random(1) + "*2.0-1.0)",
                         rad = "sqrt(max(0.0,1.0-" + zv + "*" + zv + "))";
                    e        = "vec3(" + rad + "*cos(" + az + ")," + rad + "*sin(" + az + ")," + zv + ")";
                }
                else
                {
                    e = GType(i.Kind) + "(";
                    for (uint32_t j = 0; j < Components(i.Kind); ++j)
                    {
                        if (j)
                            e += ",";
                        e += random(j);
                    }
                    e += ")";
                    if (op == "RandomRange")
                        e = "(" + a + "+(" + b + "-" + a + ")*" + e + ")";
                }
            }
            else if (op == "Noise3D")
                e = "noiseV(" + a + ")";
            else if (op.find("Transform") != std::string::npos)
                e = "(" + std::string(op.rfind("Inverse", 0) == 0 ? "inverse(owner)" : "owner") + "*vec4(" + a + "," +
                    (op.find("Position") != std::string::npos ? "1" : "0") + ")).xyz";
            else if (op == "CustomGLSL")
            {
                auto code = i.Code;
                for (size_t j = i.Args.size(); j-- > 0;)
                {
                    auto   marker = "$" + std::to_string(j);
                    size_t at     = 0;
                    while ((at = code.find(marker, at)) != std::string::npos)
                    {
                        code.replace(at, marker.size(), arg(j));
                        at += arg(j).size();
                    }
                }
                if (i.CustomDeclarations.empty())
                    e = code;
                else
                {
                    auto name = "custom" + std::to_string(k);
                    customHelpers += GType(i.Kind) + " " + name + "(";
                    e = name + "(";
                    std::set<uint32_t> arguments;
                    bool               first = true;
                    for (size_t j = 0; j < i.Args.size(); ++j)
                        if (arguments.insert(i.Args[j]).second)
                        {
                            if (!first)
                            {
                                customHelpers += ",";
                                e += ",";
                            }
                            first = false;
                            customHelpers += GType(p.Code[i.Args[j]].Kind) + " " + arg(j);
                            e += arg(j);
                        }
                    customHelpers += "){\n" + i.CustomDeclarations + code + "\nreturn " + i.Output + ";\n}\n";
                    e += ")";
                }
            }
            else if (op == "Sample")
            {
                const auto& di    = i.Interface;
                const auto& table = p.Interfaces.at(i.Parameter);
                auto        value = [&](std::string index) {
                    std::string word = "diValues[" + std::to_string(table.ValueOffset) + "+" + index + "]";
                    return word + (Components(i.Kind) == 1 ? ".x" :
                                   Components(i.Kind) == 2 ? ".xy" :
                                   Components(i.Kind) == 3 ? ".xyz" :
                                                             "");
                };
                if (di.Kind == "Texture2D")
                {
                    e = value("min(" + std::to_string(di.Height - 1) + ",int(clamp(" + a + ".y,0.0,1.0)*" +
                              F(float(di.Height)) + "))*" + std::to_string(di.Width) + "+min(" +
                              std::to_string(di.Width - 1) + ",int(clamp(" + a + ".x,0.0,1.0)*" + F(float(di.Width)) +
                              "))");
                }
                else
                {
                    e = value(std::to_string(di.Values.size() - 1));
                    for (size_t j = di.Values.size() - 1; j > 0; --j)
                        e = "(" + a + "<" + F(di.Times[j]) + "?mix(" + value(std::to_string(j - 1)) + "," +
                            value(std::to_string(j)) + ",clamp((" + a + "-" + F(di.Times[j - 1]) + ")/" +
                            F(di.Times[j] - di.Times[j - 1]) + ",0.0,1.0)):" + e + ")";
                }
            }
            else
            {
                std::string fn = op == "Lerp" ? "mix" : op == "Frac" ? "fract" : op;
                std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char x) { return char(std::tolower(x)); });
                e = fn + "(";
                for (size_t j = 0; j < i.Args.size(); ++j)
                {
                    if (j)
                        e += ",";
                    e += arg(j);
                }
                e += ")";
            }
            s += GType(i.Kind) + " r" + std::to_string(k) + "=" + e + ";\n";
        }
        for (uint32_t k = 0; k < p.Parameters.size(); ++k)
        {
            const auto& v = p.Parameters[k];
            if (auto a = attribute(v.Name); a && p.Writes.count(v.Name))
                s += "a" + std::to_string(a->Offset) + "=p" + std::to_string(k) + ";\n";
        }
        if (spawn)
            if (auto a = attribute("Particles.Velocity"))
                s += "if(pc.worldSpace!=0u)a" + std::to_string(a->Offset) + "=mat3(owner)*a" +
                     std::to_string(a->Offset) + ";\n";
        s += "if(pc.kill!=0u)a" + std::to_string(attribute("Particles.Alive")->Offset) + "=false;\n";
        for (const auto& a : attributes)
            s += "attributes[" + std::to_string(a.Offset) +
                 "u*pc.capacity+slot]=" + encode(a.Kind, "a" + std::to_string(a.Offset)) + ";\n";
        auto alive = "a" + std::to_string(attribute("Particles.Alive")->Offset);
        s += "if(!" + alive + "){flags[slot]=0u;uint at=atomicAdd(counters[0],1u);freeList[at]=slot;}\n}\n";
        s.insert(s.find("void main()"), customHelpers);
        p.GLSL = std::move(s);
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        auto result = compiler.CompileGlslToSpv(p.GLSL, shaderc_compute_shader, "VfxStage.comp", options);
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
            throw std::invalid_argument(result.GetErrorMessage());
        p.SpirV.assign(result.cbegin(), result.cend());
    }
    namespace
    {
        YAML::Node ProgramsNode(const std::map<Stage, Program>& programs)
        {
            YAML::Node out(YAML::NodeType::Sequence);
            for (const auto& [stage, p] : programs)
            {
                YAML::Node n;
                n["Stage"]             = int(stage);
                n["Target"]            = int(p.Target);
                n["GLSL"]              = p.GLSL;
                n["SpirV"]             = p.SpirV;
                n["InterfaceBindings"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& binding : p.Interfaces)
                {
                    YAML::Node b;
                    b["Instruction"] = binding.Instruction;
                    b["Binding"]     = binding.Binding;
                    b["ValueOffset"] = binding.ValueOffset;
                    b["ValueCount"]  = binding.ValueCount;
                    n["InterfaceBindings"].push_back(b);
                }
                n["Parameters"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& v : p.Parameters)
                {
                    YAML::Node a;
                    a["Name"] = v.Name;
                    a["Type"] = int(v.Kind);
                    if (v.Default)
                        a["Default"] = EncodeValue(*v.Default);
                    n["Parameters"].push_back(a);
                }
                n["Reads"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& v : p.Reads)
                    n["Reads"].push_back(v);
                n["Writes"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& v : p.Writes)
                    n["Writes"].push_back(v);
                n["Code"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& i : p.Code)
                {
                    YAML::Node a;
                    a["Op"]                 = i.Op;
                    a["Type"]               = int(i.Kind);
                    a["Args"]               = i.Args;
                    a["Literal"]            = EncodeValue(i.Literal);
                    a["PositionSpace"]      = int(i.PositionSpace);
                    a["Parameter"]          = i.Parameter;
                    a["RandomKey"]          = i.RandomKey;
                    a["Interface"]          = EncodeInterface(i.Interface);
                    a["Code"]               = i.Code;
                    a["Output"]             = i.Output;
                    a["CustomDeclarations"] = i.CustomDeclarations;
                    n["Code"].push_back(a);
                }
                out.push_back(n);
            }
            return out;
        }
        std::map<Stage, Program> ReadPrograms(const YAML::Node& in)
        {
            std::map<Stage, Program> out;
            for (const auto& n : in)
            {
                Program p;
                p.StageKind = Stage(n["Stage"].as<int>());
                p.Target    = SimTarget(n["Target"].as<int>());
                p.GLSL      = n["GLSL"].as<std::string>();
                p.SpirV     = n["SpirV"].as<std::vector<uint32_t>>();
                if (p.StageKind > Stage::ParticleUpdate || p.Target > SimTarget::GPU)
                    throw std::invalid_argument("Invalid VFX product stage");
                for (const auto& a : n["Parameters"])
                {
                    Parameter v;
                    v.Name = a["Name"].as<std::string>();
                    auto t = a["Type"].as<int>();
                    if (t < 0 || t > int(Type::Position))
                        throw std::invalid_argument("Invalid VFX product type");
                    v.Kind = Type(t);
                    if (a["Default"])
                        v.Default = DecodeValue(a["Default"]);
                    p.Parameters.push_back(v);
                }
                for (const auto& a : n["Reads"])
                    p.Reads.insert(a.as<std::string>());
                for (const auto& a : n["Writes"])
                    p.Writes.insert(a.as<std::string>());
                for (const auto& a : n["Code"])
                {
                    Instruction i;
                    i.Op   = a["Op"].as<std::string>();
                    auto t = a["Type"].as<int>();
                    if (t < 0 || t > int(Type::Position))
                        throw std::invalid_argument("Invalid VFX bytecode type");
                    i.Kind = Type(t);
                    i.Args = a["Args"].as<std::vector<uint32_t>>();
                    for (auto r : i.Args)
                        if (r >= p.Code.size())
                            throw std::invalid_argument("Invalid VFX bytecode register");
                    i.Literal       = DecodeValue(a["Literal"]);
                    i.PositionSpace = Space(a["PositionSpace"].as<int>(0));
                    i.Parameter     = a["Parameter"].as<uint32_t>();
                    if ((i.Op == "Get" || i.Op == "Set") && i.Parameter >= p.Parameters.size())
                        throw std::invalid_argument("Invalid VFX bytecode parameter");
                    i.RandomKey          = a["RandomKey"].as<uint32_t>();
                    i.Interface          = DecodeInterface(a["Interface"]);
                    i.Code               = a["Code"].as<std::string>();
                    i.Output             = a["Output"].as<std::string>();
                    i.CustomDeclarations = a["CustomDeclarations"].as<std::string>("");
                    p.Code.push_back(i);
                }
                uint32_t offset = 0;
                for (const auto& b : n["InterfaceBindings"])
                {
                    InterfaceLayout binding {b["Instruction"].as<uint32_t>(),
                                             b["Binding"].as<uint32_t>(),
                                             b["ValueOffset"].as<uint32_t>(),
                                             b["ValueCount"].as<uint32_t>()};
                    if (binding.Instruction >= p.Code.size() || p.Code[binding.Instruction].Op != "Sample" ||
                        binding.Binding != 6 || binding.ValueOffset != offset ||
                        binding.ValueCount != p.Code[binding.Instruction].Interface.Values.size())
                        throw std::invalid_argument("Invalid VFX data interface binding");
                    offset += binding.ValueCount;
                    p.Interfaces.push_back(binding);
                }
                out.emplace(p.StageKind, std::move(p));
            }
            return out;
        }
    } // namespace
    YAML::Node EncodeProduct(const CompiledSystem& p)
    {
        YAML::Node n;
        n["VfxProductVersion"] = 1;
        n["Settings"]          = EncodeSystem(p.Settings);
        n["Programs"]          = ProgramsNode(p.Programs);
        n["Emitters"]          = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& e : p.Emitters)
        {
            YAML::Node a;
            a["Settings"]   = EncodeEmitter(e.Settings);
            a["Programs"]   = ProgramsNode(e.Programs);
            a["Attributes"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& v : e.Attributes)
            {
                YAML::Node b;
                b["Name"]    = v.Name;
                b["Default"] = EncodeValue(v.Default);
                b["Offset"]  = v.Offset;
                a["Attributes"].push_back(b);
            }
            n["Emitters"].push_back(a);
        }
        return n;
    }
    CompiledSystem DecodeProduct(const YAML::Node& n)
    {
        if (n["VfxProductVersion"].as<int>() != 1)
            throw std::invalid_argument("Unsupported VFX product version");
        CompiledSystem p;
        p.Settings = DecodeSystem(n["Settings"]);
        p.Programs = ReadPrograms(n["Programs"]);
        for (const auto& a : n["Emitters"])
        {
            CompiledEmitter e;
            e.Settings = DecodeEmitter(a["Settings"]);
            e.Programs = ReadPrograms(a["Programs"]);
            std::set<std::string> names;
            for (const auto& b : a["Attributes"])
            {
                Attribute v;
                v.Name    = b["Name"].as<std::string>();
                v.Default = DecodeValue(b["Default"]);
                v.Kind    = v.Default.Kind;
                v.Offset  = b["Offset"].as<uint32_t>();
                if (v.Offset != e.Attributes.size() || !names.insert(v.Name).second)
                    throw std::invalid_argument("Invalid VFX attribute layout");
                e.Attributes.push_back(v);
            }
            p.Emitters.push_back(e);
        }
        return p;
    }
} // namespace Kans::Vfx
