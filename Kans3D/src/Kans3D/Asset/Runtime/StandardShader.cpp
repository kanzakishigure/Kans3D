#include "kspch.h"
#include "StandardShader.h"
#include <stdexcept>
#include "Kans3D/Asset/Importer/MaterialSource.h"
#include "Kans3D/Asset/Importer/ShaderImporter.h"
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "MaterialShaderInterface.h"

namespace Kans
{
    AssetID StandardShaderID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 1}; }
    AssetID SixWaySmokeShaderID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 2}; }
    AssetID SixWaySmokeMaterialID() { return AssetID {SourceAssetID::FromString("ffffffffffffffff"), 3}; }
    Ref<const MaterialDataAsset> GetSixWaySmokeMaterialAsset()
    {
        static const auto asset = [] {
            MaterialSource source;
            source.Shader           = SixWaySmokeShaderID();
            source.Render.AlphaMode = MaterialAlphaMode::Blend;
            auto result             = ResolveMaterialSource(source, &GetSixWaySmokeShader());
            if (auto error = std::get_if<AssetError>(&result))
                throw std::runtime_error(error->Message);
            auto value     = CreateRef<MaterialDataAsset>(std::get<MaterialData>(std::move(result)));
            value->assetID = SixWaySmokeMaterialID();
            return value;
        }();
        return asset;
    }
    bool IsStandardShader(AssetID id) { return !id.IsValid() || id == StandardShaderID(); }

    const ShaderData& GetStandardShader(bool particle)
    {
        static const auto build = [](bool particle) {
            const std::string                  manifest = ShaderText::StandardManifest;
            std::map<std::string, std::string> stages   = {{"Standard.vert", ShaderText::StandardVertex},
                                                           {"Standard.frag", ShaderText::StandardFragment}};
            // Keep the ordinary mesh sources identical to Standard's original program.
            // Only procedural particle variants opt into vertex diffuse lighting.
            if (particle)
            {
                auto&             vertex   = stages.at("Standard.vert");
                const std::string lighting = ShaderText::StandardParticleLightingInclude;
                vertex.insert(vertex.find("layout(location = 0)"), lighting);
                vertex.insert(vertex.find("    gl_Position"), ShaderText::StandardParticleVertexLighting);
                auto& fragment = stages.at("Standard.frag");
                fragment.insert(fragment.find("#include"), lighting);
                const auto shade = fragment.find("    vec3 color = KansShadePbr");
                fragment.insert(shade, ShaderText::StandardParticleFragmentLighting);
                fragment.replace(
                    fragment.find("    vec3 color = KansShadePbr"), 17, ShaderText::StandardParticleFallbackColor);
            }
            auto compiled = CompileEmbeddedShaderProgram(manifest, stages);
            if (auto* error = std::get_if<AssetError>(&compiled))
                throw std::runtime_error("Standard shader: " + error->Message);
            auto value    = std::get<ShaderData>(std::move(compiled));
            value.DebugName    = particle ? "Kans3D/Standard/Particle" : "Kans3D/Standard";
            value.DebugSources = {{ShaderProductStage::Vertex, stages.at("Standard.vert")},
                                  {ShaderProductStage::Fragment, stages.at("Standard.frag")}};
            auto contract = ValidateMaterialShaderInterface(value.Reflection);
            if (auto* error = std::get_if<AssetError>(&contract))
                throw std::runtime_error("Standard shader: " + error->Message);
            return value;
        };
        static const ShaderData shader = build(false);
        if (!particle)
            return shader;
        static const ShaderData particleShader = build(true);
        return particleShader;
    }

    Ref<const ShaderDataAsset> GetStandardShaderAsset()
    {
        static const Ref<const ShaderDataAsset> asset = [] {
            auto value     = CreateRef<ShaderDataAsset>(GetStandardShader());
            value->assetID = StandardShaderID();
            return value;
        }();
        return asset;
    }

    const ShaderData& GetSixWaySmokeShader(bool particle)
    {
        static const auto build = [](bool particle) {
            const std::string                  manifest = ShaderText::StandardSixWayManifest;
            std::map<std::string, std::string> stages   = {{"SixWaySmoke.vert", ShaderText::StandardSixWayVertex},
                                                           {"SixWaySmoke.frag", ShaderText::StandardSixWayFragment}};
            if (particle)
                for (auto& [name, source] : stages)
                    source.insert(source.find('\n') + 1, ShaderText::StandardParticleVariantDefine);
            auto compiled = CompileEmbeddedShaderProgram(manifest, stages);
            if (auto* error = std::get_if<AssetError>(&compiled))
                throw std::runtime_error("Kans3D/SixWaySmoke shader: " + error->Message);
            auto value    = std::get<ShaderData>(std::move(compiled));
            value.DebugName    = particle ? "Kans3D/SixWaySmoke/Particle" : "Kans3D/SixWaySmoke";
            value.DebugSources = {{ShaderProductStage::Vertex, stages.at("SixWaySmoke.vert")},
                                  {ShaderProductStage::Fragment, stages.at("SixWaySmoke.frag")}};
            auto contract = ValidateMaterialShaderInterface(value.Reflection);
            if (auto* error = std::get_if<AssetError>(&contract))
                throw std::runtime_error("Kans3D/SixWaySmoke shader: " + error->Message);
            return value;
        };
        static const ShaderData shader = build(false);
        if (!particle)
            return shader;
        static const ShaderData particleShader = build(true);
        return particleShader;
    }

    Ref<const ShaderDataAsset> GetSixWaySmokeShaderAsset()
    {
        static const Ref<const ShaderDataAsset> asset = [] {
            auto value     = CreateRef<ShaderDataAsset>(GetSixWaySmokeShader());
            value->assetID = SixWaySmokeShaderID();
            return value;
        }();
        return asset;
    }

    std::variant<MaterialData, AssetError> ResolveStandardMaterial(const MaterialData&                 legacy,
                                                                   std::optional<std::array<float, 3>> emissive)
    {
        if (legacy.Bindings && IsStandardShader(legacy.Shader))
            return legacy;
        MaterialSource source;
        source.Name       = legacy.Name;
        source.Render     = legacy.Render;
        source.Parameters = {{"BaseColor", {legacy.BaseColor.begin(), legacy.BaseColor.end()}},
                             {"Metallic", {legacy.Metallic}},
                             {"Roughness", {legacy.Roughness}}};
        using S           = MaterialTextureSemantic;
        const auto pick   = [&](const char* name, std::initializer_list<S> semantics) {
            for (S semantic : semantics)
                for (const auto& texture : legacy.Textures)
                    if (texture.Semantic == uint32_t(semantic) && texture.UVChannel == 0 && texture.Texture.IsValid())
                    {
                        source.Textures[name] = texture.Texture;
                        return;
                    }
        };
        pick("BaseColorMap", {S::BaseColor, S::Diffuse});
        pick("NormalMap", {S::Normal, S::NormalCamera, S::Height});
        pick("MetallicRoughnessMap", {S::MetallicRoughness, S::Unknown, S::DiffuseRoughness});
        pick("OcclusionMap", {S::AmbientOcclusion, S::Lightmap});
        pick("EmissiveMap", {S::EmissionColor, S::Emissive});
        // Legacy materials emitted the texture directly, with no factor.
        if (source.Textures.count("EmissiveMap"))
            source.Parameters["EmissiveColor"] = {1, 1, 1};
        if (emissive && (!source.Textures.count("EmissiveMap") || *emissive != std::array<float, 3> {0, 0, 0}))
            source.Parameters["EmissiveColor"] = {emissive->begin(), emissive->end()};
        return ResolveMaterialSource(source, nullptr);
    }
} // namespace Kans
