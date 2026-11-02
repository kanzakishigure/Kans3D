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
            const auto* words = static_cast<const uint8_t*>(data);
            for (size_t first = 0; first < bytes; first += 65536)
                commands.UpdateBuffer(destination, first, words + first, std::min(size_t(65536), bytes - first));
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
            for (const auto& [binding, slot] : MeshGpuBindings(program))
                bindings.push_back({slot, BindingType::ReadOnlyStorageBuffer});
            if (UsesForceFields(program))
                bindings.push_back({ForceFieldsGpuBinding(program), BindingType::ReadOnlyStorageBuffer});
            auto& data                 = InterfaceData[&program];
            data                       = GpuInterfaceTable(program, {});
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
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Counters{uint c[];};
layout(set=0,binding=1,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=2,std430) readonly buffer Alive{uint alive[];};
layout(set=0,binding=3,std430) buffer Partials{vec4 partials[];};
layout(push_constant) uniform P{uint capacity;}p;
shared vec4 lower[64],upper[64];
void main(){uint i=gl_GlobalInvocationID.x,lane=gl_LocalInvocationID.x;vec3 lo=vec3(3.402823e38),hi=-lo;float radius=0.0;
)");
        for (const auto& attribute : compiled.Attributes)
            if (attribute.Kind == Type::Position)
                source += "if(i<c[1]){vec3 pos=uintBitsToFloat(a[" + std::to_string(attribute.Offset) +
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
                source += "if(i<c[1]){vec2 size=uintBitsToFloat(a[" + std::to_string(at->Offset) +
                          "u*p.capacity+alive[i]]).xy;radius=max(radius,length(size*vec2(" + std::to_string(pivot.x) +
                          "," + std::to_string(pivot.y) + ")));}";
            }
        for (const auto& renderer : compiled.Settings.Renderers)
            if (renderer.Kind == RendererKind::Mesh)
            {
                auto radius = compiled.MeshRadii.find(renderer.Guid);
                if (radius == compiled.MeshRadii.end())
                    continue;
                for (const auto& a : compiled.Attributes)
                    if (a.Name == renderer.Bindings.at("Scale"))
                        source += "if(i<c[1]){vec3 scale=abs(uintBitsToFloat(a[" + std::to_string(a.Offset) +
                                  "u*p.capacity+alive[i]]).xyz);radius=max(radius," + std::to_string(radius->second) +
                                  "*max(scale.x,max(scale.y,scale.z)));}";
            }
        for (const auto& renderer : compiled.Settings.Renderers)
            if (renderer.Kind == RendererKind::Ribbon)
                for (const auto& a : compiled.Attributes)
                    if (a.Name == renderer.Bindings.at("RibbonWidth"))
                        source += "if(i<c[1])radius=max(radius,abs(uintBitsToFloat(a[" + std::to_string(a.Offset) +
                                  "u*p.capacity+alive[i]]).x)*0.5);";
        source += R"(
lower[lane]=vec4(lo,radius);upper[lane]=vec4(hi,0);barrier();
for(uint stride=32;stride>0;stride/=2){if(lane<stride){lower[lane].xyz=min(lower[lane].xyz,lower[lane+stride].xyz);lower[lane].w=max(lower[lane].w,lower[lane+stride].w);upper[lane].xyz=max(upper[lane].xyz,upper[lane+stride].xyz);}barrier();}
if(lane==0){partials[gl_WorkGroupID.x*2]=lower[0];partials[gl_WorkGroupID.x*2+1]=upper[0];}})";
        BoundsPartials = buffer(size_t((cap + 63) / 64) * 32);
        BoundsKernel   = Make(source,
                              {{0, BindingType::ReadOnlyStorageBuffer},
                               {1, BindingType::ReadOnlyStorageBuffer},
                               {2, BindingType::ReadOnlyStorageBuffer},
                               {3, BindingType::StorageBuffer}},
                              4);
        BoundsFinish   = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Counters{uint c[];};
