#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <glm/vec3.hpp>
#include "Kans3D/Audio/AudioBusLayout.h"
#include "AudioAttenuation.h"
#include "AudioEmitterShape.h"
#include "AudioOcclusion.h"
#include "AudioProfile.h"
#include "AudioUserSettings.h"

namespace Kans
{
    class AssetManager;
    class Scene;
    class AudioClipRegistry;
    class AudioClipAsset;
    class AudioEventPlayer;
    inline constexpr uint8_t DefaultAudioVoicePriority = 128;
    enum class AudioOutputMode { Device, Offline };
    struct AudioOutputFormat
    {
        uint32_t Channels = 0;
        // Channel positions use miniaudio's MA_CHANNEL values. Zero entries mean unknown.
        std::array<uint8_t, 8> ChannelMap {};
        bool                   operator==(const AudioOutputFormat& other) const
        {
            return Channels == other.Channels && ChannelMap == other.ChannelMap;
        }
        bool operator!=(const AudioOutputFormat& other) const { return !(*this == other); }
    };

    struct AudioOutputDevice
    {
        std::string       Id, Name;
        bool              IsDefault = false;
        AudioOutputFormat NativeFormat;
    };

    enum class AudioDeviceChangeReason
    {
        Selected,
        Rerouted,
        FellBackToDefault,
        SpeakerModeChanged
    };
    struct AudioDeviceChange
    {
        uint64_t                Revision = 0;
        std::string             DeviceId;
        AudioDeviceChangeReason Reason = AudioDeviceChangeReason::Selected;
        AudioOutputFormat       Format;
    };

    struct AudioEngineConfig
    {
        AudioOutputMode Mode = AudioOutputMode::Device;
        uint32_t SampleRate = 48000;
        uint32_t Channels = 2;
        uint32_t MaxListeners = 1;
        bool UseNullDevice = false; // Device mode: real-time silent playback without opening hardware.
        uint32_t        MaxRealVoices = 64;
        float           VirtualizationThreshold = 0.001f; // Linear amplitude (-60 dB). Zero disables the threshold.
        std::filesystem::path UserSettingsFile;                 // Empty leaves persistence disabled.
        uint32_t              StreamBufferFrames = 0;           // Source frames. Zero uses the default capacity.
        AudioSpatializer      DefaultSpatializer = AudioSpatializer::Panning;
        AudioOcclusionOverrides DefaultOcclusion;
        uint32_t                MaxOcclusionRaysPerFrame = DefaultMaxOcclusionRaysPerFrame;
        uint32_t                OcclusionStaleFrames     = DefaultOcclusionStaleFrames;
    };

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
    inline constexpr float    AudioSendOffDb     = -std::numeric_limits<float>::infinity();
    inline constexpr size_t   MaxAudioVoiceSends = 2;
    inline constexpr uint32_t MaxAudioListeners  = 4;

    struct AudioGeneratorFormat
    {
        uint32_t Channels = 1, SampleRate = 48000;
        bool     operator==(const AudioGeneratorFormat& other) const
        {
            return Channels == other.Channels && SampleRate == other.SampleRate;
        }
        bool operator!=(const AudioGeneratorFormat& other) const { return !(*this == other); }
        bool IsValid() const { return Channels >= 1 && Channels <= 8 && SampleRate >= 8000 && SampleRate <= 192000; }
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
        bool                     ReverbZoneSend = true, PinToCenter = false;
        float                    LfeSendDb = AudioSendOffDb;
        std::optional<glm::vec3> ReverbPosition; // Listener selection for a world-positioned 2D voice.
        std::optional<AudioGeneratorFormat> Generator;
        AudioVoiceOwner                     Owner;
        AssetID                             Clip;
        std::optional<AudioSpatializer>     Spatializer; // Empty inherits the project default.
        float                               SpatialBlend = 1;
        AudioOcclusionOverrides             Occlusion;
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
        bool                     Spatial = true; // Applies when Position is supplied.
        uint64_t                 OwnerEntity = 0;
        std::optional<AudioSpatializer> Spatializer;
        float                           SpatialBlend = 1;
        AudioOcclusionOverrides         Occlusion;
    };

