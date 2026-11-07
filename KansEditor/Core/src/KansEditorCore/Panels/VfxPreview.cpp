#include "KansEditorCore/Panels/VfxPreview.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Kans3D/Physics/PhysicsComponents.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/VFX/VfxGpu.h"

namespace Kans::VfxPreview
{
    namespace RH = RenderHardware;

    AssetID PreviewAssetID() { return AssetID::FromString("5f7e000000000001:0000000000000001"); }

    namespace
    {
        // One blocking copy of a device buffer (on demand only: captures and grid slices).
        std::vector<uint8_t> ReadBack(RH::Device& device, const RH::Handle<RH::Buffer>& buffer)
        {
            if (!buffer)
                return {};
            const size_t bytes = buffer->GetDesc().Bytes;
            auto         out   = device.CreateBuffer({bytes, RH::BufferUsage::CopyDestination, RH::MemoryUsage::Readback});
            auto         c     = device.CreateCommandList();
            c->CopyBuffer(buffer, 0, out, 0, bytes);
            device.Submit(c);
            device.WaitIdle();
            return out->Read(0, bytes);
        }
        std::vector<glm::vec4> GpuGrid(Vfx::EmitterInstance& e, const std::string& grid)
        {
            if (!e.GPU)
                return {};
            auto at = e.GPU->Grids.find(grid);
            if (at == e.GPU->Grids.end())
                return {};
            auto                   bytes = ReadBack(e.GPU->Device, at->second.Values[at->second.Read]);
            std::vector<glm::vec4> words(bytes.size() / sizeof(glm::vec4));
            std::memcpy(words.data(), bytes.data(), words.size() * sizeof(glm::vec4));
            return words;
        }
        glm::vec3 Position(const Vfx::Value& v) { return glm::vec3(v.Data); }
        void      Box(std::vector<DebugLine>& out, const Bounds& b, uint32_t color)
        {
            if (!b.IsValid())
                return;
            for (int i = 0; i < 12; ++i)
            {
                const int axis = i / 4, k = i % 4;
                glm::vec3 a = b.Min, c = b.Min;
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                a[u] = c[u] = (k & 1) ? b.Max[u] : b.Min[u];
                a[v] = c[v] = (k & 2) ? b.Max[v] : b.Min[v];
                a[axis] = b.Min[axis];
                c[axis] = b.Max[axis];
                out.push_back({a, c, color});
            }
        }
        int AttributeIndex(const Vfx::CompiledEmitter& e, const std::string& name)
        {
            for (size_t i = 0; i < e.Attributes.size(); ++i)
                if (e.Attributes[i].Name == name)
                    return int(i);
            return -1;
        }
    } // namespace

    // ---------------------------------------------------------------- spreadsheet

    std::optional<Vfx::Value> Capture::Get(size_t row, const std::string& column) const
    {
        auto at = std::find(Columns.begin(), Columns.end(), column);
        if (at == Columns.end() || row >= Rows.size())
            return std::nullopt;
        return Rows[row][size_t(at - Columns.begin())];
    }

    uint32_t ColumnComponents(Vfx::Type t)
    {
        return t == Vfx::Type::Int || t == Vfx::Type::Bool ? 1u : Vfx::Components(t);
    }

    float ComponentOf(const Vfx::Value& v, uint32_t component)
    {
        if (v.Kind == Vfx::Type::Int || v.Kind == Vfx::Type::Bool)
            return float(v.Integer);
        return v.Data[int(std::min(component, 3u))];
    }

