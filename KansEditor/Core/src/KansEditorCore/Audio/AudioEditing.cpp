#include "AudioEditing.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/Audio/AudioAttenuationEditing.h"

namespace Kans::AudioEditing
{
    namespace
    {
        constexpr float Pi = 3.14159265358979f;
        float           Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
    } // namespace

    float VolumeToDecibels(float linear)
    {
        if (!std::isfinite(linear) || linear <= 0.0f)
            return SilenceDecibels;
        return std::max(SilenceDecibels, 20.0f * std::log10(linear));
    }

    float DecibelsToVolume(float decibels)
    {
        if (!std::isfinite(decibels) || decibels <= SilenceDecibels)
            return 0.0f;
        return std::clamp(std::pow(10.0f, decibels / 20.0f), 0.0f, 4.0f);
    }

    float MaximumDecibels() { return 20.0f * std::log10(4.0f); }

    EventHints EventHintsOf(const AudioSourceComponent& source)
    {
        EventHints hints;
        // Only an Event source plays the event (a Clip or Generator source keeps its own rows).
        if (!source.Event.IsValid() || source.GetSourceKind() != AudioSourceKind::Event)
            return hints;
        hints.Clip = "Overridden: the Event picks the clip";
        if (source.Bus.empty() || source.Bus == MasterAudioBus)
            hints.Bus = "Inherits from the event while left at Master";
        if (source.Priority == DefaultAudioVoicePriority)
            hints.Priority = "Inherits from the event while left at the default (128)";
        return hints;
    }

    void Validate(AudioSourceComponent& c)
    {
        const AudioSourceComponent d;
        c.Volume         = std::clamp(Finite(c.Volume, d.Volume), 0.0f, 4.0f);
        c.Pitch          = std::clamp(Finite(c.Pitch, d.Pitch), 0.01f, 4.0f);
        c.Pan            = std::clamp(Finite(c.Pan, d.Pan), -1.0f, 1.0f);
        c.MinDistance    = std::max(Finite(c.MinDistance, d.MinDistance), 0.001f);
        c.MaxDistance    = std::max(Finite(c.MaxDistance, d.MaxDistance), c.MinDistance);
        c.Rolloff        = std::max(Finite(c.Rolloff, d.Rolloff), 0.0f);
        c.DopplerFactor  = std::max(Finite(c.DopplerFactor, d.DopplerFactor), 0.0f);
        c.ConeOuterAngle = std::clamp(Finite(c.ConeOuterAngle, d.ConeOuterAngle), 0.0f, 360.0f);
        c.ConeInnerAngle = std::clamp(Finite(c.ConeInnerAngle, d.ConeInnerAngle), 0.0f, c.ConeOuterAngle);
        c.ConeOuterGain  = std::clamp(Finite(c.ConeOuterGain, d.ConeOuterGain), 0.0f, 1.0f);
        if (int(c.Attenuation) < 0 || c.Attenuation > AudioAttenuationModel::Exponential)
            c.Attenuation = d.Attenuation;
        if (c.Kind && (int(*c.Kind) < 0 || *c.Kind > AudioSourceKind::Generator))
            c.Kind.reset();
        c.Generator.Channels   = std::clamp<uint32_t>(c.Generator.Channels, 1, 8);
        c.Generator.SampleRate = std::clamp<uint32_t>(c.Generator.SampleRate, 8000, 192000);
    }

    std::string GeneratorUnderrunText(uint64_t frames, uint32_t sampleRate)
    {
        char text[96];
        if (frames == 0 || sampleRate == 0)
            std::snprintf(text, sizeof(text), "%llu frames", static_cast<unsigned long long>(frames));
        else
            std::snprintf(text,
                          sizeof(text),
                          "%llu frame%s (%.2f ms at %u Hz)",
                          static_cast<unsigned long long>(frames),
                          frames == 1 ? "" : "s",
                          double(frames) * 1000.0 / double(sampleRate),
                          sampleRate);
        return text;
    }

    bool IsAudioFile(const std::filesystem::path& file)
    {
        auto extension = file.extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        return extension == ".wav" || extension == ".ogg" || extension == ".mp3" || extension == ".flac";
    }

    Entity PlaceAudioSource(Scene&                   scene,
                            AssetID                  clip,
                            const std::string&       name,
                            Entity                   parent,
                            std::optional<glm::vec3> position)
    {
        Entity entity = scene.CreateEntity(name.empty() ? "Audio Source" : name);
        if (parent)
            scene.SetParent(entity, parent);
        entity.AddComponent<AudioSourceComponent>().Clip = clip;
        if (position)
        {
            glm::mat4 world = scene.GetWorldTransform(entity);
            world[3]        = glm::vec4(*position, 1.0f);
            scene.SetWorldTransform(entity, world);
        }
        return entity;
    }

