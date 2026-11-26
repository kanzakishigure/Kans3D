#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Audio/AudioAttenuation.h"
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
        OuterCone,
        Shape,     // A sphere, box or line source's shape.
        ZoneShape, // A reverb zone: full send inside.
        ZoneFade   // The fade band's outer edge.
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
    // With the attenuation asset the source plays with (its own, else its event's; null for the
    // built-in model): the ranges follow the asset (where its curve first reaches 0, or its model's
    // max distance, and the end of its full-volume start), its cone replaces the source's when it
    // has one, and a shaped source also draws its shape. Ranges grow from the shape's surface.
    std::vector<GizmoSegment> SourceGizmo(const AudioSourceComponent& source,
                                          const glm::mat4&            world,
                                          const AudioAttenuationData* asset,
                                          int                         segments = 48);
    // A reverb zone: its shape (ZoneShape) and the fade band's outer edge (ZoneFade), FadeDistance
    // world metres outside it.
    std::vector<GizmoSegment>
    ReverbZoneGizmo(const AudioReverbZoneComponent& zone, const glm::mat4& world, int segments = 48);
    // The outline of `shape` placed by `world`, grown by `distance` world metres (a point: three
    // great circles of that radius; a line: a capsule).
    void              AppendShapeOutline(std::vector<GizmoSegment>& out,
                                         const AudioEmitterShape&   shape,
                                         const glm::mat4&           world,
                                         float                      distance,
                                         GizmoPart                  part,
                                         int                        segments = 48);
    AudioEmitterShape ShapeOf(const AudioReverbZoneComponent& zone);

    // Where a viewport handle sits and the way it pulls (one unit of the handle's value per unit
    // of Direction).
    struct HandleFrame
    {
        glm::vec3 Origin {0}, Direction {1, 0, 0};
    };
    // The fade distance's handle: on the zone's surface along its +X axis, pulling outward in
    // world metres.
    HandleFrame ZoneFadeHandle(const AudioReverbZoneComponent& zone, const glm::mat4& world);
    // A line source's end along +X (`positive`) or -X: the value is half the line's length.
    HandleFrame LineLengthHandle(const glm::mat4& world, bool positive);

    // A send level as its field shows it: dB, with Off (-inf) at SilenceDecibels and below.
    float SendToField(float db);
    float FieldToSend(float field);
    // The listener slot readout: "Listener 2", "Inactive", or "Inactive — over the limit of 4".
    std::string ListenerSlotText(bool active, std::optional<uint32_t> slot, uint32_t capacity);
} // namespace Kans::AudioEditing
