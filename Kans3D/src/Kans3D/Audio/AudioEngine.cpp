#include "kspch.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/Audio/AudioMusicPlayer.h"
#include "Kans3D/Core/Log/Log.h"
#include "AudioBusGraph.h"

#include <miniaudio.h>
#include <array>
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
#include <utility>
#include <glm/geometric.hpp>

namespace Kans
{
    namespace
    {
        constexpr auto NoFrame = std::numeric_limits<uint64_t>::max();
        constexpr float RealVoiceMargin  = 1.12201845f; // 1 dB in linear amplitude.
        constexpr float RadiansPerDegree = 3.14159265358979323846f / 180.f;
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
            std::atomic<bool>               Producing {false};

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
                if (!Offline && !Producing.load(std::memory_order_acquire))
                    return;
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
                    while (room != 0 && (Offline || Producing.load(std::memory_order_relaxed)))
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
                uint64_t LoopBegin = 0, LoopEnd = 0, Length = 0;
                bool     Loop = false;

                double Position(double clock, uint64_t streamDelay = 0) const
                {
                    const auto delay   = std::min(streamDelay, StreamDelayEnd);
                    const auto waited  = delay - std::min(delay, StreamDelayBase);
                    const auto elapsed = std::max(0.0, std::min(clock, static_cast<double>(Stop)) - Start - waited);
                    return WrapPosition(Source + elapsed * Rate, LoopBegin, Loop ? LoopEnd : Length, Loop);
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
                VoiceEnvelope                User;

                float WindowGain(uint64_t frame) const
                {
                    const float in = In.Gain(frame);
                    if (!Out || frame < Out->Start)
                        return in;
                    return Multiply ? in * Out->Gain(frame) : Out->Gain(frame);
                }

                float Gain(uint64_t frame) const { return WindowGain(frame) * User.Gain(frame); }
            };

