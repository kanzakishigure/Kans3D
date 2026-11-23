#pragma once

#include <miniaudio.h>
#include <memory>
#include <unordered_map>
#include "AudioBusEffectNode.h"
#include "AudioEngine.h"

namespace Kans
{
    // Owns the miniaudio nodes. All methods are called on the main thread.
    class AudioBusGraph
    {
    public:
        AudioBusGraph();
        ~AudioBusGraph();

        ma_result            Init(ma_engine&              engine,
                                  const AudioBusLayout&   layout,
                                  const AudioBusGraph*    previous,
                                  const AudioDSPRegistry& registry);
        bool                 TryUpdate(const AudioBusLayout& layout, const AudioDSPRegistry& registry);
        bool                 SetEffectParameters(const std::string&              bus,
                                                 size_t                          index,
                                                 const AudioBusEffectParameters& params,
                                                 const AudioDSPRegistry&         registry);
        void                 UpdateEffectEnabled(const std::string& bus, size_t index, bool enabled);
        void                 UpdateBypass(const std::string& bus, bool bypass);
        std::optional<float> GetEffectReductionDb(const std::string& bus, size_t index) const;
        ma_result           ConnectEndpoint();
        ma_node*            Input(const std::string& bus) const;
        AudioBusDefinition* Find(const std::string& bus);
        const std::vector<AudioBusDefinition>& GetBuses() const { return m_Layout.Buses; }
        std::optional<AudioBusMeter>           GetMeter(const std::string& bus) const;
        bool                                   ReadMeter(const std::string& bus, AudioBusMeter& meter) const;
        void                                   UpdateFaders();
        void                                   UpdateSend(const std::string& bus, size_t index);

    private:
        struct Bus;
        const AudioSidechainPeak* Detector(const AudioBusEffectParameters& params) const;
        ma_result InitChain(ma_node_graph* graph, uint32_t rate, size_t index, const AudioDSPRegistry& registry);
        AudioBusLayout                          m_Layout;
        std::vector<std::unique_ptr<Bus>>       m_Buses;
        std::unordered_map<std::string, size_t> m_Names;
        ma_node*                                m_Endpoint = nullptr;
        bool                                    m_Connected = false;
        uint32_t                                m_Channels = 0;
    };
} // namespace Kans
