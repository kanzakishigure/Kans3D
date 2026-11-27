#include "kspch.h"
#include "Kans3D/Physics/PhysicsWorld.h"

#include <unordered_map>
#include "Kans3D/Audio/AudioSystem.h"
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Debug/ErrorBoundary.h"
#include "Kans3D/Core/Debug/FatalError.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Core/Input/Input.h"
#include "Kans3D/Core/Input/KeyCodes.h"
#include "Kans3D/Core/Log/Log.h"
#include "Kans3D/ImGui/ImGuiRecovery.h"
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
        FatalError::SetShowDialog(spec.ShowFatalErrorDialog);

        //-------------create application Surface--------------------//
        CORE_ASSERT(!s_Instance, "Application already exist!");

        const auto backend = RenderHardware::ResolveStartupBackend(spec.ConfigPath);
        const auto capture = RenderHardware::GetStartupFrameCapture();
        if (!capture.Path.empty())
            m_CapturePath = std::filesystem::absolute(capture.Path).string();
        m_CaptureAfter = capture.AfterFrames;
        KansFileSystem::Init(spec.ConfigPath);

        CORE_INFO("Application [{:>8}]  is Create :", spec.Name);
        KANS_PROFILE_FUNCTION();
        s_Instance = this;

        //-------------create application Surface and init the render context-----------------//
        RenderHardware::SetBackend(backend);

        WindowSpecification windowSpec;
        windowSpec.Title        = spec.Name;
        windowSpec.Height       = spec.Height;
        windowSpec.Width        = spec.Width;
        windowSpec.Fullscreen   = spec.Fullscreen;
        windowSpec.Titlebar     = spec.Titlebar;
        m_Window                = Scope<Window>(Window::Create(windowSpec));
        m_Window->Init();
        m_Window->SetEventCallback([this](Event& e) { OnEvent(e); });

        //-------------Init the Renderer----------------------------//
        Renderer::Init(m_Window);
        AudioEngineConfig audioConfig;
        audioConfig.MaxListeners  = MaxAudioListeners;
        audioConfig.UserSettingsFile = AudioUserSettings::DefaultPath();
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

        m_LayerFailures.clear();
        KansFileSystem::ShutDown();
        AudioSystem::Shutdown();
        PhysicsWorld::Shutdown();
        Renderer::Shutdown();
        ScriptEngine::ShutDown();
        s_Instance = nullptr;
    }
    void Application::run()
    {
        KANS_PROFILE_FUNCTION();
        while (m_Running)
        {
            KANS_PROFILE_SCOPE("Application frame");
#ifndef KS_DIST
            const auto cpuStart    = RenderClockMilliseconds();
            // Sample without holding the device: a device rebuild must be able to release the old one.
            const auto deviceStart = RenderHardware::GetSharedDevice()->GetFrameCounters();
            const auto shaderStart = ProfileShaderCompiles;
            ProfileFixedSteps      = 0;
#endif
            ++m_FrameNumber;
            float time      = GetTime();
            m_Frametime     = TimeStep(float(glfwGetTime() - m_LastFrameTime));
            m_TimeStep      = glm::min<float>(m_Frametime, 0.0333f);
            m_LastFrameTime = time;

            RunApplicationFrame(
                !m_Minimized,
                [&] {
                    for (auto& [layer, state] : m_LayerFailures)
                        state.Failure = {};
                    RenderHardware::BeginWindowFrame(*m_Window);
                    if (!m_CapturePath.empty() && ++m_CaptureFrame >= m_CaptureAfter)
                    {
                        RenderHardware::RequestFrameCapture(*m_Window, m_CapturePath);
                        m_CapturePath.clear();
                    }
                },
                [&] {
                    for (Layer* layer : m_LayerStack)
                    {
                        auto& state = m_LayerFailures[layer];
                        if (!state.Update.Try("Layer " + layer->GetName() + " OnUpdate",
                                              [&] { layer->OnUpdate(m_TimeStep); }))
                            state.Failure = state.Update.Exception();
                    }
                },
                [&] {
                    m_ImGuiLayer->Begin();
                    for (Layer* layer : m_LayerStack)
                    {
                        const ImGuiRecovery recovery;
                        auto&               state = m_LayerFailures[layer];
                        if (!state.UI.Try("Layer " + layer->GetName() + " OnImGuiRender",
                                          [&] { layer->OnImGuiRender(); }))
                        {
                            state.Failure = state.UI.Exception();
                            recovery.Recover();
                        }
                    }
                    // All layer UI scopes have been recovered before native GUI submission.
                    // Backend submission failures propagate to Main: swapchain acquisition or
                    // a lost device cannot be repaired by ImGui stack recovery.
                    m_ImGuiLayer->End();
                },
                [&] {
                    for (Layer* layer : m_LayerStack)
                    {
                        auto& state = m_LayerFailures[layer];
                        if (state.Consecutive.EndFrame(bool(state.Failure)))
                            FatalError::ReportException(
                                state.Failure,
                                ("Layer " + layer->GetName() + " failed on 3 consecutive frames").c_str());
                    }
                    m_Window->SwapBuffers();
                },
                [&] {
                    if (auto* audio = AudioSystem::Get())
                        audio->Update();
                },
                [&] { ProcessEvents(); },
                [&] {
                    auto tasks = std::move(m_PostFrame);
                    m_PostFrame.clear();
                    for (auto& task : tasks)
                        task();
                });
#ifndef KS_DIST
            m_LastFrameProfile = {m_FrameNumber,
                                  RenderClockMilliseconds() - cpuStart,
                                  ProfileFixedSteps,
                                  ProfileShaderCompiles - shaderStart,
                                  RenderHardware::GetSharedDevice()->GetFrameCounters().Since(deviceStart)};
#ifdef TRACY_ENABLE
            if (KANS_PROFILE_CONNECTED())
            {
                KANS_PROFILE_PLOT("Application CPU frame (ms)", m_LastFrameProfile.CpuMilliseconds);
                KANS_PROFILE_PLOT("Physics fixed steps", int64_t(m_LastFrameProfile.FixedSteps));
                KANS_PROFILE_PLOT("Vulkan acquire wait (ms)",
                                  m_LastFrameProfile.DeviceWork.AcquireWaitNanoseconds / 1e6);
                KANS_PROFILE_PLOT("Vulkan present wait (ms)",
                                  m_LastFrameProfile.DeviceWork.PresentWaitNanoseconds / 1e6);
                KANS_PROFILE_PLOT("Vulkan fence wait (ms)", m_LastFrameProfile.DeviceWork.FenceWaitNanoseconds / 1e6);
                KANS_PROFILE_PLOT("ImGui draw calls", int64_t(m_LastFrameProfile.DeviceWork.ImGuiDrawCalls));
                KANS_PROFILE_PLOT("ImGui triangles", int64_t(m_LastFrameProfile.DeviceWork.ImGuiTriangles));
                KANS_PROFILE_PLOT("Application shader stages compiled", int64_t(m_LastFrameProfile.ShaderCompiles));
                KANS_PROFILE_PLOT("Application pipeline compiles",
                                  int64_t(m_LastFrameProfile.DeviceWork.PipelineCompiles));
            }
#endif
#endif
            KANS_PROFILE_FRAME();
        }
    }
    void Application::OnEvent(Event& e)
    {
        KANS_PROFILE_FUNCTION();
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
        KANS_PROFILE_FUNCTION();

        m_LayerStack.PushLayer(layer);
        layer->OnAttach();
    }
    void Application::PopLayer(Layer* layer)
    {
        m_LayerFailures.erase(layer);
        m_LayerStack.PopLayer(layer);
        delete layer;
    }
    void Application::PushOverlay(Layer* overlay)
    {
        KANS_PROFILE_FUNCTION();

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
        KANS_PROFILE_FUNCTION();

        if (e.GetHeight() == 0 || e.GetWidth() == 0)
        {
            m_Minimized = true;
            return false;
        }
        m_Minimized = false;
        return false;
    }

} // namespace Kans
