#pragma once
#include "VfxCompiler.h"
namespace Kans
{
    class VfxSystemAsset final : public Asset
    {
    public:
        explicit VfxSystemAsset(Vfx::CompiledSystem data) : m_Data(std::move(data)) {}
        static AssetType           GetStaticType() { return AssetType::VfxSystem; }
        AssetType                  GetAssetType() const override { return GetStaticType(); }
        const Vfx::CompiledSystem& GetData() const { return m_Data; }

    private:
        Vfx::CompiledSystem m_Data;
    };
} // namespace Kans
