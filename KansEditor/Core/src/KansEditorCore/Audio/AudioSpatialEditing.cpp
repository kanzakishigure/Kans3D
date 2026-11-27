#include "AudioSpatialEditing.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Project/ProjectFile.h"
#include "KansEditorCore/EditorAssetImportService.h"

namespace Kans::AudioSpatialEditing
{
    const char* SpatializerName(AudioSpatializer spatializer)
    {
        return spatializer == AudioSpatializer::HRTF ? "HRTF" : "Panning";
    }

    AudioSpatializer ResolveSpatializer(std::optional<AudioSpatializer> own,
                                        std::optional<AudioSpatializer> event,
                                        AudioSpatializer                project)
    {
        return own ? *own : event ? *event : project;
    }

    const char* FallbackText(AudioHRTFFallback fallback)
    {
        switch (fallback)
        {
            case AudioHRTFFallback::None:
                return "";
            case AudioHRTFFallback::NonSpatial:
                return "Panned: a 2D sound has no direction to render";
            case AudioHRTFFallback::NonStereoOutput:
                return "Panned: HRTF needs stereo output";
            case AudioHRTFFallback::SteamAudioUnavailable:
                return "Panned: Steam Audio is not available";
            case AudioHRTFFallback::EffectCreationFailed:
                return "Panned: the binaural effect could not be created";
        }
        return "";
    }

    const char* StereoHint(AudioSpatializer resolved, uint32_t mixChannels, uint32_t outputChannels)
    {
        if (resolved != AudioSpatializer::HRTF || (mixChannels == 2 && outputChannels == 2))
            return nullptr;
        return "HRTF needs stereo output; this voice will be panned";
    }

    const char* StereoHint(AudioSpatializer resolved, const AudioEngine* engine)
    {
        if (!engine || !engine->IsInitialized())
            return nullptr;
        return StereoHint(resolved, engine->GetChannels(), engine->GetOutputFormat().Channels);
    }

    AudioOcclusionSettings InheritedOcclusion(const AudioOcclusionOverrides* event,
                                              const AudioOcclusionSettings&  project)
    {
        return event ? event->Resolve(project) : project;
    }

    void Clamp(AudioOcclusionOverrides& o)
    {
        const AudioOcclusionSettings defaults;
        const auto                   unit = [](std::optional<float>& value, float fallback) {
            if (value)
                value = std::isfinite(*value) ? std::clamp(*value, 0.0f, 1.0f) : fallback;
        };
        unit(o.OcclusionStrength, defaults.OcclusionStrength);
        unit(o.TransmissionStrength, defaults.TransmissionStrength);
        if (o.SourceRadius)
            o.SourceRadius =
                std::isfinite(*o.SourceRadius) ? std::clamp(*o.SourceRadius, 0.0f, 1000.0f) : defaults.SourceRadius;
        if (o.Samples)
            o.Samples = std::clamp(*o.Samples, 1u, MaxAudioOcclusionSamples);
        if (o.Mode && *o.Mode > AudioOcclusionMode::Volumetric)
            o.Mode = AudioOcclusionMode::Off;
        if (o.Transmission && *o.Transmission > AudioTransmissionMode::FrequencyDependent)
            o.Transmission = AudioTransmissionMode::Off;
    }

    void FormatBands(const std::array<float, 3>& bands, char* out, size_t size)
    {
        std::snprintf(out, size, "%.2f / %.2f / %.2f", double(bands[0]), double(bands[1]), double(bands[2]));
    }

    std::optional<AcousticMaterialPreset> MatchAcousticPreset(const AcousticMaterial& material)
    {
        for (int i = 0; i < AcousticPresetCount; ++i)
            if (GetAcousticMaterialPreset(AcousticMaterialPreset(i)) == material)
                return AcousticMaterialPreset(i);
        return std::nullopt;
    }

    const char* AcousticPresetLabel(const AcousticMaterial& material)
    {
        const auto preset = MatchAcousticPreset(material);
        return preset ? AcousticMaterialPresetName(*preset) : "Custom";
    }

    void ClampAcoustics(AcousticMaterial& material, const AcousticMaterial& previous)
    {
        const auto clamp = [](float& value, float fallback) {
            value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
        };
        for (size_t band = 0; band < 3; ++band)
        {
            clamp(material.Absorption[band], previous.Absorption[band]);
            clamp(material.Transmission[band], previous.Transmission[band]);
        }
        clamp(material.Scattering, previous.Scattering);
    }

    // ---------------------------------------------------------------- occlusion rays

    void
    OcclusionRayRecorder::Hook(void* context, const AudioOcclusionRay& ray, const AudioOcclusionHit* hits, size_t count)
    {
        static_cast<OcclusionRayRecorder*>(context)->Record(ray, hits, count);
    }

    void OcclusionRayRecorder::Install(AudioEngine& engine)
    {
        if (m_Engine == &engine && m_Instance == engine.GetInstanceID())
            return;
        if (m_Engine != &engine)
            Remove();
        m_Engine   = &engine;
        m_Instance = engine.GetInstanceID();
        engine.SetOcclusionDebugDraw(&OcclusionRayRecorder::Hook, this);
    }

    void OcclusionRayRecorder::Remove()
    {
        if (m_Engine)
            m_Engine->SetOcclusionDebugDraw(nullptr, nullptr);
        m_Engine   = nullptr;
        m_Instance = 0;
        Clear();
    }

    bool OcclusionRayRecorder::IsInstalledOn(const AudioEngine& engine) const
    {
        return m_Engine == &engine && m_Instance == engine.GetInstanceID();
    }

