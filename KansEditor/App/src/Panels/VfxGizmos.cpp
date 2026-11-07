#include "VfxGizmos.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Kans::VfxGizmos
{
    using namespace VfxSystemEditing;
    using Gizmo::HandleResult;

    namespace
    {
        constexpr const char* ReadOnlyMessage = "Built-in templates are read-only; duplicate it to edit it.";

        glm::mat4 Segment(glm::vec3 a, glm::vec3 b)
        {
            const glm::vec3 d      = b - a;
            const float     length = glm::length(d);
            const glm::vec3 z      = length > 0.0f ? d / length : glm::vec3(0, 0, 1);
            const glm::vec3 helper = std::abs(z.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 x      = glm::normalize(glm::cross(helper, z));
            glm::mat4       m(1.0f);
            m[0] = glm::vec4(x, 0.0f);
            m[1] = glm::vec4(glm::cross(z, x), 0.0f);
            m[2] = glm::vec4(z * length, 0.0f);
            m[3] = glm::vec4(a, 1.0f);
            return m;
        }
        glm::vec3 Point(const glm::mat4& m, glm::vec3 p) { return glm::vec3(m * glm::vec4(p, 1.0f)); }
        glm::vec3 Vector(const glm::mat4& m, glm::vec3 v) { return glm::vec3(m * glm::vec4(v, 0.0f)); }
        glm::vec3 Unit(glm::vec3 v, glm::vec3 fallback)
        {
            const float n = glm::length(v);
            return n > 1e-12f ? v / n : fallback;
        }
        // The rotation part of a frame (columns normalized, then made orthogonal).
        glm::mat3 Orthonormal(const glm::mat3& m)
        {
            const glm::vec3 x = Unit(m[0], {1, 0, 0});
            glm::vec3       y = m[1] - x * glm::dot(m[1], x);
            y                 = Unit(y, std::abs(x.y) < 0.9f ? glm::normalize(glm::cross(glm::vec3(0, 0, 1), x)) : glm::vec3(1, 0, 0));
            return glm::mat3(x, y, glm::cross(x, y));
        }
        glm::quat QuatOf(const Vfx::Value& v)
        {
            glm::quat q(v.Data.w, v.Data.x, v.Data.y, v.Data.z);
            const float n = glm::length(q);
            return n > 1e-12f ? q / n : glm::quat(1, 0, 0, 0);
        }
        Vfx::Value Zero(Vfx::Type t)
        {
            if (t == Vfx::Type::Int)
                return Vfx::Value::Int(0);
            if (t == Vfx::Type::Bool)
                return Vfx::Value::Bool(false);
            if (t == Vfx::Type::Quat)
                return Vfx::Value::Vector({0, 0, 0, 1}, t);
            return Vfx::Value::Vector({}, t);
        }
        std::string Format(const char* format, ...)
        {
            char    text[160];
            va_list args;
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            return text;
        }
        const InputInfo* FindInput(const std::vector<InputInfo>& inputs, const std::string& name, std::optional<Vfx::Type> kind = {})
        {
            for (const auto& i : inputs)
                if (i.Spec.Name == name && (!kind || i.Spec.Kind == *kind))
                    return &i;
            return nullptr;
        }
        // The literal an input has now (its own, or the module default); nothing when it is linked
        // or driven by a dynamic input, which a handle cannot edit.
        std::optional<Vfx::Value> LiteralOf(const Vfx::ModuleUse& use, const InputInfo& info)
        {
            if (auto input = GetInput(use, {info.Spec.Name}))
            {
                if (input->Kind != Vfx::InputKind::Literal)
                    return std::nullopt;
                return input->Literal;
            }
            return info.Spec.Default.value_or(Zero(info.Spec.Kind));
        }
        // A position input's frame: the owner, unless its space input says World.
        glm::mat4 PositionFrame(const Vfx::ModuleUse& use, const std::vector<InputInfo>& inputs, const InputInfo& info,
                                const glm::mat4& owner)
        {
            if (info.SpaceInput.empty())
                return owner;
            const auto* space = FindInput(inputs, info.SpaceInput);
            const auto  value = space ? LiteralOf(use, *space) : std::nullopt;
            return value && value->Integer == 1 ? glm::mat4(1.0f) : owner;
        }
        bool IsBuiltin(const ModuleInfo& info, const char* name) { return info.Builtin && info.Name == name; }
        // An angle input's limits in degrees: its declared range (radians) within the module's own.
        std::pair<float, float> AngleLimits(const InputInfo& info, float lo, float hi)
        {
            if (info.Spec.Range)
            {
                lo = std::max(lo, glm::degrees(info.Spec.Range->x));
                hi = std::min(hi, glm::degrees(info.Spec.Range->y));
            }
            return {lo, std::max(lo, hi)};
        }
        bool HasFieldInputs(const std::vector<InputInfo>& inputs)
        {
            return FindInput(inputs, "Module.FieldTranslate", Vfx::Type::Vec3) && FindInput(inputs, "Module.FieldRotation", Vfx::Type::Quat) &&
                   FindInput(inputs, "Module.FieldScale", Vfx::Type::Vec3);
        }
        // Where a use sits, for the simulation stage it may run in.
        const Vfx::SimulationStage* StageOf(const Vfx::Emitter& e, const std::string& use)
        {
            for (const auto& s : e.SimulationStages)
                for (const auto& u : s.Modules)
                    if (u.Guid == use)
                        return &s;
            return nullptr;
        }
    } // namespace

    glm::mat4 SimulationFrame(const Vfx::Emitter& e, const glm::mat4& owner)
    {
        return e.SimulationSpace == Vfx::Space::Local ? owner : glm::mat4(1.0f);
    }

    uint32_t LatticeStep(uint32_t resolution, uint32_t maxLines)
    {
        maxLines = std::max(maxLines, 1u);
        return std::max(1u, (resolution + maxLines - 1) / maxLines);
    }

    std::vector<VfxPreview::DebugLine> GridLattice(const Vfx::InterfaceBinding& grid, const glm::mat4& frame, uint32_t maxLines,
                                                   uint32_t color)
    {
        std::vector<VfxPreview::DebugLine> lines;
        const glm::vec3  half = glm::abs(grid.WorldBounds) * 0.5f;
        const glm::uvec3 res  = glm::max(grid.Resolution, glm::uvec3(1));
        // The cell boundaries drawn along an axis: every Nth one, and always both ends.
        const auto boundaries = [&](int axis) {
            std::vector<float> at;
            const uint32_t     step = LatticeStep(res[axis], maxLines);
            for (uint32_t i = 0; i < res[axis]; i += step)
                at.push_back(-half[axis] + 2.0f * half[axis] * float(i) / float(res[axis]));
            at.push_back(half[axis]);
            return at;
        };
        const auto add = [&](glm::vec3 a, glm::vec3 b) { lines.push_back({Point(frame, a), Point(frame, b), color}); };
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, c = (a + 2) % 3;
            // Lines along `a` on the two faces normal to `c` (at b's boundaries) and to `b` (at c's).
            for (const auto& [across, face] : {std::pair<int, int> {b, c}, std::pair<int, int> {c, b}})
                for (float x : boundaries(across))
                    for (float side : {-1.0f, 1.0f})
                    {
                        glm::vec3 p(0.0f), q(0.0f);
                        p[a] = -half[a], q[a] = half[a];
                        p[across] = q[across] = x;
                        p[face] = q[face] = side * half[face];
                        add(p, q);
                    }
        }
        return lines;
    }

    std::vector<std::string> GridsOf(const Vfx::Emitter& e, const std::string& use)
    {
        std::vector<std::string> names;
        const auto add = [&](const std::string& name) {
            const auto at = e.DataInterfaces.find(name);
            if (at != e.DataInterfaces.end() && at->second.Kind == "Grid3D" && std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        };
        if (use.empty())
            return names;
        if (const auto* u = FindUse(e, use))
            for (const auto& [name, input] : u->Inputs)
                if (input.Kind == Vfx::InputKind::DataInterface && input.Interface.Kind == "Grid3D")
                    add(input.Interface.Source);
        if (const auto* stage = StageOf(e, use); stage && stage->Source == Vfx::IterationSource::DataInterface)
            add(stage->DataInterface);
        return names;
    }

    bool HasHandles(const Vfx::ModuleUse& use, const Context& ctx)
    {
        const auto* info = ctx.Modules ? ctx.Modules->Find(use.Script) : nullptr;
        if (!info)
            return false;
        if (IsBuiltin(*info, "ShapeLocation") || IsBuiltin(*info, "KillParticlesInVolume") || IsBuiltin(*info, "AddVelocity"))
            return true;
        const auto inputs = ModuleInputs(info->Data);
        if (HasFieldInputs(inputs))
            return true;
        for (const auto& i : inputs)
            if (i.Spec.Kind == Vfx::Type::Position || i.Interface == "Grid3D")
                return true;
        return false;
    }

    std::vector<std::pair<std::string, std::string>> UsesWithHandles(const Vfx::System& system, const Context& ctx,
                                                                     const std::string& emitter)
    {
        std::vector<std::pair<std::string, std::string>> uses;
        const auto* h = FindHandle(system, emitter);
        if (!h)
            return uses;
        Vfx::Emitter e;
        try
        {
            e = Effective(*h, ctx);
        }
        catch (const std::exception&)
        {
            return uses;
        }
        const auto add = [&](const Vfx::Stack& stack, const std::string& where) {
            for (const auto& u : stack)
                if (HasHandles(u, ctx))
                    uses.push_back({u.Guid, where + " \xE2\x80\xBA " + (ctx.Modules ? ctx.Modules->NameOf(u.Script) : u.Script.ToString())});
        };
        static const char* stages[] = {"System Spawn", "System Update", "Emitter Spawn", "Emitter Update", "Particle Spawn", "Particle Update"};
        for (const auto& [stage, stack] : e.Stages)
            add(stack, int(stage) < 6 ? stages[int(stage)] : "Stage");
        for (const auto& handler : e.Handlers)
            add(handler.Modules, "Event Handler");
        for (const auto& stage : e.SimulationStages)
            add(stage.Modules, stage.Name.empty() ? std::string("Simulation Stage") : stage.Name);
        return uses;
    }

    // ---------------------------------------------------------------------- drawing

    Gizmo::HandleStyle Gizmos::Style(Gizmo::KnobShape knob) const
    {
        Gizmo::HandleStyle style;
        style.Color       = Accent;
        style.Knob        = knob;
        style.Interactive = m_Editable;
        // Read-only knobs are dimmer and smaller: they show the shape, not something to grab.
        if (!m_Editable)
        {
            style.Color.a    = 0.55f;
            style.KnobPixels = 3.5f;
        }
        return style;
    }

    Gizmo::HandleId Gizmos::Id(const std::string& use, const std::string& input, uint64_t part) const
    {
        const uint64_t scope = std::hash<std::string> {}(m_Emitter + "/" + use + "/" + input);
        return Gizmo::MakeHandleId(scope, part + 1);
    }

    void Gizmos::Line(glm::vec3 a, glm::vec3 b, float alpha)
    {
        if (glm::length(b - a) <= 0.0f)
            return;
        m_Wire.push_back({GizmoShape::Line, Segment(a, b), glm::vec4(glm::vec3(Accent), alpha * (m_Editable ? 1.0f : 0.7f))});
    }

    void Gizmos::Circle(const glm::mat4& frame, glm::vec3 centre, glm::vec3 u, glm::vec3 v, float radius)
    {
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(Vector(frame, u * radius), 0.0f);
        m[1] = glm::vec4(Vector(frame, v * radius), 0.0f);
        m[2] = glm::vec4(Unit(glm::cross(glm::vec3(m[0]), glm::vec3(m[1])), {0, 0, 1}), 0.0f);
        m[3] = glm::vec4(Point(frame, centre), 1.0f);
        m_Wire.push_back({GizmoShape::Circle, m, glm::vec4(glm::vec3(Accent), m_Editable ? 0.9f : 0.65f)});
    }

    void Gizmos::WireBox(const glm::mat4& frame, glm::vec3 c, glm::vec3 h)
    {
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, d = (a + 2) % 3;
            for (float sb : {-1.0f, 1.0f})
                for (float sd : {-1.0f, 1.0f})
                {
                    glm::vec3 p = c, q = c;
                    p[a] -= h[a], q[a] += h[a];
                    p[b] = q[b] = c[b] + sb * h[b];
                    p[d] = q[d] = c[d] + sd * h[d];
                    Line(Point(frame, p), Point(frame, q));
                }
        }
    }

    void Gizmos::WireCone(glm::vec3 apex, glm::vec3 axis, glm::vec3 side, float length, float radians, bool spherical)
    {
        // The rim and four lines from the apex to it.
        const glm::vec3 a = Unit(axis, {0, 0, 1}), s = Unit(side - a * glm::dot(side, a), {1, 0, 0});
        const glm::vec3 t = glm::cross(a, s);
        const float     clamped = std::clamp(radians, 0.0f, spherical ? glm::pi<float>() : glm::radians(89.0f));
        const glm::vec3 rim     = apex + a * (spherical ? length * std::cos(clamped) : length);
        const float     r       = spherical ? length * std::sin(clamped) : length * std::tan(clamped);
        if (r > 0.0f)
        {
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(s * r, 0.0f);
            m[1] = glm::vec4(t * r, 0.0f);
            m[2] = glm::vec4(a, 0.0f);
            m[3] = glm::vec4(rim, 1.0f);
            m_Wire.push_back({GizmoShape::Circle, m, glm::vec4(glm::vec3(Accent), m_Editable ? 0.9f : 0.65f)});
        }
        for (const glm::vec3 d : {s, -s, t, -t})
            Line(apex, rim + d * r, 0.75f);
    }

    void Gizmos::Arrow(glm::vec3 from, glm::vec3 to)
    {
        const float length = glm::length(to - from);
        if (length <= 0.0f)
            return;
        const glm::vec3 d    = (to - from) / length;
        const float     head = std::min(length * 0.35f, std::max(m_UnitsPerPixel, 1e-6f) * 14.0f);
        Line(from, to - d * head);
        glm::mat4 cone = Segment(to - d * head, to);
        cone[0] *= head * 0.35f;
        cone[1] *= head * 0.35f;
        m_Wire.push_back({GizmoShape::Cone, cone, glm::vec4(glm::vec3(Accent), m_Editable ? 0.95f : 0.7f)});
    }

    void Gizmos::AppendDraws(std::vector<GizmoDraw>& out) const
    {
        out.insert(out.end(), m_Wire.begin(), m_Wire.end());
        if (m_FieldShown)
            m_Field.AppendDraws(out);
    }

    // ---------------------------------------------------------------------- frames

    void Gizmos::Reset()
    {
        m_Start.reset();
        m_FieldStart.reset();
        m_Field.Reset();
        m_Readout.clear();
    }

    void Gizmos::Write(const HandleResult& r, const Edit& edit, std::string readout)
    {
        if (r.Active || r.Committed)
            m_Readout = std::move(readout);
        if (!m_Editable || !m_Document)
            return;
        auto& doc = *m_Document;
        if (r.Started)
        {
            m_Start = doc.Data();
            m_Message.clear();
        }
        if (!m_Start)
            return;
        if (r.Cancelled)
        {
            doc.Data() = *m_Start;
            m_Start.reset();
            m_Readout.clear();
            return;
        }
        if (r.Changed)
        {
            doc.Data()      = *m_Start;
            const auto done = edit(doc.Data(), *m_Context);
            if (!done)
            {
                m_Message  = done.Message;
                doc.Data() = *m_Start;
            }
        }
        if (r.Committed)
        {
            doc.Commit();
            m_Start.reset();
        }
    }

    void Gizmos::Declare(Gizmo::HandleContext& context, const Pointer& p, SystemDocument& document, const Context& ctx,
                         const Selection& selection, const Options& options)
    {
        // A drag whose handle went away (selection changed, undo): put the start back.
        if (m_Start && context.GetActive() == 0 && !m_Field.IsDragging())
        {
            if (m_Document == &document)
                document.Data() = *m_Start;
            m_Start.reset();
            m_FieldStart.reset();
        }
        m_Document = &document;
        m_Context  = &ctx;
        m_Emitter  = selection.Emitter;
        m_Pointer  = p;
        m_Mouse    = p.Input.Mouse;
        m_Editable = options.Editable && !document.ReadOnly();
        if (options.Editable && document.ReadOnly())
            m_Message = ReadOnlyMessage;
        m_Declared.clear();
        m_Wire.clear();
        m_Lattice.clear();
        if (!m_Start)
            m_Readout.clear();
        m_FieldNow.reset();

        const auto* handle = FindHandle(document.Data(), selection.Emitter);
        if (!handle)
            return;
        Vfx::Emitter e;
        try
        {
            e = Effective(*handle, ctx);
        }
        catch (const std::exception&)
        {
            return;
        }
        const glm::mat4 sim = SimulationFrame(e, options.Owner);
        m_UnitsPerPixel     = p.View.UnitsPerPixel(glm::vec3(sim[3]));
        const std::string emitter = selection.Emitter;

        if (!selection.Use.empty())
            if (const auto* use = FindUse(e, selection.Use))
                DeclareUse(context, p, e, *use, options);

        // The emitter's fixed bounds (Alt: one face; LockBoundsCenter keeps them symmetric).
        if (selection.Bounds && e.FixedBounds && e.FixedBounds->IsValid())
        {
            const auto base = SettingsOf(e);
            DeclareBox(context, p, Id("", "FixedBounds"), Kind::Bounds, "", "FixedBounds", sim, e.FixedBounds->Center(),
                       (e.FixedBounds->Max - e.FixedBounds->Min) * 0.5f, options.LockBoundsCenter,
                       [emitter, base](glm::vec3 c, glm::vec3 h) -> Edit {
                           return [=](Vfx::System& s, const Context& x) {
                               auto settings = base;
                               Bounds b;
                               b.Min                = c - h;
                               b.Max                = c + h;
                               settings.FixedBounds = b;
                               return SetEmitterSettings(s, x, emitter, settings);
                           };
                       },
                       "Bounds");
        }

        // Grids the use reads, and the one chosen in the parameters panel: centred, symmetric.
        auto grids = GridsOf(e, selection.Use);
        if (!selection.Grid.empty() && std::find(grids.begin(), grids.end(), selection.Grid) == grids.end())
            if (const auto at = e.DataInterfaces.find(selection.Grid); at != e.DataInterfaces.end() && at->second.Kind == "Grid3D")
                grids.push_back(selection.Grid);
        for (const auto& name : grids)
        {
            const auto binding = e.DataInterfaces.at(name);
            const auto [c, h]  = DeclareBox(context, p, Id("", name), Kind::Grid, "", name, sim, glm::vec3(0.0f),
                                            glm::abs(binding.WorldBounds) * 0.5f, true,
                                            [emitter, name, binding](glm::vec3, glm::vec3 h) -> Edit {
                                                return [=](Vfx::System& s, const Context& x) {
                                                    auto b        = binding;
                                                    b.WorldBounds = h * 2.0f;
                                                    return SetDataInterface(s, x, emitter, name, b);
                                                };
                                            },
                                            "Grid");
            auto shown        = binding;
            shown.WorldBounds = h * 2.0f;
            const auto lines  = GridLattice(shown, sim, options.LatticeLines);
            m_Lattice.insert(m_Lattice.end(), lines.begin(), lines.end());
            (void)c;
        }
    }

    std::pair<glm::vec3, glm::vec3> Gizmos::DeclareBox(Gizmo::HandleContext& context, const Pointer& p, Gizmo::HandleId id, Kind kind,
                                                       const std::string& use, const std::string& input, const glm::mat4& frame,
                                                       glm::vec3 centre, glm::vec3 half, bool lock,
                                                       const std::function<Edit(glm::vec3, glm::vec3)>& edit, const char* label)
    {
        Gizmo::BoxHandle box;
        box.Frame      = frame;
        box.MinHalf    = 1e-3f;
        box.LockCenter = lock;
        box.Symmetric  = lock;
        box.Snap       = p.Settings.TranslateStep;
        box.SnapOn     = p.Settings.Snap;
        box.Style      = Style(Gizmo::KnobShape::Cube);
        glm::vec3  c = centre, h = glm::abs(half);
        const auto r = context.Box(id, box, c, h);
        m_Declared.push_back({id, kind, use, input, m_Editable});
        WireBox(frame, c, h);
        const glm::vec3 size = h * 2.0f;
        Write(r, edit(c, h), Format("%s %.3g \xC3\x97 %.3g \xC3\x97 %.3g m", label, size.x, size.y, size.z));
        return {c, h};
    }

    void Gizmos::DeclareUse(Gizmo::HandleContext& context, const Pointer& p, const Vfx::Emitter& e, const Vfx::ModuleUse& use,
                            const Options& options)
    {
        const auto* info = m_Context->Modules ? m_Context->Modules->Find(use.Script) : nullptr;
        if (!info)
            return;
        const auto        inputs = ModuleInputs(info->Data);
        const glm::mat4   owner  = options.Owner, sim = SimulationFrame(e, owner);
        const std::string emitter = m_Emitter, guid = use.Guid;
        const auto        literal = [&](const char* name, Vfx::Type kind) -> std::optional<Vfx::Value> {
            const auto* i = FindInput(inputs, name, kind);
            return i ? LiteralOf(use, *i) : std::nullopt;
        };
        // Writes literals to inputs of this use (overrides on inherited emitters).
        const auto set = [emitter, guid](std::vector<std::pair<std::string, Vfx::Value>> values) -> Edit {
            return [=](Vfx::System& s, const Context& x) {
                for (const auto& [name, value] : values)
                {
                    Vfx::Input input;
                    input.Literal = value;
                    if (auto r = SetInput(s, x, emitter, guid, {name}, input); !r)
                        return r;
                }
                return Result::Done();
            };
        };
        const auto vec3 = [](glm::vec3 v, Vfx::Type t = Vfx::Type::Vec3) { return Vfx::Value::Vector(glm::vec4(v, t == Vfx::Type::Position ? 1.0f : 0.0f), t); };
        const auto declared = [&](Gizmo::HandleId id, Kind kind, const std::string& input) {
            m_Declared.push_back({id, kind, guid, input, m_Editable});
        };
        const auto snapLinear = [&](Gizmo::LinearHandle& h) {
            h.Snap   = p.Settings.TranslateStep;
            h.SnapOn = p.Settings.Snap;
        };
        const auto angle = [&](const InputInfo& spec, float radians, glm::vec3 apex, glm::vec3 axis, glm::vec3 reference, float radius,
                               float natural, const char* label) {
            Gizmo::AngleHandle h;
            h.Center    = apex;
            h.Axis      = Unit(axis, {1, 0, 0});
            h.Reference = Unit(reference, {0, 0, 1});
            h.Radius    = std::max(radius, 1e-4f);
            const auto [lo, hi] = AngleLimits(spec, 0.0f, natural);
            h.Min       = lo;
            h.Max       = hi;
            h.Snap      = p.Settings.RotateStep;
            h.SnapOn    = p.Settings.Snap;
            h.Style     = Style();
            float      degrees = glm::degrees(radians);
            const auto id      = Id(guid, spec.Spec.Name);
            const auto r       = context.Angle(id, h, degrees);
            declared(id, Kind::Angle, spec.Spec.Name);
            Write(r, set({{spec.Spec.Name, Vfx::Value::Float(glm::radians(degrees))}}), Format("%s %.1f\xC2\xB0", label, degrees));
            return glm::radians(degrees);
        };
        const auto length = [&](const std::string& input, float value, glm::vec3 origin, glm::vec3 direction, const char* label,
                                const char* units, uint64_t part = 0) {
            Gizmo::LinearHandle h;
            h.Origin    = origin;
            h.Direction = direction;
            h.Min       = 1e-3f;
            h.Style     = Style();
            snapLinear(h);
            const auto id = Id(guid, input, part);
            const auto r  = context.Linear(id, h, value);
            declared(id, Kind::Length, input);
            Write(r, set({{input, Vfx::Value::Float(value)}}), Format("%s %.3g %s", label, value, units));
            return value;
        };
        const auto point = [&](const std::string& input, glm::vec3 local, const glm::mat4& frame, Kind kind, Vfx::Type type,
                               const char* label, const char* units) {
            Gizmo::PointHandle h;
            const auto         id = Id(guid, input);
            // The constraint is chosen when the drag starts: an axis key, Shift for the ground plane, or the view.
            if (context.GetActive() != id)
            {
                m_PointMode = Gizmo::PointHandle::Constraint::Screen;
                m_PointAxis = glm::vec3(0, 0, 1);
                if (p.Input.AxisKey >= 0 && p.Input.AxisKey < 3)
                {
                    m_PointMode = Gizmo::PointHandle::Constraint::Line;
                    m_PointAxis = Unit(glm::vec3(frame[p.Input.AxisKey]), glm::vec3(0, 0, 1));
                }
                else if (p.Input.Shift)
                {
                    m_PointMode = Gizmo::PointHandle::Constraint::Plane;
                    m_PointAxis = Unit(glm::vec3(frame[1]), glm::vec3(0, 1, 0));
                }
            }
            h.Mode   = m_PointMode;
            h.Axis   = m_PointAxis;
            h.Snap   = p.Settings.TranslateStep;
            h.SnapOn = p.Settings.Snap;
            h.Style  = Style();
            glm::vec3  world = Point(frame, local);
            const auto r     = context.Point(id, h, world);
            declared(id, kind, input);
            const glm::vec3 moved = Point(glm::inverse(frame), world);
            Write(r, set({{input, vec3(moved, type)}}), Format("%s (%.3g, %.3g, %.3g) %s", label, moved.x, moved.y, moved.z, units));
            return moved;
        };

        if (IsBuiltin(*info, "ShapeLocation"))
        {
            // Spawn shapes about the owner (Module.Shape: 0 sphere, 1 box, 2 cone along +Z, 3 disc in XY).
            const auto shape  = literal("Module.Shape", Vfx::Type::Int);
            const auto radius = literal("Module.Radius", Vfx::Type::Float);
            if (!shape)
                return;
            if (shape->Integer == 0 && radius)
            {
                Gizmo::SphereHandle h;
                h.Frame     = owner;
                h.MinRadius = 1e-3f;
                h.Snap      = p.Settings.TranslateStep;
                h.SnapOn    = p.Settings.Snap;
                h.Style     = Style();
                float      r0 = std::abs(radius->Data.x);
                const auto id = Id(guid, "Module.Radius");
                const auto r  = context.Sphere(id, h, r0);
                declared(id, Kind::Sphere, "Module.Radius");
                for (const auto& [u, v] : {std::pair<glm::vec3, glm::vec3> {{1, 0, 0}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}}})
                    Circle(owner, glm::vec3(0), u, v, r0);
                Write(r, set({{"Module.Radius", Vfx::Value::Float(r0)}}), Format("Radius %.3g m", r0));
            }
            else if (shape->Integer == 1)
            {
                if (const auto extents = literal("Module.BoxExtents", Vfx::Type::Vec3))
                    DeclareBox(context, p, Id(guid, "Module.BoxExtents"), Kind::Box, guid, "Module.BoxExtents", owner, glm::vec3(0),
                               glm::vec3(extents->Data), true,
                               [&](glm::vec3, glm::vec3 h) { return set({{"Module.BoxExtents", vec3(h)}}); }, "Box");
            }
            else if (shape->Integer == 2 && radius)
            {
                const auto* spec = FindInput(inputs, "Module.ConeAngle", Vfx::Type::Float);
                const auto  cone = spec ? LiteralOf(use, *spec) : std::nullopt;
                const glm::vec3 apex = glm::vec3(owner[3]), z = Vector(owner, {0, 0, 1}), x = Vector(owner, {1, 0, 0});
                const float     height = length("Module.Radius", std::abs(radius->Data.x), apex, z, "Length", "m");
                float           radians = cone ? cone->Data.x : 0.0f;
                if (spec && cone)
                    radians = angle(*spec, radians, apex, x, z, glm::length(z) * height, 89.0f, "Cone angle");
                WireCone(apex, z, -Vector(owner, {0, 1, 0}), glm::length(z) * height, radians, false);
            }
            else if (shape->Integer == 3 && radius)
            {
                float r0 = std::abs(radius->Data.x);
                for (int k = 0; k < 4; ++k)
                {
                    glm::vec3 d(0.0f);
                    d[k / 2] = k % 2 ? -1.0f : 1.0f;
                    r0       = length("Module.Radius", r0, glm::vec3(owner[3]), Vector(owner, d), "Radius", "m", uint64_t(k));
                }
                Circle(owner, glm::vec3(0), {1, 0, 0}, {0, 1, 0}, r0);
            }
            return;
        }
        if (IsBuiltin(*info, "KillParticlesInVolume"))
        {
            // A sphere or box (Module.Shape 0 or 1) about Module.Center, in simulation space.
            const auto shape  = literal("Module.Shape", Vfx::Type::Int);
            const auto center = literal("Module.Center", Vfx::Type::Vec3);
            if (!shape || !center)
                return;
            glm::vec3 c = point("Module.Center", glm::vec3(center->Data), sim, Kind::Point, Vfx::Type::Vec3, "Center", "m");
            if (shape->Integer == 1)
            {
                if (const auto extents = literal("Module.BoxExtents", Vfx::Type::Vec3))
                    DeclareBox(context, p, Id(guid, "Module.BoxExtents"), Kind::Box, guid, "Module.BoxExtents", sim, c,
                               glm::vec3(extents->Data), false,
                               [&](glm::vec3 centre, glm::vec3 h) { return set({{"Module.Center", vec3(centre)}, {"Module.BoxExtents", vec3(h)}}); },
                               "Volume");
            }
            else if (const auto radius = literal("Module.Radius", Vfx::Type::Float))
            {
                Gizmo::SphereHandle h;
                h.Frame     = sim * glm::translate(glm::mat4(1.0f), c);
                h.MinRadius = 1e-3f;
                h.Snap      = p.Settings.TranslateStep;
                h.SnapOn    = p.Settings.Snap;
                h.Style     = Style();
                float      r0 = std::abs(radius->Data.x);
                const auto id = Id(guid, "Module.Radius");
                const auto r  = context.Sphere(id, h, r0);
                declared(id, Kind::Sphere, "Module.Radius");
                for (const auto& [u, v] : {std::pair<glm::vec3, glm::vec3> {{1, 0, 0}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}}})
                    Circle(sim, c, u, v, r0);
                Write(r, set({{"Module.Radius", Vfx::Value::Float(r0)}}), Format("Radius %.3g m", r0));
            }
            return;
        }
        if (IsBuiltin(*info, "AddVelocity"))
        {
            // Module.Mode: 0 a velocity vector, 1 outward from a point, 2 a cone about +Y.
            const auto mode = literal("Module.Mode", Vfx::Type::Int);
            if (!mode)
                return;
            const glm::vec3 origin = glm::vec3(sim[3]);
            if (mode->Integer == 0)
            {
                if (const auto velocity = literal("Module.Velocity", Vfx::Type::Vec3))
                {
                    const glm::vec3 v = point("Module.Velocity", glm::vec3(velocity->Data), sim, Kind::Arrow, Vfx::Type::Vec3, "Velocity", "m/s");
                    Arrow(origin, Point(sim, v));
                }
            }
            else if (mode->Integer == 1)
            {
                if (const auto* spec = FindInput(inputs, "Module.Point", Vfx::Type::Position))
                    if (const auto value = LiteralOf(use, *spec))
                    {
                        const glm::mat4 frame = PositionFrame(use, inputs, *spec, owner);
                        const glm::vec3 at    = point("Module.Point", glm::vec3(value->Data), frame, Kind::Point, Vfx::Type::Position, "Point", "m");
                        // Rays outward from the point.
                        const float r = std::max(m_UnitsPerPixel, 1e-6f) * 24.0f;
                        for (const glm::vec3 d : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0)})
                            Arrow(Point(frame, at) + d * r * 0.4f, Point(frame, at) + d * r);
                    }
            }
            else if (mode->Integer == 2)
            {
                const auto  speed = literal("Module.Speed", Vfx::Type::Float);
                const auto* spec  = FindInput(inputs, "Module.ConeAngle", Vfx::Type::Float);
                const auto  cone  = spec ? LiteralOf(use, *spec) : std::nullopt;
                if (!speed)
                    return;
                const glm::vec3 y = Unit(Vector(sim, {0, 1, 0}), {0, 1, 0}), x = Unit(Vector(sim, {1, 0, 0}), {1, 0, 0});
                const float     s = length("Module.Speed", std::abs(speed->Data.x), origin, y, "Speed", "m/s");
                float           radians = cone ? cone->Data.x : 0.0f;
                if (spec && cone)
                    radians = angle(*spec, radians, origin, x, y, s, 180.0f, "Cone angle");
                Arrow(origin, origin + y * s);
                WireCone(origin, y, Vector(sim, {0, 0, 1}), s, radians, true);
            }
            return;
        }
        // Any module: vector field placement, and position inputs.
        if (HasFieldInputs(inputs))
        {
            Field f;
            f.Use       = guid;
            f.Frame     = sim;
            f.Translate = glm::vec3(literal("Module.FieldTranslate", Vfx::Type::Vec3).value_or(Vfx::Value::Vector({})).Data);
            f.Rotation  = QuatOf(literal("Module.FieldRotation", Vfx::Type::Quat).value_or(Zero(Vfx::Type::Quat)));
            f.Scale     = glm::vec3(literal("Module.FieldScale", Vfx::Type::Vec3).value_or(Vfx::Value::Vector({1, 1, 1, 0})).Data);
            m_FieldNow  = f;
            m_FieldPivot.Position = Point(sim, f.Translate);
            m_FieldPivot.Axes     = Orthonormal(glm::mat3(sim) * glm::mat3_cast(f.Rotation));
            declared(FieldGizmoId, Kind::Field, "Module.FieldTranslate");
            if (m_Editable && !m_Field.IsDragging() && p.Input.Allowed && context.GetActive() == 0)
            {
                const auto hit = m_Field.Pick(p.View, m_FieldPivot, Gizmo::Mode::Combined, p.Settings, p.Input.Mouse);
                if (hit.Id != Gizmo::Handle::None)
                    context.Offer(FieldGizmoId, hit.Depth, hit.Distance, Gizmo::HandlePriority(hit.Id));
            }
            // The field's frame as a small tripod (scaled by FieldScale).
            const float size = Gizmo::TransformGizmo::WorldSize(p.View, m_FieldPivot.Position, p.Settings) * 0.5f;
            for (int a = 0; a < 3; ++a)
                Line(m_FieldPivot.Position, m_FieldPivot.Position + m_FieldPivot.Axes[a] * size * std::abs(f.Scale[a]), 0.6f);
        }
        for (const auto& spec : inputs)
            if (spec.Spec.Kind == Vfx::Type::Position)
                if (const auto value = LiteralOf(use, spec))
                    point(spec.Spec.Name, glm::vec3(value->Data), PositionFrame(use, inputs, spec, owner), Kind::Point, Vfx::Type::Position,
                          spec.Spec.Name.substr(spec.Spec.Name.find('.') + 1).c_str(), "m");
    }

    void Gizmos::Update(const Gizmo::HandleContext& context, const Pointer& p)
    {
        m_FieldShown = false;
        if (!m_FieldNow || !m_Editable || !m_Document)
        {
            if (m_Field.IsDragging())
            {
                if (m_Start && m_Document)
                    m_Document->Data() = *m_Start;
                m_Start.reset();
                m_FieldStart.reset();
            }
            m_Field.Reset();
            return;
        }
        Gizmo::Input input = p.Input;
        // It moves only when it won the shared hover (or is already being dragged).
        if (!m_Field.IsDragging())
            input.Allowed = input.Allowed && !context.IsDragging() && context.Hot() == FieldGizmoId;
        const auto event = m_Field.Update(p.View, m_FieldPivot, Gizmo::Mode::Combined, p.Settings, input);
        m_FieldShown     = true;
        auto& doc        = *m_Document;
        switch (event)
        {
            case Gizmo::Event::Began:
                m_FieldStart = m_FieldNow;
                m_Start      = doc.Data();
                m_Message.clear();
                break;
            case Gizmo::Event::Dragged:
            case Gizmo::Event::Committed:
                if (m_Start && m_FieldStart)
                {
                    const auto&     d  = m_Field.GetDelta();
                    const Field&    f0 = *m_FieldStart;
                    const glm::mat3 linear(f0.Frame);
                    glm::vec3       t = f0.Translate, s = f0.Scale;
                    glm::quat       q = f0.Rotation;
                    if (d.Op == Gizmo::Operation::Translate)
                        t = f0.Translate + glm::inverse(linear) * d.Translation;
                    else if (d.Op == Gizmo::Operation::Rotate)
                    {
                        const glm::quat frame = glm::quat_cast(Orthonormal(linear));
                        q                     = glm::normalize(glm::inverse(frame) * d.Rotation * frame * f0.Rotation);
                    }
                    else if (d.Op == Gizmo::Operation::Scale)
                        s = f0.Scale * d.Scale;
                    std::vector<std::pair<std::string, Vfx::Value>> values;
                    if (t != f0.Translate)
                        values.push_back({"Module.FieldTranslate", Vfx::Value::Vector(glm::vec4(t, 0), Vfx::Type::Vec3)});
                    if (q != f0.Rotation)
                        values.push_back({"Module.FieldRotation", Vfx::Value::Vector({q.x, q.y, q.z, q.w}, Vfx::Type::Quat)});
                    if (s != f0.Scale)
                        values.push_back({"Module.FieldScale", Vfx::Value::Vector(glm::vec4(s, 0), Vfx::Type::Vec3)});
                    doc.Data() = *m_Start;
                    for (const auto& [name, value] : values)
                    {
                        Vfx::Input literal;
                        literal.Literal = value;
                        if (auto r = SetInput(doc.Data(), *m_Context, m_Emitter, f0.Use, {name}, literal); !r)
                        {
                            m_Message  = r.Message;
                            doc.Data() = *m_Start;
                            break;
                        }
                    }
                }
                if (event == Gizmo::Event::Committed)
                {
                    doc.Commit();
                    m_Start.reset();
                    m_FieldStart.reset();
                }
                break;
            case Gizmo::Event::Cancelled:
                if (m_Start)
                    doc.Data() = *m_Start;
                m_Start.reset();
                m_FieldStart.reset();
                break;
            case Gizmo::Event::None:
                break;
        }
        if (m_Field.IsDragging())
        {
            m_Readout = "Field " + m_Field.Readout();
            m_Mouse   = p.Input.Mouse;
        }
    }

    // ---------------------------------------------------------------------- scene viewport

    const SceneHandles::Choice* SceneHandles::Find(UUID entity) const
    {
        const auto at = m_Choices.find(uint64_t(entity));
        return at == m_Choices.end() ? nullptr : &at->second;
    }

    SceneHandles::Source SceneHandles::Resolve(AssetID system, bool forEditing) const
    {
        return m_Resolve && system.IsValid() ? m_Resolve(system, forEditing) : Source {};
    }

    bool SceneHandles::SetEdit(UUID entity, AssetID system, bool edit)
    {
        auto& choice = ChoiceFor(entity);
        if (!edit)
        {
            choice.Edit = false;
            m_Message.clear();
            return true;
        }
        const auto source = Resolve(system, true);
        if (!source.Document)
        {
            choice.Edit = false;
            m_Message   = "The system has no source file to edit.";
            return false;
        }
        if (source.Document->ReadOnly())
        {
            choice.Edit = false;
            m_Message   = ReadOnlyMessage;
            return false;
        }
        if (!source.Editor)
        {
            choice.Edit = false;
            m_Message   = "The system could not be opened in the system editor.";
            return false;
        }
        choice.Edit = true;
        m_Message.clear();
        return true;
    }

    void SceneHandles::Declare(Gizmo::HandleContext& context, const Pointer& p, UUID entity, AssetID system, const glm::mat4& world,
                               const Options& options)
    {
        m_Declared   = false;
        auto& choice = ChoiceFor(entity);
        const auto source = Resolve(system, false);
        if (!source.Document || !source.Context)
            return;
        if (choice.Edit && source.Document->ReadOnly())
        {
            choice.Edit = false;
            m_Message   = ReadOnlyMessage;
        }
        else if (choice.Edit && !source.Editor)
        {
            // Its system editor was closed: edits would have nowhere to be saved.
            choice.Edit = false;
            m_Message   = "Edit mode turned off: the system editor was closed.";
        }
        const auto& data = source.Document->Data();
        if (!FindHandle(data, choice.Emitter))
            choice.Emitter = data.Emitters.empty() ? std::string() : data.Emitters.front().Guid;
        Selection selection;
        selection.Emitter = choice.Emitter;
        selection.Use     = choice.Use;
        Options o         = options;
        o.Owner           = world;
        o.Editable        = choice.Edit;
        m_Gizmos.Declare(context, p, *source.Document, *source.Context, selection, o);
        m_Declared = true;
    }

    void SceneHandles::Update(const Gizmo::HandleContext& context, const Pointer& p)
    {
        if (m_Declared)
            m_Gizmos.Update(context, p);
        else
            m_Gizmos.Reset();
        m_Declared = false;
    }
} // namespace Kans::VfxGizmos
