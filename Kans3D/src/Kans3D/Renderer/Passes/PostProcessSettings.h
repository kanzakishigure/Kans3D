#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Renderer/Passes/AutoExposureSettings.h"
#include "Kans3D/Renderer/Passes/BloomSettings.h"
#include "Kans3D/Renderer/Passes/ColorGradingSettings.h"
namespace Kans
{
    enum class ToneMappingMode : uint32_t
    {
        ACES,
        Standard,
        AgX,
        PBRNeutral,
        AgXPunchy,
        AgXGolden
    };
    enum class ExposureMode : uint32_t
    {
        Manual,
        PhysicalCamera,
        Automatic
    };
    struct PhysicalCameraSettings
    {
        float Aperture = 16.f, ShutterSeconds = 1.f / 125.f, ISO = 100.f;
        // Lagarde/de Rousiers, Moving Frostbite to PBR (2014), section 5.1.
        float EV100() const { return std::log2(Aperture * Aperture / ShutterSeconds * 100.f / ISO); }
        bool  operator==(const PhysicalCameraSettings& o) const
        {
            return Aperture == o.Aperture && ShutterSeconds == o.ShutterSeconds && ISO == o.ISO;
        }
    };
    struct ExposureSettings
    {
        ExposureMode Mode = ExposureMode::Manual;
        // E = 1/(1.2*2^EV100). Legacy scalar s migrates to EV100=-log2(1.2*s).
        float                  ManualEV100 = -std::log2(1.2f * .8f), Compensation = 0.f;
        PhysicalCameraSettings Camera;
        AutoExposureSettings   Automatic;
        ExposureSettings() = default;
        explicit ExposureSettings(float legacy) { *this = legacy; }
        ExposureSettings& operator=(float legacy)
        {
            Mode         = ExposureMode::Manual;
            ManualEV100  = -std::log2(1.2f * (std::isfinite(legacy) ? std::clamp(legacy, .001f, 128.f) : .8f));
            Compensation = 0;
            return *this;
        }
        float EV100() const
        {
            return (Mode == ExposureMode::PhysicalCamera ? Camera.EV100() : ManualEV100) - Compensation;
        }
        float Multiplier() const
        {
            // Resolve the representable exposure once for all HDR consumers.
            return std::clamp(std::exp2(-EV100()) / 1.2f, 1e-10f, 1e8f);
        }
        // Compatibility with linear-exposure render consumers and legacy camera API.
                         operator float() const { return Multiplier(); }
        ExposureSettings Validated() const
        {
            auto r      = *this;
            auto finite = [](float v, float lo, float hi, float fallback) {
                return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
            };
            if (uint32_t(r.Mode) > uint32_t(ExposureMode::Automatic))
                r.Mode = ExposureMode::Manual;
            r.ManualEV100           = finite(r.ManualEV100, -24, 32, ExposureSettings {}.ManualEV100);
            r.Compensation          = finite(r.Compensation, -24, 24, 0);
            r.Camera.Aperture       = finite(r.Camera.Aperture, .1f, 128, 16);
            r.Camera.ShutterSeconds = finite(r.Camera.ShutterSeconds, .000001f, 3600, 1.f / 125);
            r.Camera.ISO            = finite(r.Camera.ISO, 1, 1000000, 100);
            r.Automatic             = Automatic.Validated();
            return r;
        }
        bool operator==(const ExposureSettings& o) const
        {
            return Automatic == o.Automatic && Mode == o.Mode && ManualEV100 == o.ManualEV100 &&
                   Compensation == o.Compensation && Camera == o.Camera;
        }
    };
    struct PostProcessMaterial
    {
        AssetID Material;
        bool    Enabled = true;
        bool    operator==(const PostProcessMaterial& other) const
        {
            return Material == other.Material && Enabled == other.Enabled;
        }
    };
    struct TemporalReconstructionSettings
    {
        float Sharpness = .3f;
        // SDK GenerateReactive sample defaults. Flags: tonemap | threshold | components max.
        float ReactiveScale = 1.f, ReactiveThreshold = .2f, ReactiveBinaryValue = .9f;
        bool  operator==(const TemporalReconstructionSettings& other) const
        {
            return Sharpness == other.Sharpness && ReactiveScale == other.ReactiveScale &&
                   ReactiveThreshold == other.ReactiveThreshold && ReactiveBinaryValue == other.ReactiveBinaryValue;
        }
        TemporalReconstructionSettings Validated() const
        {
            const auto finite = [](float value, float fallback, float maximum) {
                return std::isfinite(value) ? std::clamp(value, 0.f, maximum) : fallback;
            };
            return {finite(Sharpness, .3f, 1),
                    finite(ReactiveScale, 1, 16),
                    finite(ReactiveThreshold, .2f, 1),
                    finite(ReactiveBinaryValue, .9f, 1)};
        }
    };
    struct PostProcessSettings
    {
        ExposureSettings                 Exposure;
        ColorGradingSettings             ColorGrading;
        BloomSettings       Bloom;
        TemporalReconstructionSettings   Temporal;
        std::vector<PostProcessMaterial> Materials;
        ToneMappingMode     ToneMapping = ToneMappingMode::ACES;
        float FilterSize = 0; // Radial Blackman-Harris radius in output pixels; zero preserves legacy output.
        PostProcessSettings Validated() const
        {
            auto result     = *this;
            result.Exposure     = Exposure.Validated();
            result.Bloom    = Bloom.Validated();
            result.ColorGrading = ColorGrading.Validated();
            result.Temporal = Temporal.Validated();
            result.Materials.erase(std::remove_if(result.Materials.begin(),
                                                  result.Materials.end(),
                                                  [](const auto& entry) { return !entry.Material.IsValid(); }),
                                   result.Materials.end());
            result.FilterSize = std::isfinite(FilterSize) ? std::clamp(FilterSize, 0.f, 4.f) : 0.f;
            if (uint32_t(ToneMapping) > uint32_t(ToneMappingMode::AgXGolden))
                result.ToneMapping = ToneMappingMode::ACES;
            return result;
        }
        bool operator==(const PostProcessSettings& other) const
        {
            return ColorGrading == other.ColorGrading && Exposure == other.Exposure && Temporal == other.Temporal &&
                   Bloom == other.Bloom && ToneMapping == other.ToneMapping && FilterSize == other.FilterSize &&
                   Materials == other.Materials;
        }
        bool operator!=(const PostProcessSettings& other) const { return !(*this == other); }
    };
} // namespace Kans
