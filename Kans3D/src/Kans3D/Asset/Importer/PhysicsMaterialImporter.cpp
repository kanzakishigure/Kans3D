#include "kspch.h"
#include "PhysicsMaterialImporter.h"
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Physics/PhysicsMaterial.h"
namespace Kans
{
    namespace
    {
        std::variant<PhysicsMaterialValues, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 1024 * 1024);
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            try
            {
                return DecodePhysicsMaterialNode(
                    YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size())));
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
    } // namespace
    std::variant<ImportInputs, AssetError> PhysicsMaterialImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        return ImportInputs {};
    }
    std::variant<SourceInventory, AssetError> PhysicsMaterialImporter::Inspect(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto error = std::get_if<AssetError>(&data))
            return *error;
        return SourceInventory {};
    }
    std::variant<ImportResult, AssetError> PhysicsMaterialImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid PhysicsMaterial import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "PhysicsMaterial staging must be empty and exclusive"};
            auto data = Read(c);
            if (auto error = std::get_if<AssetError>(&data))
                return *error;
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto error = std::get_if<AssetError>(&ids))
                return *error;
            ImportResult out;
            out.LocalIDs     = std::get<LocalIDMap>(ids);
            const auto local = out.LocalIDs.at(RootObjectKey);
            auto       bytes = EncodePhysicsMaterialProduct(std::get<PhysicsMaterialValues>(data));
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            out.Products.push_back({local, AssetType::PhysicsMaterial, std::to_string(local) + ".kpm", {}});
            auto written = WriteProductFile(c.StagingDirectory / out.Products[0].StagedFile,
                                            std::get<std::vector<std::byte>>(bytes));
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
