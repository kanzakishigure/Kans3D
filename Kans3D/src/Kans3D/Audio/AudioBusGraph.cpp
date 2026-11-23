#include "kspch.h"
#include "AudioBusGraph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Kans
{
    namespace
    {
        static_assert(std::atomic<float>::is_always_lock_free);
        static_assert(std::atomic<uint64_t>::is_always_lock_free);

        float Gain(float db) { return std::pow(10.f, db / 20.f); }

        struct GainRamp
        {
            std::atomic<float> Target {1};
            std::atomic<float> Published {1};
            double             Current = 1, Step = 0;
            float              Destination = 1;
            uint32_t           Remaining   = 0;

            void Initialize(float gain)
            {
                Destination = gain;
                Current     = gain;
                Published.store(gain, std::memory_order_relaxed);
            }
            void Prepare(uint32_t frames)
            {
                const float target = Target.load(std::memory_order_relaxed);
                if (target != Destination)
                {
                    Destination = target;
                    Remaining   = frames;
                    Step        = (target - Current) / frames;
                }
            }
            float Next()
            {
                if (Remaining > 0)
                {
                    Current += Step;
                    if (--Remaining == 0)
                        Current = Destination;
                }
                return static_cast<float>(Current);
            }
            void Publish() { Published.store(static_cast<float>(Current), std::memory_order_relaxed); }
        };

        uint64_t PackMeter(float peak, float rms)
        {
            const float values[] = {peak, rms};
            uint64_t    packed;
            static_assert(sizeof(values) == sizeof(packed));
            std::memcpy(&packed, values, sizeof(packed));
            return packed;
        }

        void UnpackMeter(uint64_t packed, float& peak, float& rms)
        {
            float values[2];
            std::memcpy(values, &packed, sizeof(packed));
            peak = values[0];
            rms  = values[1];
        }

        // Stable input for sources, child buses and sends. Effects can be inserted between this node and the fader.
        const ma_node_vtable InputVtable = {[](ma_node*, const float**, ma_uint32*, float**, ma_uint32*) {},
                                            nullptr,
                                            1,
                                            1,
                                            MA_NODE_FLAG_PASSTHROUGH};

        struct SendOutput
        {
            GainRamp     Level;
            AudioSendTap Tap = AudioSendTap::PostFader;
        };

        struct FaderNode : ma_node_base
        {
            GainRamp                                           Fader, PreFader;
            std::unique_ptr<SendOutput[]>                      Sends;
            size_t                                             SendCount  = 0;
            uint32_t                                           RampFrames = 0;
            uint32_t                                           Channels   = 0;
            std::array<std::atomic<uint64_t>, MA_MAX_CHANNELS> Meters {};

            static void Process(ma_node*      node,
                                const float** inputs,
                                ma_uint32*    inputFrames,
                                float**       outputs,
                                ma_uint32*    outputFrames)
            {
                auto&                               fader  = *static_cast<FaderNode*>(static_cast<ma_node_base*>(node));
                const auto                          frames = std::min(*inputFrames, *outputFrames);
                const auto                          channels = fader.Channels;
                std::array<float, MA_MAX_CHANNELS>  peak {};
                std::array<double, MA_MAX_CHANNELS> sumSquares {};
                fader.Fader.Prepare(fader.RampFrames);
                fader.PreFader.Prepare(fader.RampFrames);
                for (size_t i = 0; i < fader.SendCount; ++i)
                    fader.Sends[i].Level.Prepare(fader.RampFrames);

                const float* source = inputs[0];
                float*       dry    = outputs[0];
                for (ma_uint32 frame = 0; frame < frames; ++frame)
                {
                    const auto  offset = frame * channels;
                    const float post   = fader.Fader.Next();
                    const float pre    = fader.PreFader.Next();
                    for (uint32_t channel = 0; channel < channels; ++channel)
                    {
                        const float sample    = source[offset + channel] * post;
                        dry[offset + channel] = sample;
                        peak[channel]         = std::max(peak[channel], std::abs(sample));
                        sumSquares[channel] += double(sample) * sample;
                    }
                    for (size_t i = 0; i < fader.SendCount; ++i)
                    {
                        auto&       send  = fader.Sends[i];
                        const float tap   = send.Tap == AudioSendTap::PreFader ? pre : post;
                        const float level = send.Level.Next();
                        for (uint32_t channel = 0; channel < channels; ++channel)
                            outputs[i + 1][offset + channel] = source[offset + channel] * tap * level;
                    }
                }
                for (uint32_t channel = 0; channel < channels; ++channel)
                {
                    const auto rms = frames ? static_cast<float>(std::sqrt(sumSquares[channel] / frames)) : 0.f;
                    fader.Meters[channel].store(PackMeter(peak[channel], rms), std::memory_order_relaxed);
                }
                fader.Fader.Publish();
                fader.PreFader.Publish();
                for (size_t i = 0; i < fader.SendCount; ++i)
                    fader.Sends[i].Level.Publish();
                *inputFrames = *outputFrames = frames;
            }
        };

        const ma_node_vtable FaderVtable = {FaderNode::Process,
                                            nullptr,
                                            1,
                                            MA_NODE_BUS_COUNT_UNKNOWN,
                                            MA_NODE_FLAG_CONTINUOUS_PROCESSING};
    } // namespace

    struct AudioBusGraph::Bus
    {
        ma_node_base          Input {};
        FaderNode             Fader {};
        bool                  InputReady = false, FaderReady = false;
        std::optional<size_t> Parent;

        ~Bus()
        {
            if (FaderReady)
                ma_node_uninit(static_cast<ma_node_base*>(&Fader), nullptr);
            if (InputReady)
                ma_node_uninit(&Input, nullptr);
        }
    };

    AudioBusGraph::AudioBusGraph()  = default;
    AudioBusGraph::~AudioBusGraph() = default;

    ma_result AudioBusGraph::Init(ma_engine& engine, const AudioBusLayout& layout, const AudioBusGraph* previous)
    {
        auto* graph           = ma_engine_get_node_graph(&engine);
        m_Endpoint            = ma_node_graph_get_endpoint(graph);
        m_Channels            = ma_engine_get_channels(&engine);
        const auto rampFrames = std::max(1u, ma_engine_get_sample_rate(&engine) / 200); // 5 ms
        m_Layout              = layout;

        for (const auto& definition : m_Layout.Buses)
        {
            auto  storage              = std::make_unique<Bus>();
            auto& bus                  = *storage;
            auto  inputConfig          = ma_node_config_init();
            inputConfig.vtable         = &InputVtable;
            inputConfig.pInputChannels = inputConfig.pOutputChannels = &m_Channels;
            auto result = ma_node_init(graph, &inputConfig, nullptr, &bus.Input);
            if (result != MA_SUCCESS)
                return result;
            bus.InputReady = true;

            auto& fader    = bus.Fader;
            fader.Channels = m_Channels;
            for (uint32_t channel = 0; channel < m_Channels; ++channel)
                fader.Meters[channel].store(0, std::memory_order_relaxed);
            fader.RampFrames = rampFrames;
            fader.SendCount  = definition.Sends.size();
            if (fader.SendCount != 0)
                fader.Sends = std::make_unique<SendOutput[]>(fader.SendCount);
            for (size_t i = 0; i < fader.SendCount; ++i)
            {
                const auto& send   = definition.Sends[i];
                fader.Sends[i].Tap = send.Tap;
                const float gain   = Gain(send.LevelDb);
                fader.Sends[i].Level.Initialize(gain);
                fader.Sends[i].Level.Target.store(gain, std::memory_order_relaxed);
            }

            const std::vector<ma_uint32> outputChannels(fader.SendCount + 1, m_Channels);
            auto                         faderConfig = ma_node_config_init();
            faderConfig.vtable                       = &FaderVtable;
            faderConfig.pInputChannels               = &m_Channels;
            faderConfig.pOutputChannels              = outputChannels.data();
            faderConfig.outputBusCount               = static_cast<ma_uint32>(outputChannels.size());
            result = ma_node_init(graph, &faderConfig, nullptr, static_cast<ma_node_base*>(&fader));
            if (result != MA_SUCCESS)
                return result;
            bus.FaderReady = true;
            result         = ma_node_attach_output_bus(&bus.Input, 0, static_cast<ma_node_base*>(&fader), 0);
            if (result != MA_SUCCESS)
                return result;
            m_Names.emplace(definition.Name, m_Buses.size());
            m_Buses.push_back(std::move(storage));
        }

        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            auto&       bus        = *m_Buses[i];
            const auto& definition = m_Layout.Buses[i];
            auto*       fader      = static_cast<ma_node_base*>(&bus.Fader);
            if (!definition.Parent.empty())
            {
                bus.Parent = m_Names.at(definition.Parent);
                if (const auto result = ma_node_attach_output_bus(fader, 0, Input(definition.Parent), 0);
                    result != MA_SUCCESS)
                    return result;
            }
            for (size_t j = 0; j < definition.Sends.size(); ++j)
                if (const auto result = ma_node_attach_output_bus(
                        fader, static_cast<ma_uint32>(j + 1), Input(definition.Sends[j].Target), 0);
                    result != MA_SUCCESS)
                    return result;
        }

        UpdateFaders();
        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            auto& fader = m_Buses[i]->Fader;
            fader.Fader.Initialize(fader.Fader.Target.load(std::memory_order_relaxed));
            fader.PreFader.Initialize(fader.PreFader.Target.load(std::memory_order_relaxed));
            if (!previous)
                continue;
            const auto found = previous->m_Names.find(m_Layout.Buses[i].Name);
            if (found == previous->m_Names.end())
                continue;
            const auto& old = previous->m_Buses[found->second]->Fader;
            fader.Fader.Initialize(old.Fader.Published.load(std::memory_order_relaxed));
            fader.PreFader.Initialize(old.PreFader.Published.load(std::memory_order_relaxed));
            const auto& oldSends = previous->m_Layout.Buses[found->second].Sends;
            const auto& sends    = m_Layout.Buses[i].Sends;
            for (size_t j = 0; j < std::min(sends.size(), oldSends.size()); ++j)
                if (sends[j].Target == oldSends[j].Target && sends[j].Tap == oldSends[j].Tap)
                    fader.Sends[j].Level.Initialize(old.Sends[j].Level.Published.load(std::memory_order_relaxed));
        }
        return MA_SUCCESS;
    }

    ma_result AudioBusGraph::ConnectEndpoint()
    {
        auto& master = m_Buses[m_Names.at(MasterAudioBus)]->Fader;
        return ma_node_attach_output_bus(static_cast<ma_node_base*>(&master), 0, m_Endpoint, 0);
    }

    ma_node* AudioBusGraph::Input(const std::string& bus) const
    {
        const auto it = m_Names.find(bus);
        return it == m_Names.end() ? nullptr : &m_Buses[it->second]->Input;
    }

    AudioBusDefinition* AudioBusGraph::Find(const std::string& bus)
    {
        const auto it = m_Names.find(bus);
        return it == m_Names.end() ? nullptr : &m_Layout.Buses[it->second];
    }

    std::optional<AudioBusMeter> AudioBusGraph::GetMeter(const std::string& bus) const
    {
        const auto it = m_Names.find(bus);
        if (it == m_Names.end())
            return std::nullopt;
        const auto&   meters = m_Buses[it->second]->Fader.Meters;
        AudioBusMeter snapshot;
        snapshot.Channels.resize(m_Channels);
        for (uint32_t channel = 0; channel < m_Channels; ++channel)
            UnpackMeter(meters[channel].load(std::memory_order_relaxed),
                        snapshot.Channels[channel].Peak,
                        snapshot.Channels[channel].Rms);
        return snapshot;
    }

    void AudioBusGraph::UpdateFaders()
    {
        const auto& definitions = m_Layout.Buses;
        const bool  soloActive =
            std::any_of(definitions.begin(), definitions.end(), [](const auto& bus) { return bus.Solo; });
        std::vector<bool> ancestors(m_Buses.size());
        for (size_t i = 0; i < m_Buses.size(); ++i)
            if (definitions[i].Solo)
                for (auto parent = m_Buses[i]->Parent; parent && !ancestors[*parent]; parent = m_Buses[*parent]->Parent)
                    ancestors[*parent] = true;

        // During solo, only soloed buses, their ancestors and their descendants pass audio.
        // Solo silences both taps. Mute and fader level affect only the post-fader signal.
        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            bool audible = !soloActive || ancestors[i];
            for (std::optional<size_t> bus = i; bus && !audible; bus = m_Buses[*bus]->Parent)
                audible = definitions[*bus].Solo;
            auto& fader = m_Buses[i]->Fader;
            fader.PreFader.Target.store(audible ? 1.f : 0.f, std::memory_order_relaxed);
            fader.Fader.Target.store(audible && !definitions[i].Mute ? Gain(definitions[i].VolumeDb) : 0.f,
                                     std::memory_order_relaxed);
        }
    }

    void AudioBusGraph::UpdateSend(const std::string& bus, size_t index)
    {
        const auto i = m_Names.at(bus);
        m_Buses[i]->Fader.Sends[index].Level.Target.store(Gain(m_Layout.Buses[i].Sends[index].LevelDb),
                                                          std::memory_order_relaxed);
    }
} // namespace Kans
