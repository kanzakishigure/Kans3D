#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Audio/AudioComponents.h"
#include "Kans3D/Scene/Entity.h"

// Editing helpers of the audio components shared by the Inspector cards, the viewport and the
// Content Browser.
namespace Kans::AudioEditing
{
    // Volume is stored linear (0..4, as the scene file and the engine take it) and shown in dB.
    // At or below SilenceDecibels the volume is 0.
    inline constexpr float SilenceDecibels = -80.0f;
    float                  VolumeToDecibels(float linear);
    float                  DecibelsToVolume(float decibels);
    // The highest volume a source takes (4, about +12 dB).
    float MaximumDecibels();

    // The Audio Source card's notes on the rows an Event takes over (null: the row is the source's own).
    // Clip: the Event picks the clip. Bus and Priority: left at their defaults, the Event's apply.
    struct EventHints
    {
        const char* Clip     = nullptr;
        const char* Bus      = nullptr;
        const char* Priority = nullptr;
    };
    EventHints EventHintsOf(const AudioSourceComponent& source);

    // Keeps edited values inside the ranges the scene serializer accepts, so every edit saves.
    void Validate(AudioSourceComponent& source);

    // Whether the file is an audio format the AudioClip importer reads (by extension, any case).
    bool IsAudioFile(const std::filesystem::path& file);

    // A new entity named `name` (the clip's file name) with an Audio Source playing `clip`, under
    // `parent` (may be null), at the world `position` when given (else the parent's origin).
    Entity PlaceAudioSource(Scene&                   scene,
                            AssetID                  clip,
                            const std::string&       name,
                            Entity                   parent   = {},
                            std::optional<glm::vec3> position = {});

    // The viewport gizmo of a selected source: the parts it draws, as world-space segments.
    enum class GizmoPart
    {
        MinDistance, // Full volume inside.
        MaxDistance, // Attenuation stops here.
        InnerCone,
        OuterCone
    };
    struct GizmoSegment
    {
        glm::vec3 A {0}, B {0};
        GizmoPart Part = GizmoPart::MinDistance;
    };
    // Distances are world units around the entity's world position (its scale does not change them);
    // the cone opens along the entity's -Z, as the engine aims it. A 2D source draws nothing; the
    // maximum distance is drawn only with an attenuation model, the cone only below 360 degrees.
    std::vector<GizmoSegment>
    SourceGizmo(const AudioSourceComponent& source, const glm::mat4& world, int segments = 48);
} // namespace Kans::AudioEditing