            // The mixer owns this state while attached. The main thread prepares it after detaching.
            // The inherited ma_sound supplies miniaudio's volume, pan and spatial DSP.
            struct Node : ma_sound
            {
                Voice*                Owner    = nullptr;
                const ma_node_vtable* Original = nullptr;
                uint64_t              Block = NoFrame, Next = 0;
                Fades                 Envelope;
                uint32_t              ResumeLength = 0, ResumeRemaining = 0;
                bool                  FractionalSeek = false;
                uint64_t              ResumeAt       = NoFrame;
                Transport             ResumeTransport;

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
                        const auto pending = voice.PendingSeek.exchange(NoFrame);
                        if (pending != NoFrame)
                            self.Seek(pending);
                        else if (self.ResumeAt != NoFrame && self.Next > self.ResumeAt)
                            self.Seek(self.ResumeTransport.Position(static_cast<double>(self.Next),
                                                                    voice.StreamDelay.load()));
                        self.ResumeAt = NoFrame;
                        const bool bypass =
                            !voice.Desc.Spatial &&
                            self.engineNode.sampleRate == ma_engine_get_sample_rate(ma_sound_get_engine(&self)) &&
                            ma_sound_get_pitch(&self) == 1 && !self.FractionalSeek;
                        if (self.engineNode.isPitchDisabled != bypass)
                        {
                            self.engineNode.isPitchDisabled = bypass;
                            self.ResetResampler();
                        }
                    }
                    const auto requested = *outCount;
                    self.Original->onProcess(node, in, inCount, out, outCount);
                    if (*outCount < requested && voice.Source->Stream && !voice.Source->Offline &&
                        !ma_sound_at_end(&self))
                        voice.StreamDelay.fetch_add(requested - *outCount, std::memory_order_relaxed);
                    const auto channels = ma_node_get_output_channels(node, 0);
                    if (self.ResumeRemaining || self.Envelope.Out || self.Envelope.In.From != 1 ||
                        self.Envelope.In.To != 1 || self.Envelope.User.From != 1 || self.Envelope.User.To != 1)
                        for (uint32_t i = 0; i < *outCount; ++i)
                        {
                            float gain = self.Envelope.Gain(self.Next + i);
                            if (self.ResumeRemaining)
                            {
                                gain *=
                                    static_cast<float>(self.ResumeLength - self.ResumeRemaining) / self.ResumeLength;
                                --self.ResumeRemaining;
                            }
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

                void Seek(double position)
                {
                    const auto frame = static_cast<uint64_t>(position);
                    ma_data_source_seek_to_pcm_frame(pDataSource, frame);
                    processingCacheFramesRemaining = 0;
                    ResetResampler();
                    auto& resampler      = *static_cast<ma_linear_resampler*>(engineNode.resampler.pBackend);
                    FractionalSeek       = position != frame;
                    resampler.inTimeFrac = static_cast<ma_uint32>((position - frame) * resampler.config.sampleRateOut);
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
            AudioSpatialParams           Spatial;
            float                        ConeInnerCos = -1, ConeOuterCos = -1;
            AudioClipRegistry*           OneShotRegistry = nullptr;
            AssetID                      OneShotClip, EventGroup;
            uint64_t                     PlayOrder = 0, PinnedStart = NoFrame;
            bool                         Attached = false, Virtual = false, CountedReal = false, Stolen = false;
            bool                         Poolable = false;
            uint64_t                 PausedAt = NoFrame, RemainingStart = 0, RemainingStop = NoFrame;
            bool                     Initialized = false, Scheduled = false;
            ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }

            const Transport& Current() const { return History.back(); }

            bool Ended(double clock) const
            {
                const auto& state = Current();
                if (state.Start == NoFrame)
                    return false;
                return Virtual ? !state.Loop && state.Position(clock, StreamDelay.load()) >= Info.LengthFrames :
                                 ma_sound_at_end(&Sound) != MA_FALSE;
            }

            bool Active(uint64_t clock) const
            {
                const auto& state = Current();
                return state.Start != NoFrame && state.Stop > clock && !Ended(static_cast<double>(clock));
            }

            bool Protected(uint64_t clock) const
            {
                return Desc.NeverVirtualize || Stolen || (PinnedStart != NoFrame && clock <= PinnedStart);
            }
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
        struct Sound
        {
            AudioSoundInfo                                     Info {};
            std::shared_ptr<const std::vector<std::byte>>      Encoded;
            std::vector<float>                                 PCM;
            std::string                                        EncodedName, DecodedName;
            bool                                               DecodedRegistered = false;
            std::array<std::vector<std::unique_ptr<Voice>>, 2> OneShots;
        };
        struct Slot
        {
            uint32_t Generation = 1;
            std::unique_ptr<Voice> Value;
            uint32_t               NextFree = UINT32_MAX;
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
            if (voice.Virtual)
                return;
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
        uint32_t                                     FreeSlot = UINT32_MAX, RealCount = 0;
        size_t                                       PooledOneShots = 0;
        std::vector<AudioClipRegistry*> ClipRegistries;
        std::array<AudioListenerState, 4>            Listeners;
        std::map<std::string, AudioConcurrencyGroup> Groups;
        std::map<AssetID, AudioConcurrencyGroup>     EventGroups;
        struct ConcurrencyVictim
        {
            AudioVoiceHandle Handle;
            float            ReleaseSeconds;
        };
        std::vector<ConcurrencyVictim>               ConcurrencyVictims;
        AudioVoiceStats                              Stats;
        uint64_t                                     PlaySequence = 0, PendingSteals = 0;
        struct Candidate
        {
            Voice* Instance;
            float  Score;
            bool   Eligible;
        };
        std::vector<Candidate> Candidates;

        void CountReal(Voice& voice, bool real)
        {
            if (voice.CountedReal == real)
                return;
            voice.CountedReal = real;
            if (real)
                ++RealCount;
            else
                --RealCount;
        }

        void SetStreamActive(Voice& voice, bool active)
        {
            auto& source = *voice.Source;
            if (!source.Stream || source.Offline || source.Producing.exchange(active) == active)
                return;
            {
                std::lock_guard<std::mutex> lock(StreamMutex);
                if (active)
                    DeviceStreams.push_back(voice.Source);
                else
                    DeviceStreams.erase(std::find(DeviceStreams.begin(), DeviceStreams.end(), voice.Source));
            }
            WakeStreams();
        }

        bool Attach(Voice& voice)
        {
            if (voice.Attached)
                return true;
            const auto result = ma_node_attach_output_bus(&voice.Sound, 0, VoiceBusInput(*Buses, voice.Desc.Bus), 0);
            if (result != MA_SUCCESS)
            {
                Warn(Error("Attach voice", result));
                return false;
            }
            voice.Attached = true;
            return true;
        }

        void Detach(Voice& voice)
        {
            if (voice.Attached)
            {
                ma_node_detach_output_bus(&voice.Sound, 0);
                voice.Attached = false;
            }
        }

        float Audibility(const Voice& voice, uint64_t clock)
        {
            const float gain = voice.Desc.Volume * voice.Envelope.Gain(clock) * Buses->AudibilityGain(voice.Desc.Bus) *
                               ma_engine_get_volume(&Engine);
            if (std::isnan(gain))
                return 0;
            if (!voice.Desc.Spatial || gain == 0)
                return gain;
            const auto& spatial  = voice.Spatial;
            auto        relative = Listeners[0].Position - spatial.Position;
            auto        squared  = glm::dot(relative, relative);
            for (uint32_t i = 1; i < Config.MaxListeners; ++i)
            {
                const auto delta           = Listeners[i].Position - spatial.Position;
                const auto distanceSquared = glm::dot(delta, delta);
                if (distanceSquared < squared)
                {
                    squared  = distanceSquared;
                    relative = delta;
                }
            }
            const float distance    = std::sqrt(squared);
            float       attenuation = 1;
            const auto  min = spatial.MinDistance, max = spatial.MaxDistance;
            if (min < max)
            {
                const auto d = std::clamp(distance, min, max);
                switch (spatial.Attenuation)
                {
                    case AudioAttenuationModel::None:
                        break;
                    case AudioAttenuationModel::Inverse:
                        attenuation = min / (min + spatial.Rolloff * (d - min));
                        break;
                    case AudioAttenuationModel::Linear:
                        attenuation = 1 - spatial.Rolloff * (d - min) / (max - min);
                        break;
                    case AudioAttenuationModel::Exponential:
                        attenuation = std::pow(d / min, -spatial.Rolloff);
                        break;
                }
            }
            if (distance > 0.001f && spatial.ConeInnerAngle < 360)
            {
                const auto dot = glm::dot(spatial.Direction, relative) / distance;
                if (dot <= voice.ConeOuterCos)
                    attenuation *= spatial.ConeOuterGain;
                else if (dot < voice.ConeInnerCos)
                    attenuation *= spatial.ConeOuterGain + (1 - spatial.ConeOuterGain) * (dot - voice.ConeOuterCos) /
                                                               (voice.ConeInnerCos - voice.ConeOuterCos);
            }
            return std::isfinite(attenuation) && attenuation > 0 ? gain * std::min(attenuation, 1.f) : 0;
        }

        void Virtualize(Voice& voice)
        {
            CancelSchedule(voice);
            Detach(voice);
            ma_sound_stop(&voice.Sound);
            SetStreamActive(voice, false);
            voice.Virtual = true;
            CountReal(voice, false);
        }

        bool Realize(Voice& voice, uint64_t clock)
        {
            const auto& state    = voice.Current();
            auto&       sound    = voice.Sound;
            const auto  start    = std::max(clock, state.Start);
            const auto  position = state.Position(static_cast<double>(start), voice.StreamDelay.load());
            if (start >= state.Stop || (!state.Loop && position >= voice.Info.LengthFrames))
                return false;
            voice.PendingSeek.store(NoFrame);
            voice.Timed.store(true);
            sound.seekTarget                 = NoFrame;
            sound.atEnd                      = MA_FALSE;
            sound.engineNode.isPitchDisabled = !voice.Desc.Spatial &&
                                               voice.Info.SampleRate == ma_engine_get_sample_rate(&Engine) &&
                                               voice.Desc.Pitch == 1 && position == std::floor(position);
            sound.Seek(position);
            sound.Block        = NoFrame;
            sound.Envelope     = voice.Envelope;
            sound.ResumeLength = sound.ResumeRemaining = std::max(1u, ma_engine_get_sample_rate(&Engine) / 200);
            if (Config.Mode == AudioOutputMode::Device)
            {
                // A device callback may advance between preparing the seek and attaching the node.
                sound.ResumeAt        = start;
                sound.ResumeTransport = state;
            }
            ma_sound_set_start_time_in_pcm_frames(&sound, start);
            ma_sound_set_stop_time_in_pcm_frames(&sound, state.Stop);
            ma_node_set_state(&sound, ma_node_state_started);
            if (!Attach(voice))
                return false;
            voice.Virtual = false;
            CountReal(voice, true);
            SetStreamActive(voice, true);
            return true;
        }

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

    AudioEngine::AudioEngine() : m_Impl(std::make_unique<Impl>()), m_EventPlayer(new AudioEventPlayer(*this)) {}

    AudioEventPlayer&       AudioEngine::GetEventPlayer() { return *m_EventPlayer; }
    const AudioEventPlayer& AudioEngine::GetEventPlayer() const { return *m_EventPlayer; }
    AudioEngine::~AudioEngine() { Shutdown(); }

    std::optional<std::string> AudioEngine::Init(const AudioEngineConfig& config)
    {
        Shutdown();
        auto& p = *m_Impl;
        p.Config = config;
        if (config.MaxListeners < 1 || config.MaxListeners > 4 || config.Channels == 0 ||
            config.Channels > MA_MAX_CHANNELS || (config.Mode == AudioOutputMode::Offline && config.SampleRate == 0) ||
            !std::isfinite(config.VirtualizationThreshold) || config.VirtualizationThreshold < 0)
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
        p.Listeners      = {};
        p.Groups.clear();
        p.EventGroups.clear();
        p.ConcurrencyVictims.clear();
        m_EventPlayer->Clear();
        p.Stats        = {};
        p.PlaySequence = p.PendingSteals = 0;
        p.Candidates.clear();
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
        auto& p = *m_Impl;
        p.DrainJobs();
        p.CollectSchedules();
        const auto now          = GetClockFrames();
        const auto heard        = GetPlaybackClockFrames();
        p.Stats.StolenThisFrame = std::exchange(p.PendingSteals, 0);
        p.Stats.AllocatedVoices = p.Stats.VirtualVoices = 0;
        for (auto& [name, count] : p.Stats.GroupActiveCounts)
            count = 0;
        p.Candidates.clear();
        uint32_t protectedCount = 0;
        for (uint32_t i = 0; i < p.Slots.size(); ++i)
        {
            const auto& slot = p.Slots[i];
            if (!slot.Value)
                continue;
            auto&       voice = *slot.Value;
            const auto& state = voice.Current();
            if (voice.Desc.AutoRelease && ((voice.Ended(heard) && (voice.Virtual || !voice.Timed.load() ||
                                                                   voice.Position(heard) >= voice.Info.LengthFrames)) ||
                                           (state.Stop != NoFrame && heard >= state.Stop)))
            {
                DestroyVoice({i, slot.Generation});
                continue;
            }
            ++p.Stats.AllocatedVoices;
            if (!voice.Active(now))
            {
                p.CountReal(voice, false);
                p.SetStreamActive(voice, false);
                continue;
            }
            if (!voice.Stolen && !voice.Desc.ConcurrencyGroup.empty())
                if (auto group = p.Stats.GroupActiveCounts.find(voice.Desc.ConcurrencyGroup);
                    group != p.Stats.GroupActiveCounts.end())
                    ++group->second;
            if (voice.Protected(now))
            {
                ++protectedCount;
                continue;
            }
            const auto audibility = p.Audibility(voice, now);
            const auto score      = voice.Virtual ? audibility : audibility * RealVoiceMargin;
            p.Candidates.push_back({&voice, score, score >= p.Config.VirtualizationThreshold});
        }
        const auto budget   = p.Config.MaxRealVoices - std::min(protectedCount, p.Config.MaxRealVoices);
        const auto eligible = static_cast<size_t>(
            std::count_if(p.Candidates.begin(), p.Candidates.end(), [](const auto& c) { return c.Eligible; }));
        if (budget != 0 && eligible > budget)
            std::nth_element(p.Candidates.begin(),
                             p.Candidates.begin() + budget,
                             p.Candidates.end(),
                             [](const auto& a, const auto& b) {
                                 if (a.Eligible != b.Eligible)
                                     return a.Eligible > b.Eligible;
                                 if (a.Instance->Desc.Priority != b.Instance->Desc.Priority)
                                     return a.Instance->Desc.Priority > b.Instance->Desc.Priority;
                                 if (a.Score != b.Score)
                                     return a.Score > b.Score;
                                 return a.Instance->PlayOrder < b.Instance->PlayOrder;
                             });
        size_t selected = 0;
        for (auto& candidate : p.Candidates)
        {
            candidate.Eligible = candidate.Eligible && selected < budget;
            if (candidate.Eligible)
                ++selected;
            else
            {
                if (!candidate.Instance->Virtual)
                    p.Virtualize(*candidate.Instance);
                ++p.Stats.VirtualVoices;
            }
        }
        const auto resume = p.WritableClock();
        for (const auto& candidate : p.Candidates)
            if (candidate.Eligible && candidate.Instance->Virtual && !p.Realize(*candidate.Instance, resume))
                ++p.Stats.VirtualVoices;
        p.Stats.RealVoices = p.RealCount;
    }

    void AudioEngine::SetMaxRealVoices(uint32_t count) { m_Impl->Config.MaxRealVoices = count; }

    void AudioEngine::SetVirtualizationThreshold(float linear)
    {
        if (std::isfinite(linear) && linear >= 0)
            m_Impl->Config.VirtualizationThreshold = linear;
    }

    const AudioVoiceStats& AudioEngine::GetVoiceStats() const { return m_Impl->Stats; }

    bool AudioEngine::DefineConcurrencyGroup(const std::string& name, const AudioConcurrencyGroup& group)
    {
        if (!IsInitialized() || name.empty() || !std::isfinite(group.ReleaseSeconds) || group.ReleaseSeconds < 0 ||
            group.Rule < AudioConcurrencyRule::StopOldest || group.Rule > AudioConcurrencyRule::RejectNew)
            return false;
        m_Impl->Groups.insert_or_assign(name, group);
        m_Impl->Stats.GroupActiveCounts.try_emplace(name, 0);
        return true;
    }

    void AudioEngine::DefineEventGroup(AssetID id, const AudioConcurrencyGroup& group)
    {
        m_Impl->EventGroups.insert_or_assign(id, group);
    }

    uint32_t AudioEngine::CountEventInstances(AssetID id) const
    {
        const auto now = GetClockFrames();
        return static_cast<uint32_t>(std::count_if(m_Impl->Slots.begin(), m_Impl->Slots.end(), [&](const auto& slot) {
            return slot.Value && slot.Value->EventGroup == id && !slot.Value->Stolen && slot.Value->Active(now);
        }));
    }

    bool AudioEngine::IsVoiceReleasing(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice && voice->Stolen;
    }

    bool AudioEngine::IsVoiceVirtual(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice && voice->Virtual && voice->Active(GetClockFrames());
    }

    void AudioEngine::SetVoicePriority(AudioVoiceHandle handle, uint8_t priority)
    {
        if (auto* voice = m_Impl->Find(handle))
            voice->Desc.Priority = priority;
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
            if (!slot.Value || !slot.Value->Attached)
                continue;
            auto& voice = *slot.Value;
            result      = ma_node_attach_output_bus(&voice.Sound, 0, p.VoiceBusInput(*next, voice.Desc.Bus), 0);
            if (result != MA_SUCCESS)
            {
                for (const auto& moved : p.Slots)
                {
                    if (&moved == &slot)
                        break;
                    if (moved.Value && moved.Value->Attached)
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
        auto entry = m_Impl->Sounds.extract(id);
        if (entry.empty())
            return;
        for (uint32_t i = 0; i < m_Impl->Slots.size(); ++i)
        {
            auto& slot = m_Impl->Slots[i];
            if (slot.Value && slot.Value->Desc.Sound == id)
                DestroyVoice({i, slot.Generation});
        }
        auto& sound = entry.mapped();
        for (auto& pool : sound.OneShots)
        {
            m_Impl->PooledOneShots -= pool.size();
            pool.clear();
        }
        if (sound.DecodedRegistered)
            ma_resource_manager_unregister_data(&m_Impl->Resources, sound.DecodedName.c_str());
        if (sound.Encoded)
        {
            std::lock_guard<std::mutex> lock(m_Impl->VFS.Mutex);
            m_Impl->VFS.Files.erase(sound.EncodedName);
        }
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

    AudioVoiceHandle
    AudioEngine::CreateVoice(const AudioVoiceDesc& desc, uint64_t loopStart, uint64_t loopEnd, bool oneShot)
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
        const bool                   poolable = oneShot && !stream;
        auto&                        pool     = sound.OneShots[desc.Spatial];
        std::unique_ptr<Impl::Voice> voice;
        if (poolable && !pool.empty())
        {
            voice = std::move(pool.back());
            pool.pop_back();
            --m_Impl->PooledOneShots;
        }
        else
            voice = std::make_unique<Impl::Voice>();
        voice->Desc = desc;
        voice->Desc.Volume = voice->Desc.Pitch = 1;
        voice->Desc.Pan                        = 0;
        voice->Info = sound.Info;
        voice->Poolable                        = poolable;
        Impl::Voice::Transport transport;
        transport.Frame     = GetClockFrames();
        transport.Loop      = desc.Loop;
        transport.LoopBegin = loopStart;
        transport.LoopEnd   = loopEnd != 0 ? loopEnd : sound.Info.LengthFrames;
        transport.Length    = sound.Info.LengthFrames;
        if (voice->Initialized)
        {
            voice->History.resize(1);
            voice->History.front() = transport;
            voice->Virtual = voice->Stolen = voice->Scheduled = false;
            voice->PausedAt = voice->PinnedStart = NoFrame;
            voice->Timed.store(false);
            voice->PendingSeek.store(NoFrame);
            voice->Envelope   = {};
            auto& node        = voice->Sound;
            node.atEnd        = MA_FALSE;
            node.seekTarget   = NoFrame;
            node.Envelope     = {};
            node.ResumeLength = node.ResumeRemaining = 0;
            node.ResumeAt                            = NoFrame;
            node.Block                               = NoFrame;
            node.engineNode.isPitchDisabled          = false;
            ma_sound_set_looping(&node, desc.Loop);
            ma_data_source_set_loop_point_in_pcm_frames(voice->Source.get(), 0, sound.Info.LengthFrames);
            node.Seek(0);
            ma_resampler_reset(&node.engineNode.resampler);
            auto& spatialGain = node.engineNode.spatializer.gainer;
            spatialGain.t     = UINT32_MAX;
            std::fill_n(spatialGain.pOldGains, spatialGain.config.channels, 1.f);
            std::fill_n(spatialGain.pNewGains, spatialGain.config.channels, 1.f);
            node.engineNode.spatializer.dopplerPitch = 1;
        }
        else
        {
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
            auto config        = ma_sound_config_init_2(&m_Impl->Engine);
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
            voice->Initialized                 = true;
            auto& node                         = voice->Sound;
            node.Owner                         = voice.get();
            node.Original                      = node.engineNode.baseNode.vtable;
            static const ma_node_vtable vtable = {Impl::Voice::Node::Process, nullptr, 0, 1, 0};
            node.engineNode.baseNode.vtable    = &vtable;
            ma_sound_set_pan_mode(&node, ma_pan_mode_balance);
        }
        voice->Commands->Owner.store(voice.get());
        uint32_t index = m_Impl->FreeSlot;
        if (index == UINT32_MAX)
        {
            index = static_cast<uint32_t>(m_Impl->Slots.size());
            if (index == UINT32_MAX)
                return {};
            m_Impl->Slots.emplace_back();
        }
        auto& slot       = m_Impl->Slots[index];
        m_Impl->FreeSlot = slot.NextFree;
        slot.Value = std::move(voice);
        AudioVoiceHandle handle{index, slot.Generation};
        SetVolume(handle, desc.Volume);
        SetPitch(handle, desc.Pitch);
        SetPan(handle, desc.Pan);
        if (desc.Spatial)
        {
            SetSpatialParams(handle, {});
        }
        return handle;
    }

    AudioVoiceHandle AudioEngine::PlayOneShot(AudioClipRegistry&                           registry,
                                              const std::shared_ptr<const AudioClipAsset>& clip,
                                              const AudioOneShotParams&                    params)
    {
        AudioVoiceDesc desc;
        desc.Volume           = params.Volume;
        desc.Pitch            = params.Pitch;
        desc.Bus              = params.Bus;
        desc.Priority         = params.Priority;
        desc.ConcurrencyGroup = params.ConcurrencyGroup;
        AudioSpatialParams spatial;
        if (params.Position)
            spatial.Position = *params.Position;
        return PlayRegisteredOneShot(registry, clip, desc, params.Position ? &spatial : nullptr, 0, nullptr);
    }

    AudioVoiceHandle AudioEngine::PlayRegisteredOneShot(AudioClipRegistry&                           registry,
                                                        const std::shared_ptr<const AudioClipAsset>& clip,
                                                        AudioVoiceDesc&                              desc,
                                                        const AudioSpatialParams*                    spatial,
                                                        double                                       startOffsetSeconds,
                                                        bool*                                        rejectedByLimit,
                                                        AssetID                                      eventGroup)
    {
        const auto sound = registry.Acquire(*this, clip);
        if (!sound)
            return {};
        desc.Sound    = *sound;
        desc.LoadMode = clip->GetData().LoadMode;
        desc.Volume *= clip->GetData().Volume;
        desc.Spatial      = spatial != nullptr;
        desc.AutoRelease  = true;
        const auto handle = CreateVoice(desc, 0, 0, true);
        auto*      voice  = m_Impl->Find(handle);
        if (!voice)
        {
            registry.Release(clip->assetID);
            return {};
        }
        voice->OneShotRegistry = &registry;
        voice->OneShotClip     = clip->assetID;
        voice->EventGroup      = eventGroup;
        if (spatial)
            SetSpatialParams(handle, *spatial);
        if (desc.Loop)
            SetLoopRegion(handle, clip->GetData().LoopBegin, clip->GetData().LoopEnd);
        if (startOffsetSeconds > 0 && voice->Info.LengthFrames > 0)
        {
            // Keep an out-of-range offset on a playable source frame, including an authored loop region.
            const auto   end    = desc.Loop ? voice->Current().LoopEnd : voice->Info.LengthFrames;
            const double offset = std::round(startOffsetSeconds * voice->Info.SampleRate);
            SeekFrames(handle, offset >= end ? end - 1 : static_cast<uint64_t>(offset));
        }
        if (PlayVoice(handle, rejectedByLimit))
            return handle;
        DestroyVoice(handle);
        return {};
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
        if (voice->Attached && input != m_Impl->VoiceBusInput(graph, voice->Desc.Bus))
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
        auto& p = *m_Impl;
        if (!p.Find(handle))
            return;
        auto& slot  = p.Slots[handle.Index];
        auto  voice = std::move(slot.Value);
        voice->Commands->Owner.store(nullptr);
        p.CancelSchedule(*voice);
        p.Detach(*voice);
        ma_sound_stop(&voice->Sound);
        p.SetStreamActive(*voice, false);
        p.CountReal(*voice, false);
        // Retire a slot permanently on generation wrap so even ancient handles cannot become live again.
        if (++slot.Generation != 0)
        {
            slot.NextFree = p.FreeSlot;
            p.FreeSlot    = handle.Index;
        }
        auto*      registry = std::exchange(voice->OneShotRegistry, nullptr);
        const auto clip     = voice->OneShotClip;
        // Keep warmed resident nodes while their sound is registered. Streams own distinct decoder state.
        if (voice->Poolable && p.PooledOneShots < 128)
            if (auto sound = p.Sounds.find(voice->Desc.Sound); sound != p.Sounds.end())
            {
                auto& pool = sound->second.OneShots[voice->Desc.Spatial];
                pool.push_back(std::move(voice));
                ++p.PooledOneShots;
            }
        voice.reset();
        if (registry)
            registry->Release(clip);
    }
    bool AudioEngine::IsVoiceValid(AudioVoiceHandle handle) const { return m_Impl->Find(handle) != nullptr; }

    bool AudioEngine::AdmitVoice(AudioVoiceHandle handle, uint64_t now)
    {
        auto&      p     = *m_Impl;
        auto&      voice = *p.Find(handle);
        auto&      victims = p.ConcurrencyVictims;
        victims.clear();
        // Plan both groups before touching any voice. Local victims also free space in the shared group.
        const auto plan = [&](const AudioConcurrencyGroup& definition, const auto& belongs) {
            for (;;)
            {
                size_t           count = 0;
                AudioVoiceHandle victim;
                uint64_t         oldest = NoFrame;
                float            worst  = std::numeric_limits<float>::infinity();
                for (uint32_t i = 0; i < p.Slots.size(); ++i)
                {
                    const auto& slot = p.Slots[i];
                    if (!slot.Value || i == handle.Index || slot.Value->Stolen || !belongs(*slot.Value) ||
                        !slot.Value->Active(now) ||
                        std::any_of(victims.begin(), victims.end(), [i](const auto& v) { return v.Handle.Index == i; }))
                        continue;
                    ++count;
                    const auto& member = *slot.Value;
                    float       score  = 0;
                    if (definition.Rule == AudioConcurrencyRule::StopQuietest)
                        score = p.Audibility(member, now);
                    else if (definition.Rule == AudioConcurrencyRule::StopLowestPriority)
                        score = member.Desc.Priority;
                    if (score < worst || (score == worst && member.PlayOrder < oldest))
                    {
                        oldest = member.PlayOrder;
                        worst  = score;
                        victim = {i, slot.Generation};
                    }
                }
                if (count < definition.MaxCount)
                    return true;
                if (!victim.IsValid() || definition.Rule == AudioConcurrencyRule::RejectNew || definition.MaxCount == 0)
                    return false;
                victims.push_back({victim, definition.ReleaseSeconds});
            }
        };
        const auto event  = p.EventGroups.find(voice.EventGroup);
        const auto shared = p.Groups.find(voice.Desc.ConcurrencyGroup);
        if ((event != p.EventGroups.end() &&
             !plan(event->second, [&](const auto& member) { return member.EventGroup == event->first; })) ||
            (shared != p.Groups.end() &&
             !plan(shared->second, [&](const auto& member) { return member.Desc.ConcurrencyGroup == shared->first; })))
        {
            ++p.Stats.RejectedStarts;
            return false;
        }
        for (const auto& victim : victims)
        {
            auto& stolen  = *p.Find(victim.Handle);
            stolen.Stolen = true;
            ++p.PendingSteals;
            ++p.Stats.TotalStolen;
            const auto release = static_cast<uint64_t>(
                std::min(static_cast<double>(victim.ReleaseSeconds) * GetSampleRate(), double(NoFrame / 2)));
            if (release == 0 || stolen.Virtual || p.WritableClock() <= stolen.Current().Start)
            {
                if (stolen.Desc.AutoRelease)
                    DestroyVoice(victim.Handle);
                else
                    Stop(victim.Handle);
            }
            else
            {
                const auto stop = std::min(stolen.Current().Stop, AddFrames(p.WritableClock(), release));
                ScheduleStop(victim.Handle, stop, release);
            }
        }
        voice.Stolen    = false;
        voice.PlayOrder = ++p.PlaySequence;
        return true;
    }

    bool AudioEngine::Play(AudioVoiceHandle handle) { return PlayVoice(handle, nullptr); }

    bool AudioEngine::PlayVoice(AudioVoiceHandle handle, bool* rejectedByLimit)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            auto       state = voice->Current();
            const auto now   = voice->Scheduled ? m_Impl->WritableClock() : GetClockFrames();
            const bool ended = voice->Ended(static_cast<double>(now)) || ma_sound_at_end(&voice->Sound);
            if (voice->Active(now) && !voice->Stolen)
                return true;
            if (!AdmitVoice(handle, now))
            {
                if (rejectedByLimit)
                    *rejectedByLimit = true;
                return false;
            }
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
                voice->Envelope.User.Start = AddFrames(voice->Envelope.User.Start, delay);
                if (voice->PinnedStart != NoFrame && voice->PinnedStart >= voice->PausedAt)
                    voice->PinnedStart = state.Start;
                voice->PausedAt = NoFrame;
            }
            else
            {
                voice->Envelope.In = {};
                voice->Envelope.Out.reset();
                voice->PinnedStart = NoFrame;
            }
            voice->Record(state, GetPlaybackClockFrames());
            if (!voice->Protected(now) && (m_Impl->RealCount >= m_Impl->Config.MaxRealVoices ||
                                           m_Impl->Audibility(*voice, now) < m_Impl->Config.VirtualizationThreshold))
            {
                m_Impl->Virtualize(*voice);
                return true;
            }
            if (voice->Virtual)
                return m_Impl->Realize(*voice, m_Impl->WritableClock());
            if (!m_Impl->Attach(*voice))
                return false;
            m_Impl->CountReal(*voice, true);
            m_Impl->SetStreamActive(*voice, true);
            if (voice->Scheduled)
            {
                m_Impl->Queue(*voice, ended);
                return true;
            }
            ma_sound_set_start_time_in_pcm_frames(&voice->Sound, state.Start);
            ma_sound_set_stop_time_in_pcm_frames(&voice->Sound, state.Stop);
            ma_sound_start(&voice->Sound);
            return true;
        }
        return false;
    }

    bool AudioEngine::ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame)
    {
        return StartVoice(handle, clockFrame, 0, AudioFadeCurve::Linear) != NoFrame;
    }

