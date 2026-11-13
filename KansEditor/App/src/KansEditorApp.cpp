#include <Kans3D.h>
#include <Kans3D/Core/EntryPoint.h>
#include <Kans3D/Core/FileSystem/FileSystem.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
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

// Without a valid --project the process is the project Hub, which starts an editor process
// for the chosen project and closes; with one it is the editor.
class KansEditor : public Kans::Application
{
public:
    KansEditor(Kans::ApplicationSpecification                          spec,
               const std::filesystem::path&                         editorRoot,
               const std::filesystem::path&                         project,
               const std::filesystem::path&                         request,
               std::shared_ptr<Kans::Launcher::WindowPlacementFlow> placements,
               const Kans::Launcher::WindowSetup&                   setup)
        : Application(spec)
    {
        // The Hub has no layout to keep; the editor layer loads the project's own.
        ImGui::GetIO().IniFilename = nullptr;
        // The window was created at this setup's size: this only places (or maximizes) it.
        Kans::Launcher::ApplyWindowSetup(GetWindow(), setup);
        if (!project.empty())
            PushLayer(new Kans::EditorLayer(project, placements));
        else
        {
            // Under a debugger the Hub becomes the editor in this process, as `--project` would.
            auto openInProcess = [this, placements](const std::filesystem::path& chosen) -> std::optional<std::string> {
                auto selection = Kans::Launcher::ResolveSelection(chosen);
                auto* file     = std::get_if<std::filesystem::path>(&selection);
                if (!file)
                    return "Not a project: " + chosen.u8string();
                Kans::Launcher::ApplyWindowSetup(GetWindow(), placements->Start(true));
                PushLayer(new Kans::EditorLayer(*file, placements));
                return std::nullopt;
            };
            auto* hub = new Kans::LauncherLayer(
                editorRoot / "Templates", placements, &Kans::EditorProcess::LaunchWithProject, std::move(openInProcess));
            PushLayer(hub);
            if (!request.empty())
                hub->Request(request);
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

    // Each process reopens where it was last left: the Hub at the "Launcher" placement, the
    // editor at the "Editor" one, and creates its window at that size.
    auto placements = std::make_shared<Launcher::WindowPlacementFlow>(Launcher::WindowPlacementStore::DefaultStorageFile(),
                                                                      EditorTheme::Scale());
    const auto setup = placements->Start(!project.empty());
    const auto size  = Launcher::InitialWindowSize(setup);

    ApplicationSpecification spec;
    spec.Name         = "Kans3D-Editor";
    spec.ConfigPath   = (absoluteEditorRoot / "KansEditor.ini").string();
    spec.Width        = uint32_t(std::max(size.Width, 1));
    spec.Height       = uint32_t(std::max(size.Height, 1));
    spec.Fullscreen   = false;
    // Windows: the Hub and the editor draw a Kans3D titlebar; elsewhere the native one stays.
    spec.Titlebar     = TitlebarMode::Custom;

    return new KansEditor(spec, absoluteEditorRoot, project, request, placements, setup);
}
