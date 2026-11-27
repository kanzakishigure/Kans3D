#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <glm/vec3.hpp>

namespace Kans
{
    class AudioHRTFAsset;
    // Normal offline pulls are up to 256 frames. On a device use at most its graph
    // quantum. The adapter holds one block, including on partial or scheduled pulls.
    inline constexpr uint32_t SteamAudioMaxBlock = 64;

    class SteamAudioEffect
    {
    public:
        ~SteamAudioEffect();
        void     Render(float* input, float* left, float* right, glm::vec3 direction);
        void     Reset();
        uint32_t Frames() const;
        uint32_t TailFrames() const { return m_TailFrames; }

    private:
        friend class SteamAudio;
        friend class SteamAudioVoice;
        SteamAudioEffect();
        struct State;
        std::unique_ptr<State> m_State;
        SteamAudioEffect*      m_RetiredNext = nullptr;
        uint32_t               m_TailFrames  = 0;
    };

    class SteamAudio
    {
    public:
        SteamAudio();
        ~SteamAudio();
        std::optional<std::string>        Init(uint32_t rate, uint32_t quantum);
        std::optional<std::string>        Select(const AudioHRTFAsset* asset);
        static std::optional<std::string> Validate(const AudioHRTFAsset& asset);
        std::unique_ptr<SteamAudioEffect> CreateEffect() const;
        bool                              IsReady() const;

    private:
        struct State;
        std::unique_ptr<State> m_State;
    };

    class SteamAudioVoice
    {
    public:
        ~SteamAudioVoice();
        void Publish(std::unique_ptr<SteamAudioEffect> effect);
        void Collect();
        // Reset is a mixer operation, or a control-thread operation on a detached node.
        void     Reset();
        bool     HasSignal() const { return m_HasSignal; }
        uint32_t DrainFrames() const;
        void Process(float* dry, const float* mono, uint32_t frames, glm::vec3 direction, float blend, bool reversed);
        void Clear(); // The node must be detached.

    private:
        static_assert(std::atomic<SteamAudioEffect*>::is_always_lock_free);
        void ProcessBlock(glm::vec3 direction, float blend, bool reversed);

        std::atomic<SteamAudioEffect*>            m_Pending {nullptr}, m_Retired {nullptr};
        SteamAudioEffect*                         m_Current       = nullptr;
        uint32_t                                  m_Position      = 0;
        float                                     m_PreviousBlend = 0;
        bool                                      m_HasSignal     = false;
        std::array<float, SteamAudioMaxBlock>     m_Input {}, m_Left {}, m_Right {};
        std::array<float, SteamAudioMaxBlock * 2> m_Output {};
    };
} // namespace Kans
