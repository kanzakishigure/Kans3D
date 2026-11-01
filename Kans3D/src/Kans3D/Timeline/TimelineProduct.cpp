#include "kspch.h"
#include "TimelineProduct.h"
#include <cstring>
#include <set>
#include "Kans3D/VFX/VfxSystemAsset.h"
namespace Kans
{
    glm::vec4 TimelineCurve::Sample(double t, glm::vec4 fallback) const
    {
        if (Keys.empty())
            return fallback;
        if (t <= Keys.front().Time)
            return Keys.front().Value;
        if (t >= Keys.back().Time)
            return Keys.back().Value;
        auto upper = std::upper_bound(Keys.begin(), Keys.end(), t, [](double v, const auto& k) { return v < k.Time; });
        const auto& a = *(upper - 1);
        const auto& b = *upper;
        if (a.Interpolation == TimelineInterpolation::Constant)
            return a.Value;
        float u = float((t - a.Time) / (b.Time - a.Time));
        if (a.Interpolation == TimelineInterpolation::Linear)
            return glm::mix(a.Value, b.Value, u);
        float dt = float(b.Time - a.Time), v = 1 - u;
        return v * v * v * a.Value + 3 * v * v * u * (a.Value + a.OutTangent * dt / 3.f) +
               3 * v * u * u * (b.Value - b.InTangent * dt / 3.f) + u * u * u * b.Value;
    }
    TimelineRange TimelineBindingRange(const TimelineData& d, const TimelineBinding& binding)
    {
        double start = binding.Lifetime.Start;
        if (start == -1)
        {
            start      = d.Length;
            bool found = false;
            auto add   = [&](double t) {
                start = std::min(start, t);
                found = true;
            };
            auto curve = [&](const TimelineCurve& c) {
                if (!c.Keys.empty())
                    add(c.Keys.front().Time);
            };
            for (const auto& t : d.Transforms)
                if (t.Binding == binding.ID)
                {
                    curve(t.Position);
                    curve(t.Rotation);
                    curve(t.Scale);
                }
            for (const auto& t : d.Properties)
                if (t.Binding == binding.ID)
                    curve(t.Curve);
            for (const auto& t : d.Activations)
                if (t.Binding == binding.ID)
                    for (const auto& r : t.Ranges)
                        add(r.Start);
            for (const auto& t : d.Animations)
                if (t.Binding == binding.ID)
                    for (const auto& c : t.Clips)
                        add(c.Start);
            for (const auto& t : d.Particles)
                if (t.Binding == binding.ID)
                    for (const auto& s : t.Sections)
                        add(s.Start);
            for (const auto& c : d.CameraCuts)
                if (c.Binding == binding.ID)
                    add(c.Start);
            if (!found)
                start = 0;
        }
        return {start, binding.Lifetime.End == 0 ? d.Length : binding.Lifetime.End};
    }
    AnimationValidation ValidateTimeline(const TimelineData& d)
    {
        auto error = [](std::string text) -> AnimationValidation {
            return AssetError {AssetErrorCode::InvalidData, "Timeline: " + text};
        };
        auto finite = [](glm::vec4 v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
        };
        auto text = [](const std::string& s) { return s.size() <= 4096 && s.find('\0') == std::string::npos; };
        if (!std::isfinite(d.Length) || d.Length < 0 || !std::isfinite(d.FrameRate) || d.FrameRate <= 0)
            return error("invalid length or frame rate");
        if (d.Bindings.size() > 4096 || d.Transforms.size() + d.Properties.size() + d.Activations.size() +
                                                d.Animations.size() + d.Particles.size() + d.Events.size() +
                                                d.CameraCuts.size() >
                                            65536)
            return error("too many bindings or tracks");
        std::set<std::string> ids, names;
        auto                  range = [&](double a, double b) {
            return std::isfinite(a) && std::isfinite(b) && a >= 0 && b > a && b <= d.Length;
        };
        for (const auto& b : d.Bindings)
        {
            if (b.ID.empty() || b.Name.empty() || !text(b.ID) || !text(b.Name) || !ids.insert(b.ID).second ||
                !names.insert(b.Name).second)
                return error("invalid or duplicate binding ID/name");
            if (unsigned(b.Kind) > 1 ||
                (b.Kind == TimelineBindingKind::Spawnable &&
                 (!b.Prefab.IsValid() || !range(TimelineBindingRange(d, b).Start, TimelineBindingRange(d, b).End))))
                return error("invalid spawnable " + b.ID);
            if (!finite(glm::vec4(b.SpawnTransform.Translation, 0)) || !finite(glm::vec4(b.SpawnTransform.Scale, 0)) ||
                !finite({b.SpawnTransform.Rotation.x,
                         b.SpawnTransform.Rotation.y,
                         b.SpawnTransform.Rotation.z,
                         b.SpawnTransform.Rotation.w}) ||
                glm::length(b.SpawnTransform.Rotation) < 1e-6f)
                return error("invalid spawn transform");
            for (const auto& c : b.ExpectedComponents)
                if (!text(c) || c.empty())
                    return error("invalid expected component");
            if (b.Particles && (b.Kind != TimelineBindingKind::Spawnable || !b.Particles->System.IsValid() ||
                                !std::isfinite(b.Particles->TimeScale) || b.Particles->TimeScale < 0))
                return error("invalid particle component for spawnable " + b.ID);
        }
        auto known = [&](const auto& t) { return ids.count(t.Binding) != 0; };
        auto curve = [&](const TimelineCurve& c) {
            if (c.Keys.size() > 1000000)
                return false;
            double prev = -1;
            for (const auto& k : c.Keys)
            {
                if (!std::isfinite(k.Time) || k.Time < 0 || k.Time > d.Length || k.Time <= prev || !finite(k.Value) ||
                    !finite(k.InTangent) || !finite(k.OutTangent) || unsigned(k.Interpolation) > 2)
                    return false;
                prev = k.Time;
            }
            return true;
        };
        std::set<std::string> transforms, activations, slots, properties;
        for (const auto& t : d.Transforms)
            if (!known(t) || !transforms.insert(t.Binding).second || !curve(t.Position) || !curve(t.Rotation) ||
                !curve(t.Scale))
                return error("unknown binding, duplicate transform or invalid/unsorted transform keys");
        for (const auto& t : d.Properties)
            if (!known(t) || t.Path.empty() || !text(t.Path) || !properties.insert(t.Binding + "\n" + t.Path).second ||
                !curve(t.Curve))
                return error("unknown binding, duplicate property or invalid/unsorted property keys");
        for (const auto& t : d.Activations)
        {
            if (!known(t) || !activations.insert(t.Binding).second)
                return error("unknown or duplicate activation binding");
            double prev = -1;
            for (const auto& r : t.Ranges)
            {
                if (!range(r.Start, r.End) || r.Start < prev)
                    return error("invalid/overlapping activation range");
                prev = r.End;
            }
        }
        for (const auto& t : d.Animations)
        {
            if (!known(t) || !text(t.Slot) || !slots.insert(t.Binding + "\n" + t.Slot).second)
                return error("unknown or duplicate animation binding/slot");
            for (const auto& c : t.Clips)
                if (!c.Clip.IsValid() || !range(c.Start, c.End) || !std::isfinite(c.Speed) || c.Speed <= 0 ||
                    !std::isfinite(c.ClipIn) || c.ClipIn < 0 || !std::isfinite(c.ClipOut) || c.ClipOut < 0 ||
                    (c.ClipOut != 0 && c.ClipOut <= c.ClipIn) || !std::isfinite(c.BlendIn) || c.BlendIn < 0 ||
                    !std::isfinite(c.BlendOut) || c.BlendOut < 0)
                    return error("invalid animation clip/trim/speed/blend");
        }
        std::set<std::string> particles;
        for (const auto& t : d.Particles)
        {
            const std::string label = "particle track " + t.Binding;
            if (!known(t) || !particles.insert(t.Binding).second)
                return error(label + ": unknown or duplicate binding");
            if (!std::isfinite(t.FixedTick) || t.FixedTick <= 0 || !std::isfinite(t.MaxSimulationTime) ||
                t.MaxSimulationTime < 0 || t.MaxSimulationTime / t.FixedTick > 1000000)
                return error(label + ": invalid fixed tick or simulation cap");
            if (t.Sections.size() > 65536 || t.Parameters.size() > 4096)
                return error(label + ": too many sections or parameters");
            double prev = -1;
            for (const auto& s : t.Sections)
            {
                if (!range(s.Start, s.End) || s.Start < prev || unsigned(s.OnEnd) > 1)
                    return error(label + ": invalid/overlapping section or OnEnd");
                prev = s.End;
            }
            std::set<std::string> parameters;
            for (const auto& p : t.Parameters)
                if (!text(p.Name) || p.Name.rfind("User.", 0) != 0 || p.Name.size() <= 5 ||
                    !parameters.insert(p.Name).second || !curve(p.Curve) ||
                    (p.Type != Vfx::Type::Float && p.Type != Vfx::Type::Vec2 && p.Type != Vfx::Type::Vec3 &&
                     p.Type != Vfx::Type::Vec4 && p.Type != Vfx::Type::Color))
                    return error(label + " parameter " + p.Name + ": invalid name, type or curve");
        }
        double previous = -1;
        for (const auto& e : d.Events)
        {
            if ((!e.Binding.empty() && !known(e)) || (!e.Method.empty() && e.Binding.empty()) || e.Name.empty() ||
                !text(e.Name) || !text(e.Payload) || !text(e.Method) || !std::isfinite(e.Time) || e.Time < 0 ||
                e.Time > d.Length || e.Time < previous)
                return error("unknown binding or invalid/unsorted event");
            previous = e.Time;
        }
        previous = -1;
        for (const auto& c : d.CameraCuts)
        {
            if (!known(c) || !range(c.Start, c.End) || c.Start < previous)
                return error("unknown binding or invalid/overlapping camera cut");
            previous = c.End;
        }
        return std::monostate {};
    }
    AnimationValidation ValidateTimelineParticleParameters(const TimelineParticleTrack& t, const VfxSystemAsset& asset)
    {
        const auto& parameters = asset.GetData().Settings.UserParameters;
        for (const auto& p : t.Parameters)
        {
            auto at =
                std::find_if(parameters.begin(), parameters.end(), [&](const auto& v) { return v.Name == p.Name; });
            if (at == parameters.end() || at->Kind != p.Type)
                return AssetError {AssetErrorCode::InvalidData,
                                   "Timeline: particle track " + t.Binding + " parameter " + p.Name +
                                       (at == parameters.end() ? ": unknown parameter" : ": type mismatch")};
        }
        return std::monostate {};
    }
    namespace
    {
        void Valid(const TimelineData& d)
        {
            auto v = ValidateTimeline(d);
            if (auto e = std::get_if<AssetError>(&v))
                throw std::runtime_error(e->Message);
        }
        YAML::Node Vector(glm::vec4 v)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (int i = 0; i < 4; ++i)
                n.push_back(v[i]);
            return n;
        }
        glm::vec4 Vector(const YAML::Node& n, glm::vec4 fallback = {})
        {
            if (!n)
                return fallback;
            if (n.IsScalar())
                return {n.as<float>(), 0, 0, 0};
            if (!n.IsSequence() || n.size() < 2 || n.size() > 4)
                throw std::runtime_error("Timeline: expected scalar or 2..4 vector");
            glm::vec4 v(0);
            for (size_t i = 0; i < n.size(); ++i)
                v[int(i)] = n[i].as<float>();
            return v;
        }
        YAML::Node Curve(const TimelineCurve& c)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& k : c.Keys)
            {
                YAML::Node v;
                v["Time"]          = k.Time;
                v["Value"]         = Vector(k.Value);
                v["Interpolation"] = k.Interpolation == TimelineInterpolation::Constant ? "Constant" :
                                     k.Interpolation == TimelineInterpolation::Bezier   ? "Bezier" :
                                                                                          "Linear";
                v["InTangent"]     = Vector(k.InTangent);
                v["OutTangent"]    = Vector(k.OutTangent);
                n.push_back(v);
            }
            return n;
        }
        void Sequence(const YAML::Node& n)
        {
            if (n && (!n.IsSequence() || n.size() > 1000000))
                throw std::runtime_error("Timeline: invalid sequence");
        }
        TimelineCurve Curve(const YAML::Node& n)
        {
            TimelineCurve c;
            Sequence(n);
            if (!n)
                return c;
            for (const auto& v : n)
            {
                if (!v["Value"])
                    throw std::runtime_error("Timeline: key Value is required");
                TimelineKey k;
                k.Time       = v["Time"].as<double>();
                k.Value      = Vector(v["Value"]);
                k.InTangent  = Vector(v["InTangent"]);
                k.OutTangent = Vector(v["OutTangent"]);
                auto mode    = v["Interpolation"] ? v["Interpolation"].as<std::string>() : "Linear";
                if (mode != "Constant" && mode != "Linear" && mode != "Bezier")
                    throw std::runtime_error("Timeline: unknown interpolation");
                k.Interpolation = mode == "Constant" ? TimelineInterpolation::Constant :
                                  mode == "Bezier"   ? TimelineInterpolation::Bezier :
                                                       TimelineInterpolation::Linear;
                c.Keys.push_back(k);
            }
            return c;
        }
        template<class T>
        T Get(const YAML::Node& n, const char* k, T fallback)
        {
            return n[k] ? n[k].as<T>() : fallback;
        }
        AssetID ID(const YAML::Node& n)
        {
            if (!n)
                return {};
            auto s  = n.as<std::string>();
            auto id = AssetID::FromString(s);
            if (id.ToString() != s)
                throw std::runtime_error("Timeline: invalid asset ID");
            return id;
        }
    } // namespace
    YAML::Node EncodeTimelineNode(const TimelineData& d)
    {
        Valid(d);
        YAML::Node n;
        n["Length"]    = d.Length;
        n["FrameRate"] = d.FrameRate;
        for (auto key :
             {"Bindings", "Transforms", "Properties", "Activations", "Animations", "Particles", "Events", "CameraCuts"})
            n[key] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& b : d.Bindings)
        {
            YAML::Node v;
            v["ID"]   = b.ID;
            v["Name"] = b.Name;
            v["Kind"] = b.Kind == TimelineBindingKind::Spawnable ? "Spawnable" : "Possessable";
            if (b.Skeleton.IsValid())
                v["Skeleton"] = b.Skeleton.ToString();
            v["ExpectedComponents"] = b.ExpectedComponents;
            if (b.Particles)
                v["ParticleSystem"] = EncodeParticleSystem(*b.Particles);
            if (b.Kind == TimelineBindingKind::Spawnable)
            {
                v["Prefab"]   = b.Prefab.ToString();
                v["Start"]    = b.Lifetime.Start;
                v["End"]      = b.Lifetime.End;
                v["Position"] = Vector(glm::vec4(b.SpawnTransform.Translation, 0));
                v["Rotation"] = Vector({b.SpawnTransform.Rotation.x,
                                        b.SpawnTransform.Rotation.y,
                                        b.SpawnTransform.Rotation.z,
                                        b.SpawnTransform.Rotation.w});
                v["Scale"]    = Vector(glm::vec4(b.SpawnTransform.Scale, 0));
            }
            n["Bindings"].push_back(v);
        }
        for (const auto& t : d.Transforms)
        {
            YAML::Node v;
            v["Binding"]  = t.Binding;
            v["Relative"] = t.Relative;
            v["Position"] = Curve(t.Position);
            v["Rotation"] = Curve(t.Rotation);
            v["Scale"]    = Curve(t.Scale);
            n["Transforms"].push_back(v);
        }
        for (const auto& t : d.Properties)
        {
            YAML::Node v;
            v["Binding"] = t.Binding;
            v["Path"]    = t.Path;
            v["Keys"]    = Curve(t.Curve);
            n["Properties"].push_back(v);
        }
        for (const auto& t : d.Activations)
        {
            YAML::Node v;
            v["Binding"] = t.Binding;
            v["Ranges"]  = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& r : t.Ranges)
            {
                YAML::Node a;
                a["Start"] = r.Start;
                a["End"]   = r.End;
                v["Ranges"].push_back(a);
            }
            n["Activations"].push_back(v);
        }
        for (const auto& t : d.Animations)
        {
            YAML::Node v;
            v["Binding"] = t.Binding;
            v["Slot"]    = t.Slot;
            v["Clips"]   = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& c : t.Clips)
            {
                YAML::Node a;
                a["Clip"]     = c.Clip.ToString();
                a["Start"]    = c.Start;
                a["End"]      = c.End;
                a["ClipIn"]   = c.ClipIn;
                a["ClipOut"]  = c.ClipOut;
                a["Speed"]    = c.Speed;
                a["BlendIn"]  = c.BlendIn;
                a["BlendOut"] = c.BlendOut;
                v["Clips"].push_back(a);
            }
            n["Animations"].push_back(v);
        }
        for (const auto& t : d.Particles)
        {
            YAML::Node v;
            v["Binding"]           = t.Binding;
            v["DesiredAge"]        = t.DesiredAge;
            v["FixedTick"]         = t.FixedTick;
            v["MaxSimulationTime"] = t.MaxSimulationTime;
            v["Sections"]          = YAML::Node(YAML::NodeType::Sequence);
            v["Parameters"]        = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& s : t.Sections)
            {
                YAML::Node a;
                a["Start"] = s.Start;
                a["End"]   = s.End;
                a["OnEnd"] = s.OnEnd == TimelineParticleOnEnd::Finish ? "Finish" : "Kill";
                v["Sections"].push_back(a);
            }
            for (const auto& p : t.Parameters)
            {
                YAML::Node a;
                a["Name"] = p.Name;
                a["Type"] = Vfx::TypeName(p.Type);
                a["Keys"] = Curve(p.Curve);
                v["Parameters"].push_back(a);
            }
            n["Particles"].push_back(v);
        }
        for (const auto& e : d.Events)
        {
            YAML::Node v;
            v["Time"]    = e.Time;
            v["Name"]    = e.Name;
            v["Payload"] = e.Payload;
            v["Binding"] = e.Binding;
            v["Method"]  = e.Method;
            n["Events"].push_back(v);
        }
        for (const auto& c : d.CameraCuts)
        {
            YAML::Node v;
            v["Start"]   = c.Start;
            v["End"]     = c.End;
            v["Binding"] = c.Binding;
            n["CameraCuts"].push_back(v);
        }
        return n;
    }
    TimelineData DecodeTimelineNode(const YAML::Node& n)
    {
        if (!n.IsMap())
            throw std::runtime_error("Timeline must be a YAML map");
        TimelineData d;
        d.Length    = n["Length"].as<double>();
        d.FrameRate = Get(n, "FrameRate", 30.0);
        for (auto key :
             {"Bindings", "Transforms", "Properties", "Activations", "Animations", "Particles", "Events", "CameraCuts"})
            Sequence(n[key]);
        if (n["Bindings"])
            for (const auto& v : n["Bindings"])
            {
                TimelineBinding b;
                b.ID      = v["ID"].as<std::string>();
                b.Name    = v["Name"].as<std::string>();
                auto kind = Get(v, "Kind", std::string("Possessable"));
                if (kind != "Possessable" && kind != "Spawnable")
                    throw std::runtime_error("Timeline: unknown binding kind");
                b.Kind     = kind == "Spawnable" ? TimelineBindingKind::Spawnable : TimelineBindingKind::Possessable;
                b.Prefab   = ID(v["Prefab"]);
                b.Skeleton = ID(v["Skeleton"]);
                if (v["ParticleSystem"])
                    b.Particles = DecodeParticleSystem(v["ParticleSystem"]);
                b.Lifetime = {Get(v, "Start", -1.0), Get(v, "End", 0.0)};
                b.SpawnTransform.Translation = glm::vec3(Vector(v["Position"]));
                b.SpawnTransform.Scale       = glm::vec3(Vector(v["Scale"], glm::vec4(1)));
                auto q                       = Vector(v["Rotation"], {0, 0, 0, 1});
                b.SpawnTransform.Rotation    = {q.w, q.x, q.y, q.z};
                if (v["ExpectedComponents"])
                {
                    Sequence(v["ExpectedComponents"]);
                    b.ExpectedComponents = v["ExpectedComponents"].as<std::vector<std::string>>();
                }
                d.Bindings.push_back(b);
            }
        if (n["Transforms"])
            for (const auto& v : n["Transforms"])
                d.Transforms.push_back({v["Binding"].as<std::string>(),
                                        Get(v, "Relative", true),
                                        Curve(v["Position"]),
                                        Curve(v["Rotation"]),
                                        Curve(v["Scale"])});
        if (n["Properties"])
            for (const auto& v : n["Properties"])
                d.Properties.push_back({v["Binding"].as<std::string>(), v["Path"].as<std::string>(), Curve(v["Keys"])});
        if (n["Activations"])
            for (const auto& v : n["Activations"])
            {
                TimelineActivationTrack t;
                t.Binding = v["Binding"].as<std::string>();
                Sequence(v["Ranges"]);
                if (v["Ranges"])
                    for (const auto& r : v["Ranges"])
                        t.Ranges.push_back({r["Start"].as<double>(), r["End"].as<double>()});
                d.Activations.push_back(t);
            }
        if (n["Animations"])
            for (const auto& v : n["Animations"])
            {
                TimelineAnimationTrack t;
                t.Binding = v["Binding"].as<std::string>();
                t.Slot    = Get(v, "Slot", std::string());
                Sequence(v["Clips"]);
                if (v["Clips"])
                    for (const auto& c : v["Clips"])
                        t.Clips.push_back({ID(c["Clip"]),
                                           c["Start"].as<double>(),
                                           c["End"].as<double>(),
                                           Get(c, "ClipIn", 0.0),
                                           Get(c, "ClipOut", 0.0),
                                           Get(c, "Speed", 1.0),
                                           Get(c, "BlendIn", 0.0),
                                           Get(c, "BlendOut", 0.0)});
                d.Animations.push_back(t);
            }
        if (n["Particles"])
            for (const auto& v : n["Particles"])
            {
                TimelineParticleTrack t;
                t.Binding           = v["Binding"].as<std::string>();
                t.DesiredAge        = Get(v, "DesiredAge", false);
                t.FixedTick         = Get(v, "FixedTick", 1.f / 60);
                t.MaxSimulationTime = Get(v, "MaxSimulationTime", 10.0);
                Sequence(v["Sections"]);
                Sequence(v["Parameters"]);
                if (v["Sections"])
                    for (const auto& a : v["Sections"])
                    {
                        auto end = Get(a, "OnEnd", std::string("Finish"));
                        if (end != "Finish" && end != "Kill")
                            throw std::runtime_error("Timeline: particle track " + t.Binding + " unknown OnEnd");
                        t.Sections.push_back(
                            {a["Start"].as<double>(),
                             a["End"].as<double>(),
                             end == "Finish" ? TimelineParticleOnEnd::Finish : TimelineParticleOnEnd::Kill});
                    }
                if (v["Parameters"])
                    for (const auto& a : v["Parameters"])
                    {
                        const auto               name = a["Name"].as<std::string>();
                        const auto               type = a["Type"].as<std::string>();
                        std::optional<Vfx::Type> kind;
                        for (auto k :
                             {Vfx::Type::Float, Vfx::Type::Vec2, Vfx::Type::Vec3, Vfx::Type::Vec4, Vfx::Type::Color})
                            if (type == Vfx::TypeName(k))
                                kind = k;
                        if (!kind)
                            throw std::runtime_error("Timeline: particle track " + t.Binding + " parameter " + name +
                                                     ": invalid type");
                        t.Parameters.push_back({name, *kind, Curve(a["Keys"])});
                    }
                d.Particles.push_back(std::move(t));
            }
        if (n["Events"])
            for (const auto& v : n["Events"])
                d.Events.push_back({v["Time"].as<double>(),
                                    v["Name"].as<std::string>(),
                                    Get(v, "Payload", std::string()),
                                    Get(v, "Binding", std::string()),
                                    Get(v, "Method", std::string())});
        if (n["CameraCuts"])
            for (const auto& v : n["CameraCuts"])
                d.CameraCuts.push_back(
                    {v["Start"].as<double>(), v["End"].as<double>(), v["Binding"].as<std::string>()});
        Valid(d);
        return d;
    }
    std::variant<std::vector<std::byte>, AssetError> EncodeTimelineProduct(const TimelineData& d)
    {
        try
        {
            YAML::Node n;
            n["magic"]   = "KTLN";
            n["version"] = 1;
            n["data"]    = EncodeTimelineNode(d);
            YAML::Emitter out;
            out.SetDoublePrecision(17);
            out.SetFloatPrecision(9);
            out << n;
            if (!out.good() || out.size() > 16 * 1024 * 1024)
                throw std::runtime_error("Timeline product too large");
            std::vector<std::byte> bytes(out.size());
            std::memcpy(bytes.data(), out.c_str(), out.size());
            return bytes;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    std::variant<TimelineData, AssetError> DecodeTimelineProduct(const std::byte* bytes, size_t size)
    {
        try
        {
            if (!bytes || !size || size > 16 * 1024 * 1024)
                throw std::runtime_error("Invalid Timeline product size");
            auto docs = YAML::LoadAll(std::string(reinterpret_cast<const char*>(bytes), size));
            if (docs.size() != 1)
                throw std::runtime_error("Timeline requires one document");
            auto n = docs[0];
            if (!n.IsMap() || n.size() != 3 || n["magic"].as<std::string>() != "KTLN" || n["version"].as<int>() != 1)
                throw std::runtime_error("Invalid Timeline product header");
            return DecodeTimelineNode(n["data"]);
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
