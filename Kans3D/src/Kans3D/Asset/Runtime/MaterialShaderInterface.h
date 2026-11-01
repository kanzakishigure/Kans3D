#pragma once
#include <glm/glm.hpp>
#include "ShaderData.h"

namespace Kans
{
    // Contract between material Shaders and the renderer.
    //
    // Descriptor set 0 holds engine data, declared by `#include <Kans3D/Engine.glsl>`;
    // set 1 holds material data: uniform blocks filled from material parameters and
    // 2D samplers bound to material textures. Vertex inputs follow the engine mesh
    // layout (`#include <Kans3D/VertexInput.glsl>`); the fragment stage writes a
    // vec4 color to location 0.
    constexpr uint32_t EngineDescriptorSet   = 0;
    constexpr uint32_t MaterialDescriptorSet = 1;
    constexpr uint32_t EngineLightCount      = 4; // Published prefix; never change.
    constexpr uint32_t EnginePointLightLimit = 16;

    enum class EngineBinding : uint32_t
    {
        Frame         = 0,
        Object        = 1,
        IrradianceMap = 2,
        PrefilterMap  = 3,
        BrdfLUT       = 4,
        DirectionalShadow    = 5,
        DirectionalShadowRaw = 6,
        PointShadow0         = 7,
        PointShadow1         = 8,
        PointShadow2         = 9,
        PointShadow3         = 10,
    };
    constexpr uint32_t EngineBindingCount = 11;

    // std140 images of the engine uniform blocks.
    struct EngineFrameUniforms
    {
        glm::mat4 ViewProjection {1.0f};
        glm::vec4 CameraPosition {0.0f};
        glm::vec4 LightPositions[EngineLightCount] {};
        // Legacy radiance: diffuse intensity of the first four distinct point lights.
        glm::vec4 LightColors[EngineLightCount] {};
        // The point light's diffuse intensity, for non-PBR (e.g. toon) shading.
        glm::vec4 LightDiffuse[EngineLightCount] {};
        float     Time = 0.0f;
        float     DebugView = 0.0f; // 0 lit, 1 base color, 2 world normals, 3 legacy wireframe.
        float     Padding[2] {};
        glm::vec4 PointPositions[EnginePointLightLimit] {};
        glm::vec4 PointRadiance[EnginePointLightLimit] {};
        glm::vec4 PointSpecular[EnginePointLightLimit] {};
        glm::vec4 DirectionalDirection {0}; // Direction of light travel; w: present.
        glm::vec4 DirectionalRadiance {0};
        glm::vec4 DirectionalSpecular {0};
        glm::vec4 DirectionalAmbient {0};
        glm::vec4 LightingInfo {0, 0.8f, 0, 0}; // x: point count, y: exposure multiplier.
        glm::mat4 CascadeViewProjection[4] {glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(1)};
        glm::vec4 CascadeSplits {0};
        glm::vec4 CascadeBlendStarts {0};
        glm::vec4 CascadeTexel[4] {};
        glm::vec4 ShadowInfo {0};
        glm::vec4 ShadowCameraDepth {0};
        glm::vec4 PointShadowIndices[4] {glm::vec4(-1), glm::vec4(-1), glm::vec4(-1), glm::vec4(-1)};
        glm::vec4 PointShadowPositionRange[4] {};
        glm::vec4 PointShadowParams[4] {};
        glm::vec4 ShadowPass {0};
        glm::vec4 ShadowPassPosition {0};
        glm::vec4 PointShadowSourceRadius {0};
        glm::vec4 EnvironmentInfo {1, 1, 1, 0}; // diffuse, specular, cos(rotation), sin(rotation).
        glm::mat4 View {1.0f};                  // World to camera; right-handed, -Z forward, +Y up.
        glm::mat4 Projection {1.0f};            // View to device clip, matching ViewProjection.
        glm::mat4 InverseView {1.0f};
        glm::mat4 InverseProjection {1.0f};
    };
    static_assert(offsetof(EngineFrameUniforms, View) == 1776, "Published environment prefix");
    static_assert(sizeof(EngineFrameUniforms) == 2032, "KansFrame std140 layout");
    static_assert(offsetof(EngineFrameUniforms, EnvironmentInfo) == 1760, "Published shadow prefix");
    static_assert(offsetof(EngineFrameUniforms, CascadeViewProjection) == 1136, "Published lighting prefix");
    static_assert(offsetof(EngineFrameUniforms, DebugView) == 276, "KansFrame debug uses existing tail padding");

    static_assert(offsetof(EngineFrameUniforms, PointPositions) == 288, "Published KansFrame prefix");
    static_assert(offsetof(EngineFrameUniforms, LightingInfo) == 1120, "KansFrame lighting tail");

    struct EngineObjectUniforms
    {
        glm::mat4 Transform {1.0f};
        glm::mat4 NormalMatrix {1.0f};
        // x: MaterialAlphaMode (0 opaque, 1 mask, 2 blend, 3 dithered, 6 additive, 7 premultiplied).
        // y: cutoff; z: double-sided. Shader-only coverage states occupy 4 and 5.
        glm::vec4 MaterialRender {0.0f, 0.5f, 0.0f, 0.0f};
    };
    static_assert(sizeof(EngineObjectUniforms) == 144, "KansObject std140 layout");
    static_assert(offsetof(EngineObjectUniforms, MaterialRender) == 128, "KansObject render state appended");

    // GLSL text of an engine include (`#include <name>`), or nullptr.
    [[nodiscard]] const std::string* FindEngineShaderInclude(const std::string& name);

    // Checks a reflected program against the material contract.
    [[nodiscard]] std::variant<std::monostate, AssetError>
    ValidateMaterialShaderInterface(const std::vector<ShaderStageReflection>& reflection,
                                    ShaderDomain                              domain = ShaderDomain::Surface);

    [[nodiscard]] std::variant<std::monostate, AssetError>
    ValidateMaterialParameterGroups(const std::vector<MaterialParameterGroup>& groups,
                                    const std::vector<ShaderStageReflection>&  reflection);

    // Re-inspects bytecode and checks shared descriptor layouts for extra passes.
    [[nodiscard]] std::variant<std::monostate, AssetError>
    ValidateMaterialShaderPasses(const ShaderProgramCode& main, const std::vector<ShaderPass>& passes);

    // Material-set descriptors of a validated program, merged across stages and
    // ordered by binding.
    [[nodiscard]] std::vector<ShaderResourceBinding>
    MaterialShaderResources(const std::vector<ShaderStageReflection>& reflection);
} // namespace Kans
