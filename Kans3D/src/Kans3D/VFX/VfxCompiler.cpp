#include "kspch.h"
#include "VfxCompiler.h"
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <glm/gtc/matrix_inverse.hpp>
#include <shaderc/shaderc.hpp>
#include "Kans3D/Physics/PhysicsWorld.h"
namespace Kans::Vfx
{
#include "VfxCollision.inc"
#include "VfxMeshSampling.inc"
    namespace
    {
        float Noise3D(glm::vec3 position)
        {
            auto base = glm::ivec3(glm::floor(position));
            auto f    = glm::fract(position);
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
            return v;
        }
        bool VectorType(Type t) { return t != Type::Int && t != Type::Bool && t != Type::IVec3; }
        bool Widen(Type from, Type to) { return CanConvert(from, to); }
        const std::map<std::string, uint32_t> Arity = {{"Collision", 13},
                                                       {"ColliderVelocity", 2},
                                                       {"Add", 2},
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
                                                       {"Exp", 1},
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
                                                       {"CurlNoise", 4},
                                                       {"SampleVectorField", 1},
                                                       {"VectorFieldBounds", 0},
                                                       {"VectorFieldResolution", 0},
                                                       {"FieldPosition", 4},
                                                       {"FieldVector", 2},
                                                       {"EmitterSeedOffset", 0},
                                                       {"ToSimulationVector", 1},
                                                       {"ToWorldVector", 1},
                                                       {"SampleForceFields", 2},
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
                                                       {"Not", 1},
                                                       {"Mod", 2}};
        // One row per graph operation (next to Arity above): the node search's categories and
        // descriptions and the pins of a new node. Pin names are labels; the compiler reads pins by order.
        std::vector<OperationInfo> BuildOperations()
        {
            using T                = Type;
            const auto           N = [](std::string name) { return PinSpec {std::move(name), PinRule::Node, T::Float, {}, {}}; };
            const auto           F = [](std::string name, T kind) { return PinSpec {std::move(name), PinRule::Fixed, kind, {}, {}}; };
            const std::vector<T> floats {T::Float, T::Vec2, T::Vec3, T::Vec4};
            const std::vector<T> arithmetic {T::Float, T::Vec2, T::Vec3, T::Vec4, T::Color, T::Position, T::Int, T::IVec3};
            const std::vector<T> all {T::Float, T::Vec2, T::Vec3, T::Vec4, T::Int, T::Bool, T::Color, T::Quat, T::Position, T::IVec3};
            const std::vector<T> numbers {T::Float, T::Vec2, T::Vec3, T::Vec4, T::Int};
            std::vector<OperationInfo> r;
            const auto add = [&](std::string name, std::string category, std::string description, std::vector<T> kinds,
                                 std::vector<PinSpec> inputs, std::vector<PinSpec> outputs, bool parameter = false,
                                 bool code = false) {
                r.push_back({std::move(name), std::move(category), std::move(description), std::move(kinds),
                             std::move(inputs), std::move(outputs), parameter, code, false});
            };
            const PinSpec value = N("Value");
            // Math
            add("Add", "Math", "A + B, per component.", arithmetic, {N("A"), N("B")}, {value});
            add("Sub", "Math", "A - B, per component.", arithmetic, {N("A"), N("B")}, {value});
            add("Mul", "Math", "A * B, per component.", arithmetic, {N("A"), N("B")}, {value});
            add("Div", "Math", "A / B, per component.", {T::Float, T::Vec2, T::Vec3, T::Vec4, T::Color, T::Int}, {N("A"), N("B")}, {value});
            add("Min", "Math", "The smaller of A and B, per component.", numbers, {N("A"), N("B")}, {value});
            add("Max", "Math", "The larger of A and B, per component.", numbers, {N("A"), N("B")}, {value});
            add("Abs", "Math", "Absolute value.", numbers, {N("A")}, {value});
            add("Floor", "Math", "Rounds down to a whole number.", floats, {N("A")}, {value});
            add("Frac", "Math", "Fractional part: A - floor(A).", floats, {N("A")}, {value});
            add("Pow", "Math", "A raised to the power B.", floats, {N("A"), N("B")}, {value});
            add("Exp", "Math", "e raised to the power A.", floats, {N("A")}, {value});
            add("Sin", "Math", "Sine of an angle in radians.", floats, {N("A")}, {value});
            add("Cos", "Math", "Cosine of an angle in radians.", floats, {N("A")}, {value});
            add("Mod", "Math", "Integer remainder of A / B (0 when B is 0).", {T::Int}, {N("A"), N("B")}, {value});
            add("Lerp", "Math", "Blends from A to B by Amount.", {T::Float, T::Vec2, T::Vec3, T::Vec4, T::Color, T::Position},
                {N("A"), N("B"), F("Amount", T::Float)}, {value});
            add("Clamp", "Math", "Keeps Value between Min and Max.", numbers, {N("Value"), N("Min"), N("Max")}, {value});
            add("Saturate", "Math", "Clamps to the 0..1 range.", floats, {N("A")}, {value});
            add("Step", "Math", "1 where X >= Edge, else 0.", floats, {N("Edge"), N("X")}, {value});
            add("Smoothstep", "Math", "Smooth 0..1 ramp of X between Edge0 and Edge1.", floats, {N("Edge0"), N("Edge1"), N("X")}, {value});
            add("Less", "Math", "True when A < B (scalars).", {T::Bool}, {F("A", T::Float), F("B", T::Float)}, {value});
            add("LessEqual", "Math", "True when A <= B (scalars).", {T::Bool}, {F("A", T::Float), F("B", T::Float)}, {value});
            add("Greater", "Math", "True when A > B (scalars).", {T::Bool}, {F("A", T::Float), F("B", T::Float)}, {value});
            add("Equal", "Math", "True when A equals B (scalars).", {T::Bool}, {F("A", T::Int), F("B", T::Int)}, {value});
            add("Select", "Math", "True when Condition holds, else False.", all, {F("Condition", T::Bool), N("True"), N("False")}, {value});
            add("And", "Math", "True when both are true.", {T::Bool}, {N("A"), N("B")}, {value});
            add("Or", "Math", "True when either is true.", {T::Bool}, {N("A"), N("B")}, {value});
            add("Not", "Math", "Negates a bool.", {T::Bool}, {N("A")}, {value});
            add("Constant", "Math", "A fixed value.", all, {}, {value});
            // Vector
            add("Dot", "Vector", "Dot product of two vectors.", {T::Float}, {F("A", T::Vec3), F("B", T::Vec3)}, {value});
            add("Cross", "Vector", "Cross product of two vec3.", {T::Vec3}, {N("A"), N("B")}, {value});
            add("Normalize", "Vector", "Unit-length direction of a vector.", {T::Vec3, T::Vec2, T::Vec4}, {N("A")}, {value});
            add("Length", "Vector", "Length of a vector.", {T::Float}, {F("A", T::Vec3)}, {value});
            add("Make", "Vector", "Builds a vector or colour from components.", {T::Vec3, T::Vec2, T::Vec4, T::Color, T::IVec3}, {}, {value});
            add("Break", "Vector", "Splits a vector into its components.", {T::Float}, {F("A", T::Vec3)}, {N("X"), N("Y"), N("Z")});
            add("Splat", "Vector", "Copies a float into every component.", {T::Vec3, T::Vec2, T::Vec4, T::Color}, {F("A", T::Float)}, {value});
            add("ToInt", "Vector", "Converts a float to int (truncates).", {T::Int}, {F("A", T::Float)}, {value});
            add("ToFloat", "Vector", "Converts an int to float.", {T::Float}, {F("A", T::Int)}, {value});
            add("TransformPosition", "Vector", "Local position to world space.", {T::Position}, {N("Position")}, {value});
            add("InverseTransformPosition", "Vector", "World position to local space.", {T::Position}, {N("Position")}, {value});
            add("TransformVector", "Vector", "Local direction to world space.", {T::Vec3}, {N("Vector")}, {value});
            add("InverseTransformVector", "Vector", "World direction to local space.", {T::Vec3}, {N("Vector")}, {value});
            add("ToSimulationVector", "Vector", "World vector into the emitter's simulation space.", {T::Vec3}, {N("Vector")}, {value});
            add("ToWorldVector", "Vector", "Simulation-space vector to world space.", {T::Vec3}, {N("Vector")}, {value});
            add("FieldPosition", "Vector", "Position into a field's translated, rotated, scaled frame.", {T::Position},
                {F("Position", T::Position), F("Translate", T::Vec3), F("Rotation", T::Quat), F("Scale", T::Vec3)}, {value});
            add("FieldVector", "Vector", "Vector into a field's rotated frame.", {T::Vec3}, {F("Vector", T::Vec3), F("Rotation", T::Quat)}, {value});
            // Noise
            add("Random", "Noise", "Deterministic random value in 0..1 per particle.", floats, {}, {value});
            add("RandomRange", "Noise", "Deterministic random value between Min and Max.", floats, {N("Min"), N("Max")}, {value});
            add("RandomVector", "Noise", "Random vector with components in 0..1.", {T::Vec3, T::Vec2, T::Vec4}, {}, {value});
            add("RandomUnitVector", "Noise", "Random direction of length 1.", {T::Vec3}, {}, {value});
            add("Noise3D", "Noise", "Smooth value noise at a point.", {T::Float}, {F("Position", T::Vec3)}, {value});
            PinSpec octaves = F("Octaves", T::Int);
            octaves.Default = Value::Int(1);
            add("CurlNoise", "Noise", "Divergence-free curl noise vector (1-4 octaves).", {T::Vec3},
                {F("Position", T::Position), F("Frequency", T::Float), F("Offset", T::Vec3), octaves}, {value});
            add("EmitterSeedOffset", "Noise", "Per-emitter random offset for noise lookups.", {T::Vec3}, {}, {value});
            // Grid
            add("GridNumCells", "Grid", "Cell count of a Grid3D.", {T::IVec3}, {}, {value}, true);
            add("GridCellSize", "Grid", "World size of one Grid3D cell.", {T::Vec3}, {}, {value}, true);
            add("GridIndexToWorld", "Grid", "World position of a cell centre.", {T::Position}, {F("Index", T::IVec3)}, {value}, true);
            add("GridWorldToIndex", "Grid", "Cell index containing a world position.", {T::IVec3}, {F("Position", T::Position)}, {value}, true);
            add("GridWorldToCell", "Grid", "Continuous cell coordinate of a world position.", {T::Vec3}, {F("Position", T::Position)}, {value}, true);
            add("GridGet", "Grid", "Reads a grid attribute at a cell.", floats, {F("Index", T::IVec3)}, {value}, true, true);
            add("GridSet", "Grid", "Writes a grid attribute at the executing cell.", floats, {F("Index", T::IVec3), N("Value")}, {value}, true, true);
            add("GridSample", "Grid", "Trilinear sample of a grid attribute at a position.", floats, {F("Position", T::Position)}, {value}, true, true);
            add("GridAddFloat", "Grid", "Atomically adds to one component of a cell.", {T::Float},
                {F("Index", T::IVec3), F("Component", T::Int), F("Value", T::Float)}, {value}, true, true);
            PinSpec steps = F("Steps", T::Int);
            steps.Default = Value::Int(16);
            add("GridLightMarch", "Grid", "Marches density toward the light; returns transmittance.", {T::Float},
                {F("Index", T::IVec3), F("Light", T::Vec4), F("Extinction", T::Float), steps, F("StepLength", T::Float)}, {value}, true, true);
            // Sampling
            add("Sample", "Sampling", "Samples a curve, gradient or texture data interface.", {T::Float, T::Color, T::Vec2, T::Vec3, T::Vec4},
                {F("Coordinate", T::Float)}, {value}, true);
            add("SampleVectorField", "Sampling", "Samples a vector field at a field-local position.", {T::Vec3}, {F("Position", T::Position)}, {value}, true);
            add("VectorFieldBounds", "Sampling", "Local bounds of a vector field.", {T::Vec3}, {}, {N("Min"), N("Max")}, true);
            add("VectorFieldResolution", "Sampling", "Cell counts of a vector field.", {T::Int}, {}, {N("X"), N("Y"), N("Z")}, true);
            add("SampleForceFields", "Sampling", "Scene wind and force fields at a position.", {T::Vec3},
                {F("Position", T::Position), F("Velocity", T::Vec3)}, {N("Wind"), N("Force"), N("Acceleration"), N("Impulse")});
            PinSpec mode = F("Mode", T::Int);
            mode.Enum    = {"SceneQueries", "DepthBuffer", "AnalyticShapes"};
            mode.Default = Value::Int(2);
            add("Collision", "Sampling", "Collides particles with the scene and returns the response.", {T::Position},
                {F("Previous", T::Position), F("Position", T::Position), F("Velocity", T::Vec3), F("Size", T::Vec2),
                 F("Scale", T::Vec3), mode, F("Restitution", T::Float), F("Friction", T::Float), F("RadiusScale", T::Float),
                 F("RadiusOverride", T::Float), F("RestingVelocity", T::Float), F("DeltaTime", T::Float), F("MeshRadius", T::Float)},
                {F("Position", T::Position), F("Velocity", T::Vec3), F("Normal", T::Vec3), F("Contact", T::Position), F("Hit", T::Bool)});
            add("ColliderVelocity", "Sampling", "Surface velocity of the collider covering a point (GPU).", {T::Vec4},
                {F("Position", T::Position), F("Radius", T::Float)}, {value});
            add("MainLightDirection", "Sampling", "Direction and intensity of the main directional light (GPU).", {T::Vec4}, {}, {value});
            const std::vector<PinSpec> surface {F("Position", T::Position), F("Normal", T::Vec3), F("MeshVelocity", T::Vec3),
                                                F("Color", T::Color), F("Valid", T::Bool)};
            add("RandomTriangle", "Sampling", "Random triangle of a mesh, area weighted.", {T::Int}, {}, {value}, true);
            add("TriangleCount", "Sampling", "Triangle count of a mesh.", {T::Int}, {}, {value}, true);
            add("SampleTriangle", "Sampling", "Surface data at barycentric coordinates of a triangle.", {T::Position},
                {F("Triangle", T::Int), F("Barycentric", T::Vec3)}, surface, true);
            add("SampleSkinnedTriangle", "Sampling", "Skinned surface data of a triangle.", {T::Position},
                {F("Triangle", T::Int), F("Barycentric", T::Vec3)}, surface, true);
            add("RandomVertex", "Sampling", "Random vertex index of a mesh.", {T::Int}, {}, {value}, true);
            add("VertexCount", "Sampling", "Vertex count of a mesh.", {T::Int}, {}, {value}, true);
            add("SampleVertex", "Sampling", "Data of one mesh vertex.", {T::Position}, {F("Index", T::Int)},
                {F("Position", T::Position), F("Normal", T::Vec3), F("Tangent", T::Vec4), F("UV0", T::Vec2), F("Color", T::Color), F("Valid", T::Bool)}, true);
            add("RandomBone", "Sampling", "Random bone index of a skeletal mesh.", {T::Int}, {}, {value}, true);
            add("BoneCount", "Sampling", "Bone count of a skeletal mesh.", {T::Int}, {}, {value}, true);
            add("SampleBone", "Sampling", "Position and rotation of one bone.", {T::Position}, {F("Index", T::Int)},
                {F("Position", T::Position), F("Rotation", T::Quat), F("Velocity", T::Vec3), F("Valid", T::Bool)}, true);
            add("BoneByName", "Sampling", "Index of the bone named in Code.", {T::Int}, {}, {value}, true, true);
            // Events
            add("WriteEvent", "Events", "Writes Count events with the payload pins to a channel.", {T::Int},
                {F("Count", T::Int), F("Position", T::Position)}, {value}, true);
            add("ParticleCount", "Events", "Particle count of another emitter.", {T::Int}, {}, {value}, true);
            add("ReadAttribute", "Events", "Reads another emitter's particle attribute by index.", all, {F("Index", T::Int)}, {value}, true, true);
            add("ReadAttributeByID", "Events", "Reads another emitter's particle attribute by unique ID.", all,
                {F("ID", T::Int)}, {value, F("Found", T::Bool)}, true, true);
            // Custom
            add("CustomGLSL", "Custom", "A GLSL expression or body; $ArgN names input N (GPU only).", floats, {F("Arg0", T::Float)}, {value}, false, true);
            add("Call", "Custom", "Calls a function script with its inputs.", floats, {}, {value});
            // Parameters
            add("Input", "Parameters", "Reads a module input (Module.*).", all, {}, {value}, true);
            add("Get", "Parameters", "Reads a parameter (Particles, Emitter, System, Engine...).", all, {}, {value}, true);
            add("Set", "Parameters", "Writes a parameter.", all, {N("Value")}, {value}, true);
            add("Reroute", "Utility", "Passes a value through, to route links.", all, {N("Value")}, {value});
            r.back().Hidden = true;
            return r;
        }
        std::string                           GType(Type);
        void                                  SpecializeGridModule(Module&                                        m,
                                                                   const std::map<std::string, Input>&            inputs,
                                                                   const std::map<std::string, InterfaceBinding>* Grids)
        {
            if (m.Identity == AssetID::FromString("732d200000000009:0000000000000001"))
            {
                auto target = inputs.find("Module.Target");
                if (target != inputs.end() && target->second.Kind == InputKind::Link)
                    for (auto& node : m.Nodes)
                        if (node.Op == "Set" && node.Parameter == "Particles.LightTransmittance")
                            node.Parameter = target->second.Link;
            }
            static const std::set<AssetID> smokeModules = {AssetID::FromString("732d200000000001:0000000000000001"),
                                                           AssetID::FromString("732d200000000002:0000000000000001"),
                                                           AssetID::FromString("732d200000000003:0000000000000001"),
                                                           AssetID::FromString("732d200000000004:0000000000000001"),
                                                           AssetID::FromString("732d200000000005:0000000000000001"),
                                                           AssetID::FromString("732d200000000006:0000000000000001"),
                                                           AssetID::FromString("732d200000000008:0000000000000001")};
            if (smokeModules.count(m.Identity))
            {
                // Secondary attributes default to the selected grid, but accept explicit bindings.
                const auto binding = inputs.find("Module.Grid");
                if (Grids && binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                {
                    const auto grid = Grids->find(binding->second.Interface.Source);
                    if (grid != Grids->end() && m.Identity == AssetID::FromString("732d200000000004:0000000000000001"))
                        for (const auto& attribute : grid->second.GridAttributes)
                            if (attribute.Name == binding->second.Interface.Channel)
                                for (auto& node : m.Nodes)
                                    if ((node.Op == "GridSample" || node.Op == "GridSet") &&
                                        node.Parameter == "Module.Grid")
                                    {
                                        node.Kind            = attribute.Kind;
                                        node.Outputs[0].Kind = attribute.Kind;
                                        if (node.Op == "GridSet")
                                            node.Inputs[1].Kind = attribute.Kind;
                                    }
                    for (auto& node : m.Nodes)
                        for (const auto& [parameter, attribute] :
                             std::map<std::string, std::string> {{"Module.Velocity", "Velocity"},
                                                                 {"Module.Accumulation", "Splat"},
                                                                 {"Module.Occupancy", "Occupancy"},
                                                                 {"Module.Density", "Density"}})
                            if (node.Parameter == parameter && !inputs.count(parameter))
                            {
                                node.Parameter = "Module.Grid";
                                node.Code      = attribute;
                            }
                    if (grid != Grids->end() && !inputs.count("Module.Occupancy") &&
                        std::none_of(grid->second.GridAttributes.begin(),
                                     grid->second.GridAttributes.end(),
                                     [](const auto& a) { return a.Name == "Occupancy"; }))
                        m.Nodes.erase(
                            std::remove_if(m.Nodes.begin(),
                                           m.Nodes.end(),
                                           [](const auto& n) { return n.Op == "GridSet" && n.Code == "Occupancy"; }),
                            m.Nodes.end());
                    m.Links.erase(std::remove_if(m.Links.begin(),
                                                 m.Links.end(),
                                                 [&](const auto& l) {
                                                     return std::none_of(m.Nodes.begin(),
                                                                         m.Nodes.end(),
                                                                         [&](const auto& n) { return n.Guid == l.To; });
                                                 }),
                                  m.Links.end());
                }
            }
            // Grid module graphs are attribute-polymorphic: infer their value branch from the binding.
            // Coordinate branches and scalar controls (Rate, Amount, dt) retain their declared types.
            static const auto              clearID = AssetID::FromString("c0d609763e645cfd:0000000000000001");
            static const std::set<AssetID> modules = {clearID,
                                                      AssetID::FromString("233b8dd42ad050aa:0000000000000001"),
                                                      AssetID::FromString("b5f3f10bb437548f:0000000000000001"),
                                                      AssetID::FromString("5e23b1e61a875ac1:0000000000000001")};
            if (modules.count(m.Identity))
            {
                auto binding = inputs.find("Module.Grid");
                if (Grids && binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                {
                    auto grid = Grids->find(binding->second.Interface.Source);
                    if (grid != Grids->end())
                    {
                        // GridClear accepts a selected attribute list; an empty channel/list clears the collection.
                        if (m.Identity == clearID && (binding->second.Interface.Channel.empty() ||
                                                      !binding->second.Interface.GridAttributes.empty()))
                        {
                            const auto attributes = binding->second.Interface.GridAttributes.empty() ?
                                                        grid->second.GridAttributes :
                                                        binding->second.Interface.GridAttributes;
                            const auto nodes      = m.Nodes;
                            const auto links      = m.Links;
                            m.Nodes.clear();
                            m.Links.clear();
                            for (const auto& attribute : attributes)
                            {
                                auto selected = std::find_if(grid->second.GridAttributes.begin(),
                                                             grid->second.GridAttributes.end(),
                                                             [&](const auto& a) { return a.Name == attribute.Name; });
                                const auto type =
                                    selected == grid->second.GridAttributes.end() ? Type::Float : selected->Kind;
                                const auto suffix = "/" + attribute.Name;
                                for (auto node : nodes)
                                {
                                    node.Guid += suffix;
                                    if (node.Op == "GridSet")
                                    {
                                        node.Code            = attribute.Name;
                                        node.Kind            = type;
                                        node.Inputs[1].Kind  = type;
                                        node.Outputs[0].Kind = type;
                                    }
                                    m.Nodes.push_back(node);
                                }
                                for (auto link : links)
                                {
                                    link.From += suffix;
                                    link.To += suffix;
                                    m.Links.push_back(link);
                                }
                            }
                            return;
                        }
                        auto attr =
                            std::find_if(grid->second.GridAttributes.begin(),
                                         grid->second.GridAttributes.end(),
                                         [&](const auto& a) { return a.Name == binding->second.Interface.Channel; });
                        if (attr != grid->second.GridAttributes.end())
                        {
                            std::set<std::string> values;
                            for (auto& p : m.Inputs)
                                if (p.Name == "Module.Value")
                                {
                                    p.Kind = attr->Kind;
                                    if (p.Default)
                                        p.Default = Value::Vector(glm::vec4(p.Default->Data.x), attr->Kind);
                                }
                            for (auto& n : m.Nodes)
                            {
                                bool value = n.Op == "GridGet" || n.Op == "GridSample" || n.Op == "GridSet" ||
                                             (n.Op == "Input" && n.Parameter == "Module.Value");
                                for (const auto& l : m.Links)
                                    if (l.To == n.Guid && values.count(l.From))
                                        value = true;
                                if (!value || n.Kind == Type::IVec3)
                                    continue;
                                values.insert(n.Guid);
                                n.Kind = attr->Kind;
                                for (auto& p : n.Outputs)
                                    p.Kind = attr->Kind;
                                for (size_t k = 0; k < n.Inputs.size(); ++k)
                                    if (n.Inputs[k].Kind == Type::Float && !(n.Op == "Lerp" && k == 2))
                                        n.Inputs[k].Kind = attr->Kind;
                            }
                        }
                    }
                }
            }
        }
        struct Compiler
        {
            const ModuleResolver&                          Resolve;
            std::map<std::string, Parameter>               Known;
            std::vector<Diagnostic>&                       Errors;
            Program                                        Out;
            std::vector<AssetID>                           Calls;
            AssetID                                        Asset;
            Space                                          SimulationSpace = Space::Local;
            MeshResolver                                   Meshes;
            SkeletonResolver                               Skeletons;
            std::map<std::string, InterfaceBinding>        MeshCache;
            std::map<std::string, InterfaceBinding>*       SharedMeshCache = nullptr;
            const std::map<std::string, InterfaceBinding>* Grids           = nullptr;
            std::string                                    Scope; // The call scope of Inline, for diagnostics.
            uint32_t GridResource(const std::string& name, const InterfaceBinding& grid)
            {
                for (uint32_t n = 0; n < Out.Resources.size(); ++n)
                    if (Out.Resources[n].Kind == "Grid3D" && Out.Resources[n].Channel == name)
                        return n;
                ResourceBinding resource;
                resource.Kind    = "Grid3D";
                resource.Channel = name;
                resource.Grid    = grid;
                resource.Binding = 7;
                for (const auto& r : Out.Resources)
                    resource.Binding = std::max(resource.Binding, r.Binding + (r.Kind == "Grid3D" ? 3u : 1u));
                Out.Resources.push_back(resource);
                return uint32_t(Out.Resources.size() - 1);
            }
            uint32_t Emit(Instruction i)
            {
                if (i.Kind == Type::Position && (i.Op == "ReadAttribute" || i.Op == "ReadAttributeByID"))
                    i.PositionSpace = SimulationSpace;
                else if (i.Kind == Type::Position && i.Op != "Get" && i.Op != "Set")
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
                if ((i.Op == "SampleForceFields" || i.Op == "CurlNoise") && !i.Args.empty())
                    i.PositionSpace = Out.Code[i.Args[0]].PositionSpace;
                if (i.Op == "Collision" && !i.Args.empty())
                    i.PositionSpace = Out.Code[i.Args[0]].PositionSpace;
                if (i.Op == "GridIndexToWorld")
                    i.PositionSpace = Space::World;
                if (i.Op == "ToSimulationVector")
                    i.PositionSpace = SimulationSpace;
                if (i.Op == "ToWorldVector")
                    i.PositionSpace = SimulationSpace;
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
                Errors.back().Scope = Scope;
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
                if (name == "Grid.ExecutionIndex")
                {
                    if (Out.IterationGrid.empty() || type != Type::IVec3)
                        Error(asset, node, pin, "Grid.ExecutionIndex requires a grid simulation stage and ivec3");
                    Instruction i;
                    i.Op   = "GridExecutionIndex";
                    i.Kind = Type::IVec3;
                    return Emit(i);
                }
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
                if (name.rfind("Transient.Event.", 0) == 0 && Out.StageKind != Stage::EventHandler)
                {
                    Error(asset, node, pin, "Transient.Event payload is only readable in an EventHandler");
                    return Constant(Value::Vector({}, type));
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
                       (n.rfind("Particles.", 0) == 0 &&
                        (s == Stage::ParticleSpawn || s == Stage::ParticleUpdate || s == Stage::EventHandler ||
                         (s == Stage::SimulationStage && Out.IterationGrid.empty()))) ||
                       (n.rfind("Emitter.", 0) == 0 && (s == Stage::EmitterSpawn || s == Stage::EmitterUpdate)) ||
                       (n.rfind("System.", 0) == 0 && (s == Stage::SystemSpawn || s == Stage::SystemUpdate));
            }
            uint32_t Inline(AssetID                                id,
                            Usage                                  expected,
                            const std::map<std::string, Input>&    inputs,
                            const std::string&                     scope,
                            const std::map<std::string, uint32_t>* arguments = nullptr)
            {
                struct ScopeGuard
                {
                    std::string& Target;
                    std::string  Saved;
                    ~ScopeGuard() { Target = std::move(Saved); }
                } guard {Scope, Scope};
                Scope = scope;
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
                SpecializeGridModule(m, inputs, Grids);
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
                            v = Grids && Grids->count(x.Link) ? Constant(Value::Vector({}, p.Kind)) :
                                                                Get(x.Link, p.Kind, id, "", p.Name);
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
                        auto input = inputs.find(n.Parameter);
                        // An Input node feeding nothing reads nothing: a curve module may keep one
                        // next to the Sample node bound to the same input.
                        const bool feeds = std::any_of(m.Links.begin(), m.Links.end(), [&](const Link& l) { return l.From == guid; });
                        if (input != inputs.end() && input->second.Kind == InputKind::DataInterface && feeds)
                            Error(id, guid, output, "Input cannot read DataInterface parameter: " + n.Parameter);
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
                    else if (n.Op == "MainLightDirection")
                    {
                        if (Out.Target != SimTarget::GPU)
                            Error(id, guid, output, "MainLightDirection requires GPU; switch the emitter to GPU");
                        if (!i.Args.empty() || i.Kind != Type::Vec4)
                            Error(id, guid, output, "MainLightDirection requires vec4 output and no inputs");
                        reg = Get("Engine.MainLightDirection", Type::Vec4, id, guid, output);
                    }
                    else if (n.Op == "Get")
                    {
                        if (n.Parameter == "Engine.MainLightDirection" && Out.Target != SimTarget::GPU)
                            Error(id, guid, output, "MainLightDirection requires GPU; switch the emitter to GPU");
                        reg = Get(Scoped(n.Parameter, scope), i.Kind, id, guid, output);
                    }
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
                    else if (n.Op == "WriteEvent" || n.Op == "ParticleCount" || n.Op == "ReadAttribute" ||
                             n.Op == "ReadAttributeByID")
                    {
                        auto binding = inputs.find(n.Parameter);
                        if (binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                            i.Interface = binding->second.Interface;
                        i.Code = n.Code; // Attribute name for particle-reader operations.
                        if (n.Op == "WriteEvent")
                        {
                            if (Out.StageKind != Stage::ParticleUpdate)
                                Error(id, guid, output, "WriteEvent requires ParticleUpdate");
                            if (i.Interface.Channel.empty())
                                i.Interface.Channel = n.Parameter;
                            if (i.Args.empty() || Out.Code[i.Args[0]].Kind != Type::Int)
                                Error(id, guid, "Count", "WriteEvent requires an int count");
                        }
                        else
                        {
                            if (i.Interface.Kind != "EmitterParticles")
                                Error(id, guid, output, "Particle reader requires EmitterParticles binding");
                            if (n.Op == "ParticleCount")
                            {
                                if (!i.Args.empty() || i.Kind != Type::Int)
                                    Error(id, guid, output, "ParticleCount requires int output and no inputs");
                            }
                            else if (i.Args.size() != 1 || Out.Code[i.Args[0]].Kind != Type::Int)
                                Error(id, guid, "Index", "Particle reader requires one int index or unique ID");
                            if (n.Op == "ReadAttributeByID" && output == "Found" && i.Kind != Type::Bool)
                                Error(id, guid, output, "Particle reader Found output requires bool");
                        }
                        if (n.Op == "WriteEvent")
                            for (uint32_t a = 1; a < i.Args.size(); ++a)
                                if (Out.Code[i.Args[a]].Kind == Type::Position)
                                    i.Args[a] = PositionTo(i.Args[a], SimulationSpace);
                        reg = Emit(i);
                    }
                    else if (n.Op.rfind("Grid", 0) == 0)
                    {
                        std::string name      = n.Parameter;
                        auto        binding   = inputs.find(n.Parameter);
                        std::string attribute = n.Code;
                        if (binding != inputs.end())
                        {
                            if (binding->second.Kind == InputKind::Link)
                                name = binding->second.Link;
                            else if (binding->second.Kind == InputKind::DataInterface)
                            {
                                name = binding->second.Interface.Source;
                                if (attribute.empty())
                                    attribute = binding->second.Interface.Channel;
                            }
                        }
                        auto grid =
                            Grids ? Grids->find(name) : std::map<std::string, InterfaceBinding>::const_iterator {};
                        if (!Grids || grid == Grids->end() || grid->second.Kind != "Grid3D")
                        {
                            Error(id, guid, "Grid", "Grid operation requires a Grid3D emitter parameter: " + name);
                            reg = Constant(Value::Vector({}, i.Kind));
                        }
                        else
                        {
                            i.Interface        = grid->second;
                            i.Interface.Source = name;
                            i.Code             = attribute;
                            i.Resource         = GridResource(name, grid->second);
                            if (Out.Target != SimTarget::GPU)
                                Error(id, guid, "Grid", "Grid3D requires GPU; switch the emitter to GPU");
                            const bool set = n.Op == "GridSet", add = n.Op == "GridAddFloat";
                            const bool sample = n.Op == "GridSample";
                            const bool march  = n.Op == "GridLightMarch";
                            const bool attr   = set || add || sample || march || n.Op == "GridGet";
                            auto       at     = std::find_if(i.Interface.GridAttributes.begin(),
                                                             i.Interface.GridAttributes.end(),
                                                             [&](const auto& a) { return a.Name == attribute; });
                            if (attr)
                            {
                                if (at == i.Interface.GridAttributes.end())
                                    Error(id, guid, "Attribute", "Unknown Grid3D attribute: " + attribute);
                                else
                                {
                                    i.Parameter = uint32_t(at - i.Interface.GridAttributes.begin());
                                    if (!add && i.Kind != at->Kind)
                                        Error(id, guid, set ? "Value" : output, "Grid attribute type mismatch");
                                }
                            }
                            const uint32_t count = march ? 5 :
                                                   set   ? 2 :
                                                   add   ? 3 :
                                                   (attr || n.Op == "GridIndexToWorld" || n.Op == "GridWorldToIndex" ||
                                                    n.Op == "GridWorldToCell") ?
                                                         1 :
                                                         0;
                            if (i.Args.size() != count)
                                Error(id, guid, "Index", "Wrong input count for " + n.Op);
                            if (count && !i.Args.empty())
                            {
                                auto expected = sample || n.Op == "GridWorldToIndex" || n.Op == "GridWorldToCell" ?
                                                    Type::Position :
                                                    Type::IVec3;
                                if (Out.Code[i.Args[0]].Kind != expected)
                                    Error(id,
                                          guid,
                                          expected == Type::Position ? "Position" : "Index",
                                          "Grid coordinate type mismatch");
                                if (expected == Type::Position)
                                    i.Args[0] = PositionTo(i.Args[0], Space::World);
                            }
                            if (set)
                            {
                                if (Out.StageKind != Stage::SimulationStage || Out.IterationGrid != name)
                                    Error(id,
                                          guid,
                                          "Grid",
                                          "GridSet requires a simulation stage whose iteration source is this grid");
                                else if (!i.Args.empty() && Out.Code[i.Args[0]].Op != "GridExecutionIndex")
                                    Error(id, guid, "Index", "GridSet may write only at Grid.ExecutionIndex");
                                if (i.Args.size() == 2 && Out.Code[i.Args[1]].Kind != i.Kind)
                                    Error(id, guid, "Value", "GridSet value type mismatch");
                            }
                            if (add)
                            {
                                if (Out.StageKind != Stage::SimulationStage || !Out.IterationGrid.empty())
                                    Error(id, guid, "Grid", "GridAddFloat requires a particle simulation stage");
                                if (i.Args.size() == 3)
                                {
                                    const auto& component = Out.Code[i.Args[1]];
                                    if (component.Op != "Constant" || component.Kind != Type::Int ||
                                        component.Literal.Integer < 0 || at == i.Interface.GridAttributes.end() ||
                                        uint32_t(component.Literal.Integer) >= Components(at->Kind))
                                        Error(
                                            id, guid, "Component", "GridAddFloat requires a constant valid component");
                                    if (Out.Code[i.Args[2]].Kind != Type::Float || i.Kind != Type::Float)
                                        Error(id, guid, "Value", "GridAddFloat requires float");
                                }
                            }
                            if (march)
                            {
                                if (Out.StageKind != Stage::SimulationStage || Out.IterationGrid != name)
                                    Error(id, guid, "Grid", "GridLightMarch requires a grid simulation stage");
                                if (i.Args.size() == 5)
                                {
                                    const auto& steps = Out.Code[i.Args[3]];
                                    if (steps.Kind != Type::Int ||
                                        (steps.Op == "Constant" &&
                                         (steps.Literal.Integer < 1 || steps.Literal.Integer > 64)))
                                        Error(id, guid, "Steps", "Steps must be in [1, 64]");
                                    for (auto [arg, pin] :
                                         {std::pair<size_t, const char*> {2, "Extinction"}, {4, "StepLength"}})
                                    {
                                        const auto& v = Out.Code[i.Args[arg]];
                                        if (v.Kind != Type::Float ||
                                            (v.Op == "Constant" &&
                                             (!std::isfinite(v.Literal.Data.x) || v.Literal.Data.x < 0)))
                                            Error(id, guid, pin, std::string(pin) + " must be >= 0");
                                    }
                                    if (Out.Code[i.Args[1]].Kind != Type::Vec4 || i.Kind != Type::Float)
                                        Error(id, guid, output, "GridLightMarch requires vec4 light and float density");
                                }
                            }
                            if (sample && Out.StageKind != Stage::ParticleUpdate &&
                                Out.StageKind != Stage::SimulationStage)
                                Error(id, guid, "Position", "GridSample requires ParticleUpdate or a simulation stage");
                            if ((n.Op == "GridNumCells" || n.Op == "GridWorldToIndex") && i.Kind != Type::IVec3)
                                Error(id, guid, output, "Grid index output requires ivec3");
                            if (n.Op == "GridCellSize" && i.Kind != Type::Vec3)
                                Error(id, guid, output, "GridCellSize requires vec3");
                            if (n.Op == "GridIndexToWorld" && i.Kind != Type::Position)
                                Error(id, guid, output, "GridIndexToWorld requires Position");
                            if (!std::set<std::string> {"GridNumCells",
                                                        "GridCellSize",
                                                        "GridIndexToWorld",
                                                        "GridWorldToIndex",
                                                        "GridWorldToCell",
                                                        "GridGet",
                                                        "GridSet",
                                                        "GridSample",
                                                        "GridAddFloat",
                                                        "GridLightMarch"}
                                     .count(n.Op))
                                Error(id, guid, output, "Unknown Grid3D operation");
                            reg = Emit(i);
                        }
                    }
                    else if (IsMeshOperation(n.Op))
                    {
                        auto binding = inputs.find(n.Parameter);
                        if (binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                            i.Interface = binding->second.Interface;
                        // Every output of one sampling node shares its random stream.
                        i.RandomKey         = Hash(StableHash(scope) ^ StableHash(guid));
                        const bool skeletal = i.Interface.Kind == "SkeletalMesh";
                        if (!skeletal && i.Interface.Kind != "StaticMesh")
                            Error(id, guid, output, "Mesh operation requires StaticMesh or SkeletalMesh binding");
                        if (i.Interface.Source.empty())
                            i.Interface.Source = skeletal ? "Owner" : "Asset";
                        if (i.Interface.Source != "Asset" && i.Interface.Source != "Owner" &&
                            i.Interface.Source != "Entity")
                            Error(id, guid, output, "Invalid mesh Source");
                        if (skeletal && i.Interface.Source == "Asset")
                            Error(id, guid, output, "SkeletalMesh requires Owner or Entity source");
                        if (i.Interface.Source == "Entity" && i.Interface.EntityPath.empty())
                            Error(id, guid, output, "Entity source requires a relative child path");
                        const bool bone =
                            n.Op == "RandomBone" || n.Op == "BoneCount" || n.Op == "SampleBone" || n.Op == "BoneByName";
                        if ((bone || n.Op == "SampleSkinnedTriangle") && !skeletal)
                            Error(id, guid, output, "Skeletal mesh operation requires SkeletalMesh binding");
                        auto count = n.Op == "SampleTriangle" || n.Op == "SampleSkinnedTriangle" ? 2u :
                                     n.Op == "SampleVertex" || n.Op == "SampleBone"              ? 1u :
                                                                                                   0u;
                        if (i.Args.size() != count)
                            Error(id, guid, output, "Wrong input count for " + n.Op);
                        if (count && !i.Args.empty() && Out.Code[i.Args[0]].Kind != Type::Int)
                            Error(id, guid, n.Inputs[0].Name, "Mesh index requires int");
                        if (count == 2 && i.Args.size() == 2 && Out.Code[i.Args[1]].Kind != Type::Vec3)
                            Error(id, guid, n.Inputs[1].Name, "Barycentric coordinates require vec3");
                        Type expectedType = Type::Int;
                        if (count)
                        {
                            if (output == "Position")
                                expectedType = Type::Position;
                            else if (output == "Normal" || output == "Velocity" || output == "MeshVelocity")
                                expectedType = Type::Vec3;
                            else if (output == "Tangent")
                                expectedType = Type::Vec4;
                            else if (output == "UV0")
                                expectedType = Type::Vec2;
                            else if (output == "Color")
                                expectedType = Type::Color;
                            else if (output == "Rotation" && bone)
                                expectedType = Type::Quat;
                            else if (output == "Valid")
                                expectedType = Type::Bool;
                            else
                                Error(id, guid, output, "Unknown mesh sample output");
                        }
                        if (i.Kind != expectedType)
                            Error(id, guid, output, "Mesh output type mismatch");
                        try
                        {
                            auto geometry = i.Interface;
                            geometry.Source.clear();
                            geometry.EntityPath.clear();
                            auto  key       = MeshBindingKey(geometry);
                            auto& meshCache = SharedMeshCache ? *SharedMeshCache : MeshCache;
                            auto  cached    = meshCache.find(key);
                            if (cached != meshCache.end())
                            {
                                i.Interface.Geometry      = cached->second.Geometry;
                                i.Interface.Skeleton      = cached->second.Skeleton;
                                i.Interface.SkeletonNames = cached->second.SkeletonNames;
                            }
                            else if (i.Interface.Asset.IsValid())
                            {
                                if (!Meshes)
                                    throw std::invalid_argument("Missing mesh resolver");
                                auto mesh       = Meshes(i.Interface.Asset);
                                auto skeletonID = mesh.Skeleton.IsValid() ? mesh.Skeleton : i.Interface.Skeleton;
                                SkeletonData skeleton;
                                if (skeletal)
                                {
                                    if (!Skeletons)
                                        throw std::invalid_argument("Missing skeleton resolver");
                                    skeleton = Skeletons(skeletonID);
                                }
                                BakeMeshSampling(i.Interface, mesh, skeleton);
                                meshCache.emplace(key, i.Interface);
                            }
                            else if (i.Interface.Source == "Asset")
                                throw std::invalid_argument("Asset source requires a mesh asset");
                            if (skeletal && i.Interface.Skeleton.IsValid() && i.Interface.SkeletonNames.empty() &&
                                Skeletons)
                            {
                                auto skeleton = Skeletons(i.Interface.Skeleton);
                                for (const auto& joint : skeleton.Joints)
                                    i.Interface.SkeletonNames.push_back(joint.Name);
                                for (const auto& names : {i.Interface.Bones, i.Interface.Region})
                                    for (const auto& name : names)
                                        if (skeleton.FindJoint(name) < 0)
                                            throw std::invalid_argument("Missing bone: " + name);
                            }
                            if (n.Op == "BoneByName")
                            {
                                SkeletonData skeleton;
                                if (Skeletons && i.Interface.Skeleton.IsValid())
                                    skeleton = Skeletons(i.Interface.Skeleton);
                                auto index = skeleton.FindJoint(n.Code);
                                if (index < 0)
                                    throw std::invalid_argument("Missing bone: " + n.Code);
                                i.Literal = Value::Int(index);
                                if (i.Interface.SkeletonNames.empty())
                                    for (const auto& joint : skeleton.Joints)
                                        i.Interface.SkeletonNames.push_back(joint.Name);
                            }
                        }
                        catch (const std::exception& ex)
                        {
                            Error(id, guid, output, ex.what());
                        }
                        reg = Emit(i);
                    }
                    else if (n.Op == "Reroute")
                    {
                        if (i.Args.size() != 1)
                            Error(id, guid, output, "Reroute requires one input");
                        reg = i.Args.empty() ? Constant(Value::Vector({}, i.Kind)) :
                                               Convert(i.Args[0], i.Kind, id, guid, output);
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
                            if (i.Interface.Kind == "StaticMesh" || i.Interface.Kind == "SkeletalMesh")
                                Error(id, guid, output, "Mesh data interfaces require mesh sampling operations");
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
                        if (IsVectorFieldOperation(i.Op))
                        {
                            auto binding = inputs.find(n.Parameter);
                            if (binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                                i.Interface = binding->second.Interface;
                            try
                            {
                                ValidateVectorField(i.Interface);
                            }
                            catch (const std::exception& e)
                            {
                                Error(id, guid, output, e.what());
                            }
                        }
                        if (i.Op == "SampleForceFields")
                        {
                            auto binding = inputs.find(n.Parameter);
                            if (binding != inputs.end() && binding->second.Kind == InputKind::DataInterface)
                                i.Interface = binding->second.Interface;
                            if (i.Interface.Kind.empty())
                                i.Interface.Kind = "ForceFields";
                            if (i.Interface.Kind != "ForceFields")
                                Error(id, guid, output, "SampleForceFields requires ForceFields data interface");
                        }
                        if (i.Op == "ColliderVelocity" && !i.Args.empty())
                            i.Args[0] = PositionTo(i.Args[0], Space::World);
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
                    if ((n.Stages & (1u << uint32_t(Out.StageKind))) &&
                        (n.Op == "Set" || n.Op == "WriteEvent" || n.Op == "GridSet" || n.Op == "GridAddFloat"))
                        visit(n.Guid, n.Outputs.empty() ? "Value" : n.Outputs.front().Name);
                uint32_t result = Constant(Value::Float(0));
                if (expected != Usage::Module)
                    result = visit(m.ResultNode, m.ResultPin);
                // Validate disconnected expressions too, so diagnostics are collected together.
                for (const auto& n : m.Nodes)
                    if ((n.Stages & (1u << uint32_t(Out.StageKind))) && n.Op != "Set" && n.Op != "WriteEvent")
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
                if (i.Op == "ColliderVelocity")
                {
                    if (Out.Target != SimTarget::GPU)
                        Error(id, n.Guid, "Value", "ColliderVelocity requires GPU; switch the emitter to GPU");
                    if (t(0) != Type::Position || t(1) != Type::Float || i.Kind != Type::Vec4)
                        error(0, "ColliderVelocity requires Position, float -> vec4");
                    if (Out.StageKind != Stage::SimulationStage && Out.StageKind != Stage::ParticleUpdate &&
                        Out.StageKind != Stage::ParticleSpawn)
                        error(0, "ColliderVelocity requires a grid or particle stage");
                }
                else if (i.Op == "ToWorldVector" || i.Op == "ToSimulationVector")
                {
                    if (t(0) != Type::Vec3 || i.Kind != Type::Vec3)
                        error(0, "Vector space conversion requires vec3 -> vec3");
                }
                else if (i.Op == "Collision")
                {
                    if (Out.StageKind != Stage::ParticleUpdate)
                        Error(id, n.Guid, "Mode", "Collision requires ParticleUpdate after SolveForcesAndVelocity");
                    if (i.Args.size() != 13)
                        return;
                    const auto& mode = Out.Code[i.Args[5]];
                    if (mode.Op != "Constant" || mode.Kind != Type::Int || mode.Literal.Integer < 0 ||
                        mode.Literal.Integer > 2)
                        Error(id,
                              n.Guid,
                              "Mode",
                              "Collision Mode requires a constant: SceneQueries (0), DepthBuffer (1), AnalyticShapes "
                              "(2)");
                    else if (Out.Target == SimTarget::GPU && mode.Literal.Integer == int(CollisionMode::SceneQueries))
                        Error(id,
                              n.Guid,
                              "Mode",
                              "Collision: SceneQueries is CPU only; use DepthBuffer or AnalyticShapes");
                    else if (Out.Target == SimTarget::CPU && mode.Literal.Integer == int(CollisionMode::DepthBuffer))
                        Error(id,
                              n.Guid,
                              "Mode",
                              "Collision: DepthBuffer requires a GPU emitter; use SceneQueries or AnalyticShapes");
                }
                else if (i.Op == "Select")
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
                    if (i.Kind != (t(0) == Type::IVec3 ? Type::Int : Type::Float) || Components(t(0)) < 2)
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
                        if (t(k) != (i.Kind == Type::IVec3 ? Type::Int : Type::Float))
                            error(k, "Make component type mismatch");
                }
                else if (i.Op == "Break")
                {
                    if (i.Kind != (t(0) == Type::IVec3 ? Type::Int : Type::Float) || Components(t(0)) < 2)
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
                else if (i.Op == "SampleVectorField")
                {
                    if (t(0) != Type::Position || i.Kind != Type::Vec3)
                        error(0, "SampleVectorField requires field-local Position -> vec3");
                }
                else if (i.Op == "VectorFieldBounds")
                {
                    if (i.Kind != Type::Vec3 || (i.Output != "Min" && i.Output != "Max"))
                        Error(id, n.Guid, i.Output, "VectorFieldBounds requires Min and Max vec3 outputs");
                }
                else if (i.Op == "VectorFieldResolution")
                {
                    if (i.Kind != Type::Int || (i.Output != "X" && i.Output != "Y" && i.Output != "Z"))
                        Error(id, n.Guid, i.Output, "VectorFieldResolution requires X, Y, Z int outputs");
                }
                else if (i.Op == "FieldPosition")
                {
                    if (t(0) != Type::Position || t(1) != Type::Vec3 || t(2) != Type::Quat || t(3) != Type::Vec3 ||
                        i.Kind != Type::Position)
                        error(0, "FieldPosition requires Position, vec3, quat, vec3 -> Position");
                }
                else if (i.Op == "FieldVector")
                {
                    if (t(0) != Type::Vec3 || t(1) != Type::Quat || i.Kind != Type::Vec3)
                        error(0, "FieldVector requires vec3, quat -> vec3");
                }
                else if (i.Op == "CurlNoise")
                {
                    if (t(0) != Type::Position || t(1) != Type::Float || t(2) != Type::Vec3 || i.Kind != Type::Vec3)
                        error(0, "CurlNoise requires Position, float, vec3, int -> vec3");
                    if (i.Args.size() == 4)
                    {
                        const auto& octave = Out.Code[i.Args[3]];
                        if (t(3) != Type::Int || octave.Op != "Constant" || octave.Literal.Integer < 1 ||
                            octave.Literal.Integer > 4)
                            error(3, "Octaves must be a compile-time integer from 1 to 4");
                    }
                }
                else if (i.Op == "SampleForceFields")
                {
                    if (t(0) != Type::Position || t(1) != Type::Vec3 || i.Kind != Type::Vec3)
                        error(0, "ForceFields requires Position, vec3 -> vec3");
                    if (i.Output != "Wind" && i.Output != "Force" && i.Output != "Acceleration" &&
                        i.Output != "Impulse")
                        error(0, "Unknown ForceFields output");
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
                          !std::set<std::string> {"Add", "Sub", "Mul", "Div", "Min", "Max", "Abs", "Clamp", "Mod"}
                               .count(i.Op)))
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
        std::map<std::string, Parameter> Collect(const std::map<Stage, Stack>&                  stages,
                                                 const ModuleResolver&                          resolve,
                                                 std::map<std::string, Parameter>               known,
                                                 const std::map<std::string, InterfaceBinding>* grids = nullptr)
        {
            std::set<AssetID>                                                              visited;
            std::function<void(AssetID, std::string, const std::map<std::string, Input>&)> scan =
                [&](AssetID id, std::string scope, const std::map<std::string, Input>& inputs) {
                    if (!visited.insert(id).second)
                        return;
                    try
                    {
                        auto m = resolve(id);
                        SpecializeGridModule(m, inputs, grids);
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
                                scan(n.Function, scope + "/" + n.Guid, {});
                        }
                    }
                    catch (const std::exception&)
                    {}
                    visited.erase(id);
                };
            for (const auto& [s, stack] : stages)
                for (const auto& use : stack)
                    if (use.Enabled)
                        scan(use.Script, use.Guid, use.Inputs);
            return known;
        }
        Program StageProgram(Stage                                          stage,
                             const Stack&                                   stack,
                             SimTarget                                      target,
                             const ModuleResolver&                          resolve,
                             std::map<std::string, Parameter>               known,
                             std::vector<Diagnostic>&                       errors,
                             AssetID                                        asset,
                             Space                                          simulationSpace = Space::Local,
                             const MeshResolver&                            meshes          = {},
                             const SkeletonResolver&                        skeletons       = {},
                             std::map<std::string, InterfaceBinding>*       meshCache       = nullptr,
                             const std::map<std::string, InterfaceBinding>* grids           = nullptr,
                             const std::string&                             iterationGrid   = "")
        {
            Compiler c {resolve, std::move(known), errors, {}, {}, asset};
            c.SimulationSpace   = simulationSpace;
            c.Meshes            = meshes;
            c.Skeletons         = skeletons;
            c.SharedMeshCache   = meshCache;
            c.Out.StageKind     = stage;
            c.Out.Target        = target;
            c.Grids             = grids;
            c.Out.IterationGrid = iterationGrid;
            if (grids && grids->count(iterationGrid))
                c.GridResource(iterationGrid, grids->at(iterationGrid));
            // Event-producing modules observe the final particle state, including kills later in the stack.
            for (bool eventPass : {false, true})
                for (const auto& use : stack)
                    if (use.Enabled)
                    {
                        bool events = false;
                        try
                        {
                            auto m = resolve(use.Script);
                            events = std::any_of(
                                m.Nodes.begin(), m.Nodes.end(), [](const auto& n) { return n.Op == "WriteEvent"; });
                        }
                        catch (const std::exception&)
                        {}
                        if (events == eventPass)
                        {
                            const auto before = errors.size();
                            c.Inline(use.Script, Usage::Module, use.Inputs, use.Guid);
                            for (auto e = before; e < errors.size(); ++e)
                                if (errors[e].Use.empty())
                                    errors[e].Use = use.Guid;
                        }
                    }
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
                if (c.Out.Code[index].Op == "Set" || c.Out.Code[index].Op == "WriteEvent" ||
                    c.Out.Code[index].Op == "GridSet" || c.Out.Code[index].Op == "GridAddFloat")
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
                if (c.Out.Code[index].Op == "Sample" || IsMeshOperation(c.Out.Code[index].Op) ||
                    IsVectorFieldOperation(c.Out.Code[index].Op))
                {
                    auto count =
                        (IsMeshOperation(c.Out.Code[index].Op) || IsVectorFieldOperation(c.Out.Code[index].Op)) ?
                            1u :
                            uint32_t(c.Out.Code[index].Interface.Values.size());
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
                s += v.Kind == Type::IVec3 ? std::to_string(int32_t(v.Data[k])) : F(v.Data[k]);
            }
            return s + ")";
        }
    } // namespace
    CompileResult Compile(const System&           system,
                          const EmitterResolver&  emitters,
                          const ModuleResolver&   modules,
                          AssetID                 asset,
                          const MeshResolver&     meshes,
                          const SkeletonResolver& skeletons)
    {
        CompileResult                           r;
        std::map<std::string, InterfaceBinding> meshCache;
        // Locates the errors added since `from` for editors (fields already set are kept).
        const auto locate = [&r](size_t from, const std::string& emitter, std::optional<Stage> stage = {},
                                 const std::string& container = {}) {
            for (auto e = from; e < r.Errors.size(); ++e)
            {
                auto& d = r.Errors[e];
                if (d.Emitter.empty())
                    d.Emitter = emitter;
                if (!d.StageKind)
                    d.StageKind = stage;
                if (d.Container.empty())
                    d.Container = container;
            }
        };
        r.Product.Settings            = system;
        auto known                    = BuiltinParameters();
        known["Transient.SpawnIndex"] = {"Transient.SpawnIndex", Type::Int, Value::Int(0), {}};
        for (const auto& p : system.UserParameters)
            known[p.Name] = p;
        known = Collect(system.Stages, modules, std::move(known));
        for (const auto& [s, stack] : system.Stages)
        {
            if (s > Stage::SystemUpdate)
            {
                if (!stack.empty())
                    r.Errors.push_back({asset,
                                        "",
                                        "Stage",
                                        s == Stage::SimulationStage ?
                                            "SimulationStage requires ordered emitter stage records" :
                                            "Stage belongs to an emitter"});
                continue;
            }
            const auto from       = r.Errors.size();
            r.Product.Programs[s] = StageProgram(
                s, stack, SimTarget::CPU, modules, known, r.Errors, asset, Space::Local, meshes, skeletons, &meshCache);
            locate(from, "", s);
        }
        // Resolve all emitter settings first: cross-emitter validation never depends on handle order.
        std::vector<std::map<std::string, Parameter>> parameters;
        for (const auto& h : system.Emitters)
            if (h.Enabled)
            {
                const auto      emitterErrors = r.Errors.size();
                CompiledEmitter ce;
                ce.Handle = h.Guid;
                try
                {
                    ce.Settings = MergeEmitter(h.Inline ? *h.Inline : emitters(h.Reference), emitters);
                    ApplyOverrides(ce.Settings, h.Overrides);
                    ce.Settings.Name = h.Name;
                }
                catch (const std::exception& e)
                {
                    r.Errors.push_back({asset, h.Guid, "", e.what()});
                    locate(emitterErrors, h.Guid);
                    continue;
                }
                auto k = known;
                for (const auto& p : ce.Settings.Attributes)
                    k[p.Name] = p;
                for (const auto& p : ce.Settings.Parameters)
                    k[p.Name] = p;
                for (const auto& [name, grid] : ce.Settings.DataInterfaces)
                {
                    if (name.rfind("Emitter.", 0) != 0 || grid.Kind != "Grid3D")
                        r.Errors.push_back({asset, h.Guid, name, "Grid3D requires a named Emitter parameter"});
                    if (ce.Settings.Target != SimTarget::GPU)
                        r.Errors.push_back(
                            {asset,
                             h.Guid,
                             name,
                             "Grid3D on emitter " + h.Name + " requires GPU; switch the emitter to GPU"});
                    if (glm::any(glm::lessThan(grid.Resolution, glm::uvec3(1))) ||
                        glm::any(glm::greaterThan(grid.Resolution, glm::uvec3(128))) ||
                        glm::any(glm::lessThanEqual(grid.WorldBounds, glm::vec3(0))) ||
                        !std::isfinite(grid.WorldBounds.x + grid.WorldBounds.y + grid.WorldBounds.z))
                        r.Errors.push_back({asset, h.Guid, name, "Invalid Grid3D Resolution or WorldBounds"});
                    std::set<std::string> names;
                    if (grid.GridAttributes.empty())
                        r.Errors.push_back({asset, h.Guid, "Attributes", "Grid3D attributes must not be empty"});
                    for (const auto& a : grid.GridAttributes)
                        if (a.Name.empty() || !names.insert(a.Name).second || a.Kind > Type::Vec4)
                            r.Errors.push_back(
                                {asset, h.Guid, "Attributes", "Duplicate or invalid Grid3D attribute: " + a.Name});
                }
                k = Collect(ce.Settings.Stages, modules, std::move(k), &ce.Settings.DataInterfaces);
                for (const auto& stage : ce.Settings.SimulationStages)
                    k = Collect(
                        {{Stage::SimulationStage, stage.Modules}}, modules, std::move(k), &ce.Settings.DataInterfaces);
                for (const auto& handler : ce.Settings.Handlers)
                    if (handler.Enabled)
                        k = Collect({{Stage::EventHandler, handler.Modules}},
                                    modules,
                                    std::move(k),
                                    &ce.Settings.DataInterfaces);
                std::set<AssetID>            visited;
                std::function<void(AssetID)> layouts = [&](AssetID id) {
                    if (!visited.insert(id).second)
                        return;
                    try
                    {
                        auto m = modules(id);
                        for (const auto& l : m.EventLayouts)
                        {
                            auto at = ce.EventLayouts.find(l.Name);
                            if (at != ce.EventLayouts.end() &&
                                YAML::Dump(EncodeEventLayout(at->second)) != YAML::Dump(EncodeEventLayout(l)))
                                r.Errors.push_back(
                                    {asset, h.Guid, l.Name, "Conflicting event payload layout: " + l.Name});
                            else
                                ce.EventLayouts[l.Name] = l;
                        }
                        for (const auto& n : m.Nodes)
                            if (n.Op == "Call")
                                layouts(n.Function);
                    }
                    catch (const std::exception&)
                    {}
                };
                for (const auto& [stage, stack] : ce.Settings.Stages)
                    for (const auto& use : stack)
                        layouts(use.Script); // Disabled generators still declare the channel's payload type.
                std::set<std::string> channelNames;
                for (const auto& c : ce.Settings.Channels)
                {
                    if (!channelNames.insert(c.Name).second || c.Name.empty() || !c.Capacity || c.Capacity > 16777216)
                        r.Errors.push_back({asset, h.Guid, c.Name, "Invalid or duplicate event channel"});
                    if (!ce.EventLayouts.count(c.Layout))
                        r.Errors.push_back({asset, h.Guid, c.Name, "Unknown event layout: " + c.Layout});
                }
                std::set<std::string> attributes;
                for (const auto& [s, stack] : ce.Settings.Stages)
                {
                    if (s >= Stage::EventHandler)
                    {
                        if (!stack.empty())
                            r.Errors.push_back({asset,
                                                h.Guid,
                                                "",
                                                s == Stage::SimulationStage ?
                                                    "SimulationStage requires ordered emitter stage records" :
                                                    "EventHandler requires handler entries"});
                        continue;
                    }
                    const auto from = r.Errors.size();
                    ce.Programs[s]  = StageProgram(s,
                                                  stack,
                                                  s >= Stage::ParticleSpawn ? ce.Settings.Target : SimTarget::CPU,
                                                  modules,
                                                  k,
                                                  r.Errors,
                                                  asset,
                                                  ce.Settings.SimulationSpace,
                                                  meshes,
                                                  skeletons,
                                                  &meshCache,
                                                  &ce.Settings.DataInterfaces);
                    locate(from, h.Guid, s);
                }
                for (auto stage : {Stage::ParticleSpawn, Stage::ParticleUpdate})
                    if (!ce.Programs.count(stage))
                    {
                        Program p;
                        p.StageKind        = stage;
                        p.Target           = ce.Settings.Target;
                        ce.Programs[stage] = p;
                    }
                std::set<std::string> stageIDs;
                for (const auto& stage : ce.Settings.SimulationStages)
                {
                    const auto stageErrors = r.Errors.size();
                    const auto error       = [&](const std::string& pin, const std::string& message) {
                        r.Errors.push_back({asset, stage.Guid, pin, "Emitter " + h.Name + ": " + message});
                    };
                    if (ce.Settings.Target != SimTarget::GPU)
                        error("IterationSource", "SimulationStage requires GPU; switch the emitter to GPU");
                    if (stage.Guid.empty() || !stageIDs.insert(stage.Guid).second || stage.Name.empty())
                        error("Name", "Duplicate or empty simulation stage GUID/Name");
                    if (stage.Iterations < 1 || stage.Iterations > 64)
                        error("Iterations", "Iterations must be 1–64");
                    if (!stage.Enabled.empty() &&
                        ((stage.Enabled.rfind("Emitter.", 0) != 0 && stage.Enabled.rfind("User.", 0) != 0) ||
                         !k.count(stage.Enabled) || k.at(stage.Enabled).Kind != Type::Bool ||
                         !k.at(stage.Enabled).Default))
                        error("Enabled", "Enabled requires a bool emitter or user parameter");
                    std::string grid;
                    if (stage.Source == IterationSource::DataInterface)
                    {
                        grid = stage.DataInterface;
                        if (!ce.Settings.DataInterfaces.count(grid) ||
                            ce.Settings.DataInterfaces.at(grid).Kind != "Grid3D")
                            error("IterationSource", "Iteration source requires a Grid3D emitter parameter");
                    }
                    auto p = StageProgram(Stage::SimulationStage,
                                          stage.Modules,
                                          ce.Settings.Target,
                                          modules,
                                          k,
                                          r.Errors,
                                          asset,
                                          ce.Settings.SimulationSpace,
                                          meshes,
                                          skeletons,
                                          &meshCache,
                                          &ce.Settings.DataInterfaces,
                                          grid);
                    locate(stageErrors, h.Guid, Stage::SimulationStage, stage.Guid);
                    for (const auto& name : p.Reads)
                        if (name.rfind("Particles.", 0) == 0)
                            attributes.insert(name);
                    for (const auto& name : p.Writes)
                        if (name.rfind("Particles.", 0) == 0)
                            attributes.insert(name);
                    ce.SimulationStages.push_back({stage, std::move(p)});
                }
                const bool hasRibbon =
                    std::any_of(ce.Settings.Renderers.begin(), ce.Settings.Renderers.end(), [](const auto& renderer) {
                        return renderer.Kind == RendererKind::Ribbon;
                    });
                const bool writesRibbonOrder =
                    std::any_of(ce.Programs.begin(), ce.Programs.end(), [](const auto& entry) {
                        return entry.second.Writes.count("Particles.RibbonLinkOrder") != 0;
                    });
                if (hasRibbon && !writesRibbonOrder)
                {
                    // Dynamic defaults are ordinary bytecode, so CPU and GPU retain identical products.
                    auto&      update    = ce.Programs.at(Stage::ParticleUpdate);
                    const auto parameter = [&](const std::string& name) {
                        for (uint32_t n = 0; n < update.Parameters.size(); ++n)
                            if (update.Parameters[n].Name == name)
                                return n;
                        update.Parameters.push_back(k.at(name));
                        return uint32_t(update.Parameters.size() - 1);
                    };
                    Instruction read;
                    read.Op        = "Get";
                    read.Kind      = Type::Float;
                    read.Parameter = parameter("Particles.NormalizedAge");
                    uint32_t value = uint32_t(update.Code.size());
                    update.Code.push_back(read);
                    Instruction write;
                    write.Op        = "Set";
                    write.Kind      = Type::Float;
                    write.Parameter = parameter("Particles.RibbonLinkOrder");
                    write.Args      = {value};
                    update.Code.push_back(write);
                    update.Reads.insert("Particles.NormalizedAge");
                    update.Writes.insert("Particles.RibbonLinkOrder");
                }
                for (const auto& [stage, p] : ce.Programs)
                {
                    for (const auto& a : p.Reads)
                        if (a.rfind("Particles.", 0) == 0)
                            attributes.insert(a);
                    for (const auto& a : p.Writes)
                        if (a.rfind("Particles.", 0) == 0)
                            attributes.insert(a);
                }
                for (auto& renderer : ce.Settings.Renderers)
                {
                    if (!std::isfinite(renderer.Curvature) || renderer.Curvature < 0 || renderer.Curvature > 1)
                        r.Errors.push_back({asset, renderer.Guid, "Curvature", "Curvature must be in [0, 1]"});
                    if (renderer.Kind == RendererKind::Sprite && renderer.NormalMode == LightingNormal::ParticleNormal)
                    {
                        renderer.Bindings["Normal"] = "Particles.Normal";
                        auto normal                 = k.find("Particles.Normal");
                        if (normal == k.end() || normal->second.Kind != Type::Vec3)
                            r.Errors.push_back(
                                {asset,
                                 renderer.Guid,
                                 "LightingNormal",
                                 "Renderer " + renderer.Guid + ": ParticleNormal requires Particles.Normal (Vec3)"});
                    }
                    const bool mesh   = renderer.Kind == RendererKind::Mesh;
                    const bool ribbon = renderer.Kind == RendererKind::Ribbon;
                    if (renderer.Kind == RendererKind::Light)
                        r.Errors.push_back({asset, renderer.Guid, "", "Light renderer not implemented"});
                    if (mesh)
                    {
                        if (renderer.FacingMode == Facing::ViewPlane)
                            renderer.FacingMode = Facing::Default;
                        if (renderer.Meshes.empty())
                            r.Errors.push_back({asset, renderer.Guid, "Meshes", "Empty mesh list"});
                        for (const auto& entry : renderer.Meshes)
                        {
                            try
                            {
                                if (!entry.Mesh.IsValid() || !meshes)
                                    throw std::invalid_argument("mesh unavailable");
                                auto data = meshes(entry.Mesh);
                                for (const auto& [slot, material] : entry.MaterialOverrides)
                                    if (slot >= data.MaterialSlots.size())
                                        r.Errors.push_back({asset,
                                                            renderer.Guid,
                                                            "MaterialOverrides",
                                                            "Material override slot does not exist on mesh"});
                                auto extent =
                                    glm::max(glm::abs(data.LocalBounds.Min * entry.Scale + entry.PivotOffset),
                                             glm::abs(data.LocalBounds.Max * entry.Scale + entry.PivotOffset));
                                ce.MeshRadii[renderer.Guid] =
                                    std::max(ce.MeshRadii[renderer.Guid], glm::length(extent));
                            }
                            catch (const std::exception&)
                            {
                                r.Errors.push_back(
                                    {asset, renderer.Guid, "Meshes", "Missing mesh asset: " + entry.Mesh.ToString()});
                            }
                        }
                    }
                    std::map<std::string, Type> bindingTypes = {{"Position", Type::Position},
                                                                {"Color", Type::Color},
                                                                {"Velocity", Type::Vec3},
                                                                {"DynamicMaterialParameter", Type::Vec4}};
                    if (ribbon)
                    {
                        bindingTypes.insert({{"RibbonID", Type::Int},
                                             {"RibbonLinkOrder", Type::Float},
                                             {"RibbonWidth", Type::Float},
                                             {"RibbonTwist", Type::Float}});
                        for (const auto& [binding, type] : bindingTypes)
                            if (!renderer.Bindings.count(binding))
                                renderer.Bindings[binding] = "Particles." + binding;
                        if (renderer.Bindings.count("RibbonFacing"))
                            bindingTypes["RibbonFacing"] = Type::Vec3;
                        if (renderer.Bindings.count("RibbonUVDistance"))
                            bindingTypes["RibbonUVDistance"] = Type::Float;
                        if (renderer.RibbonFacingMode != RibbonFacing::Screen &&
                            (!renderer.Bindings.count("RibbonFacing") || renderer.Bindings.at("RibbonFacing").empty()))
                            r.Errors.push_back({asset,
                                                renderer.Guid,
                                                "RibbonFacing",
                                                "Custom ribbon facing requires a RibbonFacing binding"});
                        if (renderer.TessellationFactor < 1 || renderer.TessellationFactor > 16)
                            r.Errors.push_back({asset,
                                                renderer.Guid,
                                                "TessellationFactor",
                                                "Ribbon tessellation factor must be 1–16"});
                        if (!std::isfinite(renderer.CurveTension))
                            r.Errors.push_back(
                                {asset, renderer.Guid, "CurveTension", "Ribbon curve tension must be finite"});
                        for (const auto& uv : renderer.UVChannels)
                            if ((uv.Mode == RibbonUVMode::TiledOverRibbonLength ||
                                 uv.Mode == RibbonUVMode::TiledFromStartOverRibbonLength) &&
                                (!std::isfinite(uv.TileSize) || uv.TileSize <= 0))
                                r.Errors.push_back(
                                    {asset, renderer.Guid, "TileSize", "Ribbon tiled UV TileSize must be positive"});
                    }
                    else if (mesh)
                    {
                        bindingTypes.insert(
                            {{"MeshOrientation", Type::Quat}, {"Scale", Type::Vec3}, {"MeshIndex", Type::Int}});
                        // Missing mesh bindings use the built-in default stream, also retained for rendering only.
                        for (const auto& [binding, type] : bindingTypes)
                            if (!renderer.Bindings.count(binding) || renderer.Bindings.at(binding).empty())
                                renderer.Bindings[binding] = "Particles." + binding;
                    }
                    else
                        bindingTypes.insert({{"SpriteSize", Type::Vec2},
                                             {"SpriteRotation", Type::Float},
                                             {"SubImageIndex", Type::Float}});
                    if (!ribbon && renderer.Bindings.count("DirectionalTransmittance") &&
                        !renderer.Bindings.at("DirectionalTransmittance").empty())
                        bindingTypes["DirectionalTransmittance"] = Type::Float;
                    if (renderer.Bindings.count("SortKey"))
                        bindingTypes["SortKey"] = Type::Float;
                    if (renderer.Sorting == Sort::Custom)
                    {
                        bindingTypes["SortKey"] = Type::Float;
                        if (!renderer.Bindings.count("SortKey") || renderer.Bindings.at("SortKey").empty())
                            r.Errors.push_back(
                                {asset, renderer.Guid, "SortKey", "Custom sort requires a SortKey binding"});
                    }
                    for (const auto& [binding, type] : bindingTypes)
                    {
                        auto name = renderer.Bindings.find(binding);
                        if (name == renderer.Bindings.end())
                        {
                            if (binding != "SortKey")
                                r.Errors.push_back({asset, renderer.Guid, binding, "Missing sprite renderer binding"});
                        }
                        else if (auto parameter = k.find(name->second);
                                 parameter != k.end() &&
                                 (parameter->second.Kind != type || name->second.rfind("Particles.", 0) != 0))
                            r.Errors.push_back(
                                {asset, renderer.Guid, binding, "Renderer binding type or namespace mismatch"});
                    }
                    for (const auto& [binding, name] : renderer.Bindings)
                    {
                        if (name.empty() && (mesh || binding == "DirectionalTransmittance"))
                            continue;
                        auto at = k.find(name);
                        if (at == k.end())
                            r.Errors.push_back({asset, renderer.Guid, binding, "Unknown renderer parameter: " + name});
                        else if (name.rfind("Particles.", 0) != 0)
                            r.Errors.push_back(
                                {asset, renderer.Guid, binding, "Renderer binding type or namespace mismatch"});
                        else if (!at->second.Default)
                            r.Errors.push_back(
                                {asset, renderer.Guid, binding, "Read of unset renderer parameter: " + name});
                        else
                            attributes.insert(name);
                    }
                }

                float collisionMeshRadius = -1;
                for (const auto& renderer : ce.Settings.Renderers)
                    if (renderer.Kind == RendererKind::Mesh && ce.MeshRadii.count(renderer.Guid))
                    {
                        collisionMeshRadius = ce.MeshRadii.at(renderer.Guid);
                        break;
                    }
                for (auto& [stage, program] : ce.Programs)
                    for (auto& parameter : program.Parameters)
                        if (parameter.Name == "Transient.CollisionMeshRadius")
                            parameter.Default = Value::Float(collisionMeshRadius);
                attributes.insert("Particles.UniqueID");
                attributes.insert("Particles.Alive");
                for (const auto& name : attributes)
                    if (auto p = k.find(name); p != k.end())
                        ce.Attributes.push_back({name,
                                                 p->second.Kind,
                                                 p->second.Default.value_or(Value::Vector({}, p->second.Kind)),
                                                 uint32_t(ce.Attributes.size())});
                parameters.push_back(std::move(k));
                r.Product.Emitters.push_back(std::move(ce));
                locate(emitterErrors, h.Guid);
            }
        auto&                           result = r.Product.Emitters;
        std::vector<std::set<uint32_t>> dependencies(result.size());
        const auto sourceIndex = [&](const std::string& source, const std::string& guid) -> uint32_t {
            for (uint32_t i = 0; i < result.size(); ++i)
                if (result[i].Handle == source)
                    return i;
            r.Errors.push_back({asset, guid, "Source", "Unknown source emitter handle: " + source});
            return UINT32_MAX;
        };
        const auto keep = [&](uint32_t            emitter,
                              const std::string&  name,
                              const std::string&  guid,
                              std::optional<Type> type = {}) -> uint32_t {
            auto& e  = result[emitter];
            auto  at = parameters[emitter].find(name);
            if (at == parameters[emitter].end() || !at->second.Default || name.rfind("Particles.", 0) != 0)
            {
                r.Errors.push_back({asset, guid, name, "Missing source attribute " + name + " on emitter " + e.Handle});
                return 0;
            }
            if (type && at->second.Kind != *type)
                r.Errors.push_back(
                    {asset, guid, name, "Source attribute type mismatch: " + name + " on emitter " + e.Handle});
            for (const auto& a : e.Attributes)
                if (a.Name == name)
                    return a.Offset;
            uint32_t offset = uint32_t(e.Attributes.size());
            e.Attributes.push_back({name, at->second.Kind, *at->second.Default, offset});
            return offset;
        };
        // Merge spawn bytecode before a spawned-particle handler, retaining one typed parameter map.
        const auto prepend = [](const Program& spawn, Program handler, Space space) {
            Program               p = spawn;
            std::vector<uint32_t> map;
            for (const auto& parameter : handler.Parameters)
            {
                auto at = std::find_if(
                    p.Parameters.begin(), p.Parameters.end(), [&](const auto& x) { return x.Name == parameter.Name; });
                if (at == p.Parameters.end())
                {
                    map.push_back(uint32_t(p.Parameters.size()));
                    p.Parameters.push_back(parameter);
                }
                else
                    map.push_back(uint32_t(at - p.Parameters.begin()));
            }
            auto velocity = std::find_if(p.Parameters.begin(), p.Parameters.end(), [](const auto& parameter) {
                return parameter.Name == "Particles.Velocity";
            });
            if (velocity == p.Parameters.end())
            {
                p.Parameters.push_back({"Particles.Velocity", Type::Vec3, Value::Vector({}, Type::Vec3), {}});
                velocity = std::prev(p.Parameters.end());
            }
            Instruction boundary;
            boundary.Op            = "SpawnBoundary";
            boundary.Parameter     = uint32_t(velocity - p.Parameters.begin());
            boundary.PositionSpace = space;
            p.Code.push_back(boundary);
            p.Reads.insert("Particles.Velocity");
            p.Writes.insert("Particles.Velocity");
            uint32_t base = uint32_t(p.Code.size());
            for (auto i : handler.Code)
            {
                for (auto& arg : i.Args)
                    arg += base;
                if (i.Op == "Get" || i.Op == "Set")
                    i.Parameter = map.at(i.Parameter);
                p.Code.push_back(std::move(i));
            }
            p.Reads.insert(handler.Reads.begin(), handler.Reads.end());
            p.Writes.insert(handler.Writes.begin(), handler.Writes.end());
            p.Interfaces.clear();
            uint32_t offset = 0;
            for (uint32_t i = 0; i < p.Code.size(); ++i)
                if (p.Code[i].Op == "Sample" || IsMeshOperation(p.Code[i].Op) || IsVectorFieldOperation(p.Code[i].Op))
                {
                    p.Code[i].Parameter = uint32_t(p.Interfaces.size());
                    auto count          = (IsMeshOperation(p.Code[i].Op) || IsVectorFieldOperation(p.Code[i].Op)) ?
                                              1u :
                                              uint32_t(p.Code[i].Interface.Values.size());
                    p.Interfaces.push_back({i, 6, offset, count});
                    offset += count;
                }
            return p;
        };
        for (uint32_t index = 0; index < result.size(); ++index)
        {
            auto&                 ce = result[index];
            std::set<std::string> ids;
            for (const auto& h : ce.Settings.Handlers)
            {
                const auto handlerErrors = r.Errors.size();
                // Every exit of this iteration locates what it added.
                struct Locate
                {
                    std::function<void()> Run;
                    ~Locate() { Run(); }
                } located {[&] { locate(handlerErrors, ce.Handle, Stage::EventHandler, h.Guid); }};
                if (!ids.insert(h.Guid).second || h.Guid.empty())
                    r.Errors.push_back({asset, h.Guid, "", "Duplicate or empty event handler GUID"});
                if (!h.Enabled)
                    continue;
                uint32_t source = sourceIndex(h.SourceEmitter, h.Guid);
                if (source == UINT32_MAX)
                    continue;
                dependencies[index].insert(source);
                auto& src = result[source];
                if (src.Settings.Target != ce.Settings.Target)
                    r.Errors.push_back({asset,
                                        h.Guid,
                                        "Source",
                                        "Event handler CPU/GPU simulation target mismatch with " + h.SourceEmitter});
                auto channel = std::find_if(src.Settings.Channels.begin(),
                                            src.Settings.Channels.end(),
                                            [&](const auto& c) { return c.Name == h.Channel; });
                if (channel == src.Settings.Channels.end())
                {
                    r.Errors.push_back(
                        {asset, h.Guid, "Channel", "Unknown event channel: " + h.Channel + " on " + h.SourceEmitter});
                    continue;
                }
                auto layout = src.EventLayouts.find(channel->Layout);
                if (layout == src.EventLayouts.end())
                    continue;
                auto k = parameters[index];
                for (const auto& field : layout->second.Fields)
                {
                    auto name = "Transient.Event." + field.Name;
                    k[name]   = {name, field.Kind, Value::Vector({}, field.Kind), {}};
                }
                auto    before = r.Errors.size();
                Program p      = StageProgram(Stage::EventHandler,
                                              h.Modules,
                                              ce.Settings.Target,
                                              modules,
                                              k,
                                              r.Errors,
                                              asset,
                                              ce.Settings.SimulationSpace,
                                              meshes,
                                              skeletons,
                                              &meshCache,
                                              &ce.Settings.DataInterfaces);
                for (auto i = before; i < r.Errors.size(); ++i)
                {
                    r.Errors[i].Asset   = asset;
                    r.Errors[i].Message = "Handler " + h.Guid + ": " + r.Errors[i].Message;
                }
                if (h.Mode == EventMode::SpawnedParticles)
                    p = prepend(ce.Programs.at(Stage::ParticleSpawn), std::move(p), ce.Settings.SimulationSpace);
                p.StageKind         = Stage::EventHandler;
                p.Handler           = true;
                p.Mode              = h.Mode;
                p.SpawnNumber       = h.SpawnNumber;
                p.MaxEventsPerFrame = h.MaxEventsPerFrame;
                p.Resources.push_back({"EventInput",
                                       h.Channel,
                                       h.SourceEmitter,
                                       source,
                                       7,
                                       channel->Capacity,
                                       0,
                                       src.Settings.SimulationSpace,
                                       layout->second});
                for (const auto& name : p.Reads)
                    if (name.rfind("Particles.", 0) == 0)
                        keep(index, name, h.Guid);
                for (const auto& name : p.Writes)
                    if (name.rfind("Particles.", 0) == 0)
                        keep(index, name, h.Guid);
                ce.Handlers.push_back({h, std::move(p), source});
            }
        }
        for (uint32_t index = 0; index < result.size(); ++index)
        {
            auto& ce   = result[index];
            auto  bind = [&](Program& p, const std::string& guid) {
                for (auto& i : p.Code)
                    if (i.Op == "WriteEvent" || i.Op == "ParticleCount" || i.Op == "ReadAttribute" ||
                        i.Op == "ReadAttributeByID")
                    {
                        ResourceBinding resource;
                        if (i.Op == "WriteEvent")
                        {
                            resource.Kind         = "EventOutput";
                            resource.Source       = index;
                            resource.SourceHandle = ce.Handle;
                            resource.Channel      = i.Interface.Channel;
                            auto c = std::find_if(ce.Settings.Channels.begin(),
                                                  ce.Settings.Channels.end(),
                                                  [&](const auto& c) { return c.Name == resource.Channel; });
                            if (c == ce.Settings.Channels.end())
                            {
                                r.Errors.push_back(
                                    {asset, guid, "Channel", "Unknown event channel: " + resource.Channel});
                                continue;
                            }
                            auto l = ce.EventLayouts.find(c->Layout);
                            if (l == ce.EventLayouts.end())
                                continue;
                            resource.Capacity      = c->Capacity;
                            resource.Layout        = l->second;
                            resource.PositionSpace = ce.Settings.SimulationSpace;
                            if (i.Args.size() == l->second.Fields.size() + 2 && p.Code[i.Args.back()].Kind != Type::Int)
                                r.Errors.push_back({asset, guid, "MaxEventsPerFrame", "Event limit requires int"});
                            if (i.Args.size() != l->second.Fields.size() + 1 &&
                                i.Args.size() != l->second.Fields.size() + 2)
                                r.Errors.push_back({asset, guid, "Payload", "Event payload field count mismatch"});
                            else
                                for (uint32_t k = 0; k < l->second.Fields.size(); ++k)
                                    if (p.Code[i.Args[k + 1]].Kind != l->second.Fields[k].Kind)
                                        r.Errors.push_back(
                                            {asset, guid, l->second.Fields[k].Name, "Event payload type mismatch"});
                        }
                        else
                        {
                            resource.Kind         = "ParticleReader";
                            resource.SourceHandle = i.Interface.SourceEmitter;
                            resource.Source       = sourceIndex(resource.SourceHandle, guid);
                            if (resource.Source == UINT32_MAX)
                                continue;
                            dependencies[index].insert(resource.Source);
                            auto& src = result[resource.Source];
                            if (src.Settings.Target != ce.Settings.Target)
                                r.Errors.push_back({asset,
                                                    guid,
                                                    "Source",
                                                    "Particle reader CPU/GPU simulation target mismatch with " +
                                                        resource.SourceHandle});
                            resource.Capacity      = src.Settings.Capacity;
                            resource.PositionSpace = src.Settings.SimulationSpace;
                            resource.IDOffset      = keep(resource.Source, "Particles.UniqueID", guid);
                            if (i.Op != "ParticleCount")
                                i.Parameter = keep(resource.Source,
                                                   i.Code,
                                                   guid,
                                                   i.Output == "Found" ? std::optional<Type> {} : i.Kind);
                        }
                        auto at = std::find_if(p.Resources.begin(), p.Resources.end(), [&](const auto& x) {
                            return x.Kind == resource.Kind && x.Source == resource.Source &&
                                   x.Channel == resource.Channel;
                        });
                        if (at == p.Resources.end())
                        {
                            resource.Binding = 7;
                            for (const auto& x : p.Resources)
                                resource.Binding = std::max(
                                    resource.Binding,
                                    x.Binding + ((x.Kind == "ParticleReader" || x.Kind == "Grid3D") ? 3u : 1u));
                            i.Resource = uint32_t(p.Resources.size());
                            p.Resources.push_back(resource);
                        }
                        else
                            i.Resource = uint32_t(at - p.Resources.begin());
                    }
                // GPU emitter-stage readers execute on the device, never against a CPU count mirror.
                if (p.StageKind == Stage::EmitterUpdate && ce.Settings.Target == SimTarget::GPU && !p.Resources.empty())
                {
                    p.Target         = SimTarget::GPU;
                    uint32_t binding = 7;
                    for (const auto& resource : p.Resources)
                        binding =
                            std::max(binding,
                                     resource.Binding +
                                         ((resource.Kind == "ParticleReader" || resource.Kind == "Grid3D") ? 3u : 1u));
                    ResourceBinding state;
                    state.Kind    = "EmitterState";
                    state.Binding = binding;
                    p.Resources.push_back(state);
                }
            };
            const auto bound = r.Errors.size();
            for (auto& [stage, p] : ce.Programs)
                bind(p, ce.Handle);
            for (auto& h : ce.Handlers)
                bind(h.Bytecode, h.Settings.Guid);
            for (auto& s : ce.SimulationStages)
                bind(s.Bytecode, s.Settings.Guid);
            locate(bound, ce.Handle);
        }
        // Stable Kahn ordering: among available emitters choose the first handle in the asset.
        std::set<uint32_t> done;
        while (done.size() < result.size())
        {
            uint32_t next = UINT32_MAX;
            for (uint32_t i = 0; i < result.size(); ++i)
                if (!done.count(i) && std::all_of(dependencies[i].begin(), dependencies[i].end(), [&](auto source) {
                        return done.count(source);
                    }))
                {
                    next = i;
                    break;
                }
            if (next == UINT32_MAX)
            {
                std::string message = "Emitter dependency cycle:";
                for (uint32_t i = 0; i < result.size(); ++i)
                    if (!done.count(i))
                        message += " " + result[i].Handle;
                r.Errors.push_back({asset, "", "Source", message});
                break;
            }
            done.insert(next);
            r.Product.TickOrder.push_back(next);
        }
        if (r.Errors.empty())
            for (auto& e : result)
            {
                auto generate = [&](Program& p) {
                    if (p.Target == SimTarget::GPU)
                        try
                        {
                            GenerateGpuProgram(p, e.Attributes);
                        }
                        catch (const std::exception& ex)
                        {
                            r.Errors.push_back({asset, e.Handle, "", ex.what()});
                        }
                };
                for (auto& [stage, p] : e.Programs)
                    generate(p);
                for (auto& h : e.Handlers)
                    generate(h.Bytecode);
                for (auto& s : e.SimulationStages)
                    generate(s.Bytecode);
            }
        return r;
    }
    bool CanConvert(Type from, Type to)
    {
        return from == to || (from == Type::Float && (to == Type::Vec2 || to == Type::Vec3 || to == Type::Vec4));
    }
    const std::vector<OperationInfo>& Operations()
    {
        static const std::vector<OperationInfo> table = BuildOperations();
        return table;
    }
    const OperationInfo* FindOperation(const std::string& name)
    {
        for (const auto& op : Operations())
            if (op.Name == name)
                return &op;
        return nullptr;
    }
    void OperationPins(const OperationInfo& op, Type kind, std::vector<Pin>& inputs, std::vector<Pin>& outputs)
    {
        const auto pin = [&](const PinSpec& spec) {
            Pin p;
            p.Name = spec.Name;
            p.Kind = spec.Rule == PinRule::Node ? kind : spec.Kind;
            if (spec.Default && spec.Default->Kind == p.Kind)
                p.Default = spec.Default;
            return p;
        };
        inputs.clear();
        outputs.clear();
        for (const auto& spec : op.Inputs)
        {
            auto p = pin(spec);
            if (!p.Default && op.Name != "Set" && op.Name != "Reroute")
                p.Default = p.Kind == Type::Int  ? Value::Int(0) :
                            p.Kind == Type::Bool ? Value::Bool(false) :
                                                   Value::Vector({}, p.Kind);
            inputs.push_back(p);
        }
        if (op.Name == "Make")
        {
            const char* names[] = {"X", "Y", "Z", "W"};
            const Type  scalar  = kind == Type::IVec3 ? Type::Int : Type::Float;
            for (uint32_t k = 0; k < Components(kind); ++k)
                inputs.push_back({names[k], scalar, scalar == Type::Int ? Value::Int(0) : Value::Float(0)});
        }
        for (const auto& spec : op.Outputs)
            outputs.push_back(pin(spec));
    }
    std::string FormatDiagnostics(const std::vector<Diagnostic>& errors)
    {
        std::string s;
        for (const auto& e : errors)
            s += e.Asset.ToString() + " node=" + e.Node + " pin=" + e.Pin + ": " + e.Message + "\n";
        return s;
    }
    glm::vec3 CurlSeedOffset(uint32_t seed, uint32_t emitter)
    {
        uint32_t h = Hash(seed ^ Hash(emitter));
        return {float(h & 255u) / 17.f, float((h >> 8) & 255u) / 17.f, float((h >> 16) & 255u) / 17.f};
    }
    glm::vec3 CurlNoise(glm::vec3 position, float frequency, glm::vec3 offset, int octaves)
    {
        auto potential = [&](glm::vec3 p) {
            return glm::vec3(Noise3D(p), Noise3D(p + glm::vec3(19, 37, 11)), Noise3D(p + glm::vec3(47, 7, 29)));
        };
        glm::vec3 result(0), p = position * frequency + offset;
        float     amplitude = 1;
        for (int octave = 0; octave < octaves; ++octave)
        {
            constexpr float e  = .01f;
            auto            dx = (potential(p + glm::vec3(e, 0, 0)) - potential(p - glm::vec3(e, 0, 0))) / (2 * e);
            auto            dy = (potential(p + glm::vec3(0, e, 0)) - potential(p - glm::vec3(0, e, 0))) / (2 * e);
            auto            dz = (potential(p + glm::vec3(0, 0, e)) - potential(p - glm::vec3(0, 0, e))) / (2 * e);
            result += amplitude * glm::vec3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
            p *= 2;
            amplitude *= .5f;
        }
        return result;
    }
    Value Evaluate(const Instruction& i, const std::vector<Value>& r, const ExecutionContext& c)
    {
        Value      out = Value::Vector({}, i.Kind);
        const auto a   = [&](size_t k) -> Value { return k < i.Args.size() ? r.at(i.Args[k]) : Value::Float(0); };
        const auto x = a(0), y = a(1), z = a(2);
        const auto op = i.Op;
        if (IsMeshOperation(op))
            return EvaluateMesh(i, r, c);
        if (IsVectorFieldOperation(op))
        {
            const auto& d = i.Interface.Geometry->Values;
            if (op == "SampleVectorField")
                out.Data = glm::vec4(SampleVectorField(i.Interface, glm::vec3(x.Data)), 0);
            else if (op == "VectorFieldBounds")
                out.Data = d[i.Output == "Max" ? 1 : 0];
            else
                out = Value::Int(int(d[2][i.Output == "X" ? 0 : i.Output == "Y" ? 1 : 2]));
            return out;
        }
        if (op == "FieldPosition" || op == "FieldVector")
        {
            auto  q     = op == "FieldPosition" ? z.Data : y.Data;
            float norm  = glm::dot(q, q);
            q           = norm > 1e-20f ? q / std::sqrt(norm) : glm::vec4(0, 0, 0, 1);
            glm::vec3 v = glm::vec3(x.Data);
            if (op == "FieldPosition")
            {
                v -= glm::vec3(y.Data);
                q = glm::vec4(-glm::vec3(q), q.w);
            }
            v += 2.f * glm::cross(glm::vec3(q), glm::cross(glm::vec3(q), v) + q.w * v);
            if (op == "FieldPosition")
            {
                auto scale = a(3).Data;
                for (int axis = 0; axis < 3; ++axis)
                    v[axis] /= std::abs(scale[axis]) >= 1e-6f ? scale[axis] : (scale[axis] < 0 ? -1e-6f : 1e-6f);
            }
            out.Data = glm::vec4(v, 0);
            return out;
        }
        if (op == "ToWorldVector")
        {
            out.Data = glm::vec4(
                (i.PositionSpace == Space::Local ? glm::mat3(c.LocalToWorld) : glm::mat3(1)) * glm::vec3(x.Data), 0);
            return out;
        }
        if (op == "ToSimulationVector")
        {
            out.Data =
                glm::vec4((i.PositionSpace == Space::Local ? glm::inverse(glm::mat3(c.LocalToWorld)) : glm::mat3(1)) *
                              glm::vec3(x.Data),
                          0);
            return out;
        }
        if (op == "EmitterSeedOffset")
        {
            out.Data = glm::vec4(CurlSeedOffset(c.Seed, c.EmitterIndex), 0);
            return out;
        }
        if (op == "CurlNoise")
        {
            auto result = CurlNoise(glm::vec3(x.Data), y.Data.x, glm::vec3(z.Data), a(3).Integer);
            out.Data    = glm::vec4(result, 0);
            return out;
        }
        if (op == "Collision")
            return CollisionValue(i, CollideParticle(i, r, c));
        if (op == "SampleForceFields")
        {
            if (!c.Fields)
                return out;
            glm::vec3  p(x.Data), v(y.Data);
            const bool local = i.PositionSpace == Space::Local;
            if (local)
            {
                p = glm::vec3(c.LocalToWorld * glm::vec4(p, 1));
                v = glm::mat3(c.LocalToWorld) * v;
            }
            auto      sample = c.Fields->Fields.Sample(p, v, c.FieldTime, c.FieldLayer, FieldSystem::Particles);
            glm::vec3 value  = i.Output == "Wind"  ? sample.Wind :
                               i.Output == "Force" ? sample.Force :
                                                     sample.Acceleration;
            if (i.Output == "Impulse")
            {
                value = {};
                for (const auto& impulse : c.Fields->Fields.GetImpulses())
                    if (impulse.Sequence >= c.FirstImpulse)
                        value += impulse.Sample(p, c.FieldLayer, FieldSystem::Particles);
            }
            if (local)
                value = glm::inverse(glm::mat3(c.LocalToWorld)) * value;
            out.Data = glm::vec4(value, 0);
            return out;
        }
        if (op == "ParticleCount" || op == "ReadAttribute" || op == "ReadAttributeByID")
            return c.ReadParticle ? c.ReadParticle(i, r) : out;
        if (op == "WriteEvent")
        {
            if (c.WriteEvent)
                c.WriteEvent(i, r);
            return Value::Int(0);
        }
        if (op == "Mod")
            return Value::Int(y.Integer ? x.Integer % y.Integer : 0);
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
            uint32_t h = Hash(c.Seed ^ Hash(c.EmitterIndex) ^ Hash(c.RandomID.value_or(c.UniqueID)) ^ i.RandomKey ^
                              Hash(lane) ^ Hash(c.EventIndex));
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
            const float v = Noise3D(glm::vec3(x.Data));
            out.Data      = glm::vec4(v);
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
            else if (op == "Exp")
                v = std::exp(av);
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
        if (p.StageKind == Stage::ParticleUpdate)
        {
            auto at                              = values.find("Particles.Position");
            values["Transient.PreviousPosition"] = at == values.end() ? Value::Vector({}, Type::Position) : at->second;
            values["Particles.HasCollided"]      = Value::Bool(false);
        }
        std::map<std::vector<uint32_t>, CollisionResult> collisions;
        std::vector<Value>                               r;
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
            else if (i.Op == "Collision")
            {
                auto at = collisions.find(i.Args);
                if (at == collisions.end())
                    at = collisions.emplace(i.Args, CollideParticle(i, r, c)).first;
                r.push_back(CollisionValue(i, at->second));
            }
            else if (i.Op == "SpawnBoundary")
            {
                if (i.PositionSpace == Space::World)
                {
                    const auto& parameter  = p.Parameters.at(i.Parameter);
                    auto        at         = values.find(parameter.Name);
                    auto        value      = at == values.end() ? *parameter.Default : at->second;
                    value.Data             = glm::vec4(glm::mat3(c.LocalToWorld) * glm::vec3(value.Data), 0);
                    values[parameter.Name] = value;
                }
                r.push_back(Value::Float(0));
            }
            else if (i.Op == "Set")
            {
                auto v                                    = r.at(i.Args.at(0));
                values[p.Parameters.at(i.Parameter).Name] = v;
                r.push_back(v);
            }
            else
            {
                auto context       = c;
                auto spawn         = values.find("Transient.SpawnIndex");
                context.SpawnIndex = spawn == values.end() ? 0 : uint32_t(spawn->second.Integer);
                r.push_back(Evaluate(i, r, context));
            }
        }
    }
    bool UsesForceFields(const Program& p)
    {
        return std::any_of(p.Code.begin(), p.Code.end(), [](const auto& i) { return i.Op == "SampleForceFields"; });
    }
    uint32_t ForceFieldsGpuBinding(const Program& p)
    {
        uint32_t binding = 7;
        for (const auto& r : p.Resources)
            binding = std::max(binding, r.Binding + ((r.Kind == "ParticleReader" || r.Kind == "Grid3D") ? 3u : 1u));
        for (const auto& entry : MeshGpuBindings(p))
            binding = std::max(binding, entry.second + 1);
        return binding;
    }
    bool UsesCollision(const Program& p)
    {
        return std::any_of(p.Code.begin(), p.Code.end(), [](const auto& i) {
            return i.Op == "Collision" || i.Op == "ColliderVelocity";
        });
    }
    bool UsesDepthCollision(const Program& p)
    {
        return std::any_of(p.Code.begin(), p.Code.end(), [&](const auto& i) {
            return i.Op == "Collision" && i.Args.size() == 13 &&
                   p.Code[i.Args[5]].Literal.Integer == int(CollisionMode::DepthBuffer);
        });
    }
    uint32_t CollisionGpuBinding(const Program& p) { return ForceFieldsGpuBinding(p) + (UsesForceFields(p) ? 1 : 0); }
    void     GenerateGpuProgram(Program& p, const std::vector<Attribute>& attributes)
    {
        const auto attribute = [&](const std::string& n) -> const Attribute* {
            for (const auto& a : attributes)
                if (a.Name == n)
                    return &a;
            return nullptr;
        };
        const auto decode = [](Type t, std::string x) {
            if (t == Type::IVec3)
                return "ivec3(" + x + ".xyz)";
            if (t == Type::Int)
                return "int(" + x + ".x)";
            if (t == Type::Bool)
                return "(" + x + ".x!=0u)";
            auto v = "uintBitsToFloat(" + x + ")";
            return v + (Components(t) == 1 ? ".x" : Components(t) == 2 ? ".xy" : Components(t) == 3 ? ".xyz" : "");
        };
        const auto encode = [](Type t, std::string x) {
            if (t == Type::IVec3)
                return "uvec4(uvec3(" + x + "),0u)";
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
        std::string       s = R"glsl(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Attributes { uvec4 attributes[]; };
layout(set=0,binding=1,std430) readonly buffer Parameters { uvec4 parameters[]; };
layout(set=0,binding=2,std430) buffer Flags { uint flags[]; };
layout(set=0,binding=3,std430) buffer FreeList { uint freeList[]; };
layout(set=0,binding=4,std430) buffer Counters { uint counters[]; };
layout(set=0,binding=5,std430) readonly buffer AliveList { uint aliveList[]; };
layout(set=0,binding=6,std430) readonly buffer DataInterfaces {vec4 diValues[];};
layout(push_constant) uniform Push {uint capacity;uint seed;uint emitter;uint requested;uint worldSpace;uint kill;} pc;
uint eventKey=0u;
uint hashV(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
float randomV(uint id,uint key,uint lane){return float(hashV(pc.seed^hashV(pc.emitter)^hashV(id)^key^hashV(lane)^hashV(eventKey))>>8)*(1.0/16777216.0);}
float noiseV(vec3 x){ivec3 b=ivec3(floor(x));vec3 f=fract(x);f=f*f*(3.0-2.0*f);float v=0;
for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int a=0;a<2;++a){ivec3 p=b+ivec3(a,y,z);float n=float(hashV(uint(p.x)^hashV(uint(p.y))^hashV(uint(p.z)))>>8)*(1.0/16777216.0);v+=n*(a!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y)*(z!=0?f.z:1-f.z);}return v;}
void main(){uint invocation=gl_GlobalInvocationID.x;uint slot;uint uniqueID;
)glsl";
        const std::string curlHelpers =
#include "VfxCurlGLSL.inc"
            ;
        s.insert(s.find("void main()"), curlHelpers);
        s.insert(s.find("void main()"), R"glsl(
vec3 fieldRotate(vec3 v,vec4 q){float n=dot(q,q);q=n>1e-20?q*inversesqrt(n):vec4(0,0,0,1);return v+2.0*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
vec3 fieldPosition(vec3 p,vec3 t,vec4 q,vec3 scale){vec3 d;for(int a=0;a<3;++a)d[a]=abs(scale[a])>=1e-6?scale[a]:(scale[a]<0?-1e-6:1e-6);return fieldRotate(p-t,vec4(-q.xyz,q.w))/d;}
)glsl");
        if (UsesCollision(p))
        {
            std::string helpers = "layout(set=0,binding=" + std::to_string(CollisionGpuBinding(p)) +
                                  ",std430) readonly buffer SceneColliders {vec4 collisionWords[];};\n";
            helpers += "layout(set=0,binding=" + std::to_string(CollisionGpuBinding(p) + 1) +
                       ") uniform sampler2D collisionDepth;\n";
            helpers +=
#include "VfxCollisionGLSL.inc"
                ;
            s.insert(s.find("void main()"), helpers);
        }
        if (UsesForceFields(p))
        {
            std::string helpers = "layout(set=0,binding=" + std::to_string(ForceFieldsGpuBinding(p)) +
                                  ",std430) readonly buffer SceneFields {vec4 fieldWords[];};\n";
            helpers +=
#include "VfxFieldsGLSL.inc"
                ;
            s.insert(s.find("void main()"), helpers);
        }
        if (std::any_of(p.Code.begin(), p.Code.end(), [](const auto& i) {
                return IsMeshOperation(i.Op) || IsVectorFieldOperation(i.Op);
            }))
        {
            std::string geometryDeclarations, accessor = "vec4 meshStatic(int g,int o){switch(g){";
            auto        meshBindings = MeshGpuBindings(p);
            for (size_t index = 0; index < meshBindings.size(); ++index)
            {
                auto id = std::to_string(index);
                geometryDeclarations += "layout(set=0,binding=" + std::to_string(meshBindings[index].second) +
                                        ",std430) readonly buffer MeshGeometry" + id + "{vec4 meshValues" + id +
                                        "[];};\n";
                accessor += "case " + id + ":return meshValues" + id + "[o];";
            }
            accessor += "}return vec4(0); }\n";
            accessor +=
#include "VfxVectorFieldGLSL.inc"
                ;
            const std::string helper = geometryDeclarations + accessor +
#include "VfxMeshSamplingGLSL.inc"
                ;
            s.insert(s.find("void main()"), helper);
        }
        std::string declarations;
        for (uint32_t r = 0; r < p.Resources.size(); ++r)
        {
            const auto& resource = p.Resources[r];
            auto        id = std::to_string(r), binding = std::to_string(resource.Binding);
            if (resource.Kind == "Grid3D")
            {
                const auto  suffix = id;
                const auto& g      = resource.Grid;
                const auto  res    = "ivec3(" + std::to_string(g.Resolution.x) + "," + std::to_string(g.Resolution.y) +
                                     "," + std::to_string(g.Resolution.z) + ")";
                const auto  cells  = std::to_string(g.Resolution.x * g.Resolution.y * g.Resolution.z);
                declarations += "layout(set=0,binding=" + binding + ",std430) readonly buffer GridRead" + id +
                                "{vec4 gridRead" + id + "[];};\n";
                declarations += "layout(set=0,binding=" + std::to_string(resource.Binding + 1) +
                                ",std430) buffer GridWrite" + id + "{vec4 gridWrite" + id + "[];};\n";
                declarations += "layout(set=0,binding=" + std::to_string(resource.Binding + 2) +
                                ",std430) buffer GridAtomic" + id + "{int gridAtomic" + id + "[];};\n";
                declarations += "int gridOffset" + id + "(int a,ivec3 p){p=clamp(p,ivec3(0)," + res + "-1);return a*" +
                                cells + "+p.x+" + std::to_string(g.Resolution.x) + "*(p.y+" +
                                std::to_string(g.Resolution.y) + "*p.z); }\n";
                declarations += "vec4 gridSample" + id +
                                "(int a,vec3 p){ivec3 lo=ivec3(floor(p));vec3 f=fract(p);vec4 v=vec4(0);for(int "
                                "z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x)v+=gridRead" +
                                id + "[gridOffset" + id +
                                "(a,lo+ivec3(x,y,z))]*(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y)*(z==0?1-f.z:f.z);return v;}\n";
            }
            else if (resource.Kind == "ParticleReader")
            {
                declarations += "layout(set=0,binding=" + binding + ",std430) readonly buffer ReaderAttributes" + id +
                                "{uvec4 readerAttributes" + id + "[];};\n";
                declarations += "layout(set=0,binding=" + std::to_string(resource.Binding + 1) +
                                ",std430) readonly buffer ReaderAlive" + id + "{uint readerAlive" + id + "[];};\n";
                declarations += "layout(set=0,binding=" + std::to_string(resource.Binding + 2) +
                                ",std430) readonly buffer ReaderCounters" + id + "{uint readerCounters" + id +
                                "[];};\n";
            }
            else if (resource.Kind == "EmitterState")
                declarations +=
                    "layout(set=0,binding=" + binding + ",std430) buffer EmitterState{uvec4 emitterState[];};\n";
            else
                declarations += "layout(set=0,binding=" + binding + ",std430) " +
                                (resource.Kind == "EventInput" ? "readonly " : "") + "buffer Events" + id +
                                "{uvec4 events" + id + "[];};\n";
        }
        s.insert(s.find("void main()"), declarations);
        const bool gridStage = !p.IterationGrid.empty();
        const bool emitter   = p.StageKind == Stage::EmitterUpdate;
        if (gridStage)
        {
            s.replace(s.find("layout(local_size_x=64) in;"),
                      std::string("layout(local_size_x=64) in;").size(),
                      "layout(local_size_x=4,local_size_y=4,local_size_z=4) in;");
        }
        const bool spawn = p.StageKind == Stage::ParticleSpawn || (p.Handler && p.Mode == EventMode::SpawnedParticles);
        if (gridStage)
        {
            auto        at  = std::find_if(p.Resources.begin(), p.Resources.end(), [&](const auto& r) {
                return r.Kind == "Grid3D" && r.Channel == p.IterationGrid;
            });
            const auto& g   = at->Grid;
            auto        id  = std::to_string(at - p.Resources.begin());
            auto        res = "ivec3(" + std::to_string(g.Resolution.x) + "," + std::to_string(g.Resolution.y) + "," +
                              std::to_string(g.Resolution.z) + ")";
            s += "ivec3 gridExecutionIndex=ivec3(gl_GlobalInvocationID);if(any(greaterThanEqual(gridExecutionIndex," +
                 res + ")))return;slot=0u;uniqueID=0u;\n";
            for (uint32_t a = 0; a < g.GridAttributes.size(); ++a)
                s += "gridWrite" + id + "[gridOffset" + id + "(" + std::to_string(a) +
                     ",gridExecutionIndex)]=gridRead" + id + "[gridOffset" + id + "(" + std::to_string(a) +
                     ",gridExecutionIndex)];\n";
        }
        else if (emitter)
            s += "if(invocation!=0u)return;slot=0u;uniqueID=0u;\n";
        else if (spawn)
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
        s += ");\nmat4 inverseOwner=inverse(owner);\n";
        for (const auto& a : attributes)
        {
            s += GType(a.Kind) + " a" + std::to_string(a.Offset) + "=" +
                 ((spawn || emitter || gridStage) ?
                      Literal(a.Default) :
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
        if (p.Handler)
        {
            const auto& event  = p.Resources.at(0);
            auto        stride = std::to_string(event.Layout.Fields.size() + 1);
            if (spawn)
                s += "uint eventIndex=invocation/" + std::to_string(std::max(1u, p.SpawnNumber)) + "u;\n";
            else
                s += "uint eventCount=min(events0[0].x," + std::to_string(p.MaxEventsPerFrame) +
                     "u);for(uint eventIndex=0u;eventIndex<eventCount;++eventIndex){\n";
            s += "eventKey=events0[1u+eventIndex*" + stride + "u+" + std::to_string(event.Layout.Fields.size()) +
                 "u].x;\n";
        }
        for (uint32_t k = 0; k < p.Parameters.size(); ++k)
        {
            const auto& v = p.Parameters[k];
            auto        a = attribute(v.Name);
            std::string value =
                a ? "a" + std::to_string(a->Offset) : decode(v.Kind, "parameters[" + std::to_string(k) + "]");
            if (v.Name == "Transient.PreviousPosition" && attribute("Particles.Position"))
                value = "a" + std::to_string(attribute("Particles.Position")->Offset);
            if (v.Name == "Particles.HasCollided" && p.StageKind == Stage::ParticleUpdate)
                value = "false";
            if (v.Name == "Transient.SpawnIndex")
                value = "int(invocation)";
            if (emitter && v.Name.rfind("Emitter.", 0) == 0 && v.Name != "Emitter.SpawnCountThisFrame")
                value = decode(v.Kind, "emitterState[" + std::to_string(k) + "]");
            if (p.Handler && v.Name.rfind("Transient.Event.", 0) == 0)
            {
                const auto& event = p.Resources.at(0);
                for (uint32_t f = 0; f < event.Layout.Fields.size(); ++f)
                    if (v.Name == "Transient.Event." + event.Layout.Fields[f].Name)
                    {
                        value = decode(v.Kind,
                                       "events0[1u+eventIndex*" + std::to_string(event.Layout.Fields.size() + 1) +
                                           "u+" + std::to_string(f) + "u]");
                        if (v.Kind == Type::Position)
                            value = event.PositionSpace == Space::World ?
                                        "(pc.worldSpace==0u?(inverseOwner*vec4(" + value + ",1)).xyz:" + value + ")" :
                                        "(pc.worldSpace!=0u?(owner*vec4(" + value + ",1)).xyz:" + value + ")";
                    }
            }
            s += GType(v.Kind) + " p" + std::to_string(k) + "=" + value + ";\n";
        }
        std::map<std::vector<uint32_t>, std::string> collisionResults;
        std::string                                  customHelpers;
        for (uint32_t k = 0; k < p.Code.size(); ++k)
        {
            const auto& i   = p.Code[k];
            const auto  arg = [&](size_t a) { return a < i.Args.size() ? "r" + std::to_string(i.Args[a]) : "0.0"; };
            std::string e;
            auto        a = arg(0), b = arg(1), c = arg(2), op = i.Op;
            if (op == "GridExecutionIndex")
                e = "gridExecutionIndex";
            else if (op.rfind("Grid", 0) == 0)
            {
                const auto& g      = i.Interface;
                auto        id     = std::to_string(i.Resource);
                auto        attr   = std::to_string(i.Parameter);
                const auto  res    = "ivec3(" + std::to_string(g.Resolution.x) + "," + std::to_string(g.Resolution.y) +
                                     "," + std::to_string(g.Resolution.z) + ")";
                const auto  bounds = Literal(Value::Vector(glm::vec4(g.WorldBounds, 0), Type::Vec3));
                // Local grids follow the complete owner transform. World grids follow owner translation.
                auto toLocal = [&](std::string v) {
                    return "(pc.worldSpace==0u?(inverseOwner*vec4(" + v + ",1)).xyz:(" + v + "-owner[3].xyz))";
                };
                auto toWorld = [&](std::string v) {
                    return "(pc.worldSpace==0u?(owner*vec4(" + v + ",1)).xyz:(" + v + "+owner[3].xyz))";
                };
                auto coordinate = [&](std::string v) {
                    return "((" + toLocal(v) + "/" + bounds + "+0.5)*vec3(" + res + "))";
                };
                auto offset  = "gridOffset" + id + "(" + attr + "," + a + ")";
                auto swizzle = Components(i.Kind) == 1 ? ".x" :
                               Components(i.Kind) == 2 ? ".xy" :
                               Components(i.Kind) == 3 ? ".xyz" :
                                                         "";
                if (op == "GridNumCells")
                    e = res;
                else if (op == "GridCellSize")
                    e = bounds + "/vec3(" + res + ")";
                else if (op == "GridIndexToWorld")
                    e = toWorld("((vec3(" + a + ")+0.5)/vec3(" + res + ")-0.5)*" + bounds);
                else if (op == "GridWorldToCell")
                    e = coordinate(a) + "-0.5";
                else if (op == "GridWorldToIndex")
                    e = "ivec3(floor(" + coordinate(a) + "))";
                else if (op == "GridGet")
                    e = "gridRead" + id + "[" + offset + "]" + swizzle;
                else if (op == "GridSample")
                    e = "gridSample" + id + "(" + attr + "," + coordinate(a) + "-0.5)" + swizzle;
                else if (op == "GridLightMarch")
                {
                    // March world-space distances even for rotated/scaled local grids.
                    const auto cell      = "cell" + std::to_string(k);
                    const auto tau       = "tau" + std::to_string(k);
                    const auto h         = "step" + std::to_string(k);
                    const auto localCell = bounds + "/vec3(" + res + ")";
                    s += "vec3 " + cell + "=" + toWorld("((vec3(" + a + ")+0.5)/vec3(" + res + ")-0.5)*" + bounds) +
                         ";\n";
                    s += "vec3 cs" + std::to_string(k) + "=" + localCell + ";\n";
                    for (int axis = 0; axis < 3; ++axis)
                        s += "if(pc.worldSpace==0u)cs" + std::to_string(k) + "[" + std::to_string(axis) +
                             "]*=length(owner[" + std::to_string(axis) + "].xyz);\n";
                    auto cs = "cs" + std::to_string(k);
                    s += "float " + h + "=" + arg(4) + ">0.0?" + arg(4) + ":min(" + cs + ".x,min(" + cs + ".y," + cs +
                         ".z));\n";
                    s += "float " + tau + "=0.0;if(" + b + ".w!=0.0){for(int stepIndex=1;stepIndex<=clamp(" + arg(3) +
                         ",1,64);++stepIndex){vec3 samplePos=" + cell + "+" + b + ".xyz*(float(stepIndex)*" + h +
                         ");vec3 q=" + coordinate("samplePos") +
                         ";if(any(lessThan(q,vec3(0)))||any(greaterThanEqual(q,vec3(" + res + "))))break;" + tau +
                         "+=gridSample" + id + "(" + attr + ",q-0.5).x*max(0.0," + c + ")*" + h + ";}}\n";
                    e = "exp(-" + tau + ")";
                }
                else if (op == "GridSet")
                {
                    s += "gridWrite" + id + "[" + offset + "]" + swizzle + "=" + b + ";\n";
                    e = b;
                }
                else if (op == "GridAddFloat")
                {
                    // Signed fixed point, 16 fractional bits. Each component has an independent int32 channel.
                    s += "atomicAdd(gridAtomic" + id + "[4*(" + offset + ")+" + b + "],int(round(" + c +
                         "*65536.0)));\n";
                    e = c;
                }
            }
            else if (op == "SpawnBoundary")
            {
                if (i.PositionSpace == Space::World)
                    s += "p" + std::to_string(i.Parameter) + "=mat3(owner)*p" + std::to_string(i.Parameter) + ";\n";
                e = "0.0";
            }
            else if (op == "ColliderVelocity")
                e = "colliderVelocityV(" + a + "," + b + ",parameters[" + std::to_string(p.Parameters.size() + 4) +
                    "].z)";
            else if (op == "Collision")
            {
                auto at = collisionResults.find(i.Args);
                if (at == collisionResults.end())
                {
                    auto name = "collision" + std::to_string(k);
                    at        = collisionResults.emplace(i.Args, name).first;
                    std::string args;
                    for (size_t n = 0; n < i.Args.size(); ++n)
                        args += (n ? "," : "") + arg(n);
                    auto offset = uint32_t(p.Parameters.size()) + 6;
                    auto matrix = [&](uint32_t first) {
                        std::string m = "mat4(";
                        for (uint32_t col = 0; col < 4; ++col)
                            m += (col ? "," : "") + std::string("uintBitsToFloat(parameters[") +
                                 std::to_string(first + col) + "])";
                        return m + ")";
                    };
                    s += "CollisionResultV " + name + "=collideParticleV(" + args + ",owner," +
                         (i.PositionSpace == Space::Local ? "true" : "false") + ",parameters[" +
                         std::to_string(p.Parameters.size() + 4) + "].z," + matrix(offset) + "," + matrix(offset + 4) +
                         ",parameters[" + std::to_string(offset + 8) + "].x!=0u);\n";
                }
                e = at->second + "." + i.Output;
            }
            else if (op == "WriteEvent")
            {
                const auto& resource = p.Resources.at(i.Resource);
                auto        name     = "events" + std::to_string(i.Resource);
                auto        capacity = std::to_string(resource.Capacity) + "u";
                auto        limit    = i.Args.size() == resource.Layout.Fields.size() + 2 ?
                                           "uint(max(0," + arg(i.Args.size() - 1) + "))" :
                                           "0xffffffffu";
                auto        tag      = std::to_string(k);
                // Reserve the per-frame generation budget independently of storage capacity.
                s += "uint wanted" + tag + "=uint(max(0," + a + "));uint generated" + tag + "=" + name +
                     "[0].z;uint allowed" + tag + "=0u;\n";
                s += "while(generated" + tag + "<" + limit + "){uint candidate=min(wanted" + tag + "," + limit +
                     "-generated" + tag + ");uint previous=atomicCompSwap(" + name + "[0].z,generated" + tag +
                     ",generated" + tag + "+candidate);if(previous==generated" + tag + "){allowed" + tag +
                     "=candidate;break;}generated" + tag + "=previous;}\n";
                s += "uint attempts" + tag + "=min(allowed" + tag + "," + capacity + ");atomicAdd(" + name +
                     "[0].y,allowed" + tag + "-attempts" + tag + ");\n";
                s += "for(uint ordinal=0u;ordinal<attempts" + tag + ";++ordinal){uint at=" + name +
                     "[0].x;bool reserved=false;while(at<" + capacity + "){uint previous=atomicCompSwap(" + name +
                     "[0].x,at,at+1u);if(previous==at){reserved=true;break;}at=previous;}if(!reserved){atomicAdd(" +
                     name + "[0].y,1u);continue;}\n";
                for (uint32_t f = 0; f < resource.Layout.Fields.size(); ++f)
                    s += name + "[1u+at*" + std::to_string(resource.Layout.Fields.size() + 1) + "u+" +
                         std::to_string(f) + "u]=" + encode(resource.Layout.Fields[f].Kind, arg(f + 1)) + ";\n";
                s += name + "[1u+at*" + std::to_string(resource.Layout.Fields.size() + 1) + "u+" +
                     std::to_string(resource.Layout.Fields.size()) + "u]=uvec4(hashV(uniqueID^hashV(ordinal^" +
                     std::to_string(i.RandomKey) + "u)),0,0,0); }\n";
                e = "0";
            }
            else if (op == "ParticleCount" || op == "ReadAttribute" || op == "ReadAttributeByID")
            {
                const auto& resource = p.Resources.at(i.Resource);
                auto        id = std::to_string(i.Resource), tag = std::to_string(k);
                auto        count = "readerCounters" + id + "[1]";
                if (op == "ParticleCount")
                    e = "int(" + count + ")";
                else
                {
                    auto found = "found" + tag, sourceSlot = "sourceSlot" + tag;
                    s += "bool " + found + "=false;uint " + sourceSlot + "=0u;\n";
                    if (op == "ReadAttribute")
                        s += "if(" + a + ">=0&&uint(" + a + ")<" + count + "){" + found + "=true;" + sourceSlot +
                             "=readerAlive" + id + "[uint(" + a + ")];}\n";
                    else
                        s += "for(uint j=0u;j<" + count + ";++j){uint candidate=readerAlive" + id +
                             "[j];if(readerAttributes" + id + "[" + std::to_string(resource.IDOffset) + "u*" +
                             std::to_string(resource.Capacity) + "u+candidate].x==uint(" + a + ")){" + found +
                             "=true;" + sourceSlot + "=candidate;break;}}\n";
                    if (i.Output == "Found")
                        e = found;
                    else
                    {
                        auto value = decode(i.Kind,
                                            "readerAttributes" + id + "[" + std::to_string(i.Parameter) + "u*" +
                                                std::to_string(resource.Capacity) + "u+" + sourceSlot + "]");
                        if (i.Kind == Type::Position)
                            value = resource.PositionSpace == Space::World ?
                                        "(pc.worldSpace==0u?(inverseOwner*vec4(" + value + ",1)).xyz:" + value + ")" :
                                        "(pc.worldSpace!=0u?(owner*vec4(" + value + ",1)).xyz:" + value + ")";
                        e = "(" + found + "?" + value + ":" + GType(i.Kind) + "(0))";
                    }
                }
            }
            else if (op == "Mod")
                e = "(" + b + "==0?0:" + a + "%" + b + ")";
            else if (op == "OwnerMatrixColumn")
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
                    return "randomV(" +
                           (p.Handler ? (p.Mode == EventMode::SpawnedParticles ?
                                             "(invocation%" + std::to_string(std::max(1u, p.SpawnNumber)) + "u)" :
                                             "uniqueID") :
                                        "uniqueID") +
                           "," + std::to_string(i.RandomKey) + "u," + std::to_string(lane) + "u)";
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
            else if (op == "ToSimulationVector")
                e = "(pc.worldSpace!=0u?" + a + ":inverse(mat3(owner))*" + a + ")";
            else if (op == "ToWorldVector")
                e = "(pc.worldSpace!=0u?" + a + ":mat3(owner)*" + a + ")";
            else if (op == "EmitterSeedOffset")
                e = "seedOffsetV()";
            else if (op == "FieldPosition")
                e = "fieldPosition(" + a + "," + b + "," + c + "," + arg(3) + ")";
            else if (op == "FieldVector")
                e = "fieldRotate(" + a + "," + b + ")";
            else if (IsVectorFieldOperation(op))
            {
                auto   bindings = MeshGpuBindings(p);
                size_t index    = 0;
                while (MeshBindingKey(*bindings.at(index).first) != MeshBindingKey(i.Interface))
                    ++index;
                auto g = std::to_string(index);
                if (op == "SampleVectorField")
                    e = "sampleVectorField(" + g + "," + a + ",bvec3(" + (i.Interface.Tiling[0] ? "true" : "false") +
                        "," + (i.Interface.Tiling[1] ? "true" : "false") + "," +
                        (i.Interface.Tiling[2] ? "true" : "false") + "))*" + F(i.Interface.Intensity);
                else if (op == "VectorFieldBounds")
                    e = "meshStatic(" + g + "," + (i.Output == "Max" ? "1" : "0") + ").xyz";
                else
                    e = "int(meshStatic(" + g + ",2)." + (i.Output == "X" ? "x" : i.Output == "Y" ? "y" : "z") + ")";
            }
            else if (op == "CurlNoise")
                e = "curlV(" + a + "," + b + "," + c + "," + arg(3) + ")";
            else if (op == "SampleForceFields")
            {
                auto pos    = i.PositionSpace == Space::Local ? "(owner*vec4(" + a + ",1)).xyz" : a;
                auto vel    = i.PositionSpace == Space::Local ? "(mat3(owner)*" + b + ")" : b;
                int  output = i.Output == "Wind" ? 0 : i.Output == "Force" ? 1 : i.Output == "Acceleration" ? 2 : 3;
                e           = "sampleFieldsV(" + pos + "," + vel + "," + std::to_string(output) + ",parameters[" +
                              std::to_string(p.Parameters.size() + 4) + "],parameters[" +
                              std::to_string(p.Parameters.size() + 5) + "] )";
                if (i.PositionSpace == Space::Local)
                    e = "(inverse(mat3(owner))*" + e + ")";
            }
            else if (op == "Noise3D")
                e = "noiseV(" + a + ")";
            else if (op.find("Transform") != std::string::npos)
                e = "(" + std::string(op.rfind("Inverse", 0) == 0 ? "inverseOwner" : "owner") + "*vec4(" + a + "," +
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
            else if (IsMeshOperation(op))
            {
                auto   meshBindings  = MeshGpuBindings(p);
                size_t geometryIndex = 0;
                while (MeshBindingKey(*meshBindings.at(geometryIndex).first) != MeshBindingKey(i.Interface))
                    ++geometryIndex;
                auto geometry = std::to_string(geometryIndex);
                auto base     = "int(diValues[" + std::to_string(p.Interfaces.at(i.Parameter).ValueOffset) + "].x)";
                if (op == "RandomTriangle" || op == "RandomVertex" || op == "RandomBone")
                {
                    auto randomID = p.Handler && spawn ?
                                        "invocation%" + std::to_string(std::max(1u, p.SpawnNumber)) + "u" :
                                        "uniqueID";
                    e             = "meshRandom(" + base + "," + geometry + "," +
                                    (op == "RandomTriangle" ? "0" :
                                     op == "RandomVertex"   ? "1" :
                                                              "2") +
                                    ",randomV(" + randomID + "," + std::to_string(i.RandomKey) + "u^hashV(" +
                                    (spawn ? "invocation" : "0u") + "+0x9e3779b9u),0u))";
                }
                else if (op == "BoneByName")
                    e = std::to_string(i.Literal.Integer);
                else if (op == "TriangleCount" || op == "VertexCount" || op == "BoneCount")
                    e = "(diValues[" + base + "+2].w==0?0:int(diValues[" + base +
                        (op == "VertexCount" ? "+3].w" :
                         op == "BoneCount"   ? "].w" :
                                               "].y") +
                        "))";
                else
                {
                    int field = i.Output == "Position" ? 0 :
                                i.Output == "Normal"   ? 1 :
                                i.Output == "Tangent"  ? 2 :
                                i.Output == "UV0"      ? 3 :
                                i.Output == "Color"    ? 4 :
                                i.Output == "Rotation" ? 7 :
                                i.Output == "Valid"    ? 8 :
                                                         5;
                    e         = "meshSample(" + base + "," + geometry + "," + a + "," +
                                (i.Args.size() == 2 ? b : "vec3(1,0,0)") + "," +
                                (op == "SampleBone"   ? "2" :
                                 op == "SampleVertex" ? "1" :
                                                        "0") +
                                "," + std::to_string(field) + ")";
                    if (i.Kind == Type::Bool)
                        e = "(" + e + ".x!=0)";
                    else if (Components(i.Kind) == 2)
                        e += ".xy";
                    else if (Components(i.Kind) == 3)
                        e += ".xyz";
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
        if (p.Handler && p.Mode == EventMode::EveryParticle)
            s += "if(!a" + std::to_string(attribute("Particles.Alive")->Offset) + ")break;}\n";
        if (emitter)
        {
            for (uint32_t k = 0; k < p.Parameters.size(); ++k)
            {
                const auto& v = p.Parameters[k];
                if (v.Name.rfind("Emitter.", 0) == 0 && p.Writes.count(v.Name))
                    s += "emitterState[" + std::to_string(k) + "]=" + encode(v.Kind, "p" + std::to_string(k)) + ";\n";
                if (v.Name == "Emitter.SpawnCountThisFrame")
                    s += "counters[15]=uint(max(0,p" + std::to_string(k) + "));\n";
            }
            s += "return;\n";
        }
        if (spawn && !p.Handler)
            if (auto a = attribute("Particles.Velocity"))
                s += "if(pc.worldSpace!=0u)a" + std::to_string(a->Offset) + "=mat3(owner)*a" +
                     std::to_string(a->Offset) + ";\n";
        if (gridStage)
            s += "return;\n";
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
                n["Handler"]           = p.Handler;
                n["Mode"]              = int(p.Mode);
                n["SpawnNumber"]       = p.SpawnNumber;
                n["MaxEventsPerFrame"] = p.MaxEventsPerFrame;
                n["Resources"]         = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& r : p.Resources)
                {
                    YAML::Node resource;
                    if (r.Kind == "Grid3D")
                        resource["Grid"] = EncodeInterface(r.Grid);
                    resource["Kind"]         = r.Kind;
                    resource["Channel"]      = r.Channel;
                    resource["SourceHandle"] = r.SourceHandle;
                    resource["Source"]       = r.Source;
                    resource["Binding"]      = r.Binding;
                    resource["Capacity"]     = r.Capacity;
                    resource["IDOffset"]     = r.IDOffset;
                    resource["Space"]        = int(r.PositionSpace);
                    if (!r.Layout.Name.empty())
                        resource["Layout"] = EncodeEventLayout(r.Layout);
                    n["Resources"].push_back(resource);
                }
                n["IterationGrid"]     = p.IterationGrid;
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
                    a["Resource"]           = i.Resource;
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
                p.StageKind         = Stage(n["Stage"].as<int>());
                p.Handler           = n["Handler"].as<bool>(false);
                p.Mode              = EventMode(n["Mode"].as<int>(0));
                p.SpawnNumber       = n["SpawnNumber"].as<uint32_t>(1);
                p.MaxEventsPerFrame = n["MaxEventsPerFrame"].as<uint32_t>(UINT32_MAX);
                for (const auto& node : n["Resources"])
                {
                    ResourceBinding resource;
                    resource.Kind = node["Kind"].as<std::string>();
                    if (resource.Kind == "Grid3D")
                        resource.Grid = DecodeInterface(node["Grid"]);
                    resource.Channel       = node["Channel"].as<std::string>();
                    resource.SourceHandle  = node["SourceHandle"].as<std::string>();
                    resource.Source        = node["Source"].as<uint32_t>();
                    resource.Binding       = node["Binding"].as<uint32_t>();
                    resource.Capacity      = node["Capacity"].as<uint32_t>();
                    resource.IDOffset      = node["IDOffset"].as<uint32_t>();
                    resource.PositionSpace = Space(node["Space"].as<int>());
                    if (node["Layout"])
                        resource.Layout = DecodeEventLayout(node["Layout"]);
                    p.Resources.push_back(resource);
                }
                p.IterationGrid = n["IterationGrid"].as<std::string>("");
                p.Target        = SimTarget(n["Target"].as<int>());
                p.GLSL          = n["GLSL"].as<std::string>();
                p.SpirV         = n["SpirV"].as<std::vector<uint32_t>>();
                if (p.StageKind > Stage::SimulationStage || p.Target > SimTarget::GPU)
                    throw std::invalid_argument("Invalid VFX product stage");
                for (const auto& a : n["Parameters"])
                {
                    Parameter v;
                    v.Name = a["Name"].as<std::string>();
                    auto t = a["Type"].as<int>();
                    if (t < 0 || t > int(Type::IVec3))
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
                    i.Op       = a["Op"].as<std::string>();
                    i.Resource = a["Resource"].as<uint32_t>(0);
                    auto t     = a["Type"].as<int>();
                    if (t < 0 || t > int(Type::IVec3))
                        throw std::invalid_argument("Invalid VFX bytecode type");
                    i.Kind = Type(t);
                    i.Args = a["Args"].as<std::vector<uint32_t>>();
                    for (auto r : i.Args)
                        if (r >= p.Code.size())
                            throw std::invalid_argument("Invalid VFX bytecode register");
                    i.Literal       = DecodeValue(a["Literal"]);
                    i.PositionSpace = Space(a["PositionSpace"].as<int>(0));
                    i.Parameter     = a["Parameter"].as<uint32_t>();
                    if ((i.Op == "Get" || i.Op == "Set" || i.Op == "SpawnBoundary") &&
                        i.Parameter >= p.Parameters.size())
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
                    if (binding.Instruction >= p.Code.size() ||
                        (p.Code[binding.Instruction].Op != "Sample" &&
                         !IsMeshOperation(p.Code[binding.Instruction].Op) &&
                         !IsVectorFieldOperation(p.Code[binding.Instruction].Op)) ||
                        binding.Binding != 6 || binding.ValueOffset != offset ||
                        binding.ValueCount != ((IsMeshOperation(p.Code[binding.Instruction].Op) ||
                                                IsVectorFieldOperation(p.Code[binding.Instruction].Op)) ?
                                                   1u :
                                                   p.Code[binding.Instruction].Interface.Values.size()))
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
        n["VfxProductVersion"] = 3;
        n["TickOrder"]         = p.TickOrder;
        n["Settings"]          = EncodeSystem(p.Settings);
        n["Programs"]          = ProgramsNode(p.Programs);
        n["Emitters"]          = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& e : p.Emitters)
        {
            YAML::Node a;
            a["Settings"] = EncodeEmitter(e.Settings);
            a["Handle"]   = e.Handle;
            for (const auto& [guid, radius] : e.MeshRadii)
                a["MeshRadii"][guid] = radius;
            a["EventLayouts"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& [name, layout] : e.EventLayouts)
                a["EventLayouts"].push_back(EncodeEventLayout(layout));
            a["Handlers"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& h : e.Handlers)
            {
                YAML::Node handler;
                handler["Guid"]     = h.Settings.Guid;
                handler["Source"]   = h.Source;
                handler["Programs"] = ProgramsNode({{Stage::EventHandler, h.Bytecode}});
                a["Handlers"].push_back(handler);
            }
            a["SimulationStages"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& s : e.SimulationStages)
            {
                YAML::Node stage;
                stage["Guid"]     = s.Settings.Guid;
                stage["Programs"] = ProgramsNode({{Stage::SimulationStage, s.Bytecode}});
                a["SimulationStages"].push_back(stage);
            }
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
        if (n["VfxProductVersion"].as<int>() != 3)
            throw std::invalid_argument("Unsupported VFX product version; reimport with VFX product version 3");
        CompiledSystem p;
        p.Settings  = DecodeSystem(n["Settings"]);
        p.TickOrder = n["TickOrder"].as<std::vector<uint32_t>>();
        p.Programs  = ReadPrograms(n["Programs"]);
        for (const auto& a : n["Emitters"])
        {
            CompiledEmitter e;
            e.Settings = DecodeEmitter(a["Settings"]);
            e.Handle   = a["Handle"].as<std::string>();
            for (const auto& b : a["MeshRadii"])
                e.MeshRadii[b.first.as<std::string>()] = b.second.as<float>();
            for (const auto& l : a["EventLayouts"])
            {
                auto layout                 = DecodeEventLayout(l);
                e.EventLayouts[layout.Name] = layout;
            }
            for (const auto& h : a["Handlers"])
            {
                CompiledHandler handler;
                auto            guid = h["Guid"].as<std::string>();
                auto at = std::find_if(e.Settings.Handlers.begin(), e.Settings.Handlers.end(), [&](const auto& x) {
                    return x.Guid == guid;
                });
                if (at == e.Settings.Handlers.end())
                    throw std::invalid_argument("Invalid VFX product handler GUID");
                handler.Settings = *at;
                handler.Source   = h["Source"].as<uint32_t>();
                handler.Bytecode = ReadPrograms(h["Programs"]).at(Stage::EventHandler);
                e.Handlers.push_back(handler);
            }
            for (const auto& s : a["SimulationStages"])
            {
                const auto guid = s["Guid"].as<std::string>();
                auto       at   = std::find_if(e.Settings.SimulationStages.begin(),
                                               e.Settings.SimulationStages.end(),
                                               [&](const auto& x) { return x.Guid == guid; });
                if (at == e.Settings.SimulationStages.end())
                    throw std::invalid_argument("Invalid VFX product simulation stage GUID");
                e.SimulationStages.push_back({*at, ReadPrograms(s["Programs"]).at(Stage::SimulationStage)});
            }
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
        std::set<uint32_t> order;
        for (auto index : p.TickOrder)
            if (index >= p.Emitters.size() || !order.insert(index).second)
                throw std::invalid_argument("Invalid VFX product tick order");
        if (order.size() != p.Emitters.size())
            throw std::invalid_argument("Incomplete VFX product tick order");
        for (const auto& e : p.Emitters)
            for (const auto& h : e.Handlers)
                if (h.Source >= p.Emitters.size())
                    throw std::invalid_argument("Invalid VFX product handler source");
        return p;
    }
} // namespace Kans::Vfx