    struct AudioVoiceStats
    {
        uint32_t                        RealVoices = 0, VirtualVoices = 0, AllocatedVoices = 0;
        uint64_t                        StolenThisFrame = 0, TotalStolen = 0, RejectedStarts = 0;
        std::map<std::string, uint32_t> GroupActiveCounts;
    };
    struct AudioSpatialParams
    {
        glm::vec3 Position{0}, Velocity{0}, Direction{0, 0, -1};
        AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
        float MinDistance = 1, MaxDistance = 100, Rolloff = 1, DopplerFactor = 1;
        float ConeInnerAngle = 360, ConeOuterAngle = 360, ConeOuterGain = 0;
        AudioEmitterShape     Shape;
        glm::mat4 ShapeTransform {1}; // Linear part maps the local shape to world space. Position supplies translation.
    };
    struct AudioZoneSend
    {
        // Index in GetBuses(), valid until the next layout change.
        size_t BusIndex = 0;
        float  Gain     = 0;
        bool   operator==(const AudioZoneSend& other) const { return BusIndex == other.BusIndex && Gain == other.Gain; }
    };

    struct AudioListenerState
    {
        glm::vec3 Position{0}, Velocity{0}, Forward{0, 0, -1}, Up{0, 1, 0};
        bool      Active   = true;
        uint64_t  Identity = 0; // Optional stable ID when active listeners are reassigned to different slots.
        std::array<AudioZoneSend, MaxAudioVoiceSends> ReverbSends {};
        bool                                          operator==(const AudioListenerState& other) const
        {
            return Position == other.Position && Velocity == other.Velocity && Forward == other.Forward &&
                   Up == other.Up && Active == other.Active && Identity == other.Identity &&
                   ReverbSends == other.ReverbSends;
        }
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
        uint64_t                                 GetInstanceID() const;
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
        std::vector<AudioOutputDevice> EnumerateOutputDevices() const;
        bool                           SetOutputDevice(std::string_view id = DefaultAudioOutputDevice);
        bool                           SetSpeakerMode(SpeakerMode mode);
        SpeakerMode                    GetSpeakerMode() const;
        // GetChannels() reports the bus mix. Offline Render writes the output format.
        AudioOutputFormat        GetOutputFormat() const;
        const AudioDeviceChange& GetLastDeviceChange() const;
        // Test seam. Offline only. The next Update handles this snapshot like a backend notification.
        void                                SimulateOutputDevices(std::vector<AudioOutputDevice> devices);
        const AudioUserSettings&            GetUserSettings() const;
        std::optional<std::string>          LoadUserSettings(const std::filesystem::path& file);
        std::optional<std::string>          LoadUserSettings();
        std::optional<std::string>          SaveUserSettings() const;
        // Where Load/SaveUserSettings read and write; empty while storage is disabled.
        const std::filesystem::path&        GetUserSettingsFile() const;
        std::optional<AudioUserBusSettings> GetUserBusSettings(const std::string& bus) const;
        bool                                SetUserBusVolumeDb(const std::string& bus, float db);
        bool                                SetUserBusMute(const std::string& bus, bool mute);
        void     SetMaxRealVoices(uint32_t count);
        void     SetVirtualizationThreshold(float linear);
        // The timing switch is sampled once per device callback or offline mix block. Profile reads are main-thread
        // only.
        void              SetProfilingEnabled(bool enabled);
        bool              IsProfilingEnabled() const;
        AudioProfileStats GetProfileStats();
        // Names are borrowed until the next audio update or control change.
        void GetVoiceSnapshot(std::vector<AudioVoiceInfo>& out);
        // Indices refer to GetBuses() and its effect slots. Refresh after a bus layout edit.
        void GetProfileNodeSnapshot(std::vector<AudioProfileNode>& out);
        // Start a fresh cursor after Init. Read retained entries in publication order and count missed entries.
        size_t ReadProfileEvents(AudioProfileEventCursor& cursor, AudioProfileEvent* out, size_t capacity);

