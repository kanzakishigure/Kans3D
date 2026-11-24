#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <glm/vec3.hpp>
#include "Kans3D/Audio/AudioBusLayout.h"

namespace Kans
{
    class AudioClipRegistry;
    class AudioClipAsset;
    class AudioEventPlayer;
    inline constexpr uint8_t DefaultAudioVoicePriority = 128;
    enum class AudioOutputMode { Device, Offline };
    struct AudioEngineConfig
    {
        AudioOutputMode Mode = AudioOutputMode::Device;
        uint32_t SampleRate = 48000;
        uint32_t Channels = 2;
        uint32_t MaxListeners = 1;
        bool UseNullDevice = false; // Device mode: real-time silent playback without opening hardware.
        uint32_t        MaxRealVoices = 64;
        float           VirtualizationThreshold = 0.001f; // Linear amplitude (-60 dB). Zero disables the threshold.
    };

    using AudioSoundID = uint64_t;
    enum class AudioLoadMode { Decompress, Stream };
    enum class AudioFadeCurve
    {
        Linear,
        EqualPower,
        SCurve
    };
    struct AudioVoiceWindow
    {
        // Output frames. FadeInElapsed and FadeOutElapsed preserve the authored envelope after a seek.
        uint64_t                Duration = std::numeric_limits<uint64_t>::max();
        uint64_t                FadeIn = 0, FadeOut = 0, FadeInElapsed = 0, FadeOutElapsed = 0;
        std::optional<uint64_t> FadeOutEnd; // Offset from scheduled start. Defaults to Duration.
        AudioFadeCurve          InCurve = AudioFadeCurve::Linear, OutCurve = AudioFadeCurve::Linear;
    };
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
        uint8_t       Priority    = DefaultAudioVoicePriority; // 255 is highest.
        std::string   ConcurrencyGroup;
        bool          NeverVirtualize = false;
    };

    enum class AudioConcurrencyRule
    {
        StopOldest,
        StopQuietest,
        StopLowestPriority,
        RejectNew
    };
    struct AudioConcurrencyGroup
    {
        uint32_t             MaxCount       = 1;
        AudioConcurrencyRule Rule           = AudioConcurrencyRule::RejectNew;
        float                ReleaseSeconds = 0.02f;
    };

    struct AudioOneShotParams
    {
        std::optional<glm::vec3> Position;
        float                    Volume = 1, Pitch = 1;
        std::string              Bus      = MasterAudioBus;
        uint8_t                  Priority = DefaultAudioVoicePriority;
        std::string              ConcurrencyGroup;
    };

    struct AudioVoiceStats
    {
        uint32_t                        RealVoices = 0, VirtualVoices = 0, AllocatedVoices = 0;
        uint64_t                        StolenThisFrame = 0, TotalStolen = 0, RejectedStarts = 0;
        std::map<std::string, uint32_t> GroupActiveCounts;
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

    class AudioMusicPlayer;

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
        // Absolute engine output frames mixed since Init, including Offline renders.
        uint64_t GetClockFrames() const;
        // First output frame available to schedule on the main thread.
        uint64_t GetSchedulingClockFrames() const;
        // Completed worker wake-ups, exposed for diagnostics.
        uint64_t GetStreamWorkerPasses() const;
        bool     IsStreamSeekPending(AudioVoiceHandle handle) const;
        // Estimated queued device frames (backend playback period size times periods, in engine frames).
        // Offline has no device latency.
        uint64_t GetOutputLatencyFrames() const;
        // Monotonic listener time in seconds, interpolated between device callbacks and bounded by the
        // mixed clock. Offline returns the exact rendered time without wall-clock interpolation.
        double   GetPlaybackClockSeconds() const;
        double   GetPlaybackClockFrames() const;
        void SetMasterVolume(float linear);
        float GetMasterVolume() const;
        uint64_t Render(float* out, uint64_t frameCount);
        void Update();
        void     SetMaxRealVoices(uint32_t count);
        void     SetVirtualizationThreshold(float linear);
        // Counts are published by Update. Groups count admitted starts, including virtual and scheduled voices.
        const AudioVoiceStats& GetVoiceStats() const;
        AudioEventPlayer&       GetEventPlayer();
        const AudioEventPlayer& GetEventPlayer() const;
        bool                    IsVoiceReleasing(AudioVoiceHandle handle) const;
        // Configure before playing. Reducing a group's limit applies on subsequent starts.
        bool DefineConcurrencyGroup(const std::string& name, const AudioConcurrencyGroup& group);
        bool IsVoiceVirtual(AudioVoiceHandle handle) const;
        void SetVoicePriority(AudioVoiceHandle handle, uint8_t priority);

        // Auto-releases the voice and the registry reference. Stop(handle) also releases it.
        // Returns an invalid handle for an unavailable clip or a rejected start.
        AudioVoiceHandle PlayOneShot(AudioClipRegistry&                           registry,
                                     const std::shared_ptr<const AudioClipAsset>& clip,
                                     const AudioOneShotParams&                    params = {});

        // Runtime settings are copies of the asset. Send indices follow the order in GetBuses().
        [[nodiscard]] std::optional<std::string> ApplyBusLayout(const AudioBusLayout& layout);
        const std::vector<AudioBusDefinition>&   GetBuses() const;
        bool                                     SetBusVolumeDb(const std::string& bus, float db);
        bool                                     SetBusMute(const std::string& bus, bool mute);
        bool                                     SetBusSolo(const std::string& bus, bool solo);
        bool                                     SetBusBypass(const std::string& bus, bool bypass);
        bool                                     SetBusSendLevelDb(const std::string& bus, size_t sendIndex, float db);
        std::optional<AudioBusMeter>             GetBusMeter(const std::string& bus) const;
        // GetBusMeter into a caller-owned meter, reusing its channel storage (no allocation once sized).
        bool ReadBusMeter(const std::string& bus, AudioBusMeter& meter) const;

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
        // Names of the registered custom DSP effects, sorted.
        std::vector<std::string> GetCustomDSPNames() const;

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
        // Returns false for an invalid handle or a start rejected by its concurrency group.
        bool Play(AudioVoiceHandle handle);
        // Schedule playback from the current source position (or zero after EOF). A past start skips
        // the elapsed engine frames at the voice's current pitch and starts at the first available frame.
        // Explicit starts reserve a real voice through their first output frame.
        bool ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame);
        bool ScheduleStart(AudioVoiceHandle handle, uint64_t clockFrame, const AudioVoiceWindow& window);
        // Silence begins at clockFrame. An optional fade occupies the preceding fadeFrames engine
        // frames. Past stop times take effect at the first available frame.
        void ScheduleStop(AudioVoiceHandle handle,
                          uint64_t         clockFrame,
                          uint64_t         fadeFrames = 0,
                          AudioFadeCurve   curve      = AudioFadeCurve::Linear);
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
        // Source position on the latency-compensated transport, including pitch and loop wraps.
        // Before the start or while paused, returns the source position held by the transport.
        uint64_t GetVoicePositionFrames(AudioVoiceHandle handle) const;
        // The same transport position in seconds at the source sample rate, retaining fractional frames.
        double GetVoicePositionSeconds(AudioVoiceHandle handle) const;
        // Target is a multiplier relative to SetVolume; seconds are measured at the engine sample rate.
        void FadeTo(AudioVoiceHandle handle, float targetVolume, float seconds);
        void SetLoopRegion(AudioVoiceHandle handle, uint64_t beginFrame, uint64_t endFrame);
        void SetSpatialParams(AudioVoiceHandle handle, const AudioSpatialParams& params);
        void SetListener(uint32_t index, const AudioListenerState& state);

    private:
        friend class AudioMusicPlayer;
        friend class AudioEventPlayer;
        void             DefineEventGroup(AssetID id, const AudioConcurrencyGroup& group);
        uint32_t         CountEventInstances(AssetID id) const;
        bool             PlayVoice(AudioVoiceHandle handle, bool* rejectedByLimit);
        AudioVoiceHandle PlayRegisteredOneShot(AudioClipRegistry&                           registry,
                                               const std::shared_ptr<const AudioClipAsset>& clip,
                                               AudioVoiceDesc&                              desc,
                                               const AudioSpatialParams*                    spatial,
                                               double                                       startOffsetSeconds,
                                               bool*                                        rejectedByLimit,
                                               AssetID                                      eventGroup = {});
        AudioVoiceHandle
        CreateVoice(const AudioVoiceDesc& desc, uint64_t loopStart, uint64_t loopEnd, bool oneShot = false);
        uint64_t                   StartVoice(AudioVoiceHandle        handle,
                                              uint64_t                clockFrame,
                                              uint64_t                fadeFrames,
                                              AudioFadeCurve          curve,
                                              const AudioVoiceWindow* window = nullptr);
        std::optional<std::string> ReplaceBusGraph(const AudioBusLayout& layout);
        bool                       AdmitVoice(AudioVoiceHandle handle, uint64_t now);
        friend class AudioClipRegistry;
        void AttachClipRegistry(AudioClipRegistry* registry);
        void DetachClipRegistry(AudioClipRegistry* registry);
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        std::unique_ptr<AudioEventPlayer> m_EventPlayer;
    };
}
