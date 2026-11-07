#include "kspch.h"
#include "UIImporter.h"
#include "Kans3D/Asset/Product/UIProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/UI/UIInstance.h"
namespace Kans
{
    namespace
    {
        std::variant<UIData, AssetError> Read(const ImportContext& c)
        {
            auto b = ReadProductFile(c.SourceFile, 16 * 1024 * 1024);
            if (auto e = std::get_if<AssetError>(&b))
                return *e;
            auto& v = std::get<std::vector<std::byte>>(b);
            return ParseUISource(v.data(), v.size());
        }
    } // namespace
    std::variant<SourceInventory, AssetError> UIImporter::Inspect(const ImportContext& c) const
    {
        auto r = Read(c);
        if (auto e = std::get_if<AssetError>(&r))
            return *e;
        auto&           d = std::get<UIData>(r);
        SourceInventory out;
        out.UI.emplace();
        std::function<void(const std::vector<UIWidget>&)> count = [&](const auto& nodes) {
            for (auto& n : nodes)
            {
                ++out.UI->WidgetCount;
                count(n.Children);
            }
        };
        count(d.Widgets);
        for (auto& [id, type] : UIDependencies(d))
            out.UI->ReferencedAssets.push_back(id);
        return out;
    }
    std::variant<ImportInputs, AssetError> UIImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto r = Read(c);
        if (auto e = std::get_if<AssetError>(&r))
            return *e;
        ImportInputs inputs;
        for (auto& [id, type] : UIDependencies(std::get<UIData>(r)))
        {
            if (id.GetSourceID() == c.SourceID)
                return AssetError {AssetErrorCode::InvalidData, "UI instance cycle: source references itself"};
            // Overrides may replace an exposed asset property. Its dependency closure
            // is needed during expansion before the exposed target can be resolved.
            inputs.ProductsWithDependencies[id] = type;
        }
        return inputs;
    }
    std::variant<ImportResult, AssetError> UIImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute() ||
            !c.Settings.empty())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid UI import context/settings"};
        try
        {
            std::error_code ec;
            auto            status = std::filesystem::symlink_status(c.StagingDirectory, ec);
            if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
                return AssetError {AssetErrorCode::InvalidArgument, "UI staging must be empty and exclusive"};
            auto r = Read(c);
            if (auto e = std::get_if<AssetError>(&r))
                return *e;
            auto d   = std::get<UIData>(std::move(r));
            auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {RootObjectKey});
            if (auto e = std::get_if<AssetError>(&ids))
                return *e;
            ImportResult out;
            out.LocalIDs                                = std::get<LocalIDMap>(ids);
            auto                                  local = out.LocalIDs.at(RootObjectKey);
            std::map<AssetID, Ref<const UIAsset>> nested;
            for (auto& [id, bytes] : c.ProductBytes)
            {
                auto decoded = DecodeUIProduct(bytes.data(), bytes.size());
                if (auto ui = std::get_if<UIData>(&decoded))
                {
                    auto a     = CreateRef<UIAsset>(std::move(*ui));
                    a->assetID = id;
                    nested[id] = a;
                }
            }
            auto root     = CreateRef<UIAsset>(d);
            root->assetID = {c.SourceID, local};
            UIInstance instance(root, [&](AssetID id) -> Ref<const UIAsset> {
                return id == root->assetID ? root : nested.count(id) ? nested.at(id) : Ref<const UIAsset>();
            });
            auto       expanded = instance.Refresh();
            if (auto e = std::get_if<AssetError>(&expanded))
                return *e;
            std::vector<AssetID> deps;
            for (auto& [id, type] : UIDependencies(d))
            {
                if (!c.ProductBytes.count(id))
                    return AssetError {AssetErrorCode::NotFound, "UI referenced Product unavailable: " + id.ToString()};
                deps.push_back(id);
            }
            auto bytes = EncodeUIProduct(d);
            if (auto e = std::get_if<AssetError>(&bytes))
                return *e;
            out.Products.push_back({local, AssetType::UI, std::to_string(local) + ".kuib", deps});
            auto written = WriteProductFile(c.StagingDirectory / out.Products[0].StagedFile,
                                            std::get<std::vector<std::byte>>(bytes));
            if (auto e = std::get_if<AssetError>(&written))
                return *e;
            return out;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, std::string("UI import: ") + e.what()};
        }
    }
} // namespace Kans
