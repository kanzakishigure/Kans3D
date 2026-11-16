#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    enum class RenderTextureFormat
    {
        RGBA8SRGB,
        RGBA16F
    };
    enum class RenderTextureUpdate
    {
        OnLoad,
        Realtime,
        OnDemand
    };
    // A texture capability: products and handles use AssetType::Texture, including all texture pickers.
    struct RenderTextureSpecification
    {
        AssetID              Material;
        uint32_t             Width = 512, Height = 256;
        RenderTextureFormat  Format = RenderTextureFormat::RGBA8SRGB;
        bool                 Mips   = true;
        RenderTextureUpdate  Update = RenderTextureUpdate::Realtime;
        double               Rate   = 0;
        std::array<float, 4> ClearColor {0, 0, 0, 0};
    };
    std::variant<RenderTextureSpecification, AssetError> ParseRenderTexture(const std::string& text);
    std::variant<std::string, AssetError>    SerializeRenderTexture(const RenderTextureSpecification& specification);
    std::variant<std::monostate, AssetError> ValidateRenderTexture(const RenderTextureSpecification& specification);

    // Scheduling is committed only after a successful render. Requests coalesce and survive an unused frame.
    struct RenderTextureSchedule
    {
        uint64_t LastFrame = UINT64_MAX, LastRequest = 0, Updates = 0;
        double   LastTime = 0;
        bool     Due(const RenderTextureSpecification&, uint64_t frame, double time, uint64_t requests) const;
        void     Complete(uint64_t frame, double time, uint64_t requests);
    };
} // namespace Kans
