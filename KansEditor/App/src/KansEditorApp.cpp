#include <Kans3D.h>
#include <Kans3D/Core/FileSystem/FileSystem.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <Kans3D/Project/ProjectFile.h>
#include <Kans3D/RHI/Vulkan/VulkanDevice.h>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Debug/DebugLayer.h"
#include "EditorLayer.h"
#include "KansEditorCore/Launcher/EditorProcess.h"
#include "KansEditorCore/Launcher/LauncherModel.h"
#include "KansEditorCore/Launcher/WindowPlacementFlow.h"
#include "KansEditorCore/Profiling/BenchmarkRun.h"
#include "Launcher/LauncherLayer.h"

namespace
{
    std::optional<int> RunBenchmarkIfRequested(int argc, char** argv)
    {
        using namespace Kans;
        namespace fs    = std::filesystem;
        namespace RH    = RenderHardware;
        const auto utf8 = EditorProcess::Arguments(argc, argv);
        if (std::none_of(
                utf8.begin() + 1, utf8.end(), [](const auto& arg) { return arg.rfind("--benchmark", 0) == 0; }))
            return std::nullopt;

        struct Cleanup
        {
            ~Cleanup()
            {
                try
                {
                    Renderer::Shutdown();
                }
                catch (const std::exception& error)
                {
                    CORE_ERROR("Benchmark device shutdown: {}", error.what());
                }
                KansFileSystem::ShutDown();
            }
        } cleanup;
        try
        {
            std::vector<const char*> pointers;
            pointers.reserve(utf8.size());
            for (const auto& arg : utf8)
                pointers.push_back(arg.c_str());
            const auto args = Launcher::ParseLaunchArguments(int(pointers.size()), pointers.data());
            if (!args.Error.empty())
                throw std::invalid_argument(args.Error);
            const auto& options     = *args.Benchmark;
            const auto  selection   = Launcher::ResolveSelection(args.Project);
            const auto* projectFile = std::get_if<fs::path>(&selection);
            if (!projectFile)
                throw std::runtime_error("Cannot open project " + args.Project.u8string());
            const auto editorRoot =
                KansFileSystem::FindEditorRoot(KansFileSystem::GetExecutableDirectory(), fs::current_path());
            if (!editorRoot)
                throw std::runtime_error("Editor root not found (Resources and KansEditor.ini are required)");
            const auto configFile = *editorRoot / "KansEditor.ini";
            KansFileSystem::Init(configFile);
            const auto loaded = LoadProjectFile(*projectFile);
            if (const auto* error = std::get_if<AssetError>(&loaded))
                throw std::runtime_error(error->Message);
            const auto config = MakeProjectConfig(projectFile->parent_path(), std::get<ProjectFile>(loaded));
            KansFileSystem::SetProject(config);
            ProjectOpenOptions projectOptions;
            projectOptions.Editor        = true;
            projectOptions.EditorModules = CompiledEditorPluginModules();
            auto opened                  = Project::Open(config, projectOptions);
            if (const auto* error = std::get_if<AssetError>(&opened))
                throw std::runtime_error(error->Message);
            auto project = std::move(std::get<std::unique_ptr<Project>>(opened));

            RH::VulkanDeviceOptions deviceOptions;
            const char*             requireCpu = std::getenv("KANS_VULKAN_REQUIRE_CPU");
            deviceOptions.RequireCpuDevice     = requireCpu && std::string_view(requireCpu) == "1";
            const auto backend                 = RH::ResolveStartupBackend(configFile.string());
            RH::InitializeHeadlessDevice(backend, RH::CreateVulkanDevice({}, deviceOptions));
            const auto device = RH::GetSharedDevice();
            YAML::Node report;
            if (options.Scene == Benchmark::ComplexScene)
                report = Benchmark::RunComplex(options, device);
            else
            {
                auto scene = fs::u8path(options.Scene);
                if (scene.is_relative())
                {
                    const auto projectScene = project->GetConfig().RootDirectory / scene;
                    scene                   = fs::exists(projectScene) ? projectScene : fs::absolute(scene);
                }
                report = Benchmark::RunProjectScene(*project, scene.lexically_normal(), options, device);
            }
            return Benchmark::Finish(options, report);
        }
        catch (const std::exception& error)
        {
            CORE_ERROR("Benchmark failed: {}", error.what());
            return 1;
        }
    }
} // namespace

#define KANS_ENTRY_HOOK RunBenchmarkIfRequested
#include <Kans3D/Core/EntryPoint.h>
#undef KANS_ENTRY_HOOK

// Without a valid --project the process is the project Hub, which starts an editor process
// for the chosen project and closes; with one it is the editor.
class KansEditor : public Kans::Application
{
public:
    KansEditor(Kans::ApplicationSpecification                       spec,
               const std::filesystem::path&                         editorRoot,
               const std::filesystem::path&                         project,
               const std::filesystem::path&                         request,
               std::shared_ptr<Kans::Launcher::WindowPlacementFlow> placements,
               const Kans::Launcher::WindowSetup&                   setup) :
        Application(spec), m_EditorRoot(editorRoot), m_Placements(std::move(placements))
    {
        // The Hub has no layout to keep; the editor layer loads the project's own.
        ImGui::GetIO().IniFilename = nullptr;
        // The window was created at this setup's size: this only places (or maximizes) it.
        Kans::Launcher::ApplyWindowSetup(GetWindow(), setup);
        if (!project.empty())
            PushEditor(project);
        else
            PushHub(request);
        PushLayer(new Kans::DebugLayer());
    }
    ~KansEditor() {}

private:
    void PushEditor(const std::filesystem::path& project)
    {
        PushLayer(new Kans::EditorLayer(project, m_Placements, [this](Kans::Layer* editor) { ReturnToHub(editor); }));
    }

    void PushHub(const std::filesystem::path& request)
    {
        // Under a debugger the Hub becomes the editor in this process, as `--project` would.
        auto openInProcess = [this](const std::filesystem::path& chosen) -> std::optional<std::string> {
            auto  selection = Kans::Launcher::ResolveSelection(chosen);
            auto* file      = std::get_if<std::filesystem::path>(&selection);
            if (!file)
                return "Not a project: " + chosen.u8string();
            Kans::Launcher::ApplyWindowSetup(GetWindow(), m_Placements->Start(true));
            PushEditor(*file);
            return std::nullopt;
        };
        auto* hub = new Kans::LauncherLayer(m_EditorRoot / "Templates",
                                            m_Placements,
                                            &Kans::EditorProcess::LaunchWithProject,
                                            std::move(openInProcess));
        PushLayer(hub);
        if (!request.empty())
            hub->Request(request);
    }

    // A project that failed to open in this process: its editor layer makes way for the Hub.
    // Runs after the frame, as layer changes must.
    void ReturnToHub(Kans::Layer* editor)
    {
        PopLayer(editor);
        // The editor had entered the project folder.
        std::error_code ec;
        std::filesystem::current_path(m_EditorRoot, ec);
        ImGui::GetIO().IniFilename = nullptr;
        Kans::Launcher::ApplyWindowSetup(GetWindow(), m_Placements->Start(false));
        PushHub({});
    }

    std::filesystem::path                                m_EditorRoot;
    std::shared_ptr<Kans::Launcher::WindowPlacementFlow> m_Placements;
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
