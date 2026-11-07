#include "kspch.h"
#include "TimelineImporter.h"
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Timeline/TimelineProduct.h"
namespace Kans
{
    namespace
    {
        std::variant<TimelineData, AssetError> Read(const ImportContext& c)
        {
            auto bytes = ReadProductFile(c.SourceFile, 1024 * 1024);
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& b = std::get<std::vector<std::byte>>(bytes);
            try
            {
                return DecodeTimelineNode(YAML::Load(std::string(reinterpret_cast<const char*>(b.data()), b.size())));
            }
            catch (const std::exception& e)
            {
                return AssetError {AssetErrorCode::InvalidData, e.what()};
            }
        }
    } // namespace
    std::variant<ImportInputs, AssetError> TimelineImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto e = std::get_if<AssetError>(&data))
            return *e;
        ImportInputs inputs;
        const auto&  d = std::get<TimelineData>(data);
        for (const auto& b : d.Bindings)
        {
            if (b.Prefab.IsValid())
                inputs.ProductsWithDependencies.emplace(b.Prefab, AssetType::Model);
            if (b.Skeleton.IsValid())
                inputs.ProductsWithDependencies.emplace(b.Skeleton, AssetType::Skeleton);
            if (b.Particles)
                inputs.ProductsWithDependencies.emplace(b.Particles->System, AssetType::VfxSystem);
        }
        for (const auto& t : d.Animations)
            for (const auto& clip : t.Clips)
                inputs.ProductsWithDependencies.emplace(clip.Clip, AssetType::AnimationClip);
        return inputs;
    }
    std::variant<SourceInventory, AssetError> TimelineImporter::Inspect(const ImportContext& c) const
    {
        auto data = Read(c);
        if (auto error = std::get_if<AssetError>(&data))
            return *error;
        return SourceInventory {};
    }
    std::variant<ImportResult, AssetError> TimelineImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty() || !c.SourceInputs.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Timeline import context"};
        try
        {
            std::error_code ec;
            const auto      status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument, "Timeline staging must be empty and exclusive"};
            auto data = Read(c);
            if (auto error = std::get_if<AssetError>(&data))
                return *error;
            auto inputs = DiscoverInputs(c);
            if (auto error = std::get_if<AssetError>(&inputs))
                return *error;
            for (const auto& [id, type] : std::get<ImportInputs>(inputs).ProductsWithDependencies)
            {
                auto found = c.ProductBytes.find(id);
                if (found == c.ProductBytes.end())
                    return AssetError {AssetErrorCode::NotFound, "Timeline dependency missing: " + id.ToString()};
                if (type == AssetType::AnimationClip)
                {
                    auto clip = DecodeAnimationClipProduct(found->second.data(), found->second.size());
                    if (auto error = std::get_if<AssetError>(&clip))
                        return *error;
                    for (const auto& track : std::get<TimelineData>(data).Animations)
                        for (const auto& section : track.Clips)
                            if (section.Clip == id && (section.ClipIn >= std::get<AnimationClipData>(clip).Duration ||
                                                       section.ClipOut > std::get<AnimationClipData>(clip).Duration))
                                return AssetError {AssetErrorCode::InvalidData, "Timeline clip trim exceeds duration"};
                }
            }
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto error = std::get_if<AssetError>(&ids))
                return *error;
            ImportResult out;
            out.LocalIDs     = std::get<LocalIDMap>(ids);
            const auto local = out.LocalIDs.at(RootObjectKey);
            auto       bytes = EncodeTimelineProduct(std::get<TimelineData>(data));
            if (auto error = std::get_if<AssetError>(&bytes))
                return *error;
            out.Products.push_back({local, AssetType::Timeline, std::to_string(local) + ".ktln", {}});
            for (const auto& [id, type] : std::get<ImportInputs>(inputs).ProductsWithDependencies)
                out.Products[0].Dependencies.push_back(id);
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
