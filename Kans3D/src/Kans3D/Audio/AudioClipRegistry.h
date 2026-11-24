#pragma once
#include <map>
#include "Kans3D/Asset/Runtime/AudioClipData.h"

namespace Kans
{
    // Main-thread only, bound to one engine while nonempty. Engine shutdown calls Clear automatically.
    class AudioClipRegistry
    {
    public:
        AudioClipRegistry() = default;
        ~AudioClipRegistry() { Clear(); }
        AudioClipRegistry(const AudioClipRegistry&)            = delete;
        AudioClipRegistry& operator=(const AudioClipRegistry&) = delete;

        [[nodiscard]] std::optional<AudioSoundID> Acquire(AudioEngine& engine, const Ref<const AudioClipAsset>& clip);
        void                                      Release(AssetID id);
        void                                      Clear();
        uint64_t                                  GetUserCount(AssetID id) const;
        // Requires an existing Acquire by the owner; creating a voice does not add a user.
        // The owner's last Release unregisters the sound and destroys any remaining voices.
        // Volume is a multiplier of the imported default. Sound/LoadMode always come from the clip.
        [[nodiscard]] AudioVoiceHandle
        CreateVoice(AudioEngine& engine, const Ref<const AudioClipAsset>& clip, AudioVoiceDesc overrides = {});

    private:
        struct Entry
        {
            AudioSoundID Sound;
            uint64_t     Users;
        };
        std::map<AssetID, Entry> m_Clips;
        AudioEngine*             m_Engine    = nullptr;
        AudioSoundID             m_NextSound = 1; // Never reset; AssetIDs are never converted to sound IDs.
    };
} // namespace Kans
