#pragma once
#include <unordered_map>
#include "Kans3D/Core/Base/Base.h"
#include "Kans3D/Core/Debug/ErrorBoundary.h"
#include "Kans3D/Core/Events/ApplicationEvent.h"
#include "Kans3D/Core/Events/Event.h"
#include "Kans3D/Core/LayerStack.h"
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/Core/Window.h"

#include "Kans3D/ImGui/ImGuiLayer.h"
namespace Kans
{

    // Returns true for a consumed close request, including one vetoed by the handler.
    inline bool HandleCloseRequest(bool& running, WindowCloseEvent& event, const std::function<bool()>& handler)
    {
        if (!handler || handler())
            running = false;
        event.Handled = true;
        return true;
    }

    struct ApplicationSpecification
    {
        std::string Name = "Kans3D";
        std::string ConfigPath;
        bool        ShowFatalErrorDialog = true; // Disable for headless applications; stderr reporting remains enabled.

        // the Windows Specification
        uint32_t Width        = 1920;
        uint32_t Height       = 1080;
        bool     Fullscreen   = false;
        TitlebarMode Titlebar     = TitlebarMode::Custom;
    };

    class Application
    {
    public:
        Application(const ApplicationSpecification& spec);

        // 保证application在派生类调用时，能调用到派生类的虚析构函数
        virtual ~Application();

        void run();
        void OnEvent(Event& e);
        void PushLayer(Layer* layer);
        void PushOverlay(Layer* overlay);
        // Detaches and deletes the layer. Call from SubmitPostFrame, never while layers are iterated.
        void PopLayer(Layer* layer);
        // Runs `task` after the current frame, outside layer iteration (e.g. to swap layers).
        void SubmitPostFrame(std::function<void()> task) { m_PostFrame.push_back(std::move(task)); }
        void Close() { m_Running = false; }
        void SetCloseRequestHandler(std::function<bool()> handler) { m_CloseRequestHandler = std::move(handler); }

        // 返回的是application的单例，所以不应该将指针返回
        inline static Application& Get() { return *s_Instance; }
        inline Window&             GetWindow() { return *m_Window; }

        const ApplicationSpecification& GetSpecification() const { return m_Specification; }

        float    GetTime() const;
        TimeStep GetTimestep() const { return m_TimeStep; }
        TimeStep GetFrametime() const { return m_Frametime; }

    public:
        ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

    private:
        void ProcessEvents();
        bool OnWindowClose(WindowCloseEvent& e);
        bool OnWindowResize(WindowResizeEvent& e);

    private:
        static Application* s_Instance;

        ApplicationSpecification m_Specification;

        Scope<Window> m_Window;
        ImGuiLayer*   m_ImGuiLayer;
        std::string   m_CapturePath;
        uint32_t      m_CaptureAfter = 0, m_CaptureFrame = 0;
        std::function<bool()> m_CloseRequestHandler;
        std::vector<std::function<void()>> m_PostFrame;
        bool          m_Running   = true;
        bool          m_Minimized = false;

        LayerStack m_LayerStack;
        struct LayerFailures
        {
            ErrorBoundary            Update, UI;
            ConsecutiveFrameFailures Consecutive;
            std::exception_ptr       Failure;
        };
        std::unordered_map<Layer*, LayerFailures> m_LayerFailures;

        TimeStep m_TimeStep;
        TimeStep m_Frametime;

        float m_LastFrameTime = 0.0f;
    };

    Application* createApplication(int argc, char** argv); // 该函数只应在应用端实现

} // namespace Kans
