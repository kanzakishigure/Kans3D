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
        const auto cap    = compiled.Settings.Capacity;
        Attributes        = buffer(size_t(cap) * compiled.Attributes.size() * 16);
        Alive             = buffer(cap * 4);
        Free              = buffer(cap * 4);
        Flags             = buffer(cap * 4);
        Counters          = buffer(64);
        Count             = buffer(4);
        DrawArgs          = buffer(16, BufferUsage::Indirect);
        DispatchArgs      = buffer(12, BufferUsage::Indirect);
        EventDispatchArgs = buffer(12, BufferUsage::Indirect);
        for (const auto& channel : compiled.Settings.Channels)
        {
            auto stride                = compiled.EventLayouts.at(channel.Layout).Fields.size() + 1;
            EventBuffers[channel.Name] = buffer(16 * (1 + size_t(channel.Capacity) * stride) + 16);
        }
        if (compiled.Settings.Target == SimTarget::CPU)
            return;
        auto makeStage = [&](const Program& program) {
            std::vector<BindGroupLayoutEntry> bindings = {{0, BindingType::StorageBuffer},
                                                          {1, BindingType::ReadOnlyStorageBuffer},
                                                          {2, BindingType::StorageBuffer},
                                                          {3, BindingType::StorageBuffer},
                                                          {4, BindingType::StorageBuffer},
                                                          {5, BindingType::ReadOnlyStorageBuffer},
                                                          {6, BindingType::ReadOnlyStorageBuffer}};
            for (const auto& resource : program.Resources)
            {
                bindings.push_back({resource.Binding,
                                    (resource.Kind == "EventOutput" || resource.Kind == "EmitterState") ?
                                        BindingType::StorageBuffer :
                                        BindingType::ReadOnlyStorageBuffer});
                if (resource.Kind == "ParticleReader")
                {
                    bindings.push_back({resource.Binding + 1, BindingType::ReadOnlyStorageBuffer});
                    bindings.push_back({resource.Binding + 2, BindingType::ReadOnlyStorageBuffer});
                }
                if (resource.Kind == "EmitterState")
                    EmitterState = buffer(std::max(size_t(16), program.Parameters.size() * 16));
            }
            auto& data = InterfaceData[&program];
            for (const auto& table : program.Interfaces)
            {
                const auto& values = program.Code.at(table.Instruction).Interface.Values;
                data.insert(data.end(), values.begin(), values.end());
            }
            if (data.empty())
                data.push_back(glm::vec4(0));
            InterfaceBuffers[&program] = buffer(data.size() * 16);
            return Make(program.GLSL, bindings, 24, &program.SpirV);
        };
        for (const auto& [stage, program] : compiled.Programs)
            if (program.Target == SimTarget::GPU)
                StageKernels[stage] = makeStage(program);
        for (const auto& handler : compiled.Handlers)
            HandlerKernels.push_back(makeStage(handler.Bytecode));
        Spawn        = StageKernels.at(Stage::ParticleSpawn);
        Update       = StageKernels.at(Stage::ParticleUpdate);
        Prepare      = Make(R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Counters{uint c[];};
layout(push_constant) uniform P{uint requested;uint allowRate;}p;
void main(){uint rate=p.allowRate!=0u?c[15]:0u;uint requested=p.requested>0xffffffffu-rate?0xffffffffu:p.requested+rate;uint n=min(requested,c[0]);uint freeBefore=atomicAdd(c[0],0u-n);c[2]=n;c[4]=freeBefore-n;c[5]=c[3];c[3]+=n;c[6]+=requested-n;c[7]+=n;}
)",
                            {{0, BindingType::StorageBuffer}},
                            8);
        PrepareEvent = Make(
            R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Counters{uint c[];};
layout(set=0,binding=1,std430) readonly buffer Events{uvec4 events[];};
layout(set=0,binding=2,std430) buffer Arguments{uint args[];};
layout(push_constant) uniform P{uint number;uint limit;uint every;}p;
void main(){uint count=min(events[0].x,p.limit);uint n=0u;
if(p.every!=0u)n=c[1];else{uint requested=p.number==0u?0u:(count>0xffffffffu/p.number?0xffffffffu:count*p.number);n=min(requested,c[0]);uint before=atomicAdd(c[0],0u-n);c[2]=n;c[4]=before-n;c[5]=c[3];c[3]+=n;c[6]+=requested-n;c[7]+=n;}
args[0]=(n+63u)/64u;args[1]=1u;args[2]=1u;}
)",
            {{0, BindingType::StorageBuffer}, {1, BindingType::ReadOnlyStorageBuffer}, {2, BindingType::StorageBuffer}},
            12);
        auto program = CreateBuiltinComputeShader(device, "CompactAlive");
        auto layout  = device.CreateBindGroupLayout({0,
                                                     {{0, BindingType::ReadOnlyStorageBuffer},
                                                      {1, BindingType::StorageBuffer},
                                                      {2, BindingType::StorageBuffer},
                                                      {3, BindingType::StorageBuffer},
                                                      {4, BindingType::StorageBuffer}}});
        Compact      = {device.CreateComputePipeline({program, {layout}, 4}), layout};
        auto source  = std::string(R"(#version 450
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
            for (const auto& [name, buffer] : EventBuffers)
                initialization.push_back({graph.ImportBuffer("VFX events", buffer), GraphAccess::TransferWrite});
            if (EmitterState)
                initialization.push_back(
                    {graph.ImportBuffer("VFX emitter state", EmitterState), GraphAccess::TransferWrite});
            std::vector<glm::uvec4> emitterValues;
            if (EmitterState)
                for (const auto& p : Compiled.Programs.at(Stage::EmitterUpdate).Parameters)
                {
                    auto at = emitter.Pending.empty() ? ParameterMap::const_iterator {} :
                                                        emitter.Pending.front().Parameters.find(p.Name);
                    emitterValues.push_back(
                        Pack(emitter.Pending.empty() || at == emitter.Pending.front().Parameters.end() ?
                                 p.Default.value_or(Value::Vector({}, p.Kind)) :
                                 at->second));
                }
            graph.AddPass(
                "VFX initialize",
                GraphPassKind::Transfer,
                std::move(initialization),
                [f,
                 fl,
                 c,
                 cap,
                 tables  = InterfaceData,
                 buffers = InterfaceBuffers,
                 events  = EventBuffers,
                 state   = EmitterState,
                 emitterValues](auto& ctx) {
                    for (const auto& [stage, data] : tables)
                        Upload(ctx.Device, ctx.Commands, buffers.at(stage), data.data(), data.size() * 16);
                    for (const auto& [name, buffer] : events)
                    {
                        ctx.Commands.FillBuffer(buffer, 0, buffer->GetDesc().Bytes, 0);
                        ctx.Commands.FillBuffer(buffer, buffer->GetDesc().Bytes - 16, 16, 0xa17ecafeu);
                    }
                    if (state)
                        Upload(ctx.Device, ctx.Commands, state, emitterValues.data(), emitterValues.size() * 16);
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
            auto requested = tick.SpawnCount;
            for (const auto& [name, buffer] : EventBuffers)
                graph.AddPass(
                    "VFX reset event channel",
                    GraphPassKind::Transfer,
                    {{graph.ImportBuffer("VFX events", buffer), GraphAccess::TransferWrite}},
                    [buffer](auto& ctx) { ctx.Commands.FillBuffer(buffer, 0, 16, 0); },
                    true);
            auto stage = [&](const Program& program, const Kernel& kernel, bool eventDispatch = false) {
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
                auto                  table        = InterfaceBuffers.at(&program);
                bool                  spawn        = program.StageKind == Stage::ParticleSpawn;
                bool                  emitterStage = program.StageKind == Stage::EmitterUpdate;
                std::vector<GraphUse> uses         = {{attributes, GraphAccess::StorageReadWrite},
                                                      {flags, GraphAccess::StorageReadWrite},
                                                      {free, GraphAccess::StorageReadWrite},
                                                      {counters, GraphAccess::StorageReadWrite},
                                                      {alive, GraphAccess::StorageRead},
                                                      {gp, GraphAccess::TransferWrite}};
                uses.push_back({graph.ImportBuffer("VFX data interface table", table), GraphAccess::StorageRead});
                if (eventDispatch)
                {
                    di = EventDispatchArgs;
                    uses.push_back({graph.ImportBuffer("VFX event dispatch", di), GraphAccess::IndirectRead});
                }
                else if (!spawn && !emitterStage)
                    uses.push_back({dispatch, GraphAccess::IndirectRead});
                std::vector<Handle<Buffer>> bound = {a, params, f, fr, c, al, table};
                for (const auto& resource : program.Resources)
                {
                    if (resource.Kind == "EmitterState")
                    {
                        bound.push_back(EmitterState);
                        uses.push_back(
                            {graph.ImportBuffer("VFX emitter state", EmitterState), GraphAccess::StorageReadWrite});
                        continue;
                    }
                    GpuEmitter* source = this;
                    if (resource.Kind != "EventOutput")
                    {
                        if (!emitter.Siblings)
                            throw std::logic_error("Cross-emitter GPU binding requires a system");
                        source = emitter.Siblings->at(resource.Source).GPU.get();
                        if (!source)
                            throw std::logic_error("GPU source emitter was not initialized");
                    }
                    if (resource.Kind == "ParticleReader")
                        for (auto buffer : {source->Attributes, source->Alive, source->Counters})
                        {
                            bound.push_back(buffer);
                            uses.push_back({graph.ImportBuffer("VFX reader source", buffer), GraphAccess::StorageRead});
                        }
                    else
                    {
                        auto buffer = source->EventBuffers.at(resource.Channel);
                        bound.push_back(buffer);
                        uses.push_back({graph.ImportBuffer("VFX event channel", buffer),
                                        resource.Kind == "EventOutput" ? GraphAccess::StorageReadWrite :
                                                                         GraphAccess::StorageRead});
                    }
                }
                graph.AddPass(
                    spawn ? "VFX particle spawn" : "VFX particle update",
                    GraphPassKind::Compute,
                    std::move(uses),
                    [kernel,
                     params,
                     words = std::move(words),
                     bound,
                     di,
                     push,
                     spawn,
                     cap,
                     eventDispatch,
                     emitterStage](auto& ctx) {
                        Upload(ctx.Device, ctx.Commands, params, words.data(), words.size() * 16);
                        ctx.Commands.BindComputePipeline(kernel.Pipeline);
                        ctx.Commands.BindGroup(Group(ctx.Device, kernel.Layout, bound));
                        ctx.Commands.PushConstants(0, &push, sizeof(push));
                        if (emitterStage)
                            ctx.Commands.Dispatch(1, 1, 1);
                        else if (eventDispatch)
                            ctx.Commands.DispatchIndirect(di);
                        else if (spawn)
                            ctx.Commands.Dispatch(std::max(1u, (cap + 63) / 64), 1, 1);
                        else
                            ctx.Commands.DispatchIndirect(di);
                    },
                    true);
            };
            if (auto at = StageKernels.find(Stage::EmitterUpdate); at != StageKernels.end())
                stage(Compiled.Programs.at(Stage::EmitterUpdate), at->second);
            auto       prepare = Prepare;
            auto       ct      = Counters;
            auto       allow   = tick.Parameters.find("Transient.AllowSpawning");
            glm::uvec2 request(requested, allow == tick.Parameters.end() || allow->second.Integer ? 1u : 0u);
            graph.AddPass(
                "VFX reserve spawn",
                GraphPassKind::Compute,
                {{counters, GraphAccess::StorageReadWrite}},
                [prepare, ct, request](auto& ctx) {
                    ctx.Commands.BindComputePipeline(prepare.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, prepare.Layout, {ct}));
                    ctx.Commands.PushConstants(0, &request, 8);
                    ctx.Commands.Dispatch(1, 1, 1);
                },
                true);
            stage(Compiled.Programs.at(Stage::ParticleSpawn), Spawn);
            compact();
            stage(Compiled.Programs.at(Stage::ParticleUpdate), Update);
            compact();
            for (uint32_t h = 0; h < Compiled.Handlers.size(); ++h)
            {
                const auto& handler = Compiled.Handlers[h];
                if (!emitter.Siblings)
                    throw std::logic_error("GPU event handler requires a system");
                auto       source = emitter.Siblings->at(handler.Source).GPU->EventBuffers.at(handler.Settings.Channel);
                auto       kernel = PrepareEvent;
                auto       c = Counters, args = EventDispatchArgs;
                glm::uvec3 push(handler.Settings.SpawnNumber,
                                handler.Settings.MaxEventsPerFrame,
                                handler.Settings.Mode == EventMode::EveryParticle ? 1u : 0u);
                graph.AddPass(
                    "VFX reserve event spawn",
                    GraphPassKind::Compute,
                    {{counters, GraphAccess::StorageReadWrite},
                     {graph.ImportBuffer("VFX source events", source), GraphAccess::StorageRead},
                     {graph.ImportBuffer("VFX event dispatch", args), GraphAccess::StorageWrite}},
                    [kernel, c, source, args, push](auto& ctx) {
                        ctx.Commands.BindComputePipeline(kernel.Pipeline);
                        ctx.Commands.BindGroup(Group(ctx.Device, kernel.Layout, {c, source, args}));
                        ctx.Commands.PushConstants(0, &push, 12);
                        ctx.Commands.Dispatch(1, 1, 1);
                    },
                    true);
                stage(handler.Bytecode, HandlerKernels[h], true);
                compact();
            }
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
    void GpuEmitter::AddSystemPasses(RenderGraph& graph, SystemInstance& instance, RenderHardware::Device& device)
    {
        auto&                             emitters = instance.Emitters();
        std::vector<std::vector<GpuTick>> ticks(emitters.size());
        size_t                            steps = 0;
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            auto& e = emitters[i];
            if (!e.GPU)
                e.GPU = std::make_unique<GpuEmitter>(device, *e.Compiled);
            if (&e.GPU->Device != &device)
                throw std::logic_error("VFX instance rendered on a different device");
            if (e.Compiled->Settings.Target == SimTarget::CPU)
                e.GPU->UploadCpu(graph, e);
            else
            {
                e.GPU->Poll(e);
                ticks[i] = std::move(e.Pending);
                steps    = std::max(steps, ticks[i].size());
            }
        }
        for (size_t step = 0; step < steps; ++step)
            for (auto i : instance.Asset()->GetData().TickOrder)
                if (step < ticks[i].size())
                {
                    emitters[i].Pending = {std::move(ticks[i][step])};
                    emitters[i].GPU->AddPasses(graph, emitters[i]);
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
