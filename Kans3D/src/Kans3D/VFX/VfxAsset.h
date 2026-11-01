#pragma once
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Core/Math/Bounds.h"

namespace Kans::Vfx
{
    enum class Type
    {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Int,
        Bool,
        Color,
        Quat,
        Position
    };
    enum class Stage
    {
        SystemSpawn,
        SystemUpdate,
        EmitterSpawn,
        EmitterUpdate,
        ParticleSpawn,
        ParticleUpdate,
        EventHandler,
        SimulationStage
    };
    enum class Usage
    {
        Module,
        Function,
        DynamicInput
    };
    enum class SimTarget
    {
        CPU,
        GPU
    };
    enum class Space
    {
        Local,
        World
    };
    enum class RendererKind
    {
        Sprite,
        Mesh,
        Ribbon,
        Light
    };
    enum class Facing
    {
        ViewPlane,
        CameraPosition,
        Velocity,
        Custom
    };
    enum class Sort
    {
        None,
        EmitterOrder,
        ParticleDepth,
        ParticleDistance
    };
    enum class CullPolicy
    {
        PauseSpawning,
        Continue
    };
    enum class InputKind
    {
        Literal,
        Link,
        DynamicInput,
        DataInterface
    };
    struct Value
    {
        Type         Kind = Type::Float;
        glm::vec4    Data {0};
        int32_t      Integer = 0;
        static Value Float(float x) { return {Type::Float, glm::vec4(x), 0}; }
        static Value Int(int32_t x) { return {Type::Int, {}, x}; }
        static Value Bool(bool x) { return {Type::Bool, {}, int32_t(x)}; }
        static Value Vector(glm::vec4 x, Type type = Type::Vec4) { return {type, x, 0}; }
        bool operator==(const Value& b) const { return Kind == b.Kind && Data == b.Data && Integer == b.Integer; }
        bool operator!=(const Value& b) const { return !(*this == b); }
    };
    using ParameterMap = std::map<std::string, Value>;
    struct Parameter
    {
        std::string              Name;
        Type                     Kind = Type::Float;
        std::optional<Value>     Default;
        std::optional<glm::vec2> Range;
    };
    struct InterfaceBinding
    {
        std::string Kind = "Curve";
        AssetID     Asset;
        // Baked sample table: x is time, values are linearly interpolated. Texture2D uses row-major texels.
        std::vector<float>     Times;
        std::vector<glm::vec4> Values;
        uint32_t               Width = 0, Height = 0;
    };
    struct Input
    {
        InputKind                    Kind = InputKind::Literal;
        Value                        Literal;
        std::string                  Link;
        AssetID                      Script;
        std::map<std::string, Input> Inputs;
        InterfaceBinding             Interface;
    };
    struct ModuleUse
    {
        std::string                  Guid;
        AssetID                      Script;
        bool                         Enabled = true;
        std::map<std::string, Input> Inputs;
    };
    using Stack = std::vector<ModuleUse>;
    struct Pin
    {
        std::string          Name;
        Type                 Kind = Type::Float;
        std::optional<Value> Default;
    };
    struct Node
    {
        std::string      Guid, Op, Parameter, Code;
        Type             Kind = Type::Float;
        Value            Constant;
        AssetID          Function;
        std::vector<Pin> Inputs;
        std::vector<Pin> Outputs;
        InterfaceBinding Interface;
    };
    struct Link
    {
        std::string From, Output = "Value", To, Input = "Value";
    };
    struct Module
    {
        AssetID                Identity;
        Usage                  ScriptUsage   = Usage::Module;
        uint32_t               AllowedStages = 0x3f;
        std::string            Category, Description;
        std::vector<Parameter> Inputs;
        std::vector<Node>      Nodes;
        std::vector<Link>      Links;
        // Function/dynamic-input return node and output pin; modules use parameter-map Set nodes.
        std::string ResultNode, ResultPin = "Value";
    };
    struct Renderer
    {
        std::string                        Guid;
        RendererKind                       Kind = RendererKind::Sprite;
        AssetID                            Material;
        Facing                             FacingMode = Facing::ViewPlane;
        Sort                               Sorting    = Sort::EmitterOrder;
        glm::vec2                          Pivot {0.5f};
        glm::vec3                          FacingVector {0, 0, 1}, AlignmentVector {0, 1, 0};
        uint32_t                           Rows = 1, Columns = 1;
        bool                               FrameBlending     = false;
        float                              DepthFadeDistance = 0;
        std::map<std::string, std::string> Bindings {
            {"Position", "Particles.Position"},
            {"Color", "Particles.Color"},
            {"SpriteSize", "Particles.SpriteSize"},
            {"SpriteRotation", "Particles.SpriteRotation"},
            {"Velocity", "Particles.Velocity"},
            {"SubImageIndex", "Particles.SubImageIndex"},
            {"DynamicMaterialParameter", "Particles.DynamicMaterialParameter"}};
    };
    struct Override
    {
        enum class Kind
        {
            AddModule,
            RemoveModule,
            MoveModule,
            SetInput,
            SetEnabled,
            AddRenderer,
            RemoveRenderer,
            ReplaceRenderer
        };
        Kind        Action    = Kind::SetInput;
        Stage       StageKind = Stage::ParticleUpdate;
        std::string Target, Before, InputName;
        ModuleUse   Added;
        Input       InputValue;
        bool        Enabled = true;
        Renderer    Render;
    };
    struct Emitter
    {
        std::string            Name;
        AssetID                Parent;
        SimTarget              Target          = SimTarget::GPU;
        Space                  SimulationSpace = Space::Local;
        uint32_t               Capacity        = 1024;
        bool                   Deterministic   = true;
        std::optional<Bounds>  FixedBounds;
        float                  CullDistance   = 0;
        CullPolicy             DistancePolicy = CullPolicy::PauseSpawning;
        std::map<Stage, Stack> Stages;
        std::vector<Renderer>  Renderers;
        std::vector<Parameter> Attributes;
        std::vector<Override>  Overrides;
        // A child stores only settings explicitly named here, plus GUID-keyed Overrides.
        std::set<std::string> SettingOverrides;
    };
    struct EmitterHandle
    {
        std::string            Guid, Name;
        bool                   Enabled = true;
        AssetID                Reference;
        std::optional<Emitter> Inline;
        std::vector<Override>  Overrides;
    };
    struct System
    {
        std::map<Stage, Stack>     Stages;
        std::vector<EmitterHandle> Emitters;
        std::vector<Parameter>     UserParameters;
        std::optional<Bounds>      FixedBounds;
        float                      WarmupTime = 0, WarmupTickDelta = 1.f / 30, MaxDeltaTime = .1f;
        uint32_t                   Seed = 1;
    };
    class InterfaceRegistry
    {
    public:
        struct Entry
        {
            Type Result;
            bool Implemented;
        };
        InterfaceRegistry();
        void         Register(std::string name, Entry);
        const Entry* Find(const std::string&) const;

