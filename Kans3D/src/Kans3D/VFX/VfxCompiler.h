#pragma once
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
    };
    struct InterfaceLayout
    {
        uint32_t Instruction = 0, Binding = 6, ValueOffset = 0, ValueCount = 0;
    };
    struct Program
    {
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
    struct CompiledEmitter
    {
        Emitter                  Settings;
        std::map<Stage, Program> Programs;
        std::vector<Attribute>   Attributes;
    };
    struct CompiledSystem
    {
        System                       Settings;
        std::map<Stage, Program>     Programs;
        std::vector<CompiledEmitter> Emitters;
    };
    struct CompileResult
    {
        CompiledSystem          Product;
        std::vector<Diagnostic> Errors;
        explicit                operator bool() const { return Errors.empty(); }
    };
    CompileResult Compile(const System&, const EmitterResolver&, const ModuleResolver&, AssetID asset = {});
    std::string   FormatDiagnostics(const std::vector<Diagnostic>&);
    struct ExecutionContext
    {
        uint32_t  Seed = 1, EmitterIndex = 0, UniqueID = 0;
        glm::mat4 LocalToWorld {1};
    };
    Value Evaluate(const Instruction&, const std::vector<Value>&, const ExecutionContext&);
    void  Execute(const Program&, ParameterMap&, const ExecutionContext& = {});
    // Compiled products contain bytecode, generated source and SPIR-V; no module source is needed at runtime.
    YAML::Node     EncodeProduct(const CompiledSystem&);
    CompiledSystem DecodeProduct(const YAML::Node&);
    void           GenerateGpuProgram(Program&, const std::vector<Attribute>&);
} // namespace Kans::Vfx
