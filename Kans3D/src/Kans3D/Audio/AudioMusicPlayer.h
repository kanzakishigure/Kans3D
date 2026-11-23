#pragma once

#include "Kans3D/Audio/AudioEngine.h"

namespace Kans
{
    struct AudioMusicTrack
    {
        AudioSoundID Sound = 0;
        // Source frames. LoopEnd == 0 plays once. A valid region plays [0, LoopEnd) once,
        // then repeats [LoopStart, LoopEnd).
        uint64_t LoopStart = 0, LoopEnd = 0;
        // Used for position queries and as the fallback tempo for quantised transitions.
        double   BPM         = 0;
        uint32_t BeatsPerBar = 4;
    };

    enum class AudioMusicTransitionType
    {
        Immediate,
        Crossfade,
        NextBeat,
        NextBar
    };

    struct AudioMusicTransition
    {
        AudioMusicTransitionType Type       = AudioMusicTransitionType::Immediate;
        uint64_t                 FadeFrames = 0; // Engine frames. Quantised transitions may also crossfade.
        AudioFadeCurve           Curve      = AudioFadeCurve::Linear;
        double                   BPM = 0; // Zero uses the current track's tempo, or the new track's tempo if idle.
        uint32_t                 BeatsPerBar   = 0; // Zero uses the same track's time signature.
        double                   OffsetSeconds = 0; // Absolute engine-clock time of beat zero.
    };

    struct AudioMusicBeatPosition
    {
        double   Beat      = 0; // Zero-based, fractional beat from the beginning of the clip.
        uint64_t Bar       = 0; // Zero-based.
        double   BeatInBar = 0; // Zero-based, fractional beat within Bar.
    };

    // A main-thread transport over streamed engine voices. The engine must outlive the player.
    // The player needs no per-frame calls: timing, fades and loop reads run with the mixer.
    class AudioMusicPlayer
    {
    public:
        explicit AudioMusicPlayer(AudioEngine& engine);
        ~AudioMusicPlayer();
        AudioMusicPlayer(const AudioMusicPlayer&)            = delete;
        AudioMusicPlayer& operator=(const AudioMusicPlayer&) = delete;

        // A failed play leaves the existing track and transition intact.
        bool Play(const AudioMusicTrack& track, const AudioMusicTransition& transition = {});
        void Stop(uint64_t fadeFrames = 0, AudioFadeCurve curve = AudioFadeCurve::Linear);
        void Pause();
        void Resume();
        // Seek the current track in source frames, or the pending track if playback hasn't started.
        void SeekFrames(uint64_t frame);
        void SetVolume(float linear);
        void SetBus(const std::string& bus);

        // During a pending switch this is the outgoing track. Once the listener clock reaches
        // the switch frame it is the new track, including during a crossfade.
        std::optional<AudioMusicTrack>        GetCurrentTrack() const;
        AudioVoiceHandle                      GetCurrentVoice() const;
        uint64_t                              GetPositionFrames() const;
        double                                GetPositionSeconds() const;
        std::optional<AudioMusicBeatPosition> GetBeatPosition() const;

    private:
        struct PlayingTrack
        {
            AudioMusicTrack  Track;
            AudioVoiceHandle Voice;
            uint64_t         Start = 0, End = UINT64_MAX, EndFromSwitch = UINT64_MAX;
        };

        uint64_t                  ScheduleSwitch(AudioVoiceHandle        incoming,
                                                 uint64_t                frame,
                                                 uint64_t                fade,
                                                 AudioFadeCurve          curve,
                                                 std::optional<uint64_t> superseded);
        uint64_t                  WritableClock() const;
        const PlayingTrack*       Current() const;
        void                      Reap();
        AudioEngine&              m_Engine;
        std::vector<PlayingTrack> m_Tracks;
        std::optional<uint64_t>   m_PausedAt;
        float                     m_Volume = 1;
        std::string               m_Bus    = "Music";
    };
} // namespace Kans
