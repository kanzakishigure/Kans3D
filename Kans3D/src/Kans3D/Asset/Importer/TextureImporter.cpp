#include "kspch.h"
#include "TextureImporter.h"
#include "Kans3D/Asset/Product/TextureProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "TextureSource.h"

namespace Kans
{

    std::variant<SourceInventory, AssetError> TextureImporter::Inspect(const ImportContext& context) const
    {
        return InspectTextureSource(context.SourceFile);
    }

    std::variant<ImportResult, AssetError> TextureImporter::Import(const ImportContext& context) const
    {
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() || !context.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Texture import context"};
        auto settings = ParseTextureImportSettings(context.Settings);
        if (const auto* error = std::get_if<AssetError>(&settings))
            return *error;
        auto allocated = AllocateLocalIDs(context.PreviousLocalIDs, {"texture:root"});
        if (const auto* error = std::get_if<AssetError>(&allocated))
            return *error;
        ImportResult result;
        result.LocalIDs      = std::get<LocalIDMap>(std::move(allocated));
        const uint64_t local = result.LocalIDs.at("texture:root");
        result.Products.push_back({local, AssetType::Texture, std::to_string(local) + ".ktex", {}});
        std::error_code error;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_empty(context.StagingDirectory, error) || error)
            return AssetError {AssetErrorCode::InvalidArgument, "Texture staging must be an empty exclusive directory"};
        auto parsed =
            ParseTextureSource(context.SourceFile, std::get<TextureImportSettings>(settings), &result.Warnings);
        if (const auto* failure = std::get_if<AssetError>(&parsed))
            return *failure;
        auto encoded = EncodeTextureProduct(std::get<TextureData>(parsed));
        if (const auto* failure = std::get_if<AssetError>(&encoded))
            return *failure;
        const auto written = WriteProductFile(context.StagingDirectory / result.Products[0].StagedFile,
                                              std::get<std::vector<std::byte>>(encoded));
        if (const auto* failure = std::get_if<AssetError>(&written))
            return *failure;
        return result;
    }

} // namespace Kans
