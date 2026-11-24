#pragma once
#include <optional>
#include <vector>
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Audio/AudioEvent.h"

namespace Kans
{
    class AssetManager;

    // Auditions the AudioEvent being edited outside Play mode: each Trigger plays the edited data
    // through the engine's event player (its selection, variation, cooldown and instance limit),
    // under an ID of its own so neither its state nor its statistics mix with the scene's events.
    // Separate from scene audio, like AudioClipPreview; the sound is 2D. The engine and the asset
    // manager must outlive their binding: Bind(nullptr, nullptr) before either goes away.
    class AudioEventPreview
    {
    public:
        AudioEventPreview();
        ~AudioEventPreview() { Bind(nullptr, nullptr); }
        AudioEventPreview(const AudioEventPreview&)            = delete;
        AudioEventPreview& operator=(const AudioEventPreview&) = delete;

        void         Bind(AudioEngine* engine, AssetManager* assets);
        bool         IsAvailable() const { return m_Engine && m_Assets && m_Engine->IsInitialized(); }
        AudioEngine* GetEngine() const { return m_Engine; }

        // Plays `data` once; the result says what was picked, or why nothing started.
        AudioEventTriggerResult Trigger(const AudioEventData& data);
        // Plays one entry's clip with the event's base and entry offsets (no random variation).
        bool PlayEntry(const AudioEventData& data, size_t entry);
        // Stops every voice the preview started and turns auto-trigger off.
        void Stop();
        // Stop, and forget the selection, cooldown, statistics and last pick (another asset).
        void Reset();

        // Auto-trigger: plays `data` every `interval` seconds of Update time while on.
        static constexpr float MinInterval = 0.05f, MaxInterval = 10.0f;
        void                   SetAutoTrigger(bool on) { m_Auto = on && IsAvailable(), m_Elapsed = 0; }
        bool                   IsAutoTriggering() const { return m_Auto; }
        void                   SetInterval(float seconds);
        float                  GetInterval() const { return m_Interval; }
        // Once per frame: drops ended voices and runs the auto-trigger with the current data.
        void Update(float deltaSeconds, const AudioEventData& data);

        const std::optional<AudioEventTriggerResult>& GetLast() const { return m_Last; }
        AudioEventStats                               GetStats() const;
        // Voices started by the preview that still play.
        size_t  GetActiveVoices() const;
        AssetID GetEventID() const { return m_ID; }

    private:
        AudioEngine*                           m_Engine = nullptr;
        AssetManager*                          m_Assets = nullptr;
        AudioClipRegistry                      m_Clips;
        AssetID                                m_ID;
        std::vector<AudioVoiceHandle>          m_Voices;
        std::optional<AudioEventTriggerResult> m_Last;
        bool                                   m_Auto     = false;
        float                                  m_Interval = 0.5f, m_Elapsed = 0;
    };
} // namespace Kans
