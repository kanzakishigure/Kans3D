#include "kspch.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Audio/AudioMusicPlayer.h"
#include "Kans3D/Core/Log/Log.h"
#include "AudioBusGraph.h"

#include <miniaudio.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>

namespace Kans
{
    namespace
    {
        constexpr auto NoFrame = std::numeric_limits<uint64_t>::max();
        using MixClock         = std::chrono::steady_clock;
        static_assert(std::atomic<uint64_t>::is_always_lock_free);

        uint64_t AddFrames(uint64_t start, uint64_t count) { return start + std::min(NoFrame - start, count); }

        double WrapPosition(double frame, uint64_t begin, uint64_t end, bool loop)
        {
            if (frame < end)
                return frame;
            return loop ? begin + std::fmod(frame - begin, static_cast<double>(end - begin)) : end;
        }

        struct VoiceEnvelope
        {
            uint64_t       Start = 0, Length = 0;
            float          From = 1, To = 1;
            AudioFadeCurve Curve = AudioFadeCurve::Linear;
            uint64_t       Elapsed = 0;

            float Gain(uint64_t frame) const
            {
                if (frame < Start)
                    return From;
                const auto progress = AddFrames(frame - Start, Elapsed);
                if (progress >= Length)
                    return To;
                const auto t = static_cast<float>(static_cast<double>(progress) / Length);
                if (Curve == AudioFadeCurve::EqualPower)
                {
                    constexpr float halfPi = 1.57079632679489661923f;
                    return To >= From ? From + (To - From) * std::sin(t * halfPi) :
                                        To + (From - To) * std::cos(t * halfPi);
                }
                const float weight = Curve == AudioFadeCurve::SCurve ? t * t * (3 - 2 * t) : t;
                return From + (To - From) * weight;
            }
        };

        void Warn(const std::string& message)
        {
            if (Log::GetCoreLogger())
                CORE_WARN("Audio: {}", message);
            else
                std::fprintf(stderr, "Audio: %s\n", message.c_str());
        }

        std::string Error(const char* operation, ma_result result)
        {
            return std::string(operation) + ": " + ma_result_description(result);
        }

        // Streaming bypasses registered buffers. This read-only VFS exposes the same owned bytes to its decoder.
        struct MemoryVFS
        {
            ma_vfs_callbacks Callbacks{};
            std::mutex Mutex;
            std::unordered_map<std::string, std::shared_ptr<const std::vector<std::byte>>> Files;
            struct File
            {
                std::shared_ptr<const std::vector<std::byte>> Bytes;
                size_t Cursor = 0;
            };

            MemoryVFS()
            {
                Callbacks.onOpen = [](ma_vfs* vfs, const char* path, ma_uint32 mode, ma_vfs_file* file) -> ma_result
                {
                    if (mode != MA_OPEN_MODE_READ)
                        return MA_ACCESS_DENIED;
                    auto& self = *static_cast<MemoryVFS*>(vfs);
                    std::lock_guard<std::mutex> lock(self.Mutex);
                    auto it = self.Files.find(path);
                    if (it == self.Files.end())
                        return MA_DOES_NOT_EXIST;
                    auto opened = new (std::nothrow) File{it->second};
                    if (!opened)
                        return MA_OUT_OF_MEMORY;
                    *file = opened;
                    return MA_SUCCESS;
                };
                Callbacks.onClose = [](ma_vfs*, ma_vfs_file file) -> ma_result
                {
                    delete static_cast<File*>(file);
                    return MA_SUCCESS;
                };
                Callbacks.onRead = [](ma_vfs*, ma_vfs_file file, void* out, size_t count, size_t* read) -> ma_result
                {
                    auto& f = *static_cast<File*>(file);
                    const size_t n = std::min(count, f.Bytes->size() - f.Cursor);
                    if (n)
                        std::memcpy(out, f.Bytes->data() + f.Cursor, n);
                    f.Cursor += n;
                    if (read)
                        *read = n;
                    return n > 0 || count == 0 ? MA_SUCCESS : MA_AT_END;
                };
                Callbacks.onSeek = [](ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) -> ma_result
                {
                    auto& f = *static_cast<File*>(file);
                    const auto base = origin == ma_seek_origin_start ? 0 :
                                      origin == ma_seek_origin_current ? f.Cursor : f.Bytes->size();
                    if (offset < -static_cast<ma_int64>(base) ||
                        offset > static_cast<ma_int64>(f.Bytes->size() - base))
                        return MA_BAD_SEEK;
                    f.Cursor = static_cast<size_t>(static_cast<ma_int64>(base) + offset);
                    return MA_SUCCESS;
                };
                Callbacks.onTell = [](ma_vfs*, ma_vfs_file file, ma_int64* cursor) -> ma_result
                {
                    *cursor = static_cast<ma_int64>(static_cast<File*>(file)->Cursor);
                    return MA_SUCCESS;
                };
                Callbacks.onInfo = [](ma_vfs*, ma_vfs_file file, ma_file_info* info) -> ma_result
                {
                    info->sizeInBytes = static_cast<File*>(file)->Bytes->size();
                    return MA_SUCCESS;
                };
            }
        };
    }

    struct AudioEngine::Impl
    {
        struct Sound
        {
            AudioSoundInfo Info{};
            std::shared_ptr<const std::vector<std::byte>> Encoded;
            std::vector<float> PCM;
            std::string EncodedName, DecodedName;
            bool DecodedRegistered = false;
        };
        // A decoder fills a preallocated PCM ring on the worker (synchronously in Offline).
        // The mixer only reads this ring and publishes seek requests. Music's fixed loop is
        // decoded on the producer side, so a seam never waits for a seek on the mixer.
        struct VoiceSource
        {
            ma_data_source_base Base{};
            ma_resource_manager_data_source Data{};
            ma_decoder                      Decoder {};
            bool                            Offline = false, Initialized = false, Stream = false, DecoderReady = false;
            AudioSoundInfo                  Info {};
            uint64_t                        LoopStart = 0, LoopEnd = 0;
            uint64_t                        DecoderFrame = 0;
            uint64_t                        Capacity     = 0;
            std::vector<float>              PCM;
            std::atomic<uint64_t>           ReadFrame {0}, WriteFrame {0}, Cursor {0};
            std::atomic<uint64_t>           RequestedSeek {0}, SeekRevision {0}, ReadyRevision {0};
            std::atomic<bool>               Eof {false};

            ma_result Init(ma_resource_manager* resources,
                           MemoryVFS&           vfs,
                           const char*          path,
                           bool                 stream,
                           bool                 offline,
                           AudioSoundInfo       info,
                           uint64_t             loopStart,
                           uint64_t             loopEnd)
            {
                Offline = offline;
                Stream    = stream;
                Info      = info;
                LoopStart = loopStart;
                LoopEnd   = loopEnd;
                ma_result result;
                if (Stream)
                {
                    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
                    result            = ma_decoder_init_vfs(&vfs, path, &config, &Decoder);
                    if (result != MA_SUCCESS)
                        return result;
                    DecoderReady = true;
                    Capacity     = std::max(2048u, Info.SampleRate / 4);
                    PCM.resize(Capacity * Info.Channels);
                    if (Offline)
                        Fill();
                    else
                        SeekRevision.store(1);
                }
                else
                {
                    result = ma_resource_manager_data_source_init(
                        resources, path, MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE, nullptr, &Data);
                    if (result != MA_SUCCESS)
                        return result;
                }
                static const ma_data_source_vtable callbacks = {
                    [](ma_data_source* source, void* out, ma_uint64 count, ma_uint64* read) -> ma_result {
                        auto& self = *static_cast<VoiceSource*>(source);
                        return self.Stream ? self.Read(out, count, *read) :
                                             ma_data_source_read_pcm_frames(&self.Data, out, count, read);
                    },
                    [](ma_data_source* source, ma_uint64 frame) -> ma_result {
                        auto& self = *static_cast<VoiceSource*>(source);
                        if (!self.Stream)
                            return ma_data_source_seek_to_pcm_frame(&self.Data, frame);
                        if (frame != self.Cursor.load(std::memory_order_relaxed))
                        {
                            self.Cursor.store(frame, std::memory_order_relaxed);
                            self.RequestedSeek.store(frame, std::memory_order_relaxed);
                            self.SeekRevision.fetch_add(1, std::memory_order_release);
                        }
                        return MA_SUCCESS;
                    },
                    [](ma_data_source* source,
                       ma_format*      format,
                       ma_uint32*      channels,
                       ma_uint32*      rate,
                       ma_channel*     map,
                       size_t          capacity) -> ma_result {
                        auto& self = *static_cast<VoiceSource*>(source);
                        if (!self.Stream)
                            return ma_data_source_get_data_format(&self.Data, format, channels, rate, map, capacity);
                        if (format)
                            *format = ma_format_f32;
                        if (channels)
                            *channels = self.Info.Channels;
                        if (rate)
                            *rate = self.Info.SampleRate;
                        ma_channel_map_init_standard(
                            ma_standard_channel_map_default, map, capacity, self.Info.Channels);
                        return MA_SUCCESS;
                    },
                    [](ma_data_source* source, ma_uint64* cursor) -> ma_result {
                        auto& self = *static_cast<VoiceSource*>(source);
                        if (!self.Stream)
                            return ma_data_source_get_cursor_in_pcm_frames(&self.Data, cursor);
                        *cursor = self.Cursor.load(std::memory_order_relaxed);
                        return MA_SUCCESS;
                    },
                    [](ma_data_source* source, ma_uint64* length) -> ma_result {
                        *length = static_cast<VoiceSource*>(source)->Info.LengthFrames;
                        return MA_SUCCESS;
                    },
                    nullptr,
                    0};
                auto config = ma_data_source_config_init();
                config.vtable = &callbacks;
                result = ma_data_source_init(&config, &Base);
                if (result != MA_SUCCESS && !Stream)
                    ma_resource_manager_data_source_uninit(&Data);
                Initialized = result == MA_SUCCESS;
                return result;
            }

