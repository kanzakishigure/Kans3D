#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <glm/vec3.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Audio/AudioOcclusion.h"
#include "Kans3D/Audio/AudioSpatializer.h"
#include "Kans3D/Physics/AcousticMaterial.h"

namespace Kans
{
    class AudioEngine;
    class AssetManager;
    struct EditorAssetImportSnapshot;
} // namespace Kans

// Editing helpers of the spatializer (panning or HRTF) and occlusion settings shared by the Audio
// Source card, the AudioEvent editor, Project Settings > Audio, the physics material editor, the
// Content Browser and the viewport's occlusion-ray overlay.
namespace Kans::AudioSpatialEditing
{
    // ---- Spatializer ----
    const char* SpatializerName(AudioSpatializer spatializer); // "Panning", "HRTF".
    // What a voice plays with: its own choice, else the event's, else the project default.
    AudioSpatializer ResolveSpatializer(std::optional<AudioSpatializer> own,
                                        std::optional<AudioSpatializer> event,
                                        AudioSpatializer                project);
    // Why an HRTF voice is panned instead ("" for None).
    const char* FallbackText(AudioHRTFFallback fallback);
    // The card's note when HRTF is chosen but the output is not stereo (the engine needs a stereo mix
    // and a stereo device); null otherwise.
    const char* StereoHint(AudioSpatializer resolved, uint32_t mixChannels, uint32_t outputChannels);
    // The same for an engine (null engine: no note).
    const char* StereoHint(AudioSpatializer resolved, const AudioEngine* engine);

    // ---- Occlusion ----
    inline constexpr const char* OcclusionModeNames[]    = {"Off", "Raycast", "Volumetric"};
    inline constexpr const char* TransmissionModeNames[] = {"Off", "Frequency independent", "Frequency dependent"};
    // The settings a source inherits: the event's overrides (when it has an event) over the project's.
    AudioOcclusionSettings InheritedOcclusion(const AudioOcclusionOverrides* event,
                                              const AudioOcclusionSettings&  project);
    // Keeps edited overrides inside the ranges the engine and the serializers accept: strengths 0..1,
    // radius 0..1000 m, 1..32 samples (non-finite values fall back to the defaults).
    void Clamp(AudioOcclusionOverrides& overrides);
    // "0.10 / 0.05 / 0.03" (low, mid, high).
    void FormatBands(const std::array<float, 3>& bands, char* out, size_t size);

    // ---- Acoustic materials ----
    inline constexpr int AcousticPresetCount = int(AcousticMaterialPreset::Rock) + 1;
    // The preset whose values these are exactly, or nothing (Custom).
    std::optional<AcousticMaterialPreset> MatchAcousticPreset(const AcousticMaterial& material);
    // The preset dropdown's label: the preset's name, or "Custom".
    const char* AcousticPresetLabel(const AcousticMaterial& material);
    // Coefficients clamped to 0..1 (non-finite values keep `previous`'s).
    void ClampAcoustics(AcousticMaterial& material, const AcousticMaterial& previous);

    // ---- The viewport's occlusion rays ----
    // Records what the engine's occlusion debug hook reports, so a viewport can draw it: the clear part
    // of each ray (start to first hit, or the whole ray) and the blocked part (first hit to the end),
    // with the hit points. Rays stay for HoldFrames drawn frames, since a voice's rays are cast only
    // when it is refreshed (every few frames). Fixed storage: recording never allocates.
    class OcclusionRayRecorder
    {
    public:
        static constexpr size_t   Capacity   = 512;
        static constexpr uint64_t HoldFrames = DefaultOcclusionStaleFrames;
        struct Ray
        {
            glm::vec3                                       Start {0}, End {0};
            std::array<glm::vec3, MaxAudioTransmissionHits> Hits {};
            uint32_t                                        HitCount = 0;
            uint64_t                                        Frame    = 0;
            bool                                            Blocked() const { return HitCount > 0; }
            // Where the clear part ends: the first hit, or the end.
            glm::vec3 ClearEnd() const { return HitCount ? Hits[0] : End; }
        };

