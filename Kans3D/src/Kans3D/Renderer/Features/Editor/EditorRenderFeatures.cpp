#include "kspch.h"
#include "EditorRenderFeatures.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Resource/EditorGridPass.h"
#include "TransformGizmoFeature.h"

namespace Kans
{
    void RegisterDefaultEditorFeatures(RenderFeatureSet& features)
    {
        features.Register("EditorGrid", std::make_unique<EditorGridFeature>());
        features.Register("DebugGeometry", std::make_unique<DebugGeometryFeature>());
        features.Register("EditorSelection", std::make_unique<EditorSelectionFeature>());
        features.Register("EditorPreview", std::make_unique<EditorPreviewFeature>());
        // Last, so the handles draw over every other overlay.
        features.Register("TransformGizmo", std::make_unique<TransformGizmoFeature>());
    }
    void EditorGridFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (!context.View.Grid.Enabled || context.View.Grids.empty())
            return;
        auto&       renderer = *context.Frame;
        const auto& view     = context.View;
        context.Graph.AddPass("Grid", context.Target, [&renderer, &view](auto& pass) {
            for (const auto& grid : view.Grids)
                EditorGridPass::Record(
                    renderer.Services, pass.Commands, view, grid, renderer.SceneTarget(RenderHardware::LoadOp::Load));
        });
    }
    void DebugGeometryFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (context.View.Geometry.empty())
            return;
        auto&       renderer = *context.Frame;
        const auto& view     = context.View;
        auto        uses     = context.Target;
        if (renderer.Samples == 1 && view.GetPostProcessSettings().FilterSize >= 1)
        {
            const auto hdr = context.Graph.ImportTexture("SceneHDR", renderer.Output.GetHDR());
            if (!renderer.PostProcessHDR)
            {
                const auto display = context.Graph.ImportTexture("Debug display input", renderer.Output.GetColors()[0]);
                context.Graph.AddPass("Debug geometry HDR prefill",
                                      {{display, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
                                      [&renderer, &view](auto& pass) {
                                          renderer.Services.GetToneMapping().Record(
                                              pass.Device,
                                              pass.Commands,
                                              renderer.Output.GetColors()[0],
                                              {renderer.Output.GetHDR()},
                                              view.GetPostProcessSettings().Exposure,
                                              true,
                                              view.GetPostProcessSettings().ToneMapping);
                                      });
            }
            renderer.PostProcessHDR = true;
            uses.push_back({hdr, GraphAccess::ColorReadWrite});
        }
        context.Graph.AddPass("Legacy geometry", std::move(uses), [&renderer, &view](auto& pass) {
            for (const auto& draw : view.Geometry)
                renderer.DrawModelGeometry(draw, pass.Commands);
        });
    }
    void EditorOutlineFeature::AddOutline(const RenderFeatureContext&  context,
                                          std::string                  name,
                                          bool                         preview,
                                          glm::vec4                    color,
                                          std::optional<SlotHighlight> highlight)
    {
        using namespace RenderHardware;
        auto&       graph    = context.Graph;
        const auto& view     = context.View;
        auto&       renderer = *context.Frame;
        const auto& target   = context.Target;
        const auto  depth    = target.back().Texture;
        TextureDesc desc;
        desc.Width       = view.Viewport.x;
        desc.Height      = view.Viewport.y;
        desc.PixelFormat = Format::R8Unorm;
        desc.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::RenderTarget;
        auto mask        = graph.CreateTexture(name + " mask", desc);
        std::vector<GraphUse> maskUses {{mask, GraphAccess::ColorWrite}};
        if (highlight)
            maskUses.push_back({depth, GraphAccess::DepthReadWrite});
        graph.AddPass(
            name + " mask", std::move(maskUses), [&renderer, &view, preview, mask, depth, highlight](auto& context) {
                // Resolve at consumption, preserving phase one's cache replacement safety.
                std::vector<MeshDraw> draws;
                if (preview)
                    renderer.AppendPrefabPreviewDraws(*view.Preview, draws);
                else if (highlight)
                {
                    renderer.AppendEntityDraws(highlight->Entity, 0, draws);
                    if (highlight->Slot)
                        draws.erase(std::remove_if(draws.begin(),
                                                   draws.end(),
                                                   [&](const auto& draw) {
                                                       return draw.Mesh->Sections[draw.Section].MaterialSlot !=
                                                              *highlight->Slot;
                                                   }),
                                    draws.end());
                }
                else
                {
                    for (auto entity : view.Highlights.Selected)
                        renderer.AppendEntityDraws(entity, 0, draws);
                }
                renderer.Services.GetOutlineService().RecordMask(
                    context.Commands,
                    context.Texture(mask),
                    draws,
                    view.Scene.sceneCamera.camera.GetProjectionMatrix() * view.Scene.sceneCamera.viewMatrix,
                    view.Viewport,
                    highlight ? context.Texture(depth) : Handle<RenderHardware::Texture>());
            });
        graph.AddPass(name + " composite",
                      {{target[0].Texture, GraphAccess::ColorReadWrite},
                       {depth, GraphAccess::DepthReadWrite},
                       {mask, GraphAccess::SampledRead}},
                      [&renderer, &view, mask, color, highlight](auto& context) {
                          renderer.Services.GetOutlineService().RecordComposite(context.Commands,
                                                                                renderer.SceneTarget(LoadOp::Load),
                                                                                context.Texture(mask),
                                                                                view.Viewport,
                                                                                color,
                                                                                2.0f,
                                                                                highlight.has_value());
                      });
    }

    void EditorSelectionFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (!context.View.Highlights.Selected.empty())
            AddOutline(context, "Selection", false, {1, .62f, .15f, 1});
        size_t index = 0;
        for (const auto& highlight : context.View.Highlights.Highlights)
            AddOutline(context, "Slot " + std::to_string(index++), false, highlight.Color, highlight);
    }
    void EditorPreviewFeature::Prepare(RenderView& view, RenderFrame& renderer)
    {
        if (!view.Preview || !view.Highlights.Isolate.empty())
            return;
        // Keep the ghost in the original shared, sorted material batch, always lit.
        renderer.SubmitPrefabPreview(*view.Preview);
    }
    void EditorPreviewFeature::AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        if (context.View.Preview && context.View.Highlights.Isolate.empty())
            AddOutline(context, "Preview", true, {.35f, .7f, 1, 1});
    }
} // namespace Kans
