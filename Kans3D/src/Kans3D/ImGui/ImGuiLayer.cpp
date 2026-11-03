#include "kspch.h"
#include "ImGuiLayer.h"
#include "EditorFonts.h"

#include <imgui.h>

#include "Kans3D/Core/Application.h"

#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
// Temp

namespace Kans
{
    ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {}

    ImGuiLayer::~ImGuiLayer() {}

    void ImGuiLayer::OnAttach()
    {
        PROFILE_FUCTION();

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // Enable Docking
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows
        // io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
        // io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;

        LoadEditorFonts();

        EditorTheme::Apply();

        m_Renderer = RenderHardware::CreateImGuiRenderer(RenderHardware::GetBackend(), &Application::Get().GetWindow());
    }

    void ImGuiLayer::OnDetach()
    {
        PROFILE_FUCTION();

        m_Renderer.reset();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::Begin()
    {
        PROFILE_FUCTION();

        m_Renderer->NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::End()
    {
        PROFILE_FUCTION();

        ImGuiIO&     io  = ImGui::GetIO();
        Application& app = Application::Get();
        io.DisplaySize   = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

        // Rendering
        ImGui::Render();
        m_Renderer->Render(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            m_Renderer->RenderPlatformWindows();
        }
        m_Renderer->EndFrame();
    }

    void ImGuiLayer::OnImGuiRender() {}

    void ImGuiLayer::OnEvent(Event& e)
    {

        if (m_BlokEvents)
        {

            ImGuiIO& io = ImGui::GetIO();
            e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
            e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
        }
    }

} // namespace Kans
