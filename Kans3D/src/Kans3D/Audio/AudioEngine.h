#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <glm/vec3.hpp>
#include "Kans3D/Audio/AudioBusLayout.h"

namespace Kans
{
    class AudioClipRegistry;
    enum class AudioOutputMode { Device, Offline };
    struct AudioEngineConfig
    {
        AudioOutputMode Mode = AudioOutputMode::Device;
        uint32_t SampleRate = 48000;
        uint32_t Channels = 2;
        uint32_t MaxListeners = 1;
        bool UseNullDevice = false; // Device mode: real-time silent playback without opening hardware.
    };

    using AudioSoundID = uint64_t;
    enum class AudioLoadMode { Decompress, Stream };
    struct AudioSoundInfo
    {
        uint32_t Channels, SampleRate;
        uint64_t LengthFrames;
    };
    struct AudioVoiceHandle
    {
        uint32_t Index = 0, Generation = 0;
        bool IsValid() const { return Generation != 0; }
    };
    struct AudioVoiceDesc
    {
        AudioSoundID Sound = 0;
        AudioLoadMode LoadMode = AudioLoadMode::Decompress;
        float Volume = 1, Pitch = 1, Pan = 0;
        bool Loop = false;
        bool AutoRelease = false;
        bool Spatial = false;
        std::string   Bus         = MasterAudioBus;
    };
    enum class AudioAttenuationModel { None, Inverse, Linear, Exponential };
    struct AudioSpatialParams
    {
        glm::vec3 Position{0}, Velocity{0}, Direction{0, 0, -1};
        AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
        float MinDistance = 1, MaxDistance = 100, Rolloff = 1, DopplerFactor = 1;
        float ConeInnerAngle = 360, ConeOuterAngle = 360, ConeOuterGain = 0;
    };
    struct AudioListenerState
    {
        glm::vec3 Position{0}, Velocity{0}, Forward{0, 0, -1}, Up{0, 1, 0};
    };

    struct AudioBusChannelMeter
    {
        float Peak = 0, Rms = 0; // Linear full-scale units for the most recent processing block.
    };

    struct AudioBusMeter
    {
        std::vector<AudioBusChannelMeter> Channels;
    };

    // All public calls are main-thread only. Handles must be used with their owning engine.
    class AudioEngine
    {
    public:
        AudioEngine();
        ~AudioEngine();
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        [[nodiscard]] std::optional<std::string> Init(const AudioEngineConfig& config);
        void Shutdown();
        bool IsInitialized() const;
        AudioOutputMode GetMode() const;
        uint32_t GetSampleRate() const;
        uint32_t GetChannels() const;
        void SetMasterVolume(float linear);
        float GetMasterVolume() const;
        uint64_t Render(float* out, uint64_t frameCount);
        void Update();

        // Runtime settings are copies of the asset. Send indices follow the order in GetBuses().
        [[nodiscard]] std::optional<std::string> ApplyBusLayout(const AudioBusLayout& layout);
        const std::vector<AudioBusDefinition>&   GetBuses() const;
        bool                                     SetBusVolumeDb(const std::string& bus, float db);
        bool                                     SetBusMute(const std::string& bus, bool mute);
        bool                                     SetBusSolo(const std::string& bus, bool solo);
        bool                                     SetBusBypass(const std::string& bus, bool bypass);
        bool                                     SetBusSendLevelDb(const std::string& bus, size_t sendIndex, float db);
        std::optional<AudioBusMeter>             GetBusMeter(const std::string& bus) const;

