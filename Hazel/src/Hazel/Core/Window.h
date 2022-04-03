#pragma once
#include "Hazel/Events/Event.h"
#include "Hazel/Core/Base.h"

///���ļ��Ƕ�window��ĳ�������
///������Ϊ���麯���������κ�����
namespace Hazel
{
	//���ڵĳ�������
	struct WindowProps
	{
		std::string Title;
		unsigned int Width;
		unsigned int Height;
		WindowProps(const std::string& title = "Hazel Engine",
			unsigned int width = 1920,
			unsigned int height = 1080)
			:Title(title), Width(width), Height(height){}
	};
	class  Window
	{
	public:
		//ʹ��function�����洢void��Event&���ĺ���
		using EventCallbackFn = std::function<void(Event&)>;
		virtual ~Window() {}
		virtual void OnUpdate() = 0;
		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;
		//window �ṹ
		virtual void SetEventCallback(const EventCallbackFn& callback)=0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
		virtual inline void* GetNativeWindow() const = 0;
		//��Щ������Ҫ��ʵ�ʵ�ƽ̨��ʵ�֣�����Ӧ�ý��������룬ֻ�ṩ�ӿ�
		static Scope<Window> Create(const WindowProps& props = WindowProps());
		//ʹ�ýṹ�幹�캯���Խṹ�����ø�ֵ
	};
}