    namespace
    {
        // An ellipse through centre + cos(a) x + sin(a) y.
        void Ellipse(std::vector<GizmoSegment>& out,
                     glm::vec3                  centre,
                     glm::vec3                  x,
                     glm::vec3                  y,
                     GizmoPart                  part,
                     int                        segments)
        {
            for (int i = 0; i < segments; ++i)
            {
                const float a = 2 * Pi * i / segments, b = 2 * Pi * (i + 1) / segments;
                out.push_back(
                    {centre + std::cos(a) * x + std::sin(a) * y, centre + std::cos(b) * x + std::sin(b) * y, part});
            }
        }

        // A world axis of `world`: its unit direction (the world axis when collapsed) and length.
        std::pair<glm::vec3, float> Axis(const glm::mat4& world, int index)
        {
            const glm::vec3 axis   = glm::vec3(world[index]);
            const float     length = glm::length(axis);
            if (!std::isfinite(length) || length <= 0.0f)
            {
                glm::vec3 fallback(0.0f);
                fallback[index] = 1.0f;
                return {fallback, 0.0f};
            }
            return {axis / length, length};
        }
    } // namespace

    AudioEmitterShape ShapeOf(const AudioReverbZoneComponent& zone)
    {
        AudioEmitterShape shape;
        shape.Type        = zone.Shape == AudioReverbZoneShape::Box ? AudioSourceShape::Box : AudioSourceShape::Sphere;
        shape.Radius      = zone.Radius;
        shape.HalfExtents = zone.HalfExtents;
        return shape;
    }

