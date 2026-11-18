#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Plugin/PluginManifest.h"

namespace fs = std::filesystem;
std::string Quote(const fs::path& p) { return "[====[" + p.generic_u8string() + "]====]"; }
std::string Cpp(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + "\"";
}
int main(int argc, char** argv)
{
    try
    {
        if (argc != 4)
            throw std::runtime_error("Expected engine root, project file and output directory");
        const auto engine = fs::u8path(argv[1]), projectFile = fs::u8path(argv[2]), output = fs::u8path(argv[3]);
        std::map<std::string, bool> overrides;
        if (!projectFile.empty())
        {
            auto root = YAML::LoadFile(projectFile.u8string());
            if (auto node = root["Plugins"])
            {
                if (!node.IsMap())
                    throw std::runtime_error("Project Plugins must be a mapping");
                for (const auto& pair : node)
                {
                    const auto name = pair.first.as<std::string>();
                    if (!Kans::IsPluginName(name) || !overrides.emplace(name, pair.second.as<bool>()).second)
                        throw std::runtime_error("Invalid or duplicate plugin override " + name);
                }
            }
        }
        else
        {
            auto all = Kans::DiscoverPlugins(engine, {});
            for (const auto& [name, plugin] : all.Plugins)
                overrides[name] = true;
        }
        auto plugins = Kans::DiscoverPlugins(
            engine, projectFile.empty() ? fs::path {} : projectFile.parent_path() / "Plugins", overrides);
        bool failed = false;
        for (const auto& message : plugins.Diagnostics)
            std::cerr << "Plugin warning: " << message << '\n';
        for (const auto& [name, plugin] : plugins.Plugins)
            for (const auto& error : plugin.Errors)
            {
                std::cerr << "Plugin " << name << ": " << error << '\n';
                // Disabling a dependency is an ordinary project configuration, not a configure failure.
                if (error.rfind("Disabled dependency ", 0) != 0)
                    failed = true;
            }
        if (failed)
            return 1;
        fs::create_directories(output);
        std::ofstream     cmake(output / "Plugins.cmake");
        std::ofstream     runtime(output / "RuntimePlugins.cpp"), editor(output / "EditorPlugins.cpp");
        const std::string header = "#include \"Kans3D/Plugin/PluginModule.h\"\n";
        runtime << header;
        editor << header;
        std::string           runtimeEntries, editorEntries;
        std::set<std::string> names;
        for (const auto& name : plugins.Order)
        {
            const auto& p = plugins.Plugins.at(name);
            cmake << "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "
                  << Quote(p.Directory / "kansplugin.yaml") << ")\n";
            for (const auto& module : p.Manifest.Modules)
            {
                if (!names.insert(module).second)
                    throw std::runtime_error("Duplicate module target " + module);
                const auto source = p.Directory / "Source" / module;
                if (!fs::exists(source / "CMakeLists.txt"))
                    throw std::runtime_error("Missing CMakeLists.txt for " + name + "/" + module);
                cmake << "add_subdirectory(" << Quote(source) << " " << Quote(output / "modules" / module) << ")\n"
                      << "if(NOT TARGET " << module
                      << ")\nmessage(FATAL_ERROR \"Plugin module target missing: " << module << "\")\nendif()\n"
                      << "get_target_property(_kans_module_type " << module << " TYPE)\n"
                      << "if(NOT _kans_module_type STREQUAL \"STATIC_LIBRARY\" AND NOT _kans_module_type STREQUAL "
                         "\"OBJECT_LIBRARY\")\n"
                      << "message(FATAL_ERROR \"Plugin module must be STATIC or OBJECT: " << module << "\")\nendif()\n";
                const bool isEditor = p.Manifest.Type == Kans::PluginType::Editor;
                auto&      cpp      = isEditor ? editor : runtime;
                cpp << "extern void KansPluginModule_" << module << "(Kans::IPluginContext&);\n";
                const auto entry = "{" + Cpp(name) + "," + Cpp(module) +
                                   ",Kans::PluginType::" + (isEditor ? "Editor" : "Runtime") + ",&KansPluginModule_" +
                                   module + ",std::filesystem::u8path(" + Cpp(p.Directory.generic_u8string()) + ")," +
                                   Cpp(p.Manifest.Version) + "},\n";
                (isEditor ? editorEntries : runtimeEntries) += entry;
                cmake << "target_link_libraries(" << (isEditor ? "KansEditorPlugins" : "Kans3D") << " PUBLIC " << module
                      << ")\n";
            }
        }
        runtime << "std::vector<Kans::PluginModuleRegistration> Kans::CompiledRuntimePluginModules() { return {\n"
                << runtimeEntries << "}; }\n";
        editor << "std::vector<Kans::PluginModuleRegistration> Kans::CompiledEditorPluginModules() { return {\n"
               << editorEntries << "}; }\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Plugin configure: " << e.what() << '\n';
        return 1;
    }
}
