#include "kspch.h"
#include "FontSystem.h"
#include <set>
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "GlyphAtlas.h"
namespace Kans
{
    FontSystem::FontSystem(std::filesystem::path folder) :
        m_Folder(folder.empty() ? KansFileSystem::GetFontsFolder() : std::move(folder))
    {}
    FontSystem& FontSystem::Default()
    {
        static FontSystem system;
        return system;
    }
    std::string FontSystem::Region(const std::string& language)
    {
        std::string l = language;
        for (auto& c : l)
            if (c >= 'A' && c <= 'Z')
                c += 32;
        if (l == "ja" || l.rfind("ja-", 0) == 0)
            return "JP";
        if (l == "ko" || l.rfind("ko-", 0) == 0)
            return "KR";
        if (l.rfind("zh-hk", 0) == 0 || l.find("-hk") != std::string::npos)
            return "HK";
        if (l.rfind("zh-hans", 0) == 0 || l.rfind("zh-cn", 0) == 0 || l.rfind("zh-sg", 0) == 0)
            return "SC";
        return "TC";
    }
    Ref<const FontAsset> FontSystem::Load(const std::filesystem::path& file, uint32_t face)
    {
        auto bytes = ReadProductFile(file, 128 * 1024 * 1024);
        if (auto* e = std::get_if<AssetError>(&bytes))
        {
            if (Log::GetCoreLogger())
                CORE_ERROR("{}", e->Message);
            return {};
        }
        FontSettings s;
        s.Face    = face;
        s.Preload = "none";
        auto data = ReadFontData(
            std::make_shared<const std::vector<std::byte>>(std::get<std::vector<std::byte>>(std::move(bytes))), s);
        if (!std::holds_alternative<FontData>(data))
            return {};
        auto font = CreateRef<FontAsset>(std::get<FontData>(std::move(data)));
        m_Tracked.push_back(font);
        font->Atlas().BeginFrame(m_Frame);
        return font;
    }
    Ref<const FontAsset> FontSystem::Latin()
    {
        if (!m_Latin)
            m_Latin = Load(m_Folder / "Roboto/Roboto-Regular.ttf");
        return m_Latin;
    }
    Ref<const FontAsset> FontSystem::CJK(const std::string& language)
    {
        auto region = Region(language);
        auto found  = m_CJK.find(region);
        if (found != m_CJK.end())
            return found->second;
        if (!m_CJKBytes)
        {
            auto read = ReadProductFile(m_Folder / "NotoSansCJK/NotoSansCJK-Regular.ttc", 128 * 1024 * 1024);
            if (!std::holds_alternative<std::vector<std::byte>>(read))
                return {};
            m_CJKBytes =
                std::make_shared<const std::vector<std::byte>>(std::get<std::vector<std::byte>>(std::move(read)));
        }
        auto*    blob     = hb_blob_create(reinterpret_cast<const char*>(m_CJKBytes->data()),
                                           unsigned(m_CJKBytes->size()),
                                           HB_MEMORY_MODE_READONLY,
                                           nullptr,
                                           nullptr);
        auto     count    = hb_face_count(blob);
        uint32_t selected = count;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto* face   = hb_face_create(blob, i);
            auto  family = FontName(face, 1);
            hb_face_destroy(face);
            if (family == "Noto Sans CJK " + region)
            {
                selected = i;
                break;
            }
        }
        hb_blob_destroy(blob);
        if (selected == count)
            return {};
        FontSettings s;
        s.Face    = selected;
        s.Preload = "none";
        auto data = ReadFontData(m_CJKBytes, s);
        if (!std::holds_alternative<FontData>(data))
            return {};
        auto font     = CreateRef<FontAsset>(std::get<FontData>(std::move(data)));
        m_CJK[region] = font;
        m_Tracked.push_back(font);
        font->Atlas().BeginFrame(m_Frame);
        return font;
    }
    std::vector<Ref<const FontAsset>> FontSystem::Chain(const Ref<const FontAsset>& primary,
                                                        const std::string&          language)
    {
        std::vector<Ref<const FontAsset>>         chain;
        std::set<const FontAsset*>                seen;
        std::set<AssetID>                         ids;
        std::function<void(Ref<const FontAsset>)> add = [&](Ref<const FontAsset> f) {
            if (!f || !seen.insert(f.get()).second || chain.size() >= 64)
                return;
            chain.push_back(f);
            if (m_Resolver)
                for (auto id : f->GetData().Settings.Fallbacks)
                    if (ids.insert(id).second)
                        add(m_Resolver(id));
        };
        add(primary);
        add(Latin());
        add(CJK(language));
        for (const auto& f : chain)
        {
            f->Atlas().BeginFrame(m_Frame);
            bool tracked = false;
            for (const auto& w : m_Tracked)
                if (w.lock() == f)
                {
                    tracked = true;
                    break;
                }
            if (!tracked)
                m_Tracked.push_back(f);
        }
        return chain;
    }
    void FontSystem::BeginFrame(uint64_t frame)
    {
        m_Frame = std::max(frame, m_Frame);
        for (auto i = m_Tracked.begin(); i != m_Tracked.end();)
            if (auto f = i->lock())
            {
                f->Atlas().BeginFrame(m_Frame);
                ++i;
            }
            else
                i = m_Tracked.erase(i);
    }
    size_t FontSystem::Rasterize(size_t budget)
    {
        size_t n = 0;
        for (auto& w : m_Tracked)
            if (auto f = w.lock())
            {
                auto attempts = std::min(budget, f->Atlas().PendingCount());
                auto count    = f->Atlas().Rasterize(attempts);
                n += count;
                budget -= attempts;
                if (!budget)
                    break;
            }
        return n;
    }
} // namespace Kans
