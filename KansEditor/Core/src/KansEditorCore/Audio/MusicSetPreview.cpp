#include "MusicSetPreview.h"
#include "Kans3D/Asset/Manager/AssetManager.h"

namespace Kans
{
    MusicSetPreview::MusicSetPreview() : m_ID(SourceAssetID::Generate(), 1) {}

    void MusicSetPreview::Bind(AudioEngine* engine, AssetManager* assets)
    {
        if (engine == m_Engine && assets == m_Assets)
            return;
        // The player's voices go before the sounds they play.
        m_Player.reset();
        m_Clips.reset();
        m_Loaded.clear();
        m_Playing = false;
        m_Engine  = engine;
        m_Assets  = assets;
    }

    bool MusicSetPreview::Load(const MusicSetData& data)
    {
        m_Player.reset();
        m_Clips.reset();
        m_Loaded.clear();
        if (!IsAvailable())
        {
            m_Error = "The editor has no audio engine";
            return false;
        }
        m_Clips          = std::make_unique<AudioClipRegistry>();
        m_Player         = std::make_unique<MusicSetPlayer>(*m_Engine);
        auto asset       = CreateRef<MusicSetAsset>(data); // A snapshot of the edited data.
        asset->assetID   = m_ID;
        const auto clips = [this](AssetID id) -> MusicSetClip {
            auto  loaded = m_Assets->Load<AudioClipAsset>(id);
            auto* clip   = std::get_if<Ref<const AudioClipAsset>>(&loaded);
            if (!clip || !(*clip)->IsValid())
                return {};
            const auto sound = m_Clips->Acquire(*m_Engine, *clip);
            if (!sound)
                return {};
            m_Loaded.emplace(id, *clip);
            return {*sound, (*clip)->GetData().Volume};
        };
        if (auto error = m_Player->Load(asset, clips))
        {
            m_Error = error->Message;
            m_Player.reset();
            m_Clips.reset();
            m_Loaded.clear();
            return false;
        }
        for (const auto& [name, value] : m_Parameters)
            m_Player->SetParameter(name, value);
        m_Error.clear();
        return true;
    }

    bool MusicSetPreview::Play(const MusicSetData& data, const std::string& state)
    {
        m_Playing = false;
        if (!Load(data))
            return false;
        if (!m_Player->Play(state))
        {
            m_Error = state.empty() ? "The music set did not start" : "Could not start " + state;
            return false;
        }
        m_Playing = true;
        return true;
    }

    void MusicSetPreview::Stop()
    {
        if (m_Player)
            m_Player->Stop();
        m_Playing = false;
    }

    bool MusicSetPreview::IsPlaying() const { return m_Playing && m_Player; }

    bool MusicSetPreview::SetState(const std::string& state) { return IsPlaying() && m_Player->SetState(state); }

    bool MusicSetPreview::SetParameter(const std::string& name, float value)
    {
        m_Parameters[name] = value;
        return m_Player && m_Player->SetParameter(name, value);
    }

    bool MusicSetPreview::TriggerStinger(const std::string& name)
    {
        return IsPlaying() && m_Player->TriggerStinger(name);
    }

    void MusicSetPreview::Update(const MusicSetData& data, uint64_t revision)
    {
        if (!IsAvailable())
        {
            m_Player.reset();
            m_Clips.reset();
            m_Loaded.clear();
            m_Playing = false;
            return;
        }
        if (!IsPlaying())
        {
            m_Revision = revision;
            return;
        }
        if (revision != m_Revision)
        {
            m_Revision = revision;
            // The edit is heard at once: the same state again, at the same place when its segment stays.
            const auto position = m_Player->GetPosition();
            const auto state    = position ? std::string(position->State) : std::string();
            const auto segment  = position ? std::string(position->Segment) : std::string();
            const auto seconds  = position ? position->Seconds : 0.0;
            const auto pending  = m_Player->GetPendingTransition();
            const auto next     = pending ? std::string(pending->State) : std::string();
            bool       played   = Play(data, state);
            if (!played && !state.empty())
                played = Play(data); // The state was renamed or removed: the first one.
            if (!played)
                return; // Stopped; the error says why (an invalid edit, a missing clip).
            if (const auto now = m_Player->GetPosition(); now && now->Segment == segment)
                m_Player->SeekSeconds(seconds);
            // A state change that was waiting for its sync point is asked for again.
            if (!next.empty() && next != state)
                m_Player->SetState(next);
            return;
        }
        m_Player->Update();
    }

    std::optional<MusicSetPosition> MusicSetPreview::GetPosition() const
    {
        return IsPlaying() ? m_Player->GetPosition() : std::nullopt;
    }

    std::optional<MusicSetPendingTransition> MusicSetPreview::GetPendingTransition() const
    {
        return IsPlaying() ? m_Player->GetPendingTransition() : std::nullopt;
    }

    std::optional<float> MusicSetPreview::GetParameter(const std::string& name) const
    {
        if (m_Player)
            return m_Player->GetParameter(name);
        const auto found = m_Parameters.find(name);
        return found == m_Parameters.end() ? std::nullopt : std::optional<float>(found->second);
    }
} // namespace Kans
