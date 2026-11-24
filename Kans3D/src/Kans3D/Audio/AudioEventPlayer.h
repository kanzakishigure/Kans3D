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
        // Plays `asset` under the selection, cooldown, limit and statistics kept for `event`, without
        // loading `event` (an editor preview of unsaved edits); its clips still load from `assets`.
        AudioEventTriggerResult Trigger(AssetManager&                     assets,
                                        AudioClipRegistry&                clips,
                                        AssetID                           event,
                                        const Ref<const AudioEventAsset>& asset,
                                        const AudioEventParams&           params = {});
        AudioEventStats GetStats(AssetID event) const;
        // The events triggered since the engine started (and not forgotten), in ID order.
        std::vector<AssetID> GetTriggeredEvents() const;
        // Drops the selection, cooldown and statistics kept for `event` (playing instances continue).
        void Forget(AssetID event);
        // Reseed per-event RNGs for their next draw. Existing selection and cooldown state are preserved.
        void SetSeed(uint64_t seed);

    private:
        friend class AudioEngine;
        explicit AudioEventPlayer(AudioEngine& engine);
        void Clear();
        struct State;
        State&                  StateOf(AssetID event);
        AudioEventTriggerResult Play(State&                            state,
                                     AssetManager&                     assets,
                                     AudioClipRegistry&                clips,
                                     AssetID                           event,
                                     const Ref<const AudioEventAsset>& asset,
                                     const AudioEventParams&           params);

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
