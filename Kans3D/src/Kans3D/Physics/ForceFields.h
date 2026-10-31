#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
namespace Kans
{
    enum class FieldShape : uint32_t
    {
        Infinite,
        Sphere,
        Box
    };
    enum class FieldFalloff : uint32_t
    {
        None,
        Linear,
        InverseSquare,
        Curve
    };
    enum class FieldType : uint32_t
    {
        Wind,
        Radial,
        Directional,
        Vortex,
        Noise,
        Drag
    };
    enum class FieldSystem : uint32_t
    {
        RigidBodies = 1,
        Cloth       = 2,
        SpringBones = 4,
        Particles   = 8
    };
    struct WindZoneComponent
    {
        FieldShape Shape = FieldShape::Infinite;
        glm::vec3  Direction {1, 0, 0};
        float      Strength = 0, TurbulenceAmplitude = 0, TurbulenceFrequency = 1;
        float      PulseMagnitude = 0, PulseFrequency = 1;
        float      Radius = 1, FalloffBand = .2f;
        glm::vec3  HalfExtents {1};
        uint32_t   LayerMask = UINT32_MAX, AffectMask = 15;
    };
    struct ForceFieldComponent
    {
        FieldType    Type    = FieldType::Radial;
        FieldShape   Shape   = FieldShape::Sphere;
        FieldFalloff Falloff = FieldFalloff::Linear;
        glm::vec3    Direction {0, 1, 0}, HalfExtents {1}, Scroll {0};
        float        Radius = 1, Strength = 1, InwardPull = 0, Frequency = 1;
        bool         Acceleration = true, CurlNoise = true;
        uint32_t     LayerMask = UINT32_MAX, AffectMask = 15, Seed = 0;
        // Uniform samples over normalized distance [0,1]. No pointers in the uploaded record.
        std::array<float, 8> FalloffCurve {1, .8571429f, .7142857f, .5714286f, .4285714f, .2857143f, .1428571f, 0};
    };
    template<class C, class F>
    void ForceFieldFields(C& c, F f)
    {
        f("Type", c.Type);
        f("Shape", c.Shape);
        f("Falloff", c.Falloff);
        f("Direction", c.Direction);
        f("HalfExtents", c.HalfExtents);
        f("Scroll", c.Scroll);
        f("Radius", c.Radius);
        f("Strength", c.Strength);
        f("InwardPull", c.InwardPull);
        f("Frequency", c.Frequency);
        f("Acceleration", c.Acceleration);
        f("CurlNoise", c.CurlNoise);
        f("LayerMask", c.LayerMask);
        f("AffectMask", c.AffectMask);
        f("Seed", c.Seed);
        f("FalloffCurve", c.FalloffCurve);
    }
    template<class C, class F>
    void WindZoneFields(C& c, F f)
    {
        f("Shape", c.Shape);
        f("Direction", c.Direction);
        f("Strength", c.Strength);
        f("TurbulenceAmplitude", c.TurbulenceAmplitude);
        f("TurbulenceFrequency", c.TurbulenceFrequency);
        f("PulseMagnitude", c.PulseMagnitude);
        f("PulseFrequency", c.PulseFrequency);
        f("Radius", c.Radius);
        f("FalloffBand", c.FalloffBand);
        f("HalfExtents", c.HalfExtents);
        f("LayerMask", c.LayerMask);
        f("AffectMask", c.AffectMask);
    }
    // A fixed, 16-byte aligned record. uint4 metadata and float4 payloads map directly to GPU buffers.
    struct alignas(16) FieldRecord
    {
        glm::uvec4           Kind {0}, Filter {0}; // type/shape/falloff/acceleration; layers/systems/seed/curl
        glm::vec4            PositionRadius {0}, Rotation {0, 0, 0, 1}, ExtentsBand {1}, DirectionStrength {0};
        glm::vec4            NoisePulse {0}, ScrollInward {0};
        std::array<float, 8> Curve {};
    };
    struct FieldImpulse
    {
        glm::vec3    Position {0}, Impulse {0};
        float        Radius          = 0;
        FieldFalloff Falloff         = FieldFalloff::Linear;
        float        UpwardsModifier = 0;
        uint32_t     LayerMask = UINT32_MAX, AffectMask = 15;
        bool         Radial   = false;
        uint64_t     Sequence = 0;
        glm::vec3 Sample(glm::vec3 position, uint32_t layer = 0, FieldSystem system = FieldSystem::RigidBodies) const;
    };
    struct FieldSample
    {
        glm::vec3 Wind {0}, Force {0}, Acceleration {0};
    };
    class ForceFieldSet
    {
    public:
        void Clear()
        {
            m_Records.clear();
            m_Impulses.clear();
        }
        void AddGlobalWind(const WindZoneComponent& w) { AddWind(w); }
        void AddWind(const WindZoneComponent&, const glm::mat4& world = glm::mat4(1));
        void AddField(const ForceFieldComponent&, const glm::mat4& world = glm::mat4(1));
        void AddImpulse(FieldImpulse impulse)
        {
            if (!impulse.Sequence)
                impulse.Sequence = ++m_NextSequence;
            m_Impulses.push_back(impulse);
        }
        void        SetImpulses(const std::vector<FieldImpulse>& impulses) { m_Impulses = impulses; }
        FieldSample Sample(glm::vec3 position, glm::vec3 velocity, double time, uint32_t layer, FieldSystem) const;
        glm::vec3   SampleWind(glm::vec3 position, double time) const
        {
            return Sample(position, {}, time, 0, FieldSystem::SpringBones).Wind;
        }
        const std::vector<FieldRecord>&  GetRecords() const { return m_Records; }
        const std::vector<FieldImpulse>& GetImpulses() const { return m_Impulses; }

    private:
        uint64_t                  m_NextSequence = 0;
        std::vector<FieldRecord>  m_Records;
        std::vector<FieldImpulse> m_Impulses;
    };
} // namespace Kans
