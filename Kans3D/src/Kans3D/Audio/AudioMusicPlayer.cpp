#include "kspch.h"
#include "Kans3D/Audio/AudioMusicPlayer.h"

#include <cmath>
#include <limits>

namespace Kans
{
    namespace
    {
        constexpr auto NoEnd = std::numeric_limits<uint64_t>::max();

        uint64_t AddFrames(uint64_t frame, uint64_t duration) { return frame + std::min(NoEnd - frame, duration); }

        std::optional<uint64_t> NextBoundary(uint64_t now, uint32_t rate, double bpm, uint32_t beats, double offset)
        {
            if (!std::isfinite(bpm) || bpm <= 0 || !std::isfinite(offset) || beats == 0)
                return std::nullopt;
            const long double span     = 60.L * rate * beats / bpm;
            const long double origin   = static_cast<long double>(offset) * rate;
            const long double boundary = origin + (std::floor((now - origin) / span) + 1) * span;
            const auto        frame    = std::max(static_cast<long double>(now) + 1, std::ceil(boundary));
            if (!std::isfinite(frame) || frame >= static_cast<long double>(NoEnd))
                return std::nullopt;
            return static_cast<uint64_t>(frame);
        }
    } // namespace

    AudioMusicPlayer::AudioMusicPlayer(AudioEngine& engine) : m_Engine(engine) {}

    AudioMusicPlayer::~AudioMusicPlayer()
    {
        for (const auto& track : m_Tracks)
            m_Engine.DestroyVoice(track.Voice);
    }

    const AudioMusicPlayer::PlayingTrack* AudioMusicPlayer::Current() const
    {
        const auto   heard = m_Engine.GetPlaybackClockFrames();
        const double clock = m_PausedAt ? std::min(heard, static_cast<double>(*m_PausedAt)) : heard;
        for (auto it = m_Tracks.rbegin(); it != m_Tracks.rend(); ++it)
        {
            if (clock < it->Start || clock >= it->End || !m_Engine.IsVoiceValid(it->Voice))
                continue;
            const auto info = m_Engine.GetSoundInfo(it->Track.Sound);
            if (it->Track.LoopEnd != 0 || (info && m_Engine.GetVoicePositionFrames(it->Voice) < info->LengthFrames))
                return &*it;
        }
        return nullptr;
    }

    void AudioMusicPlayer::Reap()
    {
        const double heard = m_Engine.GetPlaybackClockFrames();
        m_Tracks.erase(std::remove_if(m_Tracks.begin(),
                                      m_Tracks.end(),
                                      [&](const PlayingTrack& track) {
                                          if (!m_Engine.IsVoiceValid(track.Voice))
                                              return true;
                                          const auto info = m_Engine.GetSoundInfo(track.Track.Sound);
                                          if ((!m_PausedAt && heard >= track.End) ||
                                              (track.Track.LoopEnd == 0 && info && m_Engine.HasEnded(track.Voice) &&
                                               m_Engine.GetVoicePositionFrames(track.Voice) >= info->LengthFrames))
                                          {
                                              m_Engine.DestroyVoice(track.Voice);
                                              return true;
                                          }
                                          return false;
                                      }),
                       m_Tracks.end());
    }

    bool AudioMusicPlayer::Play(const AudioMusicTrack& track, const AudioMusicTransition& transition)
    {
        const auto info = m_Engine.GetSoundInfo(track.Sound);
        if (!info)
        {
            m_Engine.ProfileRejection(
                AudioVoiceReason::MissingClip, {AudioVoiceOwnerKind::Music, 0, {}}, track.Clip, track.Sound);
            return false;
        }
        if ((track.LoopEnd == 0 && track.LoopStart != 0) ||
            (track.LoopEnd != 0 && (track.LoopStart >= track.LoopEnd || track.LoopEnd > info->LengthFrames)) ||
            !std::isfinite(track.BPM) || track.BPM < 0 || track.BeatsPerBar == 0)
            return false;

        const auto*  current     = Current();
        const auto&  tempo       = current ? current->Track : track;
        const double bpm         = transition.BPM != 0 ? transition.BPM : tempo.BPM;
        const auto   beatsPerBar = transition.BeatsPerBar != 0 ? transition.BeatsPerBar : tempo.BeatsPerBar;
        const bool   quantised   = transition.Type == AudioMusicTransitionType::NextBeat ||
                                   transition.Type == AudioMusicTransitionType::NextBar;
        const auto   beats       = transition.Type == AudioMusicTransitionType::NextBar ? beatsPerBar : 1;
        if (quantised && !NextBoundary(WritableClock(), m_Engine.GetSampleRate(), bpm, beats, transition.OffsetSeconds))
            return false;

        AudioVoiceDesc desc;
        desc.Clip            = track.Clip;
        desc.Owner.Kind      = AudioVoiceOwnerKind::Music;
        desc.Sound       = track.Sound;
        desc.LoadMode    = AudioLoadMode::Stream;
        desc.Loop        = track.LoopEnd != 0;
        desc.AutoRelease = true;
        desc.NeverVirtualize = true;
        desc.Volume      = m_Volume;
        desc.Bus         = m_Bus;
        auto voice       = m_Engine.CreateVoice(desc, track.LoopStart, track.LoopEnd);
        if (!voice.IsValid())
            return false;

        const auto now   = WritableClock();
        const auto start = quantised ?
                               NextBoundary(now, m_Engine.GetSampleRate(), bpm, beats, transition.OffsetSeconds) :
                               std::optional<uint64_t>(now);
        if (!start)
        {
            m_Engine.DestroyVoice(voice);
            return false;
        }

        Reap();
        const bool              paused      = m_PausedAt.has_value();
        bool                    hasOutgoing = false;
        std::optional<uint64_t> superseded;
        for (auto& outgoing : m_Tracks)
        {
            if (paused || outgoing.Start > now)
            {
                if (outgoing.Start > now)
                    superseded = outgoing.Start;
                m_Engine.DestroyVoice(outgoing.Voice);
                continue;
            }
            hasOutgoing |= outgoing.End > now;
        }
        const auto fade =
            hasOutgoing && transition.Type != AudioMusicTransitionType::Immediate ? transition.FadeFrames : 0;
        const auto scheduled = ScheduleSwitch(voice, *start, fade, transition.Curve, superseded);
        m_PausedAt.reset();
        auto playingTrack = track;
        if (playingTrack.BPM == 0 && (quantised || transition.BPM > 0) && std::isfinite(bpm))
        {
            playingTrack.BPM         = bpm;
            playingTrack.BeatsPerBar = beatsPerBar;
        }
        m_Tracks.push_back({playingTrack, voice, scheduled, NoEnd});
        return true;
    }

