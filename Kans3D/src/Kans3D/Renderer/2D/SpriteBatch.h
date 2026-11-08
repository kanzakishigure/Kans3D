#pragma once
#include <functional>
#include "Kans3D/Renderer/ImmediatePassState.h"
#include "Kans3D/Renderer/RenderView.h"

#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/Camera/Camera.h"
#include "Kans3D/Renderer/Camera/OrthographicCamera.h"
#include "Kans3D/Renderer/Resource/SubTexture2D.h"
#include "Kans3D/Renderer/Resource/Texture.h"

namespace Kans
{

    class SpriteBatch
    {

    public:
        SpriteBatch();
        ~SpriteBatch();
        void Init(std::shared_ptr<RenderHardware::Device> device = {});
        void SetTarget(std::shared_ptr<RenderHardware::Device> device, const RenderHardware::RenderingDesc& target);
        void ClearTarget();
        using SubmissionSink = std::function<void(RenderHardware::Handle<RenderHardware::CommandList>,
                                                  RenderHardware::Handle<RenderHardware::Texture>)>;
        void SetSubmissionSink(SubmissionSink sink);
        // Retains projection metadata for RenderView's temporally jittered recorded-sprite path.
        void SetBatchSubmissionSink(std::function<void(ViewSpriteBatch)> sink);
        void SetCommandList(RenderHardware::CommandList* commands);

        void BeginScene(const OrthographicCamera& camera, ImmediatePassState state = {});
        void BeginScene(const Camera& camera, const glm::mat4& view, ImmediatePassState state = {});

        void EndScene();
        void Shutdown();
        void Flush();

        // 简单图元的绘制，一个有z轴一个无z轴
        void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& tintcolor);
        void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& tintcolor);
        void DrawQuad(const glm::mat4& transform, const glm::vec4& tintcolor);

        void DrawQuad(const glm::vec2&      position,
                      const glm::vec2&      size,
                      const Ref<Texture2D>& Texture2D,
                      const glm::vec4&      tintcolor    = glm::vec4(1.0f),
                      float                 tilingFactor = 1.0f);
        void DrawQuad(const glm::vec3&      position,
                      const glm::vec2&      size,
                      const Ref<Texture2D>& Texture2D,
                      const glm::vec4&      tintcolor    = glm::vec4(1.0f),
                      float                 tilingFactor = 1.0f);
        void DrawQuad(const glm::mat4&      transform,
                      const Ref<Texture2D>& Texture2D,
                      const glm::vec4&      tintcolor    = glm::vec4(1.0f),
                      float                 tilingFactor = 1.0f);

        void DrawQuad(const glm::vec2&         position,
                      const glm::vec2&         size,
                      const Ref<SubTexture2D>& subtexture2D,
                      const glm::vec4&         tintcolor    = glm::vec4(1.0f),
                      float                    tilingFactor = 1.0f);
        void DrawQuad(const glm::vec3&         position,
                      const glm::vec2&         size,
                      const Ref<SubTexture2D>& subtexture2D,
                      const glm::vec4&         tintcolor    = glm::vec4(1.0f),
                      float                    tilingFactor = 1.0f);
        void DrawQuad(const glm::mat4&         transform,
                      const Ref<SubTexture2D>& subtexture2D,
                      const glm::vec4&         tintcolor    = glm::vec4(1.0f),
                      float                    tilingFactor = 1.0f);

        void
        DrawRotateQuad(const glm::vec2& position, float roration, const glm::vec2& size, const glm::vec4& tintcolor);
        void
        DrawRotateQuad(const glm::vec3& position, float roration, const glm::vec2& size, const glm::vec4& tintcolor);
        void DrawRotateQuad(const glm::vec2&      position,
                            float                 roration,
                            const glm::vec2&      size,
                            const Ref<Texture2D>& Texture2D,
                            const glm::vec4&      tintcolor    = glm::vec4(1.0f),
                            float                 tilingFactor = 1.0f);
        void DrawRotateQuad(const glm::vec3&      position,
                            float                 roration,
                            const glm::vec2&      size,
                            const Ref<Texture2D>& Texture2D,
                            const glm::vec4&      tintcolor    = glm::vec4(1.0f),
                            float                 tilingFactor = 1.0f);

        void DrawSprite(const glm::mat4&,
                        const Ref<Texture2D>&,
                        const glm::vec4&,
                        float     tiling,
                        glm::vec4 uvRect,
                        bool      flipX,
                        bool      flipY);
        using Statistics = SpriteStatistics;

        void        ResetStats();
        Statistics& GetStats();
        double      GetLastRecordMilliseconds() const;

    private:
        void FlushAndReset();
        void Append(const glm::mat4&      transform,
                    const Ref<Texture2D>& texture,
                    const glm::vec4&      color,
                    const glm::vec2*      uv,
                    float                 tiling);
        struct Impl;
        std::unique_ptr<Impl> m_Data;
    };
} // namespace Kans