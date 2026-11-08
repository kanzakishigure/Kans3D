#pragma once
#include <functional>
#include <optional>
#include <unordered_set>
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Camera/Camera.h"
#include "Kans3D/Renderer/Resource/RenderAssetCache.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/EnvironmentSettings.h"
#include "Kans3D/UI/UIInstance.h"
#include "GizmoDraw.h"
#include "Graph/GraphCapture.h"
#include "ImmediatePassState.h"
#include "RenderSettings.h"

namespace Kans
{
    class Scene;
    class RenderFeatureSet;
    class RenderWorld;
    struct ShadowResources;
    struct ScenePickHit
    {
        UUID                     EntityID = 0, InstanceRoot = 0;
        std::optional<glm::vec3> Position;
        std::optional<uint32_t>  MaterialSlot, Section;
    };
    struct SlotHighlight
    {
        UUID                    Entity = 0;
        std::optional<uint32_t> Slot; // nullopt matches the whole subtree.
        glm::vec4               Color {1, .62f, .15f, 1};
    };
    struct ViewHighlights
    {
        std::vector<UUID>                        Selected; // Primary first; includes mesh descendants.
        std::vector<SlotHighlight>               Highlights;
        // Draw only these (entity subtree, slot) parts; empty draws everything.
        std::vector<std::pair<UUID, uint32_t>> Isolate;
    };
    struct SceneRenderStats
    {
        uint32_t DrawCalls = 0;
        uint64_t Triangles = 0;
        uint32_t PrepassDrawCalls   = 0;
        uint32_t VelocityDrawCalls  = 0;
        uint64_t PrepassTriangles   = 0;
        uint64_t SimulatedParticles = 0, ParticleDraws = 0;
    };
    struct ShadowStatistics
    {
        uint32_t Passes = 0, DrawCalls = 0, CastersCulled = 0;
        uint32_t                          ShadowedLights = 0;
        std::vector<ShadowPassStatistics> PerPass;
    };
    struct SpriteStatistics
    {
        uint32_t DrawCalls = 0, QuadCount = 0;
        uint32_t GetTotalVertexCount() const { return QuadCount * 4; }
        uint32_t GetTotalIndexCount() const { return QuadCount * 6; }
    };
    struct RenderCamera
    {
        Camera    camera;
        glm::mat4 viewMatrix;
        glm::vec3 Position;
    };
    using SceneRendererCamera = RenderCamera; // Editor facade compatibility.
    struct SceneInfo
    {
        PostProcessSettings     PostProcess;
        DirLight            dirLight;
        PointLight          pointLight;
        RenderCamera            sceneCamera;
        std::vector<PointLight> pointLights;
        bool                    HasDirectionalLight = false;
    };
    enum class ModelStencilMode
    {
        Disabled,
        WriteReference
    };
    enum class ModelGeometryMode
    {
        Default,
        StencilOutline,
        BackFaces
    };
    enum class SceneViewMode
    {
        Lit,
        BaseColor,
        Normals,
        Wireframe,
        ShadowCascades
    };
    struct PrefabPreview
    {
        AssetID   Prefab;
        glm::mat4 Transform {1};
    };
    struct ViewMeshSubmission
    {
        MeshComponent    Mesh;
        glm::mat4        Transform {1};
        UUID             EntityID = 0, InstanceRoot = 0;
        ModelStencilMode Stencil = ModelStencilMode::Disabled;
    };
    struct ViewShaderDraw
    {
        MeshKey          Mesh;
        glm::mat4        Transform;
        AssetID          Material;
        SceneViewMode    ViewMode;
        ModelStencilMode Stencil;
        // Prepared asset metadata only: extraction never resolves or uploads resources.
        Bounds              LocalBounds;
        MaterialRenderState Render;
        bool                Ready = false;
        uint32_t            Section = 0;
        UUID                EntityID = 0, InstanceRoot = 0;
        Bounds              WorldBounds;
        bool                HasWorldBounds = false;
        bool                CastShadows = true, ReceiveShadows = true;
        Ref<const SkinningPose> SkinPose;
        Ref<const InstanceMaterialParameters> Parameters;
        bool                                  RenderCustomDepth = false;
        uint8_t                               CustomStencil     = 0;
    };
    struct ViewGeometryDraw
    {
        glm::mat4             Transform;
        std::string           ShaderName;
        ModelGeometryMode     Mode;
        MeshKey                 Mesh;
        std::optional<uint32_t> Section;
        UUID                    EntityID = 0;
        Ref<const SkinningPose> SkinPose;
    };
    struct ViewTextDraw
    {
        TextComponent                    Style;
        glm::mat4                        Transform {1};
        UUID                             EntityID = 0, InstanceRoot = 0;
        std::shared_ptr<TextLayoutCache> Layout;
        Bounds                           WorldBounds;
    };
    struct ViewReadyDraw
    {
        size_t              DrawIndex;
        MaterialRenderState Render;
        float               DistanceSquared;
        bool                Text = false, AlwaysOnTop = false, UI = false;
    };
    struct ViewSprite
    {
        glm::mat4      Transform;
        Ref<Texture2D> Texture;
        glm::vec4      Color;
        float          TilingFactor = 1;
        glm::vec4      UVRect {0, 0, 1, 1};
        bool           FlipX = false, FlipY = false;
        int            SortingOrder = 0;
        AssetID        TextureAsset;
    };
    struct ViewUI
    {
        std::shared_ptr<UIInstance> Instance;
        glm::mat4                   Transform {1}; // Canvas pixels -> world; screen canvases use pixel projection.
        bool                        Screen = true, AlwaysOnTop = false, DoubleSided = true;
        int                         SortOrder = 0;
        UUID                        EntityID  = 0;
        Bounds                      WorldBounds;
    };
    struct GridSettings
    {
        bool     Enabled    = true;
        float    CellSize   = 1.0f;
        uint32_t MajorEvery = 10;
        // Display-space RGB; alpha is line opacity (scene alpha is preserved).
        glm::vec4 MinorColor {.38f, .38f, .38f, .28f};
        glm::vec4 MajorColor {.55f, .55f, .55f, .42f};
        glm::vec4 XAxisColor {.85f, .16f, .16f, .75f};
        glm::vec4 YAxisColor {.18f, .75f, .25f, .75f};
        glm::vec4 ZAxisColor {.18f, .38f, .90f, .75f};
        float     LineWidth = 1.0f; // Pixels; axes are 1.5 times wider.
        // Plane radius at low camera heights; scales by max(1, height/(10*CellSize)).
        float FadeDistance = 100.0f;
    };
    struct ViewGrid
    {
        enum class Plane
        {
            XZ,
            XY,
            YZ
        };
        Plane     Surface = Plane::XZ;
        glm::vec3 Origin {0}; // Snap offset, also the intersection of the coloured axes.
    };
    struct ViewSkybox
    {
        RenderHardware::Handle<RenderHardware::Texture> Texture;
        ImmediatePassState                              State;
    };
    struct ViewSpriteBatch
    {
        RenderHardware::Handle<RenderHardware::CommandList> Commands;
        RenderHardware::Handle<RenderHardware::Texture>     Texture;
        // Recorded sprites opt into temporal rendering by retaining their unsubmitted projection upload.
        // SpriteBatch::SetBatchSubmissionSink supplies this metadata. Opaque legacy command lists cannot be
        // reprojected.
        RenderHardware::Handle<RenderHardware::Buffer> ProjectionBuffer;
        glm::mat4                                      Projection {1};
    };
    enum class ViewImmediateKind
    {
        Sprites,
        RecordedSprites,
        Skybox
    };
    struct ViewImmediate
    {
        ViewImmediateKind Kind;
        size_t            Index;
        size_t            Count = SIZE_MAX;
    };
    struct ViewEnvironment
    {
        RenderHardware::Handle<RenderHardware::Texture> IrradianceMap, RadianceMap;
        std::filesystem::path Source; // Resolved HDR path, when maps were precomputed by the caller.
    };
    // One view/frame. Owns intent, CPU sort metadata and resource handles, never borrowed meshes.
    // Matrices remain in the engine's coordinate convention until recording.
    struct ViewMaterialResources
    {
        std::shared_ptr<const GpuShader>           Shader;
        std::shared_ptr<const GpuMaterialBindings> Bindings;
    };
    struct RenderView
    {
        PostProcessSettings GetPostProcessSettings() const
        {
            auto settings = Scene.sceneCamera.camera.ResolvePostProcess(Scene.PostProcess);
            if (Exposure)
                settings.Exposure = *Exposure;
            return settings.Validated();
        }
        SceneInfo                          Scene {};
        GraphCaptureSettings               Debug;
        CullingStatistics                  Culling;
        uint32_t                           BloomMipCount = 0;
        RenderFeatureSet* Features = nullptr; // Borrowed for this call; owned by the viewport/configuration.
        std::shared_ptr<RenderAssetCache>                 Assets;
        std::map<MeshKey, std::shared_ptr<const GpuMesh>> MeshResources;
        std::map<AssetID, ViewMaterialResources>          MaterialResources;
        std::optional<EngineFrameUniforms>                LightingUniforms;
        std::string                                       Pipeline = "Forward";
        std::optional<float>                              Exposure;
        std::optional<Kans::AntiAliasing>                 AntiAliasing; // nullopt inherits the camera.
        bool                                              DepthPrepass           = true;
        bool                                              TemporalJitter = false, Velocity = false, CameraCut = false;
        enum class DebugBuffer
        {
            None,
            Velocity,
            NormalRoughness
        } DebugBuffer                                                            = DebugBuffer::None;
        Kans::AntiAliasing                                EffectiveAA            = Kans::AntiAliasing::None;
        float                                             Time         = 0;
        uint32_t                                          MaxShadowedPointLights = 4;
        std::shared_ptr<ShadowResources>                  Shadows;
        ShadowStatistics                                  ShadowStats;
        bool                                              ExtractWorld = true;
        EnvironmentSettings Environment; // Effective settings; independent when UseWorldEnvironment is false.
        std::optional<std::filesystem::path> EnvironmentPath; // nullopt inherits world; empty disables IBL.
        bool                                 DrawEnvironment     = true;
        bool                                 UseWorldEnvironment = true;
        glm::uvec2                         Viewport {0};
        SceneViewMode                      DebugMode = SceneViewMode::Lit;
        std::vector<ViewMeshSubmission>    Meshes;
        ViewHighlights                       Highlights;
        // Opt-in view intent; never stored in Scene or its persistent render records.
        std::unordered_set<UUID> HiddenEntities;
        // Flattened once per extraction, also used by shadows, VFX and selection overlays.
        std::unordered_set<UUID>             HiddenHierarchy;
        std::vector<ViewShaderDraw>        Draws;
        std::vector<ViewTextDraw>            Texts;
        std::vector<ViewUI>                  UIs;
        std::vector<ViewReadyDraw>         MaterialOrder;
        std::vector<size_t>                Opaque, Mask, Blend; // Draw indices, in their filtered execution order.
        std::vector<ViewGeometryDraw>      Geometry;
        std::vector<ViewSprite>            Sprites;
        GridSettings                         Grid;
        std::vector<ViewGrid>              Grids;
        std::vector<ViewSkybox>            Skyboxes;
        std::vector<ViewSpriteBatch>       SpriteBatches; // Pre-recorded immediate sprite batches.
        std::vector<ViewImmediate>         ImmediateOrder;
        ViewEnvironment                    EnvironmentMaps;
        RenderHardware::Handle<RenderHardware::Texture> BrdfLUT;
        std::optional<PrefabPreview>                    Preview;
        // Editor transform handles, drawn last over the display image (TransformGizmo feature).
        std::vector<GizmoDraw> Gizmo;
    };
    // Scene-camera game view: no editor features, highlights or previews.
    std::optional<RenderView> MakeRuntimeView(Scene& scene, glm::uvec2 viewport);
    // CPU-only extraction of an available mesh; absent data emits no draws.
    void
    ExtractMeshDraws(RenderView& view, const ViewMeshSubmission& submission, SceneViewMode mode, const GpuMesh* mesh);
    // Planes extracted from a device view-projection matrix (Vulkan depth 0..1).
    class Frustum
    {
    public:
        explicit Frustum(const glm::mat4& deviceViewProjection);
        bool Intersects(const Bounds& worldBounds) const;

    private:
        std::array<glm::vec4, 6> m_Planes;
    };
    void ExtractDrawLists(RenderView& view, const Frustum* frustum = nullptr);
    // CPU-only packing shared by rendering and analytic contract tests.
    EngineFrameUniforms MakeLightingUniforms(const SceneInfo& scene);
} // namespace Kans