layout(set=0,binding=1,std430) readonly buffer Partials{vec4 partials[];};
layout(push_constant) uniform P{uint groups;}p;
shared vec4 lower[64],upper[64];
void main(){uint lane=gl_LocalInvocationID.x;vec4 lo=vec4(vec3(3.402823e38),0),hi=vec4(vec3(-3.402823e38),0);
for(uint i=lane;i<p.groups;i+=64){lo.xyz=min(lo.xyz,partials[i*2].xyz);lo.w=max(lo.w,partials[i*2].w);hi.xyz=max(hi.xyz,partials[i*2+1].xyz);}
lower[lane]=lo;upper[lane]=hi;barrier();
for(uint stride=32;stride>0;stride/=2){if(lane<stride){lower[lane].xyz=min(lower[lane].xyz,lower[lane+stride].xyz);lower[lane].w=max(lower[lane].w,lower[lane+stride].w);upper[lane].xyz=max(upper[lane].xyz,upper[lane+stride].xyz);}barrier();}
if(lane==0){for(uint k=0;k<3;++k){c[8+k]=floatBitsToUint(lower[0][k]);c[11+k]=floatBitsToUint(upper[0][k]);}c[14]=floatBitsToUint(lower[0].w);}})",
                              {{0, BindingType::StorageBuffer}, {1, BindingType::ReadOnlyStorageBuffer}},
                              4);
        for (auto& r : Readbacks)
            r = device.CreateBuffer({64, BufferUsage::CopyDestination, MemoryUsage::Readback});
    }
    Handle<Buffer> GpuEmitter::FieldBuffer(RenderGraph& graph, const ExecutionContext& context)
    {
        auto snapshot = context.Fields ? context.Fields : EmptyFields;
        if (!snapshot->GPU)
            snapshot->GPU = std::make_shared<FieldGpuStorage>();
        auto& slots    = snapshot->GPU->Slots;
        auto  lifetime = graph.Lifetime();
        for (auto& slot : slots)
            if (slot.Device == &Device && slot.Graph.lock() == lifetime.lock() && slot.Snapshot.lock() == snapshot)
                return slot.Buffer;
        auto at = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
            return slot.Device == &Device && slot.Graph.expired();
        });
        if (at == slots.end())
        {
            slots.emplace_back();
            at = std::prev(slots.end());
        }
        auto& slot           = *at;
        slot.Graph           = lifetime;
        slot.Snapshot        = snapshot;
        slot.Device          = &Device;
        const auto& records  = snapshot->Fields.GetRecords();
        const auto& impulses = snapshot->Fields.GetImpulses();
        static_assert(sizeof(FieldRecord) == 160);
        std::vector<glm::vec4> data(1 + records.size() * 10 + impulses.size() * 4);
        auto                   word = [](glm::uvec4 v) {
            glm::vec4 out;
            std::memcpy(&out, &v, 16);
            return out;
        };
        data[0] = word({uint32_t(records.size()), uint32_t(impulses.size()), 0, 0});
        if (!records.empty())
            std::memcpy(data.data() + 1, records.data(), records.size() * sizeof(FieldRecord));
        for (size_t i = 0; i < impulses.size(); ++i)
        {
            const auto& p = impulses[i];
            size_t      o = 1 + records.size() * 10 + i * 4;
            data[o]       = word({p.LayerMask, p.AffectMask, 0, 0});
            data[o + 1]   = glm::vec4(p.Position, p.Radius);
            data[o + 2]   = glm::vec4(p.Impulse, p.UpwardsModifier);
            data[o + 3] =
                word({uint32_t(p.Sequence), uint32_t(p.Sequence >> 32), uint32_t(p.Falloff), uint32_t(p.Radial)});
        }
        if (!slot.Buffer || slot.Buffer->GetDesc().Bytes < data.size() * 16)
            slot.Buffer = Device.CreateBuffer({std::max(size_t(4096), data.size() * 16),
                                               BufferUsage::Storage | BufferUsage::CopyDestination,
                                               MemoryUsage::DeviceLocal});
        auto buffer = slot.Buffer;
        graph.AddPass(
            "VFX scene field snapshot",
            GraphPassKind::Transfer,
            {{graph.ImportBuffer("VFX scene fields", buffer), GraphAccess::TransferWrite}},
            [buffer, data = std::move(data)](auto& ctx) {
                Upload(ctx.Device, ctx.Commands, buffer, data.data(), data.size() * 16);
            },
            true);
        return buffer;
    }
    Handle<Buffer> GpuEmitter::MeshGeometryBuffer(const InterfaceBinding& binding) const
    {
        if (!binding.Geometry)
            return {};
        auto at = binding.Geometry->Buffers.find(&Device);
        return at == binding.Geometry->Buffers.end() ? nullptr : at->second.lock();
    }
    Handle<Buffer> GpuEmitter::GeometryBuffer(const InterfaceBinding& binding, const ExecutionContext& context)
    {
        auto snapshot = binding.Kind != "VectorField" && context.MeshTable ? context.MeshTable(binding) : nullptr;
        auto geometry = snapshot ? snapshot->Geometry : binding.Geometry;
        // Unavailable sources still need a legal binding; validity prevents shader reads.
        if (!geometry)
        {
            if (!EmptyGeometry)
                EmptyGeometry = InternMeshGeometry(std::vector<glm::vec4>(12, glm::vec4(0)));
            geometry = EmptyGeometry;
        }
        auto buffer = geometry->Buffers[&Device].lock();
        if (!buffer)
        {
            buffer        = Device.CreateBuffer({geometry->Values.size() * 16,
                                                 BufferUsage::Storage | BufferUsage::CopyDestination,
                                                 MemoryUsage::DeviceLocal});
            auto commands = Device.CreateCommandList();
            Upload(Device, *commands, buffer, geometry->Values.data(), geometry->Values.size() * 16);
            Device.Submit(commands);
            geometry->Buffers[&Device] = buffer;
        }
        GeometryBuffers[geometry] = buffer;
        return buffer;
    }
    void GpuEmitter::AddPasses(RenderGraph& graph, EmitterInstance& emitter, bool readback)
    {
        auto releaseExpiredGeometry = [&]() {
            for (auto at = GeometryBuffers.begin(); at != GeometryBuffers.end();)
                if (at->first.expired())
                    at = GeometryBuffers.erase(at);
                else
                    ++at;
        };
        releaseExpiredGeometry();
        const auto cap  = Compiled.Settings.Capacity;
        auto attributes = graph.ImportBuffer("VFX attributes", Attributes),
             alive = graph.ImportBuffer("VFX alive", Alive), free = graph.ImportBuffer("VFX free", Free),
             flags = graph.ImportBuffer("VFX flags", Flags), counters = graph.ImportBuffer("VFX counters", Counters),
             count       = graph.ImportBuffer("VFX compact count", Count),
             draw        = graph.ImportBuffer("VFX draw indirect", DrawArgs),
             dispatch    = graph.ImportBuffer("VFX dispatch indirect", DispatchArgs);
        const bool reset = NeedsInitialization();
        if (reset)
        {
            const bool firstInitialization = !Initialized;
            Initialized                    = true;
            ResetPending                   = false;
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
                 tables  = firstInitialization ? InterfaceData : decltype(InterfaceData) {},
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
        if (reset)
            compact();
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
                auto config = Pack(Value::Float(float(tick.Context.FieldTime)));
                words.push_back({tick.Context.FieldLayer, config.x, 0, 0});
                words.push_back({uint32_t(tick.Context.FirstImpulse), uint32_t(tick.Context.FirstImpulse >> 32), 0, 0});
                auto& parameterWorkspace = Acquire(ParameterWorkspaces, graph);
                if (!parameterWorkspace.Output || parameterWorkspace.Output->GetDesc().Bytes < words.size() * 16)
                    parameterWorkspace.Output =
                        Device.CreateBuffer({words.size() * 16,
                                             BufferUsage::Storage | BufferUsage::CopyDestination,
                                             MemoryUsage::DeviceLocal});
                auto params = parameterWorkspace.Output;
                auto gp     = graph.ImportBuffer("VFX stage parameters", params);
                auto a = Attributes, f = Flags, fr = Free, c = Counters, al = Alive, di = DispatchArgs;
                Push push {cap,
                           tick.Context.Seed,
                           tick.Context.EmitterIndex,
                           requested,
                           uint32_t(Compiled.Settings.SimulationSpace == Space::World),
                           uint32_t(tick.Parameters.at("Emitter.ExecutionState").Integer == 3 ||
                                    tick.Parameters.at("System.ExecutionState").Integer == 3)};
                auto table      = InterfaceBuffers.at(&program);
                bool meshTables = std::any_of(
                    program.Code.begin(), program.Code.end(), [](const auto& i) { return IsMeshOperation(i.Op); });
                if (meshTables)
                {
                    auto  data      = GpuInterfaceTable(program, tick.Context);
                    auto& workspace = Acquire(InterfaceWorkspaces, graph);
                    if (!workspace.Output || workspace.Output->GetDesc().Bytes < data.size() * 16)
                        workspace.Output = Device.CreateBuffer({data.size() * 16,
                                                                BufferUsage::Storage | BufferUsage::CopyDestination,
                                                                MemoryUsage::DeviceLocal});
                    table = workspace.Output;
                    graph.AddPass(
                        "VFX mesh sampling snapshot",
                        GraphPassKind::Transfer,
                        {{graph.ImportBuffer("VFX mesh sampling snapshot", table), GraphAccess::TransferWrite}},
                        [table, data = std::move(data)](auto& ctx) {
                            Upload(ctx.Device, ctx.Commands, table, data.data(), data.size() * 16);
                        },
                        true);
                }
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
                for (const auto& [binding, slot] : MeshGpuBindings(program))
                {
                    auto buffer = GeometryBuffer(*binding, tick.Context);
                    bound.push_back(buffer);
                    uses.push_back({graph.ImportBuffer("VFX static mesh geometry", buffer), GraphAccess::StorageRead});
                }
                if (UsesForceFields(program))
                {
                    auto buffer = FieldBuffer(graph, tick.Context);
                    bound.push_back(buffer);
                    uses.push_back({graph.ImportBuffer("VFX scene fields", buffer), GraphAccess::StorageRead});
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
        releaseExpiredGeometry();
        if (worked)
        {
            auto           k = BoundsKernel;
            auto           c = Counters, a = Attributes, al = Alive, partials = BoundsPartials;
            auto           partialId = graph.ImportBuffer("VFX partial bounds", partials);
            const uint32_t groups    = (cap + 63) / 64;
            graph.AddPass(
                "VFX dynamic bounds groups",
                GraphPassKind::Compute,
                {{counters, GraphAccess::StorageRead},
                 {attributes, GraphAccess::StorageRead},
                 {alive, GraphAccess::StorageRead},
                 {partialId, GraphAccess::StorageWrite}},
                [k, c, a, al, partials, cap, groups](auto& ctx) {
                    ctx.Commands.BindComputePipeline(k.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, k.Layout, {c, a, al, partials}));
                    ctx.Commands.PushConstants(0, &cap, 4);
                    ctx.Commands.Dispatch(groups);
                },
                true);
            k = BoundsFinish;
            graph.AddPass(
                "VFX dynamic bounds reduction",
                GraphPassKind::Compute,
                {{counters, GraphAccess::StorageReadWrite}, {partialId, GraphAccess::StorageRead}},
                [k, c, partials, groups](auto& ctx) {
                    ctx.Commands.BindComputePipeline(k.Pipeline);
                    ctx.Commands.BindGroup(Group(ctx.Device, k.Layout, {c, partials}));
                    ctx.Commands.PushConstants(0, &groups, 4);
                    ctx.Commands.Dispatch(1);
                },
                true);
        }
        if (worked && readback && !Pending[ReadbackCursor])
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
                if (!instance.TimelineDesiredAge)
                    e.GPU->Poll(e);
                ticks[i] = std::move(e.Pending);
                steps    = std::max(steps, ticks[i].size());
                if (ticks[i].empty() && e.GPU->NeedsInitialization())
                    e.GPU->AddPasses(graph, e, !instance.TimelineDesiredAge);
            }
        }
        for (size_t step = 0; step < steps; ++step)
            for (auto i : instance.Asset()->GetData().TickOrder)
                if (step < ticks[i].size())
                {
                    emitters[i].Pending = {std::move(ticks[i][step])};
                    emitters[i].GPU->AddPasses(graph, emitters[i], !instance.TimelineDesiredAge);
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
    std::vector<uint32_t> GpuEmitter::SortCpu(const EmitterInstance& emitter,
                                              const Renderer&        renderer,
                                              const glm::mat4&       view,
                                              const glm::mat4&       owner)
    {
        auto indices = emitter.Alive;
        if (renderer.Kind == RendererKind::Ribbon)
        {
            struct Key
            {
                int      RibbonID;
                float    Order;
                uint32_t UniqueID;
            };
            std::vector<Key> keys(emitter.Compiled->Settings.Capacity);
            for (auto slot : indices)
            {
                float order = emitter.Read(renderer.Bindings.at("RibbonLinkOrder"), slot).Data.x;
                keys[slot]  = {emitter.Read(renderer.Bindings.at("RibbonID"), slot).Integer,
                               std::isnan(order) ? 0.f : order,
                               uint32_t(emitter.Read("Particles.UniqueID", slot).Integer)};
            }
            std::sort(indices.begin(), indices.end(), [&](uint32_t a, uint32_t b) {
                const auto& x = keys[a];
                const auto& y = keys[b];
                if (x.RibbonID != y.RibbonID)
                    return x.RibbonID < y.RibbonID;
                if (x.Order != y.Order)
                    return x.Order < y.Order;
                // Newest first, including when float spawn keys round to the same value.
                if (x.UniqueID != y.UniqueID)
                    return x.UniqueID > y.UniqueID;
                return a < b;
            });
            return indices;
        }
        if (renderer.Sorting <= Sort::EmitterOrder)
            return indices;
        auto       camera = glm::vec3(glm::inverse(view)[3]);
        const auto key    = [&](uint32_t slot) {
            if (renderer.Sorting == Sort::Custom)
            {
                float value = emitter.Read(renderer.Bindings.at("SortKey"), slot).Data.x;
                return std::isnan(value) ? 0.f : value;
            }
            auto position = emitter.Read(renderer.Bindings.at("Position"), slot).Data;
            position.w    = 1;
            if (emitter.Compiled->Settings.SimulationSpace == Space::Local)
                position = owner * position;
            return renderer.Sorting == Sort::ParticleDistance ?
                       glm::dot(glm::vec3(position) - camera, glm::vec3(position) - camera) :
                       -(view * position).z;
        };
        std::vector<float> keys(emitter.Compiled->Settings.Capacity);
        for (auto slot : indices)
            keys[slot] = key(slot);
        bool ascending = renderer.Sorting == Sort::Custom && renderer.SortAscending;
        std::stable_sort(indices.begin(), indices.end(), [&](uint32_t a, uint32_t b) {
            return ascending ? keys[a] < keys[b] : keys[a] > keys[b];
        });
        return indices;
    }
    GpuEmitter::Workspace& GpuEmitter::Acquire(std::vector<Workspace>& pool, RenderGraph& graph)
    {
        for (auto& workspace : pool)
            if (workspace.Graph.expired())
            {
                workspace.Graph       = graph.Lifetime();
                workspace.RendererKey = nullptr;
                workspace.Input       = nullptr;
                return workspace;
            }
        pool.emplace_back();
        pool.back().Graph = graph.Lifetime();
        return pool.back();
    }
    Handle<Buffer> GpuEmitter::SortForView(RenderGraph&           graph,
                                           const EmitterInstance& emitter,
                                           const Renderer&        renderer,
                                           const glm::mat4&       view,
                                           const glm::mat4&       owner,
                                           bool                   transparent)
    {
        if (renderer.Kind != RendererKind::Ribbon && (!transparent || renderer.Sorting <= Sort::EmitterOrder))
            return Alive;
        const auto cap       = Compiled.Settings.Capacity;
        auto&      workspace = Acquire(SortWorkspaces, graph);
        if (!workspace.Output)
            workspace.Output = Device.CreateBuffer(
                {size_t(cap) * 4, BufferUsage::Storage | BufferUsage::CopyDestination | BufferUsage::CopySource});
        auto output = workspace.Output;
        auto out    = graph.ImportBuffer("VFX sorted alive per view", output);
        if (Compiled.Settings.Target == SimTarget::CPU)
        {
            auto indices = SortCpu(emitter, renderer, view, owner);
            graph.AddPass(
                "VFX CPU sort upload",
                GraphPassKind::Transfer,
                {{out, GraphAccess::TransferWrite}},
                [output, indices = std::move(indices)](auto& c) {
                    if (!indices.empty())
                        for (size_t first = 0; first < indices.size(); first += 16384)
                            c.Commands.UpdateBuffer(output,
                                                    first * 4,
                                                    indices.data() + first,
                                                    std::min(size_t(16384), indices.size() - first) * 4);
                },
                true);
            return output;
        }
        if (!SortPrepare.Pipeline)
        {
            SortPrepare = Make(R"(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=1,std430) buffer Dispatch{uint args[];};
void main(){uint n=count[1],padded=1;while(padded<n)padded*=2;args[0]=n==0?0:(padded+63)/64;args[1]=1;args[2]=1;args[3]=padded;}
)",
                               {{0, BindingType::ReadOnlyStorageBuffer}, {1, BindingType::StorageBuffer}},
                               0);
            SortKeys    = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=1,std430) readonly buffer Alive{uint alive[];};
layout(set=0,binding=2,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=3,std430) buffer Pairs{uvec4 pairs[];};
layout(set=0,binding=4,std140) uniform Params{mat4 owner;mat4 view;vec4 camera;uvec4 data;uvec4 options;}p;
void main(){uint padded=1;while(padded<count[1])padded*=2;uint i=gl_GlobalInvocationID.x;if(i>=padded)return;
if(i>=count[1]){pairs[i]=p.options.w!=0?uvec4(0,0,0xffffffffu,0):uvec4(0,0xffffffffu,0,1);return;}
uint slot=alive[i];float key;
if(p.options.w!=0){key=uintBitsToFloat(a[p.data.z*p.data.x+slot]).x;if(isnan(key))key=0;
pairs[i]=uvec4(floatBitsToUint(key),a[p.data.w*p.data.x+slot].x,slot,a[p.data.y*p.data.x+slot].x);return;}
if(p.options.x==4)key=uintBitsToFloat(a[p.data.z*p.data.x+slot]).x;
else{vec4 pos=vec4(uintBitsToFloat(a[p.data.y*p.data.x+slot]).xyz,1);if(p.options.z!=0)pos=p.owner*pos;
key=p.options.x==3?dot(pos.xyz-p.camera.xyz,pos.xyz-p.camera.xyz):-(p.view*pos).z;}
if(isnan(key))key=0; if(p.options.y==0)key=-key;
pairs[i]=uvec4(floatBitsToUint(key),i,slot,0);}
)",
                               {{0, BindingType::ReadOnlyStorageBuffer},
                                {1, BindingType::ReadOnlyStorageBuffer},
                                {2, BindingType::ReadOnlyStorageBuffer},
                                {3, BindingType::StorageBuffer},
                                {4, BindingType::UniformBuffer}},
                               0);
            SortStep    = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Pairs{uvec4 pairs[];};
