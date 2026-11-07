#include "kspch.h"
#include "RagdollImporter.h"
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Physics/RagdollAsset.h"
namespace Kans
{
    namespace
    {
        std::variant<RagdollData, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 128 * 1024 * 1024);
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            try
            {
                return DecodeRagdollNode(YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size())));
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
    } // namespace
    std::variant<ImportInputs, AssetError> RagdollImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        ImportInputs inputs;
        const auto   id = std::get<RagdollData>(data).Skeleton;
        if (id.IsValid())
            inputs.Products.push_back(id);
        for (const auto& body : std::get<RagdollData>(data).Bodies)
            if (body.Material.IsValid())
                inputs.Products.push_back(body.Material);
        std::sort(inputs.Products.begin(), inputs.Products.end());
        inputs.Products.erase(std::unique(inputs.Products.begin(), inputs.Products.end()), inputs.Products.end());
        return inputs;
    }
    std::variant<SourceInventory, AssetError> RagdollImporter::Inspect(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto error = std::get_if<AssetError>(&data))
            return *error;
        return SourceInventory {};
    }
    std::variant<ImportResult, AssetError> RagdollImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Ragdoll import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument, "Ragdoll staging must be empty and exclusive"};
            auto data = Read(c);
            if (auto error = std::get_if<AssetError>(&data))
                return *error;
            if (!std::get<RagdollData>(data).Skeleton.IsValid())
                return AssetError {AssetErrorCode::InvalidData, "Ragdoll skeleton ID must be valid"};
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto error = std::get_if<AssetError>(&ids))
                return *error;
            ImportResult out;
            out.LocalIDs                       = std::get<LocalIDMap>(ids);
            const auto                   local = out.LocalIDs.at(RootObjectKey);
            const auto                   text  = YAML::Dump(EncodeRagdollNode(std::get<RagdollData>(data)));
            const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                               reinterpret_cast<const std::byte*>(text.data() + text.size()));
            out.Products.push_back({local,
                                    AssetType::Ragdoll,
                                    std::to_string(local) + ".kragdoll",
                                    {std::get<RagdollData>(data).Skeleton}});
            for (const auto& body : std::get<RagdollData>(data).Bodies)
                if (body.Material.IsValid())
                    out.Products[0].Dependencies.push_back(body.Material);
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
