#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Audio/MusicSetPlayer.h"

namespace Kans
{
    class AssetManager;

    // Auditions the music set being edited outside Play mode: Play loads a snapshot of the edited data
    // into a MusicSetPlayer of its own (stems resolved through the asset manager) and starts a state;
    // state, parameter and stinger requests go to that player. An edit while it plays reloads the
    // snapshot and resumes the same state at the same position. Separate from scene audio. The engine
    // and asset manager must outlive their binding: Bind(nullptr, nullptr) before either goes away.
    class MusicSetPreview
    {
    public:
        MusicSetPreview();
        ~MusicSetPreview() { Bind(nullptr, nullptr); }
        MusicSetPreview(const MusicSetPreview&)            = delete;
        MusicSetPreview& operator=(const MusicSetPreview&) = delete;

        void Bind(AudioEngine* engine, AssetManager* assets);
        bool IsAvailable() const { return m_Engine && m_Assets && m_Engine->IsInitialized(); }

        // Loads `data` and plays `state` (empty: the first). False with the reason in GetError otherwise.
        bool Play(const MusicSetData& data, const std::string& state = {});
        void Stop();
        bool IsPlaying() const;
        bool SetState(const std::string& state);
        bool SetParameter(const std::string& name, float value);
        bool TriggerStinger(const std::string& name);
        // Once per frame: runs the player, and reloads it when `revision` (the document's) changed while playing.
        void Update(const MusicSetData& data, uint64_t revision);

        std::optional<MusicSetPosition>          GetPosition() const;
        std::optional<MusicSetPendingTransition> GetPendingTransition() const;
        // The value the player uses (the parameter's default until set); none when not loaded.
        std::optional<float> GetParameter(const std::string& name) const;
        const std::string&   GetError() const { return m_Error; }
        MusicSetPlayer*      GetPlayer() { return m_Player.get(); }

    private:
        bool Load(const MusicSetData& data);

        AudioEngine*                                 m_Engine = nullptr;
        AssetManager*                                m_Assets = nullptr;
        AssetID                                      m_ID;
        std::unique_ptr<AudioClipRegistry>           m_Clips;
        std::unique_ptr<MusicSetPlayer>              m_Player;
        std::map<AssetID, Ref<const AudioClipAsset>> m_Loaded;
        std::map<std::string, float>                 m_Parameters; // Set during the audition, kept across reloads.
        std::string                                  m_Error;
        uint64_t                                     m_Revision = 0;
        bool                                         m_Playing  = false;
    };
} // namespace Kans