        OcclusionRayRecorder() = default;
        ~OcclusionRayRecorder() { Remove(); }
        OcclusionRayRecorder(const OcclusionRayRecorder&)            = delete;
        OcclusionRayRecorder& operator=(const OcclusionRayRecorder&) = delete;

        // Makes this recorder `engine`'s occlusion debug hook (again when the engine was initialized anew),
        // removing it from another engine first. The engine must outlive the installation.
        void Install(AudioEngine& engine);
        // Clears the hook of the engine it is installed on, and the recorded rays.
        void         Remove();
        bool         IsInstalledOn(const AudioEngine& engine) const;
        AudioEngine* GetEngine() const { return m_Engine; }

        void Record(const AudioOcclusionRay& ray, const AudioOcclusionHit* hits, size_t count);
        // A drawn frame passed: rays older than HoldFrames are dropped.
        void       EndFrame();
        void       Clear() { m_Start = m_Count = 0; }
        size_t     Size() const { return m_Count; }
        const Ray& At(size_t i) const { return m_Rays[(m_Start + i) % Capacity]; }
        uint64_t   GetFrame() const { return m_Frame; }

    private:
        static void Hook(void* context, const AudioOcclusionRay& ray, const AudioOcclusionHit* hits, size_t count);

        std::array<Ray, Capacity> m_Rays {};
        size_t                    m_Start = 0, m_Count = 0;
        uint64_t                  m_Frame    = 0;
        AudioEngine*              m_Engine   = nullptr;
        uint64_t                  m_Instance = 0;
    };

    // ---- HRTF assets in the Content Browser ----
    // The last failed import of each source, from the import service's snapshots: a failed build of a
    // source records its reason, a successful one forgets it.
    class ImportFailures
    {
    public:
        void               Observe(const EditorAssetImportSnapshot& snapshot);
        const std::string* Find(SourceAssetID source) const;

    private:
        std::unordered_map<SourceAssetID, std::string> m_Reasons;
        uint64_t                                       m_Session = 0;
        bool                                           m_Seen    = false;
    };
    // "On" or "Off": the HRTF importer's volume normalisation from a source's import settings.
    const char* NormalizeVolumeText(const std::map<std::string, std::string>& settings);
    bool        IsHRTFFile(const std::filesystem::path& file);

    // ---- Project Settings > Audio ----
    struct ProjectAudioValues
    {
        AssetID                 HRTF; // Invalid: Steam Audio's built-in HRTF.
        AudioSpatializer        DefaultSpatializer = AudioSpatializer::Panning;
        AudioOcclusionOverrides DefaultOcclusion;
        uint32_t                MaxOcclusionRaysPerFrame = DefaultMaxOcclusionRaysPerFrame;
        uint32_t                OcclusionStaleFrames     = DefaultOcclusionStaleFrames;
        bool                    operator==(const ProjectAudioValues& other) const;
        bool                    operator!=(const ProjectAudioValues& other) const { return !(*this == other); }
    };
    inline constexpr uint32_t MaxRaysPerFrameLimit = 4096, MaxStaleFramesLimit = 600;

    // The project's audio settings: read from the .kproj, applied to the editor's engine at once and
    // written back (keeping the file's other fields) when Save is called.
    class ProjectAudioSettings
    {
    public:
        std::optional<AssetError>    Load(const std::filesystem::path& projectFile);
        const ProjectAudioValues&    Get() const { return m_Values; }
        const std::filesystem::path& GetFile() const { return m_File; }
        // Validates `values` and makes them current; the engine (when given) takes the defaults at once
        // and loads a changed HRTF through `assets`. An invalid value changes nothing and returns the
        // reason. A HRTF the engine cannot load is kept (the engine plays the built-in one) and its
        // reason is kept in GetHRTFError().
        std::optional<std::string> Apply(const ProjectAudioValues& values, AudioEngine* engine, AssetManager* assets);
        bool                       IsDirty() const { return m_Dirty; }
        std::optional<AssetError>  Save();
        const std::string&         GetHRTFError() const { return m_HRTFError; }

    private:
        std::filesystem::path m_File;
        ProjectAudioValues    m_Values;
        bool                  m_Dirty = false;
        std::string           m_HRTFError;
    };
} // namespace Kans::AudioSpatialEditing
