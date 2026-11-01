#include "kspch.h"
#include "VfxAsset.h"
#include <fstream>
#include <mutex>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/FileSystem/FileSystem.h"
namespace Kans::Vfx
{
    namespace
    {
        constexpr const char* Types[]  = {"float", "vec2", "vec3", "vec4", "int", "bool", "color", "quat", "position"};
        constexpr const char* Stages[] = {"SystemSpawn",
                                          "SystemUpdate",
                                          "EmitterSpawn",
                                          "EmitterUpdate",
                                          "ParticleSpawn",
                                          "ParticleUpdate",
                                          "EventHandler",
                                          "SimulationStage"};
        template<class E, size_t N>
        E Parse(const YAML::Node& n, const char* const (&names)[N])
        {
            const auto s = n.as<std::string>();
            for (size_t i = 0; i < N; ++i)
                if (s == names[i])
                    return E(i);
            throw std::invalid_argument("Unknown VFX enum: " + s);
        }
        void Check(bool b, const std::string& message)
        {
            if (!b)
                throw std::invalid_argument(message);
        }
        YAML::Node Vec(glm::vec4 v, uint32_t count)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (uint32_t i = 0; i < count; ++i)
                n.push_back(v[i]);
            return n;
        }
        glm::vec4 ReadVec(const YAML::Node& n, uint32_t count)
        {
            Check(n.IsSequence() && n.size() == count, "Invalid VFX vector size");
            glm::vec4 v(0);
            for (uint32_t i = 0; i < count; ++i)
            {
                v[i] = n[i].as<float>();
                Check(std::isfinite(v[i]), "Non-finite VFX value");
            }
            return v;
        }
        YAML::Node ID(AssetID id) { return YAML::Node(id.ToString()); }
        AssetID    ReadID(const YAML::Node& n) { return n ? AssetID::FromString(n.as<std::string>()) : AssetID {}; }
        YAML::Node Parameters(const std::vector<Parameter>& p)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : p)
            {
                YAML::Node a;
                a["Name"] = x.Name;
                a["Type"] = TypeName(x.Kind);
                if (x.Default)
                    a["Default"] = EncodeValue(*x.Default);
                if (x.Range)
                    a["Range"] = Vec(glm::vec4(*x.Range, 0, 0), 2);
                n.push_back(a);
            }
            return n;
        }
        std::vector<Parameter> ReadParameters(const YAML::Node& n, const std::string& prefix)
        {
            std::vector<Parameter> r;
            std::set<std::string>  names;
            if (n)
                Check(n.IsSequence(), "VFX parameter declarations must be a sequence");
            for (const auto& x : n)
            {
                Parameter p;
                p.Name = x["Name"].as<std::string>();
                p.Kind = Parse<Type>(x["Type"], Types);
                Check(p.Name.rfind(prefix, 0) == 0 && p.Name.size() > prefix.size(),
                      "VFX parameter namespace: " + p.Name);
                Check(names.insert(p.Name).second, "Duplicate VFX parameter: " + p.Name);
                if (x["Default"])
                {
                    p.Default = DecodeValue(x["Default"]);
                    Check(p.Default->Kind == p.Kind, "VFX default type mismatch: " + p.Name);
                }
                if (x["Range"])
                {
                    auto v = ReadVec(x["Range"], 2);
                    Check(v.x <= v.y, "Invalid VFX input range");
                    p.Range = glm::vec2(v);
                }
                r.push_back(p);
            }
            return r;
        }
        YAML::Node BoundsNode(const Bounds& b)
        {
            YAML::Node n;
            n["Min"] = Vec(glm::vec4(b.Min, 0), 3);
            n["Max"] = Vec(glm::vec4(b.Max, 0), 3);
            return n;
        }
        Bounds ReadBounds(const YAML::Node& n)
        {
            Bounds b;
            b.Min = ReadVec(n["Min"], 3);
            b.Max = ReadVec(n["Max"], 3);
            Check(b.IsValid(), "Invalid VFX bounds");
            return b;
        }
        YAML::Node                   Inputs(const std::map<std::string, Input>&);
        Input                        ReadInput(const YAML::Node& n, uint32_t depth);
        std::map<std::string, Input> ReadInputs(const YAML::Node& n, uint32_t depth = 0)
        {
            Check(depth < 64, "VFX input nesting exceeds 64");
            std::map<std::string, Input> r;
            if (n)
                Check(n.IsMap(), "VFX inputs must be a map");
            for (const auto& x : n)
                Check(r.emplace(x.first.as<std::string>(), ReadInput(x.second, depth + 1)).second,
                      "Duplicate VFX input");
            return r;
        }
        YAML::Node InputNode(const Input& x)
        {
            YAML::Node            n;
            constexpr const char* names[] = {"Literal", "Link", "DynamicInput", "DataInterface"};
            n["Kind"]                     = names[int(x.Kind)];
            switch (x.Kind)
            {
                case InputKind::Literal:
                    n["Value"] = EncodeValue(x.Literal);
                    break;
                case InputKind::Link:
                    n["Parameter"] = x.Link;
                    break;
                case InputKind::DynamicInput:
                    n["Script"] = ID(x.Script);
                    n["Inputs"] = Inputs(x.Inputs);
                    break;
                case InputKind::DataInterface:
                    n["Binding"] = EncodeInterface(x.Interface);
                    break;
            }
            return n;
        }
        YAML::Node Inputs(const std::map<std::string, Input>& inputs)
        {
            YAML::Node n(YAML::NodeType::Map);
            for (const auto& [k, v] : inputs)
                n[k] = InputNode(v);
            return n;
        }
        Input ReadInput(const YAML::Node& n, uint32_t depth)
        {
            constexpr const char* names[] = {"Literal", "Link", "DynamicInput", "DataInterface"};
            Input                 x;
            x.Kind = Parse<InputKind>(n["Kind"], names);
            switch (x.Kind)
            {
                case InputKind::Literal:
                    x.Literal = DecodeValue(n["Value"]);
                    break;
                case InputKind::Link:
                    x.Link = n["Parameter"].as<std::string>();
                    break;
                case InputKind::DynamicInput:
                    x.Script = ReadID(n["Script"]);
                    x.Inputs = ReadInputs(n["Inputs"], depth);
                    break;
                case InputKind::DataInterface:
                    x.Interface = DecodeInterface(n["Binding"]);
                    break;
            }
            return x;
        }
        YAML::Node UseNode(const ModuleUse& x)
        {
            YAML::Node n;
            n["Guid"]    = x.Guid;
            n["Script"]  = ID(x.Script);
            n["Enabled"] = x.Enabled;
            n["Inputs"]  = Inputs(x.Inputs);
            return n;
        }
        ModuleUse ReadUse(const YAML::Node& n)
        {
            ModuleUse x;
            x.Guid    = n["Guid"].as<std::string>();
            x.Script  = ReadID(n["Script"]);
            x.Enabled = n["Enabled"].as<bool>(true);
            x.Inputs  = ReadInputs(n["Inputs"]);
            Check(!x.Guid.empty() && x.Script.IsValid(), "VFX module usage requires a stable GUID and script ID");
            return x;
        }
        YAML::Node StageNode(const std::map<Stage, Stack>& stages)
        {
            YAML::Node n(YAML::NodeType::Map);
            for (const auto& [s, stack] : stages)
            {
                YAML::Node a(YAML::NodeType::Sequence);
                for (const auto& u : stack)
                    a.push_back(UseNode(u));
                n[StageName(s)] = a;
            }
            return n;
        }
        std::map<Stage, Stack> ReadStages(const YAML::Node& n, bool system)
        {
            std::map<Stage, Stack> stages;
            std::set<std::string>  ids;
            if (n)
                Check(n.IsMap(), "VFX stages must be a map");
            for (const auto& x : n)
            {
                auto s = Parse<Stage>(x.first, Stages);
                Check(x.second.IsSequence(), "VFX stage must contain a module stack");
                if (s == Stage::EventHandler || s == Stage::SimulationStage)
                {
                    Check(x.second.size() == 0, std::string(StageName(s)) + " not implemented in V1a");
                }
                else
                    Check(system ? s <= Stage::SystemUpdate : s >= Stage::EmitterSpawn,
                          "VFX stage belongs to a different asset kind");
                auto& stack = stages[s];
                for (const auto& v : x.second)
                {
                    auto u = ReadUse(v);
                    Check(ids.insert(u.Guid).second, "Duplicate VFX usage GUID: " + u.Guid);
                    stack.push_back(std::move(u));
                }
            }
            return stages;
        }
        constexpr const char* RenderKinds[] = {"Sprite", "Mesh", "Ribbon", "Light"};
        constexpr const char* FacingModes[] = {"ViewPlane", "CameraPosition", "Velocity", "Custom"};
        constexpr const char* SortModes[]   = {"None", "EmitterOrder", "ParticleDepth", "ParticleDistance"};
        YAML::Node            RendererNode(const Renderer& x)
        {
            YAML::Node n;
            n["Guid"]              = x.Guid;
            n["Kind"]              = RenderKinds[int(x.Kind)];
            n["Material"]          = ID(x.Material);
            n["Facing"]            = FacingModes[int(x.FacingMode)];
            n["Sort"]              = SortModes[int(x.Sorting)];
            n["Pivot"]             = Vec(glm::vec4(x.Pivot, 0, 0), 2);
            n["FacingVector"]      = Vec(glm::vec4(x.FacingVector, 0), 3);
            n["AlignmentVector"]   = Vec(glm::vec4(x.AlignmentVector, 0), 3);
            n["Rows"]              = x.Rows;
            n["Columns"]           = x.Columns;
            n["FrameBlending"]     = x.FrameBlending;
            n["DepthFadeDistance"] = x.DepthFadeDistance;
            for (const auto& [k, v] : x.Bindings)
                n["Bindings"][k] = v;
            return n;
        }
        Renderer ReadRenderer(const YAML::Node& n)
        {
            Renderer x;
            x.Guid = n["Guid"].as<std::string>();
            Check(!x.Guid.empty(), "Missing VFX renderer GUID");
            x.Kind     = Parse<RendererKind>(n["Kind"], RenderKinds);
            x.Material = ReadID(n["Material"]);
            if (n["Facing"])
                x.FacingMode = Parse<Facing>(n["Facing"], FacingModes);
            if (n["Sort"])
                x.Sorting = Parse<Sort>(n["Sort"], SortModes);
            if (n["Pivot"])
                x.Pivot = ReadVec(n["Pivot"], 2);
            if (n["FacingVector"])
                x.FacingVector = ReadVec(n["FacingVector"], 3);
            if (n["AlignmentVector"])
                x.AlignmentVector = ReadVec(n["AlignmentVector"], 3);
            x.Rows    = n["Rows"].as<uint32_t>(1);
            x.Columns = n["Columns"].as<uint32_t>(1);
            Check(x.Rows && x.Columns && uint64_t(x.Rows) * x.Columns <= 65536, "Invalid VFX SubUV grid");
            x.FrameBlending     = n["FrameBlending"].as<bool>(false);
            x.DepthFadeDistance = n["DepthFadeDistance"].as<float>(0);
            Check(std::isfinite(x.DepthFadeDistance) && x.DepthFadeDistance >= 0, "Invalid VFX depth fade");
            if (n["Bindings"])
            {
                x.Bindings.clear();
                for (const auto& b : n["Bindings"])
                    x.Bindings[b.first.as<std::string>()] = b.second.as<std::string>();
            }
            return x;
        }
        constexpr const char* Actions[] = {"AddModule",
                                           "RemoveModule",
                                           "MoveModule",
                                           "SetInput",
                                           "SetEnabled",
                                           "AddRenderer",
                                           "RemoveRenderer",
                                           "ReplaceRenderer"};
        YAML::Node            OverridesNode(const std::vector<Override>& v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : v)
            {
                YAML::Node a;
                a["Action"] = Actions[int(x.Action)];
                a["Stage"]  = StageName(x.StageKind);
                a["Target"] = x.Target;
                a["Before"] = x.Before;
                if (x.Action == Override::Kind::AddModule)
                    a["Module"] = UseNode(x.Added);
                if (x.Action == Override::Kind::SetInput)
                {
                    a["Input"] = x.InputName;
                    a["Value"] = InputNode(x.InputValue);
                }
                if (x.Action == Override::Kind::SetEnabled)
                    a["Enabled"] = x.Enabled;
                if (x.Action == Override::Kind::AddRenderer || x.Action == Override::Kind::ReplaceRenderer)
                    a["Renderer"] = RendererNode(x.Render);
                n.push_back(a);
            }
            return n;
        }
        std::vector<Override> ReadOverrides(const YAML::Node& n)
        {
            std::vector<Override> r;
            for (const auto& a : n)
            {
                Override x;
                x.Action = Parse<Override::Kind>(a["Action"], Actions);
                if (a["Stage"])
                    x.StageKind = Parse<Stage>(a["Stage"], Stages);
                x.Target = a["Target"].as<std::string>("");
                x.Before = a["Before"].as<std::string>("");
                if (x.Action == Override::Kind::AddModule)
                    x.Added = ReadUse(a["Module"]);
                if (x.Action == Override::Kind::SetInput)
                {
                    x.InputName  = a["Input"].as<std::string>();
                    x.InputValue = ReadInput(a["Value"], 0);
                }
                if (x.Action == Override::Kind::SetEnabled)
                    x.Enabled = a["Enabled"].as<bool>();
                if (x.Action == Override::Kind::AddRenderer || x.Action == Override::Kind::ReplaceRenderer)
                    x.Render = ReadRenderer(a["Renderer"]);
                r.push_back(x);
            }
            return r;
        }
        YAML::Node Pins(const std::vector<Pin>& pins)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& p : pins)
            {
                YAML::Node a;
                a["Name"] = p.Name;
                a["Type"] = TypeName(p.Kind);
                if (p.Default)
                    a["Default"] = EncodeValue(*p.Default);
                n.push_back(a);
            }
            return n;
        }
        std::vector<Pin> ReadPins(const YAML::Node& n)
        {
            std::vector<Pin>      r;
            std::set<std::string> names;
            for (const auto& a : n)
            {
                Pin p;
                p.Name = a["Name"].as<std::string>();
                p.Kind = Parse<Type>(a["Type"], Types);
                Check(names.insert(p.Name).second, "Duplicate VFX pin");
                if (a["Default"])
                    p.Default = DecodeValue(a["Default"]);
                r.push_back(p);
            }
            return r;
        }
        std::filesystem::path BuiltinRoot;
        std::mutex            RootMutex;
    } // namespace
    uint32_t Components(Type t)
    {
        return t == Type::Vec2                                        ? 2 :
               t == Type::Vec3 || t == Type::Position                 ? 3 :
               t == Type::Vec4 || t == Type::Color || t == Type::Quat ? 4 :
                                                                        1;
    }
    const char* TypeName(Type t) { return Types[int(t)]; }
    const char* StageName(Stage s) { return Stages[int(s)]; }
    uint32_t    Hash(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        return x ^ (x >> 16);
    }
    uint32_t StableHash(const std::string& s)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }
    YAML::Node EncodeValue(const Value& x)
    {
        YAML::Node n;
        n["Type"] = TypeName(x.Kind);
        if (x.Kind == Type::Int)
            n["Value"] = x.Integer;
        else if (x.Kind == Type::Bool)
            n["Value"] = bool(x.Integer);
        else if (x.Kind == Type::Float)
            n["Value"] = x.Data.x;
        else
            n["Value"] = Vec(x.Data, Components(x.Kind));
        return n;
    }
    Value DecodeValue(const YAML::Node& n)
    {
        Value x;
        x.Kind = Parse<Type>(n["Type"], Types);
        if (x.Kind == Type::Int)
            x.Integer = n["Value"].as<int32_t>();
        else if (x.Kind == Type::Bool)
            x.Integer = n["Value"].as<bool>();
        else if (x.Kind == Type::Float)
        {
            x.Data = glm::vec4(n["Value"].as<float>());
            Check(std::isfinite(x.Data.x), "Non-finite VFX value");
        }
        else
            x.Data = ReadVec(n["Value"], Components(x.Kind));
        return x;
    }
    YAML::Node EncodeParameters(const ParameterMap& m)
    {
        YAML::Node n(YAML::NodeType::Map);
        for (const auto& [k, v] : m)
            n[k] = EncodeValue(v);
        return n;
    }
    ParameterMap DecodeParameters(const YAML::Node& n)
    {
        ParameterMap r;
        for (const auto& v : n)
            Check(r.emplace(v.first.as<std::string>(), DecodeValue(v.second)).second, "Duplicate VFX override");
        return r;
    }
    YAML::Node EncodeInterface(const InterfaceBinding& x)
    {
        YAML::Node n;
        n["Kind"]   = x.Kind;
        n["Asset"]  = ID(x.Asset);
        n["Width"]  = x.Width;
        n["Height"] = x.Height;
        n["Times"]  = YAML::Node(YAML::NodeType::Sequence);
        n["Values"] = YAML::Node(YAML::NodeType::Sequence);
        for (auto t : x.Times)
            n["Times"].push_back(t);
        for (auto v : x.Values)
            n["Values"].push_back(Vec(v, 4));
        return n;
    }
    InterfaceBinding DecodeInterface(const YAML::Node& n)
    {
        InterfaceBinding x;
        x.Kind   = n["Kind"].as<std::string>();
        x.Asset  = ReadID(n["Asset"]);
        x.Width  = n["Width"].as<uint32_t>(0);
        x.Height = n["Height"].as<uint32_t>(0);
        for (const auto& t : n["Times"])
        {
            float v = t.as<float>();
            Check(std::isfinite(v) && (x.Times.empty() || v > x.Times.back()),
                  "VFX curve times must be finite and increasing");
            x.Times.push_back(v);
        }
        for (const auto& v : n["Values"])
            x.Values.push_back(ReadVec(v, 4));
        Check(x.Times.empty() || x.Times.size() == x.Values.size(), "VFX curve time/value count mismatch");
        return x;
    }
    YAML::Node EncodeModule(const Module& x)
    {
        YAML::Node n;
        n["Version"]                   = 1;
        n["AssetID"]                   = ID(x.Identity);
        constexpr const char* usages[] = {"Module", "Function", "DynamicInput"};
        n["Usage"]                     = usages[int(x.ScriptUsage)];
        n["AllowedStages"]             = x.AllowedStages;
        n["Category"]                  = x.Category;
        n["Description"]               = x.Description;
        n["Inputs"]                    = Parameters(x.Inputs);
        n["ResultNode"]                = x.ResultNode;
        n["ResultPin"]                 = x.ResultPin;
        n["Nodes"]                     = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& v : x.Nodes)
        {
            YAML::Node a;
            a["Guid"]      = v.Guid;
            a["Op"]        = v.Op;
            a["Type"]      = TypeName(v.Kind);
            a["Parameter"] = v.Parameter;
            a["Code"]      = v.Code;
            a["Constant"]  = EncodeValue(v.Constant);
            a["Function"]  = ID(v.Function);
            a["Inputs"]    = Pins(v.Inputs);
            a["Outputs"]   = Pins(v.Outputs);
            a["Interface"] = EncodeInterface(v.Interface);
            n["Nodes"].push_back(a);
        }
        n["Links"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& v : x.Links)
        {
            YAML::Node a;
            a["From"]   = v.From;
            a["Output"] = v.Output;
            a["To"]     = v.To;
            a["Input"]  = v.Input;
            n["Links"].push_back(a);
        }
        return n;
    }
    Module DecodeModule(const YAML::Node& n)
    {
        Check(n["Version"].as<int>() == 1, "Unsupported VFX module version");
        Module x;
        x.Identity                     = ReadID(n["AssetID"]);
        constexpr const char* usages[] = {"Module", "Function", "DynamicInput"};
        x.ScriptUsage                  = Parse<Usage>(n["Usage"], usages);
        x.AllowedStages                = n["AllowedStages"].as<uint32_t>(0x3f);
        x.Category                     = n["Category"].as<std::string>("");
        x.Description                  = n["Description"].as<std::string>("");
        x.Inputs                       = ReadParameters(n["Inputs"], "Module.");
        x.ResultNode                   = n["ResultNode"].as<std::string>("");
        x.ResultPin                    = n["ResultPin"].as<std::string>("Value");
        std::set<std::string> ids;
        for (const auto& a : n["Nodes"])
        {
            Node v;
            v.Guid = a["Guid"].as<std::string>();
            Check(!v.Guid.empty() && ids.insert(v.Guid).second, "Duplicate or empty VFX node GUID");
            v.Op        = a["Op"].as<std::string>();
            v.Kind      = Parse<Type>(a["Type"], Types);
            v.Parameter = a["Parameter"].as<std::string>("");
            v.Code      = a["Code"].as<std::string>("");
            if (a["Constant"])
                v.Constant = DecodeValue(a["Constant"]);
            v.Function = ReadID(a["Function"]);
            v.Inputs   = ReadPins(a["Inputs"]);
            v.Outputs  = ReadPins(a["Outputs"]);
            if (a["Interface"])
                v.Interface = DecodeInterface(a["Interface"]);
            x.Nodes.push_back(v);
        }
        for (const auto& a : n["Links"])
        {
            Link v;
            v.From   = a["From"].as<std::string>();
            v.Output = a["Output"].as<std::string>("Value");
            v.To     = a["To"].as<std::string>();
            v.Input  = a["Input"].as<std::string>("Value");
            x.Links.push_back(v);
        }
        return x;
    }
    YAML::Node EncodeEmitter(const Emitter& x)
    {
        YAML::Node n;
        n["Version"]       = 1;
        n["Name"]          = x.Name;
        n["Parent"]        = ID(x.Parent);
        const auto setting = [&](const char* s) { return !x.Parent.IsValid() || x.SettingOverrides.count(s); };
        if (setting("Target"))
            n["Target"] = x.Target == SimTarget::GPU ? "GPU" : "CPU";
        if (setting("Space"))
            n["Space"] = x.SimulationSpace == Space::Local ? "Local" : "World";
        if (setting("Capacity"))
            n["Capacity"] = x.Capacity;
        if (setting("Deterministic"))
            n["Deterministic"] = x.Deterministic;
        if (setting("FixedBounds"))
        {
            if (x.FixedBounds)
                n["FixedBounds"] = BoundsNode(*x.FixedBounds);
            else
                n["FixedBounds"] = YAML::Node(YAML::NodeType::Null);
        }
        if (setting("CullDistance"))
            n["CullDistance"] = x.CullDistance;
        if (setting("DistancePolicy"))
            n["DistancePolicy"] = x.DistancePolicy == CullPolicy::PauseSpawning ? "PauseSpawning" : "Continue";
        if (!x.Parent.IsValid())
        {
            n["Stages"]     = StageNode(x.Stages);
            n["Attributes"] = Parameters(x.Attributes);
            n["Renderers"]  = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& r : x.Renderers)
                n["Renderers"].push_back(RendererNode(r));
        }
        n["Overrides"] = OverridesNode(x.Overrides);
        return n;
    }
    Emitter DecodeEmitter(const YAML::Node& n)
    {
        Check(n["Version"].as<int>() == 1, "Unsupported VFX emitter version");
        Emitter x;
        x.Name   = n["Name"].as<std::string>("");
        x.Parent = ReadID(n["Parent"]);
        for (const char* s :
             {"Target", "Space", "Capacity", "Deterministic", "FixedBounds", "CullDistance", "DistancePolicy"})
            if (n[s])
                x.SettingOverrides.insert(s);
        constexpr const char* targets[] = {"CPU", "GPU"};
        if (n["Target"])
            x.Target = Parse<SimTarget>(n["Target"], targets);
        constexpr const char* spaces[] = {"Local", "World"};
        if (n["Space"])
            x.SimulationSpace = Parse<Space>(n["Space"], spaces);
        x.Capacity = n["Capacity"].as<uint32_t>(1024);
        Check(x.Capacity > 0 && x.Capacity <= 16777216, "Invalid VFX capacity");
        x.Deterministic = n["Deterministic"].as<bool>(true);
        if (n["FixedBounds"] && !n["FixedBounds"].IsNull())
            x.FixedBounds = ReadBounds(n["FixedBounds"]);
        x.CullDistance = n["CullDistance"].as<float>(0);
        Check(std::isfinite(x.CullDistance) && x.CullDistance >= 0, "Invalid VFX cull distance");
        constexpr const char* policies[] = {"PauseSpawning", "Continue"};
        if (n["DistancePolicy"])
            x.DistancePolicy = Parse<CullPolicy>(n["DistancePolicy"], policies);
        x.Stages     = ReadStages(n["Stages"], false);
        x.Attributes = ReadParameters(n["Attributes"], "Particles.");
        std::set<std::string> ids;
        for (const auto& r : n["Renderers"])
        {
            auto v = ReadRenderer(r);
            Check(ids.insert(v.Guid).second, "Duplicate VFX renderer GUID");
            x.Renderers.push_back(v);
        }
        x.Overrides = ReadOverrides(n["Overrides"]);
        Check(!x.Parent.IsValid() || (x.Stages.empty() && x.Attributes.empty() && x.Renderers.empty()),
              "Child VFX emitter must store only overrides");
        return x;
    }
    YAML::Node EncodeSystem(const System& x)
    {
        YAML::Node n;
        n["Version"]         = 1;
        n["Stages"]          = StageNode(x.Stages);
        n["UserParameters"]  = Parameters(x.UserParameters);
        n["Seed"]            = x.Seed;
        n["WarmupTime"]      = x.WarmupTime;
        n["WarmupTickDelta"] = x.WarmupTickDelta;
        n["MaxDeltaTime"]    = x.MaxDeltaTime;
        if (x.FixedBounds)
            n["FixedBounds"] = BoundsNode(*x.FixedBounds);
        n["Emitters"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& h : x.Emitters)
        {
            YAML::Node a;
            a["Guid"]      = h.Guid;
            a["Name"]      = h.Name;
            a["Enabled"]   = h.Enabled;
            a["Reference"] = ID(h.Reference);
            if (h.Inline)
                a["Inline"] = EncodeEmitter(*h.Inline);
            a["Overrides"] = OverridesNode(h.Overrides);
            n["Emitters"].push_back(a);
        }
        return n;
    }
    System DecodeSystem(const YAML::Node& n)
    {
        Check(n["Version"].as<int>() == 1, "Unsupported VFX system version");
        System x;
        x.Stages          = ReadStages(n["Stages"], true);
        x.UserParameters  = ReadParameters(n["UserParameters"], "User.");
        x.Seed            = n["Seed"].as<uint32_t>(1);
        x.WarmupTime      = n["WarmupTime"].as<float>(0);
        x.WarmupTickDelta = n["WarmupTickDelta"].as<float>(1.f / 30);
        x.MaxDeltaTime    = n["MaxDeltaTime"].as<float>(.1f);
        Check(std::isfinite(x.WarmupTime) && x.WarmupTime >= 0 && std::isfinite(x.WarmupTickDelta) &&
                  x.WarmupTickDelta > 0 && std::isfinite(x.MaxDeltaTime) && x.MaxDeltaTime > 0 &&
                  x.WarmupTime / x.WarmupTickDelta <= 100000,
              "Invalid VFX timing");
        if (n["FixedBounds"])
            x.FixedBounds = ReadBounds(n["FixedBounds"]);
        std::set<std::string> ids, names;
        for (const auto& a : n["Emitters"])
        {
            EmitterHandle h;
            h.Guid = a["Guid"].as<std::string>();
            h.Name = a["Name"].as<std::string>();
            Check(!h.Guid.empty() && ids.insert(h.Guid).second && names.insert(h.Name).second,
                  "Duplicate VFX emitter handle GUID or name");
            h.Enabled   = a["Enabled"].as<bool>(true);
            h.Reference = ReadID(a["Reference"]);
            if (a["Inline"])
                h.Inline = DecodeEmitter(a["Inline"]);
            Check(h.Reference.IsValid() != h.Inline.has_value(),
                  "VFX emitter handle needs exactly one inline or referenced emitter");
            h.Overrides = ReadOverrides(a["Overrides"]);
            x.Emitters.push_back(h);
        }
        return x;
    }
    void ApplyOverrides(Emitter& e, const std::vector<Override>& overrides)
    {
        for (const auto& o : overrides)
        {
            if (o.Action >= Override::Kind::AddRenderer)
            {
                auto& v  = e.Renderers;
                auto  at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Target; });
                if (o.Action == Override::Kind::AddRenderer)
                {
                    Check(std::none_of(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Render.Guid; }),
                          "Duplicate VFX renderer GUID: " + o.Render.Guid);
                    auto before = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Before; });
                    Check(o.Before.empty() || before != v.end(), "Missing VFX renderer reorder target: " + o.Before);
                    v.insert(before, o.Render);
                }
                else
                {
                    Check(at != v.end(), "Missing VFX renderer override target: " + o.Target);
                    if (o.Action == Override::Kind::RemoveRenderer)
                        v.erase(at);
                    else
                    {
                        Check(o.Render.Guid == o.Target, "Renderer replacement must preserve GUID");
                        *at = o.Render;
                    }
                }
                continue;
            }
            Check(o.StageKind >= Stage::EmitterSpawn, "VFX emitter override cannot target a system stage");
            Check(o.StageKind < Stage::EventHandler, std::string(StageName(o.StageKind)) + " not implemented in V1a");
            auto& v  = e.Stages[o.StageKind];
            auto  at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Target; });
            if (o.Action == Override::Kind::AddModule)
            {
                for (const auto& [s, st] : e.Stages)
                    Check(std::none_of(st.begin(), st.end(), [&](const auto& x) { return x.Guid == o.Added.Guid; }),
                          "Duplicate VFX usage GUID: " + o.Added.Guid);
                auto before = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Before; });
                Check(o.Before.empty() || before != v.end(), "Missing VFX reorder target: " + o.Before);
                v.insert(before, o.Added);
                continue;
            }
            Check(at != v.end(), "Missing VFX module override target: " + o.Target);
            if (o.Action == Override::Kind::RemoveModule)
                v.erase(at);
            else if (o.Action == Override::Kind::SetInput)
                at->Inputs[o.InputName] = o.InputValue;
            else if (o.Action == Override::Kind::SetEnabled)
                at->Enabled = o.Enabled;
            else
            {
                auto item = *at;
                v.erase(at);
                auto before = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Before; });
                Check(o.Before.empty() || before != v.end(), "Missing VFX reorder target: " + o.Before);
                v.insert(before, item);
            }
        }
    }
    Emitter MergeEmitter(const Emitter& child, const EmitterResolver& resolve)
    {
        std::set<AssetID>                      chain;
        std::function<Emitter(const Emitter&)> merge = [&](const Emitter& c) {
            Emitter r = c;
            if (c.Parent.IsValid())
            {
                Check(chain.insert(c.Parent).second, "VFX parent cycle: " + c.Parent.ToString());
                Emitter parent;
                try
                {
                    parent = resolve(c.Parent);
                }
                catch (const std::exception& ex)
                {
                    throw std::invalid_argument("Missing VFX parent " + c.Parent.ToString() + ": " + ex.what());
                }
                r = merge(parent);
                chain.erase(c.Parent);
                r.Name = c.Name;
                for (const auto& s : c.SettingOverrides)
                {
                    if (s == "Target")
                        r.Target = c.Target;
                    else if (s == "Space")
                        r.SimulationSpace = c.SimulationSpace;
                    else if (s == "Capacity")
                        r.Capacity = c.Capacity;
                    else if (s == "Deterministic")
                        r.Deterministic = c.Deterministic;
                    else if (s == "FixedBounds")
                        r.FixedBounds = c.FixedBounds;
                    else if (s == "CullDistance")
                        r.CullDistance = c.CullDistance;
                    else if (s == "DistancePolicy")
                        r.DistancePolicy = c.DistancePolicy;
                }
            }
            ApplyOverrides(r, c.Overrides);
            r.Parent = {};
            r.Overrides.clear();
            r.SettingOverrides.clear();
            return r;
        };
        return merge(child);
    }
    InterfaceRegistry::InterfaceRegistry()
    {
        Register("Curve", {Type::Float, true});
        Register("Gradient", {Type::Color, true});
        Register("Texture2D", {Type::Color, true});
        for (const char* s : {"SkeletalMesh", "StaticMesh", "EmitterParticles", "Events"})
            Register(s, {Type::Vec4, false});
    }
    void InterfaceRegistry::Register(std::string s, Entry e)
    {
        Check(!s.empty() && m_Entries.emplace(s, e).second, "Duplicate VFX data interface: " + s);
    }
    const InterfaceRegistry::Entry* InterfaceRegistry::Find(const std::string& s) const
    {
        auto i = m_Entries.find(s);
        return i == m_Entries.end() ? nullptr : &i->second;
    }
    std::map<std::string, Parameter> BuiltinParameters()
    {
        std::map<std::string, Parameter> r;
        auto                             add = [&](std::string s, Type t, glm::vec4 v = glm::vec4(0)) {
            Value value = Value::Vector(v, t);
            r[s]        = {s, t, value, {}};
        };
        for (const char* s : {"Engine.DeltaTime",
                              "Engine.Time",
                              "System.Age",
                              "Emitter.Age",
                              "Emitter.NormalizedLoopAge",
                              "Emitter.LoopAge"})
            add(s, Type::Float);
        for (const char* s : {"System.LoopCount",
                              "System.ExecutionState",
                              "Emitter.ExecutionState",
                              "Emitter.SpawnCountThisFrame",
                              "Particles.UniqueID"})
            add(s, Type::Int);
        add("Engine.Owner.Position", Type::Position);
        // Matrix reads expose four typed vec4 pins, Column0 through Column3.
        add("Engine.Owner.LocalToWorld", Type::Vec4, {1, 0, 0, 0});
        add("Engine.Owner.Rotation", Type::Quat, {0, 0, 0, 1});
        add("Engine.Owner.Scale", Type::Vec3, glm::vec4(1));
        add("Engine.Owner.Velocity", Type::Vec3);
        add("Engine.Camera.Position", Type::Position);
        add("Particles.Position", Type::Position);
        add("Particles.Velocity", Type::Vec3);
        add("Particles.Color", Type::Color, glm::vec4(1));
        add("Particles.SpriteSize", Type::Vec2, glm::vec4(1));
        add("Particles.SpriteRotation", Type::Float);
        add("Particles.Mass", Type::Float, glm::vec4(1));
        add("Particles.Age", Type::Float);
        add("Particles.Lifetime", Type::Float, glm::vec4(1));
        add("Particles.NormalizedAge", Type::Float);
        add("Particles.SubImageIndex", Type::Float);
        add("Particles.PhysicsForce", Type::Vec3);
        add("Particles.DynamicMaterialParameter", Type::Vec4);
        add("Particles.Alive", Type::Bool);
        r["Particles.Alive"].Default = Value::Bool(true);
        return r;
    }
    std::filesystem::path BuiltinModuleRoot()
    {
        std::lock_guard<std::mutex> lock(RootMutex);
        if (!BuiltinRoot.empty())
            return BuiltinRoot;
        // The editor ships the modules next to its executable; tests and tools run from inside the
        // source tree or set an explicit root.
        for (auto base : {KansFileSystem::GetExecutableDirectory(), std::filesystem::current_path()})
        {
            for (int i = 0; i < 8 && !base.empty(); ++i, base = base.parent_path())
            {
                for (const auto& suffix : {"Resources/VFX/Modules", "KansEditor/Resources/VFX/Modules"})
                {
                    auto p = base / suffix;
                    if (std::filesystem::is_directory(p))
                        return p;
                }
            }
        }
        throw std::invalid_argument("VFX built-in module root not found");
    }
    void SetBuiltinModuleRoot(std::filesystem::path root)
    {
        std::lock_guard<std::mutex> lock(RootMutex);
        BuiltinRoot = std::move(root);
    }
    std::map<AssetID, std::filesystem::path> FindBuiltinModules()
    {
        std::map<AssetID, std::filesystem::path> r;
        for (const auto& f : std::filesystem::directory_iterator(BuiltinModuleRoot()))
            if (f.path().extension() == ".vfxmodule")
            {
                auto n  = YAML::LoadFile(f.path().string());
                auto id = ReadID(n["AssetID"]);
                Check(id.IsValid() && r.emplace(id, f.path()).second, "Invalid or duplicate built-in VFX asset ID");
            }
        return r;
    }
    AssetID BuiltinModuleID(const std::string& name)
    {
        auto n  = YAML::LoadFile((BuiltinModuleRoot() / (name + ".vfxmodule")).string());
        auto id = ReadID(n["AssetID"]);
        Check(id.IsValid(), "Invalid built-in VFX asset ID: " + name);
        return id;
    }
} // namespace Kans::Vfx
