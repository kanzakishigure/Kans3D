#include "kspch.h"
#include "AudioBusGraph.h"
#include "AudioChannelRouting.h"
#include "AudioGainRamp.h"

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

        // Stable input for sources, child buses and sends.
        const ma_node_vtable InputVtable = {[](ma_node*, const float**, ma_uint32*, float**, ma_uint32*) {},
                                            nullptr,
                                            1,
                                            1,
                                            MA_NODE_FLAG_PASSTHROUGH};

        struct ChainInput : ma_node_base
        {
            AudioEffectMix* Mix      = nullptr;
            uint32_t        Channels = 0;

            static void Process(ma_node*      node,
                                const float** inputs,
                                ma_uint32*    inputFrames,
                                float**       outputs,
                                ma_uint32*    outputFrames)
            {
                auto& split             = *static_cast<ChainInput*>(static_cast<ma_node_base*>(node));
                split.Mix->BlockEnabled = split.Mix->Enabled.load(std::memory_order_relaxed);
                *inputFrames = *outputFrames = std::min(*inputFrames, *outputFrames);
                const size_t bytes           = size_t(*outputFrames) * split.Channels * sizeof(float);
                std::memcpy(outputs[0], inputs[0], bytes);
                std::memcpy(outputs[1], inputs[0], bytes);
            }
        };

        struct ChainOutput : ma_node_base
        {
            AudioEffectMix* Mix      = nullptr;
            uint32_t        Channels = 0;

            static void Process(ma_node*      node,
                                const float** inputs,
                                ma_uint32*    inputFrames,
                                float**       outputs,
                                ma_uint32*    outputFrames)
            {
                auto& blend  = *static_cast<ChainOutput*>(static_cast<ma_node_base*>(node));
                *inputFrames = *outputFrames = std::min(*inputFrames, *outputFrames);
                const bool  enabled          = blend.Mix->BlockEnabled;
                const auto* source           = blend.Mix->Active(enabled) ? inputs[1] : inputs[0];
                std::memcpy(outputs[0], source, size_t(*outputFrames) * blend.Channels * sizeof(float));
                blend.Mix->Blend(inputs[0], outputs[0], *outputFrames, blend.Channels, enabled);
            }
        };

        const ma_node_vtable ChainInputVtable  = {ChainInput::Process,
                                                  nullptr,
                                                  1,
                                                  2,
                                                  MA_NODE_FLAG_CONTINUOUS_PROCESSING};
        const ma_node_vtable ChainOutputVtable = {ChainOutput::Process,
                                                  nullptr,
                                                  2,
                                                  1,
                                                  MA_NODE_FLAG_CONTINUOUS_PROCESSING};

        std::shared_ptr<AudioDSPRegistration> ResolveCallback(const AudioBusEffectParameters& params,
                                                              const AudioDSPRegistry&         registry)
        {
            if (const auto* custom = std::get_if<AudioCustomDSP>(&params))
                if (const auto found = registry.find(custom->Name); found != registry.end())
                    return found->second;
            return {};
        }

        struct SendOutput
        {
            AudioGainRamp Level;
            AudioSendTap Tap = AudioSendTap::PostFader;
        };

        struct FaderNode : ma_node_base
        {
            AudioGainRamp                                      Fader, PreFader;
            std::atomic<bool>                                  PinToCenter {false};
            int                                                Center = -1, Lfe = -1;
            ma_node_graph*                                     Graph = nullptr;
            AudioSidechainPeak                                 Sidechain;
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
                const bool   pin    = channels > 1 && fader.PinToCenter.load(std::memory_order_relaxed);
                std::array<float, MA_MAX_CHANNELS> pinned {};
                float*       dry    = outputs[0];
                for (ma_uint32 frame = 0; frame < frames; ++frame)
                {
                    const auto  offset = frame * channels;
                    const float post   = fader.Fader.Next();
                    const float pre    = fader.PreFader.Next();
                    const float* frameSource = source + offset;
                    if (pin)
                    {
                        float mono = 0;
                        for (uint32_t channel = 0; channel < channels; ++channel)
                            if (int(channel) != fader.Lfe)
                                mono += frameSource[channel];
                        RouteAudioCenter(pinned.data(), channels, fader.Center, mono / (channels - (fader.Lfe >= 0)));
                        frameSource = pinned.data();
                    }
                    for (uint32_t channel = 0; channel < channels; ++channel)
                    {
                        const float sample    = frameSource[channel] * post;
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
                            outputs[i + 1][offset + channel] = frameSource[channel] * tap * level;
                    }
                }
                fader.Sidechain.Publish(ma_node_graph_get_time(fader.Graph),
                                        *std::max_element(peak.begin(), peak.begin() + channels));
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
        float                                            Audibility = -1;
        bool                  InputReady = false, FaderReady = false;
        std::optional<size_t> Parent;
        AudioEffectMix                                   ChainMix;
        ChainInput                                       Split {};
        ChainOutput                                      Blend {};
        bool                                             SplitReady = false, BlendReady = false;
        std::vector<std::unique_ptr<AudioBusEffectNode>> Effects;

        ~Bus()
        {
            if (FaderReady)
                ma_node_uninit(static_cast<ma_node_base*>(&Fader), nullptr);
            if (BlendReady)
                ma_node_uninit(static_cast<ma_node_base*>(&Blend), nullptr);
            Effects.clear();
            if (SplitReady)
                ma_node_uninit(static_cast<ma_node_base*>(&Split), nullptr);
            if (InputReady)
                ma_node_uninit(&Input, nullptr);
        }
    };

    AudioBusGraph::AudioBusGraph()  = default;
    AudioBusGraph::~AudioBusGraph()
    {
        // Wait for any endpoint pull before destroying buses referenced by another bus's detector.
        if (m_Connected)
            ma_node_detach_output_bus(static_cast<ma_node_base*>(&m_Buses[m_Names.at(MasterAudioBus)]->Fader), 0);
    }

    ma_result AudioBusGraph::Init(ma_engine&              engine,
                                  const AudioBusLayout&   layout,
                                  const AudioBusGraph*    previous,
                                  const AudioDSPRegistry& registry)
    {
        auto* graph           = ma_engine_get_node_graph(&engine);
        m_Endpoint            = ma_node_graph_get_endpoint(graph);
        m_Channels            = ma_engine_get_channels(&engine);
        std::copy_n(engine.listeners[0].config.pChannelMapOut, m_Channels, m_ChannelMap.begin());
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
            fader.Graph    = graph;
            fader.Center   = AudioChannelIndex(m_ChannelMap.data(), m_Channels, MA_CHANNEL_FRONT_CENTER);
            fader.Lfe      = AudioChannelIndex(m_ChannelMap.data(), m_Channels, MA_CHANNEL_LFE);
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
            m_Names.emplace(definition.Name, m_Buses.size());
            m_Buses.push_back(std::move(storage));
        }

        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            auto&       bus        = *m_Buses[i];
            const auto& definition = m_Layout.Buses[i];
            auto*       fader      = static_cast<ma_node_base*>(&bus.Fader);
            if (const auto result = InitChain(graph, ma_engine_get_sample_rate(&engine), i, registry);
                result != MA_SUCCESS)
                return result;
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

    const AudioSidechainPeak* AudioBusGraph::Detector(const AudioBusEffectParameters& params) const
    {
        if (const auto* ducking = std::get_if<AudioDucking>(&params))
            return &m_Buses[m_Names.at(ducking->SourceBus)]->Fader.Sidechain;
        return nullptr;
    }

    ma_result
    AudioBusGraph::InitChain(ma_node_graph* graph, uint32_t rate, size_t index, const AudioDSPRegistry& registry)
    {
        auto&       bus        = *m_Buses[index];
        const auto& definition = m_Layout.Buses[index];
        auto*       fader      = static_cast<ma_node_base*>(&bus.Fader);
        if (definition.Effects.empty())
            return ma_node_attach_output_bus(&bus.Input, 0, fader, 0);

        const uint32_t twoPaths[] = {m_Channels, m_Channels};
        bus.ChainMix.RampFrames   = std::max(1u, rate / 200);
        bus.ChainMix.BlockEnabled = !definition.Bypass;
        bus.ChainMix.Enabled.store(!definition.Bypass, std::memory_order_relaxed);
        bus.ChainMix.Position = definition.Bypass ? 0 : bus.ChainMix.RampFrames;
        bus.Split.Channels = bus.Blend.Channels = m_Channels;
        bus.Split.Mix = bus.Blend.Mix = &bus.ChainMix;
        auto config                   = ma_node_config_init();
        config.vtable                 = &ChainInputVtable;
        config.pInputChannels         = &m_Channels;
        config.pOutputChannels        = twoPaths;
        auto result                   = ma_node_init(graph, &config, nullptr, static_cast<ma_node_base*>(&bus.Split));
        if (result != MA_SUCCESS)
            return result;
        bus.SplitReady         = true;
        config.vtable          = &ChainOutputVtable;
        config.pInputChannels  = twoPaths;
        config.pOutputChannels = &m_Channels;
        result                 = ma_node_init(graph, &config, nullptr, static_cast<ma_node_base*>(&bus.Blend));
        if (result != MA_SUCCESS)
            return result;
        bus.BlendReady = true;

        auto* split = static_cast<ma_node_base*>(&bus.Split);
        auto* blend = static_cast<ma_node_base*>(&bus.Blend);
        result      = ma_node_attach_output_bus(&bus.Input, 0, split, 0);
        if (result == MA_SUCCESS)
            result = ma_node_attach_output_bus(split, 0, blend, 0);
        if (result == MA_SUCCESS)
            result = ma_node_attach_output_bus(blend, 0, fader, 0);
        if (result != MA_SUCCESS)
            return result;

        ma_node*  previous       = split;
        ma_uint32 previousOutput = 1;
        bus.Effects.reserve(definition.Effects.size());
        for (const auto& effect : definition.Effects)
        {
            auto node = std::make_unique<AudioBusEffectNode>();
            result    = node->Init(graph,
                                   m_ChannelMap.data(),
                                   m_Channels,
                                   rate,
                                   effect,
                                   bus.ChainMix,
                                   Detector(effect.Parameters),
                                   ResolveCallback(effect.Parameters, registry));
            if (result == MA_SUCCESS)
                result = ma_node_attach_output_bus(previous, previousOutput, node.get(), 0);
            if (result != MA_SUCCESS)
                return result;
            previous       = node.get();
            previousOutput = 0;
            bus.Effects.push_back(std::move(node));
        }
        return ma_node_attach_output_bus(previous, previousOutput, blend, 1);
    }

    bool AudioBusGraph::TryUpdate(const AudioBusLayout& layout, const AudioDSPRegistry& registry)
    {
        if (layout.Buses.size() != m_Buses.size())
            return false;
        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            const auto& next     = layout.Buses[i];
            const auto& previous = m_Layout.Buses[i];
            if (next.Name != previous.Name || next.Parent != previous.Parent ||
                next.Sends.size() != previous.Sends.size() || next.Effects.size() != previous.Effects.size())
                return false;
            for (size_t j = 0; j < next.Sends.size(); ++j)
                if (next.Sends[j].Target != previous.Sends[j].Target || next.Sends[j].Tap != previous.Sends[j].Tap)
                    return false;
            for (size_t j = 0; j < next.Effects.size(); ++j)
            {
                const auto& params = next.Effects[j].Parameters;
                if (!m_Buses[i]->Effects[j]->CanUpdate(params, ResolveCallback(params, registry).get()))
                    return false;
            }
        }
        m_Layout = layout;
        UpdateFaders();
        for (size_t i = 0; i < m_Buses.size(); ++i)
        {
            const auto& next = m_Layout.Buses[i];
            UpdateBypass(next.Name, next.Bypass);
            for (size_t j = 0; j < next.Sends.size(); ++j)
                UpdateSend(next.Name, j);
            for (size_t j = 0; j < next.Effects.size(); ++j)
            {
                const auto& effect = next.Effects[j];
                m_Buses[i]->Effects[j]->Update(effect, Detector(effect.Parameters));
            }
        }
        return true;
    }

    bool AudioBusGraph::SetEffectParameters(const std::string&              bus,
                                            size_t                          index,
                                            const AudioBusEffectParameters& params,
                                            const AudioDSPRegistry&         registry)
    {
        auto& node = *m_Buses[m_Names.at(bus)]->Effects[index];
        if (!node.CanUpdate(params, ResolveCallback(params, registry).get()))
            return false;
        auto& effect      = Find(bus)->Effects[index];
        effect.Parameters = params;
        node.Update(effect, Detector(params));
        return true;
    }

    void AudioBusGraph::UpdateEffectEnabled(const std::string& bus, size_t index, bool enabled)
    {
        auto& effect   = Find(bus)->Effects[index];
        effect.Enabled = enabled;
        m_Buses[m_Names.at(bus)]->Effects[index]->Update(effect, Detector(effect.Parameters));
    }

    void AudioBusGraph::UpdateBypass(const std::string& bus, bool bypass)
    {
        m_Buses[m_Names.at(bus)]->ChainMix.Enabled.store(!bypass, std::memory_order_relaxed);
    }

    std::optional<float> AudioBusGraph::GetEffectReductionDb(const std::string& bus, size_t index) const
    {
        const auto it = m_Names.find(bus);
        if (it == m_Names.end() || index >= m_Buses[it->second]->Effects.size())
            return std::nullopt;
        const auto& params = m_Layout.Buses[it->second].Effects[index].Parameters;
        if (!std::holds_alternative<AudioCompressor>(params) && !std::holds_alternative<AudioLimiter>(params) &&
            !std::holds_alternative<AudioDucking>(params))
            return std::nullopt;
        return m_Buses[it->second]->Effects[index]->GetReductionDb();
    }

    ma_result AudioBusGraph::ConnectEndpoint()
    {
        auto& master = m_Buses[m_Names.at(MasterAudioBus)]->Fader;
        const auto result = ma_node_attach_output_bus(static_cast<ma_node_base*>(&master), 0, m_Endpoint, 0);
        m_Connected       = result == MA_SUCCESS;
        return result;
    }

    ma_node* AudioBusGraph::Input(const std::string& bus) const
    {
        const auto it = m_Names.find(bus);
        return it == m_Names.end() ? nullptr : &m_Buses[it->second]->Input;
    }

    ma_node* AudioBusGraph::Input(size_t index) const
    {
        return index < m_Buses.size() ? &m_Buses[index]->Input : nullptr;
    }

    std::optional<size_t> AudioBusGraph::FindIndex(const std::string& bus) const
    {
        const auto it = m_Names.find(bus);
        return it == m_Names.end() ? std::nullopt : std::optional<size_t>(it->second);
    }

    AudioBusDefinition* AudioBusGraph::Find(const std::string& bus)
    {
        const auto it = m_Names.find(bus);
        return it == m_Names.end() ? nullptr : &m_Layout.Buses[it->second];
    }

    std::optional<AudioBusMeter> AudioBusGraph::GetMeter(const std::string& bus) const
    {
        AudioBusMeter snapshot;
        if (!ReadMeter(bus, snapshot))
            return std::nullopt;
        return snapshot;
    }

    bool AudioBusGraph::ReadMeter(const std::string& bus, AudioBusMeter& meter) const
    {
        const auto it = m_Names.find(bus);
        if (it == m_Names.end())
            return false;
        const auto& meters = m_Buses[it->second]->Fader.Meters;
        meter.Channels.resize(m_Channels);
        for (uint32_t channel = 0; channel < m_Channels; ++channel)
            UnpackMeter(meters[channel].load(std::memory_order_relaxed),
                        meter.Channels[channel].Peak,
                        meter.Channels[channel].Rms);
        return true;
    }

    void AudioBusGraph::UpdateFaders()
    {
        m_AudibilityDirty       = true;
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
            fader.PinToCenter.store(definitions[i].PinToCenter, std::memory_order_relaxed);
            fader.PreFader.Target.store(audible ? 1.f : 0.f, std::memory_order_relaxed);
            fader.Fader.Target.store(audible && !definitions[i].Mute ? Gain(definitions[i].VolumeDb) : 0.f,
                                     std::memory_order_relaxed);
        }
    }

    void AudioBusGraph::UpdateSend(const std::string& bus, size_t index)
    {
        m_AudibilityDirty = true;
        const auto i = m_Names.at(bus);
        m_Buses[i]->Fader.Sends[index].Level.Target.store(Gain(m_Layout.Buses[i].Sends[index].LevelDb),
                                                          std::memory_order_relaxed);
    }

    float AudioBusGraph::AudibilityGain(const std::string& bus)
    {
        if (m_AudibilityDirty)
        {
            for (auto& entry : m_Buses)
                entry->Audibility = -1;
            m_AudibilityDirty = false;
        }
        const auto found = m_Names.find(bus);
        return ResolveAudibilityGain(found == m_Names.end() ? m_Names.at(MasterAudioBus) : found->second);
    }

    float AudioBusGraph::ResolveAudibilityGain(size_t index)
    {
        auto& bus = *m_Buses[index];
        if (bus.Audibility >= 0)
            return bus.Audibility;
        const auto post = bus.Fader.Fader.Target.load(std::memory_order_relaxed);
        const auto pre  = bus.Fader.PreFader.Target.load(std::memory_order_relaxed);
        // Routing is an acyclic graph. Estimate gain using the strongest path to the output.
        float gain = bus.Parent ? post * ResolveAudibilityGain(*bus.Parent) : post;
        for (const auto& send : m_Layout.Buses[index].Sends)
            gain = std::max(gain,
                            (send.Tap == AudioSendTap::PreFader ? pre : post) * Gain(send.LevelDb) *
                                ResolveAudibilityGain(m_Names.at(send.Target)));
        return bus.Audibility = gain;
    }
} // namespace Kans
