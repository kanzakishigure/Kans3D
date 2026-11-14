#pragma once
#include "Kans3D/Renderer/Resource/RenderAssetCache.h"
#include "Kans3D/Renderer/Resource/SelectionTools.h"
#include "RenderFeature.h"
#include "RenderWorld.h"
#include "Renderer.h"
#include "ViewTarget.h"

namespace Kans
{
    enum class MaterialPreviewShape
    {
        Sphere,
        Cube,
        Plane
    };
    struct MaterialPreviewOptions
    {
        glm::uvec2           Size {256, 256};
        MaterialPreviewShape Shape = MaterialPreviewShape::Sphere;
        // Object rotation in radians: yaw about +Y, then pitch about +X.
        glm::vec2 Rotation {0.0f};
        float     Exposure = 0.8f;
    };
    struct MaterialPreviewResult
    {
        uintptr_t Texture  = 0;
        bool     Complete = false;
    };
    struct MaterialPreviewImageResult
    {
        RenderHardware::Handle<RenderHardware::Texture> Target;
        bool                                            Complete = false;
    };
    struct MaterialThumbnail
    {
        glm::uvec2 Size {};
        // RGBA8, tightly packed, top row first; alpha is always 255.
        std::vector<uint8_t> Pixels;
    };

    // Render thread only. Owns a dedicated world, target and background feature;
    // independent of any viewport.
    class MaterialPreviewRenderer
    {
    public:
        MaterialPreviewRenderer(std::shared_ptr<Renderer>            renderer,
                                std::shared_ptr<RenderAssetCache>    assets,
                                std::shared_ptr<EnvironmentLighting> environment = {});
        // Rebind project resources without sharing any viewport state.
        void SetResources(std::shared_ptr<RenderAssetCache> assets, std::shared_ptr<EnvironmentLighting> environment)
        {
            m_Assets      = std::move(assets);
            m_Environment = std::move(environment);
        }
        ~MaterialPreviewRenderer();
        MaterialPreviewRenderer(const MaterialPreviewRenderer&)            = delete;
        MaterialPreviewRenderer& operator=(const MaterialPreviewRenderer&) = delete;

        // nullptr selects Standard. Requires a cache lease, including for resolved inputs.
        // Texture belongs to this instance, valid until its next Render/RenderThumbnail or destruction.
        // Complete means every assigned asset and GPU upload was ready.
        // Unassigned samplers use declared fallbacks and are complete; unavailable assigned assets are not.
        MaterialPreviewResult
        Render(const MaterialData& material, const ShaderData* shader, const MaterialPreviewOptions& options = {});
        MaterialPreviewResult Render(AssetID material, const MaterialPreviewOptions& options = {});
        const RenderWorld&    GetWorld() const { return m_World; }
        const ViewTarget&     GetViewTarget() const { return *m_Target; }
        const RenderHardware::Handle<RenderHardware::Texture>& GetHDRTarget() const { return m_Target->GetHDR(); }
        // Offscreen rendering exposes a neutral image, without exporting it to the UI.
        MaterialPreviewImageResult
        RenderImage(const MaterialData& material, const ShaderData* shader, const MaterialPreviewOptions& options = {});
        MaterialPreviewImageResult RenderImage(AssetID material, const MaterialPreviewOptions& options = {});
        // No image while material/shader/assigned textures are missing, loading or failed.
        std::optional<MaterialThumbnail> RenderThumbnail(AssetID material, const MaterialPreviewOptions& options = {});

    private:
        MaterialPreviewImageResult RenderImageUnchecked(const MaterialData&           material,
                                                        const ShaderData*             source,
                                                        const MaterialPreviewOptions& options);
        MaterialPreviewImageResult RenderImageUnchecked(AssetID material, const MaterialPreviewOptions& options);
        MaterialPreviewImageResult                       Draw(const GpuShader&              shader,
                                                              const GpuMaterialBindings&    bindings,
                                                              const MaterialPreviewOptions& options,
                                                              bool                          complete);
        bool                                  Resize(glm::uvec2 size);
        std::shared_ptr<RenderAssetCache>                m_Assets;
        std::shared_ptr<EnvironmentLighting>             m_Environment;
        RenderWorld                                      m_World;
        RenderFeatureSet                                 m_Features;
        glm::uvec2                            m_Size {};
        std::shared_ptr<RenderHardware::Device>          m_Device;
        std::shared_ptr<Renderer>                        m_Renderer;
        std::unique_ptr<ViewTarget>                      m_Target;
        std::array<std::optional<GpuMesh>, 3> m_Shapes;

        std::optional<ShaderData>             m_ShaderSource;
        Scope<GpuShader>                      m_Shader;
        std::shared_ptr<GpuShader>            m_ResolvedShader;
    };
} // namespace Kans