layout(set=0,binding=1,std430) readonly buffer Count{uint count[];};
layout(push_constant) uniform P{uint width;uint stride;uint ribbon;}p;
bool before(uvec4 a,uvec4 b){if(p.ribbon!=0){if(a.z==0xffffffffu||b.z==0xffffffffu)return a.z!=0xffffffffu;
if(a.w!=b.w)return int(a.w)<int(b.w);float x=uintBitsToFloat(a.x),y=uintBitsToFloat(b.x);
return x<y||(x==y&&(a.y>b.y||(a.y==b.y&&a.z<b.z)));}
if(a.w!=b.w)return a.w<b.w;float x=uintBitsToFloat(a.x),y=uintBitsToFloat(b.x);return x<y||(x==y&&a.y<b.y);}
void main(){uint padded=1;while(padded<count[1])padded*=2;uint i=gl_GlobalInvocationID.x,j=i^p.stride;if(i>=padded||j<=i||j>=padded)return;
uvec4 a=pairs[i],b=pairs[j];if(before(b,a)==((i&p.width)==0)){pairs[i]=b;pairs[j]=a;}}
)",
                               {{0, BindingType::StorageBuffer}, {1, BindingType::ReadOnlyStorageBuffer}},
                               12);
            SortOutput  = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Pairs{uvec4 pairs[];};
