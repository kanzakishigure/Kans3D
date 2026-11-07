#include <Kans3D.h>
#include <Kans3D/Core/EntryPoint.h>
#include <Kans3D/Core/FileSystem/FileSystem.h>

#include <cstdlib>
#include <Kans3D/Project/ProjectFile.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Debug/DebugLayer.h"
#include "EditorLayer.h"
#include "KansEditorCore/Launcher/EditorProcess.h"
#include "Launcher/LauncherLayer.h"
#include "KansEditorCore/Launcher/LauncherModel.h"
#include "KansEditorCore/Launcher/WindowPlacementFlow.h"
#include "Kans3D/ImGui/EditorTheme.h"

class KansEditor : public Kans::Application
{
public:
    KansEditor(Kans::ApplicationSpecification  spec,
               const std::filesystem::path& editorRoot,
               const std::filesystem::path& project,
               const std::filesystem::path& request)
        : Application(spec)
    {
        // The launcher has no layout to keep; the editor layer loads the project's own.
        ImGui::GetIO().IniFilename = nullptr;
        // The launcher and the editor each reopen where they were last left.
        auto placements = std::make_shared<Kans::Launcher::WindowPlacementFlow>(
            Kans::Launcher::WindowPlacementStore::DefaultStorageFile(), Kans::EditorTheme::Scale());
        Kans::Launcher::ApplyWindowSetup(GetWindow(), placements->Start(!project.empty()));
        if (!project.empty())
            PushLayer(new Kans::EditorLayer(project, placements));
        else
        {
            auto* launcher = new Kans::LauncherLayer(editorRoot / "Templates", placements);
            PushLayer(launcher);
            if (!request.empty())
                launcher->Request(request);
        }
        PushLayer(new Kans::DebugLayer());
    }
    ~KansEditor() {}
};
Kans::Application* Kans::createApplication(int argc, char** argv)
{
    const auto               utf8 = EditorProcess::Arguments(argc, argv);
    std::vector<const char*> pointers;
    for (const auto& argument : utf8)
        pointers.push_back(argument.c_str());
    const auto executableDir = KansFileSystem::GetExecutableDirectory();
    const auto currentDir = std::filesystem::current_path();
    const auto editorRoot = KansFileSystem::FindEditorRoot(executableDir, currentDir);
    if (!editorRoot)
    {
        CORE_ERROR("Editor root not found: required Resources/ and KansEditor.ini. Searched '{}' and '{}' and its parents.",
                   executableDir.string(), currentDir.string());
        std::exit(EXIT_FAILURE);
    }
    const auto arguments = Launcher::ParseLaunchArguments(int(utf8.size()), pointers.data());
    if (!arguments.Error.empty())
    {
        CORE_ERROR("{}", arguments.Error);
        std::exit(EXIT_FAILURE);
    }
    // A valid project opens straight away; anything else goes to the launcher,
    // which explains the problem or offers to create the missing project file.
    std::filesystem::path project, request = arguments.Project;
    if (!request.empty())
    {
        auto selection = Launcher::ResolveSelection(request);
        if (auto* file = std::get_if<std::filesystem::path>(&selection))
            project = *file;
    }
    const auto absoluteEditorRoot = std::filesystem::absolute(*editorRoot);
    std::filesystem::current_path(absoluteEditorRoot);

    ApplicationSpecification spec;
    spec.Name         = "Kans3D-Editor";
    spec.ConfigPath   = (absoluteEditorRoot / "KansEditor.ini").string();
    // Created near the size it starts at (the saved placement is applied right after).
    spec.Height       = project.empty() ? 760 : 900;
    spec.Width        = project.empty() ? 1100 : 1600;
    spec.Fullscreen   = false;
    // Windows: the editor draws a Kans3D titlebar; elsewhere the native one stays.
    spec.Titlebar     = TitlebarMode::Custom;

    return new KansEditor(spec, absoluteEditorRoot, project, request);
}