        // Effect indices follow chain order. Reacquire the view after structural edits or ApplyBusLayout.
        const std::vector<AudioBusEffect>& GetBusEffects(const std::string& bus) const;
        [[nodiscard]] std::optional<std::string>
        AddBusEffect(const std::string& bus, const AudioBusEffect& effect, std::optional<size_t> index = std::nullopt);
        [[nodiscard]] std::optional<std::string> RemoveBusEffect(const std::string& bus, size_t index);
        [[nodiscard]] std::optional<std::string> MoveBusEffect(const std::string& bus, size_t from, size_t to);
        // Replace the typed parameters of the existing effect, preserving its enabled flag.
        [[nodiscard]] std::optional<std::string>
             SetBusEffectParameters(const std::string& bus, size_t index, const AudioBusEffectParameters& parameters);
        bool SetBusEffectEnabled(const std::string& bus, size_t index, bool enabled);
        // Positive attenuation in dB, excluding compressor makeup gain. Available for dynamics effects.
        std::optional<float> GetBusEffectGainReductionDb(const std::string& bus, size_t index) const;

        // Registrations are engine-local. Registering a name rebinds every slot that already references it.
        bool RegisterCustomDSP(const std::string& name, AudioDSPCallback callback, void* user = nullptr);
        // Disables all existing instances and waits on the main thread for any in-flight invocation.
        // The callback and user data may be released as soon as this returns.
        bool UnregisterCustomDSP(const std::string& name);

        std::optional<std::string> RegisterSound(AudioSoundID id, std::shared_ptr<const std::vector<std::byte>> encodedBytes);
        std::optional<std::string> RegisterSound(AudioSoundID id, std::vector<std::byte> encodedBytes)
        {
            return RegisterSound(id, std::make_shared<const std::vector<std::byte>>(std::move(encodedBytes)));
        }
        std::optional<std::string> RegisterSoundPCM(AudioSoundID id, std::vector<float> interleaved,
                                                  uint32_t channels, uint32_t sampleRate);
        void UnregisterSound(AudioSoundID id);
        bool IsSoundRegistered(AudioSoundID id) const;
        std::optional<AudioSoundInfo> GetSoundInfo(AudioSoundID id) const;
        // Resident decompressed PCM; encoded sounds decode on the first Decompress voice, streams never do.
        uint64_t GetDecodedBytes(AudioSoundID id) const;

        AudioVoiceHandle CreateVoice(const AudioVoiceDesc& desc);
        // Empty or unknown names route to Master. Returns false for an invalid handle or failed attachment.
        bool                       SetVoiceBus(AudioVoiceHandle handle, const std::string& bus);
        std::optional<std::string> GetVoiceBus(AudioVoiceHandle handle) const;
        void DestroyVoice(AudioVoiceHandle handle);
        bool IsVoiceValid(AudioVoiceHandle handle) const;
        void Play(AudioVoiceHandle handle);
        void Stop(AudioVoiceHandle handle);
        void Pause(AudioVoiceHandle handle);
        bool IsPlaying(AudioVoiceHandle handle) const;
        bool HasEnded(AudioVoiceHandle handle) const;
        void SetVolume(AudioVoiceHandle handle, float linear);
        void SetPitch(AudioVoiceHandle handle, float pitch);
        // Balance pan: -1 = left, +1 = right; attenuates the opposite channel by 1 - abs(pan).
        void SetPan(AudioVoiceHandle handle, float pan);
        void SetLooping(AudioVoiceHandle handle, bool looping);
        void SeekFrames(AudioVoiceHandle handle, uint64_t frame);
        // Decoder cursor in source frames; may lead audible playback by one processing block.
        uint64_t GetCursorFrames(AudioVoiceHandle handle) const;
        // Target is a multiplier relative to SetVolume; seconds are measured at the engine sample rate.
        void FadeTo(AudioVoiceHandle handle, float targetVolume, float seconds);
        void SetLoopRegion(AudioVoiceHandle handle, uint64_t beginFrame, uint64_t endFrame);
        void SetSpatialParams(AudioVoiceHandle handle, const AudioSpatialParams& params);
        void SetListener(uint32_t index, const AudioListenerState& state);

    private:
        std::optional<std::string> ReplaceBusGraph(const AudioBusLayout& layout);
        friend class AudioClipRegistry;
        void AttachClipRegistry(AudioClipRegistry* registry);
        void DetachClipRegistry(AudioClipRegistry* registry);
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
