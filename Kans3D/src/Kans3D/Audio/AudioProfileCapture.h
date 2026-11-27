#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>
#include "AudioBusLayout.h"
#include "AudioProfile.h"

namespace Kans
{
    using AudioProfileClock                       = std::chrono::steady_clock;
    inline constexpr size_t AudioProfileNodeLimit = 256;

    inline uint64_t ProfileNanoseconds(AudioProfileClock::duration elapsed)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    }

    struct AudioProfileNodeKey
    {
        uint64_t Revision = 0;
        size_t   Index    = AudioProfileNodeLimit;
    };

    struct AudioProfileTimingFrame
    {
        AudioProfileCallback                        Callback;
        uint64_t                                    Tick = 0, Revision = 0;
        std::array<uint64_t, AudioProfileNodeLimit> Nanoseconds {};
    };

    struct AudioProfileMix
    {
        AudioProfileTimingFrame* Frame;
        uint64_t                 BusNanoseconds = 0;

        void Record(AudioProfileNodeKey key, uint64_t elapsed)
        {
            if (key.Index >= AudioProfileNodeLimit || key.Revision < Frame->Revision)
                return;
            if (key.Revision != Frame->Revision)
            {
                Frame->Revision = key.Revision;
                Frame->Nanoseconds.fill(0);
            }
            Frame->Nanoseconds[key.Index] += elapsed;
        }
    };

    // Set only during a sampled mix. Nodes consult mixer-local state without reading the switch.
    inline thread_local AudioProfileMix* AudioProfileActiveMix = nullptr;

    class AudioProfileEffectScope
    {
    public:
        explicit AudioProfileEffectScope(AudioProfileNodeKey key) : m_Key(key), m_Mix(AudioProfileActiveMix)
        {
            if (m_Mix && m_Mix->Frame && key.Index < AudioProfileNodeLimit)
                m_Start = AudioProfileClock::now();
            else
                m_Mix = nullptr;
        }
        ~AudioProfileEffectScope()
        {
            if (m_Mix)
                m_Mix->Record(m_Key, ProfileNanoseconds(AudioProfileClock::now() - m_Start));
        }

    private:
        AudioProfileNodeKey           m_Key;
        AudioProfileMix*              m_Mix;
        AudioProfileClock::time_point m_Start;
    };

    class AudioProfileCapture
    {
    public:
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(std::atomic<uint64_t>::is_always_lock_free);
        static constexpr size_t EventCapacity = 256;

        std::atomic<bool>     Enabled {false};
        std::atomic<uint64_t> Callbacks {0}, Overruns {0}, DroppedTimings {0};

        AudioProfileTimingFrame* Begin()
        {
            if (m_Write - m_Read.load(std::memory_order_acquire) == m_Queue.size())
                return nullptr;
            auto& frame = m_Queue[m_Write % m_Queue.size()];
            frame       = {};
            return &frame;
        }

        void End(const AudioProfileTimingFrame* frame, bool overrun)
        {
            Callbacks.store(++m_CallbackCount, std::memory_order_relaxed);
            if (overrun)
                Overruns.store(++m_OverrunCount, std::memory_order_relaxed);
            if (frame)
                m_Published.store(++m_Write, std::memory_order_release);
            else
                DroppedTimings.store(++m_DroppedCount, std::memory_order_relaxed);
        }

        void Reset()
        {
            m_Read.store(0, std::memory_order_relaxed);
            m_Published.store(0, std::memory_order_relaxed);
            Callbacks.store(0, std::memory_order_relaxed);
            Overruns.store(0, std::memory_order_relaxed);
            DroppedTimings.store(0, std::memory_order_relaxed);
            m_Write = m_CallbackCount = m_OverrunCount = m_DroppedCount = m_NextEvent = 0;
            m_Current                                                                 = {};
            m_Load                                                                    = {};
            m_Nodes.clear();
            m_Unprofiled = 0;
            m_Revision   = 0;
        }

        void SetNodes(uint64_t revision, const std::vector<AudioBusDefinition>& buses)
        {
            m_Revision = revision;
            m_Nodes.clear();
            m_Unprofiled   = 0;
            const auto add = [&](size_t bus, std::optional<size_t> slot) {
                if (m_Nodes.size() == AudioProfileNodeLimit)
                    ++m_Unprofiled;
                else
                    m_Nodes.push_back({bus, slot});
            };
            for (size_t bus = 0; bus < buses.size(); ++bus)
                add(bus, {});
            for (size_t bus = 0; bus < buses.size(); ++bus)
                for (size_t slot = 0; slot < buses[bus].Effects.size(); ++slot)
                    add(bus, slot);
        }

        const AudioProfileCallback& LatestCallback() const { return m_Current; }

        void Collect()
        {
            const auto end  = m_Published.load(std::memory_order_acquire);
            auto       read = m_Read.load(std::memory_order_relaxed);
            for (; read < end; ++read)
            {
                const auto& frame = m_Queue[read % m_Queue.size()];
                m_Current         = frame.Callback;
                m_Load.Add(frame.Tick, frame.Callback.LoadPercent());
                if (frame.Revision == 0 || m_Revision == frame.Revision)
                    for (size_t i = 0; i < m_Nodes.size(); ++i)
                    {
                        auto& node   = m_Nodes[i];
                        node.Current = frame.Nanoseconds[i];
                        node.History.Add(frame.Tick, static_cast<double>(node.Current));
                    }
            }
            m_Read.store(read, std::memory_order_release);
        }

        void ReadStats(AudioProfileStats& stats) const
        {
            const auto now          = Tick(AudioProfileClock::now());
            stats.Current           = m_Current;
            stats.LastSecond        = m_Load.Read(now, 20);
            stats.LastTenSeconds    = m_Load.Read(now, 200);
            stats.Callbacks         = Callbacks.load(std::memory_order_relaxed);
            stats.Overruns          = Overruns.load(std::memory_order_relaxed);
            stats.DroppedTimings    = DroppedTimings.load(std::memory_order_relaxed);
            stats.OverwrittenEvents = m_NextEvent > EventCapacity ? m_NextEvent - EventCapacity : 0;
            stats.UnprofiledNodes   = m_Unprofiled;
        }

        void ReadNodes(std::vector<AudioProfileNode>& out) const
        {
            const auto now = Tick(AudioProfileClock::now());
            out.resize(m_Nodes.size());
            for (size_t i = 0; i < m_Nodes.size(); ++i)
            {
                const auto& node = m_Nodes[i];
                out[i] = {node.Bus, node.Slot, node.Current, node.History.Read(now, 20), node.History.Read(now, 200)};
            }
        }

        void Push(const AudioProfileEvent& event) { m_Events[m_NextEvent++ % EventCapacity] = event; }

        size_t ReadEvents(AudioProfileEventCursor& cursor, AudioProfileEvent* out, size_t capacity) const
        {
            const auto first = m_NextEvent > EventCapacity ? m_NextEvent - EventCapacity : 0;
            if (cursor.Next < first)
            {
                cursor.Dropped += first - cursor.Next;
                cursor.Next = first;
            }
            if (cursor.Next >= m_NextEvent)
                return 0;
            const auto count = std::min<uint64_t>(capacity, m_NextEvent - cursor.Next);
            for (size_t i = 0; i < count; ++i)
                out[i] = m_Events[cursor.Next++ % EventCapacity];
            return static_cast<size_t>(count);
        }

        static uint64_t Tick(AudioProfileClock::time_point time)
        {
            return ProfileNanoseconds(time.time_since_epoch()) / 50000000;
        }

    private:
        // Fifty millisecond buckets bound memory independently of device callback frequency.
        class Rolling
        {
        public:
            void Add(uint64_t tick, double value)
            {
                auto& bucket = m_Buckets[tick % m_Buckets.size()];
                if (!bucket.Samples || bucket.Tick != tick)
                    bucket = {tick, 1, value, value, value};
                else
                {
                    ++bucket.Samples;
                    bucket.Minimum = std::min(bucket.Minimum, value);
                    bucket.Peak    = std::max(bucket.Peak, value);
                    bucket.Sum += value;
                }
            }

            AudioProfileWindow Read(uint64_t now, uint64_t ticks) const
            {
                AudioProfileWindow result;
                double             sum = 0;
                for (const auto& bucket : m_Buckets)
                    if (bucket.Samples && now - bucket.Tick < ticks)
                    {
                        result.Minimum = result.Samples ? std::min(result.Minimum, bucket.Minimum) : bucket.Minimum;
                        result.Peak    = std::max(result.Peak, bucket.Peak);
                        result.Samples += bucket.Samples;
                        sum += bucket.Sum;
                    }
                if (result.Samples)
                    result.Average = sum / result.Samples;
                return result;
            }

        private:
            struct Bucket
            {
                uint64_t Tick = 0, Samples = 0;
                double   Minimum = 0, Peak = 0, Sum = 0;
            };
            std::array<Bucket, 200> m_Buckets {};
        };

        struct Node
        {
            size_t                Bus;
            std::optional<size_t> Slot;
            uint64_t              Current = 0;
            Rolling               History;
        };

        std::array<AudioProfileTimingFrame, 256> m_Queue;
        alignas(64) std::atomic<uint64_t> m_Read {0};
        alignas(64) std::atomic<uint64_t> m_Published {0};
        uint64_t             m_Write = 0, m_CallbackCount = 0, m_OverrunCount = 0, m_DroppedCount = 0;
        uint64_t             m_Revision   = 0;
        size_t               m_Unprofiled = 0;
        std::vector<Node>    m_Nodes;
        AudioProfileCallback m_Current;
        Rolling              m_Load;
        std::array<AudioProfileEvent, EventCapacity> m_Events;
        uint64_t                                     m_NextEvent = 0;
    };
} // namespace Kans
