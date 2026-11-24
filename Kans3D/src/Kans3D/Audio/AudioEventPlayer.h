#pragma once

#include <random>
#include "AudioEvent.h"

namespace Kans
{
    class AssetManager;

    // Engine-owned. Selection, cooldown and statistics are shared by all emitters of an asset.
    // Main-thread only. Play uses its caller's registry for ownership of the resulting voice.
    class AudioEventPlayer
    {
    public:
        AudioEventTriggerResult
        Trigger(AssetManager& assets, AudioClipRegistry& clips, AssetID event, const AudioEventParams& params = {});
        AudioEventStats GetStats(AssetID event) const;
        // Reseed per-event RNGs for their next draw. Existing selection and cooldown state are preserved.
        void SetSeed(uint64_t seed);

    private:
        friend class AudioEngine;
        explicit AudioEventPlayer(AudioEngine& engine);
        void Clear();

        struct State
        {
            Ref<const AudioEventAsset> Event;
            AudioEventStats            Stats;
            std::optional<uint64_t>    LastTrigger;
            std::mt19937_64            Rng;
            AudioVoiceDesc             VoiceDesc; // Reuses routing string storage across triggers.
            std::vector<size_t>        Bag, History;
            std::vector<uint8_t>       Excluded;
            std::optional<size_t>      LastShuffled;
            size_t                     Next = 0, Remaining = 0, HistoryHead = 0, HistorySize = 0;
            bool                       Valid = false;

            void   Update(Ref<const AudioEventAsset> event);
            size_t Pick();
            void   Accept(size_t index);
        };

        AudioEngine&             m_Engine;
        uint64_t                 m_Seed;
        std::map<AssetID, State> m_Events;
    };
} // namespace Kans
