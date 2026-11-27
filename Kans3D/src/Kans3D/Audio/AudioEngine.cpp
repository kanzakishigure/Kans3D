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
#include "AudioHRTF.h"
#include "AudioProfileCapture.h"
#include "SteamAudio.h"

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

        AudioOutputFormat StandardOutputFormat(uint32_t channels)
        {
            AudioOutputFormat format {channels};
            ma_channel_map_init_standard(
                ma_standard_channel_map_default, format.ChannelMap.data(), format.ChannelMap.size(), channels);
            return format;
        }

        AudioOutputFormat NormalizeOutputFormat(AudioOutputFormat format, uint32_t fallback)
        {
            if (format.Channels == 0 || format.Channels > MaxOutputChannels)
                return StandardOutputFormat(fallback);
            std::fill(format.ChannelMap.begin() + format.Channels, format.ChannelMap.end(), MA_CHANNEL_NONE);
            if (std::find(format.ChannelMap.begin(), format.ChannelMap.begin() + format.Channels, MA_CHANNEL_NONE) !=
                    format.ChannelMap.begin() + format.Channels ||
                !ma_channel_map_is_valid(format.ChannelMap.data(), format.Channels))
                return StandardOutputFormat(format.Channels);
            return format;
        }

        AudioOutputFormat MixFormatFor(SpeakerMode mode, const AudioOutputFormat& native)
        {
            switch (mode)
            {
                case SpeakerMode::Mono:
                    return StandardOutputFormat(1);
                case SpeakerMode::Stereo:
                    return StandardOutputFormat(2);
                case SpeakerMode::Surround51:
                    return StandardOutputFormat(6);
                case SpeakerMode::Surround71:
                    return StandardOutputFormat(8);
                default:
                    return native;
            }
        }

        std::string DeviceId(ma_backend backend, const ma_device_id& id)
        {
            std::string_view bytes;
            switch (backend)
            {
                case ma_backend_wasapi: {
                    size_t length = 0;
                    while (length < std::size(id.wasapi) && id.wasapi[length])
                        ++length;
                    bytes = {reinterpret_cast<const char*>(id.wasapi), length * sizeof(id.wasapi[0])};
                    break;
                }
                case ma_backend_dsound:
                    bytes = {reinterpret_cast<const char*>(id.dsound), sizeof(id.dsound)};
                    break;
                case ma_backend_winmm:
                    bytes = {reinterpret_cast<const char*>(&id.winmm), sizeof(id.winmm)};
                    break;
                case ma_backend_pulseaudio:
                    bytes = id.pulse;
                    break;
                case ma_backend_alsa:
                    bytes = id.alsa;
                    break;
                case ma_backend_jack:
                    bytes = {reinterpret_cast<const char*>(&id.jack), sizeof(id.jack)};
                    break;
                default:
                    return {};
            }
            constexpr char digits[] = "0123456789abcdef";
            std::string    key      = ma_get_backend_name(backend);
            key += ':';
            for (const auto byte : bytes)
            {
                const auto value = static_cast<unsigned char>(byte);
                key += digits[value >> 4];
                key += digits[value & 15];
            }
            return key;
        }

        struct OutputConverter
        {
            ma_channel_converter Converter {};
            bool                 Ready = false;
            ~OutputConverter()
            {
                if (Ready)
                    ma_channel_converter_uninit(&Converter, nullptr);
            }

            ma_result Init(const AudioOutputFormat& mix, AudioOutputFormat output)
            {
                auto                                                mode = ma_channel_mix_mode_rectangular;
                std::array<std::array<float, 2>, MaxOutputChannels> weights {};
                std::array<float*, MaxOutputChannels>               rows {};
                if (mix.Channels > 2 && output.Channels <= 2)
                {
                    mode = ma_channel_mix_mode_custom_weights;
                    for (uint32_t ch = 0; ch < mix.Channels; ++ch)
                    {
                        const auto stereo = StereoDownmix(mix.ChannelMap[ch]);
                        if (output.Channels == 1)
                            weights[ch][0] = AudioMinus3Db * (stereo[0] + stereo[1]);
                        else
                            for (uint32_t speaker = 0; speaker < 2; ++speaker)
                                weights[ch][speaker] = stereo[output.ChannelMap[speaker] == MA_CHANNEL_FRONT_RIGHT];
                        rows[ch] = weights[ch].data();
                    }
                    // The mono shortcut averages every input, including LFE. Use the explicit matrix.
                    if (output.Channels == 1)
                        output.ChannelMap[0] = MA_CHANNEL_FRONT_CENTER;
                }
                else if (mix.Channels <= output.Channels && mix.Channels > 1)
                    mode = ma_channel_mix_mode_simple;
                auto config       = ma_channel_converter_config_init(ma_format_f32,
                                                                     mix.Channels,
                                                                     mix.ChannelMap.data(),
                                                                     output.Channels,
                                                                     output.ChannelMap.data(),
                                                                     mode);
                config.ppWeights  = rows.data();
                const auto result = ma_channel_converter_init(&config, nullptr, &Converter);
                Ready             = result == MA_SUCCESS;
                return result;
            }
        };

        struct AcousticMix
        {
            std::array<float, 3> Dry {1, 1, 1};
            float                Visibility = 1, Wet = 1, Filter = 1;
            bool                 operator==(const AcousticMix& other) const
            {
                return Dry == other.Dry && Visibility == other.Visibility && Wet == other.Wet && Filter == other.Filter;
            }
            bool operator!=(const AcousticMix& other) const { return !(*this == other); }
        };

        struct SpatialMix
        {
            std::array<float, MaxOutputChannels> Gains {1, 1, 1, 1, 1, 1, 1, 1};
            float                                FilterCoefficient = 1;
            bool                                 CustomPan         = false;
            uint32_t                             Listener          = 0;
            bool                                 HasNative         = false;
            AudioSpatialParams                   Native;
            float                                BinauralBlend = 0, BinauralGain = 1;
            bool                                 BinauralEnabled = false, BinauralReversed = false;
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
                       Native.ConeOuterGain == other.Native.ConeOuterGain && BinauralBlend == other.BinauralBlend &&
                       BinauralGain == other.BinauralGain && BinauralEnabled == other.BinauralEnabled &&
                       BinauralReversed == other.BinauralReversed;
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

            void Reset(const T& initial = {})
            {
                Exchanged.store(2, std::memory_order_relaxed);
                WriteSlot = 0;
                ReadSlot  = 1;
                Published = initial;
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

        float OnePole(float sample, float& history, float coefficient)
        {
            history = coefficient == 1 ? sample : glm::mix(history, sample, coefficient);
            return history;
        }

        AcousticMix EvaluateAcoustics(const AudioOcclusionResult& result, float clear, uint32_t rate)
        {
            AcousticMix mix;
            // Geometry alone leaves a small direct residual. Transmission then
            // accounts for the additional loss along obstructed paths.
            const float obstructionGain = 1 - .75f * result.Occlusion;
            mix.Visibility              = obstructionGain * clear;
            for (size_t band = 0; band < mix.Dry.size(); ++band)
                mix.Dry[band] = obstructionGain * result.Transmission[band];
            mix.Wet = .5f + .5f * mix.Dry[1];
            if (mix.Dry[0] != mix.Dry[2])
            {
                const float midpoint = std::clamp((mix.Dry[1] - mix.Dry[2]) / (mix.Dry[0] - mix.Dry[2]), 0.f, 1.f);
                mix.Filter           = AirFilterCoefficient(glm::mix(600.f, 4000.f, midpoint), rate);
            }
            return mix;
        }

        glm::vec3 UnitVector(glm::vec3 value, glm::vec3 fallback)
        {
            const auto squared = glm::dot(value, value);
            return squared > 1e-12f && std::isfinite(squared) ? value / std::sqrt(squared) : fallback;
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

        void WarnSteamAudio(const std::string& reason)
        {
            static std::atomic_flag warned = ATOMIC_FLAG_INIT;
            if (!warned.test_and_set(std::memory_order_relaxed))
                Warn("Steam Audio HRTF unavailable, using panning: " + reason);
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
            bool                            Generator = false;
            std::atomic<uint64_t>           UnderrunFrames {0}, Underruns {0};
            std::atomic<uint64_t>           DecodeNanoseconds {0}, DecodePasses {0};

            void Starved(uint64_t frames)
            {
                UnderrunFrames.fetch_add(frames, std::memory_order_relaxed);
                Underruns.fetch_add(1, std::memory_order_release);
            }

            uint64_t BufferedFrames() const
            {
                const auto read  = ReadFrame.load(std::memory_order_acquire);
                const auto write = WriteFrame.load(std::memory_order_acquire);
                return std::min(Capacity, write >= read ? write - read : 0);
            }

            uint32_t PushRoom() const
            {
                return static_cast<uint32_t>(Capacity - (WriteFrame.load(std::memory_order_relaxed) -
                                                         ReadFrame.load(std::memory_order_acquire)));
            }

            uint32_t Push(const float* samples, size_t sampleCount)
            {
                if (!samples || sampleCount % Info.Channels != 0)
                    return 0;
                const auto count = std::min<size_t>(sampleCount / Info.Channels, PushRoom());
                if (!count)
                    return 0;
                const auto write  = WriteFrame.load(std::memory_order_relaxed);
                const auto offset = write % Capacity;
                const auto first  = std::min<uint64_t>(count, Capacity - offset);
                std::copy_n(samples, first * Info.Channels, PCM.data() + offset * Info.Channels);
                std::copy_n(samples + first * Info.Channels, (count - first) * Info.Channels, PCM.data());
                WriteFrame.store(write + count, std::memory_order_release);
                return static_cast<uint32_t>(count);
            }

            ma_result ReadGenerator(void* output, ma_uint64 count, ma_uint64& read)
            {
                const auto start = ReadFrame.load(std::memory_order_relaxed);
                const auto ready = std::min<uint64_t>(count, WriteFrame.load(std::memory_order_acquire) - start);
                if (output)
                {
                    const auto offset  = start % Capacity;
                    const auto first   = std::min(ready, Capacity - offset);
                    auto*      samples = static_cast<float*>(output);
                    std::copy_n(PCM.data() + offset * Info.Channels, first * Info.Channels, samples);
                    std::copy_n(PCM.data(), (ready - first) * Info.Channels, samples + first * Info.Channels);
                    std::fill_n(samples + ready * Info.Channels, (count - ready) * Info.Channels, 0.f);
                }
                ReadFrame.store(start + ready, std::memory_order_release);
                Cursor.fetch_add(count, std::memory_order_relaxed);
                if (ready < count)
                    Starved(count - ready);
                read = count;
                return MA_SUCCESS;
            }
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
                if (ConverterReady)
                {
                    ma_channel_converter_uninit(&Converter, nullptr);
                    ConverterReady = false;
                }
                RoutingScratch.clear();
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
                if (Generator)
                    return ReadGenerator(out, count, read);
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
                           bool                 spatial,
                           bool                 generator,
                           uint32_t             streamBufferFrames)
            {
                Generator = generator;
                Offline = offline;
                Stream    = stream;
                Info      = info;
                LoopStart = loopStart;
                LoopEnd   = loopEnd;
                ma_result result;
                if (Generator)
                {
                    Capacity = std::max(2048u, Info.SampleRate / 4);
                    PCM.resize(Capacity * Info.Channels);
                }
                else if (Stream)
                {
                    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
                    result            = ma_decoder_init_vfs(&vfs, path, &config, &Decoder);
                    if (result != MA_SUCCESS)
                        return result;
                    DecoderReady = true;
                    Capacity     = streamBufferFrames ? streamBufferFrames : std::max(2048u, Info.SampleRate / 4);
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
                    if (!Stream && !Generator)
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
                        if (self.Generator)
                            return MA_SUCCESS;
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
                        if (!self.Stream && !self.Generator)
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
                if (result != MA_SUCCESS && !Stream && !Generator)
                    ma_resource_manager_data_source_uninit(&Data);
                Initialized = result == MA_SUCCESS;
                return result;
            }

            void Fill(bool profile)
            {
                if (!Offline && !Producing.load(std::memory_order_acquire))
                    return;
                const auto revision = SeekRevision.load(std::memory_order_acquire);
                auto       write    = WriteFrame.load(std::memory_order_relaxed);
                const bool reset    = ReadyRevision.load(std::memory_order_relaxed) != revision;
                const bool measure =
                    profile && (reset || (!Eof.load(std::memory_order_relaxed) &&
                                          write - ReadFrame.load(std::memory_order_acquire) < Capacity));
                const auto start = measure ? AudioProfileClock::now() : AudioProfileClock::time_point {};
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
                if (measure)
                {
                    const auto elapsed = ProfileNanoseconds(AudioProfileClock::now() - start);
                    DecodeNanoseconds.store(DecodeNanoseconds.load(std::memory_order_relaxed) + elapsed,
                                            std::memory_order_relaxed);
                    DecodePasses.store(DecodePasses.load(std::memory_order_relaxed) + 1, std::memory_order_release);
                }
            }

            ma_result Read(void* out, uint64_t count, ma_uint64& read)
            {
                if (Offline)
                    Fill(AudioProfileActiveMix != nullptr);
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
                    if (!Stream && !Generator)
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
                SteamAudioVoice                                Binaural;
                uint32_t                                       BinauralTail = 0;
                std::atomic<bool>                              BinauralBuffered {false}, ResetBinauralPending {false};

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
                                 SpatialTarget.CustomPan != next.CustomPan ||
                                 SpatialTarget.BinauralGain != next.BinauralGain)
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

                void ApplySpatialMix(float* output, uint32_t frames, uint32_t channels, float* mono = nullptr)
                {
                    // The mixer leaves legacy PCM untouched, including the original miniaudio gainer.
                    if (!mono && !SpatialRamp && SpatialCurrent.Gains == SpatialMix {}.Gains &&
                        SpatialCurrent.FilterCoefficient == 1)
                        return;
                    for (uint32_t frame = 0; frame < frames; ++frame)
                    {
                        if (SpatialRamp)
                        {
                            const float step = 1.f / SpatialRamp;
                            SpatialCurrent.FilterCoefficient =
                                glm::mix(SpatialCurrent.FilterCoefficient, SpatialTarget.FilterCoefficient, step);
                            SpatialCurrent.BinauralGain =
                                glm::mix(SpatialCurrent.BinauralGain, SpatialTarget.BinauralGain, step);
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
                            sample        = OnePole(sample, history, coefficient) * SpatialCurrent.Gains[ch];
                        }
                        if (mono)
                            mono[frame] = (AirHistory[0] + AirHistory[1]) * (0.5f * SpatialCurrent.BinauralGain);
                    }
                }

                void ResetBinaural()
                {
                    Binaural.Reset();
                    BinauralTail = 0;
                    BinauralBuffered.store(false, std::memory_order_release);
                }

                void ProcessSpatial(float* output, uint32_t& frames, uint32_t requested, uint32_t channels)
                {
                    const float blend = SpatialTarget.BinauralBlend;
                    if (!SpatialTarget.BinauralEnabled)
                    {
                        if (Binaural.HasSignal())
                            ResetBinaural();
                        ApplySpatialMix(output, frames, channels);
                        return;
                    }

                    const auto& engine   = *ma_sound_get_engine(this);
                    const auto& listener = engine.listeners[ma_sound_get_pinned_listener_index(this)];
                    // The listener mailbox is consumed by this same mixer before processing voices.
                    // Read its owned pose directly, avoiding the native spatializer's vector locks.
                    const auto& pos       = listener.position.v;
                    const auto& look      = listener.direction.v;
                    const auto& up        = listener.config.worldUp;
                    const auto  forward   = UnitVector({look.x, look.y, look.z}, {0, 0, -1});
                    const auto  right     = UnitVector(glm::cross(forward, glm::vec3(up.x, up.y, up.z)), {1, 0, 0});
                    const auto  toSource  = SpatialTarget.Native.Position - glm::vec3(pos.x, pos.y, pos.z);
                    const auto  direction = UnitVector({glm::dot(toSource, right),
                                                        glm::dot(toSource, glm::cross(right, forward)),
                                                        -glm::dot(toSource, forward)},
                                                       {0, 0, -1});
                    const bool  reversed  = SpatialTarget.BinauralReversed;
                    std::array<float, SteamAudioMaxBlock> mono;
                    for (uint32_t offset = 0; offset < frames;)
                    {
                        const auto count       = std::min<uint32_t>(mono.size(), frames - offset);
                        auto*      destination = output + offset * channels;
                        ApplySpatialMix(destination, count, channels, mono.data());
                        Binaural.Process(destination, mono.data(), count, direction, blend, reversed);
                        offset += count;
                    }
                    const bool ended = ma_sound_at_end(this) != MA_FALSE;
                    if (ended && !BinauralTail && Binaural.HasSignal())
                        BinauralTail = Binaural.DrainFrames();
                    if (BinauralTail || (!ended && frames < requested))
                    {
                        const auto count =
                            BinauralTail ? std::min(requested - frames, BinauralTail) : requested - frames;
                        std::fill_n(output + frames * channels, count * channels, 0.f);
                        if (count && Binaural.HasSignal())
                            Binaural.Process(output + frames * channels, nullptr, count, direction, blend, reversed);
                        if (BinauralTail)
                        {
                            BinauralTail -= count;
                            if (!BinauralTail)
                                ResetBinaural();
                        }
                        frames += count;
                    }
                    if (!Binaural.HasSignal())
                        BinauralBuffered.store(false, std::memory_order_release);
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
                    ResetBinaural();
                    ResetBinauralPending.store(false, std::memory_order_relaxed);
                }

                void
                ProcessNative(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount)
                {
                    if (!Owner->Source->Generator)
                    {
                        Original->onProcess(node, in, inCount, out, outCount);
                        return;
                    }

                    // ma_sound normally requests its full cache from an asset. A live producer
                    // supplies just enough input to render the requested block, so a short pull
                    // cannot consume samples or report silence for future output.
                    const auto pitch      = ma_sound_get_pitch(this);
                    const auto doppler    = engineNode.spatializer.dopplerPitch;
                    const auto engineRate = ma_engine_get_sample_rate(engineNode.pEngine);
                    const bool bypass     = pitch == 1 && doppler == 1 && engineNode.sampleRate == engineRate;
                    if (engineNode.isPitchDisabled != bypass)
                    {
                        engineNode.isPitchDisabled = bypass;
                        if (!bypass)
                            ResetResampler();
                    }
                    if (!bypass && (pitch != engineNode.oldPitch || doppler != engineNode.oldDopplerPitch))
                    {
                        engineNode.oldPitch        = pitch;
                        engineNode.oldDopplerPitch = doppler;
                        ma_resampler_set_rate_ratio(&engineNode.resampler,
                                                    float(engineNode.sampleRate) / engineRate * pitch * doppler);
                    }
                    const auto capacity  = processingCacheCap;
                    const auto requested = *outCount;
                    const auto channels  = ma_node_get_output_channels(node, 0);
                    *outCount            = 0;
                    while (*outCount < requested)
                    {
                        ma_uint64 count    = requested - *outCount;
                        ma_uint64 required = count;
                        if (!bypass)
                        {
                            if (ma_resampler_get_required_input_frame_count(&engineNode.resampler, count, &required) !=
                                MA_SUCCESS)
                                break;
                            if (required > capacity)
                            {
                                ma_uint64 fit = 0;
                                ma_resampler_get_expected_output_frame_count(&engineNode.resampler, capacity, &fit);
                                count = std::max<ma_uint64>(1, std::min(fit, count));
                                ma_resampler_get_required_input_frame_count(&engineNode.resampler, count, &required);
                            }
                        }
                        else
                            required = count = std::min<ma_uint64>(capacity, count);
                        const auto cached  = processingCacheFramesRemaining;
                        processingCacheCap = static_cast<ma_uint32>(
                            std::clamp<ma_uint64>(required - std::min<ma_uint64>(required, cached), 1, capacity));
                        float* output = out[0] + size_t(*outCount) * channels;
                        auto   mixed  = static_cast<ma_uint32>(count);
                        Original->onProcess(node, in, inCount, &output, &mixed);
                        *outCount += mixed;
                        if (mixed < count)
                            break;
                    }
                    processingCacheCap = capacity;
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
                        ProcessNative(node, in, inCount, &output, &mixed);
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
                    if (self.Pitch.Driven && !voice.Source->Generator)
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
                        if (!voice.Source->Generator && self.engineNode.isPitchDisabled != bypass)
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
                    if (voice.Desc.Spatial)
                    {
                        if (self.ResetBinauralPending.load(std::memory_order_acquire) &&
                            self.ResetBinauralPending.exchange(false, std::memory_order_acq_rel))
                            self.ResetBinaural();
                        if (self.SpatialTarget.BinauralEnabled && !self.Binaural.HasSignal())
                            self.BinauralBuffered.store(true, std::memory_order_release);
                    }
                    if (self.BinauralTail)
                        *outCount = 0;
                    else if (self.Pitch.Driven)
                        self.ProcessPitch(node, in, inCount, out, outCount);
                    else
                        self.ProcessNative(node, in, inCount, out, outCount);
                    if (*outCount < requested && voice.Source->Stream && !self.BinauralTail && !ma_sound_at_end(&self))
                    {
                        const auto missing = requested - *outCount;
                        voice.Source->Starved(missing);
                        if (!voice.Source->Offline)
                            voice.StreamDelay.fetch_add(missing, std::memory_order_relaxed);
                    }
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
                        self.ProcessSpatial(out[0], *outCount, requested, channels);
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
                    ResetBinaural();
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

                MixMailbox<AcousticMix>                 AcousticMailbox;
                AcousticMix                             AcousticTarget;
                std::array<AudioGainRamp, 3>            DryAcoustics;
                AudioGainRamp                           Visibility, WetAcoustics, AcousticFilter;
                std::array<float, MaxOutputChannels>    AcousticHistory {};
                std::unique_ptr<SteamAudioDirectEffect> DirectOwner;
                static_assert(std::atomic<SteamAudioDirectEffect*>::is_always_lock_free);
                std::atomic<SteamAudioDirectEffect*> Direct {nullptr};
                // Steam Audio 4.8.1 retains four gain interpolator samples with a one-sample effect.
                static constexpr uint32_t DirectGainLag   = 4;
                uint32_t                  DirectSettling  = 0;
                bool                      DirectAttempted = false, MixedAcoustics = false;

                void PrepareDirect(const SteamAudio* steam)
                {
                    if (DirectAttempted || !steam)
                        return;
                    DirectAttempted = true;
                    DirectOwner     = steam->CreateDirectEffect(Channels);
                    Direct.store(DirectOwner.get(), std::memory_order_release);
                }

                void InitializeAcoustics(const AcousticMix& mix)
                {
                    const auto initialize = [](AudioGainRamp& value, float target) {
                        value.Remaining = 0;
                        value.Initialize(target);
                    };
                    for (size_t band = 0; band < mix.Dry.size(); ++band)
                        initialize(DryAcoustics[band], mix.Dry[band]);
                    initialize(Visibility, mix.Visibility);
                    initialize(WetAcoustics, mix.Wet);
                    initialize(AcousticFilter, mix.Filter);
                }

                // Control thread only, with the route detached.
                void ResetAcoustics(const AcousticMix& mix)
                {
                    AcousticMailbox.Reset(mix);
                    AcousticTarget = mix;
                    InitializeAcoustics(mix);
                    AcousticHistory = {};
                    if (DirectSettling)
                        DirectOwner->Reset();
                    DirectSettling = 0;
                    MixedAcoustics = false;
                }

                void ProcessAcoustics(float** output, uint32_t frames)
                {
                    if (!frames)
                        return;
                    if (AcousticMailbox.Consume(AcousticTarget))
                    {
                        if (!MixedAcoustics)
                            InitializeAcoustics(AcousticTarget);
                        else
                        {
                            const auto dryRamp = Direct.load(std::memory_order_acquire) && RampFrames > DirectGainLag ?
                                                     RampFrames - DirectGainLag :
                                                     RampFrames;
                            for (size_t band = 0; band < AcousticTarget.Dry.size(); ++band)
                                DryAcoustics[band].Prepare(dryRamp, AcousticTarget.Dry[band]);
                            Visibility.Prepare(dryRamp, AcousticTarget.Visibility);
                            WetAcoustics.Prepare(RampFrames, AcousticTarget.Wet);
                            AcousticFilter.Prepare(RampFrames, AcousticTarget.Filter);
                        }
                    }
                    MixedAcoustics = true;
                    if (AcousticTarget == AcousticMix {} && !Visibility.Remaining && !WetAcoustics.Remaining &&
                        !DryAcoustics[0].Remaining && !DryAcoustics[1].Remaining && !DryAcoustics[2].Remaining &&
                        !AcousticFilter.Remaining && !DirectSettling)
                        return;

                    auto*      direct  = Direct.load(std::memory_order_acquire);
                    const bool profile = direct && AudioProfileActiveMix && AudioProfileActiveMix->Frame;
                    const auto start   = profile ? AudioProfileClock::now() : AudioProfileClock::time_point {};
                    for (uint32_t frame = 0; frame < frames; ++frame)
                    {
                        const auto                 offset = size_t(frame) * Channels;
                        auto*                      dry    = output[0] + offset;
                        const std::array<float, 3> gains {
                            DryAcoustics[0].Next(), DryAcoustics[1].Next(), DryAcoustics[2].Next()};
                        const float visibility = Visibility.Next();
                        const float filter     = AcousticFilter.Next();
                        const float wet        = WetAcoustics.Next();
                        const bool  obstructed = gains[0] != 1 || gains[1] != 1 || gains[2] != 1;
                        if (direct && (obstructed || DirectSettling))
                        {
                            direct->Render(dry, visibility, gains);
                            if (obstructed)
                                DirectSettling = DirectGainLag + 1;
                            else if (--DirectSettling == 0)
                                direct->Reset();
                        }
                        else
                        {
                            for (uint32_t channel = 0; channel < Channels; ++channel)
                            {
                                const float filtered = OnePole(dry[channel], AcousticHistory[channel], filter);
                                if (gains[0] != 1 || gains[2] != 1)
                                    dry[channel] = dry[channel] * gains[2] + filtered * (gains[0] - gains[2]);
                            }
                        }
                        if (wet != 1)
                            for (size_t lane = 0; lane < Sends.size(); ++lane)
                                for (uint32_t channel = 0; channel < Channels; ++channel)
                                    output[lane + 1][offset + channel] *= wet;
                    }
                    if (profile)
                        AudioProfileActiveMix->Frame->Callback.SteamAudioNanoseconds +=
                            ProfileNanoseconds(AudioProfileClock::now() - start);
                }

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

                void Uninit()
                {
                    if (Ready)
                    {
                        Disconnect();
                        ma_node_uninit(this, nullptr);
                        ResetAcoustics({});
                        Direct.store(nullptr, std::memory_order_relaxed);
                        DirectOwner.reset();
                        DirectAttempted = false;
                        Ready = false;
                    }
                }

                ~SendNode() { Uninit(); }

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
                    self.ProcessAcoustics(outputs, frames);
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
            uint64_t                           HRTFRevision  = 0;
            bool                               HasHRTFEffect = false;
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
            AudioVoiceHandle                   ProfileHandle;
            AudioVoiceReason                   ProfileReason     = AudioVoiceReason::None;
            float                              ListenerDistance  = 0;
            uint64_t                           ReportedUnderruns = 0, ReportedUnderrunFrames = 0;
            bool                               ProfileLive = false, ProfilePendingStart = false;
            uint64_t                     PlayOrder = 0, PinnedStart = NoFrame;
            bool                         Attached = false, Virtual = false, CountedReal = false, Stolen = false;
            bool                         Poolable = false;
            struct OcclusionWork
            {
                AudioOcclusionSettings Settings;
                AudioOcclusionResult   Result;
                AcousticMix            Mix;
                AudioOcclusionRay      Ray;
                glm::vec3              Right {0}, Up {0};
                std::array<float, 3>   TransmissionSum {};
                uint64_t               ListenerIdentity = 0, LastRayFrame = 0, LastServiceFrame = 0;
                uint32_t               ListenerIndex = MaxAudioListeners, Samples = 0, Blocked = 0;
                bool                   ReadyToRevive = false, HeldForOcclusion = false;
                float                  Clear = 1;
            } Acoustics;
            uint64_t                 PausedAt = NoFrame, RemainingStart = 0, RemainingStop = NoFrame;
            bool                               Initialized = false, Scheduled = false, BusHeld = false;
            ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }

            const Transport& Current() const { return History.back(); }

            bool NativeEnded() const
            {
                return ma_sound_at_end(&Sound) && !Sound.BinauralBuffered.load(std::memory_order_acquire);
            }

            bool Ended(double clock) const
            {
                const auto& state = Current();
                if (state.Start == NoFrame)
                    return false;
                return Virtual ? !state.Loop && state.Position(clock, StreamDelay.load()) >= Info.LengthFrames :
                                 NativeEnded();
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

        struct OutputDevice
        {
            Impl*                       Mixer = nullptr;
            ma_device                   Device {};
            std::atomic<bool>           Changed {false};
            std::optional<ma_device_id> Endpoint;
            bool                        Open = false;
            ~OutputDevice()
            {
                if (Open)
                    ma_device_uninit(&Device);
            }
        };

        struct DeviceList
        {
            std::vector<ma_device_info> Entries;
            ma_result                   Result = MA_SUCCESS;
        };

        DeviceList ReadDevices()
        {
            DeviceList list;
            list.Result = ma_context_enumerate_devices(
                &Context,
                [](ma_context*, ma_device_type type, const ma_device_info* info, void* user) -> ma_bool32 {
                    if (type == ma_device_type_playback)
                        static_cast<DeviceList*>(user)->Entries.push_back(*info);
                    return MA_TRUE;
                },
                &list);
            return list;
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
        AudioUserSettings                UserSettings;
        AudioOutputFormat                MixFormat, NativeFormat;
        uint32_t                         MixPeriod = 0;
        std::unique_ptr<OutputDevice>    Output;
        std::unique_ptr<OutputConverter> OutputConversion;
        AudioDeviceChange                LastDeviceChange;
        std::vector<AudioOutputDevice>   SimulatedDevices;
        bool                             Simulated = false, SimulatedChanged = false, RetryOutput = false;
        std::string                      SimulatedEndpoint;
        MixClock::time_point             NextOutputCheck {};
        ma_result                        InitMixer(const AudioOutputFormat& format);
        ma_result                        InitVoiceNodes(Voice& voice, uint64_t loopStart, uint64_t loopEnd);
        ma_result                        OpenOutput(const ma_device_id* selection, OutputDevice& output);
        ma_uint64                        MixPCM(float* output, ma_uint64 count);
        ma_uint64                        MixBlock(float* output, ma_uint64 count);
        ma_uint64                        MixOutput(float* output, ma_uint64 count);
        AudioProfileCapture              Profile;
        std::unique_ptr<SteamAudio>      HRTF;
        Ref<const AudioHRTFAsset>        ActiveHRTF;
        uint64_t                         HRTFRevision         = 1;
        uint64_t                         ProfileGraphRevision = 0, RetiredUnderruns = 0, RetiredUnderrunFrames = 0;

        void LogProfile(AudioProfileEvent event, uint64_t frame)
        {
            event.Frame   = frame;
            event.Seconds = static_cast<double>(frame) / Config.SampleRate;
            Profile.Push(event);
        }

        void LogVoice(AudioProfileEventType type,
                      const Voice&          voice,
                      uint64_t              frame,
                      AudioVoiceReason      reason = AudioVoiceReason::None,
                      uint64_t              count  = 0,
                      uint64_t              frames = 0)
        {
            LogProfile({0,
                        0,
                        type,
                        reason,
                        voice.ProfileHandle,
                        voice.Desc.Owner,
                        voice.Desc.Clip,
                        voice.Desc.Sound,
                        count,
                        frames},
                       frame);
        }

        void ProfileStart(Voice& voice, uint64_t now)
        {
            voice.ProfilePendingStart = false;
            if (!voice.ProfileLive)
            {
                voice.ProfileLive = true;
                LogVoice(AudioProfileEventType::Started, voice, now);
            }
        }

        void ProfileStop(Voice& voice, uint64_t now)
        {
            if (std::exchange(voice.ProfileLive, false))
                LogVoice(AudioProfileEventType::Stopped, voice, now);
        }

        void ObserveVoice(Voice& voice, uint64_t now, double heard)
        {
            const auto& state   = voice.Current();
            const bool  started = state.Start != NoFrame && now >= state.Start;
            if (started && voice.ProfilePendingStart)
                ProfileStart(voice, now);
            const auto& source = *voice.Source;
            if (source.Stream || source.Generator)
            {
                const auto count = source.Underruns.load(std::memory_order_acquire);
                if (count != voice.ReportedUnderruns)
                {
                    const auto frames = source.UnderrunFrames.load(std::memory_order_relaxed);
                    LogVoice(AudioProfileEventType::Underrun,
                             voice,
                             now,
                             AudioVoiceReason::None,
                             count - voice.ReportedUnderruns,
                             frames - voice.ReportedUnderrunFrames);
                    voice.ReportedUnderruns      = count;
                    voice.ReportedUnderrunFrames = frames;
                }
            }
            if (started && voice.ProfileLive &&
                (heard >= state.Stop || (voice.Ended(heard) && (voice.Virtual || !voice.Timed.load() ||
                                                                voice.Position(heard) >= voice.Info.LengthFrames))))
                ProfileStop(voice, now);
        }

        void ObserveVoices(uint64_t now, double heard)
        {
            for (const auto& slot : Slots)
                if (slot.Value)
                    ObserveVoice(*slot.Value, now, heard);
        }
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
            AudioVoiceReason Reason;
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
        struct OcclusionCandidate
        {
            Voice* Instance;
            float  Weight;
            bool   Complete = false;
        };
        std::vector<OcclusionCandidate> OcclusionCandidates;
        AudioOcclusionQuery             OcclusionQuery;
        AudioOcclusionDebugDraw         OcclusionDebugDraw    = nullptr;
        void*                           OcclusionDebugContext = nullptr;
        AudioOcclusionStats             OcclusionStats;
        uint64_t                        OcclusionFrame = 0;

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
            const auto channels =
                mix.BinauralEnabled ? MixFormat.Channels : std::min(Config.Channels, MixFormat.Channels);
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

        AudioSpatializer Spatializer(const Voice& voice) const
        {
            return voice.Desc.Spatializer.value_or(Config.DefaultSpatializer);
        }

        AudioHRTFFallback HRTFFallback(const Voice& voice) const
        {
            if (Spatializer(voice) != AudioSpatializer::HRTF)
                return AudioHRTFFallback::None;
            if (!voice.Desc.Spatial)
                return AudioHRTFFallback::NonSpatial;
            if (MixFormat.Channels != 2 || NativeFormat.Channels != 2)
                return AudioHRTFFallback::NonStereoOutput;
            if (!HRTF || !HRTF->IsReady())
                return AudioHRTFFallback::SteamAudioUnavailable;
            if (!voice.HasHRTFEffect && voice.Desc.SpatialBlend > 0)
                return AudioHRTFFallback::EffectCreationFailed;
            return AudioHRTFFallback::None;
        }

        bool PrepareBinaural(Voice& voice)
        {
            if (Spatializer(voice) != AudioSpatializer::HRTF || voice.Desc.SpatialBlend == 0)
                return false;
            if (MixFormat.Channels != 2 || NativeFormat.Channels != 2 || !HRTF || !HRTF->IsReady())
                return false;
            if (voice.HRTFRevision != HRTFRevision)
            {
                voice.HRTFRevision  = HRTFRevision;
                auto effect         = HRTF->CreateEffect();
                voice.HasHRTFEffect = bool(effect);
                if (effect)
                    voice.Sound.Binaural.Publish(std::move(effect));
                else
                    WarnSteamAudio("could not create a binaural effect");
            }
            return voice.HasHRTFEffect;
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
            voice.ListenerDistance = listenerIndex < MaxAudioListeners ?
                                         glm::length(Listeners[listenerIndex].Position - params.Position) :
                                         0.f;
            UpdateVoiceSends(voice);
            const auto direction = glm::dot(params.Direction, params.Direction) > 0 ? glm::normalize(params.Direction) :
                                                                                      glm::vec3(0, 0, -1);
            voice.ConeInnerCos   = std::cos(params.ConeInnerAngle * RadiansPerDegree * 0.5f);
            voice.ConeOuterCos   = std::cos(params.ConeOuterAngle * RadiansPerDegree * 0.5f);
            params.Direction     = direction;

            SpatialMix mix;
            const bool binaural  = PrepareBinaural(voice) && listenerIndex < Config.MaxListeners;
            mix.BinauralEnabled  = binaural;
            mix.BinauralReversed = binaural && MixFormat.ChannelMap[0] == MA_CHANNEL_FRONT_RIGHT;
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

                if (binaural)
                    mix.BinauralBlend = voice.Desc.SpatialBlend * boundary * (1.f - sample.Spread);
                if (shaped || (attenuation && voice.AttenuationHasSpread) || binaural)
                    SetCustomPan(mix, params.Position, listenerIndex, boundary, sample.Spread);
                const float postGain = (nativePreset ? 1.f : sample.Gain) * (shaped ? angularGain : 1.f);
                if (binaural)
                    mix.BinauralGain = postGain;
                for (auto& gain : mix.Gains)
                    gain *= postGain;
                mix.FilterCoefficient = AirFilterCoefficient(sample.CutoffHz, Config.SampleRate);
            }
            else
            {
                voice.SpatialAudibility.reset();
                if (binaural)
                {
                    mix.BinauralBlend = voice.Desc.SpatialBlend;
                    SetCustomPan(mix, params.Position, listenerIndex, 1.f, 0.f);
                }
            }

            mix.HasNative = true;
            mix.Native    = params;
            sound->SpatialMailbox.Publish(mix);
        }

        void RefreshSpatializers()
        {
            for (const auto& slot : Slots)
                if (slot.Value && slot.Value->Desc.Spatial)
                    ApplySpatial(*slot.Value);
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
            return VoiceGain(voice, clock, busGain);
        }

        float VoiceGain(const Voice& voice, uint64_t clock, float busGain)
        {
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

        bool WantsOcclusion(const Voice& voice) const
        {
            return OcclusionQuery.Trace && voice.Acoustics.Settings.Enabled() &&
                   ((voice.Desc.Spatial && voice.Listener < Config.MaxListeners) ||
                    (voice.Desc.ReverbPosition && voice.Listener < Config.MaxListeners));
        }

        void SetAcoustics(Voice& voice, const AudioOcclusionResult& result, float clear)
        {
            auto& work = voice.Acoustics;
            if (!work.HeldForOcclusion && work.Result == result && work.Clear == clear)
                return;
            work.Clear            = clear;
            work.Result           = result;
            work.HeldForOcclusion = false;
            work.Mix              = EvaluateAcoustics(result, clear, Config.SampleRate);
            if (work.Mix != AcousticMix {})
                voice.Route.PrepareDirect(HRTF.get());
            voice.Route.AcousticMailbox.Publish(work.Mix);
        }

        void RefreshOcclusionSettings(Voice& voice)
        {
            auto&      work     = voice.Acoustics;
            const auto settings = voice.Desc.Occlusion.Resolve(Config.DefaultOcclusion.Resolve());
            if (work.Settings == settings)
                return;
            work.Settings      = settings;
            work.Samples       = 0;
            work.ReadyToRevive = false;
            if (!WantsOcclusion(voice))
            {
                work.LastServiceFrame = 0;
                SetAcoustics(voice, {}, 1);
            }
        }

        void QueueOcclusion(Voice& voice, float audibility)
        {
            if (!WantsOcclusion(voice))
            {
                auto& work = voice.Acoustics;
                if (work.LastServiceFrame || work.HeldForOcclusion)
                {
                    work.LastServiceFrame = 0;
                    work.Samples          = 0;
                    work.ReadyToRevive    = false;
                    SetAcoustics(voice, {}, 1);
                }
                return;
            }
            const auto  position  = voice.Desc.Spatial ? voice.Spatial.Position : *voice.Desc.ReverbPosition;
            const float distance  = glm::length(position - Listeners[voice.Listener].Position);
            const float audible   = std::clamp(audibility, 0.f, 1.f);
            const float proximity = std::isfinite(distance) ? 1.f / (1.f + distance) : 0.f;
            const float weight    = (voice.Desc.Priority / 255.f + audible + proximity) / 3.f;
            auto&       work      = voice.Acoustics;
            if (work.ListenerIndex != voice.Listener || work.ListenerIdentity != Listeners[voice.Listener].Identity)
            {
                work.Samples          = 0;
                work.ReadyToRevive    = false;
                work.ListenerIndex    = voice.Listener;
                work.ListenerIdentity = Listeners[voice.Listener].Identity;
            }
            OcclusionCandidates.push_back({&voice, weight});
        }

        void UpdateOcclusion()
        {
            auto& stats = OcclusionStats;
            stats.Rays = stats.VoicesServiced = 0;
            stats.DeferredVoices              = 0;
            if (OcclusionCandidates.empty())
                return;
            std::sort(OcclusionCandidates.begin(), OcclusionCandidates.end(), [](const auto& a, const auto& b) {
                const auto lastA = a.Instance->Acoustics.LastRayFrame;
                const auto lastB = b.Instance->Acoustics.LastRayFrame;
                if (lastA != lastB)
                    return lastA < lastB;
                if (a.Weight != b.Weight)
                    return a.Weight > b.Weight;
                return a.Instance->ProfileHandle.Index < b.Instance->ProfileHandle.Index;
            });

            bool       began  = false;
            const auto budget = Config.MaxOcclusionRaysPerFrame;
            // Oldest turn first. A weight gives up to four rays within a turn
            // for volumetric work. Partial sweeps continue on later frames.
            for (bool progressed = true; stats.Rays < budget && progressed;)
            {
                progressed = false;
                for (auto& candidate : OcclusionCandidates)
                {
                    if (candidate.Complete || stats.Rays == budget)
                        continue;
                    auto&       voice    = *candidate.Instance;
                    auto&       work     = voice.Acoustics;
                    const auto& settings = work.Settings;
                    const auto  samples = settings.Mode == AudioOcclusionMode::Volumetric && settings.SourceRadius > 0 ?
                                              settings.Samples :
                                              1;
                    const bool  transmission =
                        settings.Transmission != AudioTransmissionMode::Off && settings.TransmissionStrength > 0;
                    if (!work.Samples)
                    {
                        const auto position  = voice.Desc.Spatial ? voice.Spatial.Position : *voice.Desc.ReverbPosition;
                        work.Ray             = {Listeners[voice.Listener].Position,
                                                position,
                                                voice.Desc.Owner.Entity,
                                                settings.OcclusionLayerMask};
                        const auto axis      = UnitVector(position - work.Ray.Start, {0, 0, -1});
                        const auto reference = std::abs(axis.y) < .9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                        work.Right           = UnitVector(glm::cross(axis, reference), {1, 0, 0});
                        work.Up              = glm::cross(work.Right, axis);
                        work.Blocked         = 0;
                        work.TransmissionSum = {};
                    }
                    const uint32_t burst = 1 + static_cast<uint32_t>(3 * candidate.Weight);
                    work.LastRayFrame    = OcclusionFrame;
                    progressed           = true;
                    for (uint32_t turn = 0; turn < burst && work.Samples < samples && stats.Rays < budget;
                         ++turn, ++work.Samples)
                    {
                        auto ray = work.Ray;
                        // Opposed pairs on a projected disc give a centered sample distribution.
                        if (samples > 1 && work.Samples >= samples % 2)
                        {
                            const auto  offset = work.Samples - samples % 2;
                            const auto  pair   = offset / 2;
                            const float angle  = 2.39996323f * pair;
                            const float radius = settings.SourceRadius * std::sqrt((pair + .5f) / (samples / 2));
                            const float side   = offset % 2 ? -1.f : 1.f;
                            ray.End += (side * radius) * (std::cos(angle) * work.Right + std::sin(angle) * work.Up);
                        }
                        std::array<AudioOcclusionHit, MaxAudioTransmissionHits> hits;
                        const size_t capacity  = transmission ? hits.size() : 1;
                        size_t       count     = 0;
                        const auto   delta     = ray.End - ray.Start;
                        const float  lengthSqr = glm::dot(delta, delta);
                        if (std::isfinite(lengthSqr) && lengthSqr > 1e-12f)
                        {
                            if (!std::exchange(began, true) && OcclusionQuery.Begin)
                                OcclusionQuery.Begin(OcclusionQuery.Context);
                            count = std::min(capacity,
                                             OcclusionQuery.Trace(OcclusionQuery.Context, ray, hits.data(), capacity));
                            ++stats.Rays;
                            if (OcclusionDebugDraw)
                                OcclusionDebugDraw(OcclusionDebugContext, ray, hits.data(), count);
                        }
                        work.Blocked += count != 0;
                        if (transmission)
                            for (size_t band = 0; band < work.TransmissionSum.size(); ++band)
                            {
                                // Summing attenuation in dB is multiplication in linear gain.
                                float gain = 1;
                                for (size_t hit = 0; hit < count; ++hit)
                                    gain *= hits[hit].Transmission[band];
                                work.TransmissionSum[band] += gain;
                            }
                    }
                    if (work.Samples == samples)
                    {
                        const float          blocked = float(work.Blocked) / samples;
                        AudioOcclusionResult result;
                        if (settings.Mode != AudioOcclusionMode::Off)
                            result.Occlusion = blocked * settings.OcclusionStrength;
                        if (transmission)
                        {
                            for (size_t band = 0; band < result.Transmission.size(); ++band)
                                result.Transmission[band] =
                                    glm::mix(1.f, work.TransmissionSum[band] / samples, settings.TransmissionStrength);
                            if (settings.Transmission == AudioTransmissionMode::FrequencyIndependent)
                                result.Transmission.fill(
                                    (result.Transmission[0] + result.Transmission[1] + result.Transmission[2]) / 3);
                        }
                        SetAcoustics(voice, result, transmission ? 1 - blocked * settings.TransmissionStrength : 1);
                        work.Samples          = 0;
                        work.LastServiceFrame = OcclusionFrame;
                        work.ReadyToRevive    = true;
                        candidate.Complete    = true;
                        ++stats.VoicesServiced;
                    }
                }
            }
            stats.DeferredVoices = static_cast<uint32_t>(OcclusionCandidates.size()) - stats.VoicesServiced;
            stats.BudgetOverruns += stats.DeferredVoices != 0;
        }

        static AcousticMix CurrentAcousticMix(const Voice& voice)
        {
            if (voice.Acoustics.HeldForOcclusion)
                return {{0, 0, 0}, 0, 0, 1};
            return voice.Acoustics.Mix;
        }

        void Virtualize(Voice& voice, uint64_t clock, AudioVoiceReason reason)
        {
            if (!voice.Virtual)
                LogVoice(AudioProfileEventType::Virtualised, voice, clock, reason);
            voice.ProfileReason = reason;
            CancelSchedule(voice);
            Detach(voice);
            ma_sound_stop(&voice.Sound);
            SetStreamActive(voice, false);
            voice.Virtual = true;
            voice.Acoustics.Samples       = 0;
            voice.Acoustics.ReadyToRevive = false;
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
            voice.Route.ResetAcoustics(voice.Acoustics.Mix);
            if (!Attach(voice))
                return false;
            voice.Virtual = false;
            voice.ProfileReason = AudioVoiceReason::None;
            LogVoice(AudioProfileEventType::Revived, voice, clock);
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
        if (!config.UserSettingsFile.empty())
        {
            std::vector<std::string> reasons;
            p.UserSettings = AudioUserSettings::Load(config.UserSettingsFile, reasons);
            for (const auto& reason : reasons)
                Warn(reason);
        }
        if (!IsValidAudioOcclusion(config.DefaultOcclusion.Resolve()) || config.MaxListeners < 1 ||
            config.MaxListeners > MaxAudioListeners || config.Channels == 0 || config.Channels > MaxOutputChannels ||
            (config.Mode == AudioOutputMode::Offline && config.SampleRate == 0) ||
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
                        const bool profile = p.Profile.Enabled.load(std::memory_order_relaxed);
                        for (const auto& source : streams)
                            source->Fill(profile);
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
        if (config.Mode == AudioOutputMode::Device)
        {
            if (auto error = RebuildOutput(
                    p.UserSettings.OutputDevice, p.UserSettings.Speakers, AudioDeviceChangeReason::Selected))
            {
                if (p.UserSettings.OutputDevice == DefaultAudioOutputDevice)
                {
                    Shutdown();
                    return error;
                }
                if (auto fallback = RebuildOutput(
                        DefaultAudioOutputDevice, p.UserSettings.Speakers, AudioDeviceChangeReason::FellBackToDefault))
                {
                    Shutdown();
                    return fallback;
                }
            }
        }
        else
        {
            if (auto error = RebuildOutput(
                    p.UserSettings.OutputDevice, p.UserSettings.Speakers, AudioDeviceChangeReason::Selected))
            {
                Shutdown();
                return error;
            }
        }
        static std::atomic<uint64_t> nextInstance {0};
        p.HRTF = std::make_unique<SteamAudio>();
        if (auto error = p.HRTF->Init(p.Config.SampleRate, ma_engine_get_node_graph(&p.Engine)->processingSizeInFrames))
            WarnSteamAudio(*error);
        p.InstanceID = ++nextInstance;
        p.Listeners[0].Active = true;
        for (uint32_t i = 1; i < config.MaxListeners; ++i)
        {
            p.Listeners[i].Active = false;
            ma_engine_listener_set_enabled(&p.Engine, i, false);
        }
        if (auto error = ApplyBusLayout(AudioBusLayout::Default()))
        {
            Shutdown();
            return error;
        }
        if (p.Output)
        {
            if (const auto start = ma_device_start(&p.Output->Device); start != MA_SUCCESS)
            {
                Shutdown();
                return Error("Audio device start", start);
            }
        }
        return std::nullopt;
    }

    void AudioEngine::Shutdown()
    {
        auto& p = *m_Impl;
        p.Output.reset();
        p.OutputConversion.reset();
        while (!p.ClipRegistries.empty())
            p.ClipRegistries.back()->Clear();
        for (uint32_t i = 0; i < p.Slots.size(); ++i)
            if (p.Slots[i].Value)
                DestroyVoice({i, p.Slots[i].Generation});
        while (!p.Sounds.empty())
            UnregisterSound(p.Sounds.begin()->first);
        p.ActiveHRTF.reset();
        p.HRTF.reset();
        p.Buses.reset();
        p.UserSettings = {};
        p.MixFormat = p.NativeFormat = {};
        p.MixPeriod                  = 0;
        p.LastDeviceChange           = {};
        p.NextOutputCheck            = {};
        p.SimulatedDevices.clear();
        p.SimulatedEndpoint.clear();
        p.Simulated = p.SimulatedChanged = p.RetryOutput = false;
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
        p.OcclusionCandidates.clear();
        p.OcclusionQuery        = {};
        p.OcclusionDebugDraw    = nullptr;
        p.OcclusionDebugContext = nullptr;
        p.OcclusionFrame        = 0;
        p.OcclusionStats        = {};
        p.Profile.Reset();
        p.RetiredUnderruns = p.RetiredUnderrunFrames = 0;
    }

    ma_result AudioEngine::Impl::InitVoiceNodes(Voice& voice, uint64_t loopStart, uint64_t loopEnd)
    {
        auto config        = ma_sound_config_init_2(&Engine);
        config.pDataSource = voice.Source.get();
        config.flags |= MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
        if (!voice.Desc.Spatial)
            config.flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (voice.Desc.Loop && !(voice.Source->Stream && loopEnd != 0))
            config.flags |= MA_SOUND_FLAG_LOOPING;
        if (loopEnd != 0 && !voice.Source->Stream && !voice.Source->Generator)
        {
            config.loopPointBegInPCMFrames = loopStart;
            config.loopPointEndInPCMFrames = loopEnd;
        }
        auto& node   = voice.Sound;
        auto  result = ma_sound_init_ex(&Engine, &config, &node);
        if (result != MA_SUCCESS)
            return result;
        voice.Initialized                  = true;
        node.Owner                         = &voice;
        node.ListenerIdentities            = &MixerListenerIdentities;
        node.NativePanFloor                = node.engineNode.spatializer.minSpatializationChannelGain;
        node.Original                      = node.engineNode.baseNode.vtable;
        static const ma_node_vtable vtable = {Voice::Node::Process, nullptr, 0, 1, 0};
        node.engineNode.baseNode.vtable    = &vtable;
        ma_sound_set_pan_mode(&node, ma_pan_mode_balance);
        if (voice.Source->Generator && voice.Info.SampleRate != ma_engine_get_sample_rate(&Engine))
            node.ResetResampler();
        result = voice.Route.Init(Engine, LfeChannel);
        if (result == MA_SUCCESS)
        {
            if (voice.Acoustics.Mix != AcousticMix {})
                voice.Route.PrepareDirect(HRTF.get());
            voice.Route.ResetAcoustics(CurrentAcousticMix(voice));
            result = ma_node_attach_output_bus(&node, 0, &voice.Route, 0);
        }
        return result;
    }

    ma_result AudioEngine::Impl::InitMixer(const AudioOutputFormat& format)
    {
        auto config               = ma_engine_config_init();
        config.pResourceManager   = &Resources;
        config.noDevice           = MA_TRUE;
        config.channels           = format.Channels;
        config.sampleRate         = Config.SampleRate;
        config.periodSizeInFrames = MixPeriod;
        config.listenerCount      = Config.MaxListeners;
        if (Config.Mode == AudioOutputMode::Device)
        {
            config.pProcessUserData = this;
            config.onProcess        = [](void* user, float*, ma_uint64 frames) {
                auto& impl = *static_cast<Impl*>(user);
                ++impl.ClockSequence;
                impl.LastMixedFrame.store(ma_engine_get_time_in_pcm_frames(&impl.Engine));
                impl.LastMixedCount.store(frames);
                impl.LastMixTicks.store(MixClock::now().time_since_epoch().count());
                ++impl.ClockSequence;
            };
        }
        auto result = ma_engine_init(&config, &Engine);
        if (result != MA_SUCCESS)
            return result;
        // Keep miniaudio's side-facing stereo spatial anchors for conventional left/right
        // playback. They preserve symmetric panning around the listener; the output map
        // still identifies the physical left and right speakers.
        if (format != StandardOutputFormat(2))
            for (uint32_t i = 0; i < Config.MaxListeners; ++i)
                std::copy_n(format.ChannelMap.data(),
                            format.Channels,
                            ma_spatializer_listener_get_channel_map(&Engine.listeners[i]));
        LfeChannel                = AudioChannelIndex(format.ChannelMap.data(), format.Channels, MA_CHANNEL_LFE);
        ProbeListener             = {};
        ProbeListener.config      = Engine.listeners[0].config;
        ProbeListener.isEnabled   = true;
        auto probeConfig          = ma_spatializer_config_init(format.Channels, format.Channels);
        probeConfig.positioning   = ma_positioning_relative;
        probeConfig.rolloff       = 0;
        probeConfig.dopplerFactor = 0;
        probeConfig.pChannelMapIn = Engine.listeners[0].config.pChannelMapOut;
        result                    = ma_spatializer_init(&probeConfig, nullptr, &PanProbe);
        if (result != MA_SUCCESS)
        {
            ma_engine_uninit(&Engine);
            return result;
        }
        PanProbeReady = Ready = true;
        MixFormat             = format;
        return MA_SUCCESS;
    }

    ma_result AudioEngine::Impl::OpenOutput(const ma_device_id* selection, OutputDevice& output)
    {
        auto config                      = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID        = selection;
        config.playback.format           = ma_format_f32;
        config.playback.channels         = Config.UseNullDevice ? Config.Channels : 0;
        config.sampleRate                = Config.SampleRate;
        config.wasapi.noAutoConvertSRC   = MA_TRUE;
        config.noPreSilencedOutputBuffer = MA_TRUE;
        config.noClip                    = MA_TRUE;
        output.Mixer                     = this;
        config.pUserData                 = &output;
        config.notificationCallback      = [](const ma_device_notification* notice) {
            auto& device = *static_cast<OutputDevice*>(notice->pDevice->pUserData);
            switch (notice->type)
            {
                case ma_device_notification_type_stopped:
                case ma_device_notification_type_rerouted:
                case ma_device_notification_type_unlocked:
                case ma_device_notification_type_interruption_ended:
                    device.Changed.store(true, std::memory_order_release);
                    break;
                default:
                    break;
            }
        };
        config.dataCallback = [](ma_device* device, void* outputFrames, const void*, ma_uint32 frames) {
            auto&          impl    = *static_cast<OutputDevice*>(device->pUserData)->Mixer;
            impl.MixPCM(static_cast<float*>(outputFrames), frames);
        };
        auto result = ma_device_init(&Context, &config, &output.Device);
        if (result == MA_SUCCESS && output.Device.playback.channels > MaxOutputChannels)
        {
            ma_device_uninit(&output.Device);
            config.playback.channels = MaxOutputChannels;
            result                   = ma_device_init(&Context, &config, &output.Device);
        }
        output.Open = result == MA_SUCCESS;
        return result;
    }

    ma_uint64 AudioEngine::Impl::MixPCM(float* output, ma_uint64 count)
    {
        if (!Profile.Enabled.load(std::memory_order_relaxed))
            return MixBlock(output, count);

        const auto      start  = AudioProfileClock::now();
        const auto      first  = ma_engine_get_time_in_pcm_frames(&Engine);
        const double    budget = 1e9 * count / Config.SampleRate;
        auto*           frame  = Profile.Begin();
        AudioProfileMix mix {frame};
        auto*           previous = AudioProfileActiveMix;
        AudioProfileActiveMix    = &mix;
        const auto read          = MixBlock(output, count);
        AudioProfileActiveMix    = previous;
        const auto end           = AudioProfileClock::now();
        const auto elapsed       = ProfileNanoseconds(end - start);
        if (frame)
        {
            frame->Callback = {first, count, elapsed, budget, frame->Callback.SteamAudioNanoseconds};
            frame->Tick     = AudioProfileCapture::Tick(end);
        }
        Profile.End(frame, elapsed > budget);
        return read;
    }

    ma_uint64 AudioEngine::Impl::MixBlock(float* output, ma_uint64 count)
    {
        if (Config.Mode == AudioOutputMode::Offline)
        {
            DrainJobs();
            ApplyMixerListeners();
            return MixOutput(output, count);
        }
        const auto*    graph   = ma_engine_get_node_graph(&Engine);
        const uint64_t quantum = std::max(1u, graph->processingSizeInFrames);
        const uint64_t needed  = count - std::min<uint64_t>(count, graph->processingCacheFramesRemaining);
        const auto     first   = ma_engine_get_time_in_pcm_frames(&Engine);
        MixEnd.store(AddFrames(first, (needed + quantum - 1) / quantum * quantum));
        ApplyMixerListeners();
        ApplySchedules();
        const auto read = MixOutput(output, count);
        if (read < count)
            std::fill_n(output + read * NativeFormat.Channels, (count - read) * NativeFormat.Channels, 0.f);
        MixEnd.store(ma_engine_get_time_in_pcm_frames(&Engine));
        return read;
    }

    ma_uint64 AudioEngine::Impl::MixOutput(float* output, ma_uint64 count)
    {
        if (!OutputConversion)
        {
            ma_uint64 read = 0;
            ma_engine_read_pcm_frames(&Engine, output, count, &read);
            return read;
        }
        constexpr uint32_t                                   scratchFrames = 256;
        std::array<float, scratchFrames * MaxOutputChannels> scratch;
        ma_uint64                                            total = 0;
        while (total < count)
        {
            ma_uint64  read   = 0;
            const auto result = ma_engine_read_pcm_frames(
                &Engine, scratch.data(), std::min<ma_uint64>(scratchFrames, count - total), &read);
            ma_channel_converter_process_pcm_frames(
                &OutputConversion->Converter, output + total * NativeFormat.Channels, scratch.data(), read);
            total += read;
            if (result != MA_SUCCESS || read == 0)
                break;
        }
        return total;
    }

    std::vector<AudioOutputDevice> AudioEngine::EnumerateOutputDevices() const
    {
        auto& p = *m_Impl;
        if (p.Simulated)
            return p.SimulatedDevices; // The test seam's snapshot stands in for the backend's list.
        if (!p.ContextReady || p.Config.UseNullDevice)
            return {};
        const auto                     devices = p.ReadDevices();
        std::vector<AudioOutputDevice> result;
        for (const auto& entry : devices.Entries)
        {
            auto& device     = result.emplace_back();
            device.Id        = DeviceId(p.Context.backend, entry.id);
            device.Name      = entry.name;
            device.IsDefault = entry.isDefault != 0;
            if (p.Output && p.Output->Endpoint && ma_device_id_equal(&entry.id, &*p.Output->Endpoint))
                device.NativeFormat = p.NativeFormat;
            else
            {
                ma_device_info detail {};
                if (ma_context_get_device_info(&p.Context, ma_device_type_playback, &entry.id, &detail) == MA_SUCCESS)
                {
                    // A backend can report multiple supported formats with no preferred channel count.
                    // Prefer the non-exclusive count only when the list agrees.
                    uint32_t channels = 0;
                    for (uint32_t i = 0; i < detail.nativeDataFormatCount; ++i)
                    {
                        const auto& format = detail.nativeDataFormats[i];
                        if (!format.channels || (format.flags & MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE))
                            continue;
                        if (channels != 0 && channels != format.channels)
                        {
                            channels = 0;
                            break;
                        }
                        channels = format.channels;
                    }
                    device.NativeFormat.Channels = channels;
                }
            }
        }
        return result;
    }

    const AudioDeviceChange& AudioEngine::GetLastDeviceChange() const { return m_Impl->LastDeviceChange; }
    SpeakerMode              AudioEngine::GetSpeakerMode() const { return m_Impl->UserSettings.Speakers; }
    AudioOutputFormat        AudioEngine::GetOutputFormat() const { return m_Impl->NativeFormat; }

    bool AudioEngine::SetOutputDevice(std::string_view id)
    {
        auto& p = *m_Impl;
        if (!p.Ready || (!p.Simulated && (!p.ContextReady || p.Config.UseNullDevice)))
            return false;
        if (id.empty())
            id = DefaultAudioOutputDevice;
        return (id == p.UserSettings.OutputDevice && !p.RetryOutput) ||
               !RebuildOutput(id, GetSpeakerMode(), AudioDeviceChangeReason::Selected);
    }

    bool AudioEngine::SetSpeakerMode(SpeakerMode mode)
    {
        auto& p = *m_Impl;
        if (!p.Ready || static_cast<unsigned>(mode) > static_cast<unsigned>(SpeakerMode::Surround71))
            return false;
        return mode == GetSpeakerMode() ||
               !RebuildOutput(p.UserSettings.OutputDevice, mode, AudioDeviceChangeReason::SpeakerModeChanged);
    }

    void AudioEngine::SimulateOutputDevices(std::vector<AudioOutputDevice> devices)
    {
        auto& p = *m_Impl;
        if (p.Ready && p.Config.Mode == AudioOutputMode::Offline)
        {
            p.Simulated = p.SimulatedChanged = true;
            p.SimulatedDevices               = std::move(devices);
        }
    }

    std::optional<std::string>
    AudioEngine::RebuildOutput(std::string_view id, SpeakerMode mode, AudioDeviceChangeReason reason)
    {
        auto&                               p          = *m_Impl;
        const bool                          useDefault = id == DefaultAudioOutputDevice;
        const bool                          startup    = !p.Ready;
        auto                                native     = StandardOutputFormat(p.Config.Channels);
        std::string                         simulatedEndpoint;
        std::unique_ptr<Impl::OutputDevice> next;
        const bool keepDevice = p.Output && !p.RetryOutput && reason == AudioDeviceChangeReason::SpeakerModeChanged;
        if (keepDevice)
            native = p.NativeFormat;
        else if (p.ContextReady)
        {
            std::optional<ma_device_id> endpoint;
            if (!p.Config.UseNullDevice)
            {
                for (const auto& device : p.ReadDevices().Entries)
                    if (useDefault ? device.isDefault != 0 : DeviceId(p.Context.backend, device.id) == id)
                    {
                        endpoint = device.id;
                        break;
                    }
                if (!useDefault && !endpoint)
                    return "Selected audio output device is unavailable";
            }
            next           = std::make_unique<Impl::OutputDevice>();
            next->Endpoint = endpoint;
            // Leave the backend's default routing enabled. If the backend reroutes, the
            // notification causes us to reopen and re-read the format.
            const auto result = p.OpenOutput(useDefault || !endpoint ? nullptr : &*endpoint, *next);
            if (result != MA_SUCCESS)
                return Error("Open audio output device", result);
            const auto& playback = next->Device.playback;
            native.Channels      = playback.channels;
            std::copy_n(playback.channelMap, native.Channels, native.ChannelMap.begin());
            native = NormalizeOutputFormat(native, p.Config.Channels);
            if (startup)
            {
                p.MixPeriod = playback.internalPeriodSizeInFrames;
                if (p.Config.SampleRate == 0)
                    p.Config.SampleRate = next->Device.sampleRate;
            }
        }
        else if (p.Simulated)
        {
            const auto found =
                std::find_if(p.SimulatedDevices.begin(), p.SimulatedDevices.end(), [&](const auto& device) {
                    return useDefault ? device.IsDefault : device.Id == id;
                });
            if (found != p.SimulatedDevices.end())
            {
                native            = NormalizeOutputFormat(found->NativeFormat, p.Config.Channels);
                simulatedEndpoint = found->Id;
            }
            else if (!useDefault)
                return "Selected audio output device is unavailable";
        }

        const auto                       mix = MixFormatFor(mode, native);
        std::unique_ptr<OutputConverter> conversion;
        if (mix != native)
        {
            conversion = std::make_unique<OutputConverter>();
            if (const auto result = conversion->Init(mix, native); result != MA_SUCCESS)
                return Error("Initialize audio output converter", result);
        }
        if (p.Output)
        {
            const auto result = ma_device_stop(&p.Output->Device);
            if (result != MA_SUCCESS)
                return Error("Stop audio output device", result);
        }
        auto restartOld = [&] {
            if (p.Output)
            {
                p.Output->Changed.store(false, std::memory_order_release);
                p.RetryOutput = ma_device_start(&p.Output->Device) != MA_SUCCESS;
            }
        };
        const auto lastMix = p.MixFormat;
        if (startup)
        {
            if (const auto result = p.InitMixer(mix); result != MA_SUCCESS)
                return Error("Initialize audio mixer", result);
        }
        else if (mix != lastMix)
        {
            if (auto error = ReformatMix(mix))
            {
                restartOld();
                return error;
            }
        }
        const auto lastNative = p.NativeFormat;
        p.NativeFormat        = native;
        if (!startup && native.Channels != lastNative.Channels)
            p.RefreshSpatializers();
        std::swap(p.OutputConversion, conversion);
        auto*    output  = keepDevice ? p.Output.get() : next.get();
        uint64_t latency = 0;
        if (output)
        {
            const auto& playback = output->Device.playback;
            const auto  rate     = playback.internalSampleRate;
            latency = rate ? static_cast<uint64_t>(std::ceil(double(playback.internalPeriodSizeInFrames) *
                                                             playback.internalPeriods * GetSampleRate() / rate)) :
                             0;
        }
        if (output && !startup)
        {
            if (keepDevice)
                output->Changed.store(false, std::memory_order_release);
            if (const auto result = ma_device_start(&output->Device); result != MA_SUCCESS)
            {
                p.NativeFormat = lastNative;
                std::swap(p.OutputConversion, conversion);
                if (mix != lastMix)
                    if (auto error = ReformatMix(lastMix))
                        return error;
                p.RefreshSpatializers();
                restartOld();
                return Error("Start audio output device", result);
            }
        }
        if (!keepDevice)
            p.Output = std::move(next);
        p.LatencyFrames             = latency;
        p.SimulatedEndpoint         = std::move(simulatedEndpoint);
        p.UserSettings.OutputDevice = id;
        p.UserSettings.Speakers     = mode;
        // Keep the ID of the resolved endpoint for consumers following the default.
        std::string active = p.UserSettings.OutputDevice;
        if (p.Output && p.Output->Endpoint)
            active = DeviceId(p.Context.backend, *p.Output->Endpoint);
        else if (!p.SimulatedEndpoint.empty())
            active = p.SimulatedEndpoint;
        if (!startup || reason == AudioDeviceChangeReason::FellBackToDefault)
        {
            p.LastDeviceChange = {p.LastDeviceChange.Revision + 1, std::move(active), reason, native};
            AudioProfileEvent event;
            event.Type  = AudioProfileEventType::DeviceChanged;
            event.Count = p.LastDeviceChange.Revision;
            p.LogProfile(event, GetClockFrames());
        }
        p.RetryOutput     = false;
        p.NextOutputCheck = MixClock::now() + std::chrono::seconds(1);
        return std::nullopt;
    }

    void AudioEngine::UpdateOutput()
    {
        auto&      p              = *m_Impl;
        const bool followsDefault = p.UserSettings.OutputDevice == DefaultAudioOutputDevice;
        bool       disappeared    = false;
        bool       changed        = false;
        if (p.Simulated)
        {
            if (!std::exchange(p.SimulatedChanged, false))
                return;
            const auto found = std::find_if(p.SimulatedDevices.begin(), p.SimulatedDevices.end(), [&](const auto& d) {
                return followsDefault ? d.IsDefault : d.Id == p.UserSettings.OutputDevice;
            });
            disappeared      = !followsDefault && found == p.SimulatedDevices.end();
            changed          = found != p.SimulatedDevices.end() &&
                               (found->Id != p.SimulatedEndpoint ||
                                NormalizeOutputFormat(found->NativeFormat, p.Config.Channels) != p.NativeFormat);
        }
        else if (p.Output && !p.Config.UseNullDevice)
        {
            changed        = p.Output->Changed.exchange(false, std::memory_order_acq_rel);
            const auto now = MixClock::now();
            if (changed || now >= p.NextOutputCheck)
            {
                p.NextOutputCheck = now + std::chrono::seconds(1);
                struct Scan
                {
                    std::optional<ma_device_id> Active, Default;
                    bool                        Present = false;
                } scan {p.Output->Endpoint};
                const auto result = ma_context_enumerate_devices(
                    &p.Context,
                    [](ma_context*, ma_device_type type, const ma_device_info* info, void* user) -> ma_bool32 {
                        auto& state = *static_cast<Scan*>(user);
                        if (type == ma_device_type_playback)
                        {
                            if (info->isDefault)
                                state.Default = info->id;
                            if (state.Active && ma_device_id_equal(&*state.Active, &info->id))
                                state.Present = true;
                        }
                        return MA_TRUE;
                    },
                    &scan);
                if (result == MA_SUCCESS)
                {
                    disappeared = !followsDefault && !scan.Present;
                    changed |= followsDefault && scan.Default &&
                               (!scan.Active || !ma_device_id_equal(&*scan.Active, &*scan.Default));
                }
                changed |= p.RetryOutput;
            }
        }
        if (!disappeared && !changed)
            return;
        if (!disappeared)
            if (!RebuildOutput(p.UserSettings.OutputDevice, GetSpeakerMode(), AudioDeviceChangeReason::Rerouted))
                return;
        if (!followsDefault &&
            !RebuildOutput(DefaultAudioOutputDevice, GetSpeakerMode(), AudioDeviceChangeReason::FellBackToDefault))
            return;
        // Keep the current graph alive while there is no usable device. Retry at the next scan.
        p.RetryOutput = true;
    }

    std::optional<std::string> AudioEngine::ReformatMix(const AudioOutputFormat& format)
    {
        auto& p = *m_Impl;
        p.ApplySchedules();
        const auto           previous = p.MixFormat;
        const auto           clock    = GetClockFrames();
        const auto           master   = GetMasterVolume();
        const AudioBusLayout layout {p.Buses->GetBuses()};
        struct VoiceState
        {
            Impl::Voice*  Voice;
            double        Position;
            float         Pitch;
            uint64_t      Start, Stop, LocalTime;
            ma_node_state State;
            bool          Attached, Ended, PitchDisabled;
        };
        std::vector<VoiceState> states;
        states.reserve(p.Slots.size());
        for (const auto& slot : p.Slots)
            if (slot.Value)
            {
                auto&     v      = *slot.Value;
                auto&     node   = v.Sound;
                ma_uint64 cursor = 0;
                ma_sound_get_cursor_in_pcm_frames(&node, &cursor);
                double position = double(cursor) - node.processingCacheFramesRemaining;
                if (!node.engineNode.isPitchDisabled && ma_node_get_time(&node) != 0)
                {
                    const auto& phase = *static_cast<const ma_linear_resampler*>(node.engineNode.resampler.pBackend);
                    position += double(phase.inTimeInt) - 2 + double(phase.inTimeFrac) / phase.config.sampleRateOut;
                }
                if (node.seekTarget != NoFrame)
                    position = double(node.seekTarget);
                if (position < 0 && v.Desc.Loop && cursor >= v.Current().LoopBegin)
                    position += double(v.Current().LoopEnd - v.Current().LoopBegin);
                states.push_back({&v,
                                  std::max(0.0, position),
                                  ma_sound_get_pitch(&node),
                                  ma_node_get_state_time(&node, ma_node_state_started),
                                  ma_node_get_state_time(&node, ma_node_state_stopped),
                                  ma_node_get_time(&node),
                                  ma_node_get_state(&node),
                                  v.Attached,
                                  ma_sound_at_end(&node) != MA_FALSE,
                                  node.engineNode.isPitchDisabled != MA_FALSE});
            }
        for (auto& [id, sound] : p.Sounds)
            for (auto& pool : sound.OneShots)
                pool.clear();
        p.PooledOneShots = 0;

        auto release = [&] {
            for (const auto& state : states)
            {
                auto& voice = *state.Voice;
                voice.Route.Uninit();
                voice.Attached = false;
                if (voice.Initialized)
                    ma_sound_uninit(&voice.Sound);
                voice.Initialized = false;
            }
            p.Buses.reset();
            if (p.PanProbeReady)
                ma_spatializer_uninit(&p.PanProbe, nullptr);
            if (p.Ready)
                ma_engine_uninit(&p.Engine);
            p.PanProbeReady = p.Ready = false;
        };
        auto build = [&](const AudioOutputFormat& replacement) -> std::optional<std::string> {
            if (const auto result = p.InitMixer(replacement); result != MA_SUCCESS)
                return Error("Reinitialize audio mixer", result);
            ma_engine_set_time_in_pcm_frames(&p.Engine, clock);
            ma_engine_set_volume(&p.Engine, master);
            p.ListenerMailbox.Reset();
            p.ListenerMailbox.Publish(p.Listeners);
            p.ApplyMixerListeners();
            p.Buses        = std::make_unique<AudioBusGraph>();
            auto busResult =
                p.Buses->Init(p.Engine, layout, nullptr, p.DSPRegistry, p.UserSettings, ++p.ProfileGraphRevision);
            if (busResult == MA_SUCCESS)
                busResult = p.Buses->ConnectEndpoint();
            if (busResult != MA_SUCCESS)
                return Error("Rebuild audio bus graph", busResult);
            for (const auto& state : states)
            {
                auto&      v      = *state.Voice;
                auto&      source = *v.Source;
                const auto input  = source.InputMap;
                auto       result = source.ConfigureRouting(
                    input.data(), replacement.ChannelMap.data(), replacement.Channels, v.Desc.Spatial);
                if (result == MA_SUCCESS)
                    result = p.InitVoiceNodes(
                        v, v.Current().LoopBegin, v.Current().LoopEnd == v.Info.LengthFrames ? 0 : v.Current().LoopEnd);
                if (result != MA_SUCCESS)
                    return Error("Rebuild audio voice", result);
                auto& node = v.Sound;
                node.ResetSpatialMix();
                node.Block                      = NoFrame;
                node.engineNode.isPitchDisabled = state.PitchDisabled;
                if (!source.Generator)
                    node.Seek(state.Position);
                else
                    node.ResetResampler();
                node.atEnd = state.Ended;
                ma_sound_set_volume(&node, v.Desc.Volume);
                ma_sound_set_pan(&node, v.Desc.Pan);
                ma_sound_set_pitch(&node, state.Pitch);
                ma_node_set_time(&node, state.LocalTime);
                ma_sound_set_start_time_in_pcm_frames(&node, state.Start);
                ma_sound_set_stop_time_in_pcm_frames(&node, state.Stop);
                ma_node_set_state(&node, state.State);
                if (v.Desc.Spatial)
                    p.ApplySpatial(v);
                if (source.Stream && v.Attached && p.Config.Mode == AudioOutputMode::Device)
                    p.WakeStreams();
                if (state.Attached && !p.Attach(v))
                    return "Restore audio voice routing";
            }
            p.Profile.SetNodes(p.ProfileGraphRevision, p.Buses->GetBuses());
            return std::nullopt;
        };
        release();
        if (auto error = build(format))
        {
            release();
            if (auto rollback = build(previous))
            {
                release();
                for (auto& slot : p.Slots)
                    if (slot.Value)
                    {
                        slot.Value->Commands->Owner.store(nullptr);
                        slot.Value.reset();
                        if (++slot.Generation == 0)
                            ++slot.Generation;
                    }
                p.FreeSlot = UINT32_MAX;
                Shutdown();
                return *error + "; " + *rollback;
            }
            return error;
        }
        return std::nullopt;
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
            const auto read = m_Impl->MixPCM(out + written * GetOutputFormat().Channels,
                                             std::min<uint64_t>(256, frameCount - written));
            written += read;
            if (read == 0)
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
        UpdateOutput();
        if (!p.Ready)
            return;
        p.DrainJobs();
        p.Profile.Collect();
        p.CollectSchedules();
        const auto now          = GetClockFrames();
        const auto heard        = GetPlaybackClockFrames();
        p.Stats.StolenThisFrame = std::exchange(p.PendingSteals, 0);
        p.Stats.AllocatedVoices = p.Stats.VirtualVoices = 0;
        for (auto& [name, count] : p.Stats.GroupActiveCounts)
            count = 0;
        p.Candidates.clear();
        p.OcclusionCandidates.clear();
        ++p.OcclusionFrame;
        uint32_t protectedCount = 0;
        for (uint32_t i = 0; i < p.Slots.size(); ++i)
        {
            const auto& slot = p.Slots[i];
            if (!slot.Value)
                continue;
            auto&       voice = *slot.Value;
            voice.Sound.Binaural.Collect();
            const auto& state = voice.Current();
            p.ObserveVoice(voice, now, heard);
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
                if (!voice.Virtual)
                    p.QueueOcclusion(voice, p.Audibility(voice, now));
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
            const auto reason  = candidate.Eligible ? AudioVoiceReason::Priority : AudioVoiceReason::Inaudible;
            candidate.Eligible = candidate.Eligible && selected < budget;
            if (candidate.Eligible)
            {
                ++selected;
                p.QueueOcclusion(*candidate.Instance, candidate.Score);
            }
            else
            {
                if (!candidate.Instance->Virtual)
                    p.Virtualize(*candidate.Instance, now, reason);
                candidate.Instance->Acoustics.Samples       = 0;
                candidate.Instance->Acoustics.ReadyToRevive = false;
                ++p.Stats.VirtualVoices;
            }
        }
        p.UpdateOcclusion();
        const auto resume = p.WritableClock();
        for (const auto& candidate : p.Candidates)
        {
            auto& voice = *candidate.Instance;
            if (!candidate.Eligible || !voice.Virtual)
                continue;
            if ((p.WantsOcclusion(voice) && !voice.Acoustics.ReadyToRevive) || !p.Realize(voice, resume))
                ++p.Stats.VirtualVoices;
        }
        p.Stats.RealVoices = p.RealCount;
    }

    void AudioEngine::SetMaxRealVoices(uint32_t count) { m_Impl->Config.MaxRealVoices = count; }

    void AudioEngine::SetVirtualizationThreshold(float linear)
    {
        if (std::isfinite(linear) && linear >= 0)
            m_Impl->Config.VirtualizationThreshold = linear;
    }

    void AudioEngine::SetProfilingEnabled(bool enabled)
    {
        m_Impl->Profile.Enabled.store(enabled, std::memory_order_relaxed);
    }

    bool AudioEngine::IsProfilingEnabled() const { return m_Impl->Profile.Enabled.load(std::memory_order_relaxed); }

    AudioProfileStats AudioEngine::GetProfileStats()
    {
        auto& p = *m_Impl;
        p.Profile.Collect();
        AudioProfileStats stats;
        p.Profile.ReadStats(stats);
        stats.Occlusion      = p.OcclusionStats;
        stats.Underruns      = p.RetiredUnderruns;
        stats.UnderrunFrames = p.RetiredUnderrunFrames;
        if (!p.Ready)
            return stats;
        const auto now = GetClockFrames();
        for (const auto& slot : p.Slots)
        {
            if (!slot.Value)
                continue;
            const auto& voice  = *slot.Value;
            const auto& source = *voice.Source;
            if (voice.Active(now))
            {
                if (voice.Virtual)
                    ++stats.VirtualVoices;
                else if (voice.CountedReal)
                    ++stats.RealVoices;
            }
            stats.Underruns += source.Underruns.load(std::memory_order_acquire);
            stats.UnderrunFrames += source.UnderrunFrames.load(std::memory_order_relaxed);
            if (source.Stream)
                stats.StreamingBufferBytes += source.PCM.size() * sizeof(float);
            else if (source.Generator)
                stats.GeneratorBufferBytes += source.PCM.size() * sizeof(float);
        }
        for (const auto& entry : p.Sounds)
            if (entry.second.DecodedRegistered)
                stats.DecodedBytes += entry.second.PCM.size() * sizeof(float);
        return stats;
    }

    void AudioEngine::GetVoiceSnapshot(std::vector<AudioVoiceInfo>& out)
    {
        auto&      p     = *m_Impl;
        size_t     count = 0;
        const auto now   = GetClockFrames();
        const auto heard = GetPlaybackClockFrames();
        for (const auto& slot : p.Slots)
        {
            if (!slot.Value)
                continue;
            const auto& voice  = *slot.Value;
            const auto& state  = voice.Current();
            const auto& source = *voice.Source;
            if (count == out.size())
                out.emplace_back();
            auto& row            = out[count++];
            row                  = {};
            row.Voice            = voice.ProfileHandle;
            row.Owner            = voice.Desc.Owner;
            row.Clip             = voice.Desc.Clip;
            row.Sound            = voice.Desc.Sound;
            const auto* bus      = p.BusPolicy(voice.Desc.Bus);
            row.Bus              = bus ? std::string_view(bus->Name) : std::string_view {};
            row.ConcurrencyGroup = voice.Desc.ConcurrencyGroup;
            row.Reason           = voice.ProfileReason;
            row.Priority         = voice.Desc.Priority;
            row.Spatializer      = p.Spatializer(voice);
            row.SpatialBlend     = voice.Desc.SpatialBlend;
            row.HRTFFallback     = p.HRTFFallback(voice);
            if (voice.BusHeld)
            {
                row.State  = AudioVoiceState::HeldByGamePause;
                row.Reason = AudioVoiceReason::PausedBus;
            }
            else if (voice.PausedAt != NoFrame)
                row.State = AudioVoiceState::Paused;
            else if (voice.Active(now))
            {
                if (now < state.Start)
                    row.State = AudioVoiceState::Scheduled;
                else if (voice.Virtual)
                    row.State = AudioVoiceState::Virtual;
                else if (voice.Stolen || (voice.Envelope.Out && now >= voice.Envelope.Out->Start))
                    row.State = AudioVoiceState::Stopping;
                else
                    row.State = AudioVoiceState::Playing;
            }
            const auto audibility = p.Audibility(voice, now);
            row.Audibility        = voice.Virtual ? audibility : audibility * RealVoiceMargin;
            row.Acoustics         = voice.Acoustics.Result;
            if (voice.Acoustics.LastServiceFrame)
            {
                row.OcclusionAgeFrames = p.OcclusionFrame - voice.Acoustics.LastServiceFrame;
                row.OcclusionStale     = *row.OcclusionAgeFrames > p.Config.OcclusionStaleFrames;
            }
            if (row.State == AudioVoiceState::Playing || row.State == AudioVoiceState::Stopping)
            {
                const auto dryGain =
                    p.VoiceGain(voice, now, p.Buses->DryGain(voice.Desc.Bus)) * Impl::CurrentAcousticMix(voice).Dry[1];
                if (dryGain > 0)
                    row.FinalGainDb = 20 * std::log10(dryGain);
            }
            if (voice.Listener < p.Config.MaxListeners && (voice.Desc.Spatial || voice.Desc.ReverbPosition))
            {
                row.Distance = voice.Desc.Spatial ?
                                   voice.ListenerDistance :
                                   glm::length(p.Listeners[voice.Listener].Position - *voice.Desc.ReverbPosition);
                row.Listener = static_cast<int32_t>(voice.Listener);
            }
            row.PositionSeconds     = voice.Position(heard) / voice.Info.SampleRate;
            row.LengthSeconds       = source.Generator ? std::numeric_limits<double>::infinity() :
                                                         double(voice.Info.LengthFrames) / voice.Info.SampleRate;
            row.Storage             = source.Stream    ? AudioVoiceStorage::Streaming :
                                      source.Generator ? AudioVoiceStorage::Generator :
                                                         AudioVoiceStorage::Decoded;
            const auto decodePasses = source.DecodePasses.load(std::memory_order_acquire);
            row.Stream              = {source.BufferedFrames(),
                                       source.Capacity,
                                       source.DecodeNanoseconds.load(std::memory_order_relaxed),
                                       decodePasses,
                                       source.Underruns.load(std::memory_order_acquire),
                                       source.UnderrunFrames.load(std::memory_order_relaxed)};
        }
        out.resize(count);
    }

    void AudioEngine::GetProfileNodeSnapshot(std::vector<AudioProfileNode>& out)
    {
        m_Impl->Profile.Collect();
        m_Impl->Profile.ReadNodes(out);
    }

    size_t AudioEngine::ReadProfileEvents(AudioProfileEventCursor& cursor, AudioProfileEvent* out, size_t capacity)
    {
        if (IsInitialized())
            m_Impl->ObserveVoices(GetClockFrames(), GetPlaybackClockFrames());
        return m_Impl->Profile.ReadEvents(cursor, out, capacity);
    }

    void AudioEngine::ProfileRejection(AudioVoiceReason reason, AudioVoiceOwner owner, AssetID clip, AudioSoundID sound)
    {
        if (IsInitialized())
            m_Impl->LogProfile({0, 0, AudioProfileEventType::Rejected, reason, {}, owner, clip, sound},
                               GetClockFrames());
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
        auto result =
            next->Init(p.Engine, layout, p.Buses.get(), p.DSPRegistry, p.UserSettings, ++p.ProfileGraphRevision);
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
        p.Profile.SetNodes(p.ProfileGraphRevision, p.Buses->GetBuses());
        RefreshBusPolicies();
        return std::nullopt;
    }

    const std::vector<AudioBusDefinition>& AudioEngine::GetBuses() const
    {
        static const std::vector<AudioBusDefinition> empty;
        return m_Impl->Buses ? m_Impl->Buses->GetBuses() : empty;
    }

    const AudioUserSettings& AudioEngine::GetUserSettings() const { return m_Impl->UserSettings; }

    std::optional<std::string> AudioEngine::LoadUserSettings()
    {
        const auto file = m_Impl->Config.UserSettingsFile;
        return file.empty() ? std::optional<std::string>("Audio settings storage is disabled") : LoadUserSettings(file);
    }

    std::optional<std::string> AudioEngine::LoadUserSettings(const std::filesystem::path& file)
    {
        auto& p = *m_Impl;
        if (!p.Ready)
            return "Audio engine is unavailable";
        std::vector<std::string> reasons;
        auto                     settings = AudioUserSettings::Load(file, reasons);
        for (const auto& reason : reasons)
            Warn(reason);
        const auto reason = settings.OutputDevice == p.UserSettings.OutputDevice ?
                                AudioDeviceChangeReason::SpeakerModeChanged :
                                AudioDeviceChangeReason::Selected;
        if (settings.OutputDevice != p.UserSettings.OutputDevice || settings.Speakers != GetSpeakerMode())
        {
            if (auto error = RebuildOutput(settings.OutputDevice, settings.Speakers, reason))
            {
                if (settings.OutputDevice == DefaultAudioOutputDevice)
                    return error;
                if (auto fallback = RebuildOutput(
                        DefaultAudioOutputDevice, settings.Speakers, AudioDeviceChangeReason::FellBackToDefault))
                    return fallback;
            }
        }
        p.Config.UserSettingsFile = file;
        p.UserSettings.Buses      = std::move(settings.Buses);
        p.Buses->UpdateFaders();
        return std::nullopt;
    }

    const std::filesystem::path& AudioEngine::GetUserSettingsFile() const { return m_Impl->Config.UserSettingsFile; }

    std::optional<std::string> AudioEngine::SaveUserSettings() const
    {
        const auto& p = *m_Impl;
        if (p.Config.UserSettingsFile.empty())
            return "Audio settings storage is disabled";
        auto settings = p.UserSettings;
        if (p.Buses)
            for (auto it = settings.Buses.begin(); it != settings.Buses.end();)
                it = p.Buses->Find(it->first) ? std::next(it) : settings.Buses.erase(it);
        return settings.Save(p.Config.UserSettingsFile);
    }

    std::optional<AudioUserBusSettings> AudioEngine::GetUserBusSettings(const std::string& bus) const
    {
        const auto& p = *m_Impl;
        if (!p.Buses || !p.Buses->Find(bus))
            return std::nullopt;
        const auto found = p.UserSettings.Buses.find(bus);
        return found == p.UserSettings.Buses.end() ? AudioUserBusSettings {} : found->second;
    }

    bool AudioEngine::SetUserBusVolumeDb(const std::string& bus, float db)
    {
        auto& p = *m_Impl;
        if (!p.Buses || !p.Buses->Find(bus))
            return false;
        auto& level = p.UserSettings.Buses[bus].VolumeDb;
        level       = std::isnan(db) ? 0.f : std::clamp(db, -80.f, 12.f);
        if (level != db)
            Warn(bus + ".VolumeDb " + (std::isnan(db) ? "is NaN; using 0 dB" : "clamped to [-80, 12] dB"));
        p.Buses->UpdateFaders();
        return true;
    }

    bool AudioEngine::SetUserBusMute(const std::string& bus, bool mute)
    {
        auto& p = *m_Impl;
        if (!p.Buses || !p.Buses->Find(bus))
            return false;
        p.UserSettings.Buses[bus].Mute = mute;
        p.Buses->UpdateFaders();
        return true;
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

    float AudioEngine::GetTimeScale() const { return m_Impl->TimeScale; }

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
            if (slot.Value && !slot.Value->Source->Generator && slot.Value->Desc.Sound == id)
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
        const bool generator = desc.Generator.has_value();
        auto it = m_Impl->Sounds.find(desc.Sound);
        if (!IsInitialized() || (generator ? !desc.Generator->IsValid() : it == m_Impl->Sounds.end()))
        {
            ProfileRejection(generator ? AudioVoiceReason::InvalidSource : AudioVoiceReason::MissingClip,
                             desc.Owner,
                             desc.Clip,
                             desc.Sound);
            Warn("Cannot create voice: sound is not registered or engine is not initialized");
            return {};
        }
        auto*                sound = generator ? nullptr : &it->second;
        const AudioSoundInfo info =
            generator ? AudioSoundInfo {desc.Generator->Channels, desc.Generator->SampleRate, NoFrame} : sound->Info;
        const bool stream = sound && desc.LoadMode == AudioLoadMode::Stream && sound->Encoded;
        if (sound && !stream)
        {
            if (auto error = m_Impl->EnsureDecoded(*sound))
            {
                ProfileRejection(AudioVoiceReason::InvalidSource, desc.Owner, desc.Clip, desc.Sound);
                Warn(*error);
                return {};
            }
        }
        const bool                   poolable = sound && oneShot && !stream;
        std::unique_ptr<Impl::Voice> voice;
        if (poolable && !sound->OneShots[desc.Spatial].empty())
        {
            auto& pool = sound->OneShots[desc.Spatial];
            voice = std::move(pool.back());
            pool.pop_back();
            --m_Impl->PooledOneShots;
        }
        else
            voice = std::make_unique<Impl::Voice>();
        voice->Desc = desc;
        voice->Acoustics              = {};
        voice->Acoustics.LastRayFrame = m_Impl->OcclusionFrame;
        voice->Acoustics.Settings     = desc.Occlusion.Resolve(m_Impl->Config.DefaultOcclusion.Resolve());
        if (!IsValidAudioOcclusion(voice->Acoustics.Settings))
        {
            voice->Desc.Occlusion     = {};
            voice->Acoustics.Settings = m_Impl->Config.DefaultOcclusion.Resolve();
        }
        voice->Route.ResetAcoustics({});
        voice->Desc.SpatialBlend = std::isfinite(desc.SpatialBlend) ? std::clamp(desc.SpatialBlend, 0.f, 1.f) : 1.f;
        if (generator)
            voice->Desc.Owner.Kind = AudioVoiceOwnerKind::Generator;
        voice->ProfileReason = AudioVoiceReason::None;
        voice->ProfileLive = voice->ProfilePendingStart = false;
        voice->ReportedUnderruns = voice->ReportedUnderrunFrames = 0;
        voice->ListenerDistance                                  = 0;
        voice->Desc.Volume = voice->Desc.Pitch = 1;
        voice->Desc.Pan                        = 0;
        voice->Info                            = info;
        voice->Desc.Loop                       = desc.Loop && !generator;
        voice->Desc.NeverVirtualize |= generator;
        voice->Poolable                        = poolable;
        Impl::Voice::Transport transport;
        transport.Frame     = GetClockFrames();
        transport.Loop      = voice->Desc.Loop;
        transport.LoopBegin = loopStart;
        transport.LoopEnd   = loopEnd != 0 ? loopEnd : info.LengthFrames;
        transport.Length    = info.LengthFrames;
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
            ma_data_source_set_loop_point_in_pcm_frames(voice->Source.get(), 0, info.LengthFrames);
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
            std::array<ma_channel, MaxOutputChannels> generatorMap {};
            if (generator)
                ma_channel_map_init_standard(
                    ma_standard_channel_map_default, generatorMap.data(), generatorMap.size(), info.Channels);
            auto result =
                voice->Source->Init(&m_Impl->Resources,
                                    m_Impl->VFS,
                                    sound ? (stream ? sound->EncodedName : sound->DecodedName).c_str() : nullptr,
                                    stream,
                                    GetMode() == AudioOutputMode::Offline,
                                    info,
                                    loopStart,
                                    loopEnd,
                                    sound ? sound->ChannelMap.data() : generatorMap.data(),
                                    m_Impl->Engine.listeners[0].config.pChannelMapOut,
                                    GetChannels(),
                                    desc.Spatial,
                                    generator,
                                    m_Impl->Config.StreamBufferFrames);
            if (result != MA_SUCCESS)
            {
                ProfileRejection(AudioVoiceReason::InvalidSource, voice->Desc.Owner, desc.Clip, desc.Sound);
                Warn(Error("Voice source init", result));
                return {};
            }
            result = m_Impl->InitVoiceNodes(*voice, loopStart, loopEnd);
            if (result != MA_SUCCESS)
            {
                Warn(Error("Initialize audio voice nodes", result));
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
            m_Impl->Candidates.reserve(m_Impl->Slots.capacity());
            m_Impl->OcclusionCandidates.reserve(m_Impl->Slots.capacity());
        }
        auto& slot       = m_Impl->Slots[index];
        m_Impl->FreeSlot = slot.NextFree;
        slot.Value = std::move(voice);
        AudioVoiceHandle handle{index, slot.Generation};
        slot.Value->ProfileHandle = handle;
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
        desc.Owner            = {AudioVoiceOwnerKind::OneShot, params.OwnerEntity, {}};
        desc.Spatializer      = params.Spatializer;
        desc.SpatialBlend     = params.SpatialBlend;
        desc.Occlusion        = params.Occlusion;
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
        desc.Clip = clip ? clip->assetID : AssetID {};
        if (IsBusPaused(desc.Bus))
        {
            ProfileRejection(AudioVoiceReason::PausedBus, desc.Owner, desc.Clip);
            return {};
        }
        const auto sound = registry.Acquire(*this, clip);
        if (!sound)
        {
            ProfileRejection(AudioVoiceReason::MissingClip, desc.Owner, desc.Clip);
            return {};
        }
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

    uint32_t AudioEngine::PushGeneratorFrames(AudioVoiceHandle handle, const float* pcm, size_t sampleCount)
    {
        auto* voice = m_Impl->Find(handle);
        return voice && voice->Source->Generator ? voice->Source->Push(pcm, sampleCount) : 0;
    }

    uint32_t AudioEngine::GetFramesAvailableToPush(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        return voice && voice->Source->Generator ? voice->Source->PushRoom() : 0;
    }

    uint64_t AudioEngine::GetGeneratorUnderrunFrames(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        return voice && voice->Source->Generator ? voice->Source->UnderrunFrames.load(std::memory_order_relaxed) : 0;
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
        const auto now = GetClockFrames();
        p.ObserveVoice(*voice, now, GetPlaybackClockFrames());
        p.ProfileStop(*voice, now);
        p.RetiredUnderruns += voice->ReportedUnderruns;
        p.RetiredUnderrunFrames += voice->ReportedUnderrunFrames;
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
        const auto plan = [&](const AudioConcurrencyGroup& definition, AudioVoiceReason reason, const auto& belongs) {
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
                victims.push_back({victim,
                                   definition.ReleaseSeconds,
                                   definition.Rule == AudioConcurrencyRule::StopLowestPriority ?
                                       AudioVoiceReason::Priority :
                                       reason});
            }
        };
        const auto event  = p.EventGroups.find(voice.EventGroup);
        const auto shared = p.Groups.find(voice.Desc.ConcurrencyGroup);
        auto       reason = AudioVoiceReason::None;
        if (event != p.EventGroups.end() &&
            !plan(event->second, AudioVoiceReason::InstanceLimit, [&](const auto& member) {
                return member.EventGroup == event->first;
            }))
            reason = AudioVoiceReason::InstanceLimit;
        else if (shared != p.Groups.end() &&
                 !plan(shared->second, AudioVoiceReason::ConcurrencyLimit, [&](const auto& member) {
                     return member.Desc.ConcurrencyGroup == shared->first;
                 }))
            reason = AudioVoiceReason::ConcurrencyLimit;
        if (reason != AudioVoiceReason::None)
        {
            voice.ProfileReason = reason;
            p.LogVoice(AudioProfileEventType::Rejected, voice, now, reason);
            ++p.Stats.RejectedStarts;
            return false;
        }
        for (const auto& victim : victims)
        {
            auto& stolen  = *p.Find(victim.Handle);
            stolen.Stolen = true;
            stolen.ProfileReason = victim.Reason;
            p.LogVoice(AudioProfileEventType::Stolen, stolen, now, victim.Reason);
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
        voice.ProfileReason = AudioVoiceReason::None;
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
            const bool ended = voice->Ended(static_cast<double>(now)) || voice->NativeEnded();
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
            {
                m_Impl->ObserveVoice(*voice, now, GetPlaybackClockFrames());
                m_Impl->ProfileStop(*voice, now);
                state.Source = 0;
            }
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
            if (!voice->Protected(now))
            {
                const bool overBudget = m_Impl->RealCount >= m_Impl->Config.MaxRealVoices;
                const bool inaudible =
                    !overBudget && m_Impl->Audibility(*voice, now) < m_Impl->Config.VirtualizationThreshold;
                if (overBudget || inaudible)
                {
                    voice->ProfilePendingStart = true;
                    if (state.Start <= now)
                        m_Impl->ProfileStart(*voice, now);
                    m_Impl->Virtualize(
                        *voice, now, overBudget ? AudioVoiceReason::Priority : AudioVoiceReason::Inaudible);
                    return true;
                }
            }
            if (voice->Virtual)
            {
                if (!m_Impl->WantsOcclusion(*voice) && !m_Impl->Realize(*voice, m_Impl->WritableClock()))
                    return false;
                voice->ProfilePendingStart = true;
                if (state.Start <= now)
                    m_Impl->ProfileStart(*voice, now);
                return true;
            }
            if (!m_Impl->Attach(*voice))
                return false;
            voice->ProfilePendingStart = true;
            if (state.Start <= now)
                m_Impl->ProfileStart(*voice, now);
            m_Impl->CountReal(*voice, true);
            m_Impl->SetStreamActive(*voice, true);
            if (voice->Scheduled)
            {
                m_Impl->Queue(*voice, ended);
                return true;
            }
            ma_sound_set_start_time_in_pcm_frames(&voice->Sound, state.Start);
            ma_sound_set_stop_time_in_pcm_frames(&voice->Sound, state.Stop);
            if (ma_sound_at_end(&voice->Sound) && voice->Sound.BinauralBuffered.load(std::memory_order_acquire) &&
                !voice->Sound.ResetBinauralPending.load(std::memory_order_acquire))
                ma_node_set_state(&voice->Sound, ma_node_state_started);
            else
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
        if (voice->Virtual && m_Impl->WantsOcclusion(*voice))
        {
            // An explicit scheduled start keeps its transport deadline, but its
            // route stays silent until the next complete fresh query.
            voice->Acoustics.HeldForOcclusion = true;
            voice->Acoustics.Samples          = 0;
            voice->Route.ResetAcoustics(Impl::CurrentAcousticMix(*voice));
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
        voice->ProfilePendingStart = true;
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
            const auto now = GetClockFrames();
            m_Impl->ObserveVoice(*voice, now, GetPlaybackClockFrames());
            m_Impl->ProfileStop(*voice, now);
            voice->ProfilePendingStart = false;
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
            {
                ma_sound_seek_to_pcm_frame(&voice->Sound, 0);
                if (voice->HasHRTFEffect)
                    voice->Sound.ResetBinauralPending.store(true, std::memory_order_release);
            }
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
            m_Impl->ObserveVoice(*voice, now, GetPlaybackClockFrames());
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
        if (auto* voice = m_Impl->Find(handle); voice && !voice->Source->Generator && voice->Desc.Loop != looping)
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
        if (auto* voice = m_Impl->Find(handle); voice && !voice->Source->Generator)
        {
            auto state = voice->Current();
            frame      = std::min(frame, voice->Info.LengthFrames);
            if (state.Loop && frame >= state.LoopEnd)
                frame = state.LoopBegin + (frame - state.LoopBegin) % (state.LoopEnd - state.LoopBegin);
            const bool streamingDevice = voice->Source->Stream && !voice->Source->Offline;
            const bool binaural = voice->HasHRTFEffect && m_Impl->Spatializer(*voice) == AudioSpatializer::HRTF &&
                                  voice->Desc.SpatialBlend > 0 &&
                                  m_Impl->HRTFFallback(*voice) == AudioHRTFFallback::None;
            const bool prepare  = (streamingDevice || binaural) && !voice->Virtual;
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
                if (streamingDevice)
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
                {
                    if (voice->HasHRTFEffect)
                        voice->Sound.ResetBinaural();
                    ma_sound_seek_to_pcm_frame(&voice->Sound, beginFrame);
                }
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

    void AudioEngine::SetVoiceOcclusion(AudioVoiceHandle handle, const AudioOcclusionOverrides& overrides)
    {
        auto& p     = *m_Impl;
        auto* voice = p.Find(handle);
        if (!voice || voice->Desc.Occlusion == overrides ||
            !IsValidAudioOcclusion(overrides.Resolve(p.Config.DefaultOcclusion.Resolve())))
            return;
        voice->Desc.Occlusion = overrides;
        p.RefreshOcclusionSettings(*voice);
    }

    AudioOcclusionResult AudioEngine::GetVoiceOcclusion(AudioVoiceHandle handle) const
    {
        const auto* voice = m_Impl->Find(handle);
        return voice ? voice->Acoustics.Result : AudioOcclusionResult {};
    }

    void
    AudioEngine::SetOcclusionDefaults(const AudioOcclusionOverrides& overrides, uint32_t budget, uint32_t staleFrames)
    {
        auto& p = *m_Impl;
        if (!IsValidAudioOcclusion(overrides.Resolve()))
            return;
        p.Config.DefaultOcclusion         = overrides;
        p.Config.MaxOcclusionRaysPerFrame = budget;
        p.Config.OcclusionStaleFrames     = staleFrames;
        for (auto& slot : p.Slots)
            if (slot.Value)
                p.RefreshOcclusionSettings(*slot.Value);
    }

    AudioOcclusionSettings AudioEngine::GetOcclusionDefaults() const
    {
        return m_Impl->Config.DefaultOcclusion.Resolve();
    }

    uint32_t AudioEngine::GetMaxOcclusionRaysPerFrame() const { return m_Impl->Config.MaxOcclusionRaysPerFrame; }

    void AudioEngine::SetOcclusionQuery(const AudioOcclusionQuery& query)
    {
        auto& p = *m_Impl;
        if (p.OcclusionQuery.Context == query.Context && p.OcclusionQuery.Trace == query.Trace &&
            p.OcclusionQuery.Begin == query.Begin)
            return;
        p.OcclusionQuery = query;
        for (auto& slot : p.Slots)
            if (slot.Value)
            {
                auto& voice           = *slot.Value;
                auto& work            = voice.Acoustics;
                work.Samples          = 0;
                work.ReadyToRevive    = false;
                work.LastServiceFrame = 0;
                p.SetAcoustics(voice, {}, 1);
            }
    }

    void AudioEngine::ClearOcclusionQuery(const void* context)
    {
        if (m_Impl->OcclusionQuery.Context == context)
            SetOcclusionQuery({});
    }

    void AudioEngine::SetOcclusionDebugDraw(AudioOcclusionDebugDraw draw, void* context)
    {
        m_Impl->OcclusionDebugDraw    = draw;
        m_Impl->OcclusionDebugContext = context;
    }

    void
    AudioEngine::SetVoiceSpatializer(AudioVoiceHandle handle, std::optional<AudioSpatializer> spatializer, float blend)
    {
        auto* voice = m_Impl->Find(handle);
        if (!voice || !std::isfinite(blend))
            return;
        blend = std::clamp(blend, 0.f, 1.f);
        if (voice->Desc.Spatializer == spatializer && voice->Desc.SpatialBlend == blend)
            return;
        voice->Desc.Spatializer  = spatializer;
        voice->Desc.SpatialBlend = blend;
        if (voice->Desc.Spatial)
            m_Impl->ApplySpatial(*voice);
    }

    void AudioEngine::SetDefaultSpatializer(AudioSpatializer spatializer)
    {
        if (m_Impl->Config.DefaultSpatializer != spatializer)
        {
            m_Impl->Config.DefaultSpatializer = spatializer;
            m_Impl->RefreshSpatializers();
        }
    }

    AudioSpatializer AudioEngine::GetDefaultSpatializer() const { return m_Impl->Config.DefaultSpatializer; }
    bool             AudioEngine::IsHRTFAvailable() const { return m_Impl->HRTF && m_Impl->HRTF->IsReady(); }
    AssetID AudioEngine::GetActiveHRTF() const { return m_Impl->ActiveHRTF ? m_Impl->ActiveHRTF->assetID : AssetID {}; }

    std::optional<std::string> AudioEngine::SetActiveHRTF(AssetManager* assets, AssetID id)
    {
        auto&                     impl = *m_Impl;
        Ref<const AudioHRTFAsset> requested;
        if (id.IsValid())
        {
            if (!assets)
                return "Audio HRTF selection requires an asset manager";
            auto loaded = assets->Load<AudioHRTFAsset>(id);
            if (const auto* error = std::get_if<AssetError>(&loaded))
                return error->Message;
            requested = std::get<Ref<const AudioHRTFAsset>>(std::move(loaded));
        }
        if (requested == impl.ActiveHRTF)
            return std::nullopt;
        if (!impl.HRTF)
            return "Steam Audio context unavailable";
        if (auto error = impl.HRTF->Select(requested.get()))
            return error;
        impl.ActiveHRTF = std::move(requested);
        ++impl.HRTFRevision;
        impl.RefreshSpatializers();
        return std::nullopt;
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
