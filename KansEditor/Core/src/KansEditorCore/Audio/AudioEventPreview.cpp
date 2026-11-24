#include "AudioEventPreview.h"
#include <algorithm>
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEventPlayer.h"

namespace Kans
{
    AudioEventPreview::AudioEventPreview() : m_ID(SourceAssetID::Generate(), 1) {}

    void AudioEventPreview::Bind(AudioEngine* engine, AssetManager* assets)
    {
        if (engine == m_Engine && assets == m_Assets)
            return;
        Reset();
        m_Clips.Clear();
        m_Engine = engine;
        m_Assets = assets;
    }

    AudioEventTriggerResult AudioEventPreview::Trigger(const AudioEventData& data)
    {
        if (!IsAvailable())
            return {};
        AudioEventParams params;
        params.Spatial    = false; // No emitter in the editor: heard as a 2D sound.
        const auto event  = std::make_shared<const AudioEventAsset>(data); // A snapshot of the edited data.
        auto       result = m_Engine->GetEventPlayer().Trigger(*m_Assets, m_Clips, m_ID, event, params);
        if (result.Voice.IsValid())
            m_Voices.push_back(result.Voice);
        m_Last = result;
        return result;
    }

    bool AudioEventPreview::PlayEntry(const AudioEventData& data, size_t entry)
    {
        if (!IsAvailable() || entry >= data.Entries.size())
            return false;
        const auto& chosen = data.Entries[entry];
        auto        loaded = m_Assets->Load<AudioClipAsset>(chosen.Clip);
        auto*       clip   = std::get_if<Ref<const AudioClipAsset>>(&loaded);
        if (!clip || !(*clip)->IsValid())
            return false;
        AudioOneShotParams params;
        params.Volume   = float(std::pow(10.0, (double(data.VolumeDb) + chosen.VolumeDb) / 20));
        params.Pitch    = float(std::exp2((double(data.PitchSemitones) + chosen.PitchSemitones) / 12));
        params.Bus      = data.Bus;
        params.Priority = data.Priority;
        if (!std::isfinite(params.Volume) || !std::isfinite(params.Pitch) || params.Pitch <= 0)
            return false;
        const auto voice = m_Engine->PlayOneShot(m_Clips, *clip, params);
        if (!voice.IsValid())
            return false;
        m_Voices.push_back(voice);
        return true;
    }

    void AudioEventPreview::Stop()
    {
        m_Auto    = false;
        m_Elapsed = 0;
        if (m_Engine)
            for (const auto voice : m_Voices)
                if (m_Engine->IsVoiceValid(voice))
                    m_Engine->Stop(voice);
        m_Voices.clear();
    }

    void AudioEventPreview::Reset()
    {
        Stop();
        if (m_Engine)
            m_Engine->GetEventPlayer().Forget(m_ID);
        m_Last.reset();
    }

    void AudioEventPreview::SetInterval(float seconds)
    {
        m_Interval = std::isfinite(seconds) ? std::clamp(seconds, MinInterval, MaxInterval) : 0.5f;
    }

    void AudioEventPreview::Update(float deltaSeconds, const AudioEventData& data)
    {
        if (!IsAvailable())
        {
            m_Auto = false;
            m_Voices.clear();
            return;
        }
        m_Voices.erase(std::remove_if(m_Voices.begin(),
                                      m_Voices.end(),
                                      [this](AudioVoiceHandle voice) {
                                          return !m_Engine->IsVoiceValid(voice) || m_Engine->HasEnded(voice);
                                      }),
                       m_Voices.end());
        if (!m_Auto || !std::isfinite(deltaSeconds) || deltaSeconds <= 0)
            return;
        m_Elapsed += deltaSeconds;
        if (m_Elapsed < m_Interval)
            return;
        m_Elapsed = std::fmod(m_Elapsed, m_Interval);
        Trigger(data);
    }

    AudioEventStats AudioEventPreview::GetStats() const
    {
        return m_Engine ? m_Engine->GetEventPlayer().GetStats(m_ID) : AudioEventStats {};
    }

    size_t AudioEventPreview::GetActiveVoices() const
    {
        if (!m_Engine)
            return 0;
        return size_t(std::count_if(m_Voices.begin(), m_Voices.end(), [this](AudioVoiceHandle voice) {
            return m_Engine->IsVoiceValid(voice) && !m_Engine->HasEnded(voice);
        }));
    }
} // namespace Kans
