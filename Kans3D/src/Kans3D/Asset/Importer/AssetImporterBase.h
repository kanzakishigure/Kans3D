#pragma once
#include <variant>
#include "Kans3D/Asset/AssetError.h"
#include "ImportContext.h"
#include "ImportSettingsSchema.h"
#include "SourceInventory.h"

namespace Kans
{
    // Converts one source file into a complete set of staged Products.
    // Importers never create runtime assets or GPU resources and never modify
    // published Products; the build coordinator publishes the staged set.
    class AssetImporterBase
    {
    public:
        virtual ~AssetImporterBase()        = default;
        virtual uint32_t GetVersion() const = 0;
        // Engine-owned compiler inputs, independent of source settings and importer version.
        virtual std::string          GetEnvironmentFingerprint() const { return {}; }
        virtual ImportSettingsSchema GetSettingsSchema() const { return {}; }
        // Uses exactly the same parser as Import, without reading source bytes.
        [[nodiscard]] virtual std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>& settings) const
        {
            if (!settings.empty())
                return AssetError {AssetErrorCode::InvalidArgument, "This importer accepts no settings"};
            return std::monostate {};
        }
        // Source-only, independent invocation: never stages or publishes products.
        [[nodiscard]] virtual std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const
        {
            return AssetError {AssetErrorCode::NotImplemented, "Source inspection is not supported by this importer"};
        }
        // Whether Import needs ProductBytes for the referenced Products.
        virtual bool RequiresProductBytes() const { return false; }
        // Reports other inputs so the coordinator can build them first and track
        // them for rebuilds. StagingDirectory and resolved inputs are not set.
        [[nodiscard]] virtual std::variant<ImportInputs, AssetError> DiscoverInputs(const ImportContext&) const
        {
            return ImportInputs {};
        }
        // Reads only the source and the declared inputs, then writes Products to
        // the staging directory. Previous LocalIDs must be preserved.
        [[nodiscard]] virtual std::variant<ImportResult, AssetError> Import(const ImportContext& context) const = 0;
    };
} // namespace Kans
