#include "kspch.h"
#include <algorithm>
#include <cstring>
#include <set>
#include "Kans3D/Asset/AssetScanner.h"
#include "Kans3D/Asset/Importer/MaterialImporter.h"
#include "Kans3D/Asset/Importer/ModelImporter.h"
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Asset/Product/ShaderProduct.h"
#include "Kans3D/Asset/Runtime/MaterialBindings.h"
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Project.h"

namespace Kans
{
    namespace
    {
        std::string FileStem(const std::string& name)
        {
            std::string result;
            for (unsigned char c : name)
                result += (c < 32 || c == 127 || std::string("<>:\"/\\|?*").find(char(c)) != std::string::npos) ?
                              '_' :
                              char(c);
            while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
                result.pop_back();
            if (result.empty() || result == "." || result == "..")
                result = "Material";
            // Leave room for the model stem, numeric suffix, extension and sidecar.
            if (result.size() > 80)
            {
                result.resize(80);
                while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80)
                    result.pop_back();
                if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xc0)
                    result.pop_back();
            }
            return result;
        }
    } // namespace

    std::variant<std::vector<ModelMaterialSlot>, AssetError>
    Project::GetModelMaterialSlots(SourceAssetID source, const std::string& target) const
    {
        const auto metadata = m_SourceAssetDatabase.FindByID(source);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Model source is unknown or Missing"};
        auto importers = CreateDefaultImporterRegistry();
        if (const auto* error = std::get_if<AssetError>(&importers))
            return *error;
        auto selected = std::get<ImporterRegistry>(importers).Resolve(SourceFile(metadata), metadata.Import.Importer);
        if (const auto* error = std::get_if<AssetError>(&selected))
            return *error;
        if (!dynamic_cast<const ModelImporter*>(std::get<RegisteredImporter>(selected).Importer.get()))
            return AssetError {AssetErrorCode::InvalidArgument, "Source does not use the Model importer"};
        const auto stored = m_ProductStore->FindSource(source);
        if (!stored || !m_ProductStore->FindBuild(source, target))
            return AssetError {AssetErrorCode::NotFound,
                               "Model must be imported for target " + target + " before listing material slots"};
        std::vector<ModelMaterialSlot> slots;
        const auto                     registry = GetRuntimeAssetRegistry();
        for (const auto& [key, local] : stored->LocalIDs)
        {
            if (key.compare(0, 9, "material:") != 0)
                continue;
            const AssetID embedded(source, local);
            const auto    record = registry->Find(embedded, target);
            if (!record || record->Metadata.Type != AssetType::Material)
                continue; // Reserved IDs of removed slots are not current slots.
            auto bytes = ReadRegisteredProduct(GetProductDirectory(), *record);
            if (const auto* error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& input   = std::get<std::vector<std::byte>>(bytes);
            auto        decoded = DecodeMaterialProduct(input.data(), input.size());
            if (const auto* error = std::get_if<AssetError>(&decoded))
                return *error;
            ModelMaterialSlot slot {key, std::get<MaterialData>(decoded).Name, embedded, {}};
            if (const auto remap = metadata.Import.Settings.find("material." + key);
                remap != metadata.Import.Settings.end())
            {
                const auto id = AssetID::FromString(remap->second);
                if (!id.IsValid() || id.ToString() != remap->second)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid material remap for slot " + key};
                slot.Remap = id;
            }
            slots.push_back(std::move(slot));
        }
        return slots;
    }

    std::variant<std::monostate, AssetError> Project::SetModelMaterialRemap(SourceAssetID          source,
                                                                            const std::string&     slot,
                                                                            std::optional<AssetID> material,
                                                                            const std::string&     target)
    {
        auto described = GetModelMaterialSlots(source, target);
        if (const auto* error = std::get_if<AssetError>(&described))
            return *error;
        const auto& slots = std::get<std::vector<ModelMaterialSlot>>(described);
        if (std::none_of(slots.begin(), slots.end(), [&](const auto& item) { return item.Key == slot; }) ||
            (material && !material->IsValid()))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid material slot or remap identity"};
        auto config = m_SourceAssetDatabase.FindByID(source).Import;
        if (material)
            config.Settings["material." + slot] = material->ToString();
        else
            config.Settings.erase("material." + slot);
        return SetSourceImportConfiguration(source, config);
    }

    std::variant<ModelMaterialExtractionResult, AssetError>
    Project::ExtractModelMaterials(SourceAssetID                   source,
                                   const std::filesystem::path&    directory,
                                   const std::vector<std::string>& requested,
                                   const std::string&              target)
    {
        auto described = GetModelMaterialSlots(source, target);
        if (const auto* error = std::get_if<AssetError>(&described))
            return *error;
        const auto&           slots = std::get<std::vector<ModelMaterialSlot>>(described);
        std::set<std::string> selected(requested.begin(), requested.end());
        for (const auto& key : selected)
            if (std::none_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.Key == key; }))
                return AssetError {AssetErrorCode::InvalidArgument, "Unknown material extraction slot: " + key};
        ModelMaterialExtractionResult result;
        try
        {
            const auto destination = std::filesystem::weakly_canonical(m_Config.AssetDirectory / directory);
            const auto relative    = destination.lexically_relative(m_Config.AssetDirectory);
            if (relative.empty() || *relative.begin() == "..")
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "Extraction directory must be inside the asset directory"};
            if (!AssetScanner::IsDiscoverablePath(AssetPath(relative / "material.material")))
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "Extraction directory is excluded from asset discovery"};
            std::filesystem::create_directories(destination);
            const auto metadata      = m_SourceAssetDatabase.FindByID(source);
            auto       configuration = metadata.Import;
            const auto registry      = GetRuntimeAssetRegistry();
            const auto stem          = FileStem(metadata.Path.GetPath().stem().u8string());
            for (const auto& slot : slots)
            {
                if (!requested.empty() && !selected.count(slot.Key))
                    continue;
                if (slot.Remap)
                {
                    result.Skipped.push_back(slot.Key);
                    continue;
                }
                const auto record = registry->Find(slot.EmbeddedMaterial, target);
                auto       bytes  = ReadRegisteredProduct(GetProductDirectory(), *record);
                if (const auto* error = std::get_if<AssetError>(&bytes))
                {
                    result.Error = *error;
                    return result;
                }
                const auto& input   = std::get<std::vector<std::byte>>(bytes);
                auto        decoded = DecodeMaterialProduct(input.data(), input.size());
                if (const auto* error = std::get_if<AssetError>(&decoded))
                {
                    result.Error = *error;
                    return result;
                }
                const auto& material = std::get<MaterialData>(decoded);
                ShaderData  shader   = GetStandardShader();
                if (material.Shader == SixWaySmokeShaderID())
                    shader = GetSixWaySmokeShader();
                else if (!IsStandardShader(material.Shader))
                {
                    const auto shaderRecord = registry->Find(material.Shader, target);
                    if (!shaderRecord || shaderRecord->Metadata.Type != AssetType::Shader)
                    {
                        result.Error = AssetError {AssetErrorCode::NotFound, "Embedded material Shader is unavailable"};
                        return result;
                    }
                    auto shaderBytes = ReadRegisteredProduct(GetProductDirectory(), *shaderRecord);
                    if (const auto* error = std::get_if<AssetError>(&shaderBytes))
                    {
                        result.Error = *error;
                        return result;
                    }
                    const auto& code       = std::get<std::vector<std::byte>>(shaderBytes);
                    auto        shaderData = DecodeShaderProduct(code.data(), code.size());
                    if (const auto* error = std::get_if<AssetError>(&shaderData))
                    {
                        result.Error = *error;
                        return result;
                    }
                    shader = std::get<ShaderData>(std::move(shaderData));
                }
                auto editable = EditableMaterial(material, shader);
                if (const auto* error = std::get_if<AssetError>(&editable))
                {
                    result.Error = *error;
                    return result;
                }
                const auto             text = SerializeMaterialSource(std::get<MaterialSource>(editable));
                std::vector<std::byte> output(text.size());
                std::memcpy(output.data(), text.data(), text.size());
                const auto name = stem + "_" + FileStem(slot.DisplayName);
                auto       file = destination / (name + ".material");
                for (size_t suffix = 1;
                     std::filesystem::exists(std::filesystem::symlink_status(file)) ||
                     std::filesystem::exists(std::filesystem::symlink_status(file.string() + ".kmeta"));
                     ++suffix)
                    file = destination / (name + "_" + std::to_string(suffix) + ".material");
                auto written = WriteProductFile(file, output); // Exclusive create; never overwrites.
                if (const auto* error = std::get_if<AssetError>(&written))
                {
                    result.Error = *error;
                    return result;
                }
                result.Created.push_back({slot.Key, file, {}, {}});
            }
            if (!result.Created.empty())
            {
                auto refreshed = RefreshAssets();
                if (const auto* error = std::get_if<AssetError>(&refreshed))
                {
                    result.Error = *error;
                    return result;
                }
                for (auto& created : result.Created)
                {
                    created.Source = m_SourceAssetDatabase.FindIDByPath(
                        AssetPath(created.File.lexically_relative(m_Config.AssetDirectory)));
                    if (!created.Source.IsValid())
                    {
                        result.Error = AssetError {AssetErrorCode::IoError,
                                                   "Cannot register extracted material: " + created.File.u8string()};
                        return result;
                    }
                    auto extracted = m_SourceAssetDatabase.FindByID(created.Source);
                    auto ids       = AllocateLocalIDs(extracted.LocalIDs, {MaterialImporter::RootObjectKey});
                    if (const auto* error = std::get_if<AssetError>(&ids))
                    {
                        result.Error = *error;
                        return result;
                    }
                    extracted.LocalIDs = std::get<LocalIDMap>(std::move(ids));
                    auto saved         = SaveSidecar(extracted);
                    if (const auto* error = std::get_if<AssetError>(&saved))
                    {
                        result.Error = *error;
                        return result;
                    }
                    created.Material = AssetID(created.Source, extracted.LocalIDs.at(MaterialImporter::RootObjectKey));
                    configuration.Settings["material." + created.SlotKey] = created.Material.ToString();
                }
                auto saved = SetSourceImportConfiguration(source, configuration);
                if (const auto* error = std::get_if<AssetError>(&saved))
                {
                    result.Error = *error;
                    return result;
                }
            }
            result.RemapsApplied = true;
            return result;
        }
        catch (const std::exception& error)
        {
            result.Error = AssetError {AssetErrorCode::IoError, error.what()};
            return result;
        }
    }
} // namespace Kans
