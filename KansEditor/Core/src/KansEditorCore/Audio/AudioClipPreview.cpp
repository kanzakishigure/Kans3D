#include "AudioClipPreview.h"
#include <algorithm>
#include <cmath>
#include "Kans3D/Asset/Manager/AssetManager.h"

namespace Kans
{
    namespace
    {
        uint64_t FrameAt(const AudioClipData& data, double seconds)
        {
            if (!std::isfinite(seconds) || seconds <= 0 || data.Info.LengthFrames == 0)
                return 0;
            const double frame = std::floor(seconds * data.Info.SampleRate);
            return std::min<uint64_t>(data.Info.LengthFrames - 1, uint64_t(std::min(frame, 1.8e19)));
        }
    } // namespace

    void AudioClipPreview::Bind(AudioEngine* engine, AssetManager* assets)
    {
        if (engine == m_Engine && assets == m_Assets)
            return;
        Stop();
        m_Clips.Clear();
        m_Engine = engine;
        m_Assets = assets;
    }

    void AudioClipPreview::Play(AssetID clip, const Options& options)
    {
        Stop();
        if (!IsAvailable() || !clip.IsValid())
            return;
        m_Clip    = clip;
        m_Options = options;
        Update();
    }

    void AudioClipPreview::Stop()
    {
        if (m_Engine && m_Voice.IsValid())
            m_Engine->DestroyVoice(m_Voice);
        m_Voice = {};
        if (m_Source)
            m_Clips.Release(m_Source->assetID);
        m_Source.reset();
        m_Clip = {};
    }

    void AudioClipPreview::Seek(double seconds)
    {
        m_Options.Start = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        if (m_Source && m_Engine->IsVoiceValid(m_Voice))
            m_Engine->SeekFrames(m_Voice, FrameAt(m_Source->GetData(), m_Options.Start));
    }

    std::optional<double> AudioClipPreview::GetPosition(AssetID clip) const
    {
        if (!IsPlaying(clip) || !m_Source || !m_Engine->IsVoiceValid(m_Voice))
            return std::nullopt;
        const auto& info = m_Source->GetData().Info;
        // A looping voice's decoder cursor stays inside the clip; a finished one sits at its end.
        return double(std::min(m_Engine->GetCursorFrames(m_Voice), info.LengthFrames)) / info.SampleRate;
    }

    void AudioClipPreview::Update()
    {
        if (!m_Clip.IsValid() || !IsAvailable())
            return;
        if (m_Source)
        {
            if (!m_Engine->IsVoiceValid(m_Voice) || m_Engine->HasEnded(m_Voice))
                Stop();
            return;
        }
        if (!Start())
            Stop();
    }

    bool AudioClipPreview::Start()
    {
        auto loaded = m_Assets->Request<AudioClipAsset>(m_Clip);
        if (loaded.State == AssetLoadState::Failed)
            return false;
        if (!loaded.IsReady())
            return true; // Still loading: started by a later Update.
        if (!m_Clips.Acquire(*m_Engine, loaded.Asset))
            return false;
        m_Source = loaded.Asset;
        AudioVoiceDesc desc;
        desc.Volume = m_Options.Volume;
        desc.Pitch  = m_Options.Pitch;
        desc.Loop   = m_Options.Loop;
        m_Voice     = m_Clips.CreateVoice(*m_Engine, m_Source, desc);
        if (!m_Engine->IsVoiceValid(m_Voice))
            return false;
        if (const auto frame = FrameAt(m_Source->GetData(), m_Options.Start))
            m_Engine->SeekFrames(m_Voice, frame);
        m_Engine->Play(m_Voice);
        return true;
    }
} // namespace Kans
