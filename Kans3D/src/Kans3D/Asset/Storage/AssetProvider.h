#pragma once
#include "ProductFiles.h"

namespace Kans
{
    class IAssetProvider
    {
    public:
        virtual ~IAssetProvider() = default;
        // Providers own the returned bytes and must support simultaneous reads.
        [[nodiscard]] virtual std::variant<std::vector<std::byte>, AssetError>
        Read(const ProductRecord& record) const = 0;
        // Source-aware providers may rebuild once after an engine ABI mismatch.
        // Returns the newly published bytes; read-only packages retain the original failure.
        virtual std::variant<std::vector<std::byte>, AssetError> RebuildStale(const ProductRecord&) const
        {
            return AssetError {AssetErrorCode::NotImplemented, "Provider has no source importer"};
        }
    };

    class LooseFileAssetProvider final : public IAssetProvider
    {
    public:
        [[nodiscard]] static std::variant<Ref<LooseFileAssetProvider>, AssetError>
                                                                       Open(const std::filesystem::path& root);
        [[nodiscard]] std::variant<std::vector<std::byte>, AssetError> Read(const ProductRecord& record) const override;
        const std::filesystem::path&                                   GetRoot() const { return m_Root; }

    private:
        explicit LooseFileAssetProvider(std::filesystem::path root) : m_Root(std::move(root)) {}
        std::filesystem::path m_Root;
    };
} // namespace Kans
