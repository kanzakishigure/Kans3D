#include "PluginModule.h"
#include <algorithm>
#include <stdexcept>

namespace Kans
{
    namespace
    {
        std::map<std::string, PluginTimelineProperty>& TimelinePaths()
        {
            static std::map<std::string, PluginTimelineProperty> paths;
            return paths;
        }
    } // namespace
    PluginHost::~PluginHost() { Shutdown(); }
    PluginHost::ModuleState& PluginHost::Current()
    {
        if (m_Modules.empty())
            throw std::runtime_error("Plugin registration outside a module");
        return m_Modules.back();
    }
    std::optional<AssetError> PluginHost::Start(const PluginResolution&                      resolution,
                                                const std::vector<PluginModuleRegistration>& modules,
                                                bool                                         editor)
    {
        if (!m_Modules.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Plugin host already started"};
        try
        {
            std::set<std::string> seen;
            for (const auto& name : resolution.Order)
            {
                const auto& plugin = resolution.Plugins.at(name);
                if (!plugin.Enabled || (!editor && plugin.Manifest.Type == PluginType::Editor))
                    continue;
                for (const auto& module : plugin.Manifest.Modules)
                {
                    const auto found = std::find_if(modules.begin(), modules.end(), [&](const auto& m) {
                        return m.Plugin == name && m.Name == module && m.Type == plugin.Manifest.Type;
                    });
                    if (found == modules.end() || !found->Register ||
                        (!found->Directory.empty() && plugin.ProjectOwned && found->Directory != plugin.Directory) ||
                        (!found->Version.empty() && found->Version != plugin.Manifest.Version))
                        throw std::runtime_error("Plugin " + name + " module " + module +
                                                 " is not compiled; rebuild required");
                    if (!seen.insert(module).second)
                        throw std::runtime_error("Duplicate compiled module " + module);
                    m_Modules.push_back({name + "/" + module, plugin.Manifest.Type, {}, {}});
                    found->Register(*this);
                    for (const auto& startup : Current().Startup)
                        startup();
                }
            }
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            const auto message = std::string("Plugin startup: ") + e.what();
            Shutdown();
            return AssetError {AssetErrorCode::InvalidData, message};
        }
    }
    void PluginHost::Shutdown() noexcept
    {
        for (auto module = m_Modules.rbegin(); module != m_Modules.rend(); ++module)
            for (auto callback = module->Shutdown.rbegin(); callback != module->Shutdown.rend(); ++callback)
                try
                {
                    (*callback)();
                }
                catch (...)
                { /* Continue unwinding every module. */
                }
        for (const auto& component : m_Components)
            ComponentRegistry::Runtime().Unregister(component);
        for (const auto& path : m_TimelinePaths)
            TimelinePaths().erase(path);
        m_Modules.clear();
        m_Components.clear();
        m_TimelinePaths.clear();
        m_Features.clear();
        m_Importers.clear();
        m_Panels.clear();
        m_Menus.clear();
    }
    void PluginHost::RegisterComponent(ComponentEntry entry)
    {
        Current();
        if (!entry.Icon)
            entry.Icon = "+";
        const auto name = entry.Name;
        ComponentRegistry::Runtime().RegisterEntry(std::move(entry));
        m_Components.push_back(name);
    }
    void PluginHost::RegisterRenderFeature(std::string name, std::function<std::unique_ptr<RenderFeature>()> factory)
    {
        Current();
        if (name.empty() || !factory)
            throw std::runtime_error("Invalid plugin render feature");
        for (const auto& feature : m_Features)
            if (feature.first == name)
                throw std::runtime_error("Duplicate plugin render feature " + name);
        m_Features.emplace_back(std::move(name), std::move(factory));
    }
    void PluginHost::RegisterAssetImporter(RegisteredImporter importer)
    {
        Current();
        m_Importers.push_back(std::move(importer));
        auto validation = CreateImporters();
        if (const auto* error = std::get_if<AssetError>(&validation))
            throw std::runtime_error(error->Message);
    }
    void PluginHost::RegisterEditorPanel(PluginEditorPanel panel)
    {
        if (Current().Type != PluginType::Editor)
            throw std::runtime_error("Editor panels require an Editor plugin");
        if (panel.Name.empty() || !panel.Draw)
            throw std::runtime_error("Invalid editor panel");
        m_Panels.push_back(std::move(panel));
    }
    void PluginHost::RegisterMenuItem(PluginMenuItem item)
    {
        if (Current().Type != PluginType::Editor)
            throw std::runtime_error("Editor menu items require an Editor plugin");
        if (item.Name.empty() || !item.Execute)
            throw std::runtime_error("Invalid editor menu item");
        m_Menus.push_back(std::move(item));
    }
    void PluginHost::RegisterTimelineProperty(std::string path, PluginTimelineProperty property)
    {
        Current();
        if (path.empty() || !property || TimelinePaths().count(path))
            throw std::runtime_error("Invalid or duplicate Timeline property " + path);
        TimelinePaths().emplace(path, std::move(property));
        m_TimelinePaths.push_back(std::move(path));
    }
    void PluginHost::OnStartup(std::function<void()> callback)
    {
        if (callback)
            Current().Startup.push_back(std::move(callback));
    }
    void PluginHost::OnShutdown(std::function<void()> callback)
    {
        if (callback)
            Current().Shutdown.push_back(std::move(callback));
    }
    void PluginHost::InstallRenderFeatures(RenderFeatureSet& features) const
    {
        for (const auto& [name, factory] : m_Features)
            features.Register(name, factory());
    }
    std::variant<ImporterRegistry, AssetError> PluginHost::CreateImporters() const
    {
        auto registry = CreateDefaultImporterRegistry();
        if (const auto* error = std::get_if<AssetError>(&registry))
            return *error;
        for (const auto& importer : m_Importers)
        {
            // Registry stores const importers; the registration interface promises no mutation.
            auto result = std::get<ImporterRegistry>(registry).Register(
                importer.Name, importer.Extensions, std::const_pointer_cast<AssetImporterBase>(importer.Importer));
            if (const auto* error = std::get_if<AssetError>(&result))
                return *error;
        }
        return registry;
    }
    bool PluginTimelineHasComponent(Entity entity, const std::string& name)
    {
        for (const auto& component : ComponentRegistry::Runtime().Entries())
            if (component.Name == name)
                return component.Has(entity);
        return false;
    }
    bool ApplyPluginTimelineProperty(Scene& scene, Entity entity, const std::string& path, glm::vec4& value, bool write)
    {
        const auto found = TimelinePaths().find(path);
        return found != TimelinePaths().end() && found->second(scene, entity, value, write);
    }
} // namespace Kans
