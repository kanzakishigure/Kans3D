#include "LauncherLayer.h"
#include <imgui.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/Launcher/EditorProcess.h"

namespace Kans
{
    LauncherLayer::LauncherLayer(std::filesystem::path                          templatesDir,
                                 std::shared_ptr<Launcher::WindowPlacementFlow> placements,
                                 Launcher::EditorLauncher                       launch,
                                 InProcessOpener                                openInProcess)
        : Layer("LauncherLayer"), m_Browser(std::move(templatesDir)), m_Placements(std::move(placements)),
          m_Launch(std::move(launch)), m_OpenInProcess(std::move(openInProcess))
    {
        if (!m_Placements)
            m_Placements = std::make_shared<Launcher::WindowPlacementFlow>();
    }

    void LauncherLayer::OnAttach()
    {
        auto& window = Application::Get().GetWindow();
        window.SetTitle("Kans3D - Projects");
        m_Titlebar.Attach(window);
    }

    void LauncherLayer::OnDetach()
    {
        m_Titlebar.Detach();
        // Closing the Hub keeps where it was; a launch already did.
        if (!m_Launched)
            m_Placements->Remember(Application::Get().GetWindow().GetPlacement());
    }

    void LauncherLayer::OnImGuiRender()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::Begin("##Launcher",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(3);
        if (m_Titlebar.IsCustom())
        {
            auto& window = Application::Get().GetWindow();
            const auto button = DrawLauncherTitlebar(m_Titlebar, window.IsMaximized(), window.GetTitlebarButtonState());
            RunTitlebarButton(button);
        }
        auto chosen = m_Browser.Draw();
        ImGui::End();

        if (chosen && !m_Launched && m_OpenInProcess && EditorProcess::OpensInProcess())
        {
            // A debugger would stay on this process: become the editor here instead of spawning one.
            auto& app = Application::Get();
            m_Placements->Remember(app.GetWindow().GetPlacement());
            m_Launched = true;
            app.SubmitPostFrame([this, project = *chosen] {
                if (auto error = m_OpenInProcess(project))
                {
                    m_Launched = false;
                    m_Browser.SetError(*error);
                    return;
                }
                Application::Get().PopLayer(this); // Deletes this layer: nothing may follow.
            });
        }
        else if (chosen && !m_Launched)
        {
            auto&      app    = Application::Get();
            const auto result = Launcher::LaunchFromHub(*m_Placements, *chosen, app.GetWindow().GetPlacement(), m_Launch);
            if (result.Close)
            {
                m_Launched = true;
                app.Close();
            }
            else
                m_Browser.SetError(result.Error);
        }
    }
} // namespace Kans