    private:
        std::map<std::string, Entry> m_Entries;
    };
    using EmitterResolver = std::function<Emitter(AssetID)>;
    using ModuleResolver  = std::function<Module(AssetID)>;
    Emitter                          MergeEmitter(const Emitter&, const EmitterResolver&);
    void                             ApplyOverrides(Emitter&, const std::vector<Override>&);
    std::map<std::string, Parameter> BuiltinParameters();
    uint32_t                         Hash(uint32_t);
    uint32_t                         StableHash(const std::string&);
    uint32_t                         Components(Type);
    const char*                      TypeName(Type);
    const char*                      StageName(Stage);
    YAML::Node                       EncodeValue(const Value&);
    Value                            DecodeValue(const YAML::Node&);
    YAML::Node                       EncodeModule(const Module&);
    Module                           DecodeModule(const YAML::Node&);
    YAML::Node                       EncodeEmitter(const Emitter&);
    Emitter                          DecodeEmitter(const YAML::Node&);
    YAML::Node                       EncodeSystem(const System&);
    System                           DecodeSystem(const YAML::Node&);
    YAML::Node                       EncodeInterface(const InterfaceBinding&);
    InterfaceBinding                 DecodeInterface(const YAML::Node&);
    YAML::Node                       EncodeParameters(const ParameterMap&);
    ParameterMap                     DecodeParameters(const YAML::Node&);
    // Stable IDs are declared by each source asset. The root is used only by importers/tools.
    AssetID                                  BuiltinModuleID(const std::string& name);
    std::filesystem::path                    BuiltinModuleRoot();
    void                                     SetBuiltinModuleRoot(std::filesystem::path);
    std::map<AssetID, std::filesystem::path> FindBuiltinModules();
} // namespace Kans::Vfx
