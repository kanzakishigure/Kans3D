#include "kspch.h"
#include "RenderTextureImporter.h"
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Asset/Product/TextureProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
namespace Kans
{
    namespace
    {
        std::variant<RenderTextureSpecification, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 1024 * 1024);
            if (auto* e = std::get_if<AssetError>(&bytes))
                return *e;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            return ParseRenderTexture(std::string(reinterpret_cast<const char*>(b.data()), b.size()));
        }
    } // namespace
    std::variant<ImportInputs, AssetError> RenderTextureImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto read = Read(c);
        if (auto* e = std::get_if<AssetError>(&read))
            return *e;
        const auto   material = std::get<RenderTextureSpecification>(read).Material;
        ImportInputs out;
        if (material.IsValid())
        {
            if (material.GetSourceID() == c.SourceID)
                return AssetError {AssetErrorCode::InvalidData, "RenderTexture references itself"};
            out.Products.push_back(material);
        }
        return out;
    }
    std::variant<ImportResult, AssetError> RenderTextureImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid RenderTexture import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "RenderTexture staging must be empty and exclusive"};
            auto read = Read(c);
            if (auto* e = std::get_if<AssetError>(&read))
                return *e;
            TextureData data;
            data.RenderTexture = std::get<RenderTextureSpecification>(read);
            std::vector<AssetID> deps;
            if (data.RenderTexture->Material.IsValid())
            {
                const auto id    = data.RenderTexture->Material;
                const auto found = c.ProductBytes.find(id);
                if (found == c.ProductBytes.end())
                    return AssetError {AssetErrorCode::NotFound, "RenderTexture material unavailable"};
                auto decoded = DecodeMaterialProduct(found->second.data(), found->second.size());
                if (auto* e = std::get_if<AssetError>(&decoded))
                    return *e;
                deps.push_back(id);
            }
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto* e = std::get_if<AssetError>(&ids))
                return *e;
            ImportResult out;
            out.LocalIDs       = std::get<LocalIDMap>(ids);
            const auto local   = out.LocalIDs.at(RootObjectKey);
            auto       encoded = EncodeTextureProduct(data);
            if (auto* e = std::get_if<AssetError>(&encoded))
                return *e;
            out.Products.push_back({local, AssetType::Texture, std::to_string(local) + ".ktex", deps});
            auto written = WriteProductFile(c.StagingDirectory / out.Products[0].StagedFile,
                                            std::get<std::vector<std::byte>>(encoded));
            if (auto* e = std::get_if<AssetError>(&written))
                return *e;
            return out;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
