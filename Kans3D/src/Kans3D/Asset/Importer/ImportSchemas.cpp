#include "kspch.h"
#include "Kans3D/Audio/Import/AudioImportSettings.h"
#include "Kans3D/Audio/Import/AudioImporter.h"
#include "Kans3D/Text/Import/FontImporter.h"
#include "ModelImporter.h"
#include "TextureImportSettings.h"
#include "TextureImporter.h"

namespace Kans
{
    namespace
    {
        using T = ImportSettingDesc::Type;
        ImportSettingDesc Setting(std::string                                key,
                                  T                                          kind,
                                  std::string                                value,
                                  std::string                                label,
                                  std::string                                group,
                                  std::string                                description,
                                  std::initializer_list<ImportSettingOption> choices = {})
        {
            ImportSettingDesc s;
            s.Key         = std::move(key);
            s.Kind        = kind;
            s.Default     = std::move(value);
            s.Label       = std::move(label);
            s.Group       = std::move(group);
            s.Description = std::move(description);
            s.KeyPattern  = s.Key.find('<') != std::string::npos;
            s.Options.assign(choices.begin(), choices.end());
            return s;
        }
    } // namespace
    ImportSettingsSchema ModelImporter::GetSettingsSchema() const
    {
        ImportSettingsSchema schema;
        auto&                s = schema.Settings;
        s.push_back(Setting("scale", T::Float, "1", "Scale", "Mesh", "Positive uniform model scale."));
        s.back().Min  = 0;
        s.back().Step = .1;
        s.push_back(Setting("cloth.max_distance.color_set",
                            T::Int,
                            "-1",
                            "Max Distance colour set",
                            "Cloth",
                            "-1 disables import; 0..7 selects a vertex colour set."));
        s.push_back(Setting("cloth.max_distance.channel",
                            T::Enum,
                            "R",
                            "Max Distance channel",
                            "Cloth",
                            "Colour channel containing the baked weight.",
                            {{"R", "R", ""}, {"G", "G", ""}, {"B", "B", ""}, {"A", "A", ""}}));
        s.push_back(Setting(
            "cloth.max_distance.scale", T::Float, "1", "Max Distance scale", "Cloth", "Metres per colour unit."));
        s.push_back(Setting("mesh.smooth_normals",
                            T::Enum,
                            "false",
                            "Smooth normals",
                            "Mesh",
                            "Weld by position or preserve source control point identity.",
                            {{"false", "Off", "Use the source normals only."},
                             {"weld", "Weld positions", "One smooth normal per position; closes cracks at hard edges."},
                             {"source",
                              "Source vertices",
                              "One smooth normal per FBX control point; keeps overlapping shells apart. Other "
                              "formats weld."},
                             {"true", "Weld (legacy)", "Older spelling of Weld positions."}}));
        s.push_back(Setting("morph.recompute_normals",
                            T::Enum,
                            "auto",
                            "Recompute normals",
                            "Morph targets",
                            "Auto recomputes missing normals and unchanged FBX normals.",
                            {{"auto", "Auto", "Recompute when the source has no morph normals."},
                             {"true", "Always", "Recompute every morph target's normals."},
                             {"false", "Never", "Keep the source morph normals."}}));
        const std::pair<const char*, const char*> categories[] = {{"meshes", "Meshes"},
                                                                  {"animations", "Animations"},
                                                                  {"materials", "Materials"},
                                                                  {"skeleton", "Skeleton"},
                                                                  {"morph_targets", "Morph targets"}};
        for (const auto& [category, label] : categories)
            s.push_back(Setting(std::string("include.") + category,
                                T::Bool,
                                "true",
                                label,
                                "Contents",
                                std::string("Import the source's ") + label + "." +
                                    (std::string(category) == "skeleton" ?
                                         " Without it meshes import as static geometry." :
                                     std::string(category) == "materials" ?
                                         " Without them slots use their remap or the default material." :
                                         "")));
        s.push_back(Setting("include.mesh.<name>",
                            T::Bool,
                            "true",
                            "Include mesh",
                            "Contents",
                            "Include all source sections with this exact mesh name."));
        s.push_back(Setting("include.clip.<name>",
                            T::Bool,
                            "true",
                            "Include clip",
                            "Contents",
                            "Include all clips with this exact source name."));
        s.push_back(Setting("animation.compression",
                            T::Enum,
                            "off",
                            "Compression",
                            "Animation",
                            "Clip compression mode.",
                            {{"off", "Off", "Every imported key."},
                             {"lossless", "Lossless", "Bit-identical playback; drops repeated values."},
                             {"optimal", "Optimal", "Removes keys within small model-space error limits."},
                             {"custom", "Custom", "Optimal with the error limits set below."}}));
        s.push_back(Setting("animation.compression.<clip-key>",
                            T::Enum,
                            "inherit",
                            "Clip compression",
                            "Animation",
                            "Override compression for a clip object key.",
                            {{"inherit", "Model default", "Use the model's compression."},
                             {"off", "Off", "Every imported key."},
                             {"lossless", "Lossless", "Bit-identical playback; drops repeated values."},
                             {"optimal", "Optimal", "Removes keys within small model-space error limits."},
                             {"custom", "Custom", "Optimal with the error limits set below."}}));
        s.back().InheritFrom   = "animation.compression";
        const char* fields[]   = {"position", "shell", "morph"};
        const char* labels[]   = {"Position error", "Shell distance", "Morph error"};
        const char* defaults[] = {"0.0001", "0.03", "0.001"};
        for (size_t i = 0; i < 3; ++i)
        {
            s.push_back(Setting(std::string("animation.error.") + fields[i],
                                T::Float,
                                defaults[i],
                                labels[i],
                                "Animation",
                                "Nonnegative tolerance used by custom compression."));
            s.back().Min      = 0;
            s.back().Advanced = true;
            s.push_back(Setting(std::string("animation.error.") + fields[i] + ".<clip-key>",
                                T::Float,
                                "inherit",
                                labels[i],
                                "Animation",
                                "Per-clip custom tolerance; inherit uses the global value."));
            s.back().Min         = 0;
            s.back().Advanced    = true;
            s.back().InheritFrom = std::string("animation.error.") + fields[i];
        }
        s.push_back(Setting("animation.strip_prefix",
                            T::String,
                            "",
                            "Strip prefix",
                            "Animation",
                            "Remove this prefix from joint and shape names."));
        s.push_back(Setting("animation.map.<name>",
                            T::String,
                            "",
                            "Name mapping",
                            "Animation",
                            "Map a source joint/shape name; empty removes the override."));
        s.push_back(Setting("material.<slot-key>",
                            T::Asset,
                            "",
                            "Material remap",
                            "Materials",
                            "Published material ID; empty uses the source material."));
        s.back().Key       = "material.material:<slot>"; // Includes the complete slot key, including its length prefix.
        s.back().AssetKind = AssetType::Material;
        schema.Presets     = {{"Static mesh",
                               "Geometry and materials without animations.",
                               {{"include.meshes", "true"},
                                {"include.materials", "true"},
                                {"include.animations", "false"},
                                {"include.skeleton", "false"},
                                {"include.morph_targets", "true"}}},
                              {"Character (skinned + animations)",
                               "Skinned meshes, morphs and clips.",
                               {{"include.meshes", "true"},
                                {"include.animations", "true"},
                                {"include.materials", "true"},
                                {"include.skeleton", "true"},
                                {"include.morph_targets", "true"}}},
                              {"Animations only",
                               "Skeleton and clips without geometry or materials.",
                               {{"include.meshes", "false"},
                                {"include.materials", "false"},
                                {"include.animations", "true"},
                                {"include.skeleton", "true"}}}};
        return schema;
    }
    ImportSettingsSchema TextureImporter::GetSettingsSchema() const
    {
        ImportSettingsSchema schema;
        auto&                s = schema.Settings;
        s.push_back(Setting("usage",
                            T::Enum,
                            "color",
                            "Usage",
                            "Texture",
                            "Interpret pixels as color, data or tangent-space normals.",
                            {{"color", "Color", "Albedo, emission and other colors."},
                             {"normal", "Normal map", "Tangent-space normals."},
                             {"data", "Data / mask", "Roughness, metalness, masks: never color corrected."}}));
        s.push_back(Setting("color_space",
                            T::Enum,
                            "auto",
                            "Color space",
                            "Texture",
                            "Auto respects source metadata and usage; HDR is linear.",
                            {{"auto", "Auto", "From the file and the usage."},
                             {"srgb", "sRGB", "Gamma-encoded color."},
                             {"linear", "Linear", "Values used as stored."}}));
        s.push_back(Setting("alpha_mode",
                            T::Enum,
                            "auto",
                            "Alpha mode",
                            "Texture",
                            "Auto uses channel presence.",
                            {{"auto", "Auto", "Alpha when the file has an alpha channel."},
                             {"opaque", "Opaque", "Ignore alpha."},
                             {"straight", "Straight", "Color not multiplied by alpha."},
                             {"premultiplied", "Premultiplied", "Color already multiplied by alpha."}}));
        s.push_back(Setting(
            "generate_mips", T::Bool, "true", "Generate mips", "Texture", "Build a complete filtered mip chain."));
        s.push_back(Setting("flip_y", T::Bool, "false", "Flip image Y", "Texture", "Flip source rows."));
        s.push_back(Setting("normal_flip_y",
                            T::Bool,
                            "false",
                            "Flip normal Y",
                            "Texture",
                            "Invert green channel; requires normal usage."));
        s.push_back(Setting("max_size",
                            T::Int,
                            "0",
                            "Maximum size",
                            "Texture",
                            "Limit largest dimension, preserving aspect; 0 means unlimited.",
                            {{"0", "Unlimited", "Keep the source size."},
                             {"256", "256", {}},
                             {"512", "512", {}},
                             {"1024", "1024", {}},
                             {"2048", "2048", {}},
                             {"4096", "4096", {}},
                             {"8192", "8192", {}}}));
        s.back().Min  = 0;
        s.back().Max  = 16384;
        s.back().Step = 1;
        s.push_back(Setting("filter",
                            T::Enum,
                            "linear",
                            "Filter",
                            "Texture",
                            "Minification, magnification and mip filtering.",
                            {{"linear", "Linear", "Smooth (trilinear with mips)."},
                             {"nearest", "Nearest", "Hard pixels, e.g. pixel art."}}));
        s.push_back(Setting("wrap",
                            T::Enum,
                            "repeat",
                            "Wrap",
                            "Texture",
                            "UV addressing on every axis.",
                            {{"repeat", "Repeat", "Tile outside 0..1."},
                             {"clamp", "Clamp", "Stretch the edge pixels."},
                             {"mirror", "Mirror", "Tile, flipping every other copy."}}));
        schema.Presets = {
            {"Color (sRGB)",
             "Color textures.",
             {{"usage", "color"}, {"color_space", "srgb"}, {"normal_flip_y", "false"}, {"generate_mips", "true"}}},
            {"Normal map",
             "Linear tangent-space normals.",
             {{"usage", "normal"}, {"color_space", "linear"}, {"alpha_mode", "opaque"}, {"generate_mips", "true"}}},
            {"Data / mask (linear)",
             "Uncorrected data channels.",
             {{"usage", "data"}, {"color_space", "linear"}, {"alpha_mode", "opaque"}, {"normal_flip_y", "false"}}},
            {"UI / sprite (no mips)",
             "Color sprites without mipmaps.",
             {{"usage", "color"},
              {"color_space", "srgb"},
              {"generate_mips", "false"},
              {"wrap", "clamp"},
              {"normal_flip_y", "false"}}}};
        return schema;
    }
    ImportSettingsSchema AudioImporter::GetSettingsSchema() const
    {
        ImportSettingsSchema schema;
        schema.Settings = {
            Setting("load_mode",
                    T::Enum,
                    "auto",
                    "Load mode",
                    "Audio",
                    "Auto chooses by decoded duration/size.",
                    {{"auto", "Auto", "Decompress short sounds, stream long ones."},
                     {"decompress", "Decompress", "Decode fully when loaded."},
                     {"stream", "Stream", "Decode while playing."}}),
            Setting("volume", T::Float, "1", "Volume", "Audio", "Default playback volume."),
            Setting("loop_begin", T::Float, "0", "Loop begin", "Audio", "Loop start in seconds."),
            Setting("loop_end", T::Float, "0", "Loop end", "Audio", "Loop end in seconds; 0 uses duration.")};
        for (size_t i = 1; i < schema.Settings.size(); ++i)
            schema.Settings[i].Min = 0;
        schema.Settings[1].Max = 4;
        return schema;
    }
    std::variant<std::monostate, AssetError>
    AudioImporter::ValidateSettings(const std::map<std::string, std::string>& s) const
    {
        auto parsed = ParseAudioImportSettings(s);
        if (auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        return std::monostate {};
    }
    std::variant<std::monostate, AssetError>
    TextureImporter::ValidateSettings(const std::map<std::string, std::string>& s) const
    {
        auto parsed = ParseTextureImportSettings(s);
        if (auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        return std::monostate {};
    }
    ImportSettingsSchema FontImporter::GetSettingsSchema() const
    {
        ImportSettingsSchema schema;
        schema.Settings = {
            Setting("face", T::Int, "0", "Collection face", "Font", "Index reported by source inspection."),
            Setting("glyph_size",
                    T::Enum,
                    "48",
                    "Glyph size",
                    "Atlas",
                    "Em size in atlas pixels.",
                    {{"32", "32", {}}, {"48", "48", {}}, {"64", "64", {}}}),
            Setting("pixel_range", T::Int, "6", "Pixel range", "Atlas", "Distance range and glyph padding in pixels."),
            Setting("preload",
                    T::Enum,
                    "ascii",
                    "Preload",
                    "Atlas",
                    "Glyphs baked at import time.",
                    {{"none", "None", {}}, {"ascii", "ASCII", {}}, {"latin1", "Latin-1", {}}}),
            Setting(
                "preload.extra", T::String, "", "Extra characters", "Atlas", "Additional UTF-8 characters to bake."),
            Setting("fallback.<n>", T::Asset, "", "Fallback font", "Font", "Font asset IDs in numeric order.")};
        schema.Settings[0].Min           = 0;
        schema.Settings[2].Min           = 2;
        schema.Settings[2].Max           = 16;
        schema.Settings.back().AssetKind = AssetType::Font;
        schema.Presets                   = {
            {"UI text", "Regular UI text.", {{"glyph_size", "48"}, {"pixel_range", "6"}}},
            {"Large titles", "Larger atlas glyphs for titles.", {{"glyph_size", "64"}, {"pixel_range", "8"}}}};
        return schema;
    }
} // namespace Kans
