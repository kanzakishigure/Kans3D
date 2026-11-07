#include "LauncherLayer.h"
#include <imgui.h>
#include "EditorLayer.h"
#include "Kans3D/ImGui/EditorTheme.h"

namespace Kans
{
    LauncherLayer::LauncherLayer(std::filesystem::path templatesDir, std::shared_ptr<Launcher::WindowPlacementFlow> placements)
        : Layer("LauncherLayer"), m_Browser(std::move(templatesDir)), m_Placements(std::move(placements))
    {}

    void LauncherLayer::OnAttach()
    {
        auto& window = Application::Get().GetWindow();
        window.SetTitle("Kans3D - Projects");
        m_Titlebar.Attach(window);
    }

    void LauncherLayer::OnDetach()
    {
        m_Titlebar.Detach();
        // Closing the launcher keeps where it was; handing over already did.
        if (!m_Opening && m_Placements)
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

        if (chosen && !m_Opening)
        {
            // Layers cannot change while the application iterates them.
            m_Opening = true;
            Application::Get().SubmitPostFrame([this, project = *chosen, placements = m_Placements] {
                auto&      app    = Application::Get();
                auto&      window = app.GetWindow();
                const auto setup  = placements ? placements->OpenEditor(window.GetPlacement()) : Launcher::WindowSetup {};
                app.PopLayer(this);
                if (placements)
                    Launcher::ApplyWindowSetup(window, setup);
                app.PushLayer(new EditorLayer(project, placements));
            });
        }
    }
} // namespace Kans
