#pragma once
#include <stdexcept>
#include <type_traits>
#include <entt.hpp>

#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"

namespace Kans
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene);
        Entity(const Entity& other) = default;

        // 为实现调用组件构造函数而不是直接传入构造好的组件，使用forward将参数转发
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            CORE_ASSERT(!HasComponent<T>(), "Entity Already Have component ");

            if constexpr (std::is_same_v<T, MeshComponent> || std::is_same_v<T, EditableMeshComponent>)
            {
                using Other =
                    std::conditional_t<std::is_same_v<T, MeshComponent>, EditableMeshComponent, MeshComponent>;
                CORE_ASSERT(!HasComponent<Other>(), "Mesh components are mutually exclusive");
                if (HasComponent<Other>())
                    throw std::logic_error("Mesh components are mutually exclusive");
            }
            T& component = m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
            m_Scene->OnComponentAdd<T>(*this, component);
            return component;
        }

        // All edits of existing components go through PatchComponent/SetComponent.
        // References returned by GetComponent are for reads; Debug validation can
        // detect forgotten notifications without imposing a Release ECS scan.
        template<typename T, typename F>
        void PatchComponent(F&& edit)
        {
            if constexpr (std::is_same_v<T, TransformComponent> || std::is_same_v<T, MeshComponent> ||
                          std::is_same_v<T, UIScreenComponent> || std::is_same_v<T, UIWorldComponent> ||
                          std::is_same_v<T, TextComponent> || std::is_same_v<T, EditableMeshComponent> ||
                          std::is_same_v<T, SpriteRendererComponent> || std::is_same_v<T, DirLightComponent> ||
                          std::is_same_v<T, PointLightComponent> || std::is_same_v<T, SpotLightComponent> ||
                          std::is_same_v<T, AreaLightComponent>)
            {
                auto&   component = GetComponent<T>();
                const T before    = component;
                edit(component);
                if (!SameRenderValue(before, component))
                    m_Scene->m_Registry.patch<T>(m_EntityHandle);
            }
            else
                m_Scene->m_Registry.patch<T>(m_EntityHandle, std::forward<F>(edit));
        }
        template<typename T>
        void SetComponent(const T& value)
        {
            PatchComponent<T>([&](T& component) { component = value; });
        }

        template<typename T>
        T& GetComponent()
        {
            CORE_ASSERT(HasComponent<T>(), "Entity don't Have component ");
            return m_Scene->m_Registry.get<T>(m_EntityHandle);
        }
        template<typename T>
        const T& GetComponent() const
        {
            CORE_ASSERT(HasComponent<T>(), "Entity don't Have component ");
            return m_Scene->m_Registry.get<T>(m_EntityHandle);
        }
        template<typename... T>
        bool HasComponent()
        {
            return m_Scene->m_Registry.all_of<T...>(m_EntityHandle);
        }
        template<typename... T>
        bool HasComponent() const
        {
            return m_Scene->m_Registry.all_of<T...>(m_EntityHandle);
        }
        template<typename T>
        void RemoveComponent()
        {
            CORE_ASSERT(HasComponent<T>(), "Entity don't Have component ");
            // remove方法只有在组件存在是会进行擦除，若组件不存在，则不会执行
            m_Scene->m_Registry.remove<T>(m_EntityHandle);
        }
        UUID GetUUID() const { return GetComponent<IDComponent>().ID; }

        operator bool() const { return (m_EntityHandle != entt::null) && m_Scene; }

        operator uint32_t() const { return (uint32_t)m_EntityHandle; }
        operator entt::entity() const { return m_EntityHandle; }

        bool operator==(const Entity& other) const { return other.m_EntityHandle == this->m_EntityHandle; }
        bool operator!=(const Entity& other) const { return !(*this == other); }

    private:
        static bool SameRenderValue(const TransformComponent& a, const TransformComponent& b)
        {
            return a.GetTransform() == b.GetTransform();
        }
        static bool SameRenderValue(const UIScreenComponent& a, const UIScreenComponent& b) { return a == b; }
        static bool SameRenderValue(const UIWorldComponent& a, const UIWorldComponent& b) { return a == b; }
        static bool SameRenderValue(const TextComponent& a, const TextComponent& b) { return a == b; }
        static bool SameRenderValue(const MeshComponent& a, const MeshComponent& b) { return a == b; }
        static bool SameRenderValue(const EditableMeshComponent& a, const EditableMeshComponent& b) { return a == b; }
        static bool SameRenderValue(const SpriteRendererComponent& a, const SpriteRendererComponent& b)
        {
            return a == b;
        }
        static bool SameRenderValue(const PointLightComponent& a, const PointLightComponent& b) { return a == b; }
        static bool SameRenderValue(const DirLightComponent& a, const DirLightComponent& b) { return a == b; }
        static bool SameRenderValue(const SpotLightComponent& a, const SpotLightComponent& b) { return a == b; }
        static bool SameRenderValue(const AreaLightComponent& a, const AreaLightComponent& b) { return a == b; }
        friend class Scene;
        friend struct SceneClipboardAccess;
        entt::entity m_EntityHandle {entt::null};
        Scene*       m_Scene = nullptr;
    };
} // namespace Kans
