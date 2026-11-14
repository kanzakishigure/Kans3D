#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
        const std::string& GetLastUpdateError() const { return m_UpdateError; }
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

        // Runtime scene changes (SceneManager.Load). A script asks for a scene by its path relative
        // to the project root; the session switches at the end of that frame, after every script's
        // Update and the rest of the scene update, as Godot defers change_scene and Unity applies
        // LoadScene on the next frame. The switch stops the running scene (OnRuntimeStop: scripts
        // end, physics and audio stop) and starts the loaded one the way Play does, in the same Scene
        // object and the same play session: play mode and the play clock continue, static C# state
        // stays. The edit-time scene is never written: like Unity's play mode, Stop restores the
        // snapshot taken when Play began, whichever scenes were loaded since. A file that cannot be
        // read keeps the current scene running and records LastLoadError.
        void SetProjectRoot(std::filesystem::path root) { m_ProjectRoot = std::move(root); }
        const std::filesystem::path& GetProjectRoot() const { return m_ProjectRoot; }
        // The scene loaded last during this session (project-relative; empty: still the edited one).
        const std::string& GetLoadedScene() const { return m_LoadedScene; }
        uint64_t           GetSceneLoadCount() const { return m_SceneLoads; }
        const std::string& GetLastLoadError() const { return m_LoadError; }

        // Counters reset on a new Play and when returning to Edit.
        float GetPlayTime() const { return m_PlayTime; }
        uint64_t GetFrameCount() const { return m_FrameCount; }
        static constexpr float FixedStep = 1.0f / 60.0f;

    private:
        void Reset();
        bool         UpdateSession(Scene& scene, TimeStep ts);
        void FeedScriptMouse();
        void LoadRequestedScene(Scene& scene);
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
        std::filesystem::path       m_ProjectRoot;
        std::string                 m_LoadedScene, m_LoadError;
        std::string                  m_UpdateError;
        uint64_t                    m_SceneLoads = 0;
        std::optional<glm::vec2>    m_LastPointer; // Game-view pixels, for the script mouse delta.
    };
} // namespace Kans
