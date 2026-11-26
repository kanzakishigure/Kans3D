#pragma once

#include <miniaudio.h>
#include <atomic>
#include <memory>
#include <unordered_map>
#include "AudioBusEffect.h"

namespace Kans
{
    struct AudioDSPRegistration
    {
        AudioDSPCallback      Callback = nullptr;
        void*                 User     = nullptr;
        std::atomic<bool>     Enabled {true};
        std::atomic<uint32_t> InFlight {0};
    };
    using AudioDSPRegistry = std::unordered_map<std::string, std::shared_ptr<AudioDSPRegistration>>;

    // Each bus stores the last two graph pulls. Only the graph's audio thread accesses these.
    struct AudioSidechainPeak
    {
        uint64_t Time    = UINT64_MAX;
        float    Current = 0, Previous = 0;

        void  Publish(uint64_t time, float peak);
        float ReadPrevious(uint64_t time) const;
    };

    struct AudioEffectMix
    {
        std::atomic<bool> Enabled {true};
        // The audio thread latches the chain target at the input splitter before it pulls the effects.
        bool     BlockEnabled = true;
        uint32_t Position = 1, RampFrames = 1;

        bool  Active(bool enabled) const;
        float Weight() const;
        float Next(bool enabled);
        void  Blend(const float* dry, float* wet, uint32_t frames, uint32_t channels, bool enabled);
    };

    class AudioBusEffectNode : public ma_node_base
    {
    public:
        AudioBusEffectNode();
        ~AudioBusEffectNode();

        ma_result Init(ma_node_graph*                        graph,
                       const ma_channel*                     channelMap,
                       uint32_t                              channels,
                       uint32_t                              rate,
                       const AudioBusEffect&                 effect,
                       const AudioEffectMix&                 chainMix,
                       const AudioSidechainPeak*             detector,
                       std::shared_ptr<AudioDSPRegistration> callback);
        bool      CanUpdate(const AudioBusEffectParameters& params, const AudioDSPRegistration* callback) const;
        void      Update(const AudioBusEffect& effect, const AudioSidechainPeak* detector);
        float     GetReductionDb() const;

    private:
        struct State;
        std::unique_ptr<State> m_State;
        bool                   m_Ready = false;

        static void
        Process(ma_node* node, const float** inputs, ma_uint32* inputFrames, float** outputs, ma_uint32* outputFrames);
    };
} // namespace Kans