        // Counts are published by Update. Groups count admitted starts, including virtual and scheduled voices.
        const AudioVoiceStats&  GetVoiceStats() const;
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
        std::optional<float>                     GetBusVolumeDb(const std::string& bus) const;
        bool                                     IsBusPaused(const std::string& bus) const;
        void                                     SetGamePaused(bool paused);
        void                                     SetTimeScale(float scale);
        float                                    GetTimeScale() const;
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
        // One producer per voice. Keep the voice and slot table stable during a background producer's calls.
        // PCM is interleaved. Push returns accepted input frames, and underruns count missing input frames.
        uint32_t PushGeneratorFrames(AudioVoiceHandle handle, const float* pcm, size_t sampleCount);
        uint32_t GetFramesAvailableToPush(AudioVoiceHandle handle) const;
        uint64_t GetGeneratorUnderrunFrames(AudioVoiceHandle handle) const;
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
        bool UnPause(AudioVoiceHandle handle);
        void SeekSeconds(AudioVoiceHandle handle, double seconds);
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
        void                 SetVoiceOcclusion(AudioVoiceHandle, const AudioOcclusionOverrides&);
        AudioOcclusionResult GetVoiceOcclusion(AudioVoiceHandle) const;
        void SetOcclusionDefaults(const AudioOcclusionOverrides&, uint32_t budget, uint32_t staleFrames);
        AudioOcclusionSettings GetOcclusionDefaults() const;
        void                   SetOcclusionQuery(const AudioOcclusionQuery&);
        void                   ClearOcclusionQuery(const void* context);
        void                   SetOcclusionDebugDraw(AudioOcclusionDebugDraw, void* context = nullptr);
        void SetVoiceSpatializer(AudioVoiceHandle handle, std::optional<AudioSpatializer> spatializer, float blend);
        void SetDefaultSpatializer(AudioSpatializer spatializer);
        AudioSpatializer           GetDefaultSpatializer() const;
        bool                       IsHRTFAvailable() const;
        AssetID                    GetActiveHRTF() const;
        std::optional<std::string> SetActiveHRTF(AssetManager* assets, AssetID id);
        // An empty ID restores an event's authored asset, or the voice's built-in settings.
        void SetVoiceAttenuationAsset(AudioVoiceHandle handle, AssetManager* assets, AssetID id);
        void SetListener(uint32_t index, const AudioListenerState& state);
        // Replaces the active listener set. Slots beyond the span are disabled.
        void     SetListeners(const AudioListenerState* states, size_t count);
        uint32_t GetMaxListeners() const;
        // Reports missing targets once per engine and never falls back to Master.
        std::optional<size_t> ResolveReverbBus(const std::string& name);
        void SetVoiceRouting(AudioVoiceHandle handle, bool reverbZoneSend, float lfeSendDb, bool pinToCenter);
        void SetVoiceReverbPosition(AudioVoiceHandle handle, glm::vec3 position);

    private:
        friend class Scene;
        void SetVoiceEmitter(AudioVoiceHandle handle, const AudioSpatialParams& emitter, bool followPoint);
        friend class AudioMusicPlayer;
        friend class AudioEventPlayer;
        void
        ProfileRejection(AudioVoiceReason reason, AudioVoiceOwner owner, AssetID clip = {}, AudioSoundID sound = 0);
        void             DefineEventGroup(AssetID id, const AudioConcurrencyGroup& group);
        uint32_t         CountEventInstances(AssetID id) const;
        bool             PlayVoice(AudioVoiceHandle handle, bool* rejectedByLimit);
        void             RefreshBusPolicy(AudioVoiceHandle handle);
        void             RefreshBusPolicies();
        void             ApplyVoicePitch(AudioVoiceHandle handle, bool ramp);
        AudioVoiceHandle PlayRegisteredOneShot(AudioClipRegistry&                           registry,
                                               const std::shared_ptr<const AudioClipAsset>& clip,
                                               AudioVoiceDesc&                              desc,
                                               const AudioSpatialParams*                    spatial,
                                               double                                       startOffsetSeconds,
                                               bool*                                        rejectedByLimit,
                                               AssetID                                      eventGroup       = {},
                                               AssetManager*                                assets           = nullptr,
                                               AssetID                                      eventAttenuation = {},
                                               AssetID                                      attenuationOverride = {});
        AudioVoiceHandle
        CreateVoice(const AudioVoiceDesc& desc, uint64_t loopStart, uint64_t loopEnd, bool oneShot = false);
        uint64_t                   StartVoice(AudioVoiceHandle        handle,
                                              uint64_t                clockFrame,
                                              uint64_t                fadeFrames,
                                              AudioFadeCurve          curve,
                                              const AudioVoiceWindow* window = nullptr);
        std::optional<std::string> RebuildOutput(std::string_view id, SpeakerMode mode, AudioDeviceChangeReason reason);
        std::optional<std::string> ReformatMix(const AudioOutputFormat& format);
        void                       UpdateOutput();
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
