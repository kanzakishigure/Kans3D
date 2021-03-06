#pragma once
#include "Hazel/Events/Event.h"
#include "Hazel/Core/Base.h"

///本文件是对window类的抽象描述
///函数均为纯虚函数，且无任何依赖
namespace Hazel
{
	//窗口的抽象化属性
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
		//使用function容器存储void（Event&）的函数
		using EventCallbackFn = std::function<void(Event&)>;
		virtual ~Window() {}
		virtual void OnUpdate() = 0;
		virtual unsigned int GetWidth() const = 0;
		virtual unsigned int GetHeight() const = 0;
		//window 结构
		virtual void SetEventCallback(const EventCallbackFn& callback)=0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
		virtual inline void* GetNativeWindow() const = 0;
		//这些部分需要在实际的平台上实现，这里应该将依赖隔离，只提供接口
		static Scope<Window> Create(const WindowProps& props = WindowProps());
		//使用结构体构造函数对结构体引用赋值
	};
}