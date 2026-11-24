#include "kspch.h"
#include "TimelineAudioPlayer.h"

namespace Kans
{
    namespace
    {
        constexpr double LookAheadSeconds = .25;

        uint64_t RoundedFrame(double value)
        {
            constexpr auto max = std::numeric_limits<uint64_t>::max();
            if (value >= static_cast<double>(max))
                return max;
            return static_cast<uint64_t>(std::round(std::max(0.0, value)));
        }
    } // namespace

    TimelineAudioPlayer::TimelineAudioPlayer(AudioEngine&             engine,
                                             Ref<const TimelineAsset> source,
                                             ClipResolver             clipResolver,
                                             SpatialResolver          spatialResolver) :
        m_Engine(engine), m_Source(std::move(source)), m_ResolveClip(std::move(clipResolver)),
        m_ResolveSpatial(std::move(spatialResolver)), m_Voices(m_Source ? m_Source->GetData().Audio.size() : 0)
    {}

    TimelineAudioPlayer::~TimelineAudioPlayer() { Stop(); }

    void TimelineAudioPlayer::ClearVoices()
    {
        for (auto& track : m_Voices)
        {
            for (const auto& entry : track)
                m_Engine.DestroyVoice(entry.second.Handle);
            track.clear();
        }
        m_Stats = {};
    }

    void TimelineAudioPlayer::Stop()
    {
        ClearVoices();
        m_Clips.Clear();
        m_Sounds.clear();
        m_Playing = false;
        m_Time    = 0;
    }

    void TimelineAudioPlayer::Pause()
    {
        if (!m_Playing)
            return;
        for (const auto& track : m_Voices)
            for (const auto& entry : track)
                m_Engine.Pause(entry.second.Handle);
        m_Playing = false;
        m_Stats   = {};
    }

    void TimelineAudioPlayer::Scrub(double time)
    {
        Pause();
        Seek(time);
    }

    void TimelineAudioPlayer::Seek(double time)
    {
        if (!m_Source || !std::isfinite(time))
            return;
        m_Time = std::clamp(time, 0.0, m_Source->GetData().Length);
        Reanchor();
        Schedule();
        Measure();
    }

    void TimelineAudioPlayer::Play(double time, double speed, double offset, std::optional<size_t> masterTrack)
    {
        if (!std::isfinite(speed) || !std::isfinite(offset) || !std::isfinite(time))
            return;
        m_Playing     = true;
        m_Speed       = float(speed);
        m_Offset      = offset;
        m_MasterTrack = masterTrack;
        Seek(time);
    }

    void TimelineAudioPlayer::Update(double time, double speed, double offset, std::optional<size_t> masterTrack)
    {
        if (!m_Source || !m_Playing || !std::isfinite(time) || !std::isfinite(speed) || !std::isfinite(offset))
            return;
        m_Time = std::clamp(time, 0.0, m_Source->GetData().Length);
        if (float(speed) != m_Speed || offset != m_Offset || masterTrack != m_MasterTrack)
        {
            Play(time, speed, offset, masterTrack);
            return;
        }
        const auto heard = HeardFrame();
        const auto rate  = m_Engine.GetSampleRate();
        // A silent gap can outlast the original game-to-mixer anchor. Recalibrate
        // before the next cue when no scheduled window is still on the listener clock.
        if (rate > 0 && heard >= m_AnchorFrame &&
            std::abs(m_Time - (m_AnchorTime + (heard - m_AnchorFrame) / rate * m_Speed)) > SeekDriftSeconds)
        {
            const bool hasWindow = std::any_of(m_Voices.begin(), m_Voices.end(), [&](const auto& track) {
                return std::any_of(track.begin(), track.end(), [&](const auto& entry) {
                    return heard < entry.second.Stop && m_Engine.IsVoiceValid(entry.second.Handle);
                });
            });
            if (!hasWindow)
                Reanchor();
        }
        Schedule();
        Measure();
        if (std::abs(m_Stats.DriftSeconds) > SeekDriftSeconds && (!m_MasterTrack || !GetClockTime(*m_MasterTrack)))
        {
            const auto drift = m_Stats.DriftSeconds;
            Reanchor();
            // The corrected sample will be heard when the listener clock reaches the mixer frame.
            m_AnchorTime += std::max(0.0, m_AnchorFrame - HeardFrame()) / m_Engine.GetSampleRate() * m_Speed;
            Schedule();
            Measure();
            m_Stats.DriftSeconds = drift;
        }
    }

