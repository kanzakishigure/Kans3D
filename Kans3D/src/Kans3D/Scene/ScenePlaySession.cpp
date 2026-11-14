#include "kspch.h"
#include "ScenePlaySession.h"
#include "Kans3D/Core/Debug/ErrorBoundary.h"
#include "Kans3D/Core/Debug/FatalError.h"
#include "Kans3D/Physics/PhysicsSettings.h"
#include "Kans3D/Script/ScriptInput.h"
#include "Scene.h"
#include "SceneSerializer.h"

namespace Kans
{
    void ScenePlaySession::Play(Scene& scene) { Start(scene, false); }

    void ScenePlaySession::Simulate(Scene& scene) { Start(scene, true); }

    void ScenePlaySession::Start(Scene& scene, bool simulate)
    {
        if (IsActive())
        {
            if (m_Scene == &scene && m_State == State::Paused)
            {
                m_StepRequested = false;
                m_State = State::Playing;
                scene.OnRuntimeResume();
            }
            return;
        }

        m_Snapshot = scene.Capture();
        m_UpdateError.clear();
        m_Scene = &scene;
        m_PlayTime = 0.0f;
        m_FrameCount = 0;
        m_StepRequested = false;
        m_State = State::Playing;
        m_Simulating    = simulate;
        m_LoadedScene.clear();
        m_LoadError.clear();
        m_SceneLoads = 0;
        m_LastPointer.reset();
        ScriptInput::Reset();
        scene.UI().Reset();
        SceneRuntimeOptions options;
        options.RunScripts = !simulate;
        scene.OnRuntimeStart(options);
    }

    void ScenePlaySession::Pause()
    {
        if (m_State == State::Playing)
        {
            m_State = State::Paused;
            m_Scene->OnRuntimePause();
        }
    }

    void ScenePlaySession::Step()
    {
        if (m_State == State::Paused)
            m_StepRequested = true;
    }

    void ScenePlaySession::Stop(Scene& scene)
    {
        if (!IsActive() || m_Scene != &scene)
            return;
        scene.OnRuntimeStop();
        scene.Restore(*m_Snapshot);
        Reset();
    }

    void ScenePlaySession::Discard(Scene& scene)
    {
        if (!IsActive() || m_Scene != &scene)
            return;
        scene.OnRuntimeStop();
        Reset();
    }

    bool ScenePlaySession::Update(Scene& scene, TimeStep ts)
    {
        ErrorBoundary error;
        bool          updated = false;
        if (!error.Try("Play session update", [&] { updated = UpdateSession(scene, ts); }))
        {
            m_UpdateError = error.Message();
            try
            {
                Stop(scene);
            }
            catch (...)
            {
                FatalError::ReportException(std::current_exception(), "Restoring edit scene after Play failure");
            }
        }
        return updated;
    }

    bool ScenePlaySession::UpdateSession(Scene& scene, TimeStep ts)
    {
        if (!IsActive() || m_Scene != &scene || (m_State == State::Paused && !m_StepRequested))
            return false;

        const TimeStep step = m_State == State::Paused ? TimeStep(float(scene.GetPhysicsSettings().FixedTimeStep)) : ts;
        m_StepRequested = false;
        scene.UpdateUI(m_UIInput, m_UIViewport, step.GetSeconds());
        FeedScriptMouse();
        m_UIInput.NextFrame();
        if (m_State == State::Paused)
            scene.OnRuntimeSingleStep();
        else
            scene.OnUpdate(step);
        // The frame has ended: a scene a script asked for replaces the running one now.
        LoadRequestedScene(scene);
        m_PlayTime += step.GetSeconds();
        ++m_FrameCount;
        return true;
    }

    void ScenePlaySession::FeedScriptMouse()
    {
        ScriptMouseFrame mouse;
        mouse.Position = m_UIViewport.Map(m_UIInput.Pointer);
        mouse.Delta    = m_LastPointer ? mouse.Position - *m_LastPointer : glm::vec2(0);
        mouse.Scroll   = m_UIInput.Scroll;
        mouse.Buttons  = m_UIInput.Buttons;
        m_LastPointer  = mouse.Position;
        ScriptInput::SetMouse(mouse);
    }

    void ScenePlaySession::LoadRequestedScene(Scene& scene)
    {
        auto request = ScriptInput::TakeSceneLoad();
        if (!request || m_Simulating)
            return;
        const auto file      = (m_ProjectRoot / std::filesystem::u8path(*request)).lexically_normal();
        auto       candidate = SceneSerializer::DeserializeChecked(file);
        if (const auto* error = std::get_if<AssetError>(&candidate))
        {
            m_LoadError = "SceneManager.Load(" + *request + "): " + error->Message;
            CORE_ERROR("{}", m_LoadError);
            return;
        }
        const auto loaded = std::get<Ref<Scene>>(candidate)->Capture();
        scene.OnRuntimeStop();
        scene.Restore(loaded);
        scene.UI().Reset();
        SceneRuntimeOptions options;
        options.RunScripts = true;
        scene.OnRuntimeStart(options);
        m_LoadedScene = *request;
        m_LoadError.clear();
        ++m_SceneLoads;
    }

    void ScenePlaySession::Reset()
    {
        if (m_Scene)
            m_Scene->UI().Reset();
        m_UIInput = {};
        m_LoadedScene.clear();
        m_LoadError.clear();
        m_SceneLoads = 0;
        m_LastPointer.reset();
        ScriptInput::Reset();
        m_State = State::Edit;
        m_Scene = nullptr;
        m_Snapshot.reset();
        m_StepRequested = false;
        m_Simulating    = false;
        m_PlayTime = 0.0f;
        m_FrameCount = 0;
    }
} // namespace Kans
