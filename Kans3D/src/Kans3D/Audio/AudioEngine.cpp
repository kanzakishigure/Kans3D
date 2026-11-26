#include "kspch.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Audio/AudioEventPlayer.h"
#include "Kans3D/Audio/AudioMusicPlayer.h"
#include "Kans3D/Core/Log/Log.h"
#include "AudioBusGraph.h"
#include "AudioChannelRouting.h"
#include "AudioGainRamp.h"

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
#include <glm/common.hpp>
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
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
        constexpr size_t MaxOutputChannels = 8;
        // Two destination sends plus two lanes available for previous sends to fade out.
        constexpr size_t VoiceSendLanes = 2 * MaxAudioVoiceSends;

        float RoutingGain(float db) { return IsValidAudioBusLevel(db) ? std::pow(10.f, db / 20.f) : 0.f; }

        std::array<float, 2> StereoDownmix(ma_channel channel)
        {
            switch (channel)
            {
                case MA_CHANNEL_FRONT_LEFT:
                    return {1, 0};
                case MA_CHANNEL_FRONT_RIGHT:
                    return {0, 1};
                case MA_CHANNEL_FRONT_CENTER:
                    return {AudioMinus3Db, AudioMinus3Db};
                case MA_CHANNEL_BACK_LEFT:
                case MA_CHANNEL_SIDE_LEFT:
                case MA_CHANNEL_FRONT_LEFT_CENTER:
                case MA_CHANNEL_TOP_FRONT_LEFT:
                case MA_CHANNEL_TOP_BACK_LEFT:
                    return {AudioMinus3Db, 0};
                case MA_CHANNEL_BACK_RIGHT:
                case MA_CHANNEL_SIDE_RIGHT:
                case MA_CHANNEL_FRONT_RIGHT_CENTER:
                case MA_CHANNEL_TOP_FRONT_RIGHT:
                case MA_CHANNEL_TOP_BACK_RIGHT:
                    return {0, AudioMinus3Db};
                case MA_CHANNEL_BACK_CENTER:
                case MA_CHANNEL_TOP_CENTER:
                case MA_CHANNEL_TOP_FRONT_CENTER:
                case MA_CHANNEL_TOP_BACK_CENTER:
                    return {.5f, .5f};
                default:
                    return {};
            }
        }

        struct SpatialMix
        {
            std::array<float, MaxOutputChannels> Gains {1, 1, 1, 1, 1, 1, 1, 1};
            float                                FilterCoefficient = 1;
            bool                                 CustomPan         = false;
            uint32_t                             Listener          = 0;
            bool                                 HasNative         = false;
            AudioSpatialParams                   Native;
            bool                                 operator==(const SpatialMix& other) const
            {
                return Gains == other.Gains && FilterCoefficient == other.FilterCoefficient &&
                       CustomPan == other.CustomPan && Listener == other.Listener && HasNative == other.HasNative &&
                       Native.Position == other.Native.Position && Native.Velocity == other.Native.Velocity &&
                       Native.Direction == other.Native.Direction && Native.Attenuation == other.Native.Attenuation &&
                       Native.MinDistance == other.Native.MinDistance &&
                       Native.MaxDistance == other.Native.MaxDistance && Native.Rolloff == other.Native.Rolloff &&
                       Native.DopplerFactor == other.Native.DopplerFactor &&
                       Native.ConeInnerAngle == other.Native.ConeInnerAngle &&
                       Native.ConeOuterAngle == other.Native.ConeOuterAngle &&
                       Native.ConeOuterGain == other.Native.ConeOuterGain;
            }
        };

        // One writer and one reader. Each owns a slot, and only the exchange slot
        // changes hands. The mixer reads a complete set of targets once per block.
        template<class T>
        struct MixMailbox
        {
            static constexpr uint32_t Pending = 4;
            std::array<T, 3>          Slots;
            std::atomic<uint32_t>     Exchanged {2};
            uint32_t                  WriteSlot = 0, ReadSlot = 1;
            T                         Published {};

            void Publish(const T& mix)
            {
                if (Published == mix)
                    return;
                Published        = mix;
                Slots[WriteSlot] = mix;
                WriteSlot        = Exchanged.exchange(WriteSlot | Pending, std::memory_order_acq_rel) & ~Pending;
            }

            bool Consume(T& mix)
            {
                if (!(Exchanged.load(std::memory_order_acquire) & Pending))
                    return false;
                ReadSlot = Exchanged.exchange(ReadSlot, std::memory_order_acq_rel) & ~Pending;
                mix      = Slots[ReadSlot];
                return true;
            }

            void Reset()
            {
                Exchanged.store(2, std::memory_order_relaxed);
                WriteSlot = 0;
                ReadSlot  = 1;
                Published = {};
            }
        };

        float AirFilterCoefficient(float cutoff, uint32_t sampleRate)
        {
            if (cutoff <= 0)
                return 1;
            constexpr float pi = 3.14159265358979323846f;
            // Exact digital -3 dB cutoff for y += coefficient * (x - y).
            const float sine = std::sin(pi * std::min(cutoff / sampleRate, 0.5f));
            return 2 * sine / (std::sqrt(1 + sine * sine) + sine);
        }

        float ConeGain(const glm::vec3&            direction,
                       const glm::vec3&            relative,
                       float                       distance,
                       const AudioAttenuationCone& cone)
        {
            if (distance <= 0.001f || cone.InnerAngle >= 360)
                return 1;
            const float cosine = glm::dot(direction, relative) / distance;
            const float inner  = std::cos(cone.InnerAngle * RadiansPerDegree * 0.5f);
            const float outer  = std::cos(cone.OuterAngle * RadiansPerDegree * 0.5f);
            if (cosine >= inner)
                return 1;
            if (cosine <= outer)
                return cone.OuterGain;
            return glm::mix(cone.OuterGain, 1.f, (cosine - outer) / (inner - outer));
        }

        uint64_t AddFrames(uint64_t start, uint64_t count) { return start + std::min(NoFrame - start, count); }

        double WrapPosition(double frame, uint64_t begin, uint64_t end, bool loop)
        {
            if (frame < end)
                return frame;
            return loop ? begin + std::fmod(frame - begin, static_cast<double>(end - begin)) : end;
        }

        struct PitchTransition
        {
            float    From = 1, To = 1;
            uint64_t Start  = 0;
            uint32_t Frames = 0;
            bool     Driven = false;

            float At(double clock) const
            {
                if (!Frames)
                    return To;
                return glm::mix(From, To, float(std::clamp((clock - Start) / Frames, 0.0, 1.0)));
            }

            double Advance(double first, double elapsed) const
            {
                if (!Frames || first >= double(Start) + Frames)
                    return elapsed * To;
                const double end       = first + elapsed;
                const double beginRamp = std::clamp(double(Start), first, end);
                const double endRamp   = std::clamp(double(Start) + Frames, first, end);
                return (beginRamp - first) * From + (endRamp - beginRamp) * (At(beginRamp) + At(endRamp)) * 0.5 +
                       (end - endRamp) * To;
            }

            bool operator==(const PitchTransition& other) const
            {
                return From == other.From && To == other.To && Start == other.Start && Frames == other.Frames &&
                       Driven == other.Driven;
            }
        };

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
            std::array<ma_channel, MA_MAX_CHANNELS>   InputMap {};
            std::array<ma_channel, MaxOutputChannels> OutputMap {};
            std::array<std::array<float, 2>, MA_MAX_CHANNELS> DownmixWeights {};
            ma_channel_converter                              Converter {};
            bool                                              ConverterReady = false, Downmix = false, Spatial = false;
            uint32_t                                          OutputChannels = 0;
            int                                               InputLfe = -1, OutputLfe = -1, Center = -1;
            std::atomic<bool>                                 PinToCenter {false};
            std::atomic<float>                                LfeGain {0};
            std::vector<float>                                RoutingScratch;

            ma_result
            ConfigureRouting(const ma_channel* input, const ma_channel* output, uint32_t channels, bool spatial)
            {
                Spatial = spatial;
                std::copy_n(input, Info.Channels, InputMap.begin());
                OutputChannels = Info.Channels;
                OutputLfe      = AudioChannelIndex(output, channels, MA_CHANNEL_LFE);
                Center         = AudioChannelIndex(output, channels, MA_CHANNEL_FRONT_CENTER);
                Downmix        = channels <= 2 && Info.Channels > 2;
                if (!Downmix && OutputLfe < 0 && (Spatial || channels <= 2))
                    return MA_SUCCESS;

                OutputChannels = channels;
                std::copy_n(output, channels, OutputMap.begin());
                InputLfe = AudioChannelIndex(input, Info.Channels, MA_CHANNEL_LFE);
                RoutingScratch.resize(32 * Info.Channels);
                if (Downmix)
                {
                    for (size_t ch = 0; ch < Info.Channels; ++ch)
                        DownmixWeights[ch] = StereoDownmix(input[ch]);
                }
                else
                {
                    auto       config = ma_channel_converter_config_init(ma_format_f32,
                                                                         Info.Channels,
                                                                         InputMap.data(),
                                                                         OutputChannels,
                                                                         OutputMap.data(),
                                                                         Spatial ? ma_channel_mix_mode_rectangular :
                                                                                   ma_channel_mix_mode_simple);
                    const auto result = ma_channel_converter_init(&config, nullptr, &Converter);
                    if (result != MA_SUCCESS)
                        return result;
                    ConverterReady = true;
                }
                return MA_SUCCESS;
            }

            ma_result ReadRaw(void* out, ma_uint64 count, ma_uint64& read)
            {
                return Stream ? Read(out, count, read) : ma_data_source_read_pcm_frames(&Data, out, count, &read);
            }

            ma_result ReadRouted(void* out, ma_uint64 count, ma_uint64& read)
            {
                if (RoutingScratch.empty() || !out)
                    return ReadRaw(out, count, read);
                const bool  pin         = OutputChannels > 2 && !Spatial && PinToCenter.load(std::memory_order_relaxed);
                const float lfeGain     = LfeGain.load(std::memory_order_relaxed);
                const auto  capacity    = RoutingScratch.size() / Info.Channels;
                auto*       destination = static_cast<float*>(out);
                read                    = 0;
                ma_result result        = MA_SUCCESS;
                while (read < count)
                {
                    ma_uint64  frames    = 0;
                    const auto requested = std::min<ma_uint64>(count - read, capacity);
                    result               = ReadRaw(RoutingScratch.data(), requested, frames);
                    if (ConverterReady && !pin)
                        ma_channel_converter_process_pcm_frames(&Converter, destination, RoutingScratch.data(), frames);
                    for (ma_uint64 i = 0; i < frames; ++i)
                    {
                        const auto* source = RoutingScratch.data() + i * Info.Channels;
                        auto*       frame  = destination + i * OutputChannels;
                        if (Downmix)
                        {
                            float left = 0, right = 0;
                            for (uint32_t ch = 0; ch < Info.Channels; ++ch)
                            {
                                const auto weights = DownmixWeights[ch];
                                left += source[ch] * weights[0];
                                right += source[ch] * weights[1];
                            }
                            // BS.775 Annex 4 folds Lo/Ro to mono at -3 dB per side.
                            frame[0] = OutputChannels == 1 ? AudioMinus3Db * (left + right) : left;
                            if (OutputChannels == 2)
                                frame[1] = right;
                            continue;
                        }

                        float mono = 0;
                        if (pin || (OutputLfe >= 0 && lfeGain != 0))
                        {
                            for (uint32_t ch = 0; ch < Info.Channels; ++ch)
                                if (int(ch) != InputLfe)
                                    mono += source[ch];
                            mono /= Info.Channels - (InputLfe >= 0);
                        }
                        if (pin)
                            RouteAudioCenter(frame, OutputChannels, Center, mono);
                        if (OutputLfe >= 0)
                        {
                            // Spatial voices have only the explicit LFE send. 2D beds can retain authored LFE.
                            const float bed  = !Spatial && !pin && InputLfe >= 0 ? source[InputLfe] : 0;
                            frame[OutputLfe] = bed + mono * lfeGain;
                        }
                    }
                    destination += frames * OutputChannels;
                    read += frames;
                    if (result != MA_SUCCESS || frames < requested)
                        break;
                }
                return result;
            }
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
                           uint64_t             loopEnd,
                           const ma_channel*    inputMap,
                           const ma_channel*    outputMap,
                           uint32_t             outputChannels,
                           bool                 spatial)
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
                result = ConfigureRouting(inputMap, outputMap, outputChannels, spatial);
                if (result != MA_SUCCESS)
                {
                    if (!Stream)
                        ma_resource_manager_data_source_uninit(&Data);
                    return result;
                }
                static const ma_data_source_vtable callbacks = {
                    [](ma_data_source* source, void* out, ma_uint64 count, ma_uint64* read) -> ma_result {
                        auto& self = *static_cast<VoiceSource*>(source);
                        return self.ReadRouted(out, count, *read);
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
                        if (format)
                            *format = ma_format_f32;
                        if (channels)
                            *channels = self.OutputChannels;
                        if (rate)
                            *rate = self.Info.SampleRate;
                        if (map)
                        {
                            const auto* from =
                                self.RoutingScratch.empty() ? self.InputMap.data() : self.OutputMap.data();
                            std::copy_n(from, std::min<size_t>(capacity, self.OutputChannels), map);
                        }
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
                if (ConverterReady)
                    ma_channel_converter_uninit(&Converter, nullptr);
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
                PitchTransition Pitch;

                double Position(double clock, uint64_t streamDelay = 0) const
                {
                    const auto delay   = std::min(streamDelay, StreamDelayEnd);
                    const auto waited  = delay - std::min(delay, StreamDelayBase);
                    const auto elapsed = std::max(0.0, std::min(clock, static_cast<double>(Stop)) - Start - waited);
                    const double played =
                        Pitch.Frames ? Pitch.Advance(double(Start), elapsed) * Rate / Pitch.To : elapsed * Rate;
                    return WrapPosition(Source + played, LoopBegin, Loop ? LoopEnd : Length, Loop);
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
                MixMailbox<PitchTransition>                    PitchMailbox;
                PitchTransition                                Pitch;
                MixMailbox<SpatialMix>                         SpatialMailbox;
                SpatialMix                           SpatialCurrent, SpatialTarget;
                std::array<float, MaxOutputChannels> AirHistory {};
                uint32_t                             SpatialRamp    = 0;
                float                                NativePanFloor = 0;
                bool                                 HasMixed       = false;
                const std::array<uint64_t, MaxAudioListeners>* ListenerIdentities = nullptr;
                uint64_t                                       ListenerIdentity   = 0;
                uint32_t                                       HandoffRemaining   = 0;
                std::array<float, MaxOutputChannels>           HandoffGains {};

                void SetSpatialTarget()
                {
                    SpatialMix next;
                    if (SpatialMailbox.Consume(next))
                    {
                        const auto& params = next.Native;
                        // The native pose is mixer-owned. Publishing it here avoids contending for
                        // miniaudio's vector spinlocks with the control thread.
                        auto& spatializer          = engineNode.spatializer;
                        spatializer.position.v     = {params.Position.x, params.Position.y, params.Position.z};
                        spatializer.velocity.v     = {params.Velocity.x, params.Velocity.y, params.Velocity.z};
                        spatializer.direction.v    = {params.Direction.x, params.Direction.y, params.Direction.z};
                        ma_attenuation_model model = ma_attenuation_model_inverse;
                        if (params.Attenuation == AudioAttenuationModel::Linear)
                            model = ma_attenuation_model_linear;
                        else if (params.Attenuation == AudioAttenuationModel::Exponential)
                            model = ma_attenuation_model_exponential;
                        ma_sound_set_attenuation_model(this, model);
                        ma_sound_set_min_distance(this, params.MinDistance);
                        ma_sound_set_max_distance(this, params.MaxDistance);
                        ma_sound_set_rolloff(this,
                                             params.Attenuation == AudioAttenuationModel::None ? 0.f : params.Rolloff);
                        ma_sound_set_doppler_factor(this, params.DopplerFactor);
                        ma_sound_set_cone(this,
                                          params.ConeInnerAngle * RadiansPerDegree,
                                          params.ConeOuterAngle * RadiansPerDegree,
                                          params.ConeOuterGain);
                        engineNode.spatializer.minSpatializationChannelGain = next.CustomPan ? 1 : NativePanFloor;
                        if (!HasMixed)
                            SpatialCurrent = next;
                        else if (SpatialTarget.Gains != next.Gains ||
                                 SpatialTarget.FilterCoefficient != next.FilterCoefficient ||
                                 SpatialTarget.CustomPan != next.CustomPan)
                            SpatialRamp = std::max(1u, ma_engine_get_sample_rate(ma_sound_get_engine(this)) / 100);
                        SpatialTarget = next;
                    }
                    // A frame can publish new poses while a voice is being retargeted. In this
                    // case a removed slot falls back to the mixer's closest enabled listener.
                    const auto* engine   = ma_sound_get_engine(this);
                    const auto& p        = SpatialTarget.Native.Position;
                    const auto  listener = ma_engine_listener_is_enabled(engine, SpatialTarget.Listener) ?
                                               SpatialTarget.Listener :
                                               ma_engine_find_closest_listener(engine, p.x, p.y, p.z);
                    const auto  identity = (*ListenerIdentities)[listener];
                    const auto  previous = ma_sound_get_pinned_listener_index(this);
                    if (HasMixed && (previous != listener || ListenerIdentity != identity))
                    {
                        auto& gainer = engineNode.spatializer.gainer;
                        if (!HandoffRemaining)
                        {
                            const auto  smooth   = gainer.config.smoothTimeInFrames;
                            const float position = gainer.t < smooth ? float(gainer.t) / smooth : 1.f;
                            for (uint32_t ch = 0; ch < gainer.config.channels; ++ch)
                                HandoffGains[ch] = glm::mix(gainer.pOldGains[ch], gainer.pNewGains[ch], position);
                        }
                        HandoffRemaining = std::max(1u, ma_engine_get_sample_rate(engine) / 100);
                    }
                    ListenerIdentity = identity;
                    if (previous != listener)
                        ma_sound_set_pinned_listener_index(this, listener);
                }

                static constexpr float HandoffGainFloor = 1e-7f;

                void ApplyListenerHandoff(float* output, uint32_t frames, uint32_t channels)
                {
                    // Miniaudio provides the destination gains. During the handoff use a
                    // sample-counted ramp so its SIMD gainer cannot step at block boundaries.
                    const auto*                          targets = engineNode.spatializer.gainer.pNewGains;
                    std::array<float, MaxOutputChannels> destination {};
                    for (uint32_t ch = 0; ch < channels; ++ch)
                        destination[ch] = targets[ch] <= HandoffGainFloor ? 0.f : targets[ch];
                    for (uint32_t frame = 0; frame < frames; ++frame)
                    {
                        if (HandoffRemaining)
                        {
                            const float step = 1.f / HandoffRemaining;
                            for (uint32_t ch = 0; ch < channels; ++ch)
                                HandoffGains[ch] = glm::mix(HandoffGains[ch], destination[ch], step);
                            --HandoffRemaining;
                        }
                        for (uint32_t ch = 0; ch < channels; ++ch)
                            output[frame * channels + ch] *= targets[ch] > 0 ? HandoffGains[ch] / targets[ch] : 0.f;
                    }
                }

                void ApplySpatialMix(float* output, uint32_t frames, uint32_t channels)
                {
                    // The mixer leaves legacy PCM untouched, including the original miniaudio gainer.
                    if (!SpatialRamp && SpatialCurrent.Gains == SpatialMix {}.Gains &&
                        SpatialCurrent.FilterCoefficient == 1)
                        return;
                    for (uint32_t frame = 0; frame < frames; ++frame)
                    {
                        if (SpatialRamp)
                        {
                            const float step = 1.f / SpatialRamp;
                            SpatialCurrent.FilterCoefficient =
                                glm::mix(SpatialCurrent.FilterCoefficient, SpatialTarget.FilterCoefficient, step);
                            for (uint32_t ch = 0; ch < channels; ++ch)
                            {
                                auto&       gain   = SpatialCurrent.Gains[ch];
                                const float target = SpatialTarget.Gains[ch];
                                if (gain != target)
                                    gain = std::sqrt(glm::mix(gain * gain, target * target, step));
                            }
                            if (--SpatialRamp == 0)
                                SpatialCurrent = SpatialTarget;
                        }
                        const float coefficient = SpatialCurrent.FilterCoefficient;
                        for (uint32_t ch = 0; ch < channels; ++ch)
                        {
                            auto& sample  = output[frame * channels + ch];
                            auto& history = AirHistory[ch];
                            history       = coefficient == 1 ? sample : glm::mix(history, sample, coefficient);
                            sample        = history * SpatialCurrent.Gains[ch];
                        }
                    }
                }

                void ResetSpatialMix()
                {
                    SpatialMailbox.Reset();
                    SpatialCurrent = SpatialTarget                      = {};
                    AirHistory                                          = {};
                    SpatialRamp                                         = 0;
                    HasMixed                                            = false;
                    HandoffRemaining                                    = 0;
                    ListenerIdentity                                    = 0;
                    engineNode.spatializer.minSpatializationChannelGain = NativePanFloor;
                }

                void ProcessPitch(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount)
                {
                    const uint64_t endRamp   = AddFrames(Pitch.Start, Pitch.Frames);
                    const auto     channels  = ma_node_get_output_channels(node, 0);
                    const auto     requested = *outCount;
                    *outCount                = 0;
                    while (*outCount < requested)
                    {
                        const uint64_t frame = Next + *outCount;
                        ma_uint32      count = requested - *outCount;
                        if (Pitch.Frames && frame < endRamp)
                        {
                            // Midpoint integration in short spans keeps the resampler phase continuous.
                            const auto boundary = frame < Pitch.Start ? Pitch.Start : std::min(endRamp, frame + 16);
                            count               = static_cast<ma_uint32>(std::min<uint64_t>(count, boundary - frame));
                        }
                        ma_sound_set_pitch(this, Pitch.At(double(frame) + count * 0.5));
                        float* output = out[0] + size_t(*outCount) * channels;
                        auto   mixed  = count;
                        Original->onProcess(node, in, inCount, &output, &mixed);
                        *outCount += mixed;
                        if (mixed < count)
                            break;
                    }
                }

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
                    self.PitchMailbox.Consume(self.Pitch);
                    if (self.Pitch.Driven)
                        self.engineNode.isPitchDisabled = false;
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
                            !self.Pitch.Driven && !voice.Desc.Spatial &&
                            self.engineNode.sampleRate == ma_engine_get_sample_rate(ma_sound_get_engine(&self)) &&
                            ma_sound_get_pitch(&self) == 1 && !self.FractionalSeek;
                        if (self.engineNode.isPitchDisabled != bypass)
                        {
                            self.engineNode.isPitchDisabled = bypass;
                            self.ResetResampler();
                        }
                    }
                    if (voice.Desc.Spatial)
                        self.SetSpatialTarget();
                    const auto requested = *outCount;
                    const bool handoff            = voice.Desc.Spatial && self.HandoffRemaining != 0;
                    float      minGain            = 0;
                    uint32_t   nativeSmoothFrames = 0;
                    if (handoff)
                    {
                        auto& spatializer = self.engineNode.spatializer;
                        minGain           = ma_spatializer_get_min_gain(&spatializer);
                        // A strictly positive native gain keeps the source recoverable during the ramp,
                        // even when the destination is fully attenuated.
                        ma_spatializer_set_min_gain(&spatializer, HandoffGainFloor);
                        auto& gainer       = spatializer.gainer;
                        nativeSmoothFrames = gainer.config.smoothTimeInFrames;
                        // The voice owns smoothing for the entire handoff. One native frame
                        // avoids dividing by zero when miniaudio reads its interpolation state.
                        gainer.config.smoothTimeInFrames = 1;
                        std::copy_n(gainer.pNewGains, gainer.config.channels, gainer.pOldGains);
                        gainer.t = UINT32_MAX;
                    }
                    if (self.Pitch.Driven)
                        self.ProcessPitch(node, in, inCount, out, outCount);
                    else
                        self.Original->onProcess(node, in, inCount, out, outCount);
                    if (*outCount < requested && voice.Source->Stream && !voice.Source->Offline &&
                        !ma_sound_at_end(&self))
                        voice.StreamDelay.fetch_add(requested - *outCount, std::memory_order_relaxed);
                    const auto channels = ma_node_get_output_channels(node, 0);
                    if (handoff)
                    {
                        auto& spatializer = self.engineNode.spatializer;
                        ma_spatializer_set_min_gain(&spatializer, minGain);
                        self.ApplyListenerHandoff(out[0], *outCount, channels);
                        auto& gainer = spatializer.gainer;
                        // Subsequent native pulls must start at the gain the ramp ended on.
                        for (uint32_t ch = 0; ch < channels; ++ch)
                        {
                            auto& gain = gainer.pNewGains[ch];
                            if (gain <= HandoffGainFloor)
                                gain = 0;
                            gainer.pOldGains[ch] = gain;
                        }
                        gainer.config.smoothTimeInFrames = nativeSmoothFrames;
                        gainer.t                         = nativeSmoothFrames;
                    }
                    if (voice.Desc.Spatial)
                    {
                        self.ApplySpatialMix(out[0], *outCount, channels);
                        self.HasMixed |= *outCount != 0;
                    }
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
                    if (channels == 2 && !voice.Desc.Spatial &&
                        voice.Source->PinToCenter.load(std::memory_order_relaxed))
                        for (uint32_t i = 0; i < *outCount; ++i)
                        {
                            auto* frame = out[0] + i * channels;
                            RouteAudioCenter(frame, channels, -1, .5f * (frame[0] + frame[1]));
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

            struct SendNode : ma_node_base
            {
                struct Lane
                {
                    AudioGainRamp         Level;
                    std::atomic<uint64_t> Command {0};
                    std::atomic<uint32_t> SilentRevision {0};
                    ma_node*              Target        = nullptr; // Control thread only.
                    size_t                BusIndex      = 0;
                    uint32_t              Revision      = 0;
                    float                 RequestedGain = 0;

                    void Set(float gain)
                    {
                        if (RequestedGain == gain)
                            return;
                        RequestedGain = gain;
                        uint32_t bits;
                        std::memcpy(&bits, &gain, sizeof(bits));
                        Command.store((uint64_t(++Revision) << 32) | bits, std::memory_order_relaxed);
                    }

                    bool IsSilent() const
                    {
                        return RequestedGain == 0 && SilentRevision.load(std::memory_order_relaxed) == Revision;
                    }
                };

                std::array<Lane, VoiceSendLanes> Sends;
                uint32_t                         Channels = 0, RampFrames = 0;
                int                              Lfe   = -1;
                bool                             Ready = false;

                ma_result Init(ma_engine& engine, int lfe)
                {
                    Channels   = ma_engine_get_channels(&engine);
                    RampFrames = std::max(1u, ma_engine_get_sample_rate(&engine) / 100);
                    Lfe        = lfe;
                    for (auto& lane : Sends)
                        lane.Level.Initialize(0);
                    static const ma_node_vtable vtable = {
                        Process, nullptr, 1, VoiceSendLanes + 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};
                    std::array<ma_uint32, VoiceSendLanes + 1> outputs;
                    outputs.fill(Channels);
                    auto config            = ma_node_config_init();
                    config.vtable          = &vtable;
                    config.pInputChannels  = &Channels;
                    config.pOutputChannels = outputs.data();
                    const auto result      = ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, this);
                    Ready                  = result == MA_SUCCESS;
                    return result;
                }

                ~SendNode()
                {
                    if (Ready)
                        ma_node_uninit(this, nullptr);
                }

                static void Process(ma_node*      node,
                                    const float** inputs,
                                    ma_uint32*    inputFrames,
                                    float**       outputs,
                                    ma_uint32*    outputFrames)
                {
                    auto&        self     = *static_cast<SendNode*>(static_cast<ma_node_base*>(node));
                    const auto   frames   = std::min(*inputFrames, *outputFrames);
                    const auto   channels = self.Channels;
                    const size_t samples  = size_t(frames) * channels;
                    if (samples)
                        std::copy_n(inputs[0], samples, outputs[0]);
                    for (size_t i = 0; i < self.Sends.size(); ++i)
                    {
                        auto&      lane    = self.Sends[i];
                        const auto command = lane.Command.load(std::memory_order_relaxed);
                        const auto bits    = static_cast<uint32_t>(command);
                        float      target;
                        std::memcpy(&target, &bits, sizeof(target));
                        lane.Level.Prepare(self.RampFrames, target);
                        auto* wet = outputs[i + 1];
                        if (lane.Level.Current == 0 && lane.Level.Destination == 0)
                            std::fill_n(wet, samples, 0.f);
                        else
                            for (uint32_t frame = 0; frame < frames; ++frame)
                            {
                                const auto  offset = size_t(frame) * channels;
                                const float gain   = lane.Level.Next();
                                for (uint32_t ch = 0; ch < channels; ++ch)
                                    wet[offset + ch] = int(ch) == self.Lfe ? 0.f : inputs[0][offset + ch] * gain;
                            }
                        if (lane.Level.Current == 0 && lane.Level.Destination == 0)
                            lane.SilentRevision.store(uint32_t(command >> 32), std::memory_order_relaxed);
                        lane.Level.Publish();
                    }
                    *inputFrames = *outputFrames = frames;
                }

                void Disconnect()
                {
                    ma_node_detach_all_output_buses(this);
                    for (auto& lane : Sends)
                    {
                        lane.Target = nullptr;
                        lane.Set(0);
                        lane.Level.Remaining = 0;
                        lane.Level.Initialize(0);
                        lane.SilentRevision.store(lane.Revision, std::memory_order_relaxed);
                    }
                }
            };

            std::shared_ptr<VoiceSource> Source = std::make_shared<VoiceSource>();
            std::atomic<uint64_t>        StreamDelay {0};
            Node                     Sound {};
            SendNode                           Route {};
            AudioVoiceDesc Desc;
            AudioSoundInfo           Info {};
            std::deque<Transport>    History;
            std::atomic<bool>        Timed {false};
            std::atomic<uint64_t>    PendingSeek {NoFrame};
            std::shared_ptr<Control> Commands = std::make_shared<Control>();
            Fades                    Envelope;
            AudioSpatialParams           Spatial;
            float                        ConeInnerCos = -1, ConeOuterCos = -1;
            AssetID                            EventAttenuationAsset, AttenuationID;
            AssetManager*                      AttenuationAssets = nullptr;
            std::weak_ptr<const int>           AttenuationLifetime;
            std::weak_ptr<const AssetRegistry> AttenuationRegistry;
            Ref<const AudioAttenuationAsset>   Attenuation;
            bool                               AttenuationHasSpread = false;
            std::optional<float>               SpatialAudibility;
            uint32_t                           Listener        = MaxAudioListeners;
            AudioClipRegistry*           OneShotRegistry = nullptr;
            AssetID                      OneShotClip, EventGroup;
            uint64_t                     PlayOrder = 0, PinnedStart = NoFrame;
            bool                         Attached = false, Virtual = false, CountedReal = false, Stolen = false;
            bool                         Poolable = false;
            uint64_t                 PausedAt = NoFrame, RemainingStart = 0, RemainingStop = NoFrame;
            bool                               Initialized = false, Scheduled = false, BusHeld = false;
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
            std::array<ma_channel, MA_MAX_CHANNELS>            ChannelMap {};
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
        uint64_t                        InstanceID = 0;
        bool                            GamePaused = false;
        float                           TimeScale = 1, PitchTimeScale = 1;

        const AudioBusDefinition* BusPolicy(const std::string& name) const
        {
            if (!Buses)
                return nullptr;
            if (auto* bus = Buses->Find(name))
                return bus;
            return Buses->Find(MasterAudioBus);
        }
        AudioEngineConfig Config;
        ma_spatializer                  PanProbe {};
        bool                            PanProbeReady = false;
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
        using ListenerSet = std::array<AudioListenerState, MaxAudioListeners>;
        ListenerSet                                  Listeners;
        MixMailbox<ListenerSet>                      ListenerMailbox;
        std::array<uint64_t, MaxAudioListeners>      MixerListenerIdentities {};
        ma_spatializer_listener                      ProbeListener {};
        int                                          LfeChannel = -1;
        std::unordered_set<std::string>              MissingReverbBuses;
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
            const auto result = ma_node_attach_output_bus(&voice.Route, 0, VoiceBusInput(*Buses, voice.Desc.Bus), 0);
            if (result != MA_SUCCESS)
            {
                Warn(Error("Attach voice", result));
                return false;
            }
            voice.Attached = true;
            UpdateVoiceSends(voice);
            return true;
        }

        void Detach(Voice& voice)
        {
            if (voice.Attached)
            {
                voice.Route.Disconnect();
                voice.Attached = false;
            }
        }

        void UpdateVoiceSends(Voice& voice)
        {
            if (!voice.Attached)
                return;
            const auto listener = voice.Listener;
            const bool send     = voice.Desc.ReverbZoneSend && listener < Config.MaxListeners;
            const std::array<AudioZoneSend, MaxAudioVoiceSends> empty {};
            const auto&                                         sends = send ? Listeners[listener].ReverbSends : empty;
            std::array<ma_node*, MaxAudioVoiceSends>            targets {};
            for (size_t i = 0; i < sends.size(); ++i)
                if (sends[i].Gain > 0 && std::isfinite(sends[i].Gain))
                    targets[i] = Buses->Input(sends[i].BusIndex);

            auto& lanes = voice.Route.Sends;
            for (size_t i = 0; i < lanes.size(); ++i)
            {
                auto& lane = lanes[i];
                if (!lane.Target)
                    continue;
                float targetGain = 0;
                for (size_t j = 0; j < targets.size(); ++j)
                    if (targets[j] == lane.Target && targets[j])
                    {
                        targetGain += sends[j].Gain;
                        targets[j] = nullptr;
                    }
                lane.Set(targetGain);
                if (lane.IsSilent())
                {
                    ma_node_detach_output_bus(&voice.Route, static_cast<ma_uint32>(i + 1));
                    lane.Target = nullptr;
                }
            }
            for (size_t i = 0; i < targets.size(); ++i)
            {
                if (!targets[i])
                    continue;
                float gain = sends[i].Gain;
                for (size_t j = i + 1; j < targets.size(); ++j)
                    if (targets[j] == targets[i])
                    {
                        gain += sends[j].Gain;
                        targets[j] = nullptr;
                    }
                const auto lane = std::find_if(lanes.begin(), lanes.end(), [](const auto& item) {
                    return item.Target == nullptr && item.IsSilent();
                });
                if (lane == lanes.end())
                    break; // Both spare lanes are still fading. The next update retries.
                const auto output = static_cast<ma_uint32>(lane - lanes.begin() + 1);
                if (ma_node_attach_output_bus(&voice.Route, output, targets[i], 0) == MA_SUCCESS)
                {
                    lane->Target   = targets[i];
                    lane->BusIndex = sends[i].BusIndex;
                    lane->Set(gain);
                }
            }
        }

        uint32_t ClosestListener(glm::vec3 position) const
        {
            float    closest  = std::numeric_limits<float>::infinity();
            uint32_t selected = MaxAudioListeners;
            for (uint32_t i = 0; i < Config.MaxListeners; ++i)
            {
                if (!Listeners[i].Active)
                    continue;
                const auto delta    = Listeners[i].Position - position;
                const auto distance = glm::dot(delta, delta);
                if (distance < closest)
                {
                    closest  = distance;
                    selected = i;
                }
            }
            return selected;
        }

        uint32_t ReverbListener(const Voice& voice) const
        {
            if (voice.Desc.ReverbPosition)
                return ClosestListener(*voice.Desc.ReverbPosition);
            for (uint32_t i = 0; i < Config.MaxListeners; ++i)
                if (Listeners[i].Active)
                    return i;
            return MaxAudioListeners;
        }

        void ApplyMixerListeners()
        {
            ListenerSet states;
            if (!ListenerMailbox.Consume(states))
                return;
            for (uint32_t i = 0; i < Config.MaxListeners; ++i)
            {
                const auto& state          = states[i];
                auto&       listener       = Engine.listeners[i];
                MixerListenerIdentities[i] = state.Identity;
                listener.position.v        = {state.Position.x, state.Position.y, state.Position.z};
                listener.velocity.v        = {state.Velocity.x, state.Velocity.y, state.Velocity.z};
                listener.direction.v       = {state.Forward.x, state.Forward.y, state.Forward.z};
                listener.config.worldUp    = {state.Up.x, state.Up.y, state.Up.z};
                ma_engine_listener_set_enabled(&Engine, i, state.Active);
            }
        }

        // Use the same channel map and directional gains as miniaudio. The probe is
        // initialized once and only used on the main thread.
        void SetCustomPan(SpatialMix& mix, glm::vec3 position, uint32_t listenerIndex, float boundary, float spread)
        {
            const auto& state    = Listeners[listenerIndex];
            auto&       listener = ProbeListener;
            ma_spatializer_listener_set_position(&listener, state.Position.x, state.Position.y, state.Position.z);
            ma_spatializer_listener_set_direction(&listener, state.Forward.x, state.Forward.y, state.Forward.z);
            ma_spatializer_listener_set_world_up(&listener, state.Up.x, state.Up.y, state.Up.z);
            ma_spatializer_set_positioning(&PanProbe, ma_positioning_absolute);
            ma_spatializer_set_position(&PanProbe, position.x, position.y, position.z);
            ma_vec3f relative {};
            ma_spatializer_get_relative_position_and_direction(&PanProbe, &listener, &relative, nullptr);
            ma_spatializer_set_positioning(&PanProbe, ma_positioning_relative);
            ma_spatializer_set_position(&PanProbe, relative.x, relative.y, relative.z);
            std::array<float, MaxOutputChannels> input {}, output {};
            ma_spatializer_process_pcm_frames(&PanProbe, &listener, output.data(), input.data(), 1);
            const auto channels = Config.Channels;
            float      power    = 0;
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                if (int(ch) == LfeChannel)
                    continue; // The explicit LFE send bypasses the directional and spread gains.
                const float directional = glm::mix(1.f, PanProbe.pNewChannelGainsOut[ch], boundary);
                mix.Gains[ch]           = directional;
                power += directional * directional;
            }
            // Interpolate channel power. The equal-gain endpoint has the same
            // total power, and each channel retains its original source signal.
            if (spread > 0)
                for (uint32_t ch = 0; ch < channels; ++ch)
                    if (int(ch) != LfeChannel)
                        mix.Gains[ch] = std::sqrt(
                            glm::mix(mix.Gains[ch] * mix.Gains[ch], power / (channels - (LfeChannel >= 0)), spread));
            mix.CustomPan = true;
        }

        void ApplySpatial(Voice& voice)
        {
            auto        params      = voice.Spatial;
            auto*       sound       = &voice.Sound;
            const auto* attenuation = voice.Attenuation ? &voice.Attenuation->GetData() : nullptr;
            const bool  shaped      = params.Shape.Type != AudioSourceShape::Point;
            const bool  enhanced    = shaped || attenuation;
            auto        listenerIndex = ClosestListener(params.Position);
            if (shaped)
            {
                params.ShapeTransform[3] = glm::vec4(params.Position, 1);
                float closest            = std::numeric_limits<float>::infinity();
                for (uint32_t index = 0; index < Config.MaxListeners; ++index)
                {
                    if (!Listeners[index].Active)
                        continue;
                    const auto& listener = Listeners[index].Position;
                    const auto  point    = ClosestAudioEmitterPoint(params.Shape, params.ShapeTransform, listener);
                    const auto  delta    = listener - point;
                    const float squared  = glm::dot(delta, delta);
                    if (squared < closest)
                    {
                        closest         = squared;
                        listenerIndex   = index;
                        params.Position = point;
                    }
                }
            }
            voice.Listener = listenerIndex;
            UpdateVoiceSends(voice);
            const auto direction = glm::dot(params.Direction, params.Direction) > 0 ? glm::normalize(params.Direction) :
                                                                                      glm::vec3(0, 0, -1);
            voice.ConeInnerCos   = std::cos(params.ConeInnerAngle * RadiansPerDegree * 0.5f);
            voice.ConeOuterCos   = std::cos(params.ConeOuterAngle * RadiansPerDegree * 0.5f);
            params.Direction     = direction;

            SpatialMix mix;
            mix.Listener = listenerIndex;
            if (enhanced && listenerIndex < MaxAudioListeners)
            {
                const auto relative = Listeners[listenerIndex].Position - params.Position;
                const auto distance = glm::length(relative);
                auto       sample =
                    attenuation ?
                        EvaluateAttenuation(*attenuation, distance) :
                        AudioAttenuationSample {EvaluateAttenuationPreset(
                            {params.Attenuation, params.MinDistance, params.MaxDistance, params.Rolloff}, distance)};
                // Over the first quarter metre outside the shape, ease in both
                // directional panning and cone gain. Interior points are centred.
                const float                t        = shaped ? std::clamp(distance / 0.25f, 0.f, 1.f) : 1.f;
                const float                boundary = t * t * (3 - 2 * t);
                const AudioAttenuationCone fallback {
                    params.ConeInnerAngle, params.ConeOuterAngle, params.ConeOuterGain};
                const auto& cone        = attenuation && attenuation->Cone ? *attenuation->Cone : fallback;
                const float angularGain = glm::mix(1.f, ConeGain(direction, relative, distance, cone), boundary);
                voice.SpatialAudibility = sample.Gain * angularGain;

                // Point presets use the native A1 distance model, including miniaudio's
                // listener-space transform and gainer. Curves and shapes use world distance.
                const auto* nativePreset =
                    attenuation && !shaped ? std::get_if<AudioAttenuationPreset>(&attenuation->Volume) : nullptr;
                if (nativePreset)
                {
                    params.Attenuation = nativePreset->Model;
                    params.MinDistance = nativePreset->MinDistance;
                    params.MaxDistance = nativePreset->MaxDistance;
                    params.Rolloff     = nativePreset->Rolloff;
                }
                else
                {
                    params.Attenuation = AudioAttenuationModel::None; // Retain native panning and Doppler.
                    params.MinDistance = params.MaxDistance = 1;
                }
                params.ConeInnerAngle = shaped ? 360 : cone.InnerAngle;
                params.ConeOuterAngle = shaped ? 360 : cone.OuterAngle;
                params.ConeOuterGain  = cone.OuterGain;
                if (attenuation)
                    params.DopplerFactor = attenuation->DopplerFactor;

                if (shaped || (attenuation && voice.AttenuationHasSpread))
                    SetCustomPan(mix, params.Position, listenerIndex, boundary, sample.Spread);
                const float postGain = (nativePreset ? 1.f : sample.Gain) * (shaped ? angularGain : 1.f);
                for (auto& gain : mix.Gains)
                    gain *= postGain;
                mix.FilterCoefficient = AirFilterCoefficient(sample.CutoffHz, Config.SampleRate);
            }
            else
                voice.SpatialAudibility.reset();

            mix.HasNative = true;
            mix.Native    = params;
            sound->SpatialMailbox.Publish(mix);
        }

        bool RefreshAttenuation(Voice& voice)
        {
            if (!voice.AttenuationAssets || voice.AttenuationLifetime.expired())
                return false;
            const auto registry = voice.AttenuationAssets->GetRegistry();
            if (registry == voice.AttenuationRegistry.lock())
                return false;
            voice.AttenuationRegistry = registry;
            auto        loaded        = voice.AttenuationAssets->Load<AudioAttenuationAsset>(voice.AttenuationID);
            const auto* asset         = std::get_if<Ref<const AudioAttenuationAsset>>(&loaded);
            auto        next          = asset ? *asset : nullptr;
            if (voice.Attenuation == next)
                return false;
            voice.Attenuation          = std::move(next);
            voice.AttenuationHasSpread = false;
            if (voice.Attenuation)
            {
                const auto& curve = voice.Attenuation->GetData().Spread;
                voice.AttenuationHasSpread =
                    std::any_of(curve.begin(), curve.end(), [](const auto& key) { return key.Value > 0; });
            }
            return true;
        }

        float Audibility(const Voice& voice, uint64_t clock)
        {
            float       busGain     = Buses->AudibilityGain(voice.Desc.Bus);
            const auto& definitions = Buses->GetBuses();
            if (voice.Desc.ReverbZoneSend && voice.Listener < Config.MaxListeners)
                for (const auto& send : Listeners[voice.Listener].ReverbSends)
                    if (send.Gain > 0 && send.BusIndex < definitions.size())
                        busGain = std::max(busGain, send.Gain * Buses->AudibilityGain(definitions[send.BusIndex].Name));
            for (const auto& lane : voice.Route.Sends)
                if (lane.Target)
                    busGain = std::max(busGain,
                                       lane.Level.Published.load(std::memory_order_relaxed) *
                                           Buses->AudibilityGain(definitions[lane.BusIndex].Name));
            const float gain = voice.Desc.Volume * voice.Envelope.Gain(clock) * busGain * ma_engine_get_volume(&Engine);
            if (std::isnan(gain))
                return 0;
            if (!voice.Desc.Spatial || gain == 0)
                return gain;
            if (voice.Listener == MaxAudioListeners)
                return 0;
            if (voice.SpatialAudibility)
                return gain * *voice.SpatialAudibility;
            const auto& spatial     = voice.Spatial;
            const auto  relative    = Listeners[voice.Listener].Position - spatial.Position;
            const float distance    = glm::length(relative);
            float       attenuation = 1;
            const auto  minDistance = spatial.MinDistance, maxDistance = spatial.MaxDistance;
            if (minDistance < maxDistance)
            {
                const auto d = std::clamp(distance, minDistance, maxDistance);
                switch (spatial.Attenuation)
                {
                    case AudioAttenuationModel::None:
                        break;
                    case AudioAttenuationModel::Inverse:
                        attenuation = minDistance / (minDistance + spatial.Rolloff * (d - minDistance));
                        break;
                    case AudioAttenuationModel::Linear:
                        attenuation = 1 - spatial.Rolloff * (d - minDistance) / (maxDistance - minDistance);
                        break;
                    case AudioAttenuationModel::Exponential:
                        attenuation = std::pow(d / minDistance, -spatial.Rolloff);
                        break;
                }
            }
            if (distance > 0.001f && spatial.ConeInnerAngle < 360)
            {
                const auto direction = glm::dot(spatial.Direction, spatial.Direction) > 0 ?
                                           glm::normalize(spatial.Direction) :
                                           glm::vec3(0, 0, -1);
                const auto dot       = glm::dot(direction, relative) / distance;
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
            sound.engineNode.isPitchDisabled = !state.Pitch.Driven && !voice.Desc.Spatial &&
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
        if (config.MaxListeners < 1 || config.MaxListeners > MaxAudioListeners || config.Channels == 0 ||
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
                impl.ApplyMixerListeners();
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
        static std::atomic<uint64_t> nextInstance {0};
        p.InstanceID = ++nextInstance;
        p.Ready = true;
        for (uint32_t i = 1; i < config.MaxListeners; ++i)
        {
            p.Listeners[i].Active = false;
            ma_engine_listener_set_enabled(&p.Engine, i, false);
        }
        const auto* channelMap    = p.Engine.listeners[0].config.pChannelMapOut;
        p.LfeChannel              = AudioChannelIndex(channelMap, config.Channels, MA_CHANNEL_LFE);
        p.ProbeListener           = {};
        p.ProbeListener.config    = p.Engine.listeners[0].config;
        p.ProbeListener.isEnabled = true;
        auto probeConfig          = ma_spatializer_config_init(config.Channels, config.Channels);
        probeConfig.positioning   = ma_positioning_relative;
        probeConfig.rolloff       = 0;
        probeConfig.dopplerFactor = 0;
        probeConfig.pChannelMapIn = p.Engine.listeners[0].config.pChannelMapOut;
        result                    = ma_spatializer_init(&probeConfig, nullptr, &p.PanProbe);
        if (result != MA_SUCCESS)
        {
            Shutdown();
            return Error("Audio spatial probe init", result);
        }
        p.PanProbeReady = true;
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
        p.GamePaused = false;
        p.TimeScale = p.PitchTimeScale = 1;
        p.WarnedBusNames.clear();
        p.WarnedDSPNames.clear();
        if (p.PanProbeReady)
        {
            ma_spatializer_uninit(&p.PanProbe, nullptr);
            p.PanProbeReady = false;
        }
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
        p.ListenerMailbox.Reset();
        p.MixerListenerIdentities.fill(0);
        p.MissingReverbBuses.clear();
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
    uint64_t AudioEngine::GetInstanceID() const { return IsInitialized() ? m_Impl->InstanceID : 0; }
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
            m_Impl->ApplyMixerListeners();
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
            if (p.RefreshAttenuation(voice))
                p.ApplySpatial(voice);
            else
                p.UpdateVoiceSends(voice);
            ++p.Stats.AllocatedVoices;
            const bool active = voice.Active(now);
            if ((active || voice.BusHeld) && !voice.Stolen && !voice.Desc.ConcurrencyGroup.empty())
                if (auto group = p.Stats.GroupActiveCounts.find(voice.Desc.ConcurrencyGroup);
                    group != p.Stats.GroupActiveCounts.end())
                    ++group->second;
            if (!active)
            {
                p.CountReal(voice, false);
                p.SetStreamActive(voice, false);
                continue;
            }
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
            return slot.Value && slot.Value->EventGroup == id && !slot.Value->Stolen &&
                   (slot.Value->BusHeld || slot.Value->Active(now));
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
        {
            RefreshBusPolicies();
            return std::nullopt;
        }
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
            result      = ma_node_attach_output_bus(&voice.Route, 0, p.VoiceBusInput(*next, voice.Desc.Bus), 0);
            if (result != MA_SUCCESS)
            {
                for (const auto& moved : p.Slots)
                {
                    if (&moved == &slot)
                        break;
                    if (moved.Value && moved.Value->Attached)
                        ma_node_attach_output_bus(
                            &moved.Value->Route, 0, p.VoiceBusInput(*p.Buses, moved.Value->Desc.Bus), 0);
                }
                return Error("Reattach audio voice", result);
            }
        }
        if (p.Buses)
        {
            for (auto& listener : p.Listeners)
                for (auto& send : listener.ReverbSends)
                    if (send.Gain > 0 && send.BusIndex < p.Buses->GetBuses().size())
                    {
                        const auto replacement = next->FindIndex(p.Buses->GetBuses()[send.BusIndex].Name);
                        if (replacement)
                            send.BusIndex = *replacement;
                        else
                            send = {};
                    }
            for (const auto& slot : p.Slots)
                if (slot.Value && slot.Value->Attached)
                    for (size_t i = 0; i < VoiceSendLanes; ++i)
                    {
                        auto& lane = slot.Value->Route.Sends[i];
                        if (!lane.Target)
                            continue;
                        // A retiring lane is still attached. Rebind both live and retiring lanes by name.
                        const auto replacement = next->FindIndex(p.Buses->GetBuses()[lane.BusIndex].Name);
                        const auto output      = static_cast<ma_uint32>(i + 1);
                        auto*      target      = replacement ? next->Input(*replacement) : nullptr;
                        if (target && ma_node_attach_output_bus(&slot.Value->Route, output, target, 0) == MA_SUCCESS)
                        {
                            lane.Target   = target;
                            lane.BusIndex = *replacement;
                        }
                        else
                        {
                            // Keep the detached lane reserved until the mixer acknowledges zero.
                            ma_node_detach_output_bus(&slot.Value->Route, output);
                            lane.Set(0);
                            lane.Target = nullptr;
                        }
                    }
        }
        p.Buses = std::move(next);
        RefreshBusPolicies();
        return std::nullopt;
    }

    const std::vector<AudioBusDefinition>& AudioEngine::GetBuses() const
    {
        static const std::vector<AudioBusDefinition> empty;
        return m_Impl->Buses ? m_Impl->Buses->GetBuses() : empty;
    }

    std::optional<float> AudioEngine::GetBusVolumeDb(const std::string& bus) const
    {
        const auto* definition = m_Impl->Buses ? m_Impl->Buses->Find(bus) : nullptr;
        return definition ? std::optional<float>(definition->VolumeDb) : std::nullopt;
    }

    bool AudioEngine::IsBusPaused(const std::string& bus) const
    {
        const auto* policy = m_Impl->BusPolicy(bus);
        return policy &&
               ((m_Impl->GamePaused && policy->PausesWithGame) || (m_Impl->TimeScale == 0 && policy->FollowTimeScale));
    }

    void AudioEngine::SetGamePaused(bool paused)
    {
        if (m_Impl->GamePaused != paused)
        {
            m_Impl->GamePaused = paused;
            RefreshBusPolicies();
        }
    }

    void AudioEngine::SetTimeScale(float scale)
    {
        if (std::isfinite(scale) && scale >= 0 && m_Impl->TimeScale != scale)
        {
            m_Impl->TimeScale = scale;
            if (scale > 0)
                m_Impl->PitchTimeScale = std::clamp(scale, .05f, 4.f);
            RefreshBusPolicies();
        }
    }

    void AudioEngine::RefreshBusPolicies()
    {
        for (uint32_t i = 0; i < m_Impl->Slots.size(); ++i)
            if (const auto& slot = m_Impl->Slots[i]; slot.Value)
                RefreshBusPolicy({i, slot.Generation});
    }

    void AudioEngine::RefreshBusPolicy(AudioVoiceHandle handle)
    {
        auto&      voice = *m_Impl->Find(handle);
        const bool held  = IsBusPaused(voice.Desc.Bus);
        if (held && voice.Active(GetClockFrames()))
        {
            Pause(handle);
            voice.BusHeld = true;
        }
        ApplyVoicePitch(handle, true);
        if (!held && voice.BusHeld)
            Play(handle);
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
        result             = ma_decoder_get_data_format(&decoder,
                                                        nullptr,
                                                        &sound.Info.Channels,
                                                        &sound.Info.SampleRate,
                                                        sound.ChannelMap.data(),
                                                        sound.ChannelMap.size());
        if (result == MA_SUCCESS)
            result = ma_decoder_get_length_in_pcm_frames(&decoder, &length);
        ma_decoder_uninit(&decoder);
        if (result != MA_SUCCESS || sound.Info.Channels == 0 || sound.Info.Channels > MA_MAX_CHANNELS ||
            sound.Info.SampleRate == 0 || length == 0 || length > std::vector<float>().max_size() / sound.Info.Channels)
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
        ma_channel_map_init_standard(
            ma_standard_channel_map_default, sound.ChannelMap.data(), sound.ChannelMap.size(), channels);
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
            voice->Virtual = voice->Stolen = voice->Scheduled = voice->BusHeld = false;
            voice->PausedAt = voice->PinnedStart = NoFrame;
            voice->Timed.store(false);
            voice->PendingSeek.store(NoFrame);
            voice->Envelope   = {};
            auto& node        = voice->Sound;
            node.ResetSpatialMix();
            node.PitchMailbox.Reset();
            node.Pitch = {};
            ma_sound_set_pitch(&node, 1);
            voice->EventAttenuationAsset = voice->AttenuationID = {};
            voice->AttenuationAssets                            = nullptr;
            voice->AttenuationLifetime.reset();
            voice->AttenuationRegistry.reset();
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
                                              loopEnd,
                                              sound.ChannelMap.data(),
                                              m_Impl->Engine.listeners[0].config.pChannelMapOut,
                                              GetChannels(),
                                              desc.Spatial);
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
            node.ListenerIdentities            = &m_Impl->MixerListenerIdentities;
            node.NativePanFloor                = node.engineNode.spatializer.minSpatializationChannelGain;
            node.Original                      = node.engineNode.baseNode.vtable;
            static const ma_node_vtable vtable = {Impl::Voice::Node::Process, nullptr, 0, 1, 0};
            node.engineNode.baseNode.vtable    = &vtable;
            ma_sound_set_pan_mode(&node, ma_pan_mode_balance);
            result = voice->Route.Init(m_Impl->Engine, m_Impl->LfeChannel);
            if (result == MA_SUCCESS)
                result = ma_node_attach_output_bus(&node, 0, &voice->Route, 0);
            if (result != MA_SUCCESS)
            {
                Warn(Error("Initialize voice routing", result));
                return {};
            }
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
        SetVoiceRouting(handle, desc.ReverbZoneSend, desc.LfeSendDb, desc.PinToCenter);
        SetPan(handle, desc.Pan);
        if (desc.Spatial)
        {
            SetSpatialParams(handle, {});
        }
        else
            slot.Value->Listener = m_Impl->ReverbListener(*slot.Value);
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
        desc.ReverbPosition = params.Position;
        return PlayRegisteredOneShot(
            registry, clip, desc, params.Position && params.Spatial ? &spatial : nullptr, 0, nullptr);
    }

    AudioVoiceHandle AudioEngine::PlayRegisteredOneShot(AudioClipRegistry&                           registry,
                                                        const std::shared_ptr<const AudioClipAsset>& clip,
                                                        AudioVoiceDesc&                              desc,
                                                        const AudioSpatialParams*                    spatial,
                                                        double                                       startOffsetSeconds,
                                                        bool*                                        rejectedByLimit,
                                                        AssetID                                      eventGroup,
                                                        AssetManager*                                assets,
                                                        AssetID                                      eventAttenuation,
                                                        AssetID attenuationOverride)
    {
        if (IsBusPaused(desc.Bus))
            return {};
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
        {
            SetSpatialParams(handle, *spatial);
            voice->EventAttenuationAsset = eventAttenuation;
            SetVoiceAttenuationAsset(handle, assets, attenuationOverride);
        }
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
            if (const auto result = ma_node_attach_output_bus(&voice->Route, 0, input, 0); result != MA_SUCCESS)
            {
                Warn(Error("Move audio voice", result));
                return false;
            }
        }
        voice->Desc.Bus = bus;
        RefreshBusPolicy(handle);
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
                voice->Attenuation.reset();
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
                        (!slot.Value->BusHeld && !slot.Value->Active(now)) ||
                        std::any_of(victims.begin(), victims.end(), [i](const auto& v) { return v.Handle.Index == i; }))
                        continue;
                    ++count;
                    const auto& member = *slot.Value;
                    float       score  = 0;
                    if (definition.Rule == AudioConcurrencyRule::StopQuietest)
                        score = member.BusHeld ? 0 : p.Audibility(member, now);
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
            if (voice->BusHeld && IsBusPaused(voice->Desc.Bus))
                return true;
            if (!std::exchange(voice->BusHeld, false) && !AdmitVoice(handle, now))
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
            if (IsBusPaused(voice->Desc.Bus))
            {
                Pause(handle);
                voice->BusHeld = true;
                return true;
            }
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
        if (IsBusPaused(voice->Desc.Bus))
        {
            voice->Scheduled = true;
            Pause(handle);
            voice->BusHeld = true;
        }
        else
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
            voice->BusHeld     = false;
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
            voice->BusHeld   = false;
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
    bool AudioEngine::UnPause(AudioVoiceHandle handle)
    {
        const auto* voice = m_Impl->Find(handle);
        return voice && voice->PausedAt != NoFrame && Play(handle);
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
            ApplyVoicePitch(handle, false);
        }
    }

    void AudioEngine::ApplyVoicePitch(AudioVoiceHandle handle, bool ramp)
    {
        auto&       voice      = *m_Impl->Find(handle);
        auto        state      = voice.Current();
        const auto* policy     = m_Impl->BusPolicy(voice.Desc.Bus);
        const bool  follows    = policy && policy->FollowTimeScale;
        const auto  multiplier = follows ? m_Impl->PitchTimeScale : 1.f;
        const auto  pitch      = voice.Desc.Pitch * multiplier;
        if (!std::isfinite(pitch) || pitch <= 0)
            return;
        const auto rate   = double(pitch) * voice.Info.SampleRate / GetSampleRate();
        const bool driven = follows || state.Pitch.Driven;
        if (state.Rate == rate && state.Pitch.Driven == driven)
            return;
        state.Frame           = driven ? m_Impl->WritableClock() : GetClockFrames();
        state.Source          = state.Position(double(state.Frame), voice.StreamDelay.load());
        state.StreamDelayBase = voice.StreamDelay.load();
        state.Start           = std::max(state.Start, state.Frame);
        state.Pitch           = {
            state.Pitch.At(double(state.Frame)),
            pitch,
            state.Frame,
            ramp && (state.Start != NoFrame || voice.PausedAt != NoFrame) ? std::max(1u, GetSampleRate() / 100) : 0u,
            driven};
        state.Rate = rate;
        voice.Record(state, GetPlaybackClockFrames());
        if (driven)
            voice.Sound.PitchMailbox.Publish(state.Pitch);
        else
            ma_sound_set_pitch(&voice.Sound, pitch);
    }
    void AudioEngine::SetPan(AudioVoiceHandle handle, float pan)
    {
        if (auto* voice = m_Impl->Find(handle); voice && std::isfinite(pan))
        {
            voice->Desc.Pan = std::clamp(pan, -1.f, 1.f);
            ma_sound_set_pan(&voice->Sound, !voice->Desc.Spatial && voice->Desc.PinToCenter ? 0 : voice->Desc.Pan);
        }
    }
    void AudioEngine::SetLooping(AudioVoiceHandle handle, bool looping)
    {
        if (auto* voice = m_Impl->Find(handle); voice && voice->Desc.Loop != looping)
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
    void AudioEngine::SeekSeconds(AudioVoiceHandle handle, double seconds)
    {
        if (const auto* voice = m_Impl->Find(handle); voice && std::isfinite(seconds) && seconds >= 0)
        {
            const auto frame = seconds * voice->Info.SampleRate;
            SeekFrames(handle,
                       frame >= voice->Info.LengthFrames ? voice->Info.LengthFrames : static_cast<uint64_t>(frame));
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
        voice->Spatial = params;
        m_Impl->RefreshAttenuation(*voice);
        m_Impl->ApplySpatial(*voice);
    }

    void AudioEngine::SetVoiceAttenuationAsset(AudioVoiceHandle handle, AssetManager* assets, AssetID id)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || !voice->Desc.Spatial)
            return;
        if (!id.IsValid())
            id = voice->EventAttenuationAsset;
        if (!id.IsValid())
            assets = nullptr;
        if (voice->AttenuationID == id && voice->AttenuationAssets == assets &&
            (!assets || !voice->AttenuationLifetime.expired()))
            return;
        const bool hadAsset        = bool(voice->Attenuation);
        voice->AttenuationID       = id;
        voice->AttenuationAssets   = assets;
        voice->AttenuationLifetime = assets ? assets->m_Lifetime : std::weak_ptr<const int> {};
        voice->AttenuationRegistry.reset();
        voice->Attenuation.reset();
        const bool changed = m_Impl->RefreshAttenuation(*voice);
        if (changed || hadAsset)
            m_Impl->ApplySpatial(*voice);
    }

    void AudioEngine::SetVoiceEmitter(AudioVoiceHandle handle, const AudioSpatialParams& emitter, bool followPoint)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || !voice->Desc.Spatial)
            return;
        if (!followPoint && voice->Spatial.Shape.Type == AudioSourceShape::Point &&
            emitter.Shape.Type == AudioSourceShape::Point && !voice->AttenuationID.IsValid())
            return; // Legacy point events snapshot their position at trigger time.
        auto params           = voice->Spatial;
        params.Position       = emitter.Position;
        params.Velocity       = emitter.Velocity;
        params.Shape          = emitter.Shape;
        params.ShapeTransform = emitter.ShapeTransform;
        if (params.Shape.Type != AudioSourceShape::Point || voice->AttenuationID.IsValid())
            params.Direction = emitter.Direction;
        SetSpatialParams(handle, params);
    }

    uint32_t AudioEngine::GetMaxListeners() const { return m_Impl->Config.MaxListeners; }

    std::optional<size_t> AudioEngine::ResolveReverbBus(const std::string& name)
    {
        auto& impl = *m_Impl;
        if (!impl.Buses)
            return std::nullopt;
        if (auto index = impl.Buses->FindIndex(name))
            return index;
        if (impl.MissingReverbBuses.find(name) == impl.MissingReverbBuses.end())
        {
            impl.MissingReverbBuses.emplace(name);
            Warn("Unknown reverb zone audio bus: " + name);
        }
        return std::nullopt;
    }

    void AudioEngine::SetVoiceRouting(AudioVoiceHandle handle, bool reverbZoneSend, float lfeSendDb, bool pinToCenter)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice)
            return;
        voice->Desc.ReverbZoneSend = reverbZoneSend;
        voice->Desc.LfeSendDb      = lfeSendDb;
        voice->Desc.PinToCenter    = pinToCenter;
        voice->Source->LfeGain.store(m_Impl->LfeChannel < 0 ? 0.f : RoutingGain(lfeSendDb), std::memory_order_relaxed);
        voice->Source->PinToCenter.store(pinToCenter, std::memory_order_relaxed);
        SetPan(handle, voice->Desc.Pan);
        m_Impl->UpdateVoiceSends(*voice);
    }

    void AudioEngine::SetVoiceReverbPosition(AudioVoiceHandle handle, glm::vec3 position)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || voice->Desc.Spatial)
            return;
        voice->Desc.ReverbPosition = position;
        voice->Listener            = m_Impl->ReverbListener(*voice);
        m_Impl->UpdateVoiceSends(*voice);
    }

    void AudioEngine::SetListeners(const AudioListenerState* states, size_t count)
    {
        auto& impl = *m_Impl;
        if (!IsInitialized())
            return;
        for (uint32_t i = 0; i < impl.Config.MaxListeners; ++i)
        {
            if (i < count)
                impl.Listeners[i] = states[i];
            else
            {
                impl.Listeners[i]        = {};
                impl.Listeners[i].Active = false;
            }
        }
        impl.ListenerMailbox.Publish(impl.Listeners);
        for (auto& slot : impl.Slots)
        {
            if (!slot.Value)
                continue;
            if (slot.Value->Desc.Spatial)
                impl.ApplySpatial(*slot.Value);
            else
            {
                slot.Value->Listener = impl.ReverbListener(*slot.Value);
                impl.UpdateVoiceSends(*slot.Value);
            }
        }
    }

    void AudioEngine::SetListener(uint32_t index, const AudioListenerState& state)
    {
        if (!IsInitialized() || index >= m_Impl->Config.MaxListeners)
            return;
        auto states   = m_Impl->Listeners;
        states[index] = state;
        SetListeners(states.data(), m_Impl->Config.MaxListeners);
    }
}
