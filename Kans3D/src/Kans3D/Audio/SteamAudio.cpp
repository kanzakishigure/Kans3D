#include "kspch.h"
#include "SteamAudio.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "AudioHRTF.h"
#include "AudioProfileCapture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#if defined(KANS_STEAM_AUDIO)
#include <phonon.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace Kans
{
#if defined(KANS_STEAM_AUDIO)
    namespace
    {
        struct Phonon
        {
#ifdef _WIN32
            HMODULE Library = nullptr;
#else
            void* Library = nullptr;
#endif
            decltype(&iplContextCreate)             CreateContext  = nullptr;
            decltype(&iplContextRelease)            ReleaseContext = nullptr;
            decltype(&iplHRTFCreate)                CreateHRTF     = nullptr;
            decltype(&iplHRTFRelease)               ReleaseHRTF    = nullptr;
            decltype(&iplBinauralEffectCreate)      CreateEffect   = nullptr;
            decltype(&iplBinauralEffectRelease)     ReleaseEffect  = nullptr;
            decltype(&iplBinauralEffectApply)       ApplyEffect    = nullptr;
            decltype(&iplBinauralEffectReset)       ResetEffect    = nullptr;
            decltype(&iplBinauralEffectGetTailSize) GetTailSize    = nullptr;

            decltype(&iplDirectEffectCreate)  CreateDirect  = nullptr;
            decltype(&iplDirectEffectRelease) ReleaseDirect = nullptr;
            decltype(&iplDirectEffectApply)   ApplyDirect   = nullptr;
            decltype(&iplDirectEffectReset)   ResetDirect   = nullptr;

            ~Phonon()
            {
                if (Library)
#ifdef _WIN32
                    FreeLibrary(Library);
#else
                    dlclose(Library);
#endif
            }

            template<class Function>
            bool Bind(Function& function, const char* name)
            {
#ifdef _WIN32
                function = reinterpret_cast<Function>(GetProcAddress(Library, name));
#else
                function = reinterpret_cast<Function>(dlsym(Library, name));
#endif
                return function != nullptr;
            }

            bool Load()
            {
                const auto directory = KansFileSystem::GetExecutableDirectory();
#ifdef _WIN32
                Library = LoadLibraryExW((directory / L"phonon.dll").c_str(),
                                         nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
                Library = dlopen((directory / "libphonon.so").c_str(), RTLD_LOCAL | RTLD_NOW);
#endif
                return Library && Bind(CreateContext, "iplContextCreate") &&
                       Bind(ReleaseContext, "iplContextRelease") && Bind(CreateHRTF, "iplHRTFCreate") &&
                       Bind(ReleaseHRTF, "iplHRTFRelease") && Bind(CreateEffect, "iplBinauralEffectCreate") &&
                       Bind(ReleaseEffect, "iplBinauralEffectRelease") && Bind(ApplyEffect, "iplBinauralEffectApply") &&
                       Bind(ResetEffect, "iplBinauralEffectReset") &&
                       Bind(GetTailSize, "iplBinauralEffectGetTailSize") &&
                       Bind(CreateDirect, "iplDirectEffectCreate") && Bind(ReleaseDirect, "iplDirectEffectRelease") &&
                       Bind(ApplyDirect, "iplDirectEffectApply") && Bind(ResetDirect, "iplDirectEffectReset");
            }
        };

        struct Context
        {
            Phonon           Api;
            IPLContext       Handle = nullptr;
            IPLAudioSettings Settings {};

            ~Context()
            {
                if (Handle)
                    Api.ReleaseContext(&Handle);
            }
        };

        struct HRTF
        {
            std::shared_ptr<Context> Owner;
            IPLHRTF                  Handle = nullptr;

            ~HRTF()
            {
                if (Handle)
                    Owner->Api.ReleaseHRTF(&Handle);
            }
        };

        // Steam Audio documents HRTF creation as non-thread-safe, including concurrent imports.
        std::mutex HRTFCreation;

        struct PhononDiagnostics
        {
            static thread_local PhononDiagnostics* Current;
            PhononDiagnostics*                     Previous = std::exchange(Current, this);
            std::array<char, 512>                  Text {};
            ~PhononDiagnostics() { Current = Previous; }

            static void IPLCALL Capture(IPLLogLevel level, const char* message)
            {
                if (Current && level == IPL_LOGLEVEL_ERROR && message)
                    std::snprintf(Current->Text.data(), Current->Text.size(), "%s", message);
            }
        };
        thread_local PhononDiagnostics* PhononDiagnostics::Current = nullptr;

        std::string Failure(const char* operation, IPLerror result, const PhononDiagnostics& diagnostics)
        {
            const char* reason = "SDK rejected the data or settings";
            if (result == IPL_STATUS_OUTOFMEMORY)
                reason = "out of memory";
            else if (result == IPL_STATUS_INITIALIZATION)
                reason = "SDK initialization failed";
            std::string message =
                std::string(operation) + ": " + reason + " (Steam Audio status " + std::to_string(result) + ")";
            if (diagnostics.Text[0])
                message += ": " + std::string(diagnostics.Text.data());
            return message;
        }
    } // namespace
#endif

    struct SteamAudio::State
    {
#if defined(KANS_STEAM_AUDIO)
        std::shared_ptr<Context> Owner;
        std::shared_ptr<HRTF>    Active, BuiltIn;
#endif
    };

    struct SteamAudioEffect::State
    {
#if defined(KANS_STEAM_AUDIO)
        std::shared_ptr<HRTF> Dataset;
        IPLBinauralEffect     Effect = nullptr;

        ~State()
        {
            if (Effect)
                Dataset->Owner->Api.ReleaseEffect(&Effect);
        }
#endif
    };

    struct SteamAudioDirectEffect::State
    {
#if defined(KANS_STEAM_AUDIO)
        std::shared_ptr<Context> Owner;
        IPLDirectEffect          Effect   = nullptr;
        int                      Channels = 0;

        ~State()
        {
            if (Effect)
                Owner->Api.ReleaseDirect(&Effect);
        }
#endif
    };

    SteamAudioDirectEffect::SteamAudioDirectEffect()  = default;
    SteamAudioDirectEffect::~SteamAudioDirectEffect() = default;

    std::unique_ptr<SteamAudioDirectEffect> SteamAudio::CreateDirectEffect(uint32_t channels) const
    {
#if defined(KANS_STEAM_AUDIO)
        if (!m_State || channels == 0 || channels > 8)
            return {};
        auto effect     = std::unique_ptr<SteamAudioDirectEffect>(new SteamAudioDirectEffect());
        effect->m_State = std::make_unique<SteamAudioDirectEffect::State>();
        auto& state     = *effect->m_State;
        state.Owner     = m_State->Owner;
        state.Channels  = static_cast<int>(channels);
        // The SDK requires a fixed frame size. One sample preserves exact timing
        // for irregular, scheduled and Offline pulls without delaying clear voices.
        IPLAudioSettings        audio {state.Owner->Settings.samplingRate, 1};
        IPLDirectEffectSettings settings {state.Channels};
        if (state.Owner->Api.CreateDirect(state.Owner->Handle, &audio, &settings, &state.Effect) != IPL_STATUS_SUCCESS)
            return {};
        std::array<float, 8> silence {};
        effect->Render(silence.data(), 0.f, {.8f, .6f, .4f});
        effect->Reset();
        return effect;
#else
        (void)channels;
        return {};
#endif
    }

    void SteamAudioDirectEffect::Render(float* frame, float visibility, const std::array<float, 3>& gains)
    {
#if defined(KANS_STEAM_AUDIO)
        auto&                 state = *m_State;
        std::array<float*, 8> rows;
        for (int channel = 0; channel < state.Channels; ++channel)
            rows[channel] = frame + channel;
        IPLAudioBuffer        buffer {state.Channels, 1, rows.data()};
        IPLDirectEffectParams params {};
        params.flags     = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
        params.occlusion = visibility;
        if (visibility < 1 && (gains[0] != visibility || gains[1] != visibility || gains[2] != visibility))
        {
            params.flags            = static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
                                                                        IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
            params.transmissionType = gains[0] == gains[1] && gains[1] == gains[2] ?
                                          IPL_TRANSMISSIONTYPE_FREQINDEPENDENT :
                                          IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            // The SDK evaluates visibility + (1 - visibility) * transmission.
            for (size_t band = 0; band < gains.size(); ++band)
                params.transmission[band] = std::clamp((gains[band] - visibility) / (1 - visibility), 0.f, 1.f);
        }
        state.Owner->Api.ApplyDirect(state.Effect, &params, &buffer, &buffer);
#else
        (void)frame;
        (void)visibility;
        (void)gains;
#endif
    }

    void SteamAudioDirectEffect::Reset()
    {
#if defined(KANS_STEAM_AUDIO)
        m_State->Owner->Api.ResetDirect(m_State->Effect);
#endif
    }

    SteamAudio::SteamAudio()              = default;
    SteamAudio::~SteamAudio()             = default;
    SteamAudioEffect::SteamAudioEffect()  = default;
    SteamAudioEffect::~SteamAudioEffect() = default;

    std::optional<std::string> SteamAudio::Init(uint32_t rate, uint32_t quantum)
    {
#if defined(KANS_STEAM_AUDIO)
        if (const auto* disable = std::getenv("KANS_STEAM_AUDIO_DISABLE"); disable && std::strcmp(disable, "1") == 0)
            return "disabled by KANS_STEAM_AUDIO_DISABLE";
        auto context = std::make_shared<Context>();
        try
        {
            if (!context->Api.Load())
                return "phonon library or required entry points unavailable";
        }
        catch (const std::system_error& error)
        {
            return error.what();
        }
        IPLContextSettings settings {};
        settings.version     = STEAMAUDIO_VERSION;
        settings.simdLevel   = IPL_SIMDLEVEL_AVX512; // SDK selects the best supported instruction set up to this limit.
        settings.logCallback = PhononDiagnostics::Capture;
        {
            PhononDiagnostics diagnostics;
            if (const auto result = context->Api.CreateContext(&settings, &context->Handle);
                result != IPL_STATUS_SUCCESS)
                return Failure("Create Steam Audio context", result, diagnostics);
        }
        uint32_t frames = SteamAudioMaxBlock;
        while (quantum && frames > quantum)
            frames /= 2;
        context->Settings = {static_cast<int>(rate), static_cast<int>(frames)};
        m_State           = std::make_unique<State>();
        m_State->Owner    = std::move(context);
        return Select(nullptr);
#else
        (void)rate;
        (void)quantum;
        return "built without Steam Audio";
#endif
    }

    bool SteamAudio::IsReady() const
    {
#if defined(KANS_STEAM_AUDIO)
        return m_State && m_State->Active;
#else
        return false;
#endif
    }

    std::optional<std::string> SteamAudio::Select(const AudioHRTFAsset* asset)
    {
#if defined(KANS_STEAM_AUDIO)
        if (!m_State)
            return "Steam Audio context unavailable";
        if (!asset && m_State->BuiltIn)
        {
            m_State->Active = m_State->BuiltIn;
            return std::nullopt;
        }
        auto dataset   = std::make_shared<HRTF>();
        dataset->Owner = m_State->Owner;
        IPLHRTFSettings hrtf {};
        hrtf.type   = asset ? IPL_HRTFTYPE_SOFA : IPL_HRTFTYPE_DEFAULT;
        hrtf.volume = 1.f;
        if (asset)
        {
            const auto&             sofa        = asset->GetSOFA();
            constexpr unsigned char signature[] = {0x89, 'H', 'D', 'F', 0x0d, 0x0a, 0x1a, 0x0a};
            if (sofa.size() < sizeof(signature) || sofa.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                std::memcmp(sofa.data(), signature, sizeof(signature)) != 0)
                return "SOFA data has an invalid HDF5 header or size";
            hrtf.sofaData     = reinterpret_cast<const IPLuint8*>(sofa.data());
            hrtf.sofaDataSize = static_cast<int>(sofa.size());
            hrtf.normType     = asset->NormalizeVolume() ? IPL_HRTFNORMTYPE_RMS : IPL_HRTFNORMTYPE_NONE;
        }
        {
            PhononDiagnostics                 diagnostics;
            const std::lock_guard<std::mutex> lock(HRTFCreation);
            const auto                        result = dataset->Owner->Api.CreateHRTF(
                dataset->Owner->Handle, &dataset->Owner->Settings, &hrtf, &dataset->Handle);
            if (result != IPL_STATUS_SUCCESS)
                return Failure(asset ? "Load SOFA HRTF" : "Load built-in HRTF", result, diagnostics) + " at " +
                       std::to_string(dataset->Owner->Settings.samplingRate) + " Hz";
        }
        m_State->Active = dataset;
        if (!asset)
            m_State->BuiltIn = std::move(dataset);
        return std::nullopt;
#else
        (void)asset;
        return "built without Steam Audio";
#endif
    }

    std::optional<std::string> SteamAudio::Validate(const AudioHRTFAsset& asset)
    {
        SteamAudio audio;
        if (auto error = audio.Init(48000, SteamAudioMaxBlock))
            return error;
        return audio.Select(&asset);
    }

    std::unique_ptr<SteamAudioEffect> SteamAudio::CreateEffect() const
    {
#if defined(KANS_STEAM_AUDIO)
        if (!IsReady())
            return {};
        auto effect                       = std::unique_ptr<SteamAudioEffect>(new SteamAudioEffect());
        effect->m_State                   = std::make_unique<SteamAudioEffect::State>();
        auto& state                       = *effect->m_State;
        state.Dataset                     = m_State->Active;
        auto&                     context = *state.Dataset->Owner;
        IPLBinauralEffectSettings settings {state.Dataset->Handle};
        if (context.Api.CreateEffect(context.Handle, &context.Settings, &settings, &state.Effect) != IPL_STATUS_SUCCESS)
            return {};
        // Prepare the convolution before publication, including the SDK's tail length.
        std::array<float, SteamAudioMaxBlock> silence {}, left {}, right {};
        effect->Render(silence.data(), left.data(), right.data(), {0, 0, -1});
        effect->m_TailFrames = std::max(0, context.Api.GetTailSize(state.Effect));
        effect->Reset();
        return effect;
#else
        return {};
#endif
    }

    uint32_t SteamAudioEffect::Frames() const
    {
#if defined(KANS_STEAM_AUDIO)
        return m_State->Dataset->Owner->Settings.frameSize;
#else
        return 0;
#endif
    }

    void SteamAudioEffect::Render(float* input, float* left, float* right, glm::vec3 direction)
    {
#if defined(KANS_STEAM_AUDIO)
        auto&                   dataset   = *m_State->Dataset;
        float*                  inputs[]  = {input};
        float*                  outputs[] = {left, right};
        IPLAudioBuffer          source {1, static_cast<int>(Frames()), inputs};
        IPLAudioBuffer          destination {2, source.numSamples, outputs};
        IPLBinauralEffectParams params {
            {direction.x, direction.y, direction.z}, IPL_HRTFINTERPOLATION_BILINEAR, 1.f, dataset.Handle, nullptr};
        const bool profile = AudioProfileActiveMix && AudioProfileActiveMix->Frame;
        const auto start   = profile ? AudioProfileClock::now() : AudioProfileClock::time_point {};
        dataset.Owner->Api.ApplyEffect(m_State->Effect, &params, &source, &destination);
        if (profile)
            AudioProfileActiveMix->Frame->Callback.SteamAudioNanoseconds +=
                ProfileNanoseconds(AudioProfileClock::now() - start);
#else
        (void)input;
        (void)left;
        (void)right;
        (void)direction;
#endif
    }

    void SteamAudioEffect::Reset()
    {
#if defined(KANS_STEAM_AUDIO)
        m_State->Dataset->Owner->Api.ResetEffect(m_State->Effect);
#endif
    }

    SteamAudioVoice::~SteamAudioVoice() { Clear(); }

    void SteamAudioVoice::Publish(std::unique_ptr<SteamAudioEffect> effect)
    {
        delete m_Pending.exchange(effect.release(), std::memory_order_acq_rel);
        Collect();
    }

    void SteamAudioVoice::Collect()
    {
        auto* effect = m_Retired.exchange(nullptr, std::memory_order_acq_rel);
        while (effect)
        {
            auto* next = effect->m_RetiredNext;
            delete effect;
            effect = next;
        }
    }

    void SteamAudioVoice::Clear()
    {
        Collect();
        Publish(nullptr);
        delete m_Current;
        m_Current = nullptr;
        Reset();
    }

    void SteamAudioVoice::Reset()
    {
        m_Position      = 0;
        m_PreviousBlend = 0;
        m_HasSignal     = false;
        m_Output.fill(0);
        if (m_Current)
            m_Current->Reset();
    }

    uint32_t SteamAudioVoice::DrainFrames() const
    {
        return m_Current ? m_Current->Frames() + m_Current->TailFrames() : 0;
    }

    void SteamAudioVoice::Process(float*       dry,
                                  const float* mono,
                                  uint32_t     frames,
                                  glm::vec3    direction,
                                  float        blend,
                                  bool         reversed)
    {
        if (!m_Current)
            m_Current = m_Pending.exchange(nullptr, std::memory_order_acq_rel);
        if (!m_Current)
            return;
        if (!m_HasSignal)
            m_PreviousBlend = blend;
        const auto block = m_Current->Frames();
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            m_Input[m_Position] = mono ? mono[frame] : 0;
            for (uint32_t channel = 0; channel < 2; ++channel)
                std::swap(dry[frame * 2 + channel], m_Output[m_Position * 2 + channel]);
            if (++m_Position == block)
            {
                ProcessBlock(direction, blend, reversed);
                m_Position = 0;
            }
        }
        m_HasSignal |= mono && frames != 0;
    }

    void SteamAudioVoice::ProcessBlock(glm::vec3 direction, float blend, bool reversed)
    {
        const auto block = m_Current->Frames();
        m_Current->Render(m_Input.data(),
                          reversed ? m_Right.data() : m_Left.data(),
                          reversed ? m_Left.data() : m_Right.data(),
                          direction);
        if (auto* replacement = m_Pending.exchange(nullptr, std::memory_order_acq_rel))
        {
            std::array<float, SteamAudioMaxBlock> left, right;
            replacement->Render(m_Input.data(),
                                reversed ? right.data() : left.data(),
                                reversed ? left.data() : right.data(),
                                direction);
            for (uint32_t i = 0; i < block; ++i)
            {
                const float weight = float(i + 1) / block;
                m_Left[i] += (left[i] - m_Left[i]) * weight;
                m_Right[i] += (right[i] - m_Right[i]) * weight;
            }
            auto* retired          = m_Current;
            m_Current              = replacement;
            retired->m_RetiredNext = m_Retired.load(std::memory_order_relaxed);
            while (!m_Retired.compare_exchange_weak(
                retired->m_RetiredNext, retired, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }
        // The native dry path is buffered alongside the mono input for sample alignment.
        // Steam Audio's own blend uses its neutral input as the dry signal.
        for (uint32_t i = 0; i < block; ++i)
        {
            const float weight = m_PreviousBlend + (blend - m_PreviousBlend) * (float(i + 1) / block);
            m_Output[i * 2] += (m_Left[i] - m_Output[i * 2]) * weight;
            m_Output[i * 2 + 1] += (m_Right[i] - m_Output[i * 2 + 1]) * weight;
        }
        m_PreviousBlend = blend;
    }
} // namespace Kans
