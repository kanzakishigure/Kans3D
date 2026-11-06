#include "kspch.h"
#include <cmath>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Physics/PhysicsWorld.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "Entity.h"
#include "Scene.h"
namespace Kans
{
    const PhysicsSettings& Scene::GetPhysicsSettings() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->GetSettings() : KansFileSystem::GetProjectPhysicsSettings();
    }
    DebugDraw& Scene::GetDebugDraw() const { return m_RenderWorld->Debug; }
    double     Scene::GetPhysicsInterpolationAlpha() const
    {
        return m_PhysicsAccumulator / GetPhysicsSettings().FixedTimeStep;
    }
    void Scene::OnPhysicsPrefabChanged(entt::registry&, entt::entity handle)
    {
        if (!m_SuppressNotifications && !m_PhysicsWritingTransforms && !m_ReconcilingPrefab &&
            m_Registry.all_of<PrefabOwnedComponent>(handle))
            m_CapturePrefabNodes.insert(handle);
    }
    void Scene::OnPhysicsJointRemoved(entt::registry&, entt::entity handle)
    {
        if (m_PhysicsWorld && m_Registry.all_of<IDComponent>(handle))
            m_PhysicsWorld->NotifyJointRemoved(m_Registry.get<IDComponent>(handle).ID);
    }
    void Scene::OnPhysicsComponentChanged(entt::registry& registry, entt::entity handle)
    {
        if (m_SuppressNotifications || m_PhysicsWritingTransforms)
            return;
        if (m_PhysicsWorld)
            m_PhysicsWorld->NotifyTopologyChanged({handle, this});
        OnPhysicsPrefabChanged(registry, handle);
    }
    void Scene::OnPhysicsHierarchyChanged(entt::registry& registry, entt::entity handle)
    {
        if (m_SuppressNotifications || m_PhysicsWritingTransforms)
            return;
        if (m_PhysicsWorld)
            m_PhysicsWorld->NotifyTopologyChanged({handle, this}, true);
        OnPhysicsPrefabChanged(registry, handle);
    }
    void Scene::OnPhysicsBodyOwnershipChanged(entt::registry& registry, entt::entity handle)
    {
        if (m_SuppressNotifications || m_PhysicsWritingTransforms)
            return;
        if (m_PhysicsWorld)
            m_PhysicsWorld->NotifyBodyOwnershipChanged({handle, this});
        OnPhysicsPrefabChanged(registry, handle);
    }
    void Scene::OnPhysicsMeshChanged(entt::registry& registry, entt::entity handle)
    {
        if (m_SuppressNotifications || m_PhysicsWritingTransforms)
            return;
        if (m_PhysicsWorld)
            m_PhysicsWorld->NotifyMeshSourceChanged({handle, this});
        OnPhysicsPrefabChanged(registry, handle);
    }
    void Scene::OnPhysicsTransformChanged(entt::registry&, entt::entity handle)
    {
        if (!m_SuppressNotifications && !m_PhysicsWritingTransforms && m_PhysicsWorld)
            m_PhysicsWorld->NotifyTransformChanged({handle, this});
    }
    void Scene::DispatchFixedUpdate(double dt) { ScriptEngine::OnFixedUpdate(*this, float(dt)); }
    void Scene::OnRuntimeSingleStep()
    {
        if (!m_PhysicsWorld)
            return;
        m_PhysicsSingleStep = true;
        OnUpdate(TimeStep(float(GetPhysicsSettings().FixedTimeStep)));
        m_PhysicsSingleStep = false;
    }
    void Scene::UpdatePhysics(double seconds)
    {
        if (!m_PhysicsWorld)
            return;
        GetDebugDraw().BeginFrame();
        m_PhysicsWorld->SynchronizeChanges();
        const auto&  settings = GetPhysicsSettings();
        const double step     = settings.FixedTimeStep;
        if (m_PhysicsSingleStep)
        {
            DispatchFixedUpdate(step);
            m_PhysicsWorld->Step(step);
            m_PhysicsWorld->WriteTransforms(1);
        }
        else if (!m_PhysicsPaused)
        {
            if (std::isfinite(seconds) && seconds > 0)
                m_PhysicsAccumulator += seconds;
            uint32_t count = 0;
            // Render dt enters as float. Bound rounding at the step boundary without
            // changing the duration passed to PhysX or accumulating an extra step.
            const double epsilon = step * 1e-5;
            while (m_PhysicsAccumulator + epsilon >= step && count < settings.MaxSubStepsPerFrame)
            {
                DispatchFixedUpdate(step);
                m_PhysicsWorld->Step(step);
                m_PhysicsAccumulator = std::max(0., m_PhysicsAccumulator - step);
                ++count;
            }
            if (m_PhysicsAccumulator + epsilon >= step)
            {
                const auto dropped   = std::floor((m_PhysicsAccumulator + epsilon) / step) * step;
                m_PhysicsAccumulator = std::max(0., m_PhysicsAccumulator - dropped);
                m_PhysicsWorld->CountDroppedTime(dropped);
            }
            m_PhysicsWorld->WriteTransforms(float(GetPhysicsInterpolationAlpha()));
        }
        m_PhysicsWorld->Draw(GetDebugDraw(), settings.DebugDraw | m_PhysicsDebugView);
    }
} // namespace Kans
