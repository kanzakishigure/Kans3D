#pragma once
#include <map>
#include <set>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "LocalIDMap.h"

namespace Kans
{
    class ImportContext
    {
    public:
        SourceAssetID         SourceID;
        // Absolute paths of the source file and the project asset root.
        std::filesystem::path SourceFile;
        std::filesystem::path AssetRoot;
        std::string           Target = "default";
        // Exclusive empty directory for staged Product files. Unused by discovery.
        std::filesystem::path              StagingDirectory;
        std::map<std::string, std::string> Settings;
        // Object key -> LocalID from previous imports, including removed objects.
        // LocalIDs are persistent identity; never derive them from array order.
        LocalIDMap PreviousLocalIDs;

        // Inputs resolved by the build coordinator from DiscoverInputs. Paths are
        // absolute and lexically normal.
        // Other source files the importer may read.
        std::map<std::filesystem::path, SourceAssetID> SourceInputs;
        // Source file -> its primary Product, for references made by path (model textures).
        // An invalid ID explicitly marks an unavailable optional source.
        std::map<std::filesystem::path, AssetID> ProductsBySource;
        // Published bytes of referenced Products, supplied only when RequiresProductBytes().
        std::map<AssetID, std::vector<std::byte>> ProductBytes;
    };

    // What an import reads besides its own source, reported before importing.
    struct ImportInputs
    {
        // Other source files read while importing (.mtl, glTF buffers, shader includes).
        std::vector<std::filesystem::path> SourceFiles;
        // Engine-owned source roots carry stable IDs and participate in source fingerprints.
        std::map<std::filesystem::path, SourceAssetID> BuiltinSourceFiles;
        // Source files whose Products are referenced by path (model textures).
        std::vector<std::filesystem::path> ProductSources;
        // Subset of ProductSources: build failures warn and resolve to an invalid
        // Product ID, leaving the importer free to omit the reference.
        std::set<std::filesystem::path> OptionalProductSources;
        // Import settings to give a referenced source that has none of its own,
        // e.g. {"usage": "normal"} for a texture used as a normal map.
        std::map<std::filesystem::path, std::map<std::string, std::string>> ProductSourceSettings;
        // Products referenced by ID (a Material's Shader and Textures).
        std::vector<AssetID> Products;
        // Required typed Products, including their published dependency closure.
        // Material instances need the parent's effective Shader and textures too.
        std::map<AssetID, AssetType> ProductsWithDependencies;
        // Typed, optional references: unavailable inputs warn and are omitted.
        std::map<AssetID, AssetType> OptionalProducts;
        // Problems that do not stop the import, such as a missing texture file.
        std::vector<std::string> Warnings;
    };

    class ProductDescriptor
    {
    public:
        uint64_t  LocalID = 0;
        AssetType Type    = AssetType::None;
        // Relative to StagingDirectory, not a source path or final ProductLocation.
        std::filesystem::path StagedFile;
        std::vector<AssetID>  Dependencies;
    };

    class ImportResult
    {
    public:
        // Complete product set, including an empty set if the source emits nothing.
        std::vector<ProductDescriptor> Products;
        // Complete mapping: previous entries plus newly allocated keys.
        LocalIDMap LocalIDs;
        // Recoverable problems found while decoding inputs or emitting Products.
        std::vector<std::string> Warnings;
    };
} // namespace Kans
