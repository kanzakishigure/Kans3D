#pragma once
#include <functional>
#include <string>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Renderer/Passes/AntiAliasing.h"
#include "Kans3D/Renderer/Passes/BloomSettings.h"
#include "Kans3D/Renderer/Passes/PostProcessSettings.h"
#include "Kans3D/Renderer/Passes/Upscaling.h"

namespace Kans
{
    // Bloom groups (the heading toggles bloom; advanced settings fold below it). `dirtPicker` draws the lens dirt texture
    // row and returns true when the texture changed; without it that row is omitted.
    // Returns true when any setting changed (the caller stores the validated result).
    bool DrawBloomSettings(BloomSettings& settings, const std::function<bool(AssetID&)>& dirtPicker = {});
    // Picker row for one asset; true when the selection changed.
    using AssetRowPicker = std::function<bool(const char* label, AssetID&)>;
    // Exposure, Bloom and Post materials groups inside a card; true when any changed.
    // `materialPicker` adds the ordered post-process material list (omitted without it).
    // `temporal` adds the TAA sharpness and reactive settings; a camera card leaves them out because its
    // Anti-aliasing group edits the same values.
    bool DrawPostProcessSettings(PostProcessSettings&                 settings,
                                 const std::function<bool(AssetID&)>& dirtPicker     = {},
                                 const AssetRowPicker&                materialPicker = {},
                                 bool                                 temporal       = true);

    // ---- Anti-aliasing (one group wherever a camera's mode is edited) ----

    // The mode's display name, as listed in the Mode combo ("TAA (FSR 3.1)").
    const char* AntiAliasingName(AntiAliasing mode);
    // Upscaling and sharpening only exist under TAA; other modes show those rows disabled.
    bool AntiAliasingUsesTemporalRows(AntiAliasing mode);
    // Why the TAA-only rows are disabled (their tooltip).
    extern const char* const TemporalOnlyTooltip;
    // "Render 1280×720 → Display 2560×1440", or "Render 1920×1080 (native)" when the sizes match;
    // empty while either size is unknown (no frame rendered yet).
    std::string RenderResolutionText(glm::uvec2 render, glm::uvec2 display);
    // A view's last frame sizes, for the read-only resolution line.
    struct AntiAliasingView
    {
        glm::uvec2 RenderSize {}, DisplaySize {};
    };
    // What a DrawAntiAliasingGroup call changed; the caller stores each part where it lives.
    struct AntiAliasingEdits
    {
        bool Mode = false, Upscaling = false, Temporal = false;
        bool Any() const { return Mode || Upscaling || Temporal; }
    };
    // The "Anti-aliasing" group: Mode, then (TAA only) Upscaling, the view's render and display size,
    // Sharpness and a folded Advanced group with the reactive-mask settings. `defaultMode` adds the
    // mode's reset button.
    AntiAliasingEdits DrawAntiAliasingGroup(AntiAliasing&                   mode,
                                            UpscalingSettings&              upscaling,
                                            TemporalReconstructionSettings& temporal,
                                            const AntiAliasingView*         view        = nullptr,
                                            const AntiAliasing*             defaultMode = nullptr);
    // Sharpness and the folded Advanced group (reactive scale, threshold, binary value).
    bool DrawTemporalSettings(TemporalReconstructionSettings& temporal);
} // namespace Kans
