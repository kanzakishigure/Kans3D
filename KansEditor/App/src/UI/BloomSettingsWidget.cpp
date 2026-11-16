#include "BloomSettingsWidget.h"
#include <imgui.h>
#include <cmath>
#include <string>
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    bool DrawBloomSettings(BloomSettings& settings, const std::function<bool(AssetID&)>& dirtPicker)
    {
        static const BloomSettings defaults;
        bool changed = false, toggled = false;
        // The heading's toggle turns bloom on; the common controls stay right under it.
        if (PropertyGrid::BeginGroup("Bloom", &settings.Enabled, &toggled))
        {
            const char* modes[] = {"Additive", "Energy conserving"};
            int         mode    = int(settings.Mode);
            const int   modeDefault = int(defaults.Mode);
            if (PropertyGrid::Combo("Mode", modes, 2, mode, &modeDefault))
            {
                settings.Mode = BloomSettings::Composite(mode);
                changed       = true;
            }
            const float maxIntensity = settings.Mode == BloomSettings::EnergyConserving ? 1.0f : 128.0f;
            changed |= PropertyGrid::Float("Intensity", settings.Intensity, 0.01f, 0.0f, maxIntensity, "%.3f", &defaults.Intensity);
            changed |= PropertyGrid::Float("Threshold", settings.Threshold, 0.01f, 0.0f, 65472.0f, "%.3f", &defaults.Threshold);
            changed |= PropertyGrid::Color3("Tint", settings.Tint, &defaults.Tint);
            if (dirtPicker)
            {
                changed |= dirtPicker(settings.DirtTexture);
                if (settings.DirtTexture.IsValid())
                    changed |= PropertyGrid::Float("Dirt intensity", settings.DirtIntensity, 0.01f, 0.0f, 128.0f, "%.3f",
                                                   &defaults.DirtIntensity);
            }
            PropertyGrid::EndGroup();
        }
        // Fine-tuning sits directly after Bloom, folded, rather than at the bottom of the card.
        if (settings.Enabled && PropertyGrid::BeginGroup("Bloom: Advanced", nullptr, nullptr, false))
        {
            changed |= PropertyGrid::Float("Knee", settings.Knee, 0.01f, 0.0f, 1.0f, "%.3f", &defaults.Knee);
            changed |= PropertyGrid::Float("Scatter", settings.Scatter, 0.01f, 0.0f, 1.0f, "%.3f", &defaults.Scatter);
            changed |= PropertyGrid::Float("Clamp", settings.Clamp, 10.0f, 0.0f, 65472.0f, "%.0f", &defaults.Clamp);
            changed |= PropertyGrid::Checkbox("High quality", settings.HighQualityFiltering, &defaults.HighQualityFiltering);
            PropertyGrid::EndGroup();
        }
        return changed || toggled;
    }
    namespace
    {
        // Entries run in list order within the stage their shader declares
        // (before or after tone mapping); surface materials are ignored by the renderer.
        bool DrawPostMaterials(std::vector<PostProcessMaterial>& materials, const AssetRowPicker& picker)
        {
            bool   changed = false;
            size_t erase = SIZE_MAX, up = SIZE_MAX;
            for (size_t i = 0; i < materials.size(); ++i)
            {
                ImGui::PushID(int(i));
                auto&             entry = materials[i];
                const std::string label = "Post material " + std::to_string(i + 1);
                changed |= picker(label.c_str(), entry.Material);
                if (!PropertyGrid::Row(""))
                {
                    ImGui::PopID();
                    continue;
                }
                changed |= ImGui::Checkbox("Enabled", &entry.Enabled);
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton(ICON_FA_ARROW_UP))
                    up = i;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == materials.size());
                if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
                    up = i + 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                    erase = i;
                ImGui::PopID();
            }
            if (up != SIZE_MAX)
            {
                std::swap(materials[up - 1], materials[up]);
                changed = true;
            }
            if (erase != SIZE_MAX)
            {
                materials.erase(materials.begin() + erase);
                changed = true;
            }
            AssetID added;
            if (picker("Add post material", added) && added.IsValid())
            {
                materials.push_back({added, true});
                changed = true;
            }
            return changed;
        }
    } // namespace
    bool DrawPostProcessSettings(PostProcessSettings&                 settings,
                                 const std::function<bool(AssetID&)>& dirtPicker,
                                 const AssetRowPicker&                materialPicker,
                                 bool                                 temporal,
                                 const AssetRowPicker&                lutPicker)
    {
        static const PostProcessSettings defaults;
        bool changed = false;
        if (PropertyGrid::BeginGroup("Exposure"))
        {
            const char* exposureModes[] = {"Manual", "Physical camera", "Automatic (histogram)"};
            int         mode            = int(settings.Exposure.Mode);
            if (PropertyGrid::Combo("Mode", exposureModes, 3, mode))
            {
                settings.Exposure.Mode = ExposureMode(mode);
                changed                = true;
            }
            auto& exposure = settings.Exposure;
            if (exposure.Mode == ExposureMode::Manual)
                changed |= PropertyGrid::Float(
                    "EV100", exposure.ManualEV100, .1f, -24, 32, "%.2f", &defaults.Exposure.ManualEV100);
            else if (exposure.Mode == ExposureMode::PhysicalCamera)
            {
                changed |= PropertyGrid::Float(
                    "Aperture", exposure.Camera.Aperture, .1f, .1f, 128, "f/%.2f", &defaults.Exposure.Camera.Aperture);
                changed |= PropertyGrid::Float("Shutter",
                                               exposure.Camera.ShutterSeconds,
                                               .001f,
                                               .000001f,
                                               3600,
                                               "%.6f s",
                                               &defaults.Exposure.Camera.ShutterSeconds);
                changed |= PropertyGrid::Float(
                    "ISO", exposure.Camera.ISO, 1, 1, 1000000, "%.0f", &defaults.Exposure.Camera.ISO);
            }
            if (exposure.Mode == ExposureMode::Automatic)
            {
                auto&       a = exposure.Automatic;
                const auto& d = defaults.Exposure.Automatic;
                changed |= PropertyGrid::Float(
                    "Histogram min EV", a.HistogramMinEV, .01f, -32.0f, 31.0f, "%.2f", &d.HistogramMinEV);
                changed |= PropertyGrid::Float(
                    "Histogram max EV", a.HistogramMaxEV, .01f, -31.0f, 64.0f, "%.2f", &d.HistogramMaxEV);
                changed |=
                    PropertyGrid::Float("Low percentile", a.LowPercentile, .01f, 0.0f, 0.99f, "%.2f", &d.LowPercentile);
                changed |= PropertyGrid::Float(
                    "High percentile", a.HighPercentile, .01f, 0.001f, 1.0f, "%.2f", &d.HighPercentile);
                changed |=
                    PropertyGrid::Float("Centre weight", a.CentreWeight, .01f, 0.0f, 1.0f, "%.2f", &d.CentreWeight);
                changed |= PropertyGrid::Float("Min EV", a.MinEV, .01f, -24.0f, 32.0f, "%.2f", &d.MinEV);
                changed |= PropertyGrid::Float("Max EV", a.MaxEV, .01f, -24.0f, 32.0f, "%.2f", &d.MaxEV);
                changed |= PropertyGrid::Float("Speed up", a.SpeedUp, .01f, 0.0f, 100.0f, "%.2f", &d.SpeedUp);
                changed |= PropertyGrid::Float("Speed down", a.SpeedDown, .01f, 0.0f, 100.0f, "%.2f", &d.SpeedDown);
            }
            changed |= PropertyGrid::Float(
                "Compensation", exposure.Compensation, .1f, -24, 24, "%.2f stops", &defaults.Exposure.Compensation);
            // Standard matches Blender's "Standard" view transform: exposure, then sRGB.
            const char* toneMappings[] = {
                "ACES", "Standard (none)", "AgX", "Khronos PBR Neutral", "AgX Punchy", "AgX Golden"};
            int         toneMapping    = int(settings.ToneMapping);
            const int   toneDefault    = int(defaults.ToneMapping);
            if (PropertyGrid::Combo("Tone mapping", toneMappings, 6, toneMapping, &toneDefault))
            {
                settings.ToneMapping = ToneMappingMode(toneMapping);
                changed              = true;
            }
            // Blackman-Harris reconstruction radius (Blender Film > Filter Size); 0 keeps the sharp resolve.
            changed |= PropertyGrid::Float("Pixel filter", settings.FilterSize, 0.05f, 0.0f, 4.0f, "%.2f px",
                                           &defaults.FilterSize);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Color Grading"))
        {
            auto&       g = settings.ColorGrading;
            const auto& d = defaults.ColorGrading;
            changed |= PropertyGrid::Float("Temperature", g.Temperature, 10, 1000, 40000, "%.0f K", &d.Temperature);
            changed |= PropertyGrid::Float("Tint", g.Tint, .01f, -1, 1, "%.2f", &d.Tint);
            changed |= PropertyGrid::Float("Contrast", g.Contrast, .01f, 0, 4, "%.2f", &d.Contrast);
            changed |= PropertyGrid::Float("Saturation", g.Saturation, .01f, 0, 4, "%.2f", &d.Saturation);
            changed |= PropertyGrid::Color3("Lift", g.Lift, &d.Lift);
            changed |= PropertyGrid::Color3("Gamma", g.Gamma, &d.Gamma);
            changed |= PropertyGrid::Color3("Gain", g.Gain, &d.Gain);
            if (lutPicker)
                changed |= lutPicker("LUT (.cube, log2 -12..12)", g.LUT);
            changed |= PropertyGrid::Float("Contribution", g.Contribution, .01f, 0, 1, "%.2f", &d.Contribution);
            PropertyGrid::EndGroup();
        }
        // Used by cameras set to TAA (FSR 3.1); other modes ignore it.
        if (temporal && PropertyGrid::BeginGroup("Temporal anti-aliasing"))
        {
            changed |= DrawTemporalSettings(settings.Temporal);
            PropertyGrid::EndGroup();
        }
        changed |= DrawBloomSettings(settings.Bloom, dirtPicker);
        if (materialPicker && PropertyGrid::BeginGroup("Post materials"))
        {
            changed |= DrawPostMaterials(settings.Materials, materialPicker);
            PropertyGrid::EndGroup();
        }
        return changed;
    }
    const char* const TemporalOnlyTooltip = "Upscaling and sharpening are part of TAA (FSR 3.1)";

    namespace
    {
        constexpr const char* AntiAliasingNames[] =
            {"Off", "FXAA (fast)", "MSAA 2x", "MSAA 4x", "MSAA 8x", "TAA (FSR 3.1)"};

        // A float row whose label explains the setting on hover; true when it changed.
        bool ExplainedFloat(const char* label,
                            const char* tooltip,
                            float&      value,
                            float       speed,
                            float       min,
                            float       max,
                            const char* format,
                            float       reset)
        {
            const bool modified = std::abs(value - reset) > 1e-6f;
            if (!PropertyGrid::Row(label, tooltip, modified))
                return false;
            ImGui::PushID(label);
            bool changed = ImGui::DragFloat("##value", &value, speed, min, max, format);
            if (PropertyGrid::RowReset(modified))
            {
                value   = reset;
                changed = true;
            }
            ImGui::PopID();
            return changed;
        }

        bool DrawUpscaling(UpscalingSettings& settings)
        {
            static const UpscalingSettings defaults;
            const char*                    names[]      = {"Native (1x)",
                                                           "Quality (1.5x)",
                                                           "Balanced (1.7x)",
                                                           "Performance (2x)",
                                                           "Ultra Performance (3x)",
                                                           "Custom"};
            int                            index        = int(settings.Mode);
            const int                      defaultIndex = int(defaults.Mode);
            bool changed  = PropertyGrid::Combo("Upscaling", names, int(std::size(names)), index, &defaultIndex);
            settings.Mode = UpscalingMode(index);
            if (settings.Mode == UpscalingMode::Custom)
                changed |= ExplainedFloat("Upscale ratio",
                                          "Display size divided by render size (1 to 3)",
                                          settings.CustomRatio,
                                          .01f,
                                          1.f,
                                          3.f,
                                          "%.2fx",
                                          defaults.CustomRatio);
            return changed;
        }

        bool DrawSharpness(float& sharpness)
        {
            static const TemporalReconstructionSettings defaults;
            return ExplainedFloat("Sharpness",
                                  "RCAS sharpening after reconstruction; 0 turns it off",
                                  sharpness,
                                  .01f,
                                  0.f,
                                  1.f,
                                  "%.2f",
                                  defaults.Sharpness);
        }

        // Reactive-mask generation (FSR 3.1 SDK defaults); folded, since the defaults suit most scenes.
        bool DrawTemporalAdvanced(TemporalReconstructionSettings& temporal)
        {
            static const TemporalReconstructionSettings defaults;
            if (!PropertyGrid::BeginGroup("Advanced", nullptr, nullptr, false))
                return false;
            bool changed = ExplainedFloat("Reactive scale",
                                          "How strongly colour changes after the opaque pass (transparency, "
                                          "particles) reduce the history used",
                                          temporal.ReactiveScale,
                                          .01f,
                                          0.f,
                                          16.f,
                                          "%.2f",
                                          defaults.ReactiveScale);
            changed |= ExplainedFloat("Reactive threshold",
                                      "Changes below this are ignored when building the reactive mask",
                                      temporal.ReactiveThreshold,
                                      .01f,
                                      0.f,
                                      1.f,
                                      "%.2f",
                                      defaults.ReactiveThreshold);
            changed |= ExplainedFloat("Reactive value",
                                      "Reactivity written where a change passes the threshold (the binary value)",
                                      temporal.ReactiveBinaryValue,
                                      .01f,
                                      0.f,
                                      1.f,
                                      "%.2f",
                                      defaults.ReactiveBinaryValue);
            PropertyGrid::EndGroup();
            return changed;
        }

        std::string Size(glm::uvec2 size) { return std::to_string(size.x) + "×" + std::to_string(size.y); }
    } // namespace

    const char* AntiAliasingName(AntiAliasing mode)
    {
        const auto index = size_t(mode);
        return index < std::size(AntiAliasingNames) ? AntiAliasingNames[index] : "Unknown";
    }

    bool AntiAliasingUsesTemporalRows(AntiAliasing mode) { return mode == AntiAliasing::TAA; }

    std::string RenderResolutionText(glm::uvec2 render, glm::uvec2 display)
    {
        if (!render.x || !render.y || !display.x || !display.y)
            return {};
        if (render == display)
            return "Render " + Size(render) + " (native)";
        return "Render " + Size(render) + " → Display " + Size(display);
    }

    bool DrawTemporalSettings(TemporalReconstructionSettings& temporal)
    {
        bool changed = DrawSharpness(temporal.Sharpness);
        changed |= DrawTemporalAdvanced(temporal);
        return changed;
    }

    AntiAliasingEdits DrawAntiAliasingGroup(AntiAliasing&                   mode,
                                            UpscalingSettings&              upscaling,
                                            TemporalReconstructionSettings& temporal,
                                            const AntiAliasingView*         view,
                                            const AntiAliasing*             defaultMode)
    {
        AntiAliasingEdits edits;
        if (!PropertyGrid::BeginGroup("Anti-aliasing"))
            return edits;
        int       index        = int(mode);
        const int defaultIndex = defaultMode ? int(*defaultMode) : 0;
        if (PropertyGrid::Combo("Mode",
                                AntiAliasingNames,
                                int(std::size(AntiAliasingNames)),
                                index,
                                defaultMode ? &defaultIndex : nullptr))
        {
            mode       = AntiAliasing(index);
            edits.Mode = true;
        }
        // Other modes keep the TAA rows visible but disabled, so the options stay discoverable.
        const bool   temporalRows = AntiAliasingUsesTemporalRows(mode);
        const ImVec2 top          = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!temporalRows);
        edits.Upscaling = DrawUpscaling(upscaling);
        if (temporalRows && view)
            if (const auto text = RenderResolutionText(view->RenderSize, view->DisplaySize); !text.empty())
                PropertyGrid::Text("Resolution", text, true);
        edits.Temporal = DrawSharpness(temporal.Sharpness);
        ImGui::EndDisabled();
        if (!temporalRows)
        {
            const ImVec2 bottom {top.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y};
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(top, bottom))
                ImGui::SetTooltip("%s", TemporalOnlyTooltip);
        }
        else
            edits.Temporal |= DrawTemporalAdvanced(temporal);
        PropertyGrid::EndGroup();
        return edits;
    }
} // namespace Kans
