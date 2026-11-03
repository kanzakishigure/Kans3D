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

namespace Kans::RenderHardware
{
    class Device;
    class Buffer;
} // namespace Kans::RenderHardware

namespace Kans::Vfx
{
    struct MeshSamplingGeometry
    {
        std::vector<glm::vec4> Values;
        // GPU emitters retain active buffers; this index never extends device lifetime.
        mutable std::map<const RenderHardware::Device*, std::weak_ptr<RenderHardware::Buffer>> Buffers;
    };
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
        Position,
        IVec3
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
        Custom,
        Default,
        CameraPlane
    };
    enum class Sort
    {
        None,
        EmitterOrder,
        ParticleDepth,
        ParticleDistance,
        Custom
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
        // Display names of an int input's values 0..N-1 (editor and stack UI only).
        std::vector<std::string> EnumNames;
    };
    struct GridAttribute
    {
        std::string Name;
        Type        Kind = Type::Float;
    };
    struct InterfaceBinding
    {
        std::string                Kind = "Curve";
        glm::uvec3                 Resolution {1};
        glm::vec3                  WorldBounds {1};
        std::vector<GridAttribute> GridAttributes;
        std::array<bool, 3>        Tiling {false, false, false}; // true repeats; false clamps to border.
        float                      Intensity = 1;
        AssetID                    Asset;
        std::string                SourceEmitter, Channel;
        // Mesh sources use an asset, the owner, or a child path relative to the owner.
        std::string              Source, EntityPath;
        AssetID                  Skeleton;
        std::vector<uint32_t>    Sections;
        std::vector<std::string> Bones, Region, SkeletonNames;
        // Baked sample table: x is time, values are linearly interpolated. Texture2D uses row-major texels.
        std::vector<float>                          Times;
        std::vector<glm::vec4>                      Values;
        std::shared_ptr<const MeshSamplingGeometry> Geometry;
        uint32_t                                    Width = 0, Height = 0;
    };
    struct VectorFieldSource
    {
        glm::vec3   Min {0}, Max {1};
        std::string Source = "Bake", File;
        glm::uvec3  Resolution {2};
        float       Frequency = 1, Strength = 1;
        uint32_t    Octaves = 1, Seed = 1;
    };
    YAML::Node        EncodeVectorField(const VectorFieldSource&);
    VectorFieldSource DecodeVectorField(const YAML::Node&);
    // Packed immutable grid: min, max, resolution, then x-fastest vec4 vectors (w=0).
    InterfaceBinding       ImportVectorField(const VectorFieldSource&, const std::filesystem::path&);
    std::vector<std::byte> EncodeVectorFieldProduct(const InterfaceBinding&);
    InterfaceBinding       DecodeVectorFieldProduct(const std::byte*, size_t);
    void                   ValidateVectorField(const InterfaceBinding&);
    glm::vec3              SampleVectorField(const InterfaceBinding&, glm::vec3);
    bool                   IsVectorFieldOperation(const std::string&);
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
        uint32_t         Stages = UINT32_MAX;
        Type             Kind   = Type::Float;
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
    struct EventLayout
    {
        std::string            Name;
        std::vector<Parameter> Fields;
    };
    struct EventChannel
    {
        std::string Name, Layout;
        uint32_t    Capacity = 1024;
    };
    enum class EventMode
    {
        SpawnedParticles,
        EveryParticle
    };
    struct EventHandler
    {
        std::string Guid, SourceEmitter, Channel;
        bool        Enabled     = true;
        EventMode   Mode        = EventMode::SpawnedParticles;
        uint32_t    SpawnNumber = 1, MaxEventsPerFrame = UINT32_MAX;
        Stack       Modules;
    };
    enum class IterationSource
    {
        Particles,
        DataInterface
    };
    struct SimulationStage
    {
        std::string     Guid, Name;
        IterationSource Source = IterationSource::Particles;
        std::string     DataInterface, Enabled; // Emitter parameter bindings. Empty Enabled means always enabled.
        uint32_t        Iterations = 1;
        Stack           Modules;
    };
    // Editor-only graph layout: node positions, collapsed nodes and comment boxes. The compiler and
    // runtime ignore it; an empty layout is not written, and the importer strips it from products.
    struct ModuleComment
    {
        std::string Guid, Title;
        glm::vec2   Position {0}, Size {320, 200};
        glm::vec4   Color {0.35f, 0.38f, 0.45f, 0.35f};
        bool        operator==(const ModuleComment& b) const
        {
            return Guid == b.Guid && Title == b.Title && Position == b.Position && Size == b.Size && Color == b.Color;
        }
    };
    struct ModuleLayout
    {
        std::map<std::string, glm::vec2> Positions; // Node GUID -> canvas position.
        std::set<std::string>            Collapsed;
        std::vector<ModuleComment>       Comments;
        bool Empty() const { return Positions.empty() && Collapsed.empty() && Comments.empty(); }
        bool operator==(const ModuleLayout& b) const
        {
            return Positions == b.Positions && Collapsed == b.Collapsed && Comments == b.Comments;
        }
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
        std::string              ResultNode, ResultPin = "Value";
        std::vector<EventLayout> EventLayouts;
        ModuleLayout             Editor;
    };
    struct MeshEntry
    {
        AssetID                     Mesh;
        glm::vec3                   Scale {1}, PivotOffset {0};
        std::map<uint32_t, AssetID> MaterialOverrides;
    };
    enum class RibbonFacing
    {
        Screen,
        Custom,
        CustomSide
    };
    enum class RibbonUVMode
    {
        ScaledUniformly,
        TiledOverRibbonLength,
        TiledFromStartOverRibbonLength,
        ScaledUsingRibbonSegmentLength
    };
    struct RibbonUVSettings
    {
        RibbonUVMode Mode     = RibbonUVMode::ScaledUniformly;
        float        TileSize = 1, LeadingEdge = 0, TrailingEdge = 0;
        glm::vec2    Offset {0}, Scale {1};
    };
    // An unspecified shadow setting follows the renderer kind; assignments retain explicit opt-ins/outs.
    struct RendererShadows
    {
        std::optional<bool> Value;
        RendererShadows&    operator=(bool value)
        {
            Value = value;
            return *this;
        }
        operator bool() const { return Value.value_or(true); }
    };
    enum class LightingNormal
    {
        CameraFacing,
        Spherical,
        ParticleNormal
    };
    enum class LightingEvaluation
    {
        PerPixel,
        PerVertex
    };
    struct Renderer
    {
        std::string                     Guid;
        RendererKind                    Kind = RendererKind::Sprite;
        AssetID                         Material;
        std::vector<MeshEntry>          Meshes;
        RendererShadows                 CastShadows;
        bool                            SortAscending      = false;
        RibbonFacing                    RibbonFacingMode   = RibbonFacing::Screen;
        uint32_t                        TessellationFactor = 1;
        float                           CurveTension       = 0;
        std::array<RibbonUVSettings, 2> UVChannels;
        bool               CastsShadows() const { return CastShadows.Value.value_or(Kind != RendererKind::Ribbon); }
        bool               LockedAxisEnable = false;
        glm::vec3          LockedAxis {0, 0, 1};
        Facing             FacingMode = Facing::ViewPlane;
        Sort               Sorting    = Sort::EmitterOrder;
        glm::vec2          Pivot {0.5f};
        glm::vec3          FacingVector {0, 0, 1}, AlignmentVector {0, 1, 0};
        uint32_t           Rows = 1, Columns = 1;
        LightingNormal     NormalMode        = LightingNormal::CameraFacing;
        LightingEvaluation Lighting          = LightingEvaluation::PerPixel;
        float              Curvature         = 1;
        bool               ReceiveShadows    = true;
        bool               FrameBlending     = false;
        float              DepthFadeDistance = 0;
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
            ReplaceRenderer,
            AddHandler,
            RemoveHandler,
            MoveHandler,
            AddSimulationStage,
            RemoveSimulationStage,
            MoveSimulationStage,
            ReplaceSimulationStage
        };
        Kind            Action    = Kind::SetInput;
        Stage           StageKind = Stage::ParticleUpdate;
        std::string     Target, Before, InputName;
        ModuleUse       Added;
        Input           InputValue;
        bool            Enabled = true;
        Renderer        Render;
        EventHandler    Handler;
        std::string     HandlerGuid, SimulationStageGuid;
        SimulationStage Simulation;
    };
    struct Emitter
    {
        std::string                             Name;
        AssetID                                 Parent;
        SimTarget                               Target          = SimTarget::GPU;
        Space                                   SimulationSpace = Space::Local;
        uint32_t                                Capacity        = 1024;
        bool                                    Deterministic   = true;
        std::optional<Bounds>                   FixedBounds;
        float                                   CullDistance   = 0;
        CullPolicy                              DistancePolicy = CullPolicy::PauseSpawning;
        std::map<Stage, Stack>                  Stages;
        std::vector<Renderer>                   Renderers;
        std::vector<EventChannel>               Channels;
        std::vector<EventHandler>               Handlers;
        std::vector<SimulationStage>            SimulationStages;
        std::map<std::string, InterfaceBinding> DataInterfaces;
        std::vector<Parameter>                  Parameters;
        std::vector<Parameter>                  Attributes;
        std::vector<Override>                   Overrides;
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
    YAML::Node                       EncodeEventLayout(const EventLayout&);
    EventLayout                      DecodeEventLayout(const YAML::Node&);
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
