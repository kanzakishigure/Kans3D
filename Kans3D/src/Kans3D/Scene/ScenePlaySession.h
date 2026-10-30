#pragma once
#include <cstdint>
#include <optional>
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/UI/UIInput.h"
#include "SceneSnapshot.h"

namespace Kans
{
    class Scene;

    // Call Stop or Discard before destroying the scene or this session. While
    // active, operations on a different scene are ignored.
    class ScenePlaySession
    {
    public:
        enum class State { Edit, Playing, Paused };

        ScenePlaySession() = default;
        ScenePlaySession(const ScenePlaySession&) = delete;
        ScenePlaySession& operator=(const ScenePlaySession&) = delete;

        State GetState() const { return m_State; }
        bool IsActive() const { return m_State != State::Edit; }
        void Play(Scene& scene);
        // Like Play, but without scripts: physics (with animation, timelines and audio) runs from
        // the edit state, and Stop restores it. Resumes a paused session of either kind.
        void Simulate(Scene& scene);
        bool IsSimulating() const { return IsActive() && m_Simulating; }
        void Pause();
        void Step();
        void Stop(Scene& scene);
        void Discard(Scene& scene);
        bool Update(Scene& scene, TimeStep ts);
        // Feed once per application frame. Pointer is in application coordinates; viewport maps to pixels.
        void FeedUIInput(UIInputState input, UIViewport viewport)
        {
            m_UIInput    = std::move(input);
            m_UIViewport = viewport;
        }
        bool Update(Scene& scene, TimeStep ts, UIInputState input, UIViewport viewport)
        {
            FeedUIInput(std::move(input), viewport);
            return Update(scene, ts);
        }

        // Counters reset on a new Play and when returning to Edit.
        float GetPlayTime() const { return m_PlayTime; }
        uint64_t GetFrameCount() const { return m_FrameCount; }
        static constexpr float FixedStep = 1.0f / 60.0f;

    private:
        void Reset();
        void         Start(Scene& scene, bool simulate);
        UIInputState m_UIInput;
        UIViewport   m_UIViewport;

        State                       m_State = State::Edit;
        Scene*                      m_Scene = nullptr;
        std::optional<SceneSnapshot> m_Snapshot;
        bool                        m_StepRequested = false;
        bool                         m_Simulating    = false;
        float                       m_PlayTime = 0.0f;
        uint64_t                    m_FrameCount = 0;
    };
} // namespace Kans