    bool AudioEngine::ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame, const AudioVoiceWindow& window)
    {
        return StartVoice(handle, clockFrame, 0, AudioFadeCurve::Linear, &window) != NoFrame;
    }

    uint64_t AudioEngine::StartVoice(AudioVoiceHandle        handle,
                                     uint64_t                clockFrame,
                                     uint64_t                fadeFrames,
                                     AudioFadeCurve          curve,
                                     const AudioVoiceWindow* window)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || !AdmitVoice(handle, GetClockFrames()))
            return NoFrame;
        auto& sound = voice->Sound;
        m_Impl->CancelSchedule(*voice);
        // Detach on the main thread to prepare the decoder, cache and resampler before arming playback.
        m_Impl->Detach(*voice);
        ma_sound_stop(&sound);
        auto       state  = voice->Current();
        const auto now    = m_Impl->WritableClock();
        const auto source = (voice->Ended(static_cast<double>(now)) || ma_sound_at_end(&sound)) ?
                                0 :
                                state.Position(static_cast<double>(now), voice->StreamDelay.load());
        const auto late   = !m_Impl->BuildingSchedule && now > clockFrame ? now - clockFrame : 0;
        const auto frame  = static_cast<uint64_t>(WrapPosition(
            source + late * state.Rate, state.LoopBegin, state.Loop ? state.LoopEnd : state.Length, state.Loop));
        voice->PendingSeek.store(NoFrame);
        sound.seekTarget = NoFrame;
        sound.atEnd      = MA_FALSE;
        sound.Seek(frame);
        sound.Block           = NoFrame;
        sound.ResumeRemaining = 0;
        sound.ResumeAt        = NoFrame;
        state.StreamDelayBase = voice->StreamDelay.load();
        state.Source    = static_cast<double>(frame);
        state.Frame     = now;
        state.Start     = m_Impl->BuildingSchedule ? clockFrame : std::max(m_Impl->WritableClock(), clockFrame);
        state.Stop      = NoFrame;
        voice->PausedAt = NoFrame;
        voice->Timed.store(true);
        voice->PinnedStart = state.Start;
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
        if (!m_Impl->Attach(*voice))
            return NoFrame;
        voice->Virtual = false;
        m_Impl->CountReal(*voice, true);
        m_Impl->SetStreamActive(*voice, true);
        if (voice->Source->Stream && !voice->Source->Offline)
            m_Impl->WakeStreams();
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
                const auto from = voice->Envelope.WindowGain(start);
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
            if (voice->OneShotRegistry)
            {
                DestroyVoice(handle);
                return;
            }
            m_Impl->CancelSchedule(*voice);
            ma_sound_stop(&voice->Sound);
            ma_sound_reset_stop_time(&voice->Sound);
            m_Impl->CountReal(*voice, false);
            m_Impl->SetStreamActive(*voice, false);
            voice->PinnedStart = NoFrame;
            voice->PausedAt = NoFrame;
            auto state      = voice->Current();
            state.Frame     = GetClockFrames();
            state.Start = state.Stop = NoFrame;
            state.Source             = 0;
            voice->Record(state, GetPlaybackClockFrames());
            if (voice->Timed.load())
            {
                voice->Envelope.In = {};
                voice->Envelope.Out.reset();
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
            m_Impl->CountReal(*voice, false);
            m_Impl->SetStreamActive(*voice, false);
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
        const auto* voice = m_Impl->Find(handle);
        const auto  clock = GetClockFrames();
        return voice && voice->Current().Start <= clock && voice->Active(clock);
    }
    bool AudioEngine::HasEnded(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        const auto  clock = GetClockFrames();
        return voice && (voice->Ended(static_cast<double>(clock)) || (voice->Stolen && !voice->Active(clock)));
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
            const auto rate    = static_cast<double>(pitch) * voice->Info.SampleRate / GetSampleRate();
            if (state.Rate == rate)
                return;
            state.Frame  = GetClockFrames();
            state.Source          = state.Position(static_cast<double>(state.Frame), voice->StreamDelay.load());
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Start  = std::max(state.Start, state.Frame);
            state.Rate            = rate;
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
            const bool prepare = voice->Source->Stream && !voice->Source->Offline && !voice->Virtual;
            if (prepare)
            {
                const bool attached = voice->Attached;
                m_Impl->CancelSchedule(*voice);
                m_Impl->Detach(*voice);
                if (voice->Scheduled)
                    ma_sound_stop(&voice->Sound);
                voice->PendingSeek.store(NoFrame);
                voice->Sound.seekTarget = NoFrame;
                voice->Sound.atEnd      = MA_FALSE;
                voice->Sound.ResumeAt   = NoFrame;
                voice->Sound.Seek(frame);
                m_Impl->WakeStreams();
                if (attached)
                    m_Impl->Attach(*voice);
            }
            state.Frame  = prepare ? m_Impl->WritableClock() : GetClockFrames();
            state.StreamDelayBase = voice->StreamDelay.load();
            state.Source = static_cast<double>(frame);
            state.Start  = std::max(state.Start, state.Frame);
            voice->Record(state, GetPlaybackClockFrames());
            if (voice->Virtual)
                return;
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
            if (voice->Virtual)
                return static_cast<uint64_t>(voice->Position(static_cast<double>(GetClockFrames())));
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
        {
            const auto now    = voice->PausedAt == NoFrame ? m_Impl->WritableClock() : voice->PausedAt;
            const auto frames = static_cast<uint64_t>(
                std::min(static_cast<double>(std::max(0.f, seconds) * GetSampleRate()), double(NoFrame / 2)));
            voice->Envelope.User = {now, frames, voice->Envelope.User.Gain(now), std::max(0.f, targetVolume)};
            m_Impl->Queue(*voice);
        }
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
                const bool attached = voice->Attached;
                m_Impl->Detach(*voice);
                ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(&voice->Sound), beginFrame, end);
                ma_uint64 cursor = 0;
                ma_sound_get_cursor_in_pcm_frames(&voice->Sound, &cursor);
                // A shortened loop may put the prefetched source cursor beyond its new end.
                // Queue a public seek so that only the current processing block can retain old loop samples.
                if (ma_sound_is_looping(&voice->Sound) && cursor >= end)
                    ma_sound_seek_to_pcm_frame(&voice->Sound, beginFrame);
                if (attached)
                    m_Impl->Attach(*voice);
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
        voice->Spatial           = params;
        voice->Spatial.Direction = direction;
        voice->ConeInnerCos      = std::cos(params.ConeInnerAngle * RadiansPerDegree * 0.5f);
        voice->ConeOuterCos      = std::cos(params.ConeOuterAngle * RadiansPerDegree * 0.5f);
        ma_sound_set_direction(sound, direction.x, direction.y, direction.z);
        ma_sound_set_attenuation_model(sound, model);
        ma_sound_set_min_distance(sound, params.MinDistance);
        ma_sound_set_max_distance(sound, params.MaxDistance);
        ma_sound_set_rolloff(sound, params.Attenuation == AudioAttenuationModel::None ? 0.f : params.Rolloff);
        ma_sound_set_doppler_factor(sound, params.DopplerFactor);
        ma_sound_set_cone(sound,
                          params.ConeInnerAngle * RadiansPerDegree,
                          params.ConeOuterAngle * RadiansPerDegree,
                          params.ConeOuterGain);
    }
    void AudioEngine::SetListener(uint32_t index, const AudioListenerState& state)
    {
        if (!IsInitialized() || index >= m_Impl->Config.MaxListeners)
            return;
        m_Impl->Listeners[index] = state;
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
                const auto& position = slot.Value->Spatial.Position;
                ma_sound_set_pinned_listener_index(sound,
                    ma_engine_find_closest_listener(engine, position.x, position.y, position.z));
            }
        }
    }
}
