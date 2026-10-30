#include "kspch.h"
#include "ScenePlaySession.h"
#include "Kans3D/Physics/PhysicsSettings.h"
#include "Scene.h"

namespace Kans
{
    void ScenePlaySession::Play(Scene& scene)
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
        m_Scene = &scene;
        m_PlayTime = 0.0f;
        m_FrameCount = 0;
        m_StepRequested = false;
        m_State = State::Playing;
        scene.UI().Reset();
        scene.OnRuntimeStart();
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
        if (!IsActive() || m_Scene != &scene || (m_State == State::Paused && !m_StepRequested))
            return false;

        const TimeStep step = m_State == State::Paused ? TimeStep(float(scene.GetPhysicsSettings().FixedTimeStep)) : ts;
        m_StepRequested = false;
        scene.UpdateUI(m_UIInput, m_UIViewport, step.GetSeconds());
        m_UIInput.NextFrame();
        if (m_State == State::Paused)
            scene.OnRuntimeSingleStep();
        else
            scene.OnUpdate(step);
        m_PlayTime += step.GetSeconds();
        ++m_FrameCount;
        return true;
    }

    void ScenePlaySession::Reset()
    {
        if (m_Scene)
            m_Scene->UI().Reset();
        m_UIInput = {};
        m_State = State::Edit;
        m_Scene = nullptr;
        m_Snapshot.reset();
        m_StepRequested = false;
        m_PlayTime = 0.0f;
        m_FrameCount = 0;
    }
} // namespace Kans
