#pragma once
#include "Hazel/Core/Layer.h"
#include "Hazel/Events/KeyEvent.h"
#include "Hazel/Events/MouseEvent.h"
#include "Hazel/Events/ApplicationEvent.h"
namespace Hazel
{
	class  ImGuiLayer : public Layer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer();

		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& event) override;

		void BlockEvents(bool block) { m_BlokEvents = block; }
		void Begin();
		void End();
	public:
		void SetDarkThemeColors();
	private:
		float m_Time = 0.0f;
		bool m_BlokEvents =false;
	};
	
}