layout(set=0,binding=1,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=2,std430) buffer Output{uint indices[];};
void main(){uint i=gl_GlobalInvocationID.x;if(i<count[1])indices[i]=pairs[i].z;}
)",
                               {{0, BindingType::ReadOnlyStorageBuffer},
                                {1, BindingType::ReadOnlyStorageBuffer},
                                {2, BindingType::StorageBuffer}},
                               0);
        }
        uint32_t padded = 1;
        while (padded < cap)
            padded *= 2;
        if (!workspace.Pairs)
        {
            workspace.Pairs    = Device.CreateBuffer({size_t(padded) * 16, BufferUsage::Storage});
            workspace.Dispatch = Device.CreateBuffer({16, BufferUsage::Storage | BufferUsage::Indirect});
        }
        auto pairs = workspace.Pairs, dispatch = workspace.Dispatch;
        auto pairId     = graph.ImportBuffer("VFX sort key ordinal slot", pairs),
             dispatchId = graph.ImportBuffer("VFX sort indirect dispatch", dispatch);
        auto countId    = graph.ImportBuffer("VFX draw indirect", DrawArgs);
        auto count      = DrawArgs;
        auto prepare    = SortPrepare;
        graph.AddPass(
            "VFX sort prepare",
            GraphPassKind::Compute,
            {{countId, GraphAccess::StorageRead}, {dispatchId, GraphAccess::StorageWrite}},
            [prepare, count, dispatch](auto& c) {
                c.Commands.BindComputePipeline(prepare.Pipeline);
                c.Commands.BindGroup(Group(c.Device, prepare.Layout, {count, dispatch}));
                c.Commands.Dispatch(1, 1, 1);
            },
            true);
        struct Params
        {
            glm::mat4  Owner, View;
            glm::vec4  Camera;
            glm::uvec4 Data, Options;
        } params;
        auto offset = [&](const char* binding) {
            for (const auto& a : Compiled.Attributes)
                if (a.Name == renderer.Bindings.at(binding))
                    return a.Offset;
            return 0u;
        };
        params.Owner  = owner;
        params.View   = view;
        params.Camera = glm::inverse(view)[3];
        params.Data   = {cap, offset("Position"), renderer.Sorting == Sort::Custom ? offset("SortKey") : 0, 0};
        if (renderer.Kind == RendererKind::Ribbon)
        {
            uint32_t unique = 0;
            for (const auto& a : Compiled.Attributes)
                if (a.Name == "Particles.UniqueID")
                    unique = a.Offset;
            params.Data = {cap, offset("RibbonID"), offset("RibbonLinkOrder"), unique};
        }
        params.Options = {uint32_t(renderer.Sorting),
                          uint32_t(renderer.Sorting == Sort::Custom && renderer.SortAscending),
                          uint32_t(Compiled.Settings.SimulationSpace == Space::Local),
                          uint32_t(renderer.Kind == RendererKind::Ribbon)};
        if (!workspace.Uniform)
            workspace.Uniform =
                Device.CreateBuffer({sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDestination});
        auto uniform   = workspace.Uniform;
        auto uniformId = graph.ImportBuffer("VFX sort view parameters", uniform);
        graph.AddPass(
            "VFX sort parameters",
            GraphPassKind::Transfer,
            {{uniformId, GraphAccess::TransferWrite}},
            [uniform, params](auto& c) { c.Commands.UpdateBuffer(uniform, 0, &params, sizeof(params)); },
            true);
        auto key        = SortKeys;
        auto attributes = Attributes, alive = Alive;
        graph.AddPass(
            "VFX sort keys",
            GraphPassKind::Compute,
            {{graph.ImportBuffer("VFX attributes", attributes), GraphAccess::StorageRead},
             {graph.ImportBuffer("VFX alive", alive), GraphAccess::StorageRead},
             {countId, GraphAccess::StorageRead},
             {dispatchId, GraphAccess::IndirectRead},
             {pairId, GraphAccess::StorageWrite},
             {uniformId, GraphAccess::UniformRead}},
            [key, attributes, alive, count, dispatch, pairs, uniform](auto& c) {
                c.Commands.BindComputePipeline(key.Pipeline);
                c.Commands.BindGroup(Group(c.Device, key.Layout, {attributes, alive, count, pairs, uniform}));
                c.Commands.DispatchIndirect(dispatch);
            },
            true);
        auto step = SortStep;
        for (uint32_t width = 2; width <= padded; width *= 2)
            for (uint32_t stride = width / 2; stride; stride /= 2)
                graph.AddPass(
                    "VFX sort bitonic",
                    GraphPassKind::Compute,
                    {{pairId, GraphAccess::StorageReadWrite},
                     {countId, GraphAccess::StorageRead},
                     {dispatchId, GraphAccess::IndirectRead}},
                    [step, pairs, count, dispatch, width, stride, ribbon = renderer.Kind == RendererKind::Ribbon](
                        auto& c) {
                        glm::uvec3 push(width, stride, uint32_t(ribbon));
                        c.Commands.BindComputePipeline(step.Pipeline);
                        c.Commands.BindGroup(Group(c.Device, step.Layout, {pairs, count}));
                        c.Commands.PushConstants(0, &push, 12);
                        c.Commands.DispatchIndirect(dispatch);
                    },
                    true);
        auto finish = SortOutput;
        graph.AddPass(
            "VFX sort output",
            GraphPassKind::Compute,
            {{pairId, GraphAccess::StorageRead},
             {countId, GraphAccess::StorageRead},
             {dispatchId, GraphAccess::IndirectRead},
             {out, GraphAccess::StorageWrite}},
            [finish, pairs, count, dispatch, output](auto& c) {
                c.Commands.BindComputePipeline(finish.Pipeline);
                c.Commands.BindGroup(Group(c.Device, finish.Layout, {pairs, count, output}));
                c.Commands.DispatchIndirect(dispatch);
            },
            true);
        return output;
    }
    GpuEmitter::MeshBin GpuEmitter::BinMesh(RenderGraph&                    graph,
                                            const Renderer&                 renderer,
                                            uint32_t                        entry,
                                            const std::vector<MeshSection>& sections,
                                            Handle<Buffer>                  indices)
    {
        if (!BinCount.Pipeline)
        {
            BinCount     = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=1,std430) readonly buffer Alive{uint alive[];};
layout(set=0,binding=2,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=3,std430) buffer Counts{uint counts[];};
layout(push_constant) uniform P{uint capacity;uint attributeOffset;uint entries;uint groups;}p;
shared uint selected[64];
void main(){uint lane=gl_LocalInvocationID.x,i=gl_GlobalInvocationID.x;
selected[lane]=i<count[1]?uint(clamp(int(a[p.attributeOffset*p.capacity+alive[i]].x),0,int(p.entries)-1)):0xffffffffu;
barrier();for(uint e=lane;e<p.entries;e+=64){uint n=0;for(uint j=0;j<64;++j)n+=uint(selected[j]==e);counts[e*p.groups+gl_WorkGroupID.x]=n;}}
)",
                                {{0, BindingType::ReadOnlyStorageBuffer},
                                 {1, BindingType::ReadOnlyStorageBuffer},
                                 {2, BindingType::ReadOnlyStorageBuffer},
                                 {3, BindingType::StorageBuffer}},
                                16);
            BinScan      = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Input{uint inputCounts[];};
