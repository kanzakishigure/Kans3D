#pragma once

#include <miniaudio.h>
#include <memory>
#include <unordered_map>
#include "AudioEngine.h"

namespace Kans
{
    // Owns the miniaudio nodes. All methods are called on the main thread.
    class AudioBusGraph
    {
    public:
        AudioBusGraph();
        ~AudioBusGraph();

        ma_result           Init(ma_engine& engine, const AudioBusLayout& layout, const AudioBusGraph* previous);
        ma_result           ConnectEndpoint();
        ma_node*            Input(const std::string& bus) const;
        AudioBusDefinition* Find(const std::string& bus);
        const std::vector<AudioBusDefinition>& GetBuses() const { return m_Layout.Buses; }
        std::optional<AudioBusMeter>           GetMeter(const std::string& bus) const;
        void                                   UpdateFaders();
        void                                   UpdateSend(const std::string& bus, size_t index);

    private:
        struct Bus;
        AudioBusLayout                          m_Layout;
        std::vector<std::unique_ptr<Bus>>       m_Buses;
        std::unordered_map<std::string, size_t> m_Names;
        ma_node*                                m_Endpoint = nullptr;
        uint32_t                                m_Channels = 0;
    };
} // namespace Kans
