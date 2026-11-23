#pragma once
#include <optional>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Audio/AudioClipRegistry.h"

namespace Kans
{
    class AssetManager;

    // Auditions one AudioClip at a time outside Play mode (Content Browser, Inspector): a plain
    // 2D voice through the editor's engine, independent of the scene's sources, so edit mode stays
    // silent unless something is previewed. The engine and the asset manager must outlive their
    // binding: Bind(nullptr, nullptr) before either goes away.
    class AudioClipPreview
    {
    public:
        struct Options
        {
            float  Volume = 1, Pitch = 1; // Multiply the clip's imported volume, like a source does.
            bool   Loop  = false;
            double Start = 0; // Seconds into the clip.
        };

        AudioClipPreview() = default;
        ~AudioClipPreview() { Bind(nullptr, nullptr); }
        AudioClipPreview(const AudioClipPreview&)            = delete;
        AudioClipPreview& operator=(const AudioClipPreview&) = delete;

        void Bind(AudioEngine* engine, AssetManager* assets);
        bool IsAvailable() const { return m_Engine && m_Assets; }
        // The bound engine (its bus layout names the buses a source can play through).
        AudioEngine* GetEngine() const { return m_Engine; }

        // Replaces whatever plays; the clip starts once it has loaded (see Update).
        void Play(AssetID clip, const Options& options);
        void Play(AssetID clip) { Play(clip, Options {}); }
        void Stop();
        // Moves the playhead of the playing clip, or where a loading one will start.
        void Seek(double seconds);
        // The clip auditioned (playing or still loading); invalid when idle.
        AssetID GetClip() const { return m_Clip; }
        bool    IsPlaying(AssetID clip) const { return clip.IsValid() && m_Clip == clip; }
        // Seconds into the playing clip; none while it loads or when `clip` is not the one playing.
        std::optional<double> GetPosition(AssetID clip) const;
        // Starts a clip that finished loading and stops one that reached its end; once per frame.
        void Update();

    private:
        bool Start(); // False when the clip cannot play (then the preview is idle).

        AudioEngine*              m_Engine = nullptr;
        AssetManager*             m_Assets = nullptr;
        AudioClipRegistry         m_Clips;
        AssetID                   m_Clip;
        Options                   m_Options;
        Ref<const AudioClipAsset> m_Source;
        AudioVoiceHandle          m_Voice;
    };
} // namespace Kans
