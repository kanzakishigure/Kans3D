#pragma once
#include <functional>
#include <memory>
#include "Kans3D/Asset/Importer/ImporterRegistry.h"
#include "Kans3D/Renderer/RenderFeature.h"
#include "Kans3D/Scene/ComponentRegistry.h"
#include "PluginManifest.h"

namespace Kans
{
    struct PluginEditorPanel
    {
        std::string           Name;
        std::function<void()> Draw;
    };
    struct PluginMenuItem
    {
        std::string           Name;
        std::function<void()> Execute;
    };
    using PluginTimelineProperty = std::function<bool(Scene&, Entity, glm::vec4&, bool)>;
    class IPluginContext
    {
    public:
        virtual ~IPluginContext()                                                                   = default;
        virtual void RegisterComponent(ComponentEntry entry)                                        = 0;
        virtual void RegisterRenderFeature(std::string                                     name,
                                           std::function<std::unique_ptr<RenderFeature>()> factory) = 0;
        virtual void RegisterAssetImporter(RegisteredImporter importer)                             = 0;
        virtual void RegisterEditorPanel(PluginEditorPanel panel)                                   = 0;
        virtual void RegisterMenuItem(PluginMenuItem item)                                          = 0;
        virtual void RegisterTimelineProperty(std::string path, PluginTimelineProperty property)    = 0;
        // Register during the explicit module entry; startup executes after registration.
        virtual void OnStartup(std::function<void()> callback)  = 0;
        virtual void OnShutdown(std::function<void()> callback) = 0;
    };
    using PluginModuleEntry = void (*)(IPluginContext&);
    struct PluginModuleRegistration
    {
        std::string           Plugin, Name;
        PluginType            Type;
        PluginModuleEntry     Register = nullptr;
        std::filesystem::path Directory;
        std::string           Version;
    };
    // Generated CMake translation units reference every entry explicitly, retaining static archives.
    std::vector<PluginModuleRegistration> CompiledRuntimePluginModules();
    std::vector<PluginModuleRegistration> CompiledEditorPluginModules();

    class PluginHost final : public IPluginContext
    {
    public:
        ~PluginHost();
        std::optional<AssetError>
        Start(const PluginResolution& resolution, const std::vector<PluginModuleRegistration>& modules, bool editor);
        void                                       Shutdown() noexcept;
        void                                       InstallRenderFeatures(RenderFeatureSet& features) const;
        std::variant<ImporterRegistry, AssetError> CreateImporters() const;
        const std::vector<PluginEditorPanel>&      Panels() const { return m_Panels; }
        const std::vector<PluginMenuItem>&         MenuItems() const { return m_Menus; }
        void                                       RegisterComponent(ComponentEntry entry) override;
        void RegisterRenderFeature(std::string name, std::function<std::unique_ptr<RenderFeature>()> factory) override;
        void RegisterAssetImporter(RegisteredImporter importer) override;
        void RegisterEditorPanel(PluginEditorPanel panel) override;
        void RegisterMenuItem(PluginMenuItem item) override;
        void RegisterTimelineProperty(std::string path, PluginTimelineProperty property) override;
        void OnStartup(std::function<void()> callback) override;
        void OnShutdown(std::function<void()> callback) override;

    private:
        struct ModuleState
        {
            std::string                        Name;
            PluginType                         Type;
            std::vector<std::function<void()>> Startup, Shutdown;
        };
        std::vector<ModuleState> m_Modules;
        std::vector<std::string> m_Components, m_TimelinePaths;
        std::vector<std::pair<std::string, std::function<std::unique_ptr<RenderFeature>()>>> m_Features;
        std::vector<RegisteredImporter>                                                      m_Importers;
        std::vector<PluginEditorPanel>                                                       m_Panels;
        std::vector<PluginMenuItem>                                                          m_Menus;
        ModuleState&                                                                         Current();
    };
    bool PluginTimelineHasComponent(Entity entity, const std::string& component);
    bool ApplyPluginTimelineProperty(Scene&, Entity, const std::string&, glm::vec4&, bool);
} // namespace Kans

#define KANS_PLUGIN_MODULE(Name) void KansPluginModule_##Name(::Kans::IPluginContext& context)
