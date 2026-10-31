#include "ForceFields.h"
#include <cmath>
#include <cstdint>
namespace Kans
{
    namespace
    {
        float Noise(glm::vec3 p)
        {
            const auto cell = glm::ivec3(glm::floor(p));
            auto       f    = glm::fract(p);
            f               = f * f * (3.f - 2.f * f);
            float result    = 0;
            for (uint32_t i = 0; i < 8; ++i)
            {
                uint32_t h = uint32_t(cell.x + int(i & 1)) * 0x8da6b343u ^
                             uint32_t(cell.y + int((i >> 1) & 1)) * 0xd8163841u ^
                             uint32_t(cell.z + int(i >> 2)) * 0xcb1ab31fu;
                h ^= h >> 16;
                h *= 0x7feb352du;
                h ^= h >> 15;
                h *= 0x846ca68bu;
                h ^= h >> 16;
                float v = float(h & 0xffffffu) / float(0xffffffu) * 2.f - 1.f;
                result += v * ((i & 1) ? f.x : 1 - f.x) * ((i & 2) ? f.y : 1 - f.y) * ((i & 4) ? f.z : 1 - f.z);
            }
            return result;
        }
    } // namespace
    namespace
    {
        float Attenuation(FieldFalloff mode, float d, const std::array<float, 8>& curve = {})
        {
            switch (mode)
            {
                case FieldFalloff::Linear:
                    return 1 - d;
                case FieldFalloff::InverseSquare:
                    return 1 / (1 + 99 * d * d);
                case FieldFalloff::Curve: {
                    float x = glm::clamp(d, 0.f, 1.f) * 7;
                    auto  i = std::min(6u, uint32_t(x));
                    return glm::mix(curve[i], curve[i + 1], x - i);
                }
                default:
                    return 1;
            }
        }
        glm::vec3 Unit(glm::vec3 v)
        {
            float n = glm::length(v);
            return n > 1e-8f ? v / n : glm::vec3(0);
        }
        FieldRecord Record(FieldShape shape, const glm::mat4& world, float radius, glm::vec3 extents)
        {
            FieldRecord r;
            r.Kind.y         = uint32_t(shape);
            r.PositionRadius = glm::vec4(glm::vec3(world[3]), radius);
            glm::vec3 scale(
                glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
            glm::mat3 rotation(1);
            for (int i = 0; i < 3; ++i)
                if (scale[i] > 1e-8f)
                    rotation[i] = glm::vec3(world[i]) / scale[i];
            auto q        = glm::normalize(glm::quat_cast(rotation));
            r.Rotation    = {q.x, q.y, q.z, q.w};
            r.ExtentsBand = glm::vec4(extents * scale, 0);
            r.PositionRadius.w *= std::max(scale.x, std::max(scale.y, scale.z));
            return r;
        }
    } // namespace
    void ForceFieldSet::AddWind(const WindZoneComponent& w, const glm::mat4& world)
    {
        auto r              = Record(w.Shape, world, w.Radius, w.HalfExtents);
        r.Kind.x            = uint32_t(FieldType::Wind);
        r.Filter            = {w.LayerMask, w.AffectMask, 0, 0};
        r.DirectionStrength = glm::vec4(w.Direction, w.Strength);
        r.ExtentsBand.w     = w.FalloffBand;
        r.NoisePulse        = {w.TurbulenceAmplitude, w.TurbulenceFrequency, w.PulseMagnitude, w.PulseFrequency};
        m_Records.push_back(r);
    }
    void ForceFieldSet::AddField(const ForceFieldComponent& f, const glm::mat4& world)
    {
        auto r              = Record(f.Shape, world, f.Radius, f.HalfExtents);
        r.Kind              = {uint32_t(f.Type), uint32_t(f.Shape), uint32_t(f.Falloff), f.Acceleration};
        r.Filter            = {f.LayerMask, f.AffectMask, f.Seed, f.CurlNoise};
        r.DirectionStrength = glm::vec4(f.Direction, f.Strength);
        r.NoisePulse.y      = f.Frequency;
        r.ScrollInward      = glm::vec4(f.Scroll, f.InwardPull);
        r.Curve             = f.FalloffCurve;
        m_Records.push_back(r);
    }
    FieldSample
    ForceFieldSet::Sample(glm::vec3 position, glm::vec3 velocity, double time, uint32_t layer, FieldSystem system) const
    {
        FieldSample out;
        if (layer >= 32)
            return out;
        for (const auto& r : m_Records)
        {
            if (!(r.Filter.x & (1u << layer)) || !(r.Filter.y & uint32_t(system)))
                continue;
            glm::quat q(r.Rotation.w, r.Rotation.x, r.Rotation.y, r.Rotation.z);
            auto      delta = position - glm::vec3(r.PositionRadius);
            auto      local = glm::conjugate(q) * delta;
            float     d     = 0;
            if (r.Kind.y == uint32_t(FieldShape::Sphere))
                d = glm::length(delta) / std::max(1e-8f, r.PositionRadius.w);
            if (r.Kind.y == uint32_t(FieldShape::Box))
            {
                auto v = glm::abs(local) / glm::max(glm::vec3(r.ExtentsBand), glm::vec3(1e-8f));
                d      = std::max(v.x, std::max(v.y, v.z));
            }
            if (d > 1)
                continue;
            const auto type        = FieldType(r.Kind.x);
            float      attenuation = type == FieldType::Wind ?
                                         (r.ExtentsBand.w > 0 ? glm::clamp((1 - d) / r.ExtentsBand.w, 0.f, 1.f) : 1.f) :
                                         Attenuation(FieldFalloff(r.Kind.z), d, r.Curve);
            glm::vec3  v(0), axis = q * Unit(glm::vec3(r.DirectionStrength));
            auto       p     = position * r.NoisePulse.y + glm::vec3(r.ScrollInward) * float(time) +
                               glm::vec3(float(r.Filter.z % 65536));
            auto       noise = [](glm::vec3 x) {
                return glm::vec3(Noise(x), Noise(x + glm::vec3(19, 37, 11)), Noise(x + glm::vec3(47, 7, 29)));
            };
            switch (type)
            {
                case FieldType::Wind:
                    p = position * r.NoisePulse.y + glm::vec3(float(time * r.NoisePulse.y));
                    out.Wind +=
                        attenuation * (axis * (r.DirectionStrength.w +
                                               r.NoisePulse.z * std::sin(float(time) * r.NoisePulse.w * 6.283185307f)) +
                                       r.NoisePulse.x * noise(p));
                    continue;
                case FieldType::Radial:
                    v = Unit(delta);
                    break;
                case FieldType::Directional:
                    v = axis;
                    break;
                case FieldType::Vortex: {
                    auto radial = delta - axis * glm::dot(delta, axis);
                    v           = Unit(glm::cross(axis, radial)) - r.ScrollInward.w * Unit(radial);
                    break;
                }
                case FieldType::Drag:
                    v = -velocity;
                    break;
                case FieldType::Noise:
                    if (r.Filter.w)
                    {
                        constexpr float e  = .01f;
                        auto            dx = (noise(p + glm::vec3(e, 0, 0)) - noise(p - glm::vec3(e, 0, 0))) / (2 * e);
                        auto            dy = (noise(p + glm::vec3(0, e, 0)) - noise(p - glm::vec3(0, e, 0))) / (2 * e);
                        auto            dz = (noise(p + glm::vec3(0, 0, e)) - noise(p - glm::vec3(0, 0, e))) / (2 * e);
                        v                  = {dy.z - dz.y, dz.x - dx.z, dx.y - dy.x};
                    }
                    else
                        v = noise(p);
                    break;
            }
            (r.Kind.w ? out.Acceleration : out.Force) += v * r.DirectionStrength.w * attenuation;
        }
        return out;
    }
    glm::vec3 FieldImpulse::Sample(glm::vec3 position, uint32_t layer, FieldSystem system) const
    {
        if (layer >= 32 || !(LayerMask & (1u << layer)) || !(AffectMask & uint32_t(system)) || Radius <= 0)
            return {};
        float distance = glm::distance(position, Position);
        if (distance > Radius)
            return {};
        auto v = Radial ? Unit(position - (Position - glm::vec3(0, UpwardsModifier, 0))) * Impulse.x : Impulse;
        return v * Attenuation(Falloff, distance / Radius);
    }
} // namespace Kans