    void AppendShapeOutline(std::vector<GizmoSegment>& out,
                            const AudioEmitterShape&   shape,
                            const glm::mat4&           world,
                            float                      distance,
                            GizmoPart                  part,
                            int                        segments)
    {
        if (segments < 4)
            return;
        const glm::vec3 centre = glm::vec3(world[3]);
        const float     grow   = std::max(0.0f, Finite(distance, 0.0f));
        switch (shape.Type)
        {
            case AudioSourceShape::Point: {
                // Three great circles in world axes: the distance is measured in world units.
                if (grow <= 0.0f)
                    return;
                Ellipse(out, centre, {grow, 0, 0}, {0, grow, 0}, part, segments);
                Ellipse(out, centre, {grow, 0, 0}, {0, 0, grow}, part, segments);
                Ellipse(out, centre, {0, grow, 0}, {0, 0, grow}, part, segments);
                return;
            }
            case AudioSourceShape::Sphere: {
                // The (scaled) sphere's axes, each grown by the distance.
                const float radius = std::max(0.0f, shape.Radius);
                glm::vec3   axes[3];
                for (int i = 0; i < 3; ++i)
                {
                    const auto [direction, length] = Axis(world, i);
                    axes[i]                        = direction * (length * radius + grow);
                }
                Ellipse(out, centre, axes[0], axes[1], part, segments);
                Ellipse(out, centre, axes[0], axes[2], part, segments);
                Ellipse(out, centre, axes[1], axes[2], part, segments);
                return;
            }
            case AudioSourceShape::Box: {
                glm::vec3 axes[3];
                for (int i = 0; i < 3; ++i)
                {
                    const auto [direction, length] = Axis(world, i);
                    axes[i]                        = direction * (length * std::max(0.0f, shape.HalfExtents[i]) + grow);
                }
                const auto corner = [&](int bits) {
                    return centre + (bits & 1 ? axes[0] : -axes[0]) + (bits & 2 ? axes[1] : -axes[1]) +
                           (bits & 4 ? axes[2] : -axes[2]);
                };
                for (int bits = 0; bits < 8; ++bits)
                    for (int axis = 0; axis < 3; ++axis)
                        if (!(bits & (1 << axis)))
                            out.push_back({corner(bits), corner(bits | (1 << axis)), part});
                return;
            }
            case AudioSourceShape::Line: {
                const auto [direction, length] = Axis(world, 0);
                const glm::vec3 half           = direction * (length * 0.5f * std::max(0.0f, shape.Length));
                if (grow <= 0.0f)
                {
                    out.push_back({centre - half, centre + half, part});
                    return;
                }
                // A capsule: rings around both ends, four lines along the sides, and caps.
                const glm::vec3 up   = std::abs(direction.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                const glm::vec3 side = glm::normalize(glm::cross(direction, up)) * grow;
                const glm::vec3 lift = glm::normalize(glm::cross(side, direction)) * grow;
                for (const float end : {-1.0f, 1.0f})
                {
                    const glm::vec3 at      = centre + half * end;
                    const glm::vec3 outward = direction * (grow * end);
                    Ellipse(out, at, side, lift, part, segments);
                    // Half circles over the end, in two planes through the axis.
                    for (const glm::vec3 across : {side, lift})
                        for (int i = 0; i < segments / 2; ++i)
                        {
                            const float a = Pi * (float(i) / float(segments / 2) - 0.5f);
                            const float b = Pi * (float(i + 1) / float(segments / 2) - 0.5f);
                            out.push_back({at + std::sin(a) * across + std::cos(a) * outward,
                                           at + std::sin(b) * across + std::cos(b) * outward,
                                           part});
                        }
                }
                for (const glm::vec3 offset : {side, -side, lift, -lift})
                    out.push_back({centre - half + offset, centre + half + offset, part});
                return;
            }
        }
    }

    std::vector<GizmoSegment> SourceGizmo(const AudioSourceComponent& source, const glm::mat4& world, int segments)
    {
        return SourceGizmo(source, world, nullptr, segments);
    }

    std::vector<GizmoSegment> SourceGizmo(const AudioSourceComponent& source,
                                          const glm::mat4&            world,
                                          const AudioAttenuationData* asset,
                                          int                         segments)
    {
        std::vector<GizmoSegment> out;
        if (!source.Spatial || segments < 4)
            return out;
        const glm::vec3 origin = glm::vec3(world[3]);
        if (source.Shape.Type != AudioSourceShape::Point)
            AppendShapeOutline(out, source.Shape, world, 0.0f, GizmoPart::Shape, segments);
        const auto range =
            asset ? AudioAttenuationEditing::RangeOf(*asset) :
                    AudioAttenuationEditing::RangeOf(source.Attenuation, source.MinDistance, source.MaxDistance);
        if (range.Inner > 0.0f)
            AppendShapeOutline(out, source.Shape, world, range.Inner, GizmoPart::MinDistance, segments);
        if (range.HasOuter)
            AppendShapeOutline(out, source.Shape, world, range.Outer, GizmoPart::MaxDistance, segments);
        float inner = source.ConeInnerAngle, outer = source.ConeOuterAngle;
        if (asset && asset->Cone)
            inner = asset->Cone->InnerAngle, outer = asset->Cone->OuterAngle;
        if (outer >= 360.0f)
            return out;
        // The engine aims a source along its -Z; the cone's angles are full apex angles.
        glm::vec3   forward = glm::vec3(world * glm::vec4(0, 0, -1, 0));
        const float length2 = glm::dot(forward, forward);
        forward            = std::isfinite(length2) && length2 > 0 ? forward / std::sqrt(length2) : glm::vec3(0, 0, -1);
        const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 side   = glm::normalize(glm::cross(forward, up));
        const glm::vec3 normal = glm::cross(side, forward);
        const float     reach  = std::max(range.Inner, 0.5f);
        const auto      cone   = [&](float degrees, GizmoPart part) {
            const float half   = glm::radians(std::clamp(degrees, 0.0f, 359.0f)) * 0.5f;
            const auto  end    = origin + forward * (reach * std::cos(half));
            const float radius = reach * std::sin(half);
            Ellipse(out, end, side * radius, normal * radius, part, segments);
            for (int i = 0; i < 4; ++i)
            {
                const float a = Pi * 0.5f * i;
                out.push_back({origin, end + radius * (std::cos(a) * side + std::sin(a) * normal), part});
            }
        };
        cone(inner, GizmoPart::InnerCone);
        cone(outer, GizmoPart::OuterCone);
        return out;
    }

    std::vector<GizmoSegment>
    ReverbZoneGizmo(const AudioReverbZoneComponent& zone, const glm::mat4& world, int segments)
    {
        std::vector<GizmoSegment> out;
        const auto                shape = ShapeOf(zone);
        AppendShapeOutline(out, shape, world, 0.0f, GizmoPart::ZoneShape, segments);
        if (zone.FadeDistance > 0.0f)
            AppendShapeOutline(out, shape, world, zone.FadeDistance, GizmoPart::ZoneFade, segments);
        return out;
    }

    HandleFrame ZoneFadeHandle(const AudioReverbZoneComponent& zone, const glm::mat4& world)
    {
        const auto [direction, length] = Axis(world, 0);
        const float surface =
            length * std::max(0.0f, zone.Shape == AudioReverbZoneShape::Box ? zone.HalfExtents.x : zone.Radius);
        return {glm::vec3(world[3]) + direction * surface, direction};
    }

    HandleFrame LineLengthHandle(const glm::mat4& world, bool positive)
    {
        const glm::vec3 axis = glm::vec3(world[0]);
        return {glm::vec3(world[3]), positive ? axis : -axis};
    }

    float SendToField(float db) { return std::isfinite(db) && db > SilenceDecibels ? db : SilenceDecibels; }

    float FieldToSend(float field) { return std::isfinite(field) && field > SilenceDecibels ? field : AudioSendOffDb; }

    std::string ListenerSlotText(bool active, std::optional<uint32_t> slot, uint32_t capacity)
    {
        if (slot)
            return "Listener " + std::to_string(*slot + 1);
        if (!active)
            return "Inactive";
        return "Inactive — over the limit of " + std::to_string(capacity);
    }
} // namespace Kans::AudioEditing