layout(set=0,binding=1,std430) buffer Output{uint outputCounts[];};
layout(push_constant) uniform P{uint groups;uint entries;uint stride;}p;
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.groups*p.entries)return;outputCounts[i]=inputCounts[i]+(i%p.groups>=p.stride?inputCounts[i-p.stride]:0);}
)",
                                {{0, BindingType::ReadOnlyStorageBuffer}, {1, BindingType::StorageBuffer}},
                                12);
            BinScatter   = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=1,std430) readonly buffer Alive{uint alive[];};
layout(set=0,binding=2,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=3,std430) readonly buffer Counts{uint counts[];};
layout(set=0,binding=4,std430) buffer Output{uint outputSlots[];};
layout(push_constant) uniform P{uint capacity;uint attributeOffset;uint entries;uint groups;}p;
shared uint selected[64];
void main(){uint lane=gl_LocalInvocationID.x,i=gl_GlobalInvocationID.x,slot=0,e=0xffffffffu;
if(i<count[1]){slot=alive[i];e=uint(clamp(int(a[p.attributeOffset*p.capacity+slot].x),0,int(p.entries)-1));}
selected[lane]=e;barrier();if(i>=count[1])return;uint rank=0;for(uint j=0;j<lane;++j)rank+=uint(selected[j]==e);
uint group=gl_WorkGroupID.x;uint base=group==0?0:counts[e*p.groups+group-1];outputSlots[e*p.capacity+base+rank]=slot;}
)",
                                {{0, BindingType::ReadOnlyStorageBuffer},
                                 {1, BindingType::ReadOnlyStorageBuffer},
                                 {2, BindingType::ReadOnlyStorageBuffer},
                                 {3, BindingType::ReadOnlyStorageBuffer},
                                 {4, BindingType::StorageBuffer}},
                                16);
            BinArguments = Make(R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Counts{uint counts[];};
layout(set=0,binding=1,std430) buffer Args{uint args[];};
layout(push_constant) uniform P{uint groups;uint entry;uint sections;}p;
void main(){uint s=gl_GlobalInvocationID.x;if(s<p.sections)args[s*5+1]=counts[(p.entry+1)*p.groups-1];}
)",
                                {{0, BindingType::ReadOnlyStorageBuffer}, {1, BindingType::StorageBuffer}},
                                12);
        }
        auto       lifetime  = graph.Lifetime();
        Workspace* workspace = nullptr;
        for (auto& w : BinWorkspaces)
            if (!w.Graph.owner_before(lifetime) && !lifetime.owner_before(w.Graph) && w.RendererKey == &renderer &&
                w.Input == indices.get())
                workspace = &w;
        const uint32_t cap = Compiled.Settings.Capacity, entries = uint32_t(renderer.Meshes.size());
        const uint32_t groups = (cap + 63) / 64;
        if (!workspace)
        {
            workspace              = &Acquire(BinWorkspaces, graph);
            workspace->RendererKey = &renderer;
            workspace->Input       = indices.get();
            auto ensure            = [&](Handle<Buffer>& b, size_t bytes, BufferUsage usage) {
                if (!b || b->GetDesc().Bytes < bytes)
                    b = Device.CreateBuffer({bytes, usage});
            };
            ensure(workspace->Counts, size_t(entries) * groups * 4, BufferUsage::Storage);
            ensure(workspace->Scan, size_t(entries) * groups * 4, BufferUsage::Storage);
            ensure(workspace->Gather, size_t(entries) * cap * 4, BufferUsage::Storage | BufferUsage::CopySource);
            workspace->Bins.resize(entries);
            uint32_t attribute = 0;
            for (const auto& a : Compiled.Attributes)
                if (a.Name == renderer.Bindings.at("MeshIndex"))
                    attribute = a.Offset;
            std::array<uint32_t, 4> push {cap, attribute, entries, groups};
            auto a = Attributes, count = DrawArgs, counts = workspace->Counts, scan = workspace->Scan;
            auto gather   = workspace->Gather;
            auto attrId   = graph.ImportBuffer("VFX attributes", a),
                 aliveId  = graph.ImportBuffer("VFX bin input", indices);
            auto countId  = graph.ImportBuffer("VFX draw indirect", count);
            auto countsId = graph.ImportBuffer("VFX mesh group counts", counts);
            auto scanId   = graph.ImportBuffer("VFX mesh scanned counts", scan);
            auto gatherId = graph.ImportBuffer("VFX mesh stable gather", gather);
            auto kernel   = BinCount;
            graph.AddPass(
                "VFX mesh count all entries",
                GraphPassKind::Compute,
                {{attrId, GraphAccess::StorageRead},
                 {aliveId, GraphAccess::StorageRead},
                 {countId, GraphAccess::StorageRead},
                 {countsId, GraphAccess::StorageWrite}},
                [kernel, a, indices, count, counts, push, groups](auto& c) {
                    c.Commands.BindComputePipeline(kernel.Pipeline);
                    c.Commands.BindGroup(Group(c.Device, kernel.Layout, {a, indices, count, counts}));
                    c.Commands.PushConstants(0, push.data(), 16);
                    c.Commands.Dispatch(groups);
                },
                true);
            kernel = BinScan;
            for (uint32_t stride = 1; stride < groups; stride *= 2)
            {
                graph.AddPass(
                    "VFX mesh scan",
                    GraphPassKind::Compute,
                    {{countsId, GraphAccess::StorageRead}, {scanId, GraphAccess::StorageWrite}},
                    [kernel, counts, scan, groups, entries, stride](auto& c) {
                        std::array<uint32_t, 3> p {groups, entries, stride};
                        c.Commands.BindComputePipeline(kernel.Pipeline);
                        c.Commands.BindGroup(Group(c.Device, kernel.Layout, {counts, scan}));
                        c.Commands.PushConstants(0, p.data(), 12);
                        c.Commands.Dispatch((groups * entries + 63) / 64);
                    },
                    true);
                std::swap(counts, scan);
                std::swap(countsId, scanId);
            }
            // Keep the final scan in Counts for per-entry argument generation below.
            workspace->Counts = counts;
            workspace->Scan   = scan;
            kernel            = BinScatter;
            graph.AddPass(
                "VFX mesh scatter all entries",
                GraphPassKind::Compute,
                {{attrId, GraphAccess::StorageRead},
                 {aliveId, GraphAccess::StorageRead},
                 {countId, GraphAccess::StorageRead},
                 {countsId, GraphAccess::StorageRead},
                 {gatherId, GraphAccess::StorageWrite}},
                [kernel, a, indices, count, counts, gather, push, groups](auto& c) {
                    c.Commands.BindComputePipeline(kernel.Pipeline);
                    c.Commands.BindGroup(Group(c.Device, kernel.Layout, {a, indices, count, counts, gather}));
                    c.Commands.PushConstants(0, push.data(), 16);
                    c.Commands.Dispatch(groups);
                },
                true);
        }
        auto& result = workspace->Bins.at(entry);
        if (!result.Indices)
            result.Indices = Device.CreateBuffer(
                {size_t(cap) * 4, BufferUsage::Storage | BufferUsage::CopySource | BufferUsage::CopyDestination});
        if (!result.Arguments || result.Arguments->GetDesc().Bytes < std::max(size_t(20), sections.size() * 20))
            result.Arguments = Device.CreateBuffer({std::max(size_t(20), sections.size() * 20),
                                                    BufferUsage::Storage | BufferUsage::Indirect |
                                                        BufferUsage::CopyDestination | BufferUsage::CopySource});
        std::vector<uint32_t> values;
        for (const auto& section : sections)
            values.insert(values.end(), {section.IndexCount, 0, section.FirstIndex, 0, 0});
        auto out = result.Indices, args = result.Arguments, gather = workspace->Gather, counts = workspace->Counts;
        auto outId  = graph.ImportBuffer("VFX mesh entry alive", out),
             argsId = graph.ImportBuffer("VFX mesh arguments", args);
        graph.AddPass(
            "VFX mesh entry copy",
            GraphPassKind::Transfer,
            {{graph.ImportBuffer("VFX mesh stable gather", gather), GraphAccess::TransferRead},
             {outId, GraphAccess::TransferWrite},
             {argsId, GraphAccess::TransferWrite}},
            [out, args, gather, values, entry, cap](auto& c) {
                c.Commands.CopyBuffer(gather, size_t(entry) * cap * 4, out, 0, size_t(cap) * 4);
                for (size_t first = 0; first < values.size(); first += 16384)
                    c.Commands.UpdateBuffer(
                        args, first * 4, values.data() + first, std::min(size_t(16384), values.size() - first) * 4);
            },
            true);
        auto kernel = BinArguments;
        graph.AddPass(
            "VFX mesh entry counts",
            GraphPassKind::Compute,
            {{graph.ImportBuffer("VFX mesh group counts", counts), GraphAccess::StorageRead},
             {argsId, GraphAccess::StorageReadWrite}},
            [kernel, counts, args, groups, entry, n = uint32_t(sections.size())](auto& c) {
                std::array<uint32_t, 3> p {groups, entry, n};
                c.Commands.BindComputePipeline(kernel.Pipeline);
                c.Commands.BindGroup(Group(c.Device, kernel.Layout, {counts, args}));
                c.Commands.PushConstants(0, p.data(), 12);
                c.Commands.Dispatch(std::max(1u, (n + 63) / 64));
            },
            true);
        return result;
    }
    Handle<Buffer> GpuEmitter::UploadUniform(RenderGraph& graph, const void* data, size_t bytes)
    {
        auto& w = Acquire(UniformWorkspaces, graph);
        if (!w.Uniform || w.Uniform->GetDesc().Bytes < bytes)
            w.Uniform = Device.CreateBuffer({bytes, BufferUsage::Uniform | BufferUsage::CopyDestination});
        std::vector<uint8_t> copy(bytes);
        std::memcpy(copy.data(), data, bytes);
        auto out = w.Uniform;
        graph.AddPass(
            "VFX render uniform upload",
            GraphPassKind::Transfer,
            {{graph.ImportBuffer("VFX render uniform", out), GraphAccess::TransferWrite}},
            [out, copy = std::move(copy)](auto& c) { c.Commands.UpdateBuffer(out, 0, copy.data(), copy.size()); },
            true);
        return out;
    }
    GpuEmitter::RibbonData GpuEmitter::BuildRibbon(RenderGraph&           graph,
                                                   const EmitterInstance& emitter,
                                                   const Renderer&        renderer,
                                                   const glm::mat4&       view,
                                                   const glm::mat4&       owner)
    {
        auto           indices = SortForView(graph, emitter, renderer, view, owner, true);
        const uint32_t cap = Compiled.Settings.Capacity, factor = renderer.TessellationFactor;
        auto&          w      = Acquire(RibbonWorkspaces, graph);
        const auto     ensure = [&](Handle<Buffer>& b, size_t bytes, BufferUsage usage) {
            if (!b || b->GetDesc().Bytes < bytes)
                b = Device.CreateBuffer({bytes, usage});
        };
        ensure(w.Output, size_t(cap) * factor * 6 * 96, BufferUsage::Storage | BufferUsage::CopySource);
        ensure(w.Dispatch, 32, BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::CopySource);
        ensure(w.Counts, size_t(cap) * 16, BufferUsage::Storage);
        ensure(w.Scan, size_t(cap) * 16, BufferUsage::Storage);
        struct Params
        {
            glm::mat4  Owner, InverseView;
            glm::uvec4 Offsets0, Offsets1, Config;
            glm::vec4  Curve;
            glm::vec4  UV[6];
        } params {};
        const auto offset = [&](const char* name) {
            auto b = renderer.Bindings.find(name);
            if (b != renderer.Bindings.end())
                for (const auto& a : Compiled.Attributes)
                    if (a.Name == b->second)
                        return a.Offset;
            return 0u;
        };
        params.Owner       = owner;
        params.InverseView = glm::inverse(view);
        params.Offsets0    = {offset("Position"), offset("RibbonWidth"), offset("RibbonTwist"), offset("RibbonFacing")};
        params.Offsets1    = {offset("Color"), offset("DynamicMaterialParameter"), offset("RibbonUVDistance"), cap};
        params.Config      = {offset("RibbonID"),
                              factor,
                              uint32_t(renderer.RibbonFacingMode),
                              uint32_t(Compiled.Settings.SimulationSpace == Space::Local)};
        params.Curve       = {renderer.CurveTension, float(renderer.Bindings.count("RibbonUVDistance")), 0, 0};
        for (uint32_t c = 0; c < 2; ++c)
        {
            const auto& uv       = renderer.UVChannels[c];
            params.UV[c * 3]     = {float(uv.Mode), uv.TileSize, uv.LeadingEdge, uv.TrailingEdge};
            params.UV[c * 3 + 1] = {uv.Offset, uv.Scale};
        }
        ensure(w.Uniform, sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDestination);
        auto uniform   = w.Uniform;
        auto uniformId = graph.ImportBuffer("VFX ribbon parameters", uniform);
        graph.AddPass(
            "VFX ribbon parameters",
            GraphPassKind::Transfer,
            {{uniformId, GraphAccess::TransferWrite}},
            [uniform, params](auto& c) { c.Commands.UpdateBuffer(uniform, 0, &params, sizeof(params)); },
            true);
        const std::string common = R"(
layout(set=0,binding=0,std430) readonly buffer Attributes{uvec4 a[];};
layout(set=0,binding=1,std430) readonly buffer Indices{uint indices[];};
layout(set=0,binding=2,std430) readonly buffer Count{uint count[];};
layout(set=0,binding=3,std140) uniform Params{mat4 owner;mat4 inverseView;uvec4 offsets0;uvec4 offsets1;uvec4 config;vec4 curve;vec4 uv[6];}p;
vec4 particle(uint attr,uint i){return uintBitsToFloat(a[attr*p.offsets1.w+indices[i]]);}
uint id(uint i){return a[p.config.x*p.offsets1.w+indices[i]].x;}
vec3 position(uint i){return particle(p.offsets0.x,i).xyz;}
struct Scan{uint start;uint end;uint segments;float distance;};
)";
        if (!RibbonDetect.Pipeline)
        {
            std::vector<BindGroupLayoutEntry> base {{0, BindingType::ReadOnlyStorageBuffer},
                                                    {1, BindingType::ReadOnlyStorageBuffer},
                                                    {2, BindingType::ReadOnlyStorageBuffer},
                                                    {3, BindingType::UniformBuffer},
                                                    {4, BindingType::StorageBuffer}};
            RibbonDetect = Make("#version 450\nlayout(local_size_x=64) in;\n" + common + R"(
layout(set=0,binding=4,std430) buffer Output{Scan outScan[];};
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.offsets1.w)return;
bool segment=i<count[1]&&i>0&&id(i)==id(i-1);
outScan[i]=Scan(i,i,uint(segment),segment?distance(position(i),position(i-1)):0);}
)",
                                base,
                                0);
            base[4].Type = BindingType::ReadOnlyStorageBuffer;
            base.push_back({5, BindingType::StorageBuffer});
            RibbonScan = Make("#version 450\nlayout(local_size_x=64) in;\n" + common + R"(
layout(set=0,binding=4,std430) readonly buffer Input{Scan inputScan[];};
layout(set=0,binding=5,std430) buffer Output{Scan outScan[];};
layout(push_constant) uniform Step{uint stride;}step;
void main(){uint i=gl_GlobalInvocationID.x;if(i>=p.offsets1.w)return;Scan v=inputScan[i];
if(i>=step.stride){Scan left=inputScan[i-step.stride];v.segments+=left.segments;
if(i<count[1]&&id(i)==id(i-step.stride)){v.start=left.start;v.distance+=left.distance;}}
if(i+step.stride<count[1]&&id(i)==id(i+step.stride))v.end=inputScan[i+step.stride].end;
outScan[i]=v;}
)",
                              base,
                              4);
            base.push_back({6, BindingType::StorageBuffer});
            RibbonVertices = Make("#version 450\nlayout(local_size_x=64) in;\n" + common + R"(
layout(set=0,binding=4,std430) readonly buffer Input{Scan scan[];};
layout(set=0,binding=5,std430) buffer Vertices{vec4 vertices[];};
layout(set=0,binding=6,std430) buffer Args{uint args[];};
vec3 safe(vec3 v,vec3 fallback){return dot(v,v)>1e-12?normalize(v):fallback;}
vec3 curve(vec3 a,vec3 b,vec3 c,vec3 d,float t){float t2=t*t,t3=t2*t;float k=(1-p.curve.x)*0.5;
return (2*t3-3*t2+1)*b+(t3-2*t2+t)*k*(c-a)+(-2*t3+3*t2)*c+(t3-t2)*k*(d-b);}
vec3 derivative(vec3 a,vec3 b,vec3 c,vec3 d,float t){float t2=t*t;float k=(1-p.curve.x)*0.5;
return (6*t2-6*t)*b+(3*t2-4*t+1)*k*(c-a)+(-6*t2+6*t)*c+(3*t2-2*t)*k*(d-b);}
float coordinate(uint channel,uint i,float t){uint j=i-1,start=scan[i].start,end=scan[i].end;
vec4 settings=p.uv[channel*3];int mode=int(settings.x);float total=scan[end].distance;
float dist=mix(scan[j].distance,scan[i].distance,t);
float u=(float(j-start)+t)/max(1.0,float(end-start));
if(mode==1)u=dist/settings.y;
if(mode==2)u=(p.curve.y>0.5?mix(particle(p.offsets1.z,j).x,particle(p.offsets1.z,i).x,t):dist)/settings.y;
if(mode==3)u=dist/max(total,1e-8);
return mix(settings.z,1-settings.w,u);}
void main(){uint n=count[1],v=gl_GlobalInvocationID.x,i=v/p.config.y,k=v%p.config.y;
if(v==0){uint segments=n>0?scan[n-1].segments:0;args[0]=segments*p.config.y*6;args[1]=1;args[2]=0;args[3]=0;args[4]=segments;}
if(i>=n||i==0||id(i)!=id(i-1))return;
uint j=i-1,start=scan[i].start,end=scan[i].end;
vec3 a=position(j>start?j-1:j),b=position(j),c=position(i),d=position(i<end?i+1:i);
uint corners[6]=uint[](0,2,3,0,3,1);
for(uint q=0;q<6;++q){uint corner=corners[q];float t=float(k+(corner>>1))/float(p.config.y);
vec3 pos=p.config.y==1?mix(b,c,t):curve(a,b,c,d,t);
vec3 tangent=safe(derivative(a,b,c,d,t),vec3(1,0,0));
vec3 facing=p.config.z==0?p.inverseView[3].xyz-pos:mix(particle(p.offsets0.w,j).xyz,particle(p.offsets0.w,i).xyz,t);
// Build the camera-facing frame in world space, then return the displacement to simulation space.
vec3 worldPos=pos,worldTangent=tangent;
if(p.config.w!=0){worldPos=(p.owner*vec4(pos,1)).xyz;worldTangent=safe(mat3(p.owner)*tangent,vec3(1,0,0));}
vec3 side;
if(p.config.z==0){vec3 normal=p.inverseView[3].xyz-worldPos;
side=safe(cross(normal,worldTangent),safe(cross(worldTangent,p.inverseView[1].xyz),p.inverseView[0].xyz));
if(p.config.w!=0)side=safe(inverse(mat3(p.owner))*side,vec3(0,1,0));}
else {vec3 perpendicular=safe(cross(vec3(0,0,1),tangent),vec3(1,0,0));
if(p.config.z==2)side=safe(facing-tangent*dot(facing,tangent),perpendicular);
else side=safe(cross(facing,tangent),perpendicular);}
float twist=mix(particle(p.offsets0.z,j).x,particle(p.offsets0.z,i).x,t);
side=side*cos(twist)+cross(tangent,side)*sin(twist);
float width=mix(particle(p.offsets0.y,j).x,particle(p.offsets0.y,i).x,t);
pos+=side*(float(corner&1u)-0.5)*abs(width);
vec3 normal=safe(cross(tangent,side),vec3(0,0,1));
if(p.config.w!=0){pos=(p.owner*vec4(pos,1)).xyz;normal=safe(transpose(inverse(mat3(p.owner)))*normal,vec3(0,0,1));tangent=worldTangent;}
float across=float(corner&1u);vec2 uv0=vec2(coordinate(0,i,t),across)*p.uv[1].zw+p.uv[1].xy;
vec2 uv1=vec2(coordinate(1,i,t),across)*p.uv[4].zw+p.uv[4].xy;
uint at=((scan[i].segments-1)*p.config.y*6+k*6+q)*6;
vertices[at]=vec4(pos,1);vertices[at+1]=vec4(normal,0);vertices[at+2]=vec4(uv0,uv1);
vertices[at+3]=mix(particle(p.offsets1.x,j),particle(p.offsets1.x,i),t);
vertices[at+4]=mix(particle(p.offsets1.y,j),particle(p.offsets1.y,i),t);
vertices[at+5]=vec4(tangent,1);}}
)",
                                  base,
                                  0);
        }
        auto           attrs = Attributes, count = DrawArgs, scan = w.Counts, scratch = w.Scan;
        auto           vertices = w.Output, args = w.Dispatch;
        auto           attrId    = graph.ImportBuffer("VFX attributes", attrs),
                       indexId   = graph.ImportBuffer("VFX ribbon order", indices);
        auto           countId   = graph.ImportBuffer("VFX draw indirect", count);
        auto           scanId    = graph.ImportBuffer("VFX ribbon scan", scan),
                       scratchId = graph.ImportBuffer("VFX ribbon scratch", scratch);
        const uint32_t groups    = (cap + 63) / 64;
        auto           kernel    = RibbonDetect;
        graph.AddPass(
            "VFX ribbon adjacent ID detection",
            GraphPassKind::Compute,
            {{attrId, GraphAccess::StorageRead},
             {indexId, GraphAccess::StorageRead},
             {countId, GraphAccess::StorageRead},
             {uniformId, GraphAccess::UniformRead},
             {scanId, GraphAccess::StorageWrite}},
            [kernel, attrs, indices, count, uniform, scan, groups](auto& c) {
                c.Commands.BindComputePipeline(kernel.Pipeline);
                c.Commands.BindGroup(Group(c.Device, kernel.Layout, {attrs, indices, count, uniform, scan}));
                c.Commands.Dispatch(groups);
            },
            true);
        kernel = RibbonScan;
        for (uint32_t stride = 1; stride < cap; stride *= 2)
        {
            graph.AddPass(
                "VFX ribbon segmented prefix and suffix scan",
                GraphPassKind::Compute,
                {{attrId, GraphAccess::StorageRead},
                 {indexId, GraphAccess::StorageRead},
                 {countId, GraphAccess::StorageRead},
                 {uniformId, GraphAccess::UniformRead},
                 {scanId, GraphAccess::StorageRead},
                 {scratchId, GraphAccess::StorageWrite}},
                [kernel, attrs, indices, count, uniform, scan, scratch, groups, stride](auto& c) {
                    c.Commands.BindComputePipeline(kernel.Pipeline);
                    c.Commands.BindGroup(
                        Group(c.Device, kernel.Layout, {attrs, indices, count, uniform, scan, scratch}));
                    c.Commands.PushConstants(0, &stride, 4);
                    c.Commands.Dispatch(groups);
                },
                true);
            std::swap(scan, scratch);
            std::swap(scanId, scratchId);
        }
        kernel = RibbonVertices;
        graph.AddPass(
            "VFX ribbon vertices and indirect count",
            GraphPassKind::Compute,
            {{attrId, GraphAccess::StorageRead},
             {indexId, GraphAccess::StorageRead},
             {countId, GraphAccess::StorageRead},
             {uniformId, GraphAccess::UniformRead},
             {scanId, GraphAccess::StorageRead},
             {graph.ImportBuffer("VFX ribbon vertices", vertices), GraphAccess::StorageWrite},
             {graph.ImportBuffer("VFX ribbon arguments", args), GraphAccess::StorageWrite}},
            [kernel, attrs, indices, count, uniform, scan, vertices, args, cap, factor](auto& c) {
                c.Commands.BindComputePipeline(kernel.Pipeline);
                c.Commands.BindGroup(
                    Group(c.Device, kernel.Layout, {attrs, indices, count, uniform, scan, vertices, args}));
                c.Commands.Dispatch((cap * factor + 63) / 64);
            },
            true);
        return {vertices, args};
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