            void Fill()
            {
                const auto revision = SeekRevision.load(std::memory_order_acquire);
                auto       write    = WriteFrame.load(std::memory_order_relaxed);
                const bool reset    = ReadyRevision.load(std::memory_order_relaxed) != revision;
                if (reset)
                {
                    ReadFrame.store(write, std::memory_order_relaxed);
                    const auto target = RequestedSeek.load(std::memory_order_relaxed);
                    Eof.store(ma_decoder_seek_to_pcm_frame(&Decoder, target) != MA_SUCCESS, std::memory_order_relaxed);
                    DecoderFrame = target;
                }
                if (!Eof.load(std::memory_order_relaxed))
                {
                    auto room = Capacity - (write - ReadFrame.load(std::memory_order_acquire));
                    while (room != 0)
                    {
                        if (LoopEnd != 0 && DecoderFrame == LoopEnd)
                        {
                            if (ma_decoder_seek_to_pcm_frame(&Decoder, LoopStart) != MA_SUCCESS)
                            {
                                Eof.store(true, std::memory_order_release);
                                break;
                            }
                            DecoderFrame = LoopStart;
                        }
                        const auto offset = write % Capacity;
                        auto       count  = std::min(room, Capacity - offset);
                        if (LoopEnd != 0)
                            count = std::min(count, LoopEnd - DecoderFrame);
                        ma_uint64  read = 0;
                        const auto result =
                            ma_decoder_read_pcm_frames(&Decoder, PCM.data() + offset * Info.Channels, count, &read);
                        DecoderFrame += read;
                        write += read;
                        WriteFrame.store(write, std::memory_order_release);
                        if (read != count || (result != MA_SUCCESS && result != MA_AT_END))
                        {
                            Eof.store(true, std::memory_order_release);
                            break;
                        }
                        room -= read;
                    }
                }
                if (reset)
                    ReadyRevision.store(revision, std::memory_order_release);
            }

            ma_result Read(void* out, uint64_t count, ma_uint64& read)
            {
                if (Offline)
                    Fill();
                read = 0;
                if (SeekRevision.load(std::memory_order_relaxed) != ReadyRevision.load(std::memory_order_acquire))
                    return MA_BUSY;
                const bool eof   = Eof.load(std::memory_order_acquire);
                const auto start = ReadFrame.load(std::memory_order_relaxed);
                read             = std::min(count, WriteFrame.load(std::memory_order_acquire) - start);
                if (read == 0)
                    return eof ? MA_AT_END : MA_BUSY;
                if (out)
                {
                    const auto offset  = start % Capacity;
                    const auto first   = std::min<uint64_t>(read, Capacity - offset);
                    auto*      samples = static_cast<float*>(out);
                    std::memcpy(samples, PCM.data() + offset * Info.Channels, first * Info.Channels * sizeof(float));
                    std::memcpy(
                        samples + first * Info.Channels, PCM.data(), (read - first) * Info.Channels * sizeof(float));
                }
                const auto next = Cursor.load(std::memory_order_relaxed) + read;
                Cursor.store(LoopEnd == 0 || next < LoopEnd ? next :
                                                              LoopStart + (next - LoopEnd) % (LoopEnd - LoopStart),
                             std::memory_order_relaxed);
                ReadFrame.store(start + read, std::memory_order_release);
                return MA_SUCCESS;
            }

            ~VoiceSource()
            {
                if (Initialized)
                {
                    ma_data_source_uninit(&Base);
                    if (!Stream)
                        ma_resource_manager_data_source_uninit(&Data);
                }
                if (DecoderReady)
                    ma_decoder_uninit(&Decoder);
            }
        };
        struct Voice
        {
            struct Transport
            {
                uint64_t Frame = 0, StreamDelayBase = 0, StreamDelayEnd = NoFrame;
                uint64_t Start = NoFrame, Stop = NoFrame;
                double   Source = 0, Rate = 1;
                uint64_t LoopBegin = 0, LoopEnd = 0;
                bool     Loop = false;

                double Position(double clock, uint64_t streamDelay = 0) const
                {
                    const auto delay   = std::min(streamDelay, StreamDelayEnd);
                    const auto waited  = delay - std::min(delay, StreamDelayBase);
                    const auto elapsed = std::max(0.0, std::min(clock, static_cast<double>(Stop)) - Start - waited);
                    return WrapPosition(Source + elapsed * Rate, LoopBegin, LoopEnd, Loop);
                }
            };

            struct Control
            {
                std::atomic<Voice*>   Owner {nullptr};
                std::atomic<uint64_t> Revision {0};
            };

            struct Fades
            {
                VoiceEnvelope                In;
                std::optional<VoiceEnvelope> Out;
                bool                         Multiply = false;

                float Gain(uint64_t frame) const
                {
                    const float in = In.Gain(frame);
                    if (!Out || frame < Out->Start)
                        return in;
                    return Multiply ? in * Out->Gain(frame) : Out->Gain(frame);
                }
            };

            // Only the sound's mixer callback accesses these fields. The inherited ma_sound stays
            // attached to the usual bus graph and retains miniaudio's volume, pan and spatial DSP.
            struct Node : ma_sound
            {
                Voice*                Owner    = nullptr;
                const ma_node_vtable* Original = nullptr;
                uint64_t              Block = NoFrame, Next = 0;
                Fades                 Envelope;

                static void
                Process(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount)
                {
                    auto&      self  = *static_cast<Node*>(static_cast<ma_sound*>(node));
                    auto&      voice = *self.Owner;
                    const auto block = ma_engine_get_time_in_pcm_frames(ma_sound_get_engine(&self));
                    if (block != self.Block)
                    {
                        self.Block = block;
                        self.Next  = std::max<uint64_t>(block, ma_node_get_state_time(node, ma_node_state_started));
                    }
                    if (voice.Timed.load(std::memory_order_relaxed))
                    {
                        const bool bypass =
                            !voice.Desc.Spatial &&
                            self.engineNode.sampleRate == ma_engine_get_sample_rate(ma_sound_get_engine(&self)) &&
                            ma_sound_get_pitch(&self) == 1;
                        if (self.engineNode.isPitchDisabled != bypass)
                        {
                            self.engineNode.isPitchDisabled = bypass;
                            self.ResetResampler();
                        }
                        if (const auto seek = voice.PendingSeek.exchange(NoFrame); seek != NoFrame)
                            self.Seek(seek);
                    }
                    const auto requested = *outCount;
                    self.Original->onProcess(node, in, inCount, out, outCount);
                    if (*outCount < requested && voice.Source->Stream && !voice.Source->Offline &&
                        !ma_sound_at_end(&self))
                        voice.StreamDelay.fetch_add(requested - *outCount, std::memory_order_relaxed);
                    const auto channels = ma_node_get_output_channels(node, 0);
                    if (self.Envelope.Out || self.Envelope.In.From != 1 || self.Envelope.In.To != 1)
                        for (uint32_t i = 0; i < *outCount; ++i)
                        {
                            const auto gain = self.Envelope.Gain(self.Next + i);
                            for (uint32_t ch = 0; ch < channels; ++ch)
                                out[0][i * channels + ch] *= gain;
                        }
                    self.Next += requested;
                }

                void ResetResampler()
                {
                    ma_resampler_reset(&engineNode.resampler);
                    // miniaudio's linear resampler starts with x0 = x1 = 0. For scheduled playback,
                    // preload the interpolation pair before emitting the first output frame.
                    static_cast<ma_linear_resampler*>(engineNode.resampler.pBackend)->inTimeInt = 2;
                }

                void Seek(uint64_t frame)
                {
                    ma_data_source_seek_to_pcm_frame(pDataSource, frame);
                    processingCacheFramesRemaining = 0;
                    ResetResampler();
                }
            };

            std::shared_ptr<VoiceSource> Source = std::make_shared<VoiceSource>();
            std::atomic<uint64_t>        StreamDelay {0};
            Node                     Sound {};
            AudioVoiceDesc Desc;
            AudioSoundInfo           Info {};
            std::deque<Transport>    History;
            std::atomic<bool>        Timed {false};
            std::atomic<uint64_t>    PendingSeek {NoFrame};
            std::shared_ptr<Control> Commands = std::make_shared<Control>();
            Fades                    Envelope;
            uint64_t                 PausedAt = NoFrame, RemainingStart = 0, RemainingStop = NoFrame;
            bool                     Initialized = false, Scheduled = false;
            ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }

            const Transport& Current() const { return History.back(); }
            double           Position(double clock) const
            {
                for (auto it = History.rbegin(); it != History.rend(); ++it)
                    if (it->Frame <= clock)
                        return it->Position(clock, StreamDelay.load(std::memory_order_relaxed));
                return History.front().Source;
            }

            void Record(Transport state, double heardFrame)
            {
                History.back().StreamDelayEnd = StreamDelay.load(std::memory_order_relaxed);
                while (History.size() > 1 && History[1].Frame <= heardFrame)
                    History.pop_front();
                if (History.back().Frame >= state.Frame)
                {
                    state.Frame    = History.back().Frame;
                    History.back() = state;
                }
                else
                    History.push_back(state);
            }
        };
        struct Slot
        {
            uint32_t Generation = 1;
            std::unique_ptr<Voice> Value;
        };

        struct Schedule
        {
            std::shared_ptr<Voice::Control> Control;
            uint64_t                        Revision;
            Voice::Transport                Transport;
            Voice::Fades                    Envelope;
            bool                            Replay;
        };

