#pragma once
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "VfxAsset.h"
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
        std::string Kind, Channel, SourceHandle;
        uint32_t    Source = 0, Binding = 7, Capacity = 0, IDOffset = 0;
        Space       PositionSpace = Space::Local;
        EventLayout Layout;
    };
    struct Program
    {
        std::vector<ResourceBinding> Resources;
        bool                         Handler     = false;
        EventMode                    Mode        = EventMode::SpawnedParticles;
        uint32_t                     SpawnNumber = 1, MaxEventsPerFrame = UINT32_MAX;
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
    struct CompiledEmitter
    {
        Emitter                            Settings;
        std::map<std::string, float>       MeshRadii;
        std::string                        Handle;
        std::vector<CompiledHandler>       Handlers;
        std::map<std::string, EventLayout> EventLayouts;
        std::map<Stage, Program>           Programs;
        std::vector<Attribute>             Attributes;
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
    using MeshResolver = std::function<MeshData(AssetID)>;
    CompileResult Compile(const System&,
                          const EmitterResolver&,
                          const ModuleResolver&,
                          AssetID             asset  = {},
                          const MeshResolver& meshes = {});
    std::string   FormatDiagnostics(const std::vector<Diagnostic>&);
    struct ExecutionContext
    {
        uint32_t                                                            Seed = 1, EmitterIndex = 0, UniqueID = 0;
        glm::mat4                                                           LocalToWorld {1};
        uint32_t                                                            EventIndex = 0;
        std::optional<uint32_t>                                             RandomID;
        std::function<Value(const Instruction&, const std::vector<Value>&)> ReadParticle;
        std::function<void(const Instruction&, const std::vector<Value>&)>  WriteEvent;
    };
    Value Evaluate(const Instruction&, const std::vector<Value>&, const ExecutionContext&);
    void  Execute(const Program&, ParameterMap&, const ExecutionContext& = {});
    // Compiled products contain bytecode, generated source and SPIR-V; no module source is needed at runtime.
    YAML::Node     EncodeProduct(const CompiledSystem&);
    CompiledSystem DecodeProduct(const YAML::Node&);
    void           GenerateGpuProgram(Program&, const std::vector<Attribute>&);
} // namespace Kans::Vfx