    void OcclusionRayRecorder::Record(const AudioOcclusionRay& ray, const AudioOcclusionHit* hits, size_t count)
    {
        if (m_Count == Capacity)
        {
            m_Start = (m_Start + 1) % Capacity;
            --m_Count;
        }
        Ray& out     = m_Rays[(m_Start + m_Count++) % Capacity];
        out.Start    = ray.Start;
        out.End      = ray.End;
        out.Frame    = m_Frame;
        out.HitCount = uint32_t(std::min(count, out.Hits.size()));
        for (uint32_t i = 0; i < out.HitCount; ++i)
            out.Hits[i] = hits[i].Point;
    }

    void OcclusionRayRecorder::EndFrame()
    {
        ++m_Frame;
        while (m_Count && At(0).Frame + HoldFrames < m_Frame)
        {
            m_Start = (m_Start + 1) % Capacity;
            --m_Count;
        }
    }

    // ---------------------------------------------------------------- HRTF assets

    void ImportFailures::Observe(const EditorAssetImportSnapshot& snapshot)
    {
        const bool finished =
            snapshot.State == EditorAssetImportState::Failed || snapshot.State == EditorAssetImportState::Succeeded;
        if (!finished || !snapshot.Source.IsValid() || (m_Seen && snapshot.Session == m_Session))
            return;
        m_Seen    = true;
        m_Session = snapshot.Session;
        if (snapshot.State == EditorAssetImportState::Failed && snapshot.Error)
            m_Reasons[snapshot.Source] = snapshot.Error->Message;
        else if (snapshot.State == EditorAssetImportState::Succeeded)
            m_Reasons.erase(snapshot.Source);
    }

    const std::string* ImportFailures::Find(SourceAssetID source) const
    {
        const auto found = m_Reasons.find(source);
        return found == m_Reasons.end() ? nullptr : &found->second;
    }

    const char* NormalizeVolumeText(const std::map<std::string, std::string>& settings)
    {
        const auto found = settings.find("normalizeVolume");
        return found != settings.end() && found->second == "true" ? "On" : "Off";
    }

    bool IsHRTFFile(const std::filesystem::path& file)
    {
        auto extension = file.extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        return extension == ".sofa";
    }

    // ---------------------------------------------------------------- project settings

    bool ProjectAudioValues::operator==(const ProjectAudioValues& other) const
    {
        return HRTF == other.HRTF && DefaultSpatializer == other.DefaultSpatializer &&
               DefaultOcclusion == other.DefaultOcclusion &&
               MaxOcclusionRaysPerFrame == other.MaxOcclusionRaysPerFrame &&
               OcclusionStaleFrames == other.OcclusionStaleFrames;
    }

    std::optional<AssetError> ProjectAudioSettings::Load(const std::filesystem::path& projectFile)
    {
        auto loaded = LoadProjectFile(projectFile);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        const auto& project = std::get<ProjectFile>(loaded);
        m_File              = projectFile;
        m_Values            = {project.AudioHRTF,
                               project.DefaultAudioSpatializer,
                               project.DefaultAudioOcclusion,
                               project.MaxOcclusionRaysPerFrame,
                               project.OcclusionStaleFrames};
        m_Dirty             = false;
        m_HRTFError.clear();
        return std::nullopt;
    }

    std::optional<std::string>
    ProjectAudioSettings::Apply(const ProjectAudioValues& values, AudioEngine* engine, AssetManager* assets)
    {
        if (!IsValidAudioOcclusion(values.DefaultOcclusion.Resolve()))
            return std::string("The default occlusion settings are out of range");
        if (values.MaxOcclusionRaysPerFrame < 1 || values.MaxOcclusionRaysPerFrame > MaxRaysPerFrameLimit)
            return "Max occlusion rays per frame must be 1 to " + std::to_string(MaxRaysPerFrameLimit);
        if (values.OcclusionStaleFrames < 1 || values.OcclusionStaleFrames > MaxStaleFramesLimit)
            return "Occlusion stale frames must be 1 to " + std::to_string(MaxStaleFramesLimit);
        const bool hrtfChanged = values.HRTF != m_Values.HRTF;
        if (values != m_Values)
            m_Dirty = true;
        m_Values = values;
        if (engine && engine->IsInitialized())
        {
            engine->SetDefaultSpatializer(values.DefaultSpatializer);
            engine->SetOcclusionDefaults(
                values.DefaultOcclusion, values.MaxOcclusionRaysPerFrame, values.OcclusionStaleFrames);
            if (hrtfChanged && engine->IsHRTFAvailable())
            {
                m_HRTFError.clear();
                if (auto error = engine->SetActiveHRTF(values.HRTF.IsValid() ? assets : nullptr, values.HRTF))
                {
                    m_HRTFError = *error;
                    (void)engine->SetActiveHRTF(nullptr, {});
                }
            }
        }
        return std::nullopt;
    }

    std::optional<AssetError> ProjectAudioSettings::Save()
    {
        if (m_File.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "No project file to save audio settings to"};
        auto loaded = LoadProjectFile(m_File);
        if (const auto* error = std::get_if<AssetError>(&loaded))
            return *error;
        auto project                     = std::get<ProjectFile>(std::move(loaded));
        project.AudioHRTF                = m_Values.HRTF;
        project.DefaultAudioSpatializer  = m_Values.DefaultSpatializer;
        project.DefaultAudioOcclusion    = m_Values.DefaultOcclusion;
        project.MaxOcclusionRaysPerFrame = m_Values.MaxOcclusionRaysPerFrame;
        project.OcclusionStaleFrames     = m_Values.OcclusionStaleFrames;
        if (auto error = SaveProjectFile(m_File, project))
            return error;
        m_Dirty = false;
        return std::nullopt;
    }
} // namespace Kans::AudioSpatialEditing
