#pragma once
#include <memory>
#include "Graph/RenderGraph.h"
#include "RenderView.h"

namespace Kans
{
    struct RenderFrame;
    enum class RenderExtensionPoint
    {
        Immediate, // At each original immediate submission, before that submission's core draw.
        BeforeOpaque,
        AfterOpaque, // TAA exposes the opaque/transparency boundary before its HDR snapshot.
        AfterTransparent,
        BeforePostProcess,
        Overlay
    };
    struct RenderFeatureContext
    {
        RenderGraph&                 Graph;
        const RenderView&            View;
        const std::vector<GraphUse>& Target;
        // Optional for CPU-only features; always supplied by a scene pipeline.
        RenderFrame*         Frame     = nullptr;
        const ViewImmediate* Immediate = nullptr;
    };
    class RenderFeature
    {
    public:
        virtual ~RenderFeature() = default;
        // Runs once before draw-list preparation. May contribute draws to shared core batches.
        virtual void Prepare(RenderView& view, RenderFrame& frame) {}
        virtual bool RunsAt(RenderExtensionPoint point) const                                   = 0;
        virtual void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) = 0;
    };
    // Owned by one viewport/view configuration. Registration order determines order at each point.
    // Registration/toggling is between frames; disabled features receive no callbacks.
    class RenderFeatureSet
    {
    public:
        void Register(std::string name, std::unique_ptr<RenderFeature> feature, bool enabled = true);
        void SetEnabled(const std::string& name, bool enabled);
        bool IsEnabled(const std::string& name) const;
        static void ReportPipelineFailure(const std::string& message);
        void Prepare(RenderView& view, RenderFrame& frame);
        void Inject(RenderExtensionPoint point, const RenderFeatureContext& context);

    private:
        struct Entry
        {
            std::string                    Name;
            std::unique_ptr<RenderFeature> Feature;
            bool                           Enabled;
            bool                           FailedFrame = false;
        };
        std::vector<Entry> m_Entries;
    };
} // namespace Kans
