#pragma once
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>
#include <imgui.h>
#include "Kans3D/Renderer/Graph/GraphCapture.h"
#include "Kans3D/Renderer/Graph/RenderGraph.h"
#include "Kans3D/Renderer/RenderStatistics.h"

namespace Kans
{
    class ViewportClient;

    // The editor view's last frame as a node graph: render passes (left to right in
    // execution order) and the textures they read and write, with the image each pass
    // produced. Selecting a pass and one of its outputs shows that image at full
    // resolution with channel/exposure controls and the stored value under the cursor.
    class FrameGraphPanel
    {
    public:
        void SetClient(ViewportClient* client) { m_Client = client; }
        // What the editor view should capture next frame (nothing while the panel is closed).
        GraphCaptureSettings CaptureSettings() const;
        void                 Draw();

    private:
        // A pass, or one version of a texture: the texture's content after one write
        // (version 0 is what it held before its first write in this frame).
        struct Node
        {
            GraphDebugID ID = 0;
            bool         Pass = false;
            ImVec2       Position, Size; // Graph space (unzoomed).
            uint32_t     Resource = uint32_t(-1);
            uint32_t     Version  = 0;
            GraphDebugID Writer   = 0; // The pass that produced this version (0 for version 0).
            bool         Read     = false; // Some later pass reads this version.
        };
        void Snapshot();
        void Layout();
        void DrawCanvas();
        void DrawInspector();
        void DrawImageInspector(const CompiledGraphPass& pass, const CompiledGraphResource& resource);
        // Display texture of a capture (cached per captured frame and settings); 0 when unavailable.
        uintptr_t                    Display(const CapturedImage& image, const CaptureDisplaySettings& settings);
        CaptureDisplaySettings       ThumbnailSettings(const CompiledGraphResource& resource) const;
        const CompiledGraphPass*     FindPass(GraphDebugID id) const;
        const CompiledGraphResource* FindGraphResource(GraphDebugID id) const;
        const PassStatistics*        FindStatistics(GraphDebugID id) const;
        // Resources a pass writes (in its declared order).
        std::vector<const CompiledGraphResource*> Outputs(const CompiledGraphPass& pass) const;

        ViewportClient* m_Client = nullptr;
        bool            m_Capture = true, m_Freeze = false;

        // The frame shown (kept while frozen).
        std::vector<CompiledGraphPass>            m_Passes;
        std::vector<CompiledGraphResource>        m_Resources;
        FrameStatistics                           m_Stats;
        std::shared_ptr<const CapturedGraphFrame> m_Captures;
        std::vector<Node>                         m_Nodes;
        std::vector<std::pair<GraphDebugID, GraphDebugID>> m_Edges; // From node to node.
        std::vector<char>                                  m_EdgeWrites;

        ImVec2       m_Pan {40, 40};
        float        m_Zoom = 1.0f;
        GraphDebugID m_SelectedPass = 0, m_SelectedResource = 0, m_SelectedVersion = 0;

        CaptureDisplaySettings m_Display;
        uint32_t               m_Mip = 0, m_Face = 0;
        float                  m_DepthFar = 50.0f;

        struct CachedDisplay
        {
            uint64_t                                        Frame = 0;
            RenderHardware::Handle<RenderHardware::Texture> Texture;
        };
        // (pass, resource, full resolution, settings) -> display texture of a captured frame.
        std::map<std::tuple<GraphDebugID, GraphDebugID, bool, std::string>, CachedDisplay> m_DisplayCache;

        std::optional<uint64_t>         m_Probe;
        glm::uvec2                      m_ProbePixel {0};
        std::optional<PixelProbeResult> m_ProbeResult;
    };
} // namespace Kans
