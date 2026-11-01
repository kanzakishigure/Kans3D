#include "kspch.h"
#include "VfxWorld.h"
#include <glm/gtx/matrix_decompose.hpp>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/Renderer/Resource/Skinning.h"
#include "Kans3D/Scene/Entity.h"
#include "VfxGpu.h"
namespace Kans::Vfx
{
    namespace
    {
        void Initialize(ParameterMap& p, const Program& program)
        {
            for (const auto& v : program.Parameters)
                if (!p.count(v.Name))
                    p[v.Name] = v.Default.value_or(Value::Vector({}, v.Kind));
        }
        void Run(const std::map<Stage, Program>& programs, Stage stage, ParameterMap& p, const ExecutionContext& c)
        {
            auto i = programs.find(stage);
            if (i != programs.end())
            {
                for (auto at = p.begin(); at != p.end();)
                    if (at->first.rfind("Transient.", 0) == 0)
                        at = p.erase(at);
                    else
                        ++at;
                Initialize(p, i->second);
                Execute(i->second, p, c);
            }
        }
        void EngineValues(ParameterMap&    p,
                          float            dt,
                          float            time,
                          const glm::mat4& owner,
                          glm::vec3        camera,
                          glm::vec3        velocity)
        {
            p["Engine.DeltaTime"]      = Value::Float(dt);
            p["Engine.Time"]           = Value::Float(time);
            p["Engine.Owner.Position"] = Value::Vector(glm::vec4(owner[3]), Type::Position);
            glm::vec3 scale, position, skew;
            glm::vec4 perspective;
            glm::quat q;
            glm::decompose(owner, scale, q, position, skew, perspective);
            p["Engine.Owner.Rotation"]  = Value::Vector({q.x, q.y, q.z, q.w}, Type::Quat);
            p["Engine.Owner.Scale"]     = Value::Vector(glm::vec4(scale, 0), Type::Vec3);
            p["Engine.Owner.Velocity"]  = Value::Vector(glm::vec4(velocity, 0), Type::Vec3);
            p["Engine.Camera.Position"] = Value::Vector(glm::vec4(camera, 0), Type::Position);
        }
        uint32_t AttributeIndex(const CompiledEmitter& e, const std::string& name)
        {
            for (uint32_t i = 0; i < e.Attributes.size(); ++i)
                if (e.Attributes[i].Name == name)
                    return i;
            throw std::invalid_argument("Unknown VFX attribute: " + name);
        }
    } // namespace
    EmitterInstance::EmitterInstance(const CompiledEmitter& e) : Compiled(&e)
    {
        if (e.Settings.Target == SimTarget::CPU)
        {
            for (const auto& a : e.Attributes)
                Attributes.emplace_back(e.Settings.Capacity, a.Default);
            for (uint32_t i = e.Settings.Capacity; i > 0; --i)
                Free.push_back(i - 1);
        }
        for (const auto& channel : e.Settings.Channels)
            Events[channel.Name] = {};
        for (const auto& [name, p] : BuiltinParameters())
            if (name.rfind("Emitter.", 0) == 0)
                Parameters[name] = *p.Default;
    }
    EmitterInstance::~EmitterInstance()                                     = default;
    EmitterInstance::EmitterInstance(EmitterInstance&&) noexcept            = default;
    EmitterInstance& EmitterInstance::operator=(EmitterInstance&&) noexcept = default;
    Value            EmitterInstance::Read(const std::string& n, uint32_t slot) const
    {
        return Attributes.at(AttributeIndex(*Compiled, n)).at(slot);
    }
    glm::vec3 EmitterInstance::WorldPosition(uint32_t slot, const glm::mat4& owner) const
    {
        auto p = glm::vec3(Read("Particles.Position", slot).Data);
        return Compiled->Settings.SimulationSpace == Space::Local ? glm::vec3(owner * glm::vec4(p, 1)) : p;
    }
    SystemInstance::SystemInstance(Ref<const VfxSystemAsset>      a,
                                   const ParticleSystemComponent& c,
                                   const glm::mat4&               initialOwner,
                                   MeshSourceResolver             meshSources) :
        m_MeshSources(std::move(meshSources)), m_Asset(std::move(a)),
        m_Seed(c.SeedOverride ? Hash(m_Asset->GetData().Settings.Seed ^ Hash(*c.SeedOverride)) :
                                m_Asset->GetData().Settings.Seed)
    {
        m_Owner     = initialOwner;
        FixedBounds = c.FixedBounds ? c.FixedBounds : m_Asset->GetData().Settings.FixedBounds;
        TimeScale   = c.TimeScale;
        if (!std::isfinite(TimeScale) || TimeScale < 0)
            throw std::invalid_argument("VFX time scale must be finite and nonnegative");
        for (const auto& p : m_Asset->GetData().Settings.UserParameters)
        {
            if (!p.Default)
                throw std::invalid_argument("User parameter needs default: " + p.Name);
            m_Parameters[p.Name] = *p.Default;
        }
        for (const auto& [name, value] : c.UserParameters)
            Set(name, value);
        Reset();
        if (c.AutoActivate)
            Activate(false);
    }
    SystemInstance::~SystemInstance() = default;
    void SystemInstance::Reset()
    {
        m_Emitters.clear();
        m_MeshStates.clear();
        m_Entropy = Hash(uint32_t(uint64_t(UUID {})));
        m_Time    = 0;
        for (auto i = m_Parameters.begin(); i != m_Parameters.end();)
            if (i->first.rfind("User.", 0) != 0)
                i = m_Parameters.erase(i);
            else
                ++i;
        for (const auto& [name, p] : BuiltinParameters())
            if (name.rfind("System.", 0) == 0)
                m_Parameters[name] = *p.Default;
        ExecutionContext c {m_Seed, 0, 0, m_Owner};
        EngineValues(m_Parameters, 0, 0, m_Owner, {}, {});
        Run(m_Asset->GetData().Programs, Stage::SystemSpawn, m_Parameters, c);
        for (const auto& e : m_Asset->GetData().Emitters)
        {
            m_Emitters.emplace_back(e);
            auto& instance = m_Emitters.back();
            c.Seed         = e.Settings.Deterministic ? m_Seed : Hash(m_Seed ^ m_Entropy);
            auto p         = m_Parameters;
            p.insert(instance.Parameters.begin(), instance.Parameters.end());
            Run(e.Programs, Stage::EmitterSpawn, p, c);
            for (const auto& [name, v] : p)
                if (name.rfind("Emitter.", 0) == 0)
                    instance.Parameters[name] = v;
            ++c.EmitterIndex;
        }
        for (auto& e : m_Emitters)
            e.Siblings = &m_Emitters;
    }
    void SystemInstance::Activate(bool reset)
    {
        if (reset)
            Reset();
        m_Spawning = true;
        m_Paused   = false;
        if (reset || m_Time == 0)
        {
            const auto& s     = m_Asset->GetData().Settings;
            uint32_t    ticks = uint32_t(std::floor(s.WarmupTime / s.WarmupTickDelta + 1e-5f));
            for (uint32_t n = 0; n < ticks; ++n)
                Step(s.WarmupTickDelta, m_Owner, {});
        }
    }
    void SystemInstance::DeactivateImmediate()
    {
        m_Spawning = false;
        Reset();
    }
    bool SystemInstance::IsComplete() const
    {
        if (m_Spawning && m_Parameters.at("System.ExecutionState").Integer < 2)
        {
            for (const auto& e : m_Emitters)
            {
                auto state = e.Parameters.find("Emitter.ExecutionState");
                if (state == e.Parameters.end() || state->second.Integer < 2)
                    return false;
            }
        }
        for (const auto& e : m_Emitters)
            if (e.ParticleCount || e.PendingBurst ||
                (e.Compiled->Settings.Target == SimTarget::GPU && e.SpawnRevision > 0 &&
                 (!e.GPU || !e.HasReadback || e.ReadbackSpawnRevision != e.SpawnRevision)) ||
                std::any_of(e.Pending.begin(), e.Pending.end(), [](const auto& tick) { return tick.SpawnCount > 0; }))
                return false;
        return true;
    }
    void SystemInstance::Set(const std::string& name, const Value& value)
    {
        const auto& p  = m_Asset->GetData().Settings.UserParameters;
        auto        at = std::find_if(p.begin(), p.end(), [&](const auto& x) { return x.Name == name; });
        if (at == p.end())
            throw std::invalid_argument("Unknown VFX user parameter: " + name);
        if (at->Kind != value.Kind)
            throw std::invalid_argument("VFX user parameter type mismatch: " + name);
        if (value.Kind != Type::Int && value.Kind != Type::Bool)
            for (uint32_t i = 0; i < Components(value.Kind); ++i)
                if (!std::isfinite(value.Data[i]))
                    throw std::invalid_argument("Non-finite VFX user parameter: " + name);
        m_Parameters[name] = value;
    }
    Value SystemInstance::Get(const std::string& name, Type type) const
    {
        auto at = m_Parameters.find(name);
        if (name.rfind("User.", 0) != 0 || at == m_Parameters.end())
            throw std::invalid_argument("Unknown VFX user parameter: " + name);
        if (at->second.Kind != type)
            throw std::invalid_argument("VFX user parameter type mismatch: " + name);
        return at->second;
    }
    void SystemInstance::Emit(const std::string& name, uint32_t count)
    {
        for (auto& e : m_Emitters)
            if (e.Compiled->Settings.Name == name)
            {
                e.PendingBurst = uint32_t(std::min(uint64_t(UINT32_MAX), uint64_t(e.PendingBurst) + count));
                return;
            }
        throw std::invalid_argument("Unknown VFX emitter: " + name);
    }
    uint32_t SystemInstance::ParticleCount() const
    {
        uint32_t n = 0;
        for (const auto& e : m_Emitters)
            n += e.ParticleCount;
        return n;
    }
    void SystemInstance::Tick(float delta, const glm::mat4& owner, glm::vec3 camera)
    {
        if (!std::isfinite(delta) || delta < 0)
            throw std::invalid_argument("Invalid VFX delta time");
        const float step = std::min(delta * TimeScale, m_Asset->GetData().Settings.MaxDeltaTime);
        if (!m_Paused && step > 0)
            Step(step, owner, camera);
        else
            m_Owner = owner;
    }
    void SystemInstance::PrepareMeshTables(ExecutionContext& context,
                                           float             dt,
                                           const glm::mat4&  owner,
                                           const glm::mat4&  previousOwner)
    {
        using Table = std::shared_ptr<const MeshSamplingSnapshot>;
        std::map<std::string, Table> tables;
        auto                         bindings = std::make_shared<std::map<const InterfaceBinding*, Table>>();
        auto                         prepare  = [&](const Program& program) {
            for (const auto& layout : program.Interfaces)
            {
                const auto& i = program.Code.at(layout.Instruction);
                if (!IsMeshOperation(i.Op))
                    continue;
                const auto& binding = i.Interface;
                auto        key     = MeshBindingKey(binding);
                if (auto found = tables.find(key); found != tables.end())
                {
                    (*bindings)[&binding] = found->second;
                    continue;
                }
                auto&              state = m_MeshStates[key];
                bool               asset = binding.Source.empty() || binding.Source == "Asset";
                MeshSamplingSource source;
                if (!asset && m_MeshSources)
                    source = m_MeshSources(binding);
                bool valid = asset ? bool(binding.Geometry) : source.Valid && bool(source.Mesh);
                if (asset && !state.Cached)
                {
                    state.Baked  = binding;
                    state.Cached = true;
                }
                else if (!asset && valid &&
                         (!state.Cached || state.Mesh != source.Mesh || state.Skeleton != source.Skeleton))
                {
                    state.Mesh     = source.Mesh;
                    state.Skeleton = source.Skeleton;
                    state.Baked    = binding;
                    try
                    {
                        if (binding.Kind == "SkeletalMesh")
                        {
                            if (!source.Skeleton || source.Mesh->GetData().Skeleton != source.Skeleton->assetID ||
                                (binding.Skeleton.IsValid() && binding.Skeleton != source.Skeleton->assetID))
                                throw std::invalid_argument("Source skeleton does not match compiled skeleton");
                            std::vector<std::string> names;
                            for (const auto& j : source.Skeleton->GetData().Joints)
                                names.push_back(j.Name);
                            if (!binding.SkeletonNames.empty() && binding.SkeletonNames != names)
                                throw std::invalid_argument("Source skeleton does not match compiled bone filter");
                        }
                        state.Baked = CachedMeshSampling(binding, source.Mesh, source.Skeleton);
                    }
                    catch (const std::exception&)
                    {
                        state.Baked.Geometry.reset();
                    }
                    state.Initialized = false;
                    state.Cached      = true;
                }
                valid           = valid && bool(state.Baked.Geometry);
                auto transform  = asset ? owner : source.Transform;
                auto previous   = state.Initialized ? state.PreviousTransform : asset ? previousOwner : transform;
                auto palette    = source.Palette;
                auto oldPalette = state.Initialized && state.PreviousPalette.size() == palette.size() ?
                                      state.PreviousPalette :
                                      palette;
                auto frame = MeshSamplingFrame(state.Baked, transform, previous, owner, dt, palette, oldPalette, valid);
                valid      = frame.Frame[2].w != 0;
                if (!valid && !state.Warned)
                {
                    std::string message = "VFX " + binding.Kind +
                                          " source unavailable or skeleton/filter mismatch: " + binding.EntityPath;
                    if (RuntimeWarning)
                        RuntimeWarning(message);
                    else if (Log::GetCoreLogger())
                        CORE_WARN("{}", message);
                    state.Warned = true;
                }
                tables[key]             = std::make_shared<const MeshSamplingSnapshot>(std::move(frame));
                (*bindings)[&binding]   = tables[key];
                state.PreviousTransform = transform;
                state.PreviousPalette   = std::move(palette);
                state.Initialized       = valid;
            }
        };
        for (const auto& [stage, program] : m_Asset->GetData().Programs)
            prepare(program);
        for (const auto& emitter : m_Asset->GetData().Emitters)
        {
            for (const auto& [stage, program] : emitter.Programs)
                prepare(program);
            for (const auto& handler : emitter.Handlers)
                prepare(handler.Bytecode);
        }
        context.MeshTable = [bindings](const InterfaceBinding& binding) -> Table {
            auto at = bindings->find(&binding);
            return at == bindings->end() ? nullptr : at->second;
        };
    }
    void SystemInstance::Step(float dt, const glm::mat4& owner, glm::vec3 camera)
    {
        if (dt <= 0)
            return;
        auto previousOwner = m_Owner;
        auto velocity      = (glm::vec3(owner[3]) - glm::vec3(m_Owner[3])) / dt;
        m_Owner            = owner;
        m_Time += dt;
        EngineValues(m_Parameters, dt, float(m_Time), owner, camera, velocity);
        m_Parameters["System.Age"] = Value::Float(float(m_Time));
        ExecutionContext context {m_Seed, 0, 0, owner};
        PrepareMeshTables(context, dt, owner, previousOwner);
        Run(m_Asset->GetData().Programs, Stage::SystemUpdate, m_Parameters, context);
        for (auto& e : m_Emitters)
        {
            e.EventsDropped = 0;
            for (auto& [name, events] : e.Events)
                events.clear();
        }
        for (auto emitterIndex : m_Asset->GetData().TickOrder)
        {
            auto& e              = m_Emitters[emitterIndex];
            context.EmitterIndex = emitterIndex;
            context.EventIndex   = 0;
            context.RandomID.reset();
            context.ReadParticle = [&](const Instruction& i, const std::vector<Value>& registers) {
                const auto at = std::find_if(m_Emitters.begin(), m_Emitters.end(), [&](const auto& source) {
                    return source.Compiled->Handle == i.Interface.SourceEmitter;
                });
                if (at == m_Emitters.end())
                    throw std::logic_error("Missing compiled particle-reader source");
                const auto& source = *at;
                if (i.Op == "ParticleCount")
                    return Value::Int(int32_t(source.Alive.size()));
                int32_t  index = registers.at(i.Args.at(0)).Integer;
                bool     found = index >= 0 && uint32_t(index) < source.Alive.size();
                uint32_t slot  = found ? source.Alive[index] : 0;
                if (i.Op == "ReadAttributeByID")
                {
                    found = false;
                    for (auto candidate : source.Alive)
                        if (source.Read("Particles.UniqueID", candidate).Integer == index)
                        {
                            slot  = candidate;
                            found = true;
                            break;
                        }
                }
                if (i.Output == "Found")
                    return Value::Bool(found);
                if (!found)
                    return Value::Vector({}, i.Kind);
                auto value = source.Read(i.Code, slot);
                if (value.Kind == Type::Position &&
                    source.Compiled->Settings.SimulationSpace != e.Compiled->Settings.SimulationSpace)
                {
                    auto transform = e.Compiled->Settings.SimulationSpace == Space::World ? owner : glm::inverse(owner);
                    value.Data     = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(value.Data), 1)), 0);
                }
                return value;
            };
            std::map<std::string, uint32_t> generatedEvents;
            context.WriteEvent = [&](const Instruction& i, const std::vector<Value>& registers) {
                const auto& channels = e.Compiled->Settings.Channels;
                auto        c        = std::find_if(
                    channels.begin(), channels.end(), [&](const auto& c) { return c.Name == i.Interface.Channel; });
                const auto& layout    = e.Compiled->EventLayouts.at(c->Layout);
                auto&       events    = e.Events.at(c->Name);
                uint32_t    requested = uint32_t(std::max(0, registers.at(i.Args.at(0)).Integer));
                uint32_t    limit     = i.Args.size() == layout.Fields.size() + 2 ?
                                            uint32_t(std::max(0, registers.at(i.Args.back()).Integer)) :
                                            UINT32_MAX;
                auto&       generated = generatedEvents[c->Name];
                uint32_t    bounded   = std::min(requested, limit > generated ? limit - generated : 0u);
                generated += bounded;
                uint32_t actual = std::min(bounded, c->Capacity - uint32_t(events.size()));
                e.EventsDropped += bounded - actual;
                ParameterMap payload;
                for (uint32_t f = 0; f < layout.Fields.size(); ++f)
                    payload["Transient.Event." + layout.Fields[f].Name] = registers.at(i.Args.at(f + 1));
                for (uint32_t n = 0; n < actual; ++n)
                {
                    payload["@EventIndex"] = Value::Int(int32_t(Hash(context.UniqueID ^ Hash(n ^ i.RandomKey))));
                    events.push_back(payload);
                }
            };
            const auto& compiled = *e.Compiled;
            const auto& settings = compiled.Settings;
            context.UniqueID     = 0;
            context.Seed         = settings.Deterministic ? m_Seed : Hash(m_Seed ^ m_Entropy);
            auto params          = m_Parameters;
            for (const auto& [name, v] : e.Parameters)
                params[name] = v;
            params["Emitter.SpawnCountThisFrame"] = Value::Int(0);
            auto emitterProgram                   = compiled.Programs.find(Stage::EmitterUpdate);
            if (emitterProgram == compiled.Programs.end() || emitterProgram->second.Target == SimTarget::CPU)
                Run(compiled.Programs, Stage::EmitterUpdate, params, context);
            for (const auto& [name, v] : params)
                if (name.rfind("Emitter.", 0) == 0)
                    e.Parameters[name] = v;
            bool distant =
                settings.CullDistance > 0 && glm::distance(glm::vec3(owner[3]), camera) > settings.CullDistance;
            e.HiddenByDistance = distant;
            bool     spawning  = m_Spawning && params["System.ExecutionState"].Integer == 0 &&
                                 params["Emitter.ExecutionState"].Integer == 0 &&
                                 (!distant || settings.DistancePolicy == CullPolicy::Continue);
            uint32_t count     = spawning ? uint32_t(std::max(0, params["Emitter.SpawnCountThisFrame"].Integer)) : 0;
            count              = uint32_t(std::min(uint64_t(UINT32_MAX), uint64_t(count) + e.PendingBurst));
            e.PendingBurst     = 0;
            if (settings.Target == SimTarget::GPU)
            {
                if (count || !compiled.Handlers.empty() ||
                    (emitterProgram != compiled.Programs.end() && emitterProgram->second.Target == SimTarget::GPU))
                    ++e.SpawnRevision;
                params["Transient.AllowSpawning"] = Value::Bool(spawning);
                if (e.GPU)
                    e.GPU->Poll(e);
                bool queued = std::any_of(
                    e.Pending.begin(), e.Pending.end(), [](const auto& tick) { return tick.SpawnCount > 0; });
                if (!compiled.Handlers.empty() || !compiled.EventLayouts.empty() ||
                    (emitterProgram != compiled.Programs.end() && emitterProgram->second.Target == SimTarget::GPU) ||
                    e.Siblings->size() > 1 || count || e.ParticleCount || queued ||
                    (e.GPU && e.GPU->HasPendingReadback() &&
                     (!e.HasReadback || e.ReadbackSpawnRevision != e.SpawnRevision)))
                {
                    auto gpuContext         = context;
                    gpuContext.ReadParticle = {};
                    gpuContext.WriteEvent   = {};
                    e.Pending.push_back({std::move(params), std::move(gpuContext), count});
                }
                ++context.EmitterIndex;
                continue;
            }
            uint32_t actual = std::min(count, uint32_t(e.Free.size()));
            e.Dropped += count - actual;
            e.TotalSpawned += actual;
            uint32_t spawnIndex = 0;
            auto     run        = [&](Stage stage, uint32_t slot) {
                auto p = params;
                for (size_t i = 0; i < compiled.Attributes.size(); ++i)
                    p[compiled.Attributes[i].Name] = e.Attributes[i][slot];
                context.UniqueID = uint32_t(p["Particles.UniqueID"].Integer);
                if (stage == Stage::ParticleSpawn)
                {
                    Initialize(p, compiled.Programs.at(stage));
                    p["Transient.SpawnIndex"] = Value::Int(int32_t(spawnIndex));
                    Execute(compiled.Programs.at(stage), p, context);
                }
                else
                    Run(compiled.Programs, stage, p, context);
                for (size_t i = 0; i < compiled.Attributes.size(); ++i)
                    e.Attributes[i][slot] = p[compiled.Attributes[i].Name];
            };
            for (uint32_t n = 0; n < actual; ++n)
            {
                auto slot = e.Free.back();
                e.Free.pop_back();
                for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                    e.Attributes[a][slot] = compiled.Attributes[a].Default;
                e.Attributes[AttributeIndex(compiled, "Particles.UniqueID")][slot] = Value::Int(int32_t(e.NextID++));
                if (settings.SimulationSpace == Space::World)
                    for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                        if (compiled.Attributes[a].Kind == Type::Position)
                            e.Attributes[a][slot].Data =
                                glm::vec4(glm::vec3(owner * glm::vec4(glm::vec3(e.Attributes[a][slot].Data), 1)), 0);
                spawnIndex = n;
                run(Stage::ParticleSpawn, slot);
                if (settings.SimulationSpace == Space::World)
                    for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                        if (compiled.Attributes[a].Name == "Particles.Velocity")
                            e.Attributes[a][slot].Data =
                                glm::vec4(glm::mat3(owner) * glm::vec3(e.Attributes[a][slot].Data), 0);
                e.Alive.push_back(slot);
            }
            e.DynamicBounds = Bounds {};
            e.SpriteRadius  = 0;
            size_t write    = 0;
            for (auto slot : e.Alive)
            {
                run(Stage::ParticleUpdate, slot);
                if (e.Read("Particles.Alive", slot).Integer && params["Emitter.ExecutionState"].Integer != 3 &&
                    params["System.ExecutionState"].Integer != 3)
                {
                    e.Alive[write++] = slot;
                    for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                        if (compiled.Attributes[a].Kind == Type::Position)
                            e.DynamicBounds.Expand(glm::vec3(e.Attributes[a][slot].Data));
                    for (const auto& renderer : compiled.Settings.Renderers)
                        if (renderer.Kind == RendererKind::Mesh)
                        {
                            auto radius = compiled.MeshRadii.find(renderer.Guid);
                            if (radius != compiled.MeshRadii.end())
                            {
                                auto scale = glm::abs(glm::vec3(e.Read(renderer.Bindings.at("Scale"), slot).Data));
                                e.SpriteRadius =
                                    std::max(e.SpriteRadius, radius->second * std::max({scale.x, scale.y, scale.z}));
                            }
                        }
                        else if (renderer.Kind == RendererKind::Ribbon)
                            e.SpriteRadius =
                                std::max(e.SpriteRadius,
                                         std::abs(e.Read(renderer.Bindings.at("RibbonWidth"), slot).Data.x) * .5f);
                        else if (renderer.Kind == RendererKind::Sprite)
                        {
                            auto size   = glm::vec2(e.Read(renderer.Bindings.at("SpriteSize"), slot).Data);
                            auto corner = glm::max(glm::abs(renderer.Pivot), glm::abs(glm::vec2(1) - renderer.Pivot));
                            e.SpriteRadius = std::max(e.SpriteRadius, glm::length(size * corner));
                        }
                }
                else
                    e.Free.push_back(slot);
            }
            e.Alive.resize(write);
            e.ParticleCount = uint32_t(write);
            for (const auto& handler : compiled.Handlers)
            {
                const auto& h      = handler.Settings;
                const auto& source = m_Emitters.at(handler.Source);
                const auto& events = source.Events.at(h.Channel);
                uint32_t    count  = std::min(uint32_t(events.size()), h.MaxEventsPerFrame);
                for (uint32_t eventIndex = 0; eventIndex < count; ++eventIndex)
                {
                    context.EventIndex = uint32_t(events[eventIndex].at("@EventIndex").Integer);
                    auto apply         = [&](uint32_t slot, uint32_t index) {
                        auto p = params;
                        for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                            p[compiled.Attributes[a].Name] = e.Attributes[a][slot];
                        for (auto [name, value] : events[eventIndex])
                        {
                            if (value.Kind == Type::Position &&
                                source.Compiled->Settings.SimulationSpace != settings.SimulationSpace)
                            {
                                auto transform = settings.SimulationSpace == Space::World ? owner : glm::inverse(owner);
                                value.Data = glm::vec4(glm::vec3(transform * glm::vec4(glm::vec3(value.Data), 1)), 0);
                            }
                            p[name] = value;
                        }
                        p["Transient.SpawnIndex"] = Value::Int(int32_t(index));
                        context.UniqueID          = uint32_t(p["Particles.UniqueID"].Integer);
                        if (h.Mode == EventMode::SpawnedParticles)
                            context.RandomID = index % std::max(1u, h.SpawnNumber);
                        else
                            context.RandomID.reset();
                        Initialize(p, handler.Bytecode);
                        Execute(handler.Bytecode, p, context);
                        for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                            e.Attributes[a][slot] = p[compiled.Attributes[a].Name];
                    };
                    if (h.Mode == EventMode::EveryParticle)
                    {
                        for (auto slot : e.Alive)
                            if (e.Read("Particles.Alive", slot).Integer)
                                apply(slot, 0);
                    }
                    else
                    {
                        uint32_t actual = std::min(h.SpawnNumber, uint32_t(e.Free.size()));
                        e.Dropped += h.SpawnNumber - actual;
                        e.TotalSpawned += actual;
                        for (uint32_t n = 0; n < actual; ++n)
                        {
                            auto slot = e.Free.back();
                            e.Free.pop_back();
                            for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                            {
                                auto value = compiled.Attributes[a].Default;
                                if (value.Kind == Type::Position && settings.SimulationSpace == Space::World)
                                    value.Data = glm::vec4(glm::vec3(owner * glm::vec4(glm::vec3(value.Data), 1)), 0);
                                e.Attributes[a][slot] = value;
                            }
                            e.Attributes[AttributeIndex(compiled, "Particles.UniqueID")][slot] =
                                Value::Int(int32_t(e.NextID++));
                            apply(slot, eventIndex * h.SpawnNumber + n);
                            e.Alive.push_back(slot);
                        }
                    }
                }
            }
            if (!compiled.Handlers.empty())
            {
                size_t write    = 0;
                e.DynamicBounds = Bounds {};
                e.SpriteRadius  = 0;
                for (auto slot : e.Alive)
                    if (e.Read("Particles.Alive", slot).Integer)
                    {
                        e.Alive[write++] = slot;
                        for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                            if (compiled.Attributes[a].Kind == Type::Position)
                                e.DynamicBounds.Expand(glm::vec3(e.Attributes[a][slot].Data));
                        for (const auto& renderer : settings.Renderers)
                            if (renderer.Kind == RendererKind::Mesh)
                            {
                                auto radius = compiled.MeshRadii.find(renderer.Guid);
                                if (radius != compiled.MeshRadii.end())
                                {
                                    auto scale = glm::abs(glm::vec3(e.Read(renderer.Bindings.at("Scale"), slot).Data));
                                    e.SpriteRadius = std::max(e.SpriteRadius,
                                                              radius->second * std::max({scale.x, scale.y, scale.z}));
                                }
                            }
                            else if (renderer.Kind == RendererKind::Ribbon)
                                e.SpriteRadius =
                                    std::max(e.SpriteRadius,
                                             std::abs(e.Read(renderer.Bindings.at("RibbonWidth"), slot).Data.x) * .5f);
                            else if (renderer.Kind == RendererKind::Sprite)
                            {
                                auto size = glm::vec2(e.Read(renderer.Bindings.at("SpriteSize"), slot).Data);
                                auto corner =
                                    glm::max(glm::abs(renderer.Pivot), glm::abs(glm::vec2(1) - renderer.Pivot));
                                e.SpriteRadius = std::max(e.SpriteRadius, glm::length(size * corner));
                            }
                    }
                    else
                        e.Free.push_back(slot);
                e.Alive.resize(write);
                e.ParticleCount = uint32_t(write);
            }
            ++context.EmitterIndex;
        }
    }
    SystemInstance* VfxWorld::Find(UUID id)
    {
        auto p = m_Instances.find(id);
        return p == m_Instances.end() ? nullptr : p->second.get();
    }
    void VfxWorld::Remove(UUID id)
    {
        m_Instances.erase(id);
        m_Configurations.erase(id);
        m_Overrides.erase(id);
        m_AutoDestroy.erase(id);
    }
    void VfxWorld::Clear()
    {
        m_Instances.clear();
        m_Configurations.clear();
        m_Playing = false;
    }
    void VfxWorld::Start()
    {
        Clear();
        m_Playing = true;
        Tick(0);
    }
    void VfxWorld::Reset(UUID id)
    {
        if (auto p = Find(id))
            p->Activate(true);
    }
    void VfxWorld::NotifyAssetChanged(AssetID id)
    {
        for (auto i = m_Instances.begin(); i != m_Instances.end();)
            if (i->second->Asset()->assetID == id)
            {
                m_Configurations.erase(i->first);
                i = m_Instances.erase(i);
            }
            else
                ++i;
    }
    bool VfxWorld::Bind(Entity entity, Ref<const VfxSystemAsset> asset)
    {
        if (!entity || !entity.HasComponent<ParticleSystemComponent>() || !asset ||
            asset->assetID != entity.GetComponent<ParticleSystemComponent>().System)
            return false;
        m_Overrides[entity.GetUUID()] = asset;
        m_Configurations.erase(entity.GetUUID());
        m_Instances.erase(entity.GetUUID());
        Tick(0, !m_Playing);
        return Find(entity.GetUUID()) != nullptr;
    }
    void VfxWorld::Tick(float delta, bool preview, glm::vec3 camera)
    {
        std::set<UUID>    live;
        std::vector<UUID> destroy;
        m_Scene.m_Registry.view<ParticleSystemComponent>().each([&](auto handle, const ParticleSystemComponent& c) {
            Entity entity {handle, &m_Scene};
            auto   id = entity.GetUUID();
            live.insert(id);
            if ((preview && !c.PreviewInEditMode) || !m_Scene.IsEntityActive(entity))
            {
                m_Instances.erase(id);
                m_Configurations.erase(id);
                return;
            }
            auto old = m_Configurations.find(id);
            if (old == m_Configurations.end() || old->second != c)
            {
                Ref<const VfxSystemAsset> source;
                auto                      over = m_Overrides.find(id);
                if (over != m_Overrides.end())
                    source = over->second;
                else if (auto manager = m_Scene.GetAssetManager())
                {
                    auto result = manager->Request<VfxSystemAsset>(c.System);
                    if (result.IsReady())
                        source = result.Asset;
                }
                if (!source)
                    return;
                auto meshSources = [this, entity](const InterfaceBinding& binding) {
                    MeshSamplingSource result;
                    Entity             bound = entity;
                    if (binding.Source == "Entity")
                    {
                        std::istringstream path(binding.EntityPath);
                        std::string        name;
                        while (std::getline(path, name, '/'))
                        {
                            Entity child;
                            if (name.empty() || name == "." || name == "..")
                                return result;
                            for (auto candidate : m_Scene.GetChildren(bound))
                                if (candidate.GetComponent<TagComponent>().Tag == name)
                                {
                                    child = candidate;
                                    break;
                                }
                            if (!child)
                                return result;
                            bound = child;
                        }
                    }
                    if (!bound || !bound.HasComponent<MeshComponent>())
                        return result;
                    auto manager = m_Scene.GetAssetManager();
                    if (!manager)
                        return result;
                    auto loaded = manager->Request<MeshAsset>(bound.GetComponent<MeshComponent>().Mesh);
                    if (!loaded.IsReady())
                        return result;
                    result.Mesh      = loaded.Asset;
                    result.Transform = m_Scene.GetWorldTransform(bound);
                    if (binding.Kind == "SkeletalMesh")
                    {
                        auto root = m_Scene.GetDeformationRoot(bound);
                        if (!root || !root.HasComponent<AnimatorComponent>())
                            return result;
                        const auto& animator = root.GetComponent<AnimatorComponent>();
                        result.Skeleton      = animator.SkeletonSource;
                        if (!result.Skeleton)
                        {
                            auto skeleton = manager->Request<SkeletonAsset>(animator.Skeleton);
                            if (skeleton.IsReady())
                                result.Skeleton = skeleton.Asset;
                        }
                        auto pose = m_Scene.GetSkinningPose(bound);
                        if (!pose || !result.Skeleton)
                            return result;
                        result.Palette = pose->Palette;
                        // Rendering palettes are model-space; the deformation root supplies the world transform.
                        result.Transform = m_Scene.GetWorldTransform(root);
                    }
                    result.Valid = true;
                    return result;
                };
                auto instance =
                    std::make_unique<SystemInstance>(source, c, m_Scene.GetWorldTransform(entity), meshSources);
                instance->AutoDestroy = m_AutoDestroy.count(id) != 0;
                m_Instances[id]       = std::move(instance);
                m_Configurations[id]  = c;
            }
            auto instance = Find(id);
            if (instance)
            {
                instance->Tick(delta, m_Scene.GetWorldTransform(entity), camera);
                if (!preview && instance->AutoDestroy && instance->IsComplete())
                    destroy.push_back(id);
            }
        });
        for (auto i = m_Instances.begin(); i != m_Instances.end();)
            if (!live.count(i->first))
            {
                m_Configurations.erase(i->first);
                m_Overrides.erase(i->first);
                i = m_Instances.erase(i);
            }
            else
                ++i;
        for (auto id : destroy)
            m_Scene.DestroyEntity(m_Scene.GetEntityByUUID(id));
    }
    Entity VfxWorld::Spawn(AssetID asset, glm::vec3 position, glm::quat rotation, bool autoDestroy)
    {
        auto manager = m_Scene.GetAssetManager();
        auto record  = manager ? manager->GetRegistry()->Find(asset) : std::optional<ProductRecord> {};
        if (!record || record->Metadata.Type != AssetType::VfxSystem)
            throw std::invalid_argument("VFX system asset is unavailable: " + asset.ToString());
        auto                    e = m_Scene.CreateEntity("VFX");
        ParticleSystemComponent c;
        c.System = asset;
        e.AddComponent<ParticleSystemComponent>(c);
        auto m = glm::translate(glm::mat4(1), position) * glm::mat4_cast(rotation);
        m_Scene.SetWorldTransform(e, m);
        if (autoDestroy)
            m_AutoDestroy.insert(e.GetUUID());
        Tick(0);
        if (auto i = Find(e.GetUUID()))
            i->AutoDestroy = autoDestroy;
        return e;
    }
} // namespace Kans::Vfx
namespace Kans
{
    YAML::Node EncodeParticleSystem(const ParticleSystemComponent& c)
    {
        if (!std::isfinite(c.TimeScale) || c.TimeScale < 0)
            throw std::invalid_argument("Invalid particle time scale");
        YAML::Node n;
        n["System"]            = c.System.ToString();
        n["AutoActivate"]      = c.AutoActivate;
        n["PreviewInEditMode"] = c.PreviewInEditMode;
        if (c.SeedOverride)
            n["SeedOverride"] = *c.SeedOverride;
        n["UserParameters"] = Vfx::EncodeParameters(c.UserParameters);
        n["TimeScale"]      = c.TimeScale;
        if (c.FixedBounds)
        {
            if (!c.FixedBounds->IsValid())
                throw std::invalid_argument("Invalid particle bounds");
            for (int k = 0; k < 3; ++k)
            {
                n["FixedBounds"]["Min"].push_back(c.FixedBounds->Min[k]);
                n["FixedBounds"]["Max"].push_back(c.FixedBounds->Max[k]);
            }
        }
        return n;
    }
    ParticleSystemComponent DecodeParticleSystem(const YAML::Node& n)
    {
        ParticleSystemComponent c;
        c.System            = AssetID::FromString(n["System"].as<std::string>());
        c.AutoActivate      = n["AutoActivate"].as<bool>(true);
        c.PreviewInEditMode = n["PreviewInEditMode"].as<bool>(true);
        if (n["SeedOverride"])
            c.SeedOverride = n["SeedOverride"].as<uint32_t>();
        c.UserParameters = Vfx::DecodeParameters(n["UserParameters"]);
        c.TimeScale      = n["TimeScale"].as<float>(1);
        if (n["FixedBounds"])
        {
            Bounds b;
            for (int k = 0; k < 3; ++k)
            {
                b.Min[k] = n["FixedBounds"]["Min"][k].as<float>();
                b.Max[k] = n["FixedBounds"]["Max"][k].as<float>();
            }
            c.FixedBounds = b;
        }
        EncodeParticleSystem(c);
        return c;
    }
} // namespace Kans
