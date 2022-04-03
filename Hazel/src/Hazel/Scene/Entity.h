#pragma once
#include "Hazel/Scene/Scene.h"
#include <entt.hpp>

namespace Hazel
{
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene);
		Entity(const Entity& other) = default;


		//Ϊʵ�ֵ���������캯��������ֱ�Ӵ��빹��õ������ʹ��forward������ת��
		template<typename T, typename ... Args>
		T& AddComponent(Args&&... args)
		{
			HZ_ASSERT(!HasComponent<T>(), "Entity Already Have component ");
			return m_Scene->Reg().emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T>
		T& GetComponent()
		{
			HZ_ASSERT(HasComponent<T>(), "Entity don't Have component ");
			return m_Scene->Reg().get<T>(m_EntityHandle);
		}

		template<typename T>
		bool HasComponent()
		{
			return m_Scene->Reg().all_of<T>(m_EntityHandle);
		}

		template<typename T>
			void RemoveComponent()
		{
			HZ_ASSERT(HasComponent<T>(), "Entity don't Have component ");
			//remove����ֻ������������ǻ���в���������������ڣ��򲻻�ִ��
			m_Scene->Reg().remove<T>(m_EntityHandle);
		}
		

		operator bool() const { return (m_EntityHandle != entt::null) && m_Scene; }

	private:
		entt::entity m_EntityHandle{entt::null};
		Scene* m_Scene = nullptr;
	};
}