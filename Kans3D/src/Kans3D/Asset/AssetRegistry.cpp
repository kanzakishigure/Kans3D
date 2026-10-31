#include "kspch.h"
#include "AssetRegistry.h"
#include <algorithm>
#include <unordered_set>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Product/ContentDigest.h"

namespace Kans
{
    bool IsProductTarget(const std::string& target)
    {
        return !target.empty() && target.size() <= 128 &&
               target.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") ==
                   std::string::npos;
    }
    namespace
    {
        constexpr size_t kMaximumRecords = 1000000;
        constexpr size_t kMaximumBytes   = 256 * 1024 * 1024;
        bool             ValidRecord(const ProductRecord& record)
        {
            const auto type = static_cast<uint16_t>(record.Metadata.Type);
            if (!record.Metadata.IsValid() || type > static_cast<uint16_t>(AssetType::Ragdoll) ||
                !IsProductTarget(record.Target) || !record.Location.RelativeFile.IsValid() ||
                record.Location.RelativeFile.ToString().size() > 4096 || !record.Location.ByteSize ||
                !IsContentDigest(record.Location.ContentDigest) ||
                record.Metadata.Dependencies.size() > kMaximumRecords)
                return false;
            std::unordered_set<AssetID> dependencies;
            for (const auto& id : record.Metadata.Dependencies)
                if (!id.IsValid() || id == record.Metadata.assetID || !dependencies.insert(id).second)
                    return false;
            return true;
        }
    } // namespace
    std::optional<ProductRecord> AssetRegistry::Find(AssetID id, const std::string& target) const
    {
        const auto asset = m_Products.find(id);
        if (asset == m_Products.end())
            return std::nullopt;
        const auto variant = asset->second.find(target);
        return variant == asset->second.end() ? std::nullopt : std::optional<ProductRecord>(variant->second);
    }
    Ref<AssetMetadata> AssetRegistry::GetAssetMetadata(AssetID id) const
    {
        const auto record = Find(id);
        return record ? CreateRef<AssetMetadata>(record->Metadata) : nullptr;
    }
    std::vector<ProductRecord> AssetRegistry::GetAllProducts() const
    {
        std::vector<ProductRecord> result;
        for (const auto& entry : m_Products)
            for (const auto& variant : entry.second)
                result.push_back(variant.second);
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.Metadata.assetID == b.Metadata.assetID ?
                       a.Target < b.Target :
                       a.Metadata.assetID.ToString() < b.Metadata.assetID.ToString();
        });
        return result;
    }
    std::variant<AssetRegistry, AssetError> AssetRegistry::FromProducts(const std::vector<ProductRecord>& products)
    {
        if (products.size() > kMaximumRecords)
            return AssetError {AssetErrorCode::InvalidData, "Too many Registry Products"};
        AssetRegistry candidate;
        for (const auto& record : products)
        {
            if (!ValidRecord(record))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Registry Product record"};
            auto& variants = candidate.m_Products[record.Metadata.assetID];
            if (!variants.empty() && variants.begin()->second.Metadata.Type != record.Metadata.Type)
                return AssetError {AssetErrorCode::InvalidData, "Asset type differs between target variants"};
            if (!variants.emplace(record.Target, record).second)
                return AssetError {AssetErrorCode::DuplicateID, "Duplicate Registry AssetID/target"};
        }
        return candidate;
    }
    std::variant<std::monostate, AssetError>
    AssetRegistry::ReplaceSourceProducts(SourceAssetID                     source,
                                         const std::string&                target,
                                         const std::vector<ProductRecord>& products)
    {
        if (!source.IsValid() || !IsProductTarget(target))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Registry source/target replacement"};
        for (const auto& record : products)
            if (record.Metadata.assetID.GetSourceID() != source || record.Target != target)
                return AssetError {AssetErrorCode::InvalidArgument, "Product belongs to another source/target"};
        auto validated = FromProducts(products);
        if (const auto* error = std::get_if<AssetError>(&validated))
            return *error;
        auto all = GetAllProducts();
        all.erase(std::remove_if(all.begin(),
                                 all.end(),
                                 [&](const auto& record) {
                                     return record.Metadata.assetID.GetSourceID() == source && record.Target == target;
                                 }),
                  all.end());
        all.insert(all.end(), products.begin(), products.end());
        auto replacement = FromProducts(all);
        if (const auto* error = std::get_if<AssetError>(&replacement))
            return *error;
        Swap(std::get<AssetRegistry>(replacement));
        return std::monostate {};
    }
    std::variant<std::string, AssetError> EncodeAssetRegistry(const AssetRegistry& registry)
    {
        YAML::Emitter out;
        out << YAML::BeginMap << YAML::Key << "version" << YAML::Value << 1 << YAML::Key << "products" << YAML::Value
            << YAML::BeginSeq;
        for (const auto& record : registry.GetAllProducts())
        {
            if (!ValidRecord(record))
                return AssetError {AssetErrorCode::InvalidData, "Invalid Registry Product"};
            out << YAML::BeginMap << YAML::Key << "id" << YAML::Value << record.Metadata.assetID.ToString() << YAML::Key
                << "type" << YAML::Value << static_cast<uint16_t>(record.Metadata.Type) << YAML::Key << "target"
                << YAML::Value << record.Target << YAML::Key << "path" << YAML::Value
                << record.Location.RelativeFile.ToString() << YAML::Key << "size" << YAML::Value
                << record.Location.ByteSize << YAML::Key << "digest" << YAML::Value << record.Location.ContentDigest
                << YAML::Key << "dependencies" << YAML::Value << YAML::BeginSeq;
            for (const auto& dependency : record.Metadata.Dependencies)
                out << dependency.ToString();
            out << YAML::EndSeq << YAML::EndMap;
        }
        out << YAML::EndSeq << YAML::EndMap;
        if (!out.good() || out.size() > kMaximumBytes)
            return AssetError {AssetErrorCode::InvalidData, "Cannot encode Registry within size limit"};
        return std::string(out.c_str(), out.size());
    }
    std::variant<AssetRegistry, AssetError> DecodeAssetRegistry(const std::string& bytes)
    {
        if (bytes.empty() || bytes.size() > kMaximumBytes)
            return AssetError {AssetErrorCode::InvalidData, "Invalid Registry byte length"};
        try
        {
            const auto documents = YAML::LoadAll(bytes);
            if (documents.size() != 1 || !documents[0].IsMap() || documents[0].size() != 2)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Registry root"};
            const auto& root = documents[0];
            if (root["version"].as<uint32_t>() != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Unsupported Registry version"};
            if (!root["products"].IsSequence() || root["products"].size() > kMaximumRecords)
                return AssetError {AssetErrorCode::InvalidData, "Invalid Registry Product list"};
            std::vector<ProductRecord> records;
            for (const auto& input : root["products"])
            {
                if (!input.IsMap() || input.size() != 7 || !input["dependencies"].IsSequence() ||
                    input["dependencies"].size() > kMaximumRecords)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Registry Product fields"};
                ProductRecord record;
                record.Metadata.assetID      = AssetID::FromString(input["id"].as<std::string>());
                record.Metadata.Type         = static_cast<AssetType>(input["type"].as<uint16_t>());
                record.Target                = input["target"].as<std::string>();
                record.Location.RelativeFile = AssetPath(std::filesystem::u8path(input["path"].as<std::string>()));
                const auto size              = input["size"].as<std::string>();
                if (size.empty() || size.find_first_not_of("0123456789") != std::string::npos)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Registry Product size"};
                record.Location.ByteSize      = input["size"].as<uint64_t>();
                record.Location.ContentDigest = input["digest"].as<std::string>();
                for (const auto& dependency : input["dependencies"])
                    record.Metadata.Dependencies.push_back(AssetID::FromString(dependency.as<std::string>()));
                records.push_back(std::move(record));
            }
            return AssetRegistry::FromProducts(records);
        }
        catch (const YAML::Exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, "Cannot decode Registry: " + std::string(error.what())};
        }
    }
    bool SameProductContent(const ProductRecord& a, const ProductRecord& b)
    {
        if (a.Metadata.assetID != b.Metadata.assetID || a.Metadata.Type != b.Metadata.Type || a.Target != b.Target ||
            a.Location.ByteSize != b.Location.ByteSize || a.Location.ContentDigest != b.Location.ContentDigest ||
            a.Metadata.Dependencies.size() != b.Metadata.Dependencies.size())
            return false;
        const std::unordered_set<AssetID> inputs(a.Metadata.Dependencies.begin(), a.Metadata.Dependencies.end());
        if (inputs.size() != a.Metadata.Dependencies.size())
            return false;
        std::unordered_set<AssetID> checked;
        for (const auto dependency : b.Metadata.Dependencies)
            if (!inputs.count(dependency) || !checked.insert(dependency).second)
                return false;
        return true;
    }

    std::variant<ProductChangeSet, AssetError>
    CompareAssetRegistries(const AssetRegistry& previous, const AssetRegistry& current, const std::string& target)
    {
        if (!IsProductTarget(target))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Product comparison target"};
        ProductChangeSet result;
        for (const auto& record : previous.GetAllProducts())
            if (record.Target == target)
            {
                const auto next = current.Find(record.Metadata.assetID, target);
                if (!next)
                    result.Removed.push_back(record.Metadata.assetID);
                else if (!SameProductContent(record, *next))
                    result.Modified.push_back(record.Metadata.assetID);
            }
        for (const auto& record : current.GetAllProducts())
            if (record.Target == target && !previous.Find(record.Metadata.assetID, target))
                result.Added.push_back(record.Metadata.assetID);
        const auto order = [](AssetID a, AssetID b) {
            return a.GetSourceID() == b.GetSourceID() ? a.GetLocalID() < b.GetLocalID() :
                                                        a.GetSourceID().GetUUID() < b.GetSourceID().GetUUID();
        };
        std::sort(result.Added.begin(), result.Added.end(), order);
        std::sort(result.Modified.begin(), result.Modified.end(), order);
        std::sort(result.Removed.begin(), result.Removed.end(), order);
        return result;
    }
} // namespace Kans
