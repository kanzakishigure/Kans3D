#include "kspch.h"
#include "FontImporter.h"
#include <hb-ot.h>
#include <charconv>
#include <set>
#include "Kans3D/Asset/Product/FontProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
namespace Kans
{
    std::variant<FontSettings, AssetError> ParseFontImportSettings(const std::map<std::string, std::string>& values)
    {
        FontSettings                s;
        std::map<uint32_t, AssetID> fallback;
        auto                        number = [](const std::string& v, uint32_t& n) {
            auto r = std::from_chars(v.data(), v.data() + v.size(), n);
            return r.ec == std::errc {} && r.ptr == v.data() + v.size();
        };
        for (const auto& [k, v] : values)
        {
            if (k == "face" || k == "glyph_size" || k == "pixel_range")
            {
                uint32_t n;
                if (!number(v, n))
                    return AssetError {AssetErrorCode::InvalidArgument, "Invalid Font number: " + k};
                if (k == "face")
                    s.Face = n;
                else if (k == "glyph_size")
                    s.GlyphSize = n;
                else
                    s.PixelRange = n;
            }
            else if (k == "preload")
                s.Preload = v;
            else if (k == "preload.extra")
                s.Extra = v;
            else if (k.rfind("fallback.", 0) == 0)
            {
                uint32_t index;
                if (!number(k.substr(9), index) || index >= 64)
                    return AssetError {AssetErrorCode::InvalidArgument, "Invalid fallback index"};
                if (v.empty())
                    continue;
                auto id = AssetID::FromString(v);
                if (!id.IsValid())
                    return AssetError {AssetErrorCode::InvalidArgument, "Invalid fallback Font ID"};
                fallback[index] = id;
            }
            else
                return AssetError {AssetErrorCode::InvalidArgument, "Unknown Font setting: " + k};
        }
        if ((s.GlyphSize != 32 && s.GlyphSize != 48 && s.GlyphSize != 64) || s.PixelRange < 2 || s.PixelRange > 16 ||
            (s.Preload != "none" && s.Preload != "ascii" && s.Preload != "latin1") || s.Extra.size() > 65536)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Font settings"};
        for (const auto& [i, id] : fallback)
            s.Fallbacks.push_back(id);
        return s;
    }
    std::variant<std::monostate, AssetError>
    FontImporter::ValidateSettings(const std::map<std::string, std::string>& v) const
    {
        auto s = ParseFontImportSettings(v);
        if (auto* e = std::get_if<AssetError>(&s))
            return *e;
        return std::monostate {};
    }
    std::variant<ImportInputs, AssetError> FontImporter::DiscoverInputs(const ImportContext& c) const
    {
        auto s = ParseFontImportSettings(c.Settings);
        if (auto* e = std::get_if<AssetError>(&s))
            return *e;
        ImportInputs inputs;
        for (const auto& id : std::get<FontSettings>(s).Fallbacks)
        {
            inputs.Products.push_back(id);
            inputs.ProductsWithDependencies[id] = AssetType::Font;
        }
        return inputs;
    }
    std::variant<SourceInventory, AssetError> FontImporter::Inspect(const ImportContext& c) const
    {
        auto settings = ParseFontImportSettings(c.Settings);
        if (auto* e = std::get_if<AssetError>(&settings))
            return *e;
        auto source = ReadProductFile(c.SourceFile, 128 * 1024 * 1024);
        if (auto* e = std::get_if<AssetError>(&source))
            return *e;
        auto bytes =
            std::make_shared<const std::vector<std::byte>>(std::get<std::vector<std::byte>>(std::move(source)));
        auto data = ReadFontData(bytes, std::get<FontSettings>(settings));
        if (auto* e = std::get_if<AssetError>(&data))
            return *e;
        FontAsset     a(std::get<FontData>(data));
        InspectedFont font;
        font.Family     = FontName(a.GetFace(), 1);
        font.Style      = FontName(a.GetFace(), 2);
        font.GlyphCount = hb_face_get_glyph_count(a.GetFace());
        font.Latin      = a.GlyphIndex('A') && a.GlyphIndex('z');
        font.CJK        = a.GlyphIndex(0x570b) || a.GlyphIndex(0x56fd);
        font.Kana       = a.GlyphIndex(0x3042) != 0;
        font.Hangul     = a.GlyphIndex(0xd55c) != 0;
        auto* blob      = hb_blob_create(reinterpret_cast<const char*>(bytes->data()),
                                         unsigned(bytes->size()),
                                         HB_MEMORY_MODE_READONLY,
                                         nullptr,
                                         nullptr);
        auto  count     = hb_face_count(blob);
        for (unsigned i = 0; i < count; ++i)
        {
            auto*             face = hb_face_create(blob, i);
            InspectedFontFace f;
            f.Index                     = i;
            f.Name                      = FontName(face, 1) + " " + FontName(face, 2);
            unsigned              n     = 0;
            auto*                 names = hb_ot_name_list_names(face, &n);
            std::set<std::string> langs;
            for (unsigned j = 0; j < n; ++j)
                if (names[j].language)
                    langs.insert(hb_language_to_string(names[j].language));
            f.Languages.assign(langs.begin(), langs.end());
            font.Faces.push_back(std::move(f));
            hb_face_destroy(face);
        }
        hb_blob_destroy(blob);
        SourceInventory out;
        out.Font = std::move(font);
        return out;
    }
    std::variant<ImportResult, AssetError> FontImporter::Import(const ImportContext& c) const
    {
        if (!c.SourceID.IsValid() || !c.SourceFile.is_absolute() || !c.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Font import context"};
        auto settings = ParseFontImportSettings(c.Settings);
        if (auto* e = std::get_if<AssetError>(&settings))
            return *e;
        std::error_code ec;
        auto            status = std::filesystem::symlink_status(c.StagingDirectory, ec);
        if (ec || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_empty(c.StagingDirectory, ec) || ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Font staging must be empty and exclusive"};
        auto source = ReadProductFile(c.SourceFile, 128 * 1024 * 1024);
        if (auto* e = std::get_if<AssetError>(&source))
            return *e;
        auto data = ReadFontData(
            std::make_shared<const std::vector<std::byte>>(std::get<std::vector<std::byte>>(std::move(source))),
            std::get<FontSettings>(settings));
        if (auto* e = std::get_if<AssetError>(&data))
            return *e;
        auto&              d = std::get<FontData>(data);
        FontAsset          a(d);
        std::set<uint32_t> glyphs {0};
        unsigned           end = d.Settings.Preload == "latin1" ? 256 : d.Settings.Preload == "ascii" ? 127 : 32;
        for (unsigned cp = 32; cp < end; ++cp)
            glyphs.insert(a.GlyphIndex(cp));
        for (auto cp : DecodeText(d.Settings.Extra))
            glyphs.insert(a.GlyphIndex(cp.Value));
        for (auto g : glyphs)
        {
            auto b = GenerateGlyph(a, g);
            if (auto* e = std::get_if<AssetError>(&b))
                return *e;
            d.Preloaded.push_back(std::get<GlyphBitmap>(std::move(b)));
        }
        auto encoded = EncodeFontProduct(d);
        if (auto* e = std::get_if<AssetError>(&encoded))
            return *e;
        auto ids = AllocateLocalIDs(c.PreviousLocalIDs, {"font:root"});
        if (auto* e = std::get_if<AssetError>(&ids))
            return *e;
        ImportResult out;
        out.LocalIDs = std::get<LocalIDMap>(ids);
        auto id      = out.LocalIDs.at("font:root");
        out.Products.push_back({id, AssetType::Font, std::to_string(id) + ".kfnt", d.Settings.Fallbacks});
        auto written = WriteProductFile(c.StagingDirectory / out.Products[0].StagedFile,
                                        std::get<std::vector<std::byte>>(encoded));
        if (auto* e = std::get_if<AssetError>(&written))
            return *e;
        return out;
    }
} // namespace Kans
