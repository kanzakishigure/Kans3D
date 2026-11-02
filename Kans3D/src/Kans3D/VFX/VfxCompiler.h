#pragma once
#include "Kans3D/Animation/AnimationData.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Physics/ForceFields.h"
#include "VfxAsset.h"
namespace Kans
{
    class PhysicsWorld;
}
namespace Kans::Vfx
{
    struct Diagnostic
    {
        AssetID     Asset;
        std::string Node, Pin, Message;
    };
    struct Instruction
    {
        std::string           Op;
        Type                  Kind = Type::Float;
        std::vector<uint32_t> Args;
        Value                 Literal;
        Space                 PositionSpace = Space::Local;
        uint32_t              Parameter = 0, RandomKey = 0;
        InterfaceBinding      Interface;
        std::string           Code, Output, CustomDeclarations;
        uint32_t              Resource = 0;
    };
    struct InterfaceLayout
    {
        uint32_t Instruction = 0, Binding = 6, ValueOffset = 0, ValueCount = 0;
    };
    struct ResourceBinding
    {
        std::string      Kind, Channel, SourceHandle;
        uint32_t         Source = 0, Binding = 7, Capacity = 0, IDOffset = 0;
        Space            PositionSpace = Space::Local;
        EventLayout      Layout;
        InterfaceBinding Grid;
    };
    struct Program
    {
        std::vector<ResourceBinding> Resources;
        bool                         Handler     = false;
        EventMode                    Mode        = EventMode::SpawnedParticles;
        uint32_t                     SpawnNumber = 1, MaxEventsPerFrame = UINT32_MAX;
        std::string                  IterationGrid;
        Stage                        StageKind = Stage::ParticleUpdate;
        SimTarget                    Target    = SimTarget::CPU;
        std::vector<Parameter>       Parameters;
        std::vector<Instruction>     Code;
        std::vector<InterfaceLayout> Interfaces;
        std::set<std::string>        Reads, Writes;
        std::string                  GLSL;
        std::vector<uint32_t>        SpirV;
    };
    struct Attribute
    {
        std::string Name;
        Type        Kind;
        Value       Default;
        uint32_t    Offset = 0;
    };
    struct CompiledHandler
    {
        EventHandler Settings;
        Program      Bytecode;
        uint32_t     Source = 0;
    };
    struct CompiledSimulationStage
    {
        SimulationStage Settings;
        Program         Bytecode;
    };
    struct CompiledEmitter
    {
        Emitter                              Settings;
        std::map<std::string, float>         MeshRadii;
        std::string                          Handle;
        std::vector<CompiledHandler>         Handlers;
        std::vector<CompiledSimulationStage> SimulationStages;
        std::map<std::string, EventLayout>   EventLayouts;
        std::map<Stage, Program>             Programs;
        std::vector<Attribute>               Attributes;
    };
    struct CompiledSystem
    {
        System                       Settings;
        std::vector<uint32_t>        TickOrder;
        std::map<Stage, Program>     Programs;
        std::vector<CompiledEmitter> Emitters;
    };
    struct CompileResult
    {
        CompiledSystem          Product;
        std::vector<Diagnostic> Errors;
        explicit                operator bool() const { return Errors.empty(); }
    };
    using SkeletonResolver = std::function<SkeletonData(AssetID)>;
    using MeshResolver     = std::function<MeshData(AssetID)>;
    CompileResult Compile(const System&,
                          const EmitterResolver&,
                          const ModuleResolver&,
                          AssetID                 asset     = {},
                          const MeshResolver&     meshes    = {},
                          const SkeletonResolver& skeletons = {});
    std::string   FormatDiagnostics(const std::vector<Diagnostic>&);
    struct MeshSamplingSnapshot
    {
        std::shared_ptr<const MeshSamplingGeometry> Geometry;
        std::vector<glm::vec4>                      Frame;
    };
    struct FieldGpuStorage;
    struct FieldSnapshot
    {
        ForceFieldSet                    Fields;
        std::shared_ptr<FieldGpuStorage> GPU;
    };
    bool     UsesForceFields(const Program&);
    uint32_t ForceFieldsGpuBinding(const Program&);
    enum class CollisionMode : int32_t
    {
        SceneQueries,
        DepthBuffer,
        AnalyticShapes
    };
    enum class ColliderKind : uint32_t
    {
        Plane,
        Sphere,
        Box,
        Capsule
    };
    // Seven vec4 words, shared with GLSL. Axis w components hold box half extents;
    // a capsule uses AxisY for its axis and half segment length.
    struct CollisionShape
    {
        glm::vec4  CenterRadius {0};
        glm::vec4  AxisX {1, 0, 0, .5f}, AxisY {0, 1, 0, .5f}, AxisZ {0, 0, 1, .5f};
        glm::vec4  Velocity {0}; // Linear velocity at the shape centre.
        glm::vec4  AngularVelocity {0};
        glm::uvec4 Meta {uint32_t(ColliderKind::Box), 0, 0, 0};
    };
    struct CollisionGpuStorage;
    struct CollisionDepthSnapshot;
    struct CollisionSnapshot
    {
        static constexpr uint32_t            Capacity = 256;
        std::vector<CollisionShape>          Shapes;
        std::shared_ptr<CollisionGpuStorage> GPU;
    };
    struct CollisionResult
    {
        glm::vec3 Position {0}, Velocity {0}, Normal {0}, Contact {0};
        bool      Hit = false;
    };
    bool     UsesCollision(const Program&);
    bool     UsesDepthCollision(const Program&);
    uint32_t CollisionGpuBinding(const Program&);
    struct ExecutionContext
    {
        uint32_t                                                            Seed = 1, EmitterIndex = 0, UniqueID = 0;
        glm::mat4                                                           LocalToWorld {1};
        uint32_t                                                            EventIndex = 0;
        std::optional<uint32_t>                                             RandomID;
        std::function<Value(const Instruction&, const std::vector<Value>&)> ReadParticle;
        std::function<void(const Instruction&, const std::vector<Value>&)>  WriteEvent;
        uint32_t                                                            SpawnIndex = 0;
        std::shared_ptr<FieldSnapshot>                                      Fields;
        std::shared_ptr<CollisionSnapshot>                                  Colliders;
        std::shared_ptr<CollisionDepthSnapshot>                             Depth;
        PhysicsWorld*                                                       Physics       = nullptr;
        uint32_t                                                            CollisionMask = UINT32_MAX;
        double                                                              FieldTime     = 0;
        uint32_t                                                            FieldLayer    = 0;
        uint64_t                                                            FirstImpulse  = 0;
        std::function<std::shared_ptr<const MeshSamplingSnapshot>(const InterfaceBinding&)> MeshTable;
    };
    // Mesh sampling tables share the CPU/GPU vec4 layout. CDFs use bind-pose area,
    // Niagara's default approximation; morph targets are intentionally excluded.
    bool IsMeshOperation(const std::string&);
    InterfaceBinding
         CachedMeshSampling(const InterfaceBinding&, const Ref<const MeshAsset>&, const Ref<const SkeletonAsset>&);
    void BakeMeshSampling(InterfaceBinding&, const MeshData&, const SkeletonData& = {});
    MeshSamplingSnapshot   MeshSamplingFrame(const InterfaceBinding&,
                                             const glm::mat4&              source,
                                             const glm::mat4&              previousSource,
                                             const glm::mat4&              owner,
                                             float                         delta,
                                             const std::vector<glm::mat4>& palette         = {},
                                             const std::vector<glm::mat4>& previousPalette = {},
                                             bool                          valid           = true);
    std::vector<glm::vec4> GpuInterfaceTable(const Program&, const ExecutionContext&);
    std::vector<std::pair<const InterfaceBinding*, uint32_t>> MeshGpuBindings(const Program&);
    std::shared_ptr<const MeshSamplingGeometry>               InternMeshGeometry(std::vector<glm::vec4>);
    std::string                                               MeshBindingKey(const InterfaceBinding&);
    glm::vec3 CurlNoise(glm::vec3 position, float frequency, glm::vec3 offset, int octaves);
    glm::vec3 CurlSeedOffset(uint32_t seed, uint32_t emitter = 0);
    Value     Evaluate(const Instruction&, const std::vector<Value>&, const ExecutionContext&);
    void      Execute(const Program&, ParameterMap&, const ExecutionContext& = {});
    // Compiled products contain bytecode, generated source and SPIR-V; no module source is needed at runtime.
    YAML::Node     EncodeProduct(const CompiledSystem&);
    CompiledSystem DecodeProduct(const YAML::Node&);
    void           GenerateGpuProgram(Program&, const std::vector<Attribute>&);
} // namespace Kans::Vfx
