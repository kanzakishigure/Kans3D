#include "kspch.h"
#include "VfxGpu.h"
#include <cstring>
#include "Kans3D/Renderer/ComputeUtils.h"
namespace Kans::Vfx
{
    using namespace RenderHardware;
    namespace
    {
        void
        Upload(Device& device, CommandList& commands, const Handle<Buffer>& destination, const void* data, size_t bytes)
        {
            auto staging = device.CreateBuffer({bytes, BufferUsage::CopySource, MemoryUsage::Upload});
            staging->Write(0, data, bytes);
            commands.CopyBuffer(staging, 0, destination, 0, bytes);
        }
        Handle<BindGroup> Group(Device& d, const Handle<BindGroupLayout>& layout, std::vector<Handle<Buffer>> buffers)
        {
            BindGroupDesc g;
            g.Layout = layout;
            for (uint32_t k = 0; k < buffers.size(); ++k)
                g.Entries.push_back({k, buffers[k], 0, buffers[k]->GetDesc().Bytes});
            return d.CreateBindGroup(g);
        }
        struct Push
        {
            uint32_t Capacity, Seed, Emitter, Requested, WorldSpace, Kill;
        };
    } // namespace
    glm::uvec4 Pack(const Value& v)
    {
        glm::uvec4 x(0);
        if (v.Kind == Type::Int || v.Kind == Type::Bool)
            x.x = uint32_t(v.Integer);
        else
            std::memcpy(&x, &v.Data, 16);
        return x;
    }
    Value Unpack(glm::uvec4 x, Type t)
    {
        Value v;
        v.Kind = t;
        if (t == Type::Int || t == Type::Bool)
            v.Integer = int32_t(x.x);
        else
            std::memcpy(&v.Data, &x, 16);
        return v;
    }
    GpuEmitter::Kernel GpuEmitter::Make(const std::string&                       source,
                                        const std::vector<BindGroupLayoutEntry>& bindings,
                                        uint32_t                                 push,
                                        const std::vector<uint32_t>*             spirv)
    {
        Handle<Shader> shader;
        if (spirv)
        {
            ShaderData data;
            data.Code.Modules.push_back({ShaderProductStage::Compute, "main", *spirv});
            shader = Device.CreateShader(data);
        }
        else
        {
            const auto& data = GetInternalComputeShaderData("VFX", source);
            if (auto e = std::get_if<AssetError>(&data))
                throw std::invalid_argument(e->Message);
            shader = Device.CreateShader(std::get<ShaderData>(data));
        }
        auto layout = Device.CreateBindGroupLayout({0, bindings});
        return {Device.CreateComputePipeline({shader, {layout}, push}), layout};
    }
    GpuEmitter::GpuEmitter(RenderHardware::Device& device, const CompiledEmitter& compiled) :
        Device(device), Compiled(compiled)
    {
        auto buffer = [&](size_t bytes, BufferUsage extra = BufferUsage(0)) {
            return Device.CreateBuffer(
                {bytes,
                 BufferUsage::Storage | BufferUsage::CopySource | BufferUsage::CopyDestination | extra,
                 MemoryUsage::DeviceLocal});
        };
        const auto cap = compiled.Settings.Capacity;
        Attributes     = buffer(size_t(cap) * compiled.Attributes.size() * 16);
        Alive          = buffer(cap * 4);
        Free           = buffer(cap * 4);
        Flags          = buffer(cap * 4);
        Counters       = buffer(64);
        Count          = buffer(4);
        DrawArgs       = buffer(16, BufferUsage::Indirect);
        DispatchArgs   = buffer(12, BufferUsage::Indirect);
        if (compiled.Settings.Target == SimTarget::CPU)
            return;
        std::vector<BindGroupLayoutEntry> bindings = {{0, BindingType::StorageBuffer},
                                                      {1, BindingType::ReadOnlyStorageBuffer},
                                                      {2, BindingType::StorageBuffer},
                                                      {3, BindingType::StorageBuffer},
                                                      {4, BindingType::StorageBuffer},
                                                      {5, BindingType::ReadOnlyStorageBuffer},
                                                      {6, BindingType::ReadOnlyStorageBuffer}};
        for (auto stage : {Stage::ParticleSpawn, Stage::ParticleUpdate})
        {
            auto& data = InterfaceData[stage];
            for (const auto& table : compiled.Programs.at(stage).Interfaces)
            {
                const auto& values = compiled.Programs.at(stage).Code.at(table.Instruction).Interface.Values;
                data.insert(data.end(), values.begin(), values.end());
            }
            if (data.empty())
                data.push_back(glm::vec4(0));
            InterfaceBuffers[stage] = buffer(data.size() * 16);
        }
        const auto& spawn  = compiled.Programs.at(Stage::ParticleSpawn);
        const auto& update = compiled.Programs.at(Stage::ParticleUpdate);
        Spawn              = Make(spawn.GLSL, bindings, 24, &spawn.SpirV);
        Update             = Make(update.GLSL, bindings, 24, &update.SpirV);
        Prepare            = Make(R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Counters{uint c[];};
layout(push_constant) uniform P{uint requested;}p;
void main(){uint n=min(p.requested,c[0]);uint freeBefore=atomicAdd(c[0],0u-n);c[2]=n;c[4]=freeBefore-n;c[5]=c[3];c[3]+=n;c[6]+=p.requested-n;c[7]+=n;}
)",
                                  {{0, BindingType::StorageBuffer}},
                                  4);
        auto program       = CreateBuiltinComputeShader(device, "CompactAlive");
        auto layout        = device.CreateBindGroupLayout({0,
                                                           {{0, BindingType::ReadOnlyStorageBuffer},
                                                            {1, BindingType::StorageBuffer},
                                                            {2, BindingType::StorageBuffer},
                                                            {3, BindingType::StorageBuffer},
                                                            {4, BindingType::StorageBuffer}}});
        Compact            = {device.CreateComputePipeline({program, {layout}, 4}), layout};
        auto source        = std::string(R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Counters{uint c[];};
layout(set=0,binding=1,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=2,std430) readonly buffer Alive{uint alive[];};
layout(push_constant) uniform P{uint capacity;}p;
void main(){vec3 lo=vec3(3.402823e38),hi=-lo;float radius=0.0;
)");
        for (const auto& attribute : compiled.Attributes)
            if (attribute.Kind == Type::Position)
                source += "for(uint i=0;i<c[1];++i){vec3 pos=uintBitsToFloat(a[" + std::to_string(attribute.Offset) +
                          "u*p.capacity+alive[i]]).xyz;lo=min(lo,pos);hi=max(hi,pos);}";
        for (const auto& renderer : compiled.Settings.Renderers)
            if (renderer.Kind == RendererKind::Sprite)
            {
                auto at =
                    std::find_if(compiled.Attributes.begin(), compiled.Attributes.end(), [&](const auto& attribute) {
                        return attribute.Name == renderer.Bindings.at("SpriteSize");
                    });
                if (at == compiled.Attributes.end())
                    continue;
                auto pivot = glm::max(glm::abs(renderer.Pivot), glm::abs(glm::vec2(1) - renderer.Pivot));
                source += "for(uint i=0;i<c[1];++i){vec2 size=uintBitsToFloat(a[" + std::to_string(at->Offset) +
                          "u*p.capacity+alive[i]]).xy;radius=max(radius,length(size*vec2(" + std::to_string(pivot.x) +
                          "," + std::to_string(pivot.y) + ")));}";
            }
        source += "for(uint "
                  "k=0;k<3;++k){c[8+k]=floatBitsToUint(lo[k]);c[11+k]=floatBitsToUint(hi[k]);}c[14]=floatBitsToUint("
                  "radius);}";
        BoundsKernel = Make(source,
                            {{0, BindingType::StorageBuffer},
                             {1, BindingType::ReadOnlyStorageBuffer},
                             {2, BindingType::ReadOnlyStorageBuffer}},
                            4);
        for (auto& r : Readbacks)
            r = device.CreateBuffer({64, BufferUsage::CopyDestination, MemoryUsage::Readback});
    }
    void GpuEmitter::AddPasses(RenderGraph& graph, EmitterInstance& emitter)
    {
        const auto cap  = Compiled.Settings.Capacity;
        auto attributes = graph.ImportBuffer("VFX attributes", Attributes),
             alive = graph.ImportBuffer("VFX alive", Alive), free = graph.ImportBuffer("VFX free", Free),
             flags = graph.ImportBuffer("VFX flags", Flags), counters = graph.ImportBuffer("VFX counters", Counters),
             count    = graph.ImportBuffer("VFX compact count", Count),
             draw     = graph.ImportBuffer("VFX draw indirect", DrawArgs),
             dispatch = graph.ImportBuffer("VFX dispatch indirect", DispatchArgs);
        if (!Initialized)
        {
            Initialized             = true;
            auto                  f = Free, fl = Flags, c = Counters;
            std::vector<GraphUse> initialization = {{free, GraphAccess::TransferWrite},
                                                    {flags, GraphAccess::TransferWrite},
                                                    {counters, GraphAccess::TransferWrite}};
            for (const auto& [stage, buffer] : InterfaceBuffers)
                initialization.push_back(
                    {graph.ImportBuffer("VFX data interface table", buffer), GraphAccess::TransferWrite});
            graph.AddPass(
                "VFX initialize",
                GraphPassKind::Transfer,
                std::move(initialization),
                [f, fl, c, cap, tables = InterfaceData, buffers = InterfaceBuffers](auto& ctx) {
                    for (const auto& [stage, data] : tables)
                        Upload(ctx.Device, ctx.Commands, buffers.at(stage), data.data(), data.size() * 16);
                    std::vector<uint32_t> indices(cap);
                    for (uint32_t k = 0; k < cap; ++k)
                        indices[k] = cap - 1 - k;
                    Upload(ctx.Device, ctx.Commands, f, indices.data(), indices.size() * 4);
                    std::array<uint32_t, 16> values {};
                    values[0] = cap;
                    Upload(ctx.Device, ctx.Commands, c, values.data(), 64);
                    ctx.Commands.FillBuffer(fl, 0, cap * 4, 0);
                },
                true);
        }
        auto compact = [&]() {
            auto kernel = Compact;
            auto f = Flags, a = Alive, c = Count, d = DrawArgs, di = DispatchArgs, ct = Counters;
            graph.AddPass(
                "VFX compact alive",
                GraphPassKind::Compute,
                {{flags, GraphAccess::StorageRead},
                 {alive, GraphAccess::StorageWrite},
                 {count, GraphAccess::StorageReadWrite},
                 {draw, GraphAccess::StorageReadWrite},
                 {dispatch, GraphAccess::StorageReadWrite},
                 {counters, GraphAccess::TransferWrite}},
                [kernel, f, a, c, d, di, ct, cap](auto& ctx) {
                    ctx.Commands.FillBuffer(c, 0, 4, 0);
                    ctx.Commands.FillBuffer(d, 0, 16, 0);
                    ctx.Commands.FillBuffer(di, 0, 12, 0);
                    ctx.Commands.BindComputePipeline(kernel.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, kernel.Layout, {f, a, c, d, di}));
                    ctx.Commands.PushConstants(0, &cap, 4);
                    ctx.Commands.Dispatch((cap + 63) / 64, 1, 1);
                    ctx.Commands.CopyBuffer(c, 0, ct, 4, 4);
                },
                true);
        };
        for (const auto& tick : emitter.Pending)
        {
            auto prepare   = Prepare;
            auto ct        = Counters;
            auto requested = tick.SpawnCount;
            graph.AddPass(
                "VFX reserve spawn",
                GraphPassKind::Compute,
                {{counters, GraphAccess::StorageReadWrite}},
                [prepare, ct, requested](auto& ctx) {
                    ctx.Commands.BindComputePipeline(prepare.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, prepare.Layout, {ct}));
                    ctx.Commands.PushConstants(0, &requested, 4);
                    ctx.Commands.Dispatch(1, 1, 1);
                },
                true);
            auto stage = [&](Stage which, const Kernel& kernel) {
                const auto&             program = Compiled.Programs.at(which);
                std::vector<glm::uvec4> words;
                for (const auto& p : program.Parameters)
                {
                    auto at = tick.Parameters.find(p.Name);
                    words.push_back(
                        Pack(at == tick.Parameters.end() ? p.Default.value_or(Value::Vector({}, p.Kind)) : at->second));
                }
                for (int k = 0; k < 4; ++k)
                {
                    glm::uvec4 v;
                    std::memcpy(&v, &tick.Context.LocalToWorld[k], 16);
                    words.push_back(v);
                }
                auto params = Device.CreateBuffer(
                    {words.size() * 16, BufferUsage::Storage | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
                auto                  gp = graph.ImportBuffer("VFX stage parameters", params);
                auto                  a = Attributes, f = Flags, fr = Free, c = Counters, al = Alive, di = DispatchArgs;
                Push                  push {cap,
                                            tick.Context.Seed,
                                            tick.Context.EmitterIndex,
                                            requested,
                                            uint32_t(Compiled.Settings.SimulationSpace == Space::World),
                                            uint32_t(tick.Parameters.at("Emitter.ExecutionState").Integer == 3 ||
                                                     tick.Parameters.at("System.ExecutionState").Integer == 3)};
                auto                  table = InterfaceBuffers.at(which);
                bool                  spawn = which == Stage::ParticleSpawn;
                std::vector<GraphUse> uses  = {{attributes, GraphAccess::StorageReadWrite},
                                               {flags, GraphAccess::StorageReadWrite},
                                               {free, GraphAccess::StorageReadWrite},
                                               {counters, GraphAccess::StorageReadWrite},
                                               {alive, GraphAccess::StorageRead},
                                               {gp, GraphAccess::TransferWrite}};
                uses.push_back({graph.ImportBuffer("VFX data interface table", table), GraphAccess::StorageRead});
                if (!spawn)
                    uses.push_back({dispatch, GraphAccess::IndirectRead});
                graph.AddPass(
                    spawn ? "VFX particle spawn" : "VFX particle update",
                    GraphPassKind::Compute,
                    std::move(uses),
                    [kernel, params, words = std::move(words), a, f, fr, c, al, di, push, spawn, cap, table](
                        auto& ctx) {
                        Upload(ctx.Device, ctx.Commands, params, words.data(), words.size() * 16);
                        ctx.Commands.BindComputePipeline(kernel.Pipeline);
                        ctx.Commands.BindGroup(Group(ctx.Device, kernel.Layout, {a, params, f, fr, c, al, table}));
                        ctx.Commands.PushConstants(0, &push, sizeof(push));
                        if (spawn)
                            ctx.Commands.Dispatch(std::max(1u, (std::min(push.Requested, cap) + 63) / 64), 1, 1);
                        else
                            ctx.Commands.DispatchIndirect(di);
                    },
                    true);
            };
            stage(Stage::ParticleSpawn, Spawn);
            compact();
            stage(Stage::ParticleUpdate, Update);
            compact();
        }
        bool worked = !emitter.Pending.empty();
        emitter.Pending.clear();
        if (worked)
        {
            auto k = BoundsKernel;
            auto c = Counters, a = Attributes, al = Alive;
            graph.AddPass(
                "VFX dynamic bounds",
                GraphPassKind::Compute,
                {{counters, GraphAccess::StorageReadWrite},
                 {attributes, GraphAccess::StorageRead},
                 {alive, GraphAccess::StorageRead}},
                [k, c, a, al, cap](auto& ctx) {
                    ctx.Commands.BindComputePipeline(k.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, k.Layout, {c, a, al}));
                    ctx.Commands.PushConstants(0, &cap, 4);
                    ctx.Commands.Dispatch(1, 1, 1);
                },
                true);
        }
        if (worked && !Pending[ReadbackCursor])
        {
            auto target = Readbacks[ReadbackCursor], source = Counters;
            auto output = graph.ImportBuffer("VFX async readback", target);
            graph.AddPass(
                "VFX readback",
                GraphPassKind::Transfer,
                {{counters, GraphAccess::TransferRead}, {output, GraphAccess::TransferWrite}},
                [target, source](auto& ctx) { ctx.Commands.CopyBuffer(source, 0, target, 0, 64); },
                true);
            Pending[ReadbackCursor]                = true;
            ReadbackSequence[ReadbackCursor]       = ++SubmittedSequence;
            ReadbackSpawnRevisions[ReadbackCursor] = emitter.SpawnRevision;
            ReadbackCursor                         = (ReadbackCursor + 1) % 3;
        }
    }
    void GpuEmitter::UploadCpu(RenderGraph& graph, const EmitterInstance& e)
    {
        std::vector<glm::uvec4> data;
        data.reserve(Compiled.Attributes.size() * Compiled.Settings.Capacity);
        for (const auto& a : e.Attributes)
            for (const auto& v : a)
                data.push_back(Pack(v));
        auto                    a = Attributes, al = Alive, d = DrawArgs;
        auto                    indices = e.Alive;
        std::array<uint32_t, 4> args {6, uint32_t(indices.size()), 0, 0};
        graph.AddPass(
            "VFX CPU sprite upload",
            GraphPassKind::Transfer,
            {{graph.ImportBuffer("VFX attributes", a), GraphAccess::TransferWrite},
             {graph.ImportBuffer("VFX alive", al), GraphAccess::TransferWrite},
             {graph.ImportBuffer("VFX draw indirect", d), GraphAccess::TransferWrite}},
            [a, al, d, data = std::move(data), indices = std::move(indices), args](auto& c) {
                Upload(c.Device, c.Commands, a, data.data(), data.size() * 16);
                if (!indices.empty())
                    Upload(c.Device, c.Commands, al, indices.data(), indices.size() * 4);
                Upload(c.Device, c.Commands, d, args.data(), 16);
            },
            true);
    }
    void GpuEmitter::Poll(EmitterInstance& emitter)
    {
        for (uint32_t n = 0; n < 3; ++n)
        {
            uint32_t k = (ReadbackCursor + n) % 3;
            if (Pending[k] && Readbacks[k]->IsReadbackReady())
            {
                if (ReadbackSequence[k] < LatestSequence)
                {
                    Pending[k] = false;
                    continue;
                }
                LatestSequence                 = ReadbackSequence[k];
                auto                     bytes = Readbacks[k]->Read(0, 64);
                std::array<uint32_t, 16> v;
                std::memcpy(v.data(), bytes.data(), 64);
                emitter.ParticleCount         = v[1];
                emitter.HasReadback           = true;
                emitter.ReadbackSpawnRevision = ReadbackSpawnRevisions[k];
                emitter.Dropped               = v[6];
                emitter.TotalSpawned          = v[7];
                std::memcpy(&emitter.SpriteRadius, &v[14], 4);
                for (int j = 0; j < 3; ++j)
                {
                    std::memcpy(&emitter.DynamicBounds.Min[j], &v[8 + j], 4);
                    std::memcpy(&emitter.DynamicBounds.Max[j], &v[11 + j], 4);
                }
                Pending[k] = false;
            }
        }
    }
} // namespace Kans::Vfx
