#pragma once
#include <optional>
#include <glm/glm.hpp>
#include "Kans3D/Renderer/Passes/AntiAliasing.h"
#include "Kans3D/Renderer/Passes/PostProcessSettings.h"

namespace Kans
{

    class Camera
    {
    public:
        Camera() = default;
        Camera(const glm::mat4& ProjectionMatrix);
        virtual ~Camera() = default;

        const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
        void SetProjectionMatirx(const glm::mat4& ProjectionMatrix) { m_ProjectionMatrix = ProjectionMatrix; }
        void SetPerspectiveProjectionMatrix(const float radFov,
                                            const float width,
                                            const float height,
                                            const float nearP,
                                            const float farP);

        const std::optional<PostProcessSettings>& GetPostProcessOverride() const { return m_PostProcessOverride; }
        void                                      SetPostProcessOverride(std::optional<PostProcessSettings> settings)
        {
            m_PostProcessOverride = settings ? std::optional<PostProcessSettings>(settings->Validated()) : std::nullopt;
        }
        PostProcessSettings ResolvePostProcess(const PostProcessSettings& scene) const
        {
            return m_PostProcessOverride.value_or(scene).Validated();
        }
        float  GetExposure() const { return m_PostProcessOverride.value_or(PostProcessSettings {}).Exposure; }
        float& GetExposure() { return EnsurePostProcess().Exposure; }
        void   SetExposure(float value) { EnsurePostProcess().Exposure = value; }
        const BloomSettings& GetBloomSettings() const
        {
            static const BloomSettings defaults;
            return m_PostProcessOverride ? m_PostProcessOverride->Bloom : defaults;
        }
        BloomSettings& GetBloomSettings() { return EnsurePostProcess().Bloom; }
        void           SetBloomSettings(const BloomSettings& value) { EnsurePostProcess().Bloom = value.Validated(); }

        AntiAliasing GetAntiAliasing() const { return m_AntiAliasing; }
        void         SetAntiAliasing(AntiAliasing mode)
        {
            AntiAliasingSamples(mode);
            m_AntiAliasing = mode;
        }

    protected:
        glm::mat4 m_ProjectionMatrix;
        std::optional<PostProcessSettings> m_PostProcessOverride;
        PostProcessSettings&               EnsurePostProcess()
        {
            if (!m_PostProcessOverride)
                m_PostProcessOverride.emplace();
            return *m_PostProcessOverride;
        }
        AntiAliasing  m_AntiAliasing = AntiAliasing::None;
    };

} // namespace Kans