    void TimelineAudioPlayer::Reanchor()
    {
        ClearVoices();
        m_AnchorTime  = m_Time;
        m_AnchorFrame = m_Engine.GetSchedulingClockFrames();
    }

    uint64_t TimelineAudioPlayer::FrameAt(double timelineTime) const
    {
        return RoundedFrame(m_AnchorFrame + (timelineTime - m_AnchorTime) / m_Speed * m_Engine.GetSampleRate());
    }

    double TimelineAudioPlayer::HeardFrame() const { return m_Engine.GetPlaybackClockFrames(); }

    void TimelineAudioPlayer::Schedule()
    {
        if (!m_Source || !m_Playing || !m_Engine.IsInitialized() || !std::isfinite(m_Speed) || m_Speed <= 0)
            return;
        const auto&  audio    = m_Source->GetData().Audio;
        const auto   heard    = HeardFrame();
        const auto   writable = m_Engine.GetSchedulingClockFrames();
        const auto   horizon  = double(writable) + LookAheadSeconds * m_Engine.GetSampleRate();
        const double end      = m_Source->GetData().Length;
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const auto& track  = audio[i];
            auto&       voices = m_Voices[i];
            for (auto it = voices.begin(); it != voices.end();)
            {
                if (heard >= it->second.Stop || !m_Engine.IsVoiceValid(it->second.Handle))
                {
                    m_Engine.DestroyVoice(it->second.Handle);
                    it = voices.erase(it);
                }
                else
                    ++it;
            }
            if (track.Mute)
                continue;
            const auto first = std::upper_bound(track.Clips.begin(),
                                                track.Clips.end(),
                                                m_Time - m_Offset,
                                                [](double time, const auto& clip) { return time < clip.End; });
            for (auto clip = first; clip != track.Clips.end(); ++clip)
            {
                const auto start = clip->Start + m_Offset;
                if (start >= end || FrameAt(start) > horizon)
                    break;
                const auto index = size_t(clip - track.Clips.begin());
                if (!voices.count(index))
                    ScheduleClip(i, index, writable);
            }
            if (!track.Binding.empty())
            {
                const auto spatial = m_ResolveSpatial ? m_ResolveSpatial(track.Binding) : std::nullopt;
                for (const auto& entry : voices)
                {
                    const auto& voice = entry.second;
                    m_Engine.SetVolume(voice.Handle, spatial ? voice.Volume : 0);
                    if (spatial)
                        m_Engine.SetSpatialParams(voice.Handle, *spatial);
                }
            }
        }
    }

    void TimelineAudioPlayer::ScheduleClip(size_t trackIndex, size_t clipIndex, uint64_t writable)
    {
        const auto& track = m_Source->GetData().Audio[trackIndex];
        const auto& clip  = track.Clips[clipIndex];
        const auto  start = clip.Start + m_Offset, authoredEnd = clip.End + m_Offset;
        const auto  end       = std::min(m_Source->GetData().Length, authoredEnd);
        const auto  scheduled = FrameAt(start);
        const auto  frame     = std::max(writable, scheduled);
        const auto  stop      = FrameAt(end);
        if (frame >= stop)
            return;

        auto sound = m_Sounds.find(clip.Clip);
        if (sound == m_Sounds.end() || !m_Engine.IsSoundRegistered(sound->second))
        {
            auto       asset = m_ResolveClip ? m_ResolveClip(clip.Clip) : nullptr;
            const auto id    = m_Clips.Acquire(m_Engine, asset);
            if (!id)
                return;
            sound = m_Sounds.insert_or_assign(clip.Clip, *id).first;
        }
        const auto info = m_Engine.GetSoundInfo(sound->second);
        if (!info)
            return;
        const auto   rate    = m_Engine.GetSampleRate();
        const double atStart = m_AnchorTime + (double(frame) - m_AnchorFrame) * m_Speed / rate;
        const auto   elapsed = frame == scheduled && start >= m_AnchorTime ? 0.0 : std::max(0.0, atStart - start);
        const auto   source  = RoundedFrame((clip.ClipIn + elapsed) * info->SampleRate);
        if (source >= info->LengthFrames)
            return;

        AudioVoiceDesc desc;
        desc.Sound        = sound->second;
        desc.Bus          = track.Bus;
        desc.LoadMode     = AudioLoadMode::Stream;
        desc.NeverVirtualize = true;
        desc.Pitch        = float(m_Speed);
        const auto volume = std::pow(10.f, clip.VolumeDb / 20.f);
        desc.Spatial      = !track.Binding.empty();
        desc.Volume       = desc.Spatial ? 0 : volume;
        const auto voice  = m_Engine.CreateVoice(desc);
        if (!voice.IsValid())
            return;

        // All boundaries are rounded on the same output-frame grid, including a shared seam.
        const auto       authoredStop = FrameAt(authoredEnd);
        const auto       fadeOutStart = FrameAt(authoredEnd - clip.FadeOut.Length);
        AudioVoiceWindow window;
        window.Duration      = stop - frame;
        window.FadeIn        = RoundedFrame(clip.FadeIn.Length * rate / m_Speed);
        window.FadeInElapsed = RoundedFrame(elapsed * rate / m_Speed);
        window.FadeOut       = RoundedFrame(clip.FadeOut.Length * rate / m_Speed);
        window.FadeOutElapsed =
            RoundedFrame(std::max(0.0, atStart - authoredEnd + clip.FadeOut.Length) * rate / m_Speed);
        window.InCurve  = clip.FadeIn.Curve;
        window.OutCurve = clip.FadeOut.Curve;
        // Preserve the authored fade if the timeline boundary truncates a positively shifted clip.
        if (stop < authoredStop)
        {
            window.FadeOutEnd = authoredStop - frame;
            if (stop <= fadeOutStart)
                window.FadeOut = 0;
        }
        m_Engine.SeekFrames(voice, source);
        m_Engine.ScheduleStart(voice, frame, window);
        m_Voices[trackIndex].emplace(clipIndex,
                                     Voice {voice, frame, stop, double(info->LengthFrames) / info->SampleRate, volume});
    }

    const TimelineAudioPlayer::Voice* TimelineAudioPlayer::ActiveVoice(size_t track, size_t& clip, double heard) const
    {
        if (!m_Playing || track >= m_Voices.size())
            return nullptr;
        for (const auto& [index, voice] : m_Voices[track])
            if (heard >= voice.Start && heard < voice.Stop && m_Engine.IsVoiceValid(voice.Handle) &&
                m_Engine.GetVoicePositionSeconds(voice.Handle) < voice.SourceDuration)
            {
                clip = index;
                return &voice;
            }
        return nullptr;
    }

    AudioVoiceHandle TimelineAudioPlayer::GetTrackVoice(size_t track) const
    {
        size_t      clip;
        const auto* voice = ActiveVoice(track, clip, HeardFrame());
        return voice ? voice->Handle : AudioVoiceHandle {};
    }

    std::optional<double> TimelineAudioPlayer::GetClockTime(size_t track) const
    {
        size_t index;
        if (const auto* voice = ActiveVoice(track, index, HeardFrame()))
        {
            const auto& clip = m_Source->GetData().Audio[track].Clips[index];
            return clip.Start + m_Offset + m_Engine.GetVoicePositionSeconds(voice->Handle) - clip.ClipIn;
        }
        return std::nullopt;
    }

    void TimelineAudioPlayer::Measure()
    {
        m_Stats          = {};
        const auto heard = HeardFrame();
        for (size_t track = 0; track < m_Voices.size(); ++track)
        {
            size_t      index;
            const auto* voice = ActiveVoice(track, index, heard);
            if (!voice)
                continue;
            ++m_Stats.ActiveVoices;
            const auto&  clip     = m_Source->GetData().Audio[track].Clips[index];
            const double expected = m_Time - clip.Start - m_Offset + clip.ClipIn;
            const double drift    = m_Engine.GetVoicePositionSeconds(voice->Handle) - expected;
            if (std::abs(drift) > std::abs(m_Stats.DriftSeconds))
                m_Stats.DriftSeconds = drift;
        }
    }
} // namespace Kans
