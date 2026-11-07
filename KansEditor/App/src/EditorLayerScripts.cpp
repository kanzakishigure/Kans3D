#include "EditorLayer.h"
#include <fstream>
#include <imgui.h>
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Script/ScriptEngine.h"
#include "UI/EditorIcons.h"

namespace Kans
{
    namespace
    {
        constexpr const char* ScriptAssemblyName = "GameScripts";

        std::string MenuText(const char* icon, const char* text) { return std::string(icon) + "  " + text; }

        // References the engine API and its class-registry generator next to this editor build.
        // KansScriptCoreDir can be overridden on the command line (dotnet build -p:KansScriptCoreDir=...).
        std::string ProjectFile(const std::filesystem::path& engineScripts)
        {
            const auto dir = engineScripts.generic_u8string();
            return "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
                   "  <PropertyGroup>\n"
                   "    <TargetFramework>net10.0</TargetFramework>\n"
                   "    <Nullable>enable</Nullable>\n"
                   "    <IsAotCompatible>true</IsAotCompatible>\n"
                   "    <AssemblyName>" +
                   std::string(ScriptAssemblyName) +
                   "</AssemblyName>\n"
                   "    <KansScriptCoreDir Condition=\"'$(KansScriptCoreDir)' == ''\">" +
                   dir +
                   "</KansScriptCoreDir>\n"
                   "  </PropertyGroup>\n"
                   "  <ItemGroup>\n"
                   "    <Reference Include=\"ScriptCore\">\n"
                   "      <HintPath>$(KansScriptCoreDir)/ScriptCore.dll</HintPath>\n"
                   "      <Private>false</Private>\n"
                   "    </Reference>\n"
                   "    <Analyzer Include=\"$(KansScriptCoreDir)/KansScriptCore.Generators.dll\" />\n"
                   "  </ItemGroup>\n"
                   "</Project>\n";
        }

        constexpr const char* ExampleScript = R"(using Kans;

namespace Game
{
    // Attach with a Script component whose class is "Game.Rotator".
    public class Rotator : Entity
    {
        public float Speed = 1.0f;

