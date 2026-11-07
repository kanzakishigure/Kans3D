#include "LauncherLayer.h"
#include <imgui.h>
#include "EditorLayer.h"

namespace Kans
{
    LauncherLayer::LauncherLayer(std::filesystem::path templatesDir)
        : Layer("LauncherLayer"), m_Browser(std::move(templatesDir))
    {}

    void LauncherLayer::OnAttach() { Application::Get().GetWindow().SetTitle("Kans3D - Projects"); }

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
        auto chosen = m_Browser.Draw();
        ImGui::End();

        if (chosen && !m_Opening)
        {
            // Layers cannot change while the application iterates them.
            m_Opening = true;
            Application::Get().SubmitPostFrame([this, project = *chosen] {
                auto& app = Application::Get();
                app.PopLayer(this);
                app.PushLayer(new EditorLayer(project));
            });
        }
    }
} // namespace Kans
