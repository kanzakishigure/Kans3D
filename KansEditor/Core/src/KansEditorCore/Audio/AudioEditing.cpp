#include "AudioEditing.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"

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
        if (!source.Event.IsValid())
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

    std::vector<GizmoSegment> SourceGizmo(const AudioSourceComponent& source, const glm::mat4& world, int segments)
    {
        std::vector<GizmoSegment> out;
        if (!source.Spatial || segments < 4)
            return out;
        const glm::vec3 origin = glm::vec3(world[3]);
        const auto      circle =
            [&](const glm::vec3& centre, const glm::vec3& x, const glm::vec3& y, float radius, GizmoPart part) {
                for (int i = 0; i < segments; ++i)
                {
                    const float a = 2 * Pi * i / segments, b = 2 * Pi * (i + 1) / segments;
                    out.push_back({centre + radius * (std::cos(a) * x + std::sin(a) * y),
                                   centre + radius * (std::cos(b) * x + std::sin(b) * y),
                                   part});
                }
            };
        // Three great circles in world axes: the distance is measured in world units.
        const auto sphere = [&](float radius, GizmoPart part) {
            circle(origin, {1, 0, 0}, {0, 1, 0}, radius, part);
            circle(origin, {1, 0, 0}, {0, 0, 1}, radius, part);
            circle(origin, {0, 1, 0}, {0, 0, 1}, radius, part);
        };
        sphere(source.MinDistance, GizmoPart::MinDistance);
        if (source.Attenuation != AudioAttenuationModel::None && source.MaxDistance > source.MinDistance)
            sphere(source.MaxDistance, GizmoPart::MaxDistance);
        if (source.ConeOuterAngle >= 360.0f)
            return out;
        // The engine aims a source along its -Z; the cone's angles are full apex angles.
        glm::vec3   forward = glm::vec3(world * glm::vec4(0, 0, -1, 0));
        const float length2 = glm::dot(forward, forward);
        forward            = std::isfinite(length2) && length2 > 0 ? forward / std::sqrt(length2) : glm::vec3(0, 0, -1);
        const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 side   = glm::normalize(glm::cross(forward, up));
        const glm::vec3 normal = glm::cross(side, forward);
        const float     reach  = std::max(source.MinDistance, 0.5f);
        const auto      cone   = [&](float degrees, GizmoPart part) {
            const float half = glm::radians(std::clamp(degrees, 0.0f, 359.0f)) * 0.5f;
            const auto  end  = origin + forward * (reach * std::cos(half));
            circle(end, side, normal, reach * std::sin(half), part);
            for (int i = 0; i < 4; ++i)
            {
                const float a = Pi * 0.5f * i;
                out.push_back(
                    {origin, end + reach * std::sin(half) * (std::cos(a) * side + std::sin(a) * normal), part});
            }
        };
        cone(source.ConeInnerAngle, GizmoPart::InnerCone);
        cone(source.ConeOuterAngle, GizmoPart::OuterCone);
        return out;
    }
} // namespace Kans::AudioEditing
