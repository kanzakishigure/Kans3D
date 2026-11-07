#include "kspch.h"
#include "ActionImporter.h"
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
namespace Kans
{
    namespace
    {
        std::variant<ActionData, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 1024 * 1024);
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            try
            {
                return DecodeActionNode(YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size())));
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
    } // namespace
    std::variant<ImportInputs, AssetError> ActionImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        ImportInputs inputs;
        inputs.ProductsWithDependencies.emplace(std::get<ActionData>(data).Clip, AssetType::AnimationClip);
        return inputs;
    }
    std::variant<SourceInventory, AssetError> ActionImporter::Inspect(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto error = std::get_if<AssetError>(&data))
            return *error;
        return SourceInventory {};
    }
    std::variant<ImportResult, AssetError> ActionImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Action import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument, "Action staging must be empty and exclusive"};
            auto data = Read(c);
            if (auto error = std::get_if<AssetError>(&data))
                return *error;
            const auto& action = std::get<ActionData>(data);
            const auto  found  = c.ProductBytes.find(action.Clip);
            if (found == c.ProductBytes.end())
                return AssetError {AssetErrorCode::NotFound, "Action: unknown clip " + action.Clip.ToString()};
            auto clip = DecodeAnimationClipProduct(found->second.data(), found->second.size());
            if (auto error = std::get_if<AssetError>(&clip))
                return AssetError {AssetErrorCode::InvalidData, "Action Clip: " + error->Message};
            auto valid = ValidateAction(action, &std::get<AnimationClipData>(clip));
            if (auto error = std::get_if<AssetError>(&valid))
                return *error;
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto error = std::get_if<AssetError>(&ids))
                return *error;
            ImportResult out;
            out.LocalIDs     = std::get<LocalIDMap>(ids);
            const auto local = out.LocalIDs.at(RootObjectKey);
            auto       bytes = EncodeActionProduct(std::get<ActionData>(data));
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            out.Products.push_back({local, AssetType::Action, std::to_string(local) + ".kacn", {action.Clip}});
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