    void AudioMusicPlayer::Stop(uint64_t fadeFrames, AudioFadeCurve curve)
    {
        Reap();
        const auto now     = WritableClock();
        const auto pending = !m_Tracks.empty() && m_Tracks.back().Start > now ? m_Tracks.back().Start : NoEnd;
        for (auto& track : m_Tracks)
        {
            if (m_PausedAt || track.Start > now)
            {
                m_Engine.DestroyVoice(track.Voice);
                continue;
            }
            const auto end = AddFrames(now, fadeFrames);
            if (end < track.End || track.End == NoEnd || (pending != NoEnd && track.EndFromSwitch == pending))
            {
                track.End           = end;
                track.EndFromSwitch = NoEnd;
                m_Engine.ScheduleStop(track.Voice, end, fadeFrames, curve);
            }
        }
        m_PausedAt.reset();
    }

    void AudioMusicPlayer::Pause()
    {
        if (m_PausedAt)
            return;
        Reap();
        m_PausedAt = m_Engine.GetClockFrames();
        for (const auto& track : m_Tracks)
            if (track.End > *m_PausedAt)
                m_Engine.Pause(track.Voice);
    }

    void AudioMusicPlayer::Resume()
    {
        if (!m_PausedAt)
            return;
        const auto delay = m_Engine.GetClockFrames() - *m_PausedAt;
        for (auto& track : m_Tracks)
        {
            if (track.End <= *m_PausedAt)
                continue;
            if (track.Start > *m_PausedAt)
                track.Start = AddFrames(track.Start, delay);
            if (track.End != NoEnd)
                track.End = AddFrames(track.End, delay);
            if (track.EndFromSwitch != NoEnd)
                track.EndFromSwitch = AddFrames(track.EndFromSwitch, delay);
            m_Engine.Play(track.Voice);
        }
        m_PausedAt.reset();
    }

    void AudioMusicPlayer::SeekFrames(uint64_t frame)
    {
        if (const auto* current = Current())
            m_Engine.SeekFrames(current->Voice, frame);
        else if (!m_Tracks.empty())
            m_Engine.SeekFrames(m_Tracks.back().Voice, frame);
    }

    void AudioMusicPlayer::SetVolume(float linear)
    {
        if (!std::isfinite(linear))
            return;
        m_Volume = std::max(0.f, linear);
        for (const auto& track : m_Tracks)
            m_Engine.SetVolume(track.Voice, m_Volume);
    }

    void AudioMusicPlayer::SetBus(const std::string& bus)
    {
        m_Bus = bus;
        for (const auto& track : m_Tracks)
            m_Engine.SetVoiceBus(track.Voice, bus);
    }

    std::optional<AudioMusicTrack> AudioMusicPlayer::GetCurrentTrack() const
    {
        const auto* current = Current();
        return current ? std::optional<AudioMusicTrack>(current->Track) : std::nullopt;
    }

    AudioVoiceHandle AudioMusicPlayer::GetCurrentVoice() const
    {
        const auto* current = Current();
        return current ? current->Voice : AudioVoiceHandle {};
    }

    uint64_t AudioMusicPlayer::GetPositionFrames() const
    {
        const auto* current = Current();
        return current ? m_Engine.GetVoicePositionFrames(current->Voice) : 0;
    }

    double AudioMusicPlayer::GetPositionSeconds() const
    {
        const auto* current = Current();
        return current ? m_Engine.GetVoicePositionSeconds(current->Voice) : 0;
    }

    std::optional<AudioMusicBeatPosition> AudioMusicPlayer::GetBeatPosition() const
    {
        const auto* current = Current();
        if (!current || current->Track.BPM == 0)
            return std::nullopt;
        const auto beat = m_Engine.GetVoicePositionSeconds(current->Voice) * current->Track.BPM / 60;
        const auto bar  = std::floor(beat / current->Track.BeatsPerBar);
        if (!std::isfinite(bar) || bar >= static_cast<double>(NoEnd))
            return std::nullopt;
        return AudioMusicBeatPosition {beat, static_cast<uint64_t>(bar), std::fmod(beat, current->Track.BeatsPerBar)};
    }
} // namespace Kans
