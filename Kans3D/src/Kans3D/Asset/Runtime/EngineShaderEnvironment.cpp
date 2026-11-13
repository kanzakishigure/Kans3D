#include "kspch.h"
#include "EngineShaderEnvironment.h"
#include "Kans3D/Asset/Product/ContentDigest.h"
#include "Kans3D/Renderer/Shading/SkinningSource.h"
#include "Kans3D/VFX/VfxAsset.h"
#include "EngineShaderText.h"
#include "MaterialShaderInterface.h"

namespace Kans::ShaderText
{
#define KANS_SHADER_TEXT(id, name, text) const char* const id = text;
#include "EngineShaderTexts.inc"
#undef KANS_SHADER_TEXT
} // namespace Kans::ShaderText

namespace Kans
{
    const std::map<std::string, std::string>& GetEngineShaderTexts()
    {
        static const auto texts = [] {
            auto                                     result = GetEngineShaderIncludes();
            const std::map<std::string, std::string> generated {
#define KANS_SHADER_TEXT(id, name, text) {name, ShaderText::id},
#include "EngineShaderTexts.inc"
#undef KANS_SHADER_TEXT
                {"Skinning/MorphVector", SkinningMorphVectorGLSL},
                {
                    "Vfx/Curl.glsl",
#include "Kans3D/VFX/VfxCurlGLSL.inc"
                },
                {
                    "Vfx/Collision.glsl",
#include "Kans3D/VFX/VfxCollisionGLSL.inc"
                },
                {
                    "Vfx/Fields.glsl",
#include "Kans3D/VFX/VfxFieldsGLSL.inc"
                },
                {
                    "Vfx/VectorField.glsl",
#include "Kans3D/VFX/VfxVectorFieldGLSL.inc"
                },
                {
                    "Vfx/MeshSampling.glsl",
#include "Kans3D/VFX/VfxMeshSamplingGLSL.inc"
                }};
            result.insert(generated.begin(), generated.end());
            // These binary-resident type spellings are also emitted by GType in VFX programs.
            for (const auto type : {Vfx::Type::Float,
                                    Vfx::Type::Vec2,
                                    Vfx::Type::Vec3,
                                    Vfx::Type::Vec4,
                                    Vfx::Type::Int,
                                    Vfx::Type::Bool,
                                    Vfx::Type::IVec3})
            {
                const std::string text = Vfx::TypeName(type);
                result.emplace("Vfx/Type/" + text, text);
            }
            return result;
        }();
        return texts;
    }

    std::string ComputeEngineShaderDigest(const std::map<std::string, std::string>& texts, uint32_t contractVersion)
    {
        std::vector<std::byte> bytes;
        const auto             appendNumber = [&](uint64_t value) {
            for (unsigned i = 0; i < 8; ++i)
                bytes.push_back(std::byte((value >> (i * 8)) & 255));
        };
        const auto append = [&](const std::string& text) {
            appendNumber(text.size());
            for (unsigned char c : text)
                bytes.push_back(std::byte(c));
        };
        append("Kans3D.EngineShaderTexts.1");
        appendNumber(contractVersion);
        for (const auto& [name, text] : texts)
        {
            append(name);
            append(text);
        }
        return ComputeContentDigest(bytes);
    }

    const std::string& GetEngineShaderDigest()
    {
        static const auto digest = ComputeEngineShaderDigest(GetEngineShaderTexts(), EngineShaderContractVersion);
        return digest;
    }
} // namespace Kans
