#include "kspch.h"
#include "VfxAsset.h"
#include <cstring>
#include <fstream>
#include <mutex>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/FileSystem/FileSystem.h"
#include "VfxCompiler.h"
namespace Kans::Vfx
{
    namespace
    {
        constexpr const char* Types[] =
            {"float", "vec2", "vec3", "vec4", "int", "bool", "color", "quat", "position", "ivec3"};
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
                if (!x.EnumNames.empty())
                    a["Enum"] = x.EnumNames;
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
                if (x["Enum"])
                {
                    p.EnumNames = x["Enum"].as<std::vector<std::string>>();
                    Check(p.Kind == Type::Int, "VFX enum input requires int: " + p.Name);
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
                if (s == Stage::EventHandler)
                {
                    Check(!system || x.second.size() == 0, "EventHandler belongs to an emitter");
                    continue; // Handler records are decoded separately from module stacks.
                }
                if (s == Stage::SimulationStage)
                {
                    Check(!system || x.second.size() == 0, "SimulationStage belongs to an emitter");
                    continue; // Ordered simulation stage records are decoded separately.
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
        constexpr const char* FacingModes[] =
            {"ViewPlane", "CameraPosition", "Velocity", "Custom", "Default", "CameraPlane"};
        constexpr const char* SortModes[] = {"None", "EmitterOrder", "ParticleDepth", "ParticleDistance", "Custom"};
        YAML::Node            RendererNode(const Renderer& x)
        {
            YAML::Node n;
            n["Guid"]             = x.Guid;
            n["Kind"]             = RenderKinds[int(x.Kind)];
            n["Material"]         = ID(x.Material);
            n["Facing"]           = FacingModes[int(x.FacingMode)];
            n["Sort"]             = SortModes[int(x.Sorting)];
            n["SortAscending"]    = x.SortAscending;
            n["CastShadows"]      = x.CastsShadows();
            n["LockedAxisEnable"] = x.LockedAxisEnable;
            n["LockedAxis"]       = Vec(glm::vec4(x.LockedAxis, 0), 3);
            if (x.Kind == RendererKind::Ribbon)
            {
                constexpr const char* facing[] = {"Screen", "Custom", "CustomSide"};
                constexpr const char* modes[]  = {"ScaledUniformly",
                                                  "TiledOverRibbonLength",
                                                  "TiledFromStartOverRibbonLength",
                                                  "ScaledUsingRibbonSegmentLength"};
                n["RibbonFacing"]              = facing[int(x.RibbonFacingMode)];
                n["TessellationFactor"]        = x.TessellationFactor;
                n["CurveTension"]              = x.CurveTension;
                for (const auto& uv : x.UVChannels)
                {
                    YAML::Node u;
                    u["Mode"]         = modes[int(uv.Mode)];
                    u["TileSize"]     = uv.TileSize;
                    u["Offset"]       = Vec(glm::vec4(uv.Offset, 0, 0), 2);
                    u["Scale"]        = Vec(glm::vec4(uv.Scale, 0, 0), 2);
                    u["LeadingEdge"]  = uv.LeadingEdge;
                    u["TrailingEdge"] = uv.TrailingEdge;
                    n["UVChannels"].push_back(u);
                }
            }
            n["Meshes"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& entry : x.Meshes)
            {
                YAML::Node m;
                m["Mesh"]              = ID(entry.Mesh);
                m["Scale"]             = Vec(glm::vec4(entry.Scale, 0), 3);
                m["PivotOffset"]       = Vec(glm::vec4(entry.PivotOffset, 0), 3);
                m["MaterialOverrides"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& [slot, material] : entry.MaterialOverrides)
                {
                    YAML::Node o;
                    o["Slot"]     = slot;
                    o["Material"] = ID(material);
                    m["MaterialOverrides"].push_back(o);
                }
                n["Meshes"].push_back(m);
            }
            n["Pivot"]                         = Vec(glm::vec4(x.Pivot, 0, 0), 2);
            n["FacingVector"]                  = Vec(glm::vec4(x.FacingVector, 0), 3);
            n["AlignmentVector"]               = Vec(glm::vec4(x.AlignmentVector, 0), 3);
            n["Rows"]                          = x.Rows;
            n["Columns"]                       = x.Columns;
            constexpr const char* normals[]    = {"CameraFacing", "Spherical", "ParticleNormal"};
            constexpr const char* evaluation[] = {"PerPixel", "PerVertex"};
            n["LightingNormal"]                = normals[int(x.NormalMode)];
            n["LightingEvaluation"]            = evaluation[int(x.Lighting)];
            n["Curvature"]                     = x.Curvature;
            n["ReceiveShadows"]                = x.ReceiveShadows;
            n["FrameBlending"]                 = x.FrameBlending;
            n["DepthFadeDistance"]             = x.DepthFadeDistance;
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
            x.Material = n["Material"] ? ReadID(n["Material"]) : AssetID {};
            if (x.Kind == RendererKind::Mesh)
                x.FacingMode = Facing::Default;
            if (x.Kind == RendererKind::Ribbon)
            {
                constexpr const char* facing[] = {"Screen", "Custom", "CustomSide"};
                constexpr const char* modes[]  = {"ScaledUniformly",
                                                  "TiledOverRibbonLength",
                                                  "TiledFromStartOverRibbonLength",
                                                  "ScaledUsingRibbonSegmentLength"};
                if (n["RibbonFacing"])
                    x.RibbonFacingMode = Parse<RibbonFacing>(n["RibbonFacing"], facing);
                x.TessellationFactor = n["TessellationFactor"].as<uint32_t>(1);
                x.CurveTension       = n["CurveTension"].as<float>(0);
                if (n["UVChannels"])
                {
                    Check(n["UVChannels"].size() == 2, "Ribbon requires two UV channels");
                    for (size_t i = 0; i < 2; ++i)
                    {
                        auto  u  = n["UVChannels"][i];
                        auto& uv = x.UVChannels[i];
                        if (u["Mode"])
                            uv.Mode = Parse<RibbonUVMode>(u["Mode"], modes);
                        uv.TileSize = u["TileSize"].as<float>(1);
                        if (u["Offset"])
                            uv.Offset = ReadVec(u["Offset"], 2);
                        if (u["Scale"])
                            uv.Scale = ReadVec(u["Scale"], 2);
                        uv.LeadingEdge  = u["LeadingEdge"].as<float>(0);
                        uv.TrailingEdge = u["TrailingEdge"].as<float>(0);
                    }
                }
            }
            x.SortAscending    = n["SortAscending"].as<bool>(false);
            x.CastShadows      = n["CastShadows"].as<bool>(x.Kind != RendererKind::Ribbon);
            x.LockedAxisEnable = n["LockedAxisEnable"].as<bool>(false);
            if (n["LockedAxis"])
                x.LockedAxis = ReadVec(n["LockedAxis"], 3);
            for (const auto& m : n["Meshes"])
            {
                MeshEntry entry;
                entry.Mesh = ReadID(m["Mesh"]);
                if (m["Scale"])
                    entry.Scale = ReadVec(m["Scale"], 3);
                if (m["PivotOffset"])
                    entry.PivotOffset = ReadVec(m["PivotOffset"], 3);
                for (const auto& o : m["MaterialOverrides"])
                    entry.MaterialOverrides[o["Slot"].as<uint32_t>()] = ReadID(o["Material"]);
                x.Meshes.push_back(entry);
            }
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
            constexpr const char* normals[]    = {"CameraFacing", "Spherical", "ParticleNormal"};
            constexpr const char* evaluation[] = {"PerPixel", "PerVertex"};
            if (n["LightingNormal"])
                x.NormalMode = Parse<LightingNormal>(n["LightingNormal"], normals);
            if (n["LightingEvaluation"])
                x.Lighting = Parse<LightingEvaluation>(n["LightingEvaluation"], evaluation);
            x.Curvature      = n["Curvature"].as<float>(1);
            x.ReceiveShadows = n["ReceiveShadows"].as<bool>(true);
            Check(std::isfinite(x.Curvature) && x.Curvature >= 0 && x.Curvature <= 1, "Invalid VFX Curvature");
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
                                           "ReplaceRenderer",
                                           "AddHandler",
                                           "RemoveHandler",
                                           "MoveHandler",
                                           "AddSimulationStage",
                                           "RemoveSimulationStage",
                                           "MoveSimulationStage",
                                           "ReplaceSimulationStage",
                                           "MoveRenderer"};
        YAML::Node            HandlerNode(const EventHandler& h)
        {
            YAML::Node n;
            n["Guid"]              = h.Guid;
            n["Enabled"]           = h.Enabled;
            n["Source"]["Emitter"] = h.SourceEmitter;
            n["Source"]["Channel"] = h.Channel;
            n["Mode"]              = h.Mode == EventMode::SpawnedParticles ? "SpawnedParticles" : "EveryParticle";
            n["SpawnNumber"]       = h.SpawnNumber;
            n["MaxEventsPerFrame"] = h.MaxEventsPerFrame;
            n["Modules"]           = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& u : h.Modules)
                n["Modules"].push_back(UseNode(u));
            return n;
        }
        EventHandler ReadHandler(const YAML::Node& n)
        {
            EventHandler h;
            h.Guid = n["Guid"].as<std::string>();
            Check(!h.Guid.empty(), "Event handler requires a stable GUID");
            h.Enabled                     = n["Enabled"].as<bool>(true);
            h.SourceEmitter               = n["Source"]["Emitter"].as<std::string>();
            h.Channel                     = n["Source"]["Channel"].as<std::string>();
            constexpr const char* modes[] = {"SpawnedParticles", "EveryParticle"};
            h.Mode                        = Parse<EventMode>(n["Mode"], modes);
            h.SpawnNumber                 = n["SpawnNumber"].as<uint32_t>(1);
            h.MaxEventsPerFrame           = n["MaxEventsPerFrame"].as<uint32_t>(UINT32_MAX);
            std::set<std::string> ids;
            for (const auto& u : n["Modules"])
            {
                auto use = ReadUse(u);
                Check(ids.insert(use.Guid).second, "Duplicate VFX usage GUID: " + use.Guid);
                h.Modules.push_back(use);
            }
            return h;
        }
        YAML::Node SimulationNode(const SimulationStage& s)
        {
            YAML::Node n;
            n["Guid"]                    = s.Guid;
            n["Name"]                    = s.Name;
            n["IterationSource"]["Kind"] = s.Source == IterationSource::Particles ? "Particles" : "DataInterface";
            if (s.Source == IterationSource::DataInterface)
                n["IterationSource"]["Parameter"] = s.DataInterface;
            n["Iterations"] = s.Iterations;
            if (!s.Enabled.empty())
                n["Enabled"] = s.Enabled;
            n["Modules"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& u : s.Modules)
                n["Modules"].push_back(UseNode(u));
            return n;
        }
        SimulationStage ReadSimulation(const YAML::Node& n)
        {
            SimulationStage s;
            Check(bool(n["IterationSource"]), "SimulationStage requires ordered stage records with IterationSource");
            s.Guid            = n["Guid"].as<std::string>();
            s.Name            = n["Name"].as<std::string>();
            const auto source = n["IterationSource"]["Kind"].as<std::string>();
            Check(source == "Particles" || source == "DataInterface", "Invalid simulation iteration source");
            s.Source        = source == "Particles" ? IterationSource::Particles : IterationSource::DataInterface;
            s.DataInterface = n["IterationSource"]["Parameter"].as<std::string>("");
            s.Iterations    = n["Iterations"].as<uint32_t>(1);
            s.Enabled       = n["Enabled"].as<std::string>("");
            if (auto error = SimulationStageError(s))
                Check(false, *error);
            std::set<std::string> ids;
            for (const auto& u : n["Modules"])
            {
                auto use = ReadUse(u);
                Check(ids.insert(use.Guid).second, "Duplicate VFX usage GUID: " + use.Guid);
                s.Modules.push_back(use);
            }
            return s;
        }
        YAML::Node OverridesNode(const std::vector<Override>& v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& x : v)
            {
                YAML::Node a;
                a["Action"] = Actions[int(x.Action)];
                a["Stage"]  = StageName(x.StageKind);
                a["Target"] = x.Target;
                a["Before"] = x.Before;
                if (!x.SimulationStageGuid.empty())
                    a["SimulationStageGuid"] = x.SimulationStageGuid;
                if (x.Action == Override::Kind::AddSimulationStage ||
                    x.Action == Override::Kind::ReplaceSimulationStage)
                    a["SimulationStage"] = SimulationNode(x.Simulation);
                if (!x.HandlerGuid.empty())
                    a["HandlerGuid"] = x.HandlerGuid;
                if (x.Action == Override::Kind::AddHandler)
                    a["Handler"] = HandlerNode(x.Handler);
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
                if (x.Action == Override::Kind::MoveModule && x.ToStage)
                {
                    a["ToStage"] = StageName(*x.ToStage);
                    if (!x.ToContainer.empty())
                        a["ToContainer"] = x.ToContainer;
                }
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
                x.Target              = a["Target"].as<std::string>("");
                x.Before              = a["Before"].as<std::string>("");
                x.HandlerGuid         = a["HandlerGuid"].as<std::string>("");
                x.SimulationStageGuid = a["SimulationStageGuid"].as<std::string>("");
                if (x.Action == Override::Kind::AddSimulationStage ||
                    x.Action == Override::Kind::ReplaceSimulationStage)
                    x.Simulation = ReadSimulation(a["SimulationStage"]);
                if (x.Action == Override::Kind::AddHandler)
                    x.Handler = ReadHandler(a["Handler"]);
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
                if (x.Action == Override::Kind::MoveModule && a["ToStage"])
                {
                    x.ToStage     = Parse<Stage>(a["ToStage"], Stages);
                    x.ToContainer = a["ToContainer"].as<std::string>("");
                    Check(*x.ToStage >= Stage::EmitterSpawn, "VFX module move cannot target a system stage");
                    Check((*x.ToStage != Stage::EventHandler && *x.ToStage != Stage::SimulationStage) ||
                              !x.ToContainer.empty(),
                          "VFX module move into " + std::string(StageName(*x.ToStage)) + " requires ToContainer");
                }
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
        return t == Type::Vec2                                            ? 2 :
               t == Type::Vec3 || t == Type::Position || t == Type::IVec3 ? 3 :
               t == Type::Vec4 || t == Type::Color || t == Type::Quat     ? 4 :
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
#include "VfxVectorField.inc"
    YAML::Node EncodeInterface(const InterfaceBinding& x)
    {
        YAML::Node n;
        n["Kind"] = x.Kind;
        if (x.Kind == "Grid3D")
        {
            if (!x.Source.empty())
                n["Source"] = x.Source;
            n["Resolution"]  = Vec(glm::vec4(x.Resolution, 0), 3);
            n["WorldBounds"] = Vec(glm::vec4(x.WorldBounds, 0), 3);
            n["Attributes"]  = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& a : x.GridAttributes)
            {
                YAML::Node attribute;
                attribute["Name"] = a.Name;
                attribute["Type"] = TypeName(a.Kind);
                n["Attributes"].push_back(attribute);
            }
        }
        if (x.Kind == "VectorField")
        {
            for (bool repeat : x.Tiling)
                n["Tiling"].push_back(repeat ? "Repeat" : "Clamp");
            n["Intensity"] = x.Intensity;
        }
        if (!x.SourceEmitter.empty())
            n["SourceEmitter"] = x.SourceEmitter;
        if (!x.Channel.empty())
            n["Channel"] = x.Channel;
        if (x.Kind == "StaticMesh" || x.Kind == "SkeletalMesh")
        {
            n["Source"]        = x.Source.empty() ? (x.Kind == "StaticMesh" ? "Asset" : "Owner") : x.Source;
            n["Entity"]        = x.EntityPath;
            n["Skeleton"]      = ID(x.Skeleton);
            n["Sections"]      = x.Sections;
            n["Bones"]         = x.Bones;
            n["Region"]        = x.Region;
            n["SkeletonNames"] = x.SkeletonNames;
        }
        n["Asset"]  = ID(x.Asset);
        n["Width"]  = x.Width;
        n["Height"] = x.Height;
        n["Times"]  = YAML::Node(YAML::NodeType::Sequence);
        n["Values"] = YAML::Node(YAML::NodeType::Sequence);
        for (auto t : x.Times)
            n["Times"].push_back(t);
        for (auto v : x.Geometry ? x.Geometry->Values : x.Values)
            n["Values"].push_back(Vec(v, 4));
        return n;
    }
    InterfaceBinding DecodeInterface(const YAML::Node& n)
    {
        InterfaceBinding x;
        x.Kind = n["Kind"].as<std::string>();
        if (x.Kind == "Grid3D")
        {
            const auto resolution = ReadVec(n["Resolution"], 3);
            for (int k = 0; k < 3; ++k)
                Check(resolution[k] >= 1 && resolution[k] <= 128 && resolution[k] == std::floor(resolution[k]),
                      "Grid3D Resolution must contain integers from 1 to 128");
            x.Resolution  = glm::uvec3(resolution);
            x.WorldBounds = ReadVec(n["WorldBounds"], 3);
            for (const auto& a : n["Attributes"])
                x.GridAttributes.push_back({a["Name"].as<std::string>(), Parse<Type>(a["Type"], Types)});
        }
        if (x.Kind == "VectorField")
        {
            x.Intensity = n["Intensity"].as<float>(1);
            Check(std::isfinite(x.Intensity), "Non-finite VectorField intensity");
            if (auto tiling = n["Tiling"])
            {
                Check(tiling.IsSequence() && tiling.size() == 3, "VectorField Tiling requires three axes");
                for (int axis = 0; axis < 3; ++axis)
                {
                    auto mode = tiling[axis].as<std::string>();
                    Check(mode == "Repeat" || mode == "Clamp", "VectorField Tiling requires Repeat or Clamp");
                    x.Tiling[axis] = mode == "Repeat";
                }
            }
        }
        x.SourceEmitter = n["SourceEmitter"].as<std::string>("");
        x.Channel       = n["Channel"].as<std::string>("");
        x.Asset         = ReadID(n["Asset"]);
        x.Source = n["Source"].as<std::string>(x.Kind == "SkeletalMesh" ? "Owner" : x.Kind == "Grid3D" ? "" : "Asset");
        x.EntityPath = n["Entity"].as<std::string>("");
        x.Skeleton   = ReadID(n["Skeleton"]);
        if (n["Sections"])
            x.Sections = n["Sections"].as<std::vector<uint32_t>>();
        if (n["Bones"])
            x.Bones = n["Bones"].as<std::vector<std::string>>();
        if (n["Region"])
            x.Region = n["Region"].as<std::vector<std::string>>();
        if (n["SkeletonNames"])
            x.SkeletonNames = n["SkeletonNames"].as<std::vector<std::string>>();
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
        if ((x.Kind == "StaticMesh" || x.Kind == "SkeletalMesh" || x.Kind == "VectorField") && !x.Values.empty())
            x.Geometry = InternMeshGeometry(std::exchange(x.Values, {}));
        if (x.Kind == "VectorField" && x.Geometry)
            ValidateVectorField(x);
        return x;
    }
    YAML::Node EncodeEventLayout(const EventLayout& x)
    {
        YAML::Node n;
        n["Name"]   = x.Name;
        n["Fields"] = Parameters(x.Fields);
        return n;
    }
    EventLayout DecodeEventLayout(const YAML::Node& n)
    {
        EventLayout x;
        x.Name   = n["Name"].as<std::string>();
        x.Fields = ReadParameters(n["Fields"], "");
        Check(!x.Name.empty() && !x.Fields.empty(), "Invalid VFX event layout");
        return x;
    }
    YAML::Node EncodeModule(const Module& x)
    {
        YAML::Node n;
        n["Version"] = 1;
        if (!x.EventLayouts.empty())
            for (const auto& l : x.EventLayouts)
                n["EventLayouts"].push_back(EncodeEventLayout(l));
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
            a["Guid"] = v.Guid;
            a["Op"]   = v.Op;
            if (v.Stages != UINT32_MAX)
                a["Stages"] = v.Stages;
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
        if (!x.Editor.Empty())
        {
            YAML::Node editor;
            editor["Nodes"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& v : x.Nodes) // Node order keeps the metadata stable across saves.
            {
                auto at        = x.Editor.Positions.find(v.Guid);
                bool collapsed = x.Editor.Collapsed.count(v.Guid) > 0;
                if (at == x.Editor.Positions.end() && !collapsed)
                    continue;
                YAML::Node a;
                a["Guid"] = v.Guid;
                if (at != x.Editor.Positions.end())
                    a["Position"] = Vec(glm::vec4(at->second, 0, 0), 2);
                if (collapsed)
                    a["Collapsed"] = true;
                editor["Nodes"].push_back(a);
            }
            if (!x.Editor.Comments.empty())
                for (const auto& c : x.Editor.Comments)
                {
                    YAML::Node a;
                    a["Guid"]     = c.Guid;
                    a["Title"]    = c.Title;
                    a["Position"] = Vec(glm::vec4(c.Position, 0, 0), 2);
                    a["Size"]     = Vec(glm::vec4(c.Size, 0, 0), 2);
                    a["Color"]    = Vec(c.Color, 4);
                    editor["Comments"].push_back(a);
                }
            n["Editor"] = editor;
        }
        return n;
    }
    Module DecodeModule(const YAML::Node& n)
    {
        Check(n["Version"].as<int>() == 1, "Unsupported VFX module version");
        Module x;
        for (const auto& l : n["EventLayouts"])
            x.EventLayouts.push_back(DecodeEventLayout(l));
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
            v.Stages    = a["Stages"].as<uint32_t>(UINT32_MAX);
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
        if (const auto editor = n["Editor"])
        {
            for (const auto& a : editor["Nodes"])
            {
                auto guid = a["Guid"].as<std::string>();
                if (!ids.count(guid))
                    continue; // Metadata of a removed node is dropped, never an error.
                if (a["Position"])
                    x.Editor.Positions[guid] = glm::vec2(ReadVec(a["Position"], 2));
                if (a["Collapsed"].as<bool>(false))
                    x.Editor.Collapsed.insert(guid);
            }
            for (const auto& a : editor["Comments"])
            {
                ModuleComment c;
                c.Guid     = a["Guid"].as<std::string>("");
                c.Title    = a["Title"].as<std::string>("");
                c.Position = glm::vec2(ReadVec(a["Position"], 2));
                c.Size     = glm::max(glm::vec2(ReadVec(a["Size"], 2)), glm::vec2(1));
                if (a["Color"])
                    c.Color = ReadVec(a["Color"], 4);
                x.Editor.Comments.push_back(c);
            }
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
            n["Stages"] = StageNode(x.Stages);
            if (!x.SimulationStages.empty())
                for (const auto& stage : x.SimulationStages)
                    n["Stages"]["SimulationStage"].push_back(SimulationNode(stage));
            if (!x.Parameters.empty())
                n["Parameters"] = Parameters(x.Parameters);
            for (const auto& [name, binding] : x.DataInterfaces)
                n["DataInterfaces"][name] = EncodeInterface(binding);
            if (!x.Handlers.empty())
            {
                n["Stages"]["EventHandler"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& h : x.Handlers)
                    n["Stages"]["EventHandler"].push_back(HandlerNode(h));
            }
            if (!x.Channels.empty())
                for (const auto& c : x.Channels)
                {
                    YAML::Node channel;
                    channel["Name"]     = c.Name;
                    channel["Layout"]   = c.Layout;
                    channel["Capacity"] = c.Capacity;
                    n["Channels"].push_back(channel);
                }
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
        x.Parameters = ReadParameters(n["Parameters"], "Emitter.");
        for (const auto& pair : n["DataInterfaces"])
            Check(x.DataInterfaces.emplace(pair.first.as<std::string>(), DecodeInterface(pair.second)).second,
                  "Duplicate emitter data interface parameter");
        std::set<std::string> simulationIDs;
        if (n["Stages"])
            for (const auto& node : n["Stages"]["SimulationStage"])
            {
                auto stage = ReadSimulation(node);
                Check(simulationIDs.insert(stage.Guid).second, "Duplicate simulation stage GUID: " + stage.Guid);
                x.SimulationStages.push_back(stage);
            }
        std::set<std::string> handlerIDs, channelNames;
        if (n["Stages"])
            for (const auto& h : n["Stages"]["EventHandler"])
            {
                auto handler = ReadHandler(h);
                Check(handlerIDs.insert(handler.Guid).second, "Duplicate VFX handler GUID: " + handler.Guid);
                x.Handlers.push_back(handler);
            }
        for (const auto& c : n["Channels"])
        {
            EventChannel channel {
                c["Name"].as<std::string>(), c["Layout"].as<std::string>(), c["Capacity"].as<uint32_t>(1024)};
            Check(!channel.Name.empty() && channelNames.insert(channel.Name).second, "Duplicate or empty VFX channel");
            Check(channel.Capacity > 0 && channel.Capacity <= 16777216, "Invalid VFX event channel capacity");
            x.Channels.push_back(channel);
        }
        x.Attributes = ReadParameters(n["Attributes"], "Particles.");
        std::set<std::string> ids;
        for (const auto& r : n["Renderers"])
        {
            auto v = ReadRenderer(r);
            Check(ids.insert(v.Guid).second, "Duplicate VFX renderer GUID");
            x.Renderers.push_back(v);
        }
        x.Overrides = ReadOverrides(n["Overrides"]);
        Check(!x.Parent.IsValid() || (x.Stages.empty() && x.Attributes.empty() && x.Renderers.empty() &&
                                      x.Handlers.empty() && x.Channels.empty() && x.SimulationStages.empty() &&
                                      x.DataInterfaces.empty() && x.Parameters.empty()),
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
    std::optional<std::string> SimulationStageError(const SimulationStage& s)
    {
        if (s.Guid.empty() || s.Name.empty())
            return std::string("Simulation stage requires GUID and Name");
        if (s.Iterations < 1 || s.Iterations > 64)
            return "Simulation Iterations must be 1–64: " + s.Guid;
        return std::nullopt;
    }
    std::string MoveAcrossStagesError(const std::string& use, Stage to)
    {
        return "VFX module " + use + " cannot move to " + StageName(to) + ": the stage is not in its AllowedStages";
    }
    namespace
    {
        // The stack `kind` names (the handler or simulation stage `container` for those stages).
        Stack* EmitterStack(Emitter& e, Stage kind, const std::string& container)
        {
            if (kind == Stage::EventHandler)
            {
                auto h = std::find_if(
                    e.Handlers.begin(), e.Handlers.end(), [&](const auto& h) { return h.Guid == container; });
                Check(h != e.Handlers.end(), "Missing VFX handler override target: " + container);
                return &h->Modules;
            }
            if (kind == Stage::SimulationStage)
            {
                auto s = std::find_if(e.SimulationStages.begin(), e.SimulationStages.end(), [&](const auto& s) {
                    return s.Guid == container;
                });
                Check(s != e.SimulationStages.end(), "Missing simulation stage override target: " + container);
                e.Stages.erase(Stage::SimulationStage);
                return &s->Modules;
            }
            return &e.Stages[kind];
        }
        // The stack holding the use `guid`, wherever it is now.
        Stack* StackHolding(Emitter& e, const std::string& guid)
        {
            const auto holds = [&](const Stack& v) {
                return std::any_of(v.begin(), v.end(), [&](const auto& x) { return x.Guid == guid; });
            };
            for (auto& [kind, v] : e.Stages)
                if (kind != Stage::SimulationStage && holds(v))
                    return &v;
            for (auto& h : e.Handlers)
                if (holds(h.Modules))
                    return &h.Modules;
            for (auto& s : e.SimulationStages)
                if (holds(s.Modules))
                    return &s.Modules;
            return nullptr;
        }
    } // namespace
    void ApplyOverrides(Emitter& e, const std::vector<Override>& overrides, const ModuleResolver* modules)
    {
        for (const auto& o : overrides)
        {
            if (o.Action == Override::Kind::MoveRenderer)
            {
                auto& v  = e.Renderers;
                auto  at = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Target; });
                Check(at != v.end(), "Missing VFX renderer override target: " + o.Target);
                auto item = *at;
                v.erase(at);
                auto before = std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.Guid == o.Before; });
                Check(o.Before.empty() || before != v.end(), "Missing VFX renderer reorder target: " + o.Before);
                v.insert(before, item);
                continue;
            }
            if (o.Action >= Override::Kind::AddSimulationStage)
            {
                auto& v  = e.SimulationStages;
                auto  at = std::find_if(v.begin(), v.end(), [&](const auto& s) { return s.Guid == o.Target; });
                if (o.Action == Override::Kind::AddSimulationStage)
                {
                    Check(!o.Simulation.Guid.empty() &&
                              std::none_of(
                                  v.begin(), v.end(), [&](const auto& s) { return s.Guid == o.Simulation.Guid; }),
                          "Duplicate simulation stage GUID: " + o.Simulation.Guid);
                    auto before = std::find_if(v.begin(), v.end(), [&](const auto& s) { return s.Guid == o.Before; });
                    Check(o.Before.empty() || before != v.end(),
                          "Missing simulation stage reorder target: " + o.Before);
                    v.insert(before, o.Simulation);
                }
                else
                {
                    Check(at != v.end(), "Missing simulation stage override target: " + o.Target);
                    if (o.Action == Override::Kind::RemoveSimulationStage)
                        v.erase(at);
                    else if (o.Action == Override::Kind::ReplaceSimulationStage)
                    {
                        Check(o.Simulation.Guid == o.Target, "Simulation stage replacement must preserve GUID");
                        *at = o.Simulation;
                    }
                    else
                    {
                        auto item = *at;
                        v.erase(at);
                        auto before =
                            std::find_if(v.begin(), v.end(), [&](const auto& s) { return s.Guid == o.Before; });
                        Check(o.Before.empty() || before != v.end(),
                              "Missing simulation stage reorder target: " + o.Before);
                        v.insert(before, item);
                    }
                }
                continue;
            }
            if (o.Action >= Override::Kind::AddHandler ||
                (o.StageKind == Stage::EventHandler && o.HandlerGuid.empty() &&
                 (o.Action == Override::Kind::SetEnabled || o.Action == Override::Kind::SetInput)))
            {
                auto& v  = e.Handlers;
                auto  at = std::find_if(v.begin(), v.end(), [&](const auto& h) { return h.Guid == o.Target; });
                if (o.Action == Override::Kind::AddHandler)
                {
                    Check(std::none_of(v.begin(), v.end(), [&](const auto& h) { return h.Guid == o.Handler.Guid; }),
                          "Duplicate VFX handler GUID: " + o.Handler.Guid);
                    auto before = std::find_if(v.begin(), v.end(), [&](const auto& h) { return h.Guid == o.Before; });
                    Check(o.Before.empty() || before != v.end(), "Missing VFX handler reorder target: " + o.Before);
                    v.insert(before, o.Handler);
                }
                else
                {
                    Check(at != v.end(), "Missing VFX handler override target: " + o.Target);
                    if (o.Action == Override::Kind::RemoveHandler)
                        v.erase(at);
                    else if (o.Action == Override::Kind::SetEnabled)
                        at->Enabled = o.Enabled;
                    else if (o.Action == Override::Kind::SetInput)
                    {
                        Check(o.InputValue.Kind == InputKind::Literal && o.InputValue.Literal.Kind == Type::Int &&
                                  o.InputValue.Literal.Integer >= 0,
                              "Handler input requires nonnegative int");
                        if (o.InputName == "SpawnNumber")
                            at->SpawnNumber = uint32_t(o.InputValue.Literal.Integer);
                        else if (o.InputName == "MaxEventsPerFrame")
                            at->MaxEventsPerFrame = uint32_t(o.InputValue.Literal.Integer);
                        else
                            Check(false, "Unknown VFX handler input: " + o.InputName);
                    }
                    else
                    {
                        auto item = *at;
                        v.erase(at);
                        auto before =
                            std::find_if(v.begin(), v.end(), [&](const auto& h) { return h.Guid == o.Before; });
                        Check(o.Before.empty() || before != v.end(), "Missing VFX handler reorder target: " + o.Before);
                        v.insert(before, item);
                    }
                }
                continue;
            }
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

            Stack* stack = EmitterStack(e,
                                        o.StageKind,
                                        o.StageKind == Stage::EventHandler ? o.HandlerGuid : o.SimulationStageGuid);
            auto   at = std::find_if(stack->begin(), stack->end(), [&](const auto& x) { return x.Guid == o.Target; });
            if (o.Action != Override::Kind::AddModule && at == stack->end())
            {
                // GUID-keyed: the use may have moved to another stack since the override was written.
                if (auto* holder = StackHolding(e, o.Target))
                {
                    stack = holder;
                    at    = std::find_if(stack->begin(), stack->end(), [&](const auto& x) { return x.Guid == o.Target; });
                }
            }
            auto& v = *stack;
            if (o.Action == Override::Kind::MoveModule && o.ToStage)
            {
                Check(at != v.end(), "Missing VFX module override target: " + o.Target);
                Check(*o.ToStage >= Stage::EmitterSpawn, "VFX module move cannot target a system stage");
                if (modules)
                {
                    Module m;
                    try
                    {
                        m = (*modules)(at->Script);
                    }
                    catch (const std::exception&)
                    {
                        m.AllowedStages = ~0u; // An unresolved script is reported by the compiler.
                    }
                    Check((m.AllowedStages & (1u << uint32_t(*o.ToStage))) != 0,
                          MoveAcrossStagesError(o.Target, *o.ToStage));
                }
                auto item = *at;
                v.erase(at);
                auto& to     = *EmitterStack(e, *o.ToStage, o.ToContainer);
                auto  before = std::find_if(to.begin(), to.end(), [&](const auto& x) { return x.Guid == o.Before; });
                Check(o.Before.empty() || before != to.end(), "Missing VFX reorder target: " + o.Before);
                to.insert(before, item);
                continue;
            }
            if (o.Action == Override::Kind::AddModule)
            {
                for (const auto& [s, st] : e.Stages)
                    Check(std::none_of(st.begin(), st.end(), [&](const auto& x) { return x.Guid == o.Added.Guid; }),
                          "Duplicate VFX usage GUID: " + o.Added.Guid);
                for (const auto& handler : e.Handlers)
                    Check(std::none_of(handler.Modules.begin(),
                                       handler.Modules.end(),
                                       [&](const auto& x) { return x.Guid == o.Added.Guid; }),
                          "Duplicate VFX usage GUID: " + o.Added.Guid);
                for (const auto& simulation : e.SimulationStages)
                    Check(std::none_of(simulation.Modules.begin(),
                                       simulation.Modules.end(),
                                       [&](const auto& x) { return x.Guid == o.Added.Guid; }),
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
    Emitter MergeEmitter(const Emitter& child, const EmitterResolver& resolve, const ModuleResolver* modules)
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
            ApplyOverrides(r, c.Overrides, modules);
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
        Register("EmitterParticles", {Type::Vec4, true});
        Register("Events", {Type::Vec4, true});
        Register("ForceFields", {Type::Vec3, true});
        Register("VectorField", {Type::Vec3, true});
        Register("Grid3D", {Type::Vec4, true});
        for (const char* s : {"SkeletalMesh", "StaticMesh"})
            Register(s, {Type::Vec4, true});
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
        add("Engine.MainLightDirection", Type::Vec4);
        add("Particles.LightTransmittance", Type::Float, glm::vec4(1));
        add("Particles.Position", Type::Position);
        add("Transient.PreviousPosition", Type::Position);
        add("Transient.CollisionMeshRadius", Type::Float, glm::vec4(-1));
        add("Particles.CollisionCount", Type::Int);
        add("Particles.HasCollided", Type::Bool);
        add("Particles.CollisionNormal", Type::Vec3);
        add("Particles.CollisionPosition", Type::Position);
        add("Particles.MeshTriangle", Type::Int);
        add("Particles.MeshBarycentric", Type::Vec3, {1, 0, 0, 0});
        add("Particles.Velocity", Type::Vec3);
        add("Particles.Color", Type::Color, glm::vec4(1));
        add("Particles.SpriteSize", Type::Vec2, glm::vec4(1));
        add("Particles.SpriteRotation", Type::Float);
        add("Particles.MeshOrientation", Type::Quat, {0, 0, 0, 1});
        add("Particles.Scale", Type::Vec3, glm::vec4(1));
        r["Particles.MeshIndex"] = {"Particles.MeshIndex", Type::Int, Value::Int(0), {}};
        r["Particles.RibbonID"]  = {"Particles.RibbonID", Type::Int, Value::Int(0), {}};
        add("Particles.RibbonLinkOrder", Type::Float);
        add("Particles.RibbonWidth", Type::Float, glm::vec4(1));
        add("Particles.RibbonTwist", Type::Float);
        add("Particles.RibbonFacing", Type::Vec3, {0, 0, 1, 0});
        add("Particles.RibbonUVDistance", Type::Float);
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
