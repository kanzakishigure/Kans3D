#include "kspch.h"
#include "AudioBusEffectNode.h"
#include "AudioBusEffectFields.h"
#include "AudioChannelRouting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace Kans
{
    namespace
    {
        static_assert(std::atomic<float>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
        static_assert(std::atomic<const AudioSidechainPeak*>::is_always_lock_free);

        float ToGain(float db) { return std::pow(10.f, db * .05f); }
        float ToDb(float gain) { return 20.f * std::log10(std::max(gain, 1e-20f)); }
        float Decay(float ms, uint32_t rate) { return std::exp(-1000.f / (ms * rate)); }
        float FlushQuiet(float sample) { return std::abs(sample) < 1e-20f ? 0.f : sample; }

        void Copy(const float* input, float* output, size_t samples)
        {
            if (input != output)
                std::memcpy(output, input, samples * sizeof(float));
        }

        float Mix(float dry, float wet, float amount)
        {
            if (amount == 0)
                return dry;
            if (amount == 1)
                return wet;
            return (1.f - amount) * dry + amount * wet;
        }

        float Peak(const float* frame, uint32_t channels)
        {
            float peak = 0;
            for (uint32_t c = 0; c < channels; ++c)
                peak = std::max(peak, std::abs(frame[c]));
            return peak;
        }

        using Values = std::array<float, 6>;

        Values NumericParameters(const AudioBusEffectParameters& parameters)
        {
            Values values {};
            size_t index = 0;
            std::visit(
                [&](const auto& params) {
                    VisitAudioEffectFields(params, [&](const char*, const auto& value, auto...) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, float>)
                            values[index++] = value;
                    });
                },
                parameters);
            return values;
        }

        struct ParameterRamp
        {
            std::array<std::atomic<float>, 6> Targets {};
            Values                            Current {}, Destination {}, Step {};
            uint32_t                          Remaining = 0;

            void Set(const Values& values)
            {
                for (size_t i = 0; i < values.size(); ++i)
                    Targets[i].store(values[i], std::memory_order_relaxed);
            }

            void Prepare(uint32_t frames)
            {
                Values next;
                for (size_t i = 0; i < next.size(); ++i)
                    next[i] = Targets[i].load(std::memory_order_relaxed);
                if (next == Destination)
                    return;
                Remaining   = frames;
                Destination = next;
                for (size_t i = 0; i < next.size(); ++i)
                    Step[i] = (next[i] - Current[i]) / frames;
            }

            void Advance(uint32_t frames = 1)
            {
                if (Remaining == 0)
                    return;
                if (frames >= Remaining)
                {
                    Remaining = 0;
                    Current   = Destination;
                    return;
                }
                Remaining -= frames;
                for (size_t i = 0; i < Current.size(); ++i)
                    Current[i] += Step[i] * frames;
            }
        };

        class DelayLine
        {
        public:
            void Init(uint32_t maxFrames, uint32_t channels, uint32_t rate, float initialMs)
            {
                m_Channels = channels;
                m_Capacity = maxFrames + 1;
                m_Rate     = rate;
                m_Data.resize(size_t(m_Capacity) * channels);
                m_From = m_To = Frames(initialMs);
                m_FadeFrames  = std::max(1u, rate / 200);
            }

            void Prepare(float ms)
            {
                if (m_Remaining == 0 && Frames(ms) != m_To)
                {
                    m_From      = m_To;
                    m_To        = Frames(ms);
                    m_Remaining = m_FadeFrames;
                }
            }

            float Read(uint32_t channel, float input) const
            {
                const auto tap = [&](uint32_t delay) {
                    const auto frame = m_Cursor >= delay ? m_Cursor - delay : m_Cursor + (m_Capacity - delay);
                    return delay ? m_Data[size_t(frame) * m_Channels + channel] : input;
                };
                const float next = tap(m_To);
                if (m_Remaining == 0)
                    return next;
                return Mix(tap(m_From), next, float(m_FadeFrames - m_Remaining) / m_FadeFrames);
            }

            void Write(uint32_t channel, float value) { m_Data[size_t(m_Cursor) * m_Channels + channel] = value; }

            void Advance()
            {
                if (++m_Cursor == m_Capacity)
                    m_Cursor = 0;
                if (m_Remaining)
                    --m_Remaining;
            }

        private:
            uint32_t Frames(float ms) const
            {
                return std::min(m_Capacity - 1, static_cast<uint32_t>(std::lround(double(ms) * m_Rate / 1000)));
            }

            std::vector<float> m_Data;
            uint32_t           m_Channels = 0, m_Capacity = 0, m_Rate = 0, m_Cursor = 0;
            uint32_t           m_From = 0, m_To = 0, m_FadeFrames = 0, m_Remaining = 0;
        };

        struct ReverbLine
        {
            std::vector<float> Data;
            size_t             Index   = 0;
            float              LowPass = 0;

            float Read() const { return Data[Index]; }

            void Write(float sample)
            {
                Data[Index] = FlushQuiet(sample);
                if (++Index == Data.size())
                    Index = 0;
            }
        };

        struct ReverbTank
        {
            std::array<ReverbLine, 8> Combs;
            std::array<ReverbLine, 4> AllPasses;

            void Init(uint32_t channel, uint32_t rate)
            {
                constexpr uint32_t combLengths[]    = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
                constexpr uint32_t allPassLengths[] = {556, 441, 341, 225};
                const auto         resize           = [=](auto& lines, const auto& lengths) {
                    for (size_t i = 0; i < lines.size(); ++i)
                        lines[i].Data.resize(std::max(1l, std::lround((lengths[i] + 23.0 * channel) * rate / 44100)));
                };
                resize(Combs, combLengths);
                resize(AllPasses, allPassLengths);
            }

            float Process(float input, float feedback, float damping)
            {
                float wet = 0;
                for (auto& comb : Combs)
                {
                    const float delayed = comb.Read();
                    comb.LowPass        = FlushQuiet(Mix(delayed, comb.LowPass, damping));
                    comb.Write(input * .125f + comb.LowPass * feedback);
                    wet += delayed;
                }
                for (auto& allPass : AllPasses)
                {
                    const float delayed = allPass.Read();
                    allPass.Write(wet + delayed * .5f);
                    wet = delayed - wet;
                }
                return wet;
            }
        };

        class LookAheadLimiter
        {
        public:
            void Init(uint32_t rate, uint32_t channels)
            {
                m_Latency = std::max(1u, rate / 1000);
                m_Audio.resize(size_t(m_Latency) * channels);
                m_Minima.resize(m_Latency + 2);
                m_Average.resize(m_Latency, 1.f);
                m_Sum = m_Latency;
            }

            void Process(const float* input, float* output, uint32_t channels, float ceiling, float release)
            {
                const float peak     = Peak(input, channels);
                const float required = peak > ceiling ? ceiling / peak : 1.f;

                // A monotonic queue gives the minimum safe gain over the delay window.
                if (m_Head != m_Tail && m_Minima[m_Head].Frame + m_Latency < m_Frame)
                    m_Head = Next(m_Head);
                while (m_Head != m_Tail)
                {
                    const auto previous = m_Tail == 0 ? m_Minima.size() - 1 : m_Tail - 1;
                    if (m_Minima[previous].Gain < required)
                        break;
                    m_Tail = previous;
                }
                m_Minima[m_Tail] = {m_Frame++, required};
                m_Tail           = Next(m_Tail);

                // Every gain in this average constrains the delayed sample. Averaging gives a
                // look-ahead attack ramp while preserving the sample-peak ceiling.
                const float minimum = m_Minima[m_Head].Gain;
                m_Sum += double(minimum) - m_Average[m_Cursor];
                m_Average[m_Cursor] = minimum;
                const float target  = static_cast<float>(m_Sum / m_Latency);
                m_Gain              = std::min(target, Mix(target, m_Gain, release));

                for (uint32_t c = 0; c < channels; ++c)
                {
                    auto& delayed = m_Audio[size_t(m_Cursor) * channels + c];
                    output[c]     = std::clamp(delayed * m_Gain, -ceiling, ceiling);
                    delayed       = input[c];
                }
                if (++m_Cursor == m_Latency)
                    m_Cursor = 0;
            }

            float ReductionDb() const { return -ToDb(m_Gain); }

        private:
            struct Minimum
            {
                uint64_t Frame;
                float    Gain;
            };
            size_t Next(size_t index) const { return index + 1 == m_Minima.size() ? 0 : index + 1; }

            std::vector<float>   m_Audio, m_Average;
            std::vector<Minimum> m_Minima;
            uint64_t             m_Frame = 0;
            size_t               m_Head = 0, m_Tail = 0;
            uint32_t             m_Latency = 0, m_Cursor = 0;
            double               m_Sum  = 0;
            float                m_Gain = 1;
        };

        template<class T, auto Uninit>
        struct Filter
        {
            T    Node {};
            bool Ready = false;
            ~Filter()
            {
                if (Ready)
                    Uninit(&Node, nullptr);
            }

            void Process(const float* input, float* output, ma_uint32 frames)
            {
                ma_uint32 inputFrames = frames;
                Node.baseNode.vtable->onProcess(&Node, &input, &inputFrames, &output, &frames);
            }
        };

        // The enclosing graph slot owns dry/wet mixing. Delegate its wet path to the
        // stock miniaudio node callback, with both state and buffers already allocated.
        using LowPass   = Filter<ma_lpf_node, ma_lpf_node_uninit>;
        using HighPass  = Filter<ma_hpf_node, ma_hpf_node_uninit>;
        using PeakingEQ = Filter<ma_peak_node, ma_peak_node_uninit>;
    } // namespace

    void AudioSidechainPeak::Publish(uint64_t time, float peak)
    {
        if (Time == time)
            Current = std::max(Current, peak);
        else
        {
            Time     = time;
            Previous = Current;
            Current  = peak;
        }
    }

    float AudioSidechainPeak::ReadPrevious(uint64_t time) const
    {
        // The graph clock is constant during an endpoint pull. Use its preceding pull whether
        // the source fader has already run in this pull or is still waiting to be visited.
        return Time == time ? Previous : Current;
    }

    bool AudioEffectMix::Active(bool enabled) const { return Position != 0 || enabled; }

    float AudioEffectMix::Weight() const { return float(Position) / RampFrames; }

    float AudioEffectMix::Next(bool enabled)
    {
        if (enabled && Position < RampFrames)
            ++Position;
        if (!enabled && Position > 0)
            --Position;
        return Weight();
    }

    void AudioEffectMix::Blend(const float* dry, float* wet, uint32_t frames, uint32_t channels, bool enabled)
    {
        if (Position == (enabled ? RampFrames : 0))
        {
            if (!enabled)
                Copy(dry, wet, size_t(frames) * channels);
            return;
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float amount = Next(enabled);
            for (uint32_t c = 0; c < channels; ++c, ++dry, ++wet)
                *wet = Mix(*dry, *wet, amount);
        }
    }

    struct AudioBusEffectNode::State
    {
        ma_node_graph*                         Graph    = nullptr;
        uint32_t                               Channels = 0, Rate = 0, Ramp = 0, Order = 0;
        AudioEffectType                        Type = AudioEffectType::LowPass;
        AudioEffectMix                         EnableMix;
        const AudioEffectMix*                  ChainMix = nullptr;
        std::atomic<const AudioSidechainPeak*> Detector {nullptr};
        std::shared_ptr<AudioDSPRegistration>  Callback;

        ParameterRamp                                              Params;
        Values                                                     FilterValues {};
        std::variant<std::monostate, LowPass, HighPass, PeakingEQ> FilterNode;
        DelayLine                                                  Delay;
        std::vector<ReverbTank>                                    Tanks;
        std::vector<std::pair<int, int>>                           ReverbPairs;
        int                                                        Lfe = -1;
        std::vector<float>                                         WetFrame;
        LookAheadLimiter                                           Limiter;
        float                                                      Reduction = 0;
        std::atomic<float>                                         PublishedReduction {0};

        double Frequency(float hz) const { return std::min(double(hz), Rate * .495); }

        ma_result InitFilter()
        {
            const auto& p      = Params.Current;
            ma_result   result = MA_SUCCESS;
            switch (Type)
            {
                case AudioEffectType::LowPass: {
                    auto&      filter = FilterNode.emplace<LowPass>();
                    const auto config = ma_lpf_node_config_init(Channels, Rate, Frequency(p[0]), Order);
                    result            = ma_lpf_node_init(Graph, &config, nullptr, &filter.Node);
                    filter.Ready      = result == MA_SUCCESS;
                    break;
                }
                case AudioEffectType::HighPass: {
                    auto&      filter = FilterNode.emplace<HighPass>();
                    const auto config = ma_hpf_node_config_init(Channels, Rate, Frequency(p[0]), Order);
                    result            = ma_hpf_node_init(Graph, &config, nullptr, &filter.Node);
                    filter.Ready      = result == MA_SUCCESS;
                    break;
                }
                case AudioEffectType::PeakingEQ: {
                    auto&      filter = FilterNode.emplace<PeakingEQ>();
                    const auto config = ma_peak_node_config_init(Channels, Rate, p[1], p[2], Frequency(p[0]));
                    result            = ma_peak_node_init(Graph, &config, nullptr, &filter.Node);
                    filter.Ready      = result == MA_SUCCESS;
                    break;
                }
            }
            return result;
        }

        void ProcessFilter(const float* input, float* output, uint32_t frames)
        {
            while (frames)
            {
                const auto& p     = Params.Current;
                const auto  count = Params.Remaining ? std::min(16u, frames) : frames;
                std::visit(
                    [&](auto& filter) {
                        using F = std::decay_t<decltype(filter)>;
                        if constexpr (!std::is_same_v<F, std::monostate>)
                        {
                            // Reinitializing these miniaudio nodes preserves the delay registers and the allocation.
                            if (FilterValues != p)
                            {
                                if constexpr (std::is_same_v<F, LowPass>)
                                {
                                    const auto config =
                                        ma_lpf_config_init(ma_format_f32, Channels, Rate, Frequency(p[0]), Order);
                                    ma_lpf_node_reinit(&config, &filter.Node);
                                }
                                else if constexpr (std::is_same_v<F, HighPass>)
                                {
                                    const auto config =
                                        ma_hpf_config_init(ma_format_f32, Channels, Rate, Frequency(p[0]), Order);
                                    ma_hpf_node_reinit(&config, &filter.Node);
                                }
                                else
                                {
                                    const auto config = ma_peak2_config_init(
                                        ma_format_f32, Channels, Rate, p[1], p[2], Frequency(p[0]));
                                    ma_peak_node_reinit(&config, &filter.Node);
                                }
                            }
                            filter.Process(input, output, count);
                        }
                    },
                    FilterNode);
                FilterValues = p;
                Params.Advance(count);
                frames -= count;
                input += size_t(count) * Channels;
                output += size_t(count) * Channels;
            }
        }

        void ProcessDelay(const float* input, float* output, uint32_t frames)
        {
            // Time changes crossfade between two taps. Feedback and wet mix ramp sample by sample.
            Delay.Prepare(Params.Destination[0]);
            for (uint32_t i = 0; i < frames; ++i)
            {
                const auto& p = Params.Current;
                for (uint32_t c = 0; c < Channels; ++c, ++input, ++output)
                {
                    const float delayed = Delay.Read(c, *input);
                    Delay.Write(c, FlushQuiet(*input + delayed * p[1]));
                    *output = Mix(*input, delayed, p[2]);
                }
                Delay.Advance();
                Params.Advance();
            }
        }

        void ProcessReverb(const float* input, float* output, uint32_t frames)
        {
            Delay.Prepare(Params.Destination[4]);
            for (uint32_t i = 0; i < frames; ++i)
            {
                const auto& p        = Params.Current;
                const float feedback = .7f + .28f * p[0];
                for (uint32_t c = 0; c < Channels; ++c)
                {
                    if (int(c) == Lfe)
                        continue;
                    const float excitation = Delay.Read(c, input[c]);
                    Delay.Write(c, input[c]);
                    WetFrame[c] = Tanks[c].Process(excitation, feedback, p[1] * .9f);
                }
                const float cross = (1.f - p[2]) * .5f;
                for (const auto& [leftIndex, rightIndex] : ReverbPairs)
                {
                    const float left = WetFrame[leftIndex], right = WetFrame[rightIndex];
                    WetFrame[leftIndex]  = Mix(left, right, cross);
                    WetFrame[rightIndex] = Mix(right, left, cross);
                }
                for (uint32_t c = 0; c < Channels; ++c)
                    output[c] = int(c) == Lfe ? input[c] : Mix(input[c], WetFrame[c], p[3]);
                Delay.Advance();
                Params.Advance();
                input += Channels;
                output += Channels;
            }
        }

        void ProcessDynamics(const float* input, float* output, uint32_t frames)
        {
            const auto*  detector     = Detector.load(std::memory_order_relaxed);
            const float  sidechain    = detector ? detector->ReadPrevious(ma_node_graph_get_time(Graph)) : 0.f;
            const bool   ducking      = Type == AudioEffectType::Ducking;
            const bool   limiting     = Type == AudioEffectType::Limiter;
            const size_t releaseIndex = limiting ? 1 : 3;
            float        attack       = limiting ? 0.f : Decay(Params.Current[2], Rate);
            float        release      = Decay(Params.Current[releaseIndex], Rate);
            float        threshold    = limiting || ducking ? ToGain(Params.Current[0]) : 0.f;
            for (uint32_t i = 0; i < frames; ++i)
            {
                const auto& p = Params.Current;
                if (limiting)
                {
                    Limiter.Process(input, output, Channels, threshold, release);
                }
                else
                {
                    float target = 0, makeup = 0;
                    if (ducking)
                        target = sidechain > threshold ? p[1] : 0.f;
                    else
                    {
                        const float over     = ToDb(Peak(input, Channels)) - p[0];
                        const float halfKnee = p[5] * .5f;
                        const float knee     = over + halfKnee;
                        const float compressed =
                            std::abs(over) < halfKnee ? knee * knee / (2.f * p[5]) : std::max(0.f, over);
                        target = compressed * (1.f - 1.f / p[1]);
                        makeup = p[4];
                    }
                    Reduction        = FlushQuiet(Mix(target, Reduction, target > Reduction ? attack : release));
                    const float gain = ToGain(makeup - Reduction);
                    for (uint32_t c = 0; c < Channels; ++c)
                        output[c] = input[c] * gain;
                }
                if (Params.Remaining)
                {
                    Params.Advance();
                    attack    = limiting ? 0.f : Decay(p[2], Rate);
                    release   = Decay(p[releaseIndex], Rate);
                    threshold = limiting || ducking ? ToGain(p[0]) : 0.f;
                }
                input += Channels;
                output += Channels;
            }
            if (limiting)
                Reduction = Limiter.ReductionDb();
        }

        void ProcessCustom(const float* input, float* output, uint32_t frames)
        {
            Copy(input, output, size_t(frames) * Channels);
            auto* callback = Callback.get();
            if (!callback || !callback->Enabled.load(std::memory_order_relaxed))
                return;

            // Sequential consistency closes the race between the entrant and UnregisterCustomDSP.
            callback->InFlight.fetch_add(1);
            if (callback->Enabled.load())
                callback->Callback(output, frames, Channels, Rate, callback->User);
            callback->InFlight.fetch_sub(1);
        }
    };

    AudioBusEffectNode::AudioBusEffectNode() : ma_node_base {}, m_State(std::make_unique<State>()) {}

    AudioBusEffectNode::~AudioBusEffectNode()
    {
        if (m_Ready)
            ma_node_uninit(this, nullptr);
    }

    ma_result AudioBusEffectNode::Init(ma_node_graph*                        graph,
                                       const ma_channel*                     channelMap,
                                       uint32_t                              channels,
                                       uint32_t                              rate,
                                       const AudioBusEffect&                 effect,
                                       const AudioEffectMix&                 chainMix,
                                       const AudioSidechainPeak*             detector,
                                       std::shared_ptr<AudioDSPRegistration> callback)
    {
        auto& s                = *m_State;
        s.Graph                = graph;
        s.Channels             = channels;
        s.Rate                 = rate;
        s.Ramp                 = std::max(1u, rate / 200);
        s.ChainMix             = &chainMix;
        s.EnableMix.RampFrames = s.Ramp;
        s.EnableMix.Position   = effect.Enabled ? s.Ramp : 0;
        s.Type                 = GetEffectType(effect.Parameters);
        s.Callback             = std::move(callback);
        Update(effect, detector);
        s.FilterValues = s.Params.Current = s.Params.Destination = NumericParameters(effect.Parameters);
        if (const auto* lowPass = std::get_if<AudioLowPass>(&effect.Parameters))
            s.Order = lowPass->Order;
        if (const auto* highPass = std::get_if<AudioHighPass>(&effect.Parameters))
            s.Order = highPass->Order;
        if (s.Type <= AudioEffectType::PeakingEQ)
        {
            const auto result = s.InitFilter();
            if (result != MA_SUCCESS)
                return result;
        }
        if (const auto* delay = std::get_if<AudioDelay>(&effect.Parameters))
            s.Delay.Init(rate * 2, channels, rate, delay->TimeMs);
        if (const auto* reverb = std::get_if<AudioReverb>(&effect.Parameters))
        {
            s.Delay.Init(rate / 4, channels, rate, reverb->PreDelayMs);
            s.Tanks.resize(channels);
            s.WetFrame.resize(channels);
            s.Lfe = AudioChannelIndex(channelMap, channels, MA_CHANNEL_LFE);
            constexpr std::pair<ma_channel, ma_channel> pairs[] = {
                {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT},
                {MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT},
                {MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT},
                {MA_CHANNEL_FRONT_LEFT_CENTER, MA_CHANNEL_FRONT_RIGHT_CENTER},
                {MA_CHANNEL_TOP_FRONT_LEFT, MA_CHANNEL_TOP_FRONT_RIGHT},
                {MA_CHANNEL_TOP_BACK_LEFT, MA_CHANNEL_TOP_BACK_RIGHT}};
            for (const auto& [left, right] : pairs)
            {
                const auto leftIndex  = AudioChannelIndex(channelMap, channels, left);
                const auto rightIndex = AudioChannelIndex(channelMap, channels, right);
                if (leftIndex >= 0 && rightIndex >= 0)
                    s.ReverbPairs.emplace_back(leftIndex, rightIndex);
            }
            for (uint32_t c = 0; c < channels; ++c)
                if (int(c) != s.Lfe)
                    s.Tanks[c].Init(c, rate);
        }
        if (std::holds_alternative<AudioLimiter>(effect.Parameters))
            s.Limiter.Init(rate, channels);

        static const ma_node_vtable vtable = {Process, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};
        auto                        config = ma_node_config_init();
        config.vtable                      = &vtable;
        config.pInputChannels = config.pOutputChannels = &channels;
        const auto result                              = ma_node_init(graph, &config, nullptr, this);
        m_Ready                                        = result == MA_SUCCESS;
        return result;
    }

    bool AudioBusEffectNode::CanUpdate(const AudioBusEffectParameters& params,
                                       const AudioDSPRegistration*     callback) const
    {
        if (GetEffectType(params) != m_State->Type || callback != m_State->Callback.get())
            return false;
        if (const auto* lowPass = std::get_if<AudioLowPass>(&params))
            return lowPass->Order == m_State->Order;
        if (const auto* highPass = std::get_if<AudioHighPass>(&params))
            return highPass->Order == m_State->Order;
        return true;
    }

    void AudioBusEffectNode::Update(const AudioBusEffect& effect, const AudioSidechainPeak* detector)
    {
        m_State->Params.Set(NumericParameters(effect.Parameters));
        m_State->EnableMix.Enabled.store(effect.Enabled, std::memory_order_relaxed);
        m_State->Detector.store(detector, std::memory_order_relaxed);
    }

    float AudioBusEffectNode::GetReductionDb() const
    {
        return m_State->PublishedReduction.load(std::memory_order_relaxed);
    }

    void AudioBusEffectNode::Process(ma_node*      node,
                                     const float** inputs,
                                     ma_uint32*    inputFrames,
                                     float**       outputs,
                                     ma_uint32*    outputFrames)
    {
        auto&                         effect = *static_cast<AudioBusEffectNode*>(static_cast<ma_node_base*>(node));
        const AudioProfileEffectScope profile(effect.ProfileKey);
        auto&                         s      = *effect.m_State;
        const auto  frames = std::min(*inputFrames, *outputFrames);
        const auto* dry    = inputs[0];
        auto*       wet    = outputs[0];
        *inputFrames = *outputFrames = frames;
        const bool enabled           = s.EnableMix.Enabled.load(std::memory_order_relaxed);
        if (!s.EnableMix.Active(enabled) || !s.ChainMix->Active(s.ChainMix->BlockEnabled))
        {
            Copy(dry, wet, size_t(frames) * s.Channels);
            s.PublishedReduction.store(0, std::memory_order_relaxed);
            return;
        }

        s.Params.Prepare(s.Ramp);
        switch (s.Type)
        {
            case AudioEffectType::LowPass:
            case AudioEffectType::HighPass:
            case AudioEffectType::PeakingEQ:
                s.ProcessFilter(dry, wet, frames);
                break;
            case AudioEffectType::Delay:
                s.ProcessDelay(dry, wet, frames);
                break;
            case AudioEffectType::Reverb:
                s.ProcessReverb(dry, wet, frames);
                break;
            case AudioEffectType::Compressor:
            case AudioEffectType::Limiter:
            case AudioEffectType::Ducking:
                s.ProcessDynamics(dry, wet, frames);
                break;
            case AudioEffectType::Custom:
                s.ProcessCustom(dry, wet, frames);
                break;
        }
        s.EnableMix.Blend(dry, wet, frames, s.Channels, enabled);
        s.PublishedReduction.store(s.Reduction * s.EnableMix.Weight(), std::memory_order_relaxed);
    }
} // namespace Kans
