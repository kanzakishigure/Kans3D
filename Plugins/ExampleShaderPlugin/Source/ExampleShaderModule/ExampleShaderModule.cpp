#include "Kans3D/Plugin/PluginModule.h"
namespace
{
    class ExampleFeature final : public Kans::RenderFeature
    {
    public:
        bool RunsAt(Kans::RenderExtensionPoint point) const override
        {
            return point == Kans::RenderExtensionPoint::BeforeOpaque;
        }
        void AddPasses(Kans::RenderExtensionPoint, const Kans::RenderFeatureContext& context) override
        {
            context.Graph.AddPass("ExampleShaderPlugin", {}, {}, true);
        }
    };
} // namespace
KANS_PLUGIN_MODULE(ExampleShaderModule)
{
    context.RegisterRenderFeature("ExampleShaderPlugin", [] { return std::make_unique<ExampleFeature>(); });
    context.OnStartup([] {});
    context.OnShutdown([] {});
}