        struct ScheduleBatch
        {
            std::vector<Schedule> Operations;
            ScheduleBatch*        Next = nullptr;
            std::atomic<bool>     Applied {false};
        };

        std::atomic<ScheduleBatch*>                 PendingSchedule {nullptr};
        std::atomic<bool>                           ApplyingSchedule {false};
        std::vector<std::unique_ptr<ScheduleBatch>> ScheduleStorage;
        ScheduleBatch*                              BuildingSchedule = nullptr;

        static void Apply(const Schedule& command)
        {
            auto* voice = command.Control->Owner.load();
            if (!voice || command.Revision != command.Control->Revision.load())
                return;
            auto&       sound = voice->Sound;
            const auto& state = command.Transport;
            sound.Envelope    = command.Envelope;
            if (state.Start == NoFrame || state.Stop <= state.Start)
            {
                ma_sound_stop(&sound);
                return;
            }
            if (command.Replay)
            {
                ma_sound_stop(&sound);
                ma_sound_start(&sound);
                if (voice->Timed.load(std::memory_order_relaxed))
                    sound.Seek(0);
            }
            ma_sound_set_start_time_in_pcm_frames(&sound, state.Start);
            ma_sound_set_stop_time_in_pcm_frames(&sound, state.Stop);
            ma_node_set_state(&sound, ma_node_state_started);
        }

        void CollectSchedules()
        {
            ScheduleStorage.erase(
                std::remove_if(ScheduleStorage.begin(),
                               ScheduleStorage.end(),
                               [](const auto& batch) { return batch->Applied.load(std::memory_order_acquire); }),
                ScheduleStorage.end());
        }

        void Submit(std::unique_ptr<ScheduleBatch> batch)
        {
            CollectSchedules();
            auto* next = batch.get();
            ScheduleStorage.push_back(std::move(batch));
            auto* previous = PendingSchedule.load();
            do
            {
                next->Next = previous;
            } while (!PendingSchedule.compare_exchange_weak(previous, next));
        }

        void ApplySchedules()
        {
            if (!PendingSchedule.load())
                return;
            // The main thread can invalidate a voice and wait for this short pass before freeing it.
            // The mixer never waits on the producer or frees a batch.
            ApplyingSchedule.store(true);
            auto*          entries = PendingSchedule.exchange(nullptr);
            ScheduleBatch* ordered = nullptr;
            while (entries)
            {
                auto* next    = entries->Next;
                entries->Next = ordered;
                ordered       = entries;
                entries       = next;
            }
            while (ordered)
            {
                auto* current = ordered;
                ordered       = current->Next;
                for (const auto& operation : current->Operations)
                    Apply(operation);
                current->Applied.store(true, std::memory_order_release);
            }
            ApplyingSchedule.store(false);
        }

        void Queue(Voice& voice, bool replay = false)
        {
            voice.Scheduled = true;
            Schedule command {voice.Commands, ++voice.Commands->Revision, voice.Current(), voice.Envelope, replay};
            if (Config.Mode == AudioOutputMode::Offline)
                Apply(command);
            else if (BuildingSchedule)
                BuildingSchedule->Operations.push_back(std::move(command));
            else
            {
                auto batch = std::make_unique<ScheduleBatch>();
                batch->Operations.push_back(std::move(command));
                Submit(std::move(batch));
            }
        }

        void CancelSchedule(Voice& voice)
        {
            ++voice.Commands->Revision;
            while (ApplyingSchedule.load())
                std::this_thread::yield();
        }

        MemoryVFS VFS;
        ma_context Context{};
        ma_resource_manager Resources{};
        ma_engine Engine{};
        std::thread                     StreamWorker;
        std::mutex                      StreamMutex;
        std::condition_variable                   StreamWake;
        bool                                      StopWorker = false, StreamChanged = false;
        std::vector<std::shared_ptr<VoiceSource>> DeviceStreams;
        std::atomic<uint64_t>                     StreamWorkerPasses {0};

        void WakeStreams()
        {
            {
                std::lock_guard<std::mutex> lock(StreamMutex);
                StreamChanged = true;
            }
            StreamWake.notify_one();
        }
        std::atomic<uint64_t>           ClockSequence {0}, LastMixedFrame {0}, LastMixedCount {0};
        std::atomic<uint64_t>           MixEnd {0};
        std::atomic<int64_t>            LastMixTicks {0};
        mutable double                  PlaybackFrames = 0;
        uint64_t                        LatencyFrames  = 0;
        bool ContextReady = false, ResourcesReady = false, Ready = false;
        AudioEngineConfig Config;
        std::unique_ptr<AudioBusGraph>  Buses;
        std::unordered_set<std::string> WarnedBusNames;
        AudioDSPRegistry                DSPRegistry;
        std::unordered_set<std::string> WarnedDSPNames;

        uint64_t WritableClock() const
        {
            return std::max<uint64_t>(ma_engine_get_time_in_pcm_frames(&Engine), MixEnd.load());
        }

        void WarnUnknownDSP(const AudioBusEffectParameters& parameters)
        {
            if (const auto* custom = std::get_if<AudioCustomDSP>(&parameters))
                if (DSPRegistry.count(custom->Name) == 0 && WarnedDSPNames.insert(custom->Name).second)
                    Warn("Unknown custom DSP '" + custom->Name + "'; skipping effect");
        }

        ma_node* VoiceBusInput(AudioBusGraph& graph, const std::string& name)
        {
            if (!name.empty())
            {
                if (auto* input = graph.Input(name))
                    return input;
                if (WarnedBusNames.insert(name).second)
                    Warn("Unknown audio bus '" + name + "'; routing to Master");
            }
            return graph.Input(MasterAudioBus);
        }

        std::unordered_map<AudioSoundID, Sound> Sounds;
        std::vector<Slot> Slots;
        std::vector<AudioClipRegistry*> ClipRegistries;

        Voice* Find(AudioVoiceHandle handle) const
        {
            if (!Ready || !handle.IsValid() || handle.Index >= Slots.size())
                return nullptr;
            const auto& slot = Slots[handle.Index];
            return slot.Generation == handle.Generation ? slot.Value.get() : nullptr;
        }

        void DrainJobs()
        {
            if (ResourcesReady && Config.Mode == AudioOutputMode::Offline)
                while (ma_resource_manager_process_next_job(&Resources) == MA_SUCCESS) {}
        }

