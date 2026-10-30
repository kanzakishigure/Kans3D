#include "kspch.h"
#include "Kans3D/Physics/PhysicsWorld.h"

#include "Kans3D/Audio/AudioSystem.h"
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/Input/Input.h"
#include "Kans3D/Input/KeyCodes.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Script/ScriptEngine.h"

#include <cstdlib>
#include <filesystem>
#include <GLFW/glfw3.h>
namespace Kans
{
#define BIND_EVENT_FN(x) std::bind(&x, this, std::placeholders::_1)
    Application* Application::s_Instance = nullptr;
    // 使用在bind中使用this指针会永久的将函数this指针绑定为第一个参数

    Application::Application(const ApplicationSpecification& spec) : m_Specification(spec)
    {

        //-------------create application Surface--------------------//
        CORE_ASSERT(!s_Instance, "Application already exist!");

        const auto backend = RenderHardware::ResolveStartupBackend(spec.ConfigPath);
        const auto capture = RenderHardware::GetStartupFrameCapture();
        if (!capture.Path.empty())
            m_CapturePath = std::filesystem::absolute(capture.Path).string();
        m_CaptureAfter = capture.AfterFrames;
        KansFileSystem::Init(spec.ConfigPath);

        CORE_INFO("Application [{:>8}]  is Create :", spec.Name);
        PROFILE_FUCTION();
        s_Instance = this;

        //-------------create application Surface and init the render context-----------------//
        RenderHardware::SetBackend(backend);

        WindowSpecification windowSpec;
        windowSpec.Title        = spec.Name;
        windowSpec.Height       = spec.Height;
        windowSpec.Width        = spec.Width;
        windowSpec.Fullscreen   = spec.Fullscreen;
        windowSpec.HideTitlebar = spec.HideTitlebar;
        m_Window                = Scope<Window>(Window::Create(windowSpec));
        m_Window->Init();
        m_Window->SetEventCallback([this](Event& e) { OnEvent(e); });

        //-------------Init the Renderer----------------------------//
        Renderer::Init(m_Window);
        AudioEngineConfig audioConfig;
        const char* nullAudio = std::getenv("KANS_AUDIO_NULL");
        const bool forceSilent = nullAudio && std::string_view(nullAudio) == "1";
        audioConfig.UseNullDevice = forceSilent;
        auto audioError = AudioSystem::Init(audioConfig);
        if (audioError && !forceSilent)
        {
            CORE_WARN("Audio device unavailable: {}. Using silent audio.", *audioError);
            audioConfig.UseNullDevice = true;
            audioError = AudioSystem::Init(audioConfig);
        }
        if (audioError)
            CORE_WARN("Audio initialization failed: {}", *audioError);
        //-------------Init The UI Layer------------------------//
        m_ImGuiLayer = new ImGuiLayer();
        PushOverlay(m_ImGuiLayer);
        //-------------Init the ScriptEengine-----------------------//
        ScriptEngine::Init();
    }
    Application::~Application()
    {

        // we will terminate  all thread here

        m_Window->SetEventCallback([](Event& e) {});

        // clear the resource
        for (Layer* layer : m_LayerStack)
        {
            layer->OnDetach();
            delete layer;
        }

        KansFileSystem::ShutDown();
        AudioSystem::Shutdown();
        PhysicsWorld::Shutdown();
        Renderer::Shutdown();
        ScriptEngine::ShutDown();
    }
    void Application::run()
    {
        PROFILE_FUCTION();
        while (m_Running)
        {
            float time      = GetTime();
            m_Frametime     = TimeStep(float(glfwGetTime() - m_LastFrameTime));
            m_TimeStep      = glm::min<float>(m_Frametime, 0.0333f);
            m_LastFrameTime = time;

            if (!m_Minimized)
            {
                RenderHardware::BeginWindowFrame(*m_Window);
                if (!m_CapturePath.empty() && ++m_CaptureFrame >= m_CaptureAfter)
                {
                    RenderHardware::RequestFrameCapture(*m_Window, m_CapturePath);
                    m_CapturePath.clear();
                }
                {
                    PROFILE_SCOPE("application_layerstark_update");
                    // 普通layer进行渲染
                    for (Layer* layer : m_LayerStack)
                        layer->OnUpdate(m_TimeStep);
                }
                {
                    PROFILE_SCOPE("application_imgui_layerstark_update");
                    // 处理 ImGuiLayer渲染
                    m_ImGuiLayer->Begin();
                    for (Layer* layer : m_LayerStack)
                        layer->OnImGuiRender();
                    m_ImGuiLayer->End();
                }
                m_Window->SwapBuffers();
            }
            if (auto* audio = AudioSystem::Get())
                audio->Update();
            ProcessEvents();
            auto tasks = std::move(m_PostFrame);
            m_PostFrame.clear();
            for (auto& task : tasks)
                task();
        }
    }
    void Application::OnEvent(Event& e)
    {
        PROFILE_FUCTION();
        EventDispatcher dispatcher(e);
        dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(Application::OnWindowClose));
        dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FN(Application::OnWindowResize));
        if (e.Handled)
            return;

        for (auto it = m_LayerStack.begin(); it != m_LayerStack.end(); ++it)
        {

            (*it)->OnEvent(
                e); // the processing order of events depends on render order ,the last layer will handle the event fast
            if (e.Handled)
                break;
        }
    }
    void Application::PushLayer(Layer* layer)
    {
        PROFILE_FUCTION();

        m_LayerStack.PushLayer(layer);
        layer->OnAttach();
    }
    void Application::PopLayer(Layer* layer)
    {
        m_LayerStack.PopLayer(layer);
        delete layer;
    }
    void Application::PushOverlay(Layer* overlay)
    {
        PROFILE_FUCTION();

        m_LayerStack.PushLayer(overlay);
        overlay->OnAttach();
    }

    float Application::GetTime() const { return (float)glfwGetTime(); }

    void Application::ProcessEvents() { m_Window->ProcessEvents(); }

    bool Application::OnWindowClose(WindowCloseEvent& e)
    {
        return HandleCloseRequest(m_Running, e, m_CloseRequestHandler);
    }
    bool Application::OnWindowResize(WindowResizeEvent& e)
    {
        PROFILE_FUCTION();

        if (e.GetHeight() == 0 || e.GetWidth() == 0)
        {
            m_Minimized = true;
            return false;
        }
        m_Minimized = false;
        return false;
    }

} // namespace Kans
