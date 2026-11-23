#include "kspch.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/Audio/AudioClipRegistry.h"
#include "Kans3D/Core/Log/Log.h"
#include "AudioBusGraph.h"

#include <miniaudio.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>

namespace Kans
{
    namespace
    {
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
        // Looping belongs to this public data source, so stream pages contain ordinary source frames.
        // Changing loop points or disabling looping does not leave old loops in prefetched decoder pages.
        struct VoiceSource
        {
            ma_data_source_base Base{};
            ma_resource_manager_data_source Data{};
            ma_resource_manager* Resources = nullptr;
            bool Offline = false, Initialized = false;

            void DrainJobs()
            {
                if (Offline)
                    while (ma_resource_manager_process_next_job(Resources) == MA_SUCCESS) {}
            }

            ma_result Init(ma_resource_manager* resources, const char* path, bool stream, bool offline)
            {
                Resources = resources;
                Offline = offline;
                auto result = ma_resource_manager_data_source_init(resources, path,
                    stream ? MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM : MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE,
                    nullptr, &Data);
                if (result != MA_SUCCESS)
                    return result;
                static const ma_data_source_vtable callbacks = {
                    [](ma_data_source* source, void* out, ma_uint64 count, ma_uint64* read) -> ma_result
                    {
                        auto& self = *static_cast<VoiceSource*>(source);
                        self.DrainJobs();
                        return ma_data_source_read_pcm_frames(&self.Data, out, count, read);
                    },
                    [](ma_data_source* source, ma_uint64 frame) -> ma_result
                    {
                        auto& self = *static_cast<VoiceSource*>(source);
                        auto result = ma_data_source_seek_to_pcm_frame(&self.Data, frame);
                        self.DrainJobs();
                        return result;
                    },
                    [](ma_data_source* source, ma_format* format, ma_uint32* channels, ma_uint32* rate,
                       ma_channel* map, size_t capacity) -> ma_result
                    {
                        return ma_data_source_get_data_format(&static_cast<VoiceSource*>(source)->Data,
                            format, channels, rate, map, capacity);
                    },
                    [](ma_data_source* source, ma_uint64* cursor) -> ma_result
                    {
                        return ma_data_source_get_cursor_in_pcm_frames(&static_cast<VoiceSource*>(source)->Data, cursor);
                    },
                    [](ma_data_source* source, ma_uint64* length) -> ma_result
                    {
                        return ma_data_source_get_length_in_pcm_frames(&static_cast<VoiceSource*>(source)->Data, length);
                    },
                    nullptr,
                    0
                };
                auto config = ma_data_source_config_init();
                config.vtable = &callbacks;
                result = ma_data_source_init(&config, &Base);
                if (result != MA_SUCCESS)
                {
                    ma_resource_manager_data_source_uninit(&Data);
                    return result;
                }
                Initialized = true;
                return MA_SUCCESS;
            }

            ~VoiceSource()
            {
                if (Initialized)
                {
                    ma_data_source_uninit(&Base);
                    ma_resource_manager_data_source_uninit(&Data);
                }
            }
        };
        struct Voice
        {
            VoiceSource Source;
            ma_sound Sound{};
            AudioVoiceDesc Desc;
            bool Initialized = false;
            ~Voice() { if (Initialized) ma_sound_uninit(&Sound); }
        };
        struct Slot
        {
            uint32_t Generation = 1;
            std::unique_ptr<Voice> Value;
        };

        MemoryVFS VFS;
        ma_context Context{};
        ma_resource_manager Resources{};
        ma_engine Engine{};
        bool ContextReady = false, ResourcesReady = false, Ready = false;
        AudioEngineConfig Config;
        std::unique_ptr<AudioBusGraph>  Buses;
        std::unordered_set<std::string> WarnedBusNames;

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
        if (config.Mode == AudioOutputMode::Offline)
        {
            resourcesConfig.jobThreadCount = 0;
            resourcesConfig.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
        }
        auto result = ma_resource_manager_init(&resourcesConfig, &p.Resources);
        if (result != MA_SUCCESS)
        {
            Shutdown();
            return Error("Resource manager init", result);
        }
        p.ResourcesReady = true;
        auto engineConfig = ma_engine_config_init();
        engineConfig.pResourceManager = &p.Resources;
        engineConfig.pContext = p.ContextReady ? &p.Context : nullptr;
        engineConfig.noDevice = config.Mode == AudioOutputMode::Offline;
        engineConfig.channels = config.Channels;
        engineConfig.sampleRate = config.SampleRate;
        engineConfig.listenerCount = config.MaxListeners;
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
        if (p.Ready)
            ma_engine_uninit(&p.Engine);
        p.Ready = false;
        if (p.ResourcesReady)
            ma_resource_manager_uninit(&p.Resources);
        p.ResourcesReady = false;
        if (p.ContextReady)
            ma_context_uninit(&p.Context);
        p.ContextReady = false;
    }