        std::optional<std::string> EnsureDecoded(Sound& sound)
        {
            if (sound.DecodedRegistered)
                return std::nullopt;
            ma_decoder decoder;
            auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
            auto result = ma_decoder_init_vfs(&VFS, sound.EncodedName.c_str(), &config, &decoder);
            if (result != MA_SUCCESS)
                return Error("Decoder init", result);
            sound.PCM.resize(static_cast<size_t>(sound.Info.LengthFrames) * sound.Info.Channels);
            ma_uint64 read = 0;
            result = ma_decoder_read_pcm_frames(&decoder, sound.PCM.data(), sound.Info.LengthFrames, &read);
            ma_decoder_uninit(&decoder);
            if ((result != MA_SUCCESS && result != MA_AT_END) || read == 0)
            {
                sound.PCM.clear();
                return Error("Decode", result);
            }
            sound.PCM.resize(static_cast<size_t>(read) * sound.Info.Channels);
            result = ma_resource_manager_register_decoded_data(&Resources, sound.DecodedName.c_str(), sound.PCM.data(),
                read, ma_format_f32, sound.Info.Channels, sound.Info.SampleRate);
            if (result != MA_SUCCESS)
                return Error("Register decoded data", result);
            sound.DecodedRegistered = true;
            return std::nullopt;
        }
    };

    AudioEngine::AudioEngine() : m_Impl(std::make_unique<Impl>()) {}
    AudioEngine::~AudioEngine() { Shutdown(); }

    std::optional<std::string> AudioEngine::Init(const AudioEngineConfig& config)
    {
        Shutdown();
        auto& p = *m_Impl;
        p.Config = config;
        if (config.MaxListeners < 1 || config.MaxListeners > 4 || config.Channels == 0 ||
            config.Channels > MA_MAX_CHANNELS || (config.Mode == AudioOutputMode::Offline && config.SampleRate == 0))
            return "Invalid audio engine configuration";

        if (config.Mode == AudioOutputMode::Device)
        {
            // Explicit backends prevent miniaudio from silently selecting its null device on a headless machine.
#ifdef _WIN32
            const ma_backend backends[] = {ma_backend_wasapi, ma_backend_dsound, ma_backend_winmm};
#else
            const ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa, ma_backend_jack};
#endif
            const ma_backend nullBackend = ma_backend_null;
            auto result = ma_context_init(config.UseNullDevice ? &nullBackend : backends,
                config.UseNullDevice ? 1 : sizeof(backends) / sizeof(backends[0]), nullptr, &p.Context);
            if (result != MA_SUCCESS)
                return Error("Audio context init", result);
            p.ContextReady = true;
        }
        auto resourcesConfig = ma_resource_manager_config_init();
        resourcesConfig.decodedFormat = ma_format_f32;
        resourcesConfig.pVFS = &p.VFS;
        resourcesConfig.jobThreadCount = 0;
        resourcesConfig.flags          = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
        auto result = ma_resource_manager_init(&resourcesConfig, &p.Resources);
        if (result != MA_SUCCESS)
        {
            Shutdown();
            return Error("Resource manager init", result);
        }
        p.ResourcesReady = true;
        if (config.Mode == AudioOutputMode::Device)
        {
            try
            {
                p.StopWorker    = false;
                p.StreamChanged = false;
                p.StreamWorkerPasses.store(0);
                p.StreamWorker = std::thread([&p] {
                    std::vector<std::shared_ptr<Impl::VoiceSource>> streams;
                    std::unique_lock<std::mutex>                    lock(p.StreamMutex);
                    const auto changed = [&] { return p.StopWorker || p.StreamChanged; };
                    while (!p.StopWorker)
                    {
                        if (p.DeviceStreams.empty())
                            p.StreamWake.wait(lock, changed);
                        else
                            p.StreamWake.wait_for(lock, std::chrono::milliseconds(5), changed);
                        if (p.StopWorker)
                            break;
                        p.StreamChanged = false;
                        streams         = p.DeviceStreams;
                        ++p.StreamWorkerPasses;
                        // Neither the main thread nor the mixer waits for decoder work or source destruction.
                        lock.unlock();
                        for (const auto& source : streams)
                            source->Fill();
                        streams.clear();
                        lock.lock();
                    }
                });
            }
            catch (const std::system_error& error)
            {
                Shutdown();
                return error.what();
            }
        }
        auto engineConfig = ma_engine_config_init();
        engineConfig.pResourceManager = &p.Resources;
        engineConfig.pContext = p.ContextReady ? &p.Context : nullptr;
        engineConfig.noDevice = config.Mode == AudioOutputMode::Offline;
        engineConfig.channels = config.Channels;
        engineConfig.sampleRate = config.SampleRate;
        engineConfig.listenerCount = config.MaxListeners;
        engineConfig.noAutoStart      = MA_TRUE;
        if (config.Mode == AudioOutputMode::Device)
        {
            engineConfig.dataCallback = [](ma_device* device, void* output, const void*, ma_uint32 frames) {
                auto*          engine  = static_cast<ma_engine*>(device->pUserData);
                auto&          impl    = *static_cast<Impl*>(engine->pProcessUserData);
                const auto*    graph   = ma_engine_get_node_graph(engine);
                const uint64_t quantum = std::max(1u, graph->processingSizeInFrames);
                const uint64_t needed  = frames - std::min(frames, graph->processingCacheFramesRemaining);
                const auto     first   = ma_engine_get_time_in_pcm_frames(engine);
                impl.MixEnd.store(AddFrames(first, (needed + quantum - 1) / quantum * quantum));
                impl.ApplySchedules();
                ma_engine_read_pcm_frames(engine, output, frames, nullptr);
                impl.MixEnd.store(ma_engine_get_time_in_pcm_frames(engine));
            };
            engineConfig.pProcessUserData = &p;
            engineConfig.onProcess        = [](void* user, float*, ma_uint64 count) {
                auto& impl = *static_cast<Impl*>(user);
                ++impl.ClockSequence;
                impl.LastMixedFrame.store(ma_engine_get_time_in_pcm_frames(&impl.Engine));
                impl.LastMixedCount.store(count);
                impl.LastMixTicks.store(MixClock::now().time_since_epoch().count());
                ++impl.ClockSequence;
            };
        }
        result = ma_engine_init(&engineConfig, &p.Engine);
        if (result != MA_SUCCESS)
        {
            Shutdown();
            return Error("Audio engine init", result);
        }
        p.Ready = true;
        if (auto error = ApplyBusLayout(AudioBusLayout::Default()))
        {
            Shutdown();
            return error;
        }
        if (config.Mode == AudioOutputMode::Device)
        {
            const auto&    playback     = ma_engine_get_device(&p.Engine)->playback;
            const uint64_t deviceFrames = uint64_t {playback.internalPeriodSizeInFrames} * playback.internalPeriods;
            if (playback.internalSampleRate != 0)
                p.LatencyFrames = static_cast<uint64_t>(
                    std::ceil(static_cast<double>(deviceFrames) * GetSampleRate() / playback.internalSampleRate));
            if (const auto start = ma_engine_start(&p.Engine); start != MA_SUCCESS)
            {
                Shutdown();
                return Error("Audio engine start", start);
            }
        }
        return std::nullopt;
    }

    void AudioEngine::Shutdown()
    {
        auto& p = *m_Impl;
        if (p.Ready && p.Config.Mode == AudioOutputMode::Device)
            ma_engine_stop(&p.Engine);
        while (!p.ClipRegistries.empty())
            p.ClipRegistries.back()->Clear();
        for (uint32_t i = 0; i < p.Slots.size(); ++i)
            if (p.Slots[i].Value)
                DestroyVoice({i, p.Slots[i].Generation});
        while (!p.Sounds.empty())
            UnregisterSound(p.Sounds.begin()->first);
        p.Buses.reset();
        p.WarnedBusNames.clear();
        p.WarnedDSPNames.clear();
        if (p.Ready)
            ma_engine_uninit(&p.Engine);
        p.Ready = false;
        if (p.StreamWorker.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(p.StreamMutex);
                p.StopWorker = true;
            }
            p.StreamWake.notify_one();
            p.StreamWorker.join();
        }
        if (p.ResourcesReady)
            ma_resource_manager_uninit(&p.Resources);
        p.ResourcesReady = false;
        if (p.ContextReady)
            ma_context_uninit(&p.Context);
        p.ContextReady = false;
        p.PendingSchedule.store(nullptr);
        p.ScheduleStorage.clear();
        p.ClockSequence.store(0);
        p.LastMixedFrame.store(0);
        p.LastMixedCount.store(0);
        p.LastMixTicks.store(0);
        p.MixEnd.store(0);
        p.PlaybackFrames = 0;
        p.LatencyFrames  = 0;
    }

    bool AudioEngine::IsInitialized() const { return m_Impl->Ready; }
    AudioOutputMode AudioEngine::GetMode() const { return m_Impl->Config.Mode; }
    uint32_t AudioEngine::GetSampleRate() const { return IsInitialized() ? ma_engine_get_sample_rate(&m_Impl->Engine) : 0; }
    uint32_t AudioEngine::GetChannels() const { return IsInitialized() ? ma_engine_get_channels(&m_Impl->Engine) : 0; }
    uint64_t AudioEngine::GetClockFrames() const
    {
        return IsInitialized() ? ma_engine_get_time_in_pcm_frames(&m_Impl->Engine) : 0;
    }

    uint64_t AudioEngine::GetSchedulingClockFrames() const { return IsInitialized() ? m_Impl->WritableClock() : 0; }

    uint64_t AudioEngine::GetStreamWorkerPasses() const { return m_Impl->StreamWorkerPasses.load(); }

    bool AudioEngine::IsStreamSeekPending(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice && voice->Source->Stream && !voice->Source->Offline &&
               voice->Source->SeekRevision.load() != voice->Source->ReadyRevision.load();
    }

    uint64_t AudioEngine::GetOutputLatencyFrames() const { return m_Impl->LatencyFrames; }

    double AudioEngine::GetPlaybackClockFrames() const
    {
        auto& p = *m_Impl;
        if (!IsInitialized() || GetMode() == AudioOutputMode::Offline)
            return static_cast<double>(GetClockFrames());
        // Read a consistent mix timestamp. The caller is the main thread, so it may retry if
        // a device callback is publishing concurrently.
        uint64_t sequence, mixed, count;
        int64_t  ticks;
        do
        {
            sequence = p.ClockSequence.load();
            mixed    = p.LastMixedFrame.load();
            count    = p.LastMixedCount.load();
            ticks    = p.LastMixTicks.load();
        } while (sequence % 2 != 0 || sequence != p.ClockSequence.load());
        const auto   last    = MixClock::time_point(MixClock::duration(ticks));
        const double elapsed = std::chrono::duration<double>(MixClock::now() - last).count() * GetSampleRate();
        const auto   heard =
            static_cast<double>(mixed) - p.LatencyFrames + std::clamp(elapsed, 0.0, static_cast<double>(count));
        p.PlaybackFrames = std::clamp(heard, p.PlaybackFrames, static_cast<double>(mixed));
        return p.PlaybackFrames;
    }

    double AudioEngine::GetPlaybackClockSeconds() const
    {
        return IsInitialized() ? GetPlaybackClockFrames() / GetSampleRate() : 0;
    }
    void AudioEngine::SetMasterVolume(float linear)
    {
        if (IsInitialized() && std::isfinite(linear))
            ma_engine_set_volume(&m_Impl->Engine, std::max(0.f, linear));
    }
    float AudioEngine::GetMasterVolume() const
    {
        return IsInitialized() ? ma_engine_get_volume(&m_Impl->Engine) : 0;
    }

    uint64_t AudioEngine::Render(float* out, uint64_t frameCount)
    {
        if (!IsInitialized() || GetMode() != AudioOutputMode::Offline || !out)
            return 0;
        uint64_t written = 0;
        while (written < frameCount)
        {
            m_Impl->DrainJobs();
            ma_uint64 read = 0;
            const auto result = ma_engine_read_pcm_frames(&m_Impl->Engine, out + written * GetChannels(),
                std::min<uint64_t>(256, frameCount - written), &read);
            written += read;
            if (result != MA_SUCCESS || read == 0)
                break;
        }
        m_Impl->DrainJobs();
        return written;
    }

    void AudioEngine::Update()
    {
        if (!IsInitialized())
            return;
        m_Impl->DrainJobs();
        m_Impl->CollectSchedules();
        for (uint32_t i = 0; i < m_Impl->Slots.size(); ++i)
        {
            auto& slot = m_Impl->Slots[i];
            // ma_sound_at_end reads miniaudio's atomic flag; no engine callback runs on the audio thread.
            if (slot.Value && slot.Value->Desc.AutoRelease)
            {
                const auto stop = slot.Value->Current().Stop;
                if ((ma_sound_at_end(&slot.Value->Sound) &&
                     (!slot.Value->Timed.load() ||
                      slot.Value->Position(GetPlaybackClockFrames()) >= slot.Value->Info.LengthFrames)) ||
                    (stop != NoFrame && GetPlaybackClockFrames() >= stop))
                    DestroyVoice({i, slot.Generation});
            }
        }
    }

    std::optional<std::string> AudioEngine::ApplyBusLayout(const AudioBusLayout& layout)
    {
        auto& p = *m_Impl;
        if (!p.Ready)
            return "Audio engine is unavailable";
        if (auto error = ValidateAudioBusLayout(layout))
            return error->Message;

        for (const auto& bus : layout.Buses)
            for (const auto& effect : bus.Effects)
                p.WarnUnknownDSP(effect.Parameters);
        if (p.Buses && p.Buses->TryUpdate(layout, p.DSPRegistry))
            return std::nullopt;
        return ReplaceBusGraph(layout);
    }

    std::optional<std::string> AudioEngine::ReplaceBusGraph(const AudioBusLayout& layout)
    {
        auto& p = *m_Impl;
        // Build off the endpoint, then move existing sound nodes without touching their playback state.
        auto next   = std::make_unique<AudioBusGraph>();
        auto result = next->Init(p.Engine, layout, p.Buses.get(), p.DSPRegistry);
        if (result == MA_SUCCESS)
            result = next->ConnectEndpoint();
        if (result != MA_SUCCESS)
            return Error("Build audio bus graph", result);
        for (const auto& slot : p.Slots)
        {
            if (!slot.Value)
                continue;
            auto& voice = *slot.Value;
            result      = ma_node_attach_output_bus(&voice.Sound, 0, p.VoiceBusInput(*next, voice.Desc.Bus), 0);
            if (result != MA_SUCCESS)
            {
                for (const auto& moved : p.Slots)
                {
                    if (&moved == &slot)
                        break;
                    if (moved.Value)
                        ma_node_attach_output_bus(
                            &moved.Value->Sound, 0, p.VoiceBusInput(*p.Buses, moved.Value->Desc.Bus), 0);
                }
                return Error("Reattach audio voice", result);
            }
        }
        p.Buses = std::move(next);
        return std::nullopt;
    }

    const std::vector<AudioBusDefinition>& AudioEngine::GetBuses() const
    {
        static const std::vector<AudioBusDefinition> empty;
        return m_Impl->Buses ? m_Impl->Buses->GetBuses() : empty;
    }

    bool AudioEngine::SetBusVolumeDb(const std::string& bus, float db)
    {
        auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition || !IsValidAudioBusLevel(db))
            return false;
        definition->VolumeDb = db;
        m_Impl->Buses->UpdateFaders();
        return true;
    }

    bool AudioEngine::SetBusMute(const std::string& bus, bool mute)
    {
        auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition)
            return false;
        definition->Mute = mute;
        m_Impl->Buses->UpdateFaders();
        return true;
    }

    bool AudioEngine::SetBusSolo(const std::string& bus, bool solo)
    {
        auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition)
            return false;
        definition->Solo = solo;
        m_Impl->Buses->UpdateFaders();
        return true;
    }

    bool AudioEngine::SetBusBypass(const std::string& bus, bool bypass)
    {
        auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition)
            return false;
        definition->Bypass = bypass;
        m_Impl->Buses->UpdateBypass(bus, bypass);
        return true;
    }

    bool AudioEngine::SetBusSendLevelDb(const std::string& bus, size_t sendIndex, float db)
    {
        auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition || sendIndex >= definition->Sends.size() || !IsValidAudioBusLevel(db))
            return false;
        definition->Sends[sendIndex].LevelDb = db;
        m_Impl->Buses->UpdateSend(bus, sendIndex);
        return true;
    }

    std::optional<AudioBusMeter> AudioEngine::GetBusMeter(const std::string& bus) const
    {
        return m_Impl->Buses ? m_Impl->Buses->GetMeter(bus) : std::nullopt;
    }

    bool AudioEngine::ReadBusMeter(const std::string& bus, AudioBusMeter& meter) const
    {
        return m_Impl->Buses && m_Impl->Buses->ReadMeter(bus, meter);
    }

    const std::vector<AudioBusEffect>& AudioEngine::GetBusEffects(const std::string& bus) const
    {
        const auto*                              definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        static const std::vector<AudioBusEffect> empty;
        return definition ? definition->Effects : empty;
    }

    std::optional<std::string>
    AudioEngine::AddBusEffect(const std::string& bus, const AudioBusEffect& effect, std::optional<size_t> index)
    {
        const auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition)
            return "Unknown audio bus: " + bus;
        const auto position = index.value_or(definition->Effects.size());
        if (position > definition->Effects.size())
            return "Audio bus effect index out of range";
        AudioBusLayout layout {GetBuses()};
        auto&          effects = layout.Buses[definition - GetBuses().data()].Effects;
        effects.insert(effects.begin() + position, effect);
        return ApplyBusLayout(layout);
    }

    std::optional<std::string> AudioEngine::RemoveBusEffect(const std::string& bus, size_t index)
    {
        const auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition || index >= definition->Effects.size())
            return "Unknown audio bus or effect index";
        AudioBusLayout layout {GetBuses()};
        auto&          effects = layout.Buses[definition - GetBuses().data()].Effects;
        effects.erase(effects.begin() + index);
        return ApplyBusLayout(layout);
    }

    std::optional<std::string> AudioEngine::MoveBusEffect(const std::string& bus, size_t from, size_t to)
    {
        const auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        if (!definition || from >= definition->Effects.size() || to >= definition->Effects.size())
            return "Unknown audio bus or effect index";
        if (from == to)
            return std::nullopt;
        AudioBusLayout layout {GetBuses()};
        auto&          effects = layout.Buses[definition - GetBuses().data()].Effects;
        auto           effect  = std::move(effects[from]);
        effects.erase(effects.begin() + from);
        effects.insert(effects.begin() + to, std::move(effect));
        return ReplaceBusGraph(layout);
    }

    std::optional<std::string> AudioEngine::SetBusEffectParameters(const std::string&              bus,
                                                                   size_t                          index,
                                                                   const AudioBusEffectParameters& parameters)
    {
        auto&       p          = *m_Impl;
        const auto* definition = p.Buses ? p.Buses->Find(bus) : nullptr;
        if (!definition || index >= definition->Effects.size())
            return "Unknown audio bus or effect index";
        if (definition->Effects[index].Parameters.index() != parameters.index())
            return "Audio bus effect parameter type does not match its slot";
        if (auto error = ValidateAudioBusEffect(parameters, GetBuses()))
            return error->Message;
        p.WarnUnknownDSP(parameters);
        if (p.Buses->SetEffectParameters(bus, index, parameters, p.DSPRegistry))
            return std::nullopt;
        AudioBusLayout layout {GetBuses()};
        layout.Buses[definition - GetBuses().data()].Effects[index].Parameters = parameters;
        return ReplaceBusGraph(layout);
    }

    bool AudioEngine::SetBusEffectEnabled(const std::string& bus, size_t index, bool enabled)
    {
        const auto& effects = GetBusEffects(bus);
        if (index >= effects.size())
            return false;
        m_Impl->Buses->UpdateEffectEnabled(bus, index, enabled);
        return true;
    }

    std::optional<float> AudioEngine::GetBusEffectGainReductionDb(const std::string& bus, size_t index) const
    {
        return m_Impl->Buses ? m_Impl->Buses->GetEffectReductionDb(bus, index) : std::nullopt;
    }

    bool AudioEngine::RegisterCustomDSP(const std::string& name, AudioDSPCallback callback, void* user)
    {
        auto& p = *m_Impl;
        if (name.empty() || !callback || p.DSPRegistry.count(name) != 0)
            return false;
        auto registration      = std::make_shared<AudioDSPRegistration>();
        registration->Callback = callback;
        registration->User     = user;
        p.DSPRegistry.emplace(name, std::move(registration));
        p.WarnedDSPNames.erase(name);
        // Slots that already name this effect were built without a callback (or with an unregistered one).
        // Rebuilding binds them now instead of leaving them as passthrough until the next layout change.
        if (p.Buses)
        {
            const auto& buses      = GetBuses();
            const bool  referenced = std::any_of(buses.begin(), buses.end(), [&](const auto& bus) {
                return std::any_of(bus.Effects.begin(), bus.Effects.end(), [&](const auto& effect) {
                    const auto* custom = std::get_if<AudioCustomDSP>(&effect.Parameters);
                    return custom && custom->Name == name;
                });
            });
            if (referenced)
                if (auto error = ReplaceBusGraph(AudioBusLayout {buses}))
                    Warn("Bind custom DSP '" + name + "': " + *error);
        }
        return true;
    }

    bool AudioEngine::UnregisterCustomDSP(const std::string& name)
    {
        auto&      registry = m_Impl->DSPRegistry;
        const auto found    = registry.find(name);
        if (found == registry.end())
            return false;
        const auto& registration = found->second;
        registration->Enabled.store(false);
        while (registration->InFlight.load() != 0)
            std::this_thread::yield();
        registry.erase(found);
        return true;
    }

    std::vector<std::string> AudioEngine::GetCustomDSPNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Impl->DSPRegistry.size());
        for (const auto& entry : m_Impl->DSPRegistry)
            names.push_back(entry.first);
        std::sort(names.begin(), names.end());
        return names;
    }

    void AudioEngine::AttachClipRegistry(AudioClipRegistry* registry)
    {
        m_Impl->ClipRegistries.push_back(registry);
    }

    void AudioEngine::DetachClipRegistry(AudioClipRegistry* registry)
    {
        auto& registries = m_Impl->ClipRegistries;
        registries.erase(std::remove(registries.begin(), registries.end(), registry), registries.end());
    }

    std::optional<std::string> AudioEngine::RegisterSound(AudioSoundID id, std::shared_ptr<const std::vector<std::byte>> encodedBytes)
    {
        if (!IsInitialized())
            return "Audio engine is not initialized";
        if (IsSoundRegistered(id))
            return "Audio sound ID is already registered";
        if (!encodedBytes || encodedBytes->empty())
            return "Audio sound data is empty";
        ma_decoder decoder;
        auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
        auto result = ma_decoder_init_memory(encodedBytes->data(), encodedBytes->size(), &config, &decoder);
        if (result != MA_SUCCESS)
            return Error("Decoder init", result);
        Impl::Sound sound;
        ma_uint64 length = 0;
        result = ma_decoder_get_data_format(&decoder, nullptr, &sound.Info.Channels, &sound.Info.SampleRate, nullptr, 0);
        if (result == MA_SUCCESS)
            result = ma_decoder_get_length_in_pcm_frames(&decoder, &length);
        ma_decoder_uninit(&decoder);
        if (result != MA_SUCCESS || sound.Info.Channels == 0 || sound.Info.SampleRate == 0 || length == 0 ||
            length > std::vector<float>().max_size() / sound.Info.Channels)
            return "Invalid or unknown audio sound length";
        sound.Info.LengthFrames = length;
        sound.Encoded = std::move(encodedBytes);
        sound.EncodedName = "encoded/" + std::to_string(id);
        sound.DecodedName = "decoded/" + std::to_string(id);
        {
            std::lock_guard<std::mutex> lock(m_Impl->VFS.Mutex);
            m_Impl->VFS.Files.emplace(sound.EncodedName, sound.Encoded);
        }
        m_Impl->Sounds.emplace(id, std::move(sound));
        return std::nullopt;
    }

    std::optional<std::string> AudioEngine::RegisterSoundPCM(AudioSoundID id, std::vector<float> interleaved,
                                                            uint32_t channels, uint32_t sampleRate)
    {
        if (!IsInitialized())
            return "Audio engine is not initialized";
        if (IsSoundRegistered(id))
            return "Audio sound ID is already registered";
        if (!channels || channels > MA_MAX_CHANNELS || !sampleRate || interleaved.empty() || interleaved.size() % channels)
            return "Invalid audio PCM data";
        Impl::Sound sound;
        sound.Info = {channels, sampleRate, interleaved.size() / channels};
        sound.PCM = std::move(interleaved);
        sound.DecodedName = "decoded/" + std::to_string(id);
        auto result = ma_resource_manager_register_decoded_data(&m_Impl->Resources, sound.DecodedName.c_str(),
            sound.PCM.data(), sound.Info.LengthFrames, ma_format_f32, channels, sampleRate);
        if (result != MA_SUCCESS)
            return Error("Register PCM data", result);
        sound.DecodedRegistered = true;
        m_Impl->Sounds.emplace(id, std::move(sound));
        return std::nullopt;
    }

    void AudioEngine::UnregisterSound(AudioSoundID id)
    {
        auto it = m_Impl->Sounds.find(id);
        if (it == m_Impl->Sounds.end())
            return;
        for (uint32_t i = 0; i < m_Impl->Slots.size(); ++i)
        {
            auto& slot = m_Impl->Slots[i];
            if (slot.Value && slot.Value->Desc.Sound == id)
                DestroyVoice({i, slot.Generation});
        }
        auto& sound = it->second;
        if (sound.DecodedRegistered)
            ma_resource_manager_unregister_data(&m_Impl->Resources, sound.DecodedName.c_str());
        if (sound.Encoded)
        {
            std::lock_guard<std::mutex> lock(m_Impl->VFS.Mutex);
            m_Impl->VFS.Files.erase(sound.EncodedName);
        }
        m_Impl->Sounds.erase(it);
    }

    bool AudioEngine::IsSoundRegistered(AudioSoundID id) const { return m_Impl->Sounds.count(id) != 0; }
    std::optional<AudioSoundInfo> AudioEngine::GetSoundInfo(AudioSoundID id) const
    {
        auto it = m_Impl->Sounds.find(id);
        return it != m_Impl->Sounds.end() ? std::optional<AudioSoundInfo>(it->second.Info) : std::nullopt;
    }
    uint64_t AudioEngine::GetDecodedBytes(AudioSoundID id) const
    {
        auto it = m_Impl->Sounds.find(id);
        if (it == m_Impl->Sounds.end() || !it->second.DecodedRegistered)
            return 0;
        return it->second.PCM.size() * sizeof(float);
    }

    AudioVoiceHandle AudioEngine::CreateVoice(const AudioVoiceDesc& desc) { return CreateVoice(desc, 0, 0); }

    AudioVoiceHandle AudioEngine::CreateVoice(const AudioVoiceDesc& desc, uint64_t loopStart, uint64_t loopEnd)
    {
        auto it = m_Impl->Sounds.find(desc.Sound);
        if (!IsInitialized() || it == m_Impl->Sounds.end())
        {
            Warn("Cannot create voice: sound is not registered or engine is not initialized");
            return {};
        }
        auto& sound = it->second;
        const bool stream = desc.LoadMode == AudioLoadMode::Stream && sound.Encoded;
        if (!stream)
        {
            if (auto error = m_Impl->EnsureDecoded(sound))
            {
                Warn(*error);
                return {};
            }
        }
        auto voice = std::make_unique<Impl::Voice>();
        voice->Desc = desc;
        voice->Info = sound.Info;
        Impl::Voice::Transport transport;
        transport.Frame     = GetClockFrames();
        transport.Loop      = desc.Loop;
        transport.LoopBegin = loopStart;
        transport.LoopEnd   = loopEnd != 0 ? loopEnd : sound.Info.LengthFrames;
        voice->History.push_back(transport);
        auto result = voice->Source->Init(&m_Impl->Resources,
                                          m_Impl->VFS,
                                          (stream ? sound.EncodedName : sound.DecodedName).c_str(),
                                          stream,
                                          GetMode() == AudioOutputMode::Offline,
                                          sound.Info,
                                          loopStart,
                                          loopEnd);
        if (result != MA_SUCCESS)
        {
            Warn(Error("Voice source init", result));
            return {};
        }
        auto config = ma_sound_config_init_2(&m_Impl->Engine);
        config.pDataSource = voice->Source.get();
        config.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
        if (!desc.Spatial)
            config.flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (desc.Loop && !(stream && loopEnd != 0))
            config.flags |= MA_SOUND_FLAG_LOOPING;
        if (loopEnd != 0 && !stream)
        {
            config.loopPointBegInPCMFrames = loopStart;
            config.loopPointEndInPCMFrames = loopEnd;
        }
        result = ma_sound_init_ex(&m_Impl->Engine, &config, &voice->Sound);
        if (result != MA_SUCCESS)
        {
            Warn(Error("Voice init", result));
            return {};
        }
        voice->Initialized = true;
        auto& node         = voice->Sound;
        node.Owner         = voice.get();
        voice->Commands->Owner.store(voice.get());
        node.Original                      = node.engineNode.baseNode.vtable;
        static const ma_node_vtable vtable = {Impl::Voice::Node::Process, nullptr, 0, 1, 0};
        node.engineNode.baseNode.vtable    = &vtable;
        result = ma_node_attach_output_bus(&node, 0, m_Impl->VoiceBusInput(*m_Impl->Buses, desc.Bus), 0);
        if (result != MA_SUCCESS)
        {
            Warn(Error("Attach voice", result));
            return {};
        }
        uint32_t index = 0;
        for (; index < m_Impl->Slots.size(); ++index)
            if (!m_Impl->Slots[index].Value && m_Impl->Slots[index].Generation != 0)
                break;
        if (index == m_Impl->Slots.size())
            m_Impl->Slots.emplace_back();
        auto& slot = m_Impl->Slots[index];
        if (stream && GetMode() == AudioOutputMode::Device)
        {
            {
                std::lock_guard<std::mutex> lock(m_Impl->StreamMutex);
                m_Impl->DeviceStreams.push_back(voice->Source);
            }
            m_Impl->WakeStreams();
        }
        slot.Value = std::move(voice);
        AudioVoiceHandle handle{index, slot.Generation};
        SetVolume(handle, desc.Volume);
        SetPitch(handle, desc.Pitch);
        SetPan(handle, desc.Pan);
        ma_sound_set_pan_mode(&slot.Value->Sound, ma_pan_mode_balance);
        if (desc.Spatial)
        {
            SetSpatialParams(handle, {});
        }
        return handle;
    }

    bool AudioEngine::SetVoiceBus(AudioVoiceHandle handle, const std::string& bus)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice)
            return false;
        if (voice->Desc.Bus == bus)
            return true;
        auto& graph = *m_Impl->Buses;
        auto* input = m_Impl->VoiceBusInput(graph, bus);
        if (input != m_Impl->VoiceBusInput(graph, voice->Desc.Bus))
        {
            if (const auto result = ma_node_attach_output_bus(&voice->Sound, 0, input, 0); result != MA_SUCCESS)
            {
                Warn(Error("Move audio voice", result));
                return false;
            }
        }
        voice->Desc.Bus = bus;
        return true;
    }

    std::optional<std::string> AudioEngine::GetVoiceBus(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice)
            return std::nullopt;
        return m_Impl->Buses->Input(voice->Desc.Bus) ? voice->Desc.Bus : MasterAudioBus;
    }

    void AudioEngine::DestroyVoice(AudioVoiceHandle handle)
    {
        if (!m_Impl->Find(handle))
            return;
        auto& slot = m_Impl->Slots[handle.Index];
        slot.Value->Commands->Owner.store(nullptr);
        m_Impl->CancelSchedule(*slot.Value);
        if (slot.Value->Source->Stream && GetMode() == AudioOutputMode::Device)
        {
            std::lock_guard<std::mutex> lock(m_Impl->StreamMutex);
            auto&                       streams = m_Impl->DeviceStreams;
            streams.erase(std::find(streams.begin(), streams.end(), slot.Value->Source));
        }
        slot.Value.reset();
        // Retire a slot permanently on generation wrap so even ancient handles cannot become live again.
        ++slot.Generation;
    }
    bool AudioEngine::IsVoiceValid(AudioVoiceHandle handle) const { return m_Impl->Find(handle) != nullptr; }
    void AudioEngine::Play(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            auto       state = voice->Current();
            const auto now   = voice->Scheduled ? m_Impl->WritableClock() : GetClockFrames();
            const bool ended = ma_sound_at_end(&voice->Sound);
            if (state.Start != NoFrame && now < state.Stop && !ended)
                return;
            if (ended)
                state.Source = 0;
            else
                state.Source = state.Position(static_cast<double>(now), voice->StreamDelay.load());
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Frame = now;
            state.Start = now;
            state.Stop  = NoFrame;
            if (voice->PausedAt != NoFrame)
            {
                state.Start = AddFrames(now, voice->RemainingStart);
                if (voice->RemainingStop != NoFrame)
                    state.Stop = AddFrames(now, voice->RemainingStop);
                const auto delay         = now - voice->PausedAt;
                voice->Envelope.In.Start = AddFrames(voice->Envelope.In.Start, delay);
                if (voice->Envelope.Out)
                    voice->Envelope.Out->Start = AddFrames(voice->Envelope.Out->Start, delay);
                voice->PausedAt = NoFrame;
            }
            else
                voice->Envelope = {};
            if (voice->Source->Stream && !voice->Source->Offline)
                m_Impl->WakeStreams();
            if (voice->Scheduled)
            {
                voice->Record(state, GetPlaybackClockFrames());
                m_Impl->Queue(*voice, ended);
                return;
            }
            ma_sound_set_start_time_in_pcm_frames(&voice->Sound, state.Start);
            ma_sound_set_stop_time_in_pcm_frames(&voice->Sound, state.Stop);
            ma_sound_start(&voice->Sound);
            voice->Record(state, GetPlaybackClockFrames());
        }
    }

    void AudioEngine::ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame)
    {
        StartVoice(handle, clockFrame, 0, AudioFadeCurve::Linear);
    }

    void AudioEngine::ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame, const AudioVoiceWindow& window)
    {
        StartVoice(handle, clockFrame, 0, AudioFadeCurve::Linear, &window);
    }

    uint64_t AudioEngine::StartVoice(AudioVoiceHandle        handle,
                                     uint64_t                clockFrame,
                                     uint64_t                fadeFrames,
                                     AudioFadeCurve          curve,
                                     const AudioVoiceWindow* window)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice)
            return clockFrame;
        auto& sound = voice->Sound;
        m_Impl->CancelSchedule(*voice);
        // Detach on the main thread to prepare the decoder, cache and resampler before arming playback.
        ma_node_detach_output_bus(&sound, 0);
        ma_sound_stop(&sound);
        auto       state  = voice->Current();
        const auto now    = m_Impl->WritableClock();
        const auto source =
            ma_sound_at_end(&sound) ? 0 : state.Position(static_cast<double>(now), voice->StreamDelay.load());
        const auto late   = !m_Impl->BuildingSchedule && now > clockFrame ? now - clockFrame : 0;
        const auto frame =
            static_cast<uint64_t>(WrapPosition(source + late * state.Rate, state.LoopBegin, state.LoopEnd, state.Loop));
        voice->PendingSeek.store(NoFrame);
        sound.seekTarget = NoFrame;
        sound.atEnd      = MA_FALSE;
        sound.Seek(frame);
        if (voice->Source->Stream && !voice->Source->Offline)
            m_Impl->WakeStreams();
        state.StreamDelayBase = voice->StreamDelay.load();
        state.Source    = static_cast<double>(frame);
        state.Frame     = now;
        state.Start     = m_Impl->BuildingSchedule ? clockFrame : std::max(m_Impl->WritableClock(), clockFrame);
        state.Stop      = NoFrame;
        voice->PausedAt = NoFrame;
        voice->Timed.store(true);
        voice->Envelope = {{state.Start, fadeFrames, fadeFrames == 0 ? 1.f : 0.f, 1.f, curve}, std::nullopt};
        if (window)
        {
            state.Stop         = AddFrames(clockFrame, window->Duration);
            voice->Envelope.In = {state.Start,
                                  window->FadeIn,
                                  window->FadeIn == 0 ? 1.f : 0.f,
                                  1.f,
                                  window->InCurve,
                                  AddFrames(window->FadeInElapsed, state.Start - clockFrame)};
            if (window->FadeOut != 0 && state.Stop != NoFrame)
            {
                const auto remaining = window->FadeOut - std::min(window->FadeOutElapsed, window->FadeOut);
                const auto fadeEnd   = AddFrames(clockFrame, window->FadeOutEnd.value_or(window->Duration));
                const auto begin     = fadeEnd - std::min(remaining, fadeEnd - clockFrame);
                const auto start     = std::max(begin, state.Start);
                voice->Envelope.Out  = VoiceEnvelope {
                    start, window->FadeOut, 1, 0, window->OutCurve, AddFrames(window->FadeOutElapsed, start - begin)};
                voice->Envelope.Multiply = true;
            }
        }
        ma_node_attach_output_bus(&sound, 0, m_Impl->VoiceBusInput(*m_Impl->Buses, voice->Desc.Bus), 0);
        voice->Record(state, GetPlaybackClockFrames());
        m_Impl->Queue(*voice);
        return state.Start;
    }

    void
    AudioEngine::ScheduleStop(AudioVoiceHandle handle, uint64_t clockFrame, uint64_t fadeFrames, AudioFadeCurve curve)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            auto state  = voice->Current();
            state.Frame = m_Impl->BuildingSchedule ? GetClockFrames() : m_Impl->WritableClock();
            state.Stop  = m_Impl->BuildingSchedule ? clockFrame : std::max(state.Frame, clockFrame);
            if (fadeFrames != 0)
            {
                const auto earliest = m_Impl->BuildingSchedule ? 0 : state.Frame;
                const auto start    = std::max(earliest, state.Stop - std::min(state.Stop, fadeFrames));
                // A superseded future fade should have no bearing on the replacement.
                if (voice->Envelope.Out && voice->Envelope.Out->Start > state.Frame)
                    voice->Envelope.Out.reset();
                const auto from = voice->Envelope.Gain(start);
                if (voice->Envelope.Out)
                    voice->Envelope.In = *voice->Envelope.Out;
                voice->Envelope.Out = VoiceEnvelope {start, state.Stop - start, from, 0, curve};
            }
            else
                voice->Envelope.Out.reset();
            voice->Envelope.Multiply = false;
            voice->Record(state, GetPlaybackClockFrames());
            m_Impl->Queue(*voice);
        }
    }
    uint64_t AudioMusicPlayer::WritableClock() const
    {
        return m_Engine.IsInitialized() ? m_Engine.m_Impl->WritableClock() : 0;
    }

    uint64_t AudioMusicPlayer::ScheduleSwitch(AudioVoiceHandle        incoming,
                                              uint64_t                frame,
                                              uint64_t                fade,
                                              AudioFadeCurve          curve,
                                              std::optional<uint64_t> superseded)
    {
        auto& impl  = *m_Engine.m_Impl;
        auto  batch = std::make_unique<AudioEngine::Impl::ScheduleBatch>();
        batch->Operations.reserve(m_Tracks.size() + 1);
        struct ScheduleScope
        {
            AudioEngine::Impl& Engine;
            ~ScheduleScope() { Engine.BuildingSchedule = nullptr; }
        } scope {impl};
        impl.BuildingSchedule = batch.get();
        const auto start      = m_Engine.StartVoice(incoming, frame, fade, curve);
        const auto end        = AddFrames(start, fade);
        for (auto& outgoing : m_Tracks)
        {
            if (!m_Engine.IsVoiceValid(outgoing.Voice))
                continue;
            if (end < outgoing.End || outgoing.End == NoFrame || (superseded && outgoing.EndFromSwitch == *superseded))
            {
                outgoing.End           = end;
                outgoing.EndFromSwitch = start;
                m_Engine.ScheduleStop(outgoing.Voice, end, fade, curve);
            }
        }
        if (!batch->Operations.empty())
            impl.Submit(std::move(batch));
        return start;
    }

    void AudioEngine::Stop(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            m_Impl->CancelSchedule(*voice);
            ma_sound_stop(&voice->Sound);
            ma_sound_reset_stop_time(&voice->Sound);
            voice->PausedAt = NoFrame;
            auto state      = voice->Current();
            state.Frame     = GetClockFrames();
            state.Start = state.Stop = NoFrame;
            state.Source             = 0;
            voice->Record(state, GetPlaybackClockFrames());
            if (voice->Timed.load())
            {
                voice->Envelope = {};
                voice->PendingSeek.store(0);
            }
            else
                ma_sound_seek_to_pcm_frame(&voice->Sound, 0);
        }
    }
    void AudioEngine::Pause(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            auto       state = voice->Current();
            const auto now   = GetClockFrames();
            if (state.Start == NoFrame || state.Stop <= now)
                return;
            m_Impl->CancelSchedule(*voice);
            ma_sound_stop(&voice->Sound);
            voice->PausedAt       = now;
            voice->RemainingStart = state.Start > now ? state.Start - now : 0;
            voice->RemainingStop  = state.Stop == NoFrame ? NoFrame : state.Stop - now;
            state.Source          = state.Position(static_cast<double>(now), voice->StreamDelay.load());
            state.Frame           = now;
            state.Start = state.Stop = NoFrame;
            voice->Record(state, GetPlaybackClockFrames());
        }
    }
    bool AudioEngine::IsPlaying(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        return voice && GetClockFrames() < voice->Current().Stop && ma_sound_is_playing(&voice->Sound);
    }
    bool AudioEngine::HasEnded(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        return voice && ma_sound_at_end(&voice->Sound);
    }
    void AudioEngine::SetVolume(AudioVoiceHandle handle, float linear)
    {
        if (auto* voice = m_Impl->Find(handle); voice && std::isfinite(linear))
        {
            voice->Desc.Volume = std::max(0.f, linear);
            ma_sound_set_volume(&voice->Sound, voice->Desc.Volume);
        }
    }
    void AudioEngine::SetPitch(AudioVoiceHandle handle, float pitch)
    {
        if (auto* voice = m_Impl->Find(handle); voice && std::isfinite(pitch) && pitch > 0)
        {
            voice->Desc.Pitch = pitch;
            ma_sound_set_pitch(&voice->Sound, pitch);
            auto state   = voice->Current();
            state.Frame  = GetClockFrames();
            state.Source          = state.Position(static_cast<double>(state.Frame), voice->StreamDelay.load());
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Start  = std::max(state.Start, state.Frame);
            state.Rate   = static_cast<double>(pitch) * voice->Info.SampleRate / GetSampleRate();
            voice->Record(state, GetPlaybackClockFrames());
        }
    }
    void AudioEngine::SetPan(AudioVoiceHandle handle, float pan)
    {
        if (auto* voice = m_Impl->Find(handle); voice && std::isfinite(pan))
        {
            voice->Desc.Pan = std::clamp(pan, -1.f, 1.f);
            ma_sound_set_pan(&voice->Sound, voice->Desc.Pan);
        }
    }
    void AudioEngine::SetLooping(AudioVoiceHandle handle, bool looping)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            voice->Desc.Loop = looping;
            ma_sound_set_looping(&voice->Sound, looping);
            auto state   = voice->Current();
            state.Frame  = GetClockFrames();
            state.Source          = state.Position(static_cast<double>(state.Frame), voice->StreamDelay.load());
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Start  = std::max(state.Start, state.Frame);
            state.Loop   = looping;
            voice->Record(state, GetPlaybackClockFrames());
        }
    }
    void AudioEngine::SeekFrames(AudioVoiceHandle handle, uint64_t frame)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            auto state = voice->Current();
            frame      = std::min(frame, voice->Info.LengthFrames);
            if (state.Loop && frame >= state.LoopEnd)
                frame = state.LoopBegin + (frame - state.LoopBegin) % (state.LoopEnd - state.LoopBegin);
            const bool prepare = voice->Source->Stream && !voice->Source->Offline;
            if (prepare)
            {
                m_Impl->CancelSchedule(*voice);
                ma_node_detach_output_bus(&voice->Sound, 0);
                if (voice->Scheduled)
                    ma_sound_stop(&voice->Sound);
                voice->PendingSeek.store(NoFrame);
                voice->Sound.seekTarget = NoFrame;
                voice->Sound.atEnd      = MA_FALSE;
                voice->Sound.Seek(frame);
                m_Impl->WakeStreams();
                ma_node_attach_output_bus(&voice->Sound, 0, m_Impl->VoiceBusInput(*m_Impl->Buses, voice->Desc.Bus), 0);
            }
            state.Frame  = prepare ? m_Impl->WritableClock() : GetClockFrames();
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Source = static_cast<double>(frame);
            state.Start  = std::max(state.Start, state.Frame);
            voice->Record(state, GetPlaybackClockFrames());
            if (prepare && voice->Scheduled)
                m_Impl->Queue(*voice);
            else if (!prepare && voice->Timed.load())
                voice->PendingSeek.store(frame);
            else if (!prepare)
                ma_sound_seek_to_pcm_frame(&voice->Sound, frame);
        }
    }
    uint64_t AudioEngine::GetCursorFrames(AudioVoiceHandle handle) const
    {
        ma_uint64 cursor = 0;
        if (auto* voice = m_Impl->Find(handle))
        {
            if (const auto seek = voice->PendingSeek.load(); seek != NoFrame)
                return seek;
            ma_sound_get_cursor_in_pcm_frames(&voice->Sound, &cursor);
        }
        return cursor;
    }

    uint64_t AudioEngine::GetVoicePositionFrames(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice ? static_cast<uint64_t>(voice->Position(GetPlaybackClockFrames())) : 0;
    }

    double AudioEngine::GetVoicePositionSeconds(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice ? voice->Position(GetPlaybackClockFrames()) / voice->Info.SampleRate : 0;
    }
    void AudioEngine::FadeTo(AudioVoiceHandle handle, float targetVolume, float seconds)
    {
        if (auto* voice = m_Impl->Find(handle); voice && std::isfinite(targetVolume) && std::isfinite(seconds))
            ma_sound_set_fade_in_pcm_frames(&voice->Sound, -1, std::max(0.f, targetVolume),
                static_cast<uint64_t>(std::max(0.f, seconds) * GetSampleRate()));
    }
    void AudioEngine::SetLoopRegion(AudioVoiceHandle handle, uint64_t beginFrame, uint64_t endFrame)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            const auto length = voice->Info.LengthFrames;
            const auto end = endFrame == 0 ? length : std::min(endFrame, length);
            if (beginFrame < end)
            {
                // Detaching waits for a current mixer read; only this source owns the loop points, not the stream worker.
                ma_node_detach_output_bus(&voice->Sound, 0);
                ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(&voice->Sound), beginFrame, end);
                ma_uint64 cursor = 0;
                ma_sound_get_cursor_in_pcm_frames(&voice->Sound, &cursor);
                // A shortened loop may put the prefetched source cursor beyond its new end.
                // Queue a public seek so that only the current processing block can retain old loop samples.
                if (ma_sound_is_looping(&voice->Sound) && cursor >= end)
                    ma_sound_seek_to_pcm_frame(&voice->Sound, beginFrame);
                ma_node_attach_output_bus(&voice->Sound, 0, m_Impl->VoiceBusInput(*m_Impl->Buses, voice->Desc.Bus), 0);
                auto state      = voice->Current();
                state.Frame     = GetClockFrames();
                const auto delay      = voice->StreamDelay.load();
                state.Source          = state.Position(static_cast<double>(state.Frame), delay);
                state.StreamDelayBase = delay;
                state.Start     = std::max(state.Start, state.Frame);
                state.LoopBegin = beginFrame;
                state.LoopEnd   = end;
                voice->Record(state, GetPlaybackClockFrames());
            }
        }
    }
    void AudioEngine::SetSpatialParams(AudioVoiceHandle handle, const AudioSpatialParams& params)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || !voice->Desc.Spatial)
            return;
        auto* sound = &voice->Sound;
        // Miniaudio's None bypasses panning, cones and Doppler as well as distance gain.
        // Inverse with zero rolloff retains spatialization while disabling only distance attenuation.
        ma_attenuation_model model = ma_attenuation_model_inverse;
        switch (params.Attenuation)
        {
            case AudioAttenuationModel::None: break;
            case AudioAttenuationModel::Inverse: model = ma_attenuation_model_inverse; break;
            case AudioAttenuationModel::Linear: model = ma_attenuation_model_linear; break;
            case AudioAttenuationModel::Exponential: model = ma_attenuation_model_exponential; break;
        }
        ma_sound_set_positioning(sound, ma_positioning_absolute);
        ma_sound_set_position(sound, params.Position.x, params.Position.y, params.Position.z);
        // 0.11.25's public pin setter rejects MA_LISTENER_INDEX_CLOSEST, so select through the public query.
        ma_sound_set_pinned_listener_index(sound, ma_engine_find_closest_listener(&m_Impl->Engine,
            params.Position.x, params.Position.y, params.Position.z));
        ma_sound_set_velocity(sound, params.Velocity.x, params.Velocity.y, params.Velocity.z);
        const auto direction = glm::dot(params.Direction, params.Direction) > 0 ?
            glm::normalize(params.Direction) : glm::vec3(0, 0, -1);
        ma_sound_set_direction(sound, direction.x, direction.y, direction.z);
        ma_sound_set_attenuation_model(sound, model);
        ma_sound_set_min_distance(sound, params.MinDistance);
        ma_sound_set_max_distance(sound, params.MaxDistance);
        ma_sound_set_rolloff(sound, params.Attenuation == AudioAttenuationModel::None ? 0.f : params.Rolloff);
        ma_sound_set_doppler_factor(sound, params.DopplerFactor);
        constexpr float radiansPerDegree = 3.14159265358979323846f / 180.f;
        ma_sound_set_cone(sound, params.ConeInnerAngle * radiansPerDegree,
            params.ConeOuterAngle * radiansPerDegree, params.ConeOuterGain);
    }
    void AudioEngine::SetListener(uint32_t index, const AudioListenerState& state)
    {
        if (!IsInitialized() || index >= m_Impl->Config.MaxListeners)
            return;
        auto* engine = &m_Impl->Engine;
        ma_engine_listener_set_position(engine, index, state.Position.x, state.Position.y, state.Position.z);
        ma_engine_listener_set_velocity(engine, index, state.Velocity.x, state.Velocity.y, state.Velocity.z);
        ma_engine_listener_set_direction(engine, index, state.Forward.x, state.Forward.y, state.Forward.z);
        ma_engine_listener_set_world_up(engine, index, state.Up.x, state.Up.y, state.Up.z);
        for (auto& slot : m_Impl->Slots)
        {
            if (slot.Value && slot.Value->Desc.Spatial)
            {
                auto* sound = &slot.Value->Sound;
                const auto position = ma_sound_get_position(sound);
                ma_sound_set_pinned_listener_index(sound,
                    ma_engine_find_closest_listener(engine, position.x, position.y, position.z));
            }
        }
    }
}
