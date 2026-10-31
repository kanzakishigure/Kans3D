#include "kspch.h"
#include "ImporterRegistry.h"
#include <algorithm>
#include "ActionImporter.h"
#include "AudioImporter.h"
#include "ClothImporter.h"
#include "ExpressionImporter.h"
#include "FontImporter.h"
#include "MaterialImporter.h"
#include "ModelImporter.h"
#include "PhysicsMaterialImporter.h"
#include "RagdollImporter.h"
#include "ShaderImporter.h"
#include "TextureImporter.h"
#include "TimelineImporter.h"
#include "UIImporter.h"

namespace Kans
{
    namespace
    {
        std::string Lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
            });
            return text;
        }
        bool IsName(const std::string& text, const char* allowed)
        {
            return !text.empty() && text.size() <= 256 && text.find_first_not_of(allowed) == std::string::npos;
        }
    } // namespace

    std::variant<std::monostate, AssetError> ImporterRegistry::Register(const std::string&                 name,
                                                                        const std::vector<std::string>&    extensions,
                                                                        std::shared_ptr<AssetImporterBase> importer,
                                                                        bool                               makeDefault)
    {
        constexpr const char* NameCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
        if (!IsName(name, NameCharacters) || !importer || importer->GetVersion() == 0 || extensions.empty() ||
            m_Named.count(name))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid or duplicate importer registration"};
        RegisteredImporter entry {name, importer, {}};
        for (const auto& extension : extensions)
        {
            auto normalized = Lowercase(extension);
            if (normalized.size() < 2 || normalized.front() != '.' ||
                !IsName(normalized.substr(1), "abcdefghijklmnopqrstuvwxyz0123456789_") ||
                std::count(entry.Extensions.begin(), entry.Extensions.end(), normalized) ||
                (makeDefault && m_Defaults.count(normalized)))
                return AssetError {AssetErrorCode::InvalidArgument, "Invalid or already claimed importer extension"};
            entry.Extensions.push_back(std::move(normalized));
        }
        if (makeDefault)
            for (const auto& extension : entry.Extensions)
                m_Defaults.emplace(extension, name);
        m_Named.emplace(name, std::move(entry));
        return std::monostate {};
    }

    std::variant<RegisteredImporter, AssetError> ImporterRegistry::Resolve(const std::filesystem::path& sourceFile,
                                                                           const std::string&           name) const
    {
        const auto  extension = Lowercase(sourceFile.extension().u8string());
        std::string selected  = name;
        if (selected.empty())
        {
            const auto found = m_Defaults.find(extension);
            if (found == m_Defaults.end())
                return AssetError {AssetErrorCode::NotFound, "No importer for source format " + extension};
            selected = found->second;
        }
        const auto found = m_Named.find(selected);
        if (found == m_Named.end())
            return AssetError {AssetErrorCode::NotFound, "Unknown importer: " + selected};
        const auto& supported = found->second.Extensions;
        if (std::find(supported.begin(), supported.end(), extension) == supported.end())
            return AssetError {AssetErrorCode::InvalidArgument, selected + " does not import " + extension + " files"};
        return found->second;
    }

    std::variant<ImportSettingsSchema, AssetError> ImporterRegistry::GetSettingsSchema(const std::string& name) const
    {
        auto it = m_Named.find(name);
        if (it == m_Named.end())
            return AssetError {AssetErrorCode::NotFound, "Unknown importer: " + name};
        return it->second.Importer->GetSettingsSchema();
    }

    std::variant<ImportSettingsSchema, AssetError>
    ImporterRegistry::GetSettingsSchemaForSource(const std::filesystem::path& file, const std::string& name) const
    {
        auto resolved = Resolve(file, name);
        if (auto* error = std::get_if<AssetError>(&resolved))
            return *error;
        return std::get<RegisteredImporter>(resolved).Importer->GetSettingsSchema();
    }

    bool ImporterRegistry::Supports(const std::filesystem::path& sourceFile) const
    {
        return m_Defaults.count(Lowercase(sourceFile.extension().u8string())) != 0;
    }

    std::variant<ImporterRegistry, AssetError> CreateDefaultImporterRegistry()
    {
        ImporterRegistry registry;
        const std::pair<const char*, std::pair<std::vector<std::string>, std::shared_ptr<AssetImporterBase>>>
            importers[] = {
                {"ModelImporter",
                 {{".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds"}, std::make_shared<ModelImporter>()}},
                {"TextureImporter",
                 {{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"}, std::make_shared<TextureImporter>()}},
                {"ShaderImporter", {{".vert", ".frag", ".comp", ".shaderprog"}, std::make_shared<ShaderImporter>()}},
                {"MaterialImporter", {{".material"}, std::make_shared<MaterialImporter>()}},
                {"ActionImporter", {{".action"}, std::make_shared<ActionImporter>()}},
                {"RagdollImporter", {{".ragdoll"}, std::make_shared<RagdollImporter>()}},
                {"ClothImporter", {{".cloth"}, std::make_shared<ClothImporter>()}},
                {"PhysicsMaterialImporter", {{".physmat"}, std::make_shared<PhysicsMaterialImporter>()}},
                {"TimelineImporter", {{".timeline"}, std::make_shared<TimelineImporter>()}},
                {"ExpressionImporter", {{".expression"}, std::make_shared<ExpressionImporter>()}},
                {"UIImporter", {{".kui"}, std::make_shared<UIImporter>()}},
                {"FontImporter", {{".ttf", ".otf", ".ttc", ".otc"}, std::make_shared<FontImporter>()}},
                {"AudioImporter", {{".wav", ".ogg", ".mp3", ".flac"}, std::make_shared<AudioImporter>()}},
        };
        for (const auto& [name, entry] : importers)
        {
            auto registered = registry.Register(name, entry.first, entry.second);
            if (auto* error = std::get_if<AssetError>(&registered))
                return std::move(*error);
        }
        return registry;
    }
} // namespace Kans