        protected override void OnUpdate(float ts)
        {
            var transform = GetComponent<TransformComponent>()!;
            var rotation = transform.Rotation;
            rotation.Y += Speed * ts;
            transform.Rotation = rotation;
        }
    }
}
)";
    } // namespace

    std::filesystem::path EditorLayer::ScriptProjectPath() const
    {
        std::error_code ec;
        const auto      root = m_AssetImports ? m_AssetImports->GetConfig().RootDirectory : std::filesystem::current_path(ec);
        return root / "Scripts" / (std::string(ScriptAssemblyName) + ".csproj");
    }

    std::filesystem::path EditorLayer::ScriptAssemblyPath() const
    {
        // SDK default output for a project without a runtime identifier.
        return ScriptProjectPath().parent_path() / "bin" / "Debug" / "net10.0" / (std::string(ScriptAssemblyName) + ".dll");
    }

    void EditorLayer::CreateScriptProject()
    {
        const auto project = ScriptProjectPath();
        std::error_code ec;
        if (std::filesystem::exists(project, ec))
            return;
        std::filesystem::create_directories(project.parent_path(), ec);
        std::ofstream file(project, std::ios::binary);
        file << ProjectFile(KansFileSystem::GetScriptAssemblyPath().parent_path());
        const auto example = project.parent_path() / "Rotator.cs";
        if (!std::filesystem::exists(example, ec))
            std::ofstream(example, std::ios::binary) << ExampleScript;
        if (!file)
        {
            Record(AssetError {AssetErrorCode::IoError, "Cannot write " + project.string()});
            return;
        }
        CORE_INFO("Created script project {}", project.string());
        BuildScripts();
    }

    void EditorLayer::BuildScripts()
    {
        if (m_ScriptBuild.valid() || !ScriptEngine::IsInitialized())
            return;
        m_ScriptStatus = "Building scripts...";
        m_ScriptBuildProgress.Begin(m_Progress);
        m_ScriptBuild =
            std::async(std::launch::async,
                       [this,
                        project       = ScriptProjectPath(),
                        scriptCoreDir = KansFileSystem::GetScriptAssemblyPath().parent_path()] {
                           return ScriptBuild::Build(
                               project, ScriptBuild::Config::Debug, scriptCoreDir, [this](std::string_view line) {
                                   m_ScriptBuildProgress.OutputLine(line);
                               });
                       });
    }

    void EditorLayer::PollScriptBuild()
    {
        m_Progress.Prune();
        m_ScriptBuildProgress.Poll(m_Progress);
        if (!m_ScriptBuild.valid() || m_ScriptBuild.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        const auto result = m_ScriptBuild.get();
        m_ScriptBuildProgress.Finish(m_Progress, result);
        for (const auto& d : result.Diagnostics)
        {
            const auto text = fmt::format("{}({},{}): {} {}: {}", d.File.string(), d.Line, d.Column, d.Severity, d.Code, d.Message);
            if (d.Severity == "error")
                CORE_ERROR("{}", text);
            else
                CORE_WARN("{}", text);
        }
        if (!result.Success)
        {
            if (result.Diagnostics.empty())
                CORE_ERROR("Script build failed:\n{}", result.Output);
            m_ScriptStatus = "Script build failed (see Console)";
            return;
        }
        // Play may have started while building; the new assembly loads after Stop.
        if (m_Play.IsActive())
        {
            m_ScriptStatus = "Built; stop Play and reload to use the new scripts";
            return;
        }
        const bool loaded = m_ScriptsLoaded ? ScriptEngine::ReloadAppAssembly() :
                                              ScriptEngine::LoadAppAssembly(ScriptAssemblyPath());
        m_ScriptsLoaded |= loaded;
        m_ScriptStatus = loaded ? fmt::format("Scripts loaded ({} classes)", ScriptEngine::GetEntityClasses().size()) :
                                  "Script load failed: " + ScriptEngine::GetLastError();
        if (!loaded)
            CORE_ERROR("{}", m_ScriptStatus);
    }

    void EditorLayer::LoadProjectScripts()
    {
        std::error_code ec;
        if (!ScriptEngine::IsInitialized() || !std::filesystem::exists(ScriptAssemblyPath(), ec))
            return;
        m_ScriptsLoaded = ScriptEngine::LoadAppAssembly(ScriptAssemblyPath());
        if (!m_ScriptsLoaded)
            CORE_WARN("Project scripts not loaded: {}", ScriptEngine::GetLastError());
    }

    void EditorLayer::DrawScriptsMenu()
    {
        if (!ImGui::BeginMenu("Scripts"))
            return;
        std::error_code ec;
        const bool      available = ScriptEngine::IsInitialized();
        const bool      exists    = std::filesystem::exists(ScriptProjectPath(), ec);
        const bool      building  = m_ScriptBuild.valid();
        if (ImGui::MenuItem(MenuText(ICON_FA_CODE, "Create Script Project").c_str(), nullptr, false, available && !exists))
            CreateScriptProject();
        if (ImGui::MenuItem(MenuText(ICON_FA_ROTATE_RIGHT, "Build & Reload Scripts").c_str(),
                            nullptr,
                            false,
                            available && exists && !building && !m_Play.IsActive()))
            BuildScripts();
        ImGui::Separator();
        if (!available)
            ImGui::TextDisabled("C# scripting unavailable: %s", ScriptEngine::GetLastError().c_str());
        else if (!m_ScriptStatus.empty())
            ImGui::TextDisabled("%s", m_ScriptStatus.c_str());
        else
            ImGui::TextDisabled("%s", ScriptProjectPath().string().c_str());
        ImGui::EndMenu();
    }
} // namespace Kans
