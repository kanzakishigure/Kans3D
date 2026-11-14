#pragma once
#include "Kans3D/Renderer/RenderFeature.h"

namespace Kans
{
    class EditorGridFeature final : public RenderFeature
    {
    public:
        bool RunsAt(RenderExtensionPoint point) const override { return point == RenderExtensionPoint::Overlay; }
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
    };
    class DebugGeometryFeature final : public RenderFeature
    {
    public:
        bool RunsAt(RenderExtensionPoint point) const override
        {
            return point == RenderExtensionPoint::AfterTransparent || point == RenderExtensionPoint::BeforePostProcess;
        }
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
    };
    class EditorOutlineFeature : public RenderFeature
    {
    public:
        bool RunsAt(RenderExtensionPoint point) const override { return point == RenderExtensionPoint::Overlay; }

    protected:
        static void AddOutline(const RenderFeatureContext&  context,
                               std::string                  name,
                               bool                         preview,
                               glm::vec4                    color,
                               std::optional<SlotHighlight> highlight = {});
    };
    class EditorSelectionFeature final : public EditorOutlineFeature
    {
    public:
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
    };
    class EditorPreviewFeature final : public EditorOutlineFeature
    {
    public:
        void Prepare(RenderView& view, RenderFrame& frame) override;
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
    };
    // Registers the editor's overlays (grid, debug shells, selection and drag-preview
    // outlines, transform handles); called by each editor viewport for its own feature set.
    void RegisterDefaultEditorFeatures(RenderFeatureSet& features);
} // namespace Kans
