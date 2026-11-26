#pragma once

#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Timeline.h"

namespace Kans
{
    // Main-thread audio transport, shared by scene playback and editor preview. The engine
    // must outlive this player. The clip resolver can return null while an asset is loading.
    class TimelineAudioPlayer
    {
    public:
        using ClipResolver    = std::function<Ref<const AudioClipAsset>(AssetID)>;
        using SpatialResolver = std::function<std::optional<AudioSpatialParams>(const std::string& binding)>;
        static constexpr double SeekDriftSeconds = .050;

        TimelineAudioPlayer(AudioEngine&, Ref<const TimelineAsset>, ClipResolver, SpatialResolver = {});
        ~TimelineAudioPlayer();

        void Play(double time, double speed = 1, double offset = 0, std::optional<size_t> masterTrack = {});
        void Update(double time, double speed = 1, double offset = 0, std::optional<size_t> masterTrack = {});
        void Pause();
        void SetGamePaused(bool paused);
        // A playing seek rearms playback at the cursor. A paused seek is silent.
        void Seek(double time);
        // Silent editor scrubbing. Play(Time(), ...) resumes from the new cursor.
        void Scrub(double time);
        void Stop();

        double                Time() const { return m_Time; }
        std::optional<double> GetClockTime(size_t track) const;
        TimelineAudioStats    GetStats() const { return m_Stats; }
        AudioVoiceHandle      GetTrackVoice(size_t track) const;

    private:
        struct Voice
        {
            AudioVoiceHandle Handle;
            uint64_t         Start = 0, Stop = 0;
            double           SourceDuration = 0;
            float            Volume         = 1;
        };

        void         ClearVoices();
        void         Reanchor();
        void         Schedule();
        void         ScheduleClip(size_t track, size_t clip, uint64_t writable);
        void         Measure();
        uint64_t     FrameAt(double timelineTime) const;
        double       HeardFrame() const;
        const Voice* ActiveVoice(size_t track, size_t& clip, double heard) const;

        AudioEngine&                         m_Engine;
        Ref<const TimelineAsset>             m_Source;
        ClipResolver                         m_ResolveClip;
        SpatialResolver                      m_ResolveSpatial;
        AudioClipRegistry                    m_Clips;
        std::map<AssetID, AudioSoundID>      m_Sounds;
        std::vector<std::map<size_t, Voice>> m_Voices;
        bool                                 m_Playing = false;
        double                               m_Time = 0, m_Speed = 1, m_Offset = 0, m_AnchorTime = 0;
        uint64_t                             m_AnchorFrame = 0;
        std::optional<size_t>                m_MasterTrack;
        std::optional<uint64_t>              m_GamePauseFrame;
        TimelineAudioStats                   m_Stats;
    };
} // namespace Kans
