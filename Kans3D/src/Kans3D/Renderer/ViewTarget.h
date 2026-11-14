#pragma once
#include <glm/glm.hpp>
#include "Kans3D/Core/Debug/ErrorBoundary.h"
#include "Kans3D/Renderer/Passes/AntiAliasingPass.h"
#include "Graph/RenderGraph.h"
#include "RenderFeature.h"
namespace Kans
{
    // Per-viewport allocations. Resources and pool release before the device lease.
    struct ShadowResources;
    struct TemporalState;
    class Fsr3Upscaler;
    class ViewTarget
    {
    public:
        // Covers view construction as well as graph compilation/recording. Retry on every call.
        template<class F>
        bool TryRender(const std::string& name, F&& render)
        {
            if (m_Error.Try(name, std::forward<F>(render)))
                return true;
            RecoverFailedFrame();
            return false;
        }
        const ErrorBoundary& GetError() const { return m_Error; }
        explicit ViewTarget(std::shared_ptr<RenderHardware::Device> device,
                            uint32_t                                outputs      = 2,
                            bool                                    sampledDepth = true);
        ViewTarget(const ViewTarget&)                                                                    = delete;
        ViewTarget&                                                         operator=(const ViewTarget&) = delete;
        void                                                                Resize(glm::uvec2 size);
        void                                                                EnsureSampledDepth();
        void                                                                EnsureNormalRoughness();
        void                                                                EnsureVelocity();
        // previousUV - currentUV, unjittered [0,1] UV units, origin top-left; FSR scale (1,1).
        RenderHardware::Handle<RenderHardware::Texture> GetVelocity() const
        {
            return m_VelocityValid ? m_SceneVelocity : nullptr;
        }
        glm::vec2      GetJitter() const { return m_Jitter; }
        void           SetJitter(glm::vec2 jitter) { m_Jitter = jitter; }
        TemporalState& GetTemporalState();
        void           InvalidateTemporalHistory();
        Fsr3Upscaler&                        EnsureFsr3Upscaler();
        const std::shared_ptr<Fsr3Upscaler>& GetFsr3Upscaler() const { return m_Fsr3Upscaler; }
        void                                 ReleaseFsr3Upscaler() { m_Fsr3Upscaler.reset(); }
        uint64_t       GetFrameNumber() const { return m_FrameNumber; }
        // RG: world normal octahedron in [0,1]; B: perceptual roughness;
        // A: 0 background, 1 valid normal, 2 reconstruct normal from depth.
        // Null for frames without a prepass; the allocation is retained until Resize.
        RenderHardware::Handle<RenderHardware::Texture> GetNormalRoughness() const
        {
            return m_NormalRoughnessValid ? m_NormalRoughness : nullptr;
        }
        bool     IsNormalRoughnessValid() const { return m_NormalRoughnessValid; }
        uint64_t NextFrameNumber()
        {
            m_NormalRoughnessValid = false;
            m_VelocityValid        = false;
            m_Jitter               = {};
            return ++m_FrameNumber;
        }
        FrameProfiler&                GetFrameProfiler() { return m_Profiler; }
        std::shared_ptr<GraphCapture> GetGraphCapture(); // Lazy; only allocated on request.
        const std::shared_ptr<RenderHardware::Device>&                      GetDevice() const { return m_Device; }
        glm::uvec2                                                          GetSize() const { return m_Size; }
        const std::vector<RenderHardware::Handle<RenderHardware::Texture>>& GetColors() const { return m_Colors; }
        const RenderHardware::Handle<RenderHardware::Texture>&              GetHDR() const { return m_HDR; }
        const RenderHardware::Handle<RenderHardware::Texture>&              GetDepth() const { return m_Depth; }
        std::shared_ptr<ShadowResources>&                                   GetShadowStorage() { return m_Shadows; }
        RenderFeatureSet& GetDefaultFeatures() { return m_DefaultFeatures; }
        ReconstructionFilter& GetReconstructionFilter() { return m_ReconstructionFilter; }
        TransientTexturePool&                                               GetTransientPool() { return m_Pool; }
        const TransientTexturePool&                                         GetTransientPool() const { return m_Pool; }
        RenderHardware::RenderingDesc Target(RenderHardware::LoadOp load, bool hdr = false) const;
        // Compatibility: output indices 2 and 3 select depth with two colors.
        uintptr_t GetOutput(uint32_t index) const;

    private:
        void                                                         RecoverFailedFrame();
        ErrorBoundary                                                m_Error;
        std::shared_ptr<RenderHardware::Device>                      m_Device;
        uint32_t                                                     m_OutputCount;
        bool                                                         m_SampledDepth;
        glm::uvec2                                                   m_Size {};
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> m_Colors;
        RenderHardware::Handle<RenderHardware::Texture>              m_HDR, m_Depth;
        RenderHardware::Handle<RenderHardware::Texture>              m_NormalRoughness;
        bool                                                         m_NormalRoughnessValid = false;
        RenderHardware::Handle<RenderHardware::Texture>              m_SceneVelocity;
        bool                                                         m_VelocityValid = false;
        glm::vec2                                                    m_Jitter {0};
        std::shared_ptr<Fsr3Upscaler>                                m_Fsr3Upscaler;
        std::shared_ptr<TemporalState>                               m_Temporal;
        std::shared_ptr<ShadowResources>                             m_Shadows;
        TransientTexturePool                                         m_Pool;
        uint64_t                                                     m_FrameNumber = 0;
        FrameProfiler                                                m_Profiler;
        std::shared_ptr<GraphCapture>                                m_Capture;
        RenderFeatureSet                                             m_DefaultFeatures;
        ReconstructionFilter                                         m_ReconstructionFilter;
    };
} // namespace Kans