    std::vector<size_t> SortRows(const Capture& c, size_t column, uint32_t component, bool ascending)
    {
        std::vector<size_t> order(c.Rows.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        if (column >= c.Columns.size())
            return order;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const float x = ComponentOf(c.Rows[a][column], component), y = ComponentOf(c.Rows[b][column], component);
            return ascending ? x < y : x > y;
        });
        return order;
    }

    std::vector<size_t> FilterRows(const Capture& c, const std::vector<size_t>& rows, size_t column, uint32_t component,
                                   float min, float max)
    {
        std::vector<size_t> kept;
        for (auto r : rows)
            if (column < c.Columns.size() && r < c.Rows.size())
            {
                const float x = ComponentOf(c.Rows[r][column], component);
                if (x >= min && x <= max)
                    kept.push_back(r);
            }
        return kept;
    }

    // ---------------------------------------------------------------- grids

    GridSlice ExtractSlice(const std::vector<glm::vec4>& words, const Vfx::InterfaceBinding& grid, const GridView& view)
    {
        GridSlice s;
        s.View        = view;
        s.Resolution  = grid.Resolution;
        s.WorldBounds = grid.WorldBounds;
        int attribute = -1;
        for (size_t i = 0; i < grid.GridAttributes.size(); ++i)
            if (grid.GridAttributes[i].Name == view.Attribute)
                attribute = int(i);
        const int axis = std::clamp(view.Axis, 0, 2), u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
        const glm::uvec3 r = grid.Resolution;
        const size_t cells = size_t(r.x) * r.y * r.z;
        if (attribute < 0 || words.size() < cells * (size_t(attribute) + 1))
            return s;
        s.View.Index = std::clamp(view.Index, 0, int(r[axis]) - 1);
        s.Size       = {r[u], r[v]};
        s.Values.resize(size_t(s.Size.x) * s.Size.y);
        s.Min = FLT_MAX;
        s.Max = -FLT_MAX;
        for (uint32_t b = 0; b < s.Size.y; ++b)
            for (uint32_t a = 0; a < s.Size.x; ++a)
            {
                glm::uvec3 p;
                p[axis]           = uint32_t(s.View.Index);
                p[u]              = a;
                p[v]              = b;
                const size_t word = size_t(attribute) * cells + p.x + size_t(r.x) * (p.y + size_t(r.y) * p.z);
                const float  x    = words[word][int(std::min(view.Component, 3u))];
                s.Values[a + s.Size.x * b] = x;
                s.Min                      = std::min(s.Min, x);
                s.Max                      = std::max(s.Max, x);
            }
        return s;
    }

    std::optional<double> VfxGpuMilliseconds(const FrameStatistics& stats)
    {
        if (!stats.Gpu)
            return std::nullopt;
        double total = 0;
        for (const auto& pass : stats.Gpu->Passes)
            if (pass.Name.rfind("VFX", 0) == 0)
                total += pass.EndMilliseconds - pass.BeginMilliseconds;
        return total;
    }

    std::optional<double> SystemLifetime(const Vfx::CompiledSystem& system)
    {
        if (system.Emitters.empty())
            return std::nullopt;
        double     longest = 0;
        const auto state   = Vfx::BuiltinModuleID("EmitterState");
        for (const auto& e : system.Emitters)
        {
            const auto stage = e.Settings.Stages.find(Vfx::Stage::EmitterUpdate);
            if (stage == e.Settings.Stages.end())
                return std::nullopt;
            auto use = std::find_if(stage->second.begin(), stage->second.end(),
                                    [&](const auto& u) { return u.Enabled && u.Script == state; });
            if (use == stage->second.end())
                return std::nullopt;
            // Literal inputs or the module's defaults (5 s, no delay, loop forever).
            auto literal = [&](const char* name, Vfx::Value fallback) -> std::optional<Vfx::Value> {
                auto at = use->Inputs.find(name);
                if (at == use->Inputs.end())
                    return fallback;
                if (at->second.Kind != Vfx::InputKind::Literal)
                    return std::nullopt;
                return at->second.Literal;
            };
            auto duration = literal("Module.Duration", Vfx::Value::Float(5));
            auto delay    = literal("Module.Delay", Vfx::Value::Float(0));
            auto loops    = literal("Module.LoopCount", Vfx::Value::Int(0));
            if (!duration || !delay || !loops || loops->Integer <= 0)
                return std::nullopt;
            longest = std::max(longest, double(delay->Data.x) + double(duration->Data.x) * loops->Integer);
        }
        return longest > 0 ? std::optional<double>(longest) : std::nullopt;
    }

    // ---------------------------------------------------------------- session

    Session::Session() : m_Scene(CreateRef<Scene>("VFX preview")), m_GridReader(GpuGrid)
    {
        m_Entity = m_Scene->CreateEntity("Effect");
        ParticleSystemComponent c;
        c.System = PreviewAssetID();
        m_Entity.AddComponent<ParticleSystemComponent>(c);
    }

    Session::~Session() = default;

    Vfx::SystemInstance* Session::Instance() const { return m_Scene->GetVfxWorld().Find(m_Entity.GetUUID()); }

    const Vfx::CompiledSystem* Session::Product() const { return m_Asset ? &m_Asset->GetData() : nullptr; }

    Vfx::EmitterInstance* Session::Find(const std::string& handle) const
    {
        auto* instance = Instance();
        if (!instance)
            return nullptr;
        for (auto& e : instance->Emitters())
            if (e.Compiled->Handle == handle)
                return &e;
        return nullptr;
    }

    bool Session::SetProduct(Vfx::CompiledSystem product)
    {
        auto asset     = CreateRef<VfxSystemAsset>(std::move(product));
        asset->assetID = PreviewAssetID();
        const auto& component = m_Entity.GetComponent<ParticleSystemComponent>();
        try
        {
            // Probe first: a product that cannot start must not replace the running preview.
            Vfx::SystemInstance probe(asset, component);
        }
        catch (const std::exception& e)
        {
            SetOutdated(e.what());
            return false;
        }
        auto& world = m_Scene->GetVfxWorld();
        bool  bound = false;
        try
        {
            bound = world.Bind(m_Entity, asset);
        }
        catch (const std::exception&)
        {
        }
        if (!bound)
        {
            if (m_Asset)
                world.Bind(m_Entity, m_Asset);
            SetOutdated("The preview could not start the new product");
            return false;
        }
        m_Asset    = asset;
        m_Outdated = false;
        m_Reason.clear();
        ++m_Builds;
        if (!m_DurationSet)
            m_Duration = SystemLifetime(asset->GetData()).value_or(5.0);
        m_SpawnBefore.assign(asset->GetData().Emitters.size(), 0);
        m_SpawnedThisFrame.assign(asset->GetData().Emitters.size(), 0);
        if (m_Capture && !Find(m_Capture->Emitter))
            m_Capture.reset();
        ApplyOverrides();
        // Replay to the time the preview showed, deterministically from the start.
        const auto target = m_Steps;
        m_Steps           = 0;
        Refresh();
        while (m_Steps < target)
            StepOnce();
        m_SliceDue = true;
        return true;
    }

    void Session::SetOutdated(std::string reason)
    {
        m_Outdated = true;
        m_Reason   = std::move(reason);
    }

    void Session::ApplyOverrides()
    {
        auto* instance = Instance();
        if (!instance)
            return;
        for (auto at = m_UserOverrides.begin(); at != m_UserOverrides.end();)
            try
            {
                instance->Set(at->first, at->second);
                ++at;
            }
            catch (const std::exception&)
            {
                at = m_UserOverrides.erase(at); // Renamed or retyped in the system: dropped.
            }
    }

    void Session::Refresh() { m_Scene->GetVfxWorld().Tick(0, true, Camera); }

    void Session::StepOnce()
    {
        if (auto* instance = Instance())
            instance->Tick(m_FixedStep, glm::mat4(1), Camera);
        ++m_Steps;
    }

    void Session::BeginFrameSpawns()
    {
        if (auto* instance = Instance())
            for (size_t i = 0; i < instance->Emitters().size() && i < m_SpawnBefore.size(); ++i)
                m_SpawnBefore[i] = instance->Emitters()[i].TotalSpawned;
    }

    void Session::EndFrameSpawns()
    {
        if (auto* instance = Instance())
            for (size_t i = 0; i < instance->Emitters().size() && i < m_SpawnBefore.size(); ++i)
            {
                const auto now = instance->Emitters()[i].TotalSpawned;
                m_SpawnedThisFrame[i] = now >= m_SpawnBefore[i] ? now - m_SpawnBefore[i] : now;
            }
    }

    void Session::Play()
    {
        if (Time() + 1e-9 >= m_Duration)
            Restart();
        m_Playing = true;
    }

    void Session::Pause()
    {
        m_Playing = false;
        m_Pending = 0;
    }

    void Session::StepFrame()
    {
        Pause();
        Refresh();
        BeginFrameSpawns();
        StepOnce();
        EndFrameSpawns();
        if (AutoCapture && !CaptureHandle.empty())
            CaptureEmitter(CaptureHandle, CaptureLimit);
    }

    void Session::Restart()
    {
        if (auto* instance = Instance())
            instance->Activate(true);
        m_Steps   = 0;
        m_Pending = 0;
    }

    void Session::Seek(double seconds)
    {
        seconds           = std::clamp(seconds, 0.0, m_Duration);
        // Whole steps; the tolerance absorbs a float step that is not exact (1/60 in float).
        const auto target = uint64_t(std::floor(seconds / double(m_FixedStep) + 1e-3));
        if (target < m_Steps)
            Restart();
        Refresh();
        while (m_Steps < target)
            StepOnce();
        m_Pending = 0;
    }

    void Session::Advance(double realSeconds)
    {
        if (!m_Playing || !(realSeconds > 0))
            return;
        m_Pending += realSeconds * double(m_Speed) / double(m_FixedStep);
        int steps = int(std::floor(m_Pending + 1e-6));
        m_Pending = std::max(0.0, m_Pending - steps);
        if (steps > MaxStepsPerFrame)
        {
            steps     = MaxStepsPerFrame;
            m_Pending = 0;
        }
        if (steps == 0)
            return;
        Refresh();
        BeginFrameSpawns();
        for (int n = 0; n < steps; ++n)
        {
            if (Time() + double(m_FixedStep) > m_Duration + 1e-6)
            {
                if (!Loop)
                {
                    Pause();
                    break;
                }
                Restart();
                BeginFrameSpawns();
            }
            StepOnce();
        }
        EndFrameSpawns();
    }

    void Session::SetSpeed(float speed) { m_Speed = std::clamp(std::isfinite(speed) ? speed : 1.0f, 0.1f, 4.0f); }

    void Session::SetFixedStep(float seconds)
    {
        if (!std::isfinite(seconds))
            return;
        const double time = Time();
        m_FixedStep       = std::clamp(seconds, 0.001f, 0.1f);
        Restart();
        Seek(time);
    }

    void Session::SetDuration(double seconds)
    {
        if (std::isfinite(seconds) && seconds > 0)
        {
            m_Duration    = seconds;
            m_DurationSet = true;
            if (Time() > m_Duration)
                Seek(m_Duration);
        }
    }

    bool Session::SetUserParameter(const std::string& name, const Vfx::Value& value)
    {
        if (auto* instance = Instance())
            try
            {
                instance->Set(name, value);
            }
            catch (const std::exception&)
            {
                return false;
            }
        m_UserOverrides[name] = value;
        return true;
    }

    void Session::ClearUserParameter(const std::string& name)
    {
        m_UserOverrides.erase(name);
        if (!m_Asset)
            return;
        for (const auto& p : m_Asset->GetData().Settings.UserParameters)
            if (p.Name == name && p.Default)
                if (auto* instance = Instance())
                    instance->Set(name, *p.Default);
    }

    bool Session::Emit(const std::string& handle, uint32_t count)
    {
        auto* e = Find(handle);
        if (!e)
            return false;
        Instance()->Emit(e->Compiled->Settings.Name, count);
        return true;
    }

    void Session::SetMuted(const std::string& handle, bool muted)
    {
        if (muted ? m_Muted.insert(handle).second : m_Muted.erase(handle) != 0)
            ++m_MaskRevision;
    }

    void Session::SetSolo(const std::string& handle)
    {
        if (m_Solo != handle)
        {
            m_Solo = handle;
            ++m_MaskRevision;
        }
    }

    Vfx::System Session::Masked(const Vfx::System& s) const
    {
        auto r = s;
        for (auto& h : r.Emitters)
            if ((!m_Solo.empty() && h.Guid != m_Solo) || m_Muted.count(h.Guid))
                h.Enabled = false;
        return r;
    }

    std::vector<EmitterStats> Session::Stats() const
    {
        std::vector<EmitterStats> r;
        auto*                     instance = Instance();
        if (!instance)
            return r;
        for (size_t i = 0; i < instance->Emitters().size(); ++i)
        {
            const auto&  e        = instance->Emitters()[i];
            const auto&  settings = e.Compiled->Settings;
            EmitterStats s;
            s.Failure                 = e.Failure;
            s.Handle           = e.Compiled->Handle;
            s.Name             = settings.Name;
            s.Gpu              = settings.Target == Vfx::SimTarget::GPU;
            s.Count            = e.ParticleCount;
            s.Capacity         = settings.Capacity;
            s.SpawnedThisFrame = i < m_SpawnedThisFrame.size() ? m_SpawnedThisFrame[i] : 0;
            s.CpuMilliseconds  = e.TickMilliseconds;
            const uint64_t attributes = e.Compiled->Attributes.size();
            s.MemoryBytes = s.Gpu ? (e.GPU && e.GPU->Attributes ? e.GPU->Attributes->GetDesc().Bytes :
                                                                  uint64_t(settings.Capacity) * attributes * 16) :
                                    uint64_t(settings.Capacity) * attributes * sizeof(Vfx::Value);
            r.push_back(std::move(s));
        }
        return r;
    }

    Capture Session::CaptureEmitter(const std::string& handle, size_t limit)
    {
        Capture c;
        c.Emitter = handle;
        c.Time    = Time();
        auto* e   = Find(handle);
        if (!e)
        {
            c.Error = "The emitter is not running in the preview";
            m_Capture = c;
            return c;
        }
        const auto& compiled = *e->Compiled;
        for (const auto& a : compiled.Attributes)
        {
            c.Columns.push_back(a.Name);
            c.Kinds.push_back(a.Kind);
        }
        ++m_Readbacks;
        if (compiled.Settings.Target == Vfx::SimTarget::CPU)
        {
            c.Alive = uint32_t(e->Alive.size());
            for (size_t n = 0; n < e->Alive.size() && n < limit; ++n)
            {
                std::vector<Vfx::Value> row;
                for (size_t a = 0; a < compiled.Attributes.size(); ++a)
                    row.push_back(e->Attributes[a][e->Alive[n]]);
                c.Rows.push_back(std::move(row));
            }
        }
        else if (!e->GPU || !e->GPU->Attributes)
            c.Error = "The GPU emitter has not simulated yet";
        else
        {
            auto& device   = e->GPU->Device;
            auto  counters = ReadBack(device, e->GPU->Counters);
            auto  alive    = ReadBack(device, e->GPU->Alive);
            auto  words    = ReadBack(device, e->GPU->Attributes);
            uint32_t count = 0;
            if (counters.size() >= 8)
                std::memcpy(&count, counters.data() + 4, 4);
            count                   = std::min<uint32_t>(count, uint32_t(alive.size() / 4));
            c.Alive                 = count;
            const uint32_t capacity = compiled.Settings.Capacity;
            for (uint32_t n = 0; n < count && n < limit; ++n)
            {
                uint32_t slot = 0;
                std::memcpy(&slot, alive.data() + size_t(n) * 4, 4);
                std::vector<Vfx::Value> row;
                for (const auto& a : compiled.Attributes)
                {
                    glm::uvec4   word {0};
                    const size_t at = (size_t(a.Offset) * capacity + slot) * sizeof(glm::uvec4);
                    if (at + sizeof(word) <= words.size())
                        std::memcpy(&word, words.data() + at, sizeof(word));
                    row.push_back(Vfx::Unpack(word, a.Kind));
                }
                c.Rows.push_back(std::move(row));
            }
        }
        m_Capture = c;
        return c;
    }

    void Session::SetGridView(std::optional<GridView> view)
    {
        if (view != m_GridView)
        {
            m_GridView = std::move(view);
            m_SliceDue = true;
            if (!m_GridView)
                m_Slice.reset();
        }
    }

    std::vector<std::string> Session::Grids(const std::string& handle) const
    {
        std::vector<std::string> r;
        if (auto* e = Find(handle))
            for (const auto& [name, binding] : e->Compiled->Settings.DataInterfaces)
                if (binding.Kind == "Grid3D")
                    r.push_back(name);
        return r;
    }

    void Session::UpdateSlice(double now)
    {
        if (!m_GridView)
            return;
        const bool dataChanged = m_Steps != m_SliceSteps || m_Builds != m_SliceBuild;
        const bool due = m_SliceDue || (dataChanged && (!m_Playing || now - m_SliceTime >= SliceInterval));
        if (!due)
            return;
        m_SliceDue   = false;
        m_SliceSteps = m_Steps;
        m_SliceBuild = m_Builds;
        m_SliceTime  = now;
        auto* e      = Find(m_GridView->Emitter);
        if (!e)
            return;
        auto binding = e->Compiled->Settings.DataInterfaces.find(m_GridView->Grid);
        if (binding == e->Compiled->Settings.DataInterfaces.end() || binding->second.Kind != "Grid3D")
            return;
        ++m_Readbacks;
        m_Slice = ExtractSlice(m_GridReader(*e, m_GridView->Grid), binding->second, *m_GridView);
    }

    std::vector<DebugLine> Session::DebugLines(const DebugOptions& o) const
    {
        std::vector<DebugLine> out;
        auto*                  instance = Instance();
        if (!instance)
            return out;
        const glm::mat4 owner = instance->OwnerTransform();
        if (o.Origins)
        {
            const glm::vec3 p(owner[3]);
            out.push_back({p, p + glm::vec3(owner[0]) * 0.25f, 0xff3c3cff});
            out.push_back({p, p + glm::vec3(owner[1]) * 0.25f, 0xff3cff3c});
            out.push_back({p, p + glm::vec3(owner[2]) * 0.25f, 0xffff3c3c});
        }
        if (o.Bounds && instance->FixedBounds)
            Box(out, instance->FixedBounds->Transformed(owner), 0xff40d0ff);
        for (const auto& e : instance->Emitters())
        {
            const auto& settings = e.Compiled->Settings;
            const bool  local    = settings.SimulationSpace == Vfx::Space::Local;
            auto        world    = [&](glm::vec3 p) { return local ? glm::vec3(owner * glm::vec4(p, 1)) : p; };
            auto        vector   = [&](glm::vec3 v) { return local ? glm::mat3(owner) * v : v; };
            if (o.Bounds && !instance->FixedBounds)
            {
                Bounds b = settings.FixedBounds.value_or(e.DynamicBounds);
                Box(out, local ? b.Transformed(owner) : b, settings.FixedBounds ? 0xff40d0ff : 0xffd0d040);
            }
            // Particle data: live for CPU emitters, the last capture for GPU ones (no readback here).
            std::vector<std::pair<glm::vec3, std::pair<glm::vec3, bool>>> contacts;
            const int position = AttributeIndex(*e.Compiled, "Particles.Position");
            const int velocity = AttributeIndex(*e.Compiled, "Particles.Velocity");
            const int hit = AttributeIndex(*e.Compiled, "Particles.HasCollided"),
                      at = AttributeIndex(*e.Compiled, "Particles.CollisionPosition"),
                      normal = AttributeIndex(*e.Compiled, "Particles.CollisionNormal");
            auto particle = [&](auto read) {
                if (o.Velocity && position >= 0 && velocity >= 0)
                {
                    const glm::vec3 p = world(Position(read(position)));
                    out.push_back({p, p + vector(glm::vec3(read(velocity).Data)) * o.VelocityScale, 0xff40a0ff});
                }
                if (o.Contacts && hit >= 0 && at >= 0 && normal >= 0 && read(hit).Integer)
                {
                    const glm::vec3 p = world(Position(read(at)));
                    out.push_back({p, p + vector(glm::vec3(read(normal).Data)) * 0.15f, 0xffff40ff});
                }
            };
            if (settings.Target == Vfx::SimTarget::CPU)
            {
                for (auto slot : e.Alive)
                    particle([&](int a) { return e.Attributes[size_t(a)][slot]; });
            }
            else if (m_Capture && m_Capture->Emitter == e.Compiled->Handle && m_Capture->Columns.size() == e.Compiled->Attributes.size())
                for (const auto& row : m_Capture->Rows)
                    particle([&](int a) { return row[size_t(a)]; });
            if (o.VectorFields)
                for (const auto& [stage, program] : e.Compiled->Programs)
                    for (const auto& i : program.Code)
                        if (i.Op == "SampleVectorField" && i.Interface.Values.size() > 3)
                        {
                            const glm::vec3 min(i.Interface.Values[0]), max(i.Interface.Values[1]);
                            const glm::vec3 step = (max - min) / 4.0f;
                            const float     len  = std::min({step.x, step.y, step.z}) * 0.8f;
                            for (int z = 0; z <= 4; ++z)
                                for (int y = 0; y <= 4; ++y)
                                    for (int x = 0; x <= 4; ++x)
                                    {
                                        const glm::vec3 p = min + step * glm::vec3(x, y, z);
                                        glm::vec3       v = Vfx::SampleVectorField(i.Interface, p);
                                        const float     n = glm::length(v);
                                        if (n > 1e-6f)
                                            out.push_back({world(p), world(p + v / n * len), 0xff40ffa0});
                                    }
                        }
        }
        return out;
    }

    // ---------------------------------------------------------------- scene props

    void Session::SetFloor(bool shown)
    {
        if (shown == bool(m_Floor))
            return;
        if (!shown)
        {
            m_Scene->DestroyEntity(m_Floor);
            m_Floor = {};
            return;
        }
        m_Floor = m_Scene->CreateEntity("Floor");
        EditableMeshComponent mesh;
        ShapeDesc             d;
        d.Size     = {10, 0.1f, 10};
        mesh.Shape = d;
        RegenerateShape(mesh);
        m_Floor.AddComponent<EditableMeshComponent>(mesh);
        m_Floor.PatchComponent<TransformComponent>([](auto& t) { t.Position = {0, -0.05f, 0}; });
        BoxColliderComponent collider;
        collider.Shape.HalfExtents = {5, 0.05f, 5};
        m_Floor.AddComponent<BoxColliderComponent>(collider);
    }

    void Session::SetSphere(bool shown)
    {
        if (shown == bool(m_Sphere))
            return;
        if (!shown)
        {
            m_Scene->DestroyEntity(m_Sphere);
            m_Sphere = {};
            return;
        }
        m_Sphere = m_Scene->CreateEntity("Sphere");
        EditableMeshComponent mesh;
        ShapeDesc             d;
        d.Type     = ShapeType::Sphere;
        d.Radius   = 0.5f;
        mesh.Shape = d;
        RegenerateShape(mesh);
        m_Sphere.AddComponent<EditableMeshComponent>(mesh);
        m_Sphere.PatchComponent<TransformComponent>([](auto& t) { t.Position = {0, 0.5f, 0}; });
        SphereColliderComponent collider;
        collider.Shape.Radius = 0.5f;
        m_Sphere.AddComponent<SphereColliderComponent>(collider);
    }

    void Session::SetSun(bool shown, glm::vec3 direction)
    {
        if (!shown)
        {
            if (m_Sun)
                m_Scene->DestroyEntity(m_Sun);
            m_Sun = {};
            return;
        }
        if (!m_Sun)
            m_Sun = m_Scene->CreateEntity("Sun");
        DirLightComponent light;
        light.Direction         = glm::length(direction) > 1e-6f ? glm::normalize(direction) : glm::vec3(0, -1, 0);
        light.Diffuse_Intensity = light.Specular_Intensity = glm::vec3(2.0f);
        light.Ambient_Intensity = glm::vec3(0.3f);
        if (m_Sun.HasComponent<DirLightComponent>())
            m_Sun.SetComponent<DirLightComponent>(light);
        else
            m_Sun.AddComponent<DirLightComponent>(light);
    }
} // namespace Kans::VfxPreview
