#pragma once
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/RHI/RHI.h"

namespace Kans
{

    class Environment : public Asset
    {
    public:
        RenderHardware::Handle<RenderHardware::Texture> RadianceMap;
        RenderHardware::Handle<RenderHardware::Texture> IrradianceMap;

        Environment() = default;
        Environment(const RenderHardware::Handle<RenderHardware::Texture>& radianceMap,
                    const RenderHardware::Handle<RenderHardware::Texture>& irradianceMap) :
            RadianceMap(radianceMap), IrradianceMap(irradianceMap)
        {}

        static AssetType  GetStaticType() { return AssetType::EnvMap; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
    };

} // namespace Kans