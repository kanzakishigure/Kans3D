#include "kspch.h"
#include "ClothImporter.h"
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Physics/ClothAsset.h"
namespace Kans
{
    namespace
    {
        std::variant<ClothData, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 128 * 1024 * 1024);
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            try
            {
                return DecodeClothNode(YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size())));
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
    } // namespace
    std::variant<ImportInputs, AssetError> ClothImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        ImportInputs inputs;
        const auto   id = std::get<ClothData>(data).Mesh;
        if (id.IsValid())
            inputs.Products.push_back(id);
        return inputs;
    }
    std::variant<SourceInventory, AssetError> ClothImporter::Inspect(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto error = std::get_if<AssetError>(&data))
            return *error;
        return SourceInventory {};
    }
    std::variant<ImportResult, AssetError> ClothImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Cloth import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument, "Cloth staging must be empty and exclusive"};
            auto data = Read(c);
            if (auto error = std::get_if<AssetError>(&data))
                return *error;
            if (!std::get<ClothData>(data).Mesh.IsValid())
                return AssetError {AssetErrorCode::InvalidData, "Cloth source mesh ID must be valid"};
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto error = std::get_if<AssetError>(&ids))
                return *error;
            ImportResult out;
            out.LocalIDs                       = std::get<LocalIDMap>(ids);
            const auto                   local = out.LocalIDs.at(RootObjectKey);
            const auto                   text  = YAML::Dump(EncodeClothNode(std::get<ClothData>(data)));
            const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                               reinterpret_cast<const std::byte*>(text.data() + text.size()));
            out.Products.push_back(
                {local, AssetType::Cloth, std::to_string(local) + ".kcloth", {std::get<ClothData>(data).Mesh}});
            auto written = WriteProductFile(c.StagingDirectory / out.Products[0].StagedFile, bytes);
            if (auto error = std::get_if<AssetError>(&written))
                return *error;
            return out;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