    bool AudioEngine::IsInitialized() const { return m_Impl->Ready; }
    AudioOutputMode AudioEngine::GetMode() const { return m_Impl->Config.Mode; }
    uint32_t AudioEngine::GetSampleRate() const { return IsInitialized() ? ma_engine_get_sample_rate(&m_Impl->Engine) : 0; }
    uint32_t AudioEngine::GetChannels() const { return IsInitialized() ? ma_engine_get_channels(&m_Impl->Engine) : 0; }
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
        for (uint32_t i = 0; i < m_Impl->Slots.size(); ++i)
        {
            auto& slot = m_Impl->Slots[i];
            // ma_sound_at_end reads miniaudio's atomic flag; no engine callback runs on the audio thread.
            if (slot.Value && slot.Value->Desc.AutoRelease && ma_sound_at_end(&slot.Value->Sound))
                DestroyVoice({i, slot.Generation});
        }
    }

    std::optional<std::string> AudioEngine::ApplyBusLayout(const AudioBusLayout& layout)
    {
        auto& p = *m_Impl;
        if (!p.Ready)
            return "Audio engine is unavailable";
        if (auto error = ValidateAudioBusLayout(layout))
            return error->Message;

        // Build off the endpoint, then move existing sound nodes without touching their playback state.
        auto next   = std::make_unique<AudioBusGraph>();
        auto result = next->Init(p.Engine, layout, p.Buses.get());
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

    AudioVoiceHandle AudioEngine::CreateVoice(const AudioVoiceDesc& desc)
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
        auto result = voice->Source.Init(&m_Impl->Resources,
            (stream ? sound.EncodedName : sound.DecodedName).c_str(), stream, GetMode() == AudioOutputMode::Offline);
        if (result != MA_SUCCESS)
        {
            Warn(Error("Voice source init", result));
            return {};
        }
        auto config = ma_sound_config_init_2(&m_Impl->Engine);
        config.pDataSource = &voice->Source;
        config.pInitialAttachment = m_Impl->VoiceBusInput(*m_Impl->Buses, desc.Bus);
        if (!desc.Spatial)
            config.flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (desc.Loop)
            config.flags |= MA_SOUND_FLAG_LOOPING;
        result = ma_sound_init_ex(&m_Impl->Engine, &config, &voice->Sound);
        if (result != MA_SUCCESS)
        {
            Warn(Error("Voice init", result));
            return {};
        }
        voice->Initialized = true;
        uint32_t index = 0;
        for (; index < m_Impl->Slots.size(); ++index)
            if (!m_Impl->Slots[index].Value && m_Impl->Slots[index].Generation != 0)
                break;
        if (index == m_Impl->Slots.size())
            m_Impl->Slots.emplace_back();
        auto& slot = m_Impl->Slots[index];
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
        slot.Value.reset();
        // Retire a slot permanently on generation wrap so even ancient handles cannot become live again.
        ++slot.Generation;
    }
    bool AudioEngine::IsVoiceValid(AudioVoiceHandle handle) const { return m_Impl->Find(handle) != nullptr; }
    void AudioEngine::Play(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
            ma_sound_start(&voice->Sound);
    }
    void AudioEngine::Stop(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            ma_sound_stop(&voice->Sound);
            ma_sound_seek_to_pcm_frame(&voice->Sound, 0);
        }
    }
    void AudioEngine::Pause(AudioVoiceHandle handle)
    {
        if (auto* voice = m_Impl->Find(handle))
            ma_sound_stop(&voice->Sound);
    }
    bool AudioEngine::IsPlaying(AudioVoiceHandle handle) const
    {
        auto* voice = m_Impl->Find(handle);
        return voice && ma_sound_is_playing(&voice->Sound);
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
        }
    }
    void AudioEngine::SeekFrames(AudioVoiceHandle handle, uint64_t frame)
    {
        if (auto* voice = m_Impl->Find(handle))
        {
            const auto length = m_Impl->Sounds.at(voice->Desc.Sound).Info.LengthFrames;
            ma_sound_seek_to_pcm_frame(&voice->Sound, std::min(frame, length));
        }
    }
    uint64_t AudioEngine::GetCursorFrames(AudioVoiceHandle handle) const
    {
        ma_uint64 cursor = 0;
        if (auto* voice = m_Impl->Find(handle))
            ma_sound_get_cursor_in_pcm_frames(&voice->Sound, &cursor);
        return cursor;
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
            const auto length = m_Impl->Sounds.at(voice->Desc.Sound).Info.LengthFrames;
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
                ma_node_attach_output_bus(&voice->Sound, 0, ma_engine_get_endpoint(&m_Impl->Engine), 0);
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
