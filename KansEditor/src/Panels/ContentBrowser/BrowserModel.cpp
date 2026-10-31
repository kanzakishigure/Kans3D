#include "BrowserModel.h"
#include <algorithm>
#include <cctype>
#include <tuple>

namespace Kans::Browser
{
    std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    }

    AssetKind KindOf(const fs::path& path, bool directory)
    {
        if (directory)
            return AssetKind::Folder;
        const auto extension = Lowercase(path.extension().u8string());
        static const std::map<std::string, AssetKind> kinds     = {
            {".obj", AssetKind::Model},
            {".fbx", AssetKind::Model},
            {".gltf", AssetKind::Model},
            {".glb", AssetKind::Model},
            {".dae", AssetKind::Model},
            {".3ds", AssetKind::Model},
            {".pmx", AssetKind::Model},
            {".blend", AssetKind::Model},
            {".png", AssetKind::Texture},
            {".jpg", AssetKind::Texture},
            {".jpeg", AssetKind::Texture},
            {".bmp", AssetKind::Texture},
            {".tga", AssetKind::Texture},
            {".hdr", AssetKind::Texture},
            {".material", AssetKind::Material},
            {".mtl", AssetKind::Material},
            {".shaderprog", AssetKind::Shader},
            {".vert", AssetKind::Shader},
            {".frag", AssetKind::Shader},
            {".comp", AssetKind::Shader},
            {".glsl", AssetKind::Shader},
            {".kans", AssetKind::Scene},
            {".wav", AssetKind::Audio},
            {".ogg", AssetKind::Audio},
            {".mp3", AssetKind::Audio},
            {".flac", AssetKind::Audio},
            {".ttf", AssetKind::Font},
            {".otf", AssetKind::Font},
            {".ttc", AssetKind::Font},
            {".otc", AssetKind::Font},
            {".kui", AssetKind::UI},
            {".timeline", AssetKind::Timeline},
            {".action", AssetKind::Action},
            {".expression", AssetKind::Expression},
            {".physmat", AssetKind::PhysicsMaterial},
            {".cloth", AssetKind::Cloth},
        };
        const auto found = kinds.find(extension);
        return found == kinds.end() ? AssetKind::Other : found->second;
    }

    const char* KindName(AssetKind kind)
    {
        switch (kind)
        {
            case AssetKind::Folder:
                return "Folder";
            case AssetKind::Model:
                return "Model";
            case AssetKind::Texture:
                return "Texture";
            case AssetKind::Material:
                return "Material";
            case AssetKind::Shader:
                return "Shader";
            case AssetKind::Scene:
                return "Scene";
            case AssetKind::Audio:
                return "Audio";
            case AssetKind::Font:
                return "Font";
            case AssetKind::UI:
                return "UI";
            case AssetKind::Timeline:
                return "Timeline";
            case AssetKind::Action:
                return "Action";
            case AssetKind::Expression:
                return "Expression";
            case AssetKind::PhysicsMaterial:
                return "Physics Material";
            case AssetKind::Cloth:
                return "Cloth";
            default:
                return "File";
        }
    }

    bool IsHidden(const fs::path& path)
    {
        const auto name = Lowercase(path.filename().u8string());
        if (name.empty() || name[0] == '.')
            return true;
        const std::string sidecar = ".kmeta";
        return (name.size() >= sidecar.size() && name.compare(name.size() - sidecar.size(), sidecar.size(), sidecar) == 0) ||
               name.find(".kmeta.tmp-") != std::string::npos;
    }

    namespace
    {
        bool SameEntries(const std::vector<Entry>& a, const std::vector<Entry>& b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Entry& x, const Entry& y) {
                return x.Path == y.Path && x.Directory == y.Directory && x.Size == y.Size && x.Modified == y.Modified;
            });
        }

        std::vector<Entry> ReadDirectory(const fs::path& directory)
        {
            std::vector<Entry> entries;
            std::error_code    error;
            for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            {
                const auto& item = *it;
                if (IsHidden(item.path()))
                    continue;
                Entry entry;
                entry.Path      = item.path();
                entry.Name      = item.path().filename().u8string();
                std::error_code ignored;
                entry.Directory = item.is_directory(ignored);
                entry.Extension = entry.Directory ? std::string() : Lowercase(item.path().extension().u8string());
                entry.Kind      = KindOf(item.path(), entry.Directory);
                entry.Size      = entry.Directory ? 0 : item.file_size(ignored);
                entry.Modified  = item.last_write_time(ignored);
                entries.push_back(std::move(entry));
            }
            SortEntries(entries, SortKey::Name, true);
            return entries;
        }
    } // namespace

    const std::vector<Entry>& DirectoryCache::List(const fs::path& directory)
    {
        const auto now    = std::chrono::steady_clock::now();
        auto [it, added]  = m_Listings.try_emplace(directory);
        auto& listing     = it->second;
        if (!added && now - listing.Checked < m_Interval)
            return listing.Entries;
        std::error_code error;
        const auto      writeTime = fs::last_write_time(directory, error);
        // A directory's write time covers added, removed and renamed entries; file
        // edits do not change it, so listings are re-read every few checks anyway.
        const bool stale = added || error || writeTime != listing.WriteTime || now - listing.Checked > m_Interval * 4;
        listing.Checked  = now;
        if (!stale)
            return listing.Entries;
        listing.WriteTime = writeTime;
        auto entries      = ReadDirectory(directory);
        if (added || !SameEntries(entries, listing.Entries))
        {
            listing.Entries = std::move(entries);
            ++m_Generation;
        }
        return listing.Entries;
    }

    bool DirectoryCache::HasSubdirectories(const fs::path& directory)
    {
        const auto& entries = List(directory);
        return std::any_of(entries.begin(), entries.end(), [](const Entry& entry) { return entry.Directory; });
    }

    void DirectoryCache::Invalidate()
    {
        m_Listings.clear();
        ++m_Generation;
    }

    void SortEntries(std::vector<Entry>& entries, SortKey key, bool ascending)
    {
        std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
            if (a.Directory != b.Directory)
                return a.Directory;
            const auto nameA = Lowercase(a.Name), nameB = Lowercase(b.Name);
            bool       less = false, greater = false;
            switch (key)
            {
                case SortKey::Name:
                    less = nameA < nameB, greater = nameB < nameA;
                    break;
                case SortKey::Type:
                    less    = std::tie(a.Kind, a.Extension, nameA) < std::tie(b.Kind, b.Extension, nameB);
                    greater = std::tie(b.Kind, b.Extension, nameB) < std::tie(a.Kind, a.Extension, nameA);
                    break;
                case SortKey::Size:
                    less    = std::tie(a.Size, nameA) < std::tie(b.Size, nameB);
                    greater = std::tie(b.Size, nameB) < std::tie(a.Size, nameA);
                    break;
                case SortKey::Modified:
                    less    = std::tie(a.Modified, nameA) < std::tie(b.Modified, nameB);
                    greater = std::tie(b.Modified, nameB) < std::tie(a.Modified, nameA);
                    break;
            }
            return ascending ? less : greater;
        });
    }

    bool Filter::Matches(const Entry& entry) const
    {
        if (!Kinds[static_cast<int>(entry.Kind)])
            return false;
        return Text.empty() || Lowercase(entry.Name).find(Lowercase(Text)) != std::string::npos;
    }

    bool Filter::IsActive() const
    {
        return !Text.empty() || std::any_of(std::begin(Kinds), std::end(Kinds), [](bool on) { return !on; });
    }

    bool Filter::ShowsAllTypes() const
    {
        return std::all_of(std::begin(Kinds) + 1, std::end(Kinds), [](bool on) { return on; });
    }

    void Filter::TogglePill(AssetKind kind)
    {
        const int index = int(kind);
        if (index <= 0 || index >= AssetKindCount)
            return;
        if (ShowsAllTypes())
        {
            std::fill(std::begin(Kinds) + 1, std::end(Kinds), false);
            Kinds[index] = true;
        }
        else
            Kinds[index] = !Kinds[index];
        if (std::none_of(std::begin(Kinds) + 1, std::end(Kinds), [](bool on) { return on; }))
            std::fill(std::begin(Kinds) + 1, std::end(Kinds), true);
        Kinds[0] = true;
    }

    fs::path UniqueChildPath(const fs::path& directory, const std::string& stem, const std::string& extension)
    {
        std::error_code error;
        for (int index = 0; index < 10000; ++index)
        {
            const std::string name =
                stem + (index ? " (" + std::to_string(index) + ")" : std::string()) + extension;
            const auto candidate = directory / fs::u8path(name);
            if (!fs::exists(candidate, error))
                return candidate;
        }
        return directory / fs::u8path(stem + extension);
    }

    bool ValidName(const std::string& name)
    {
        if (name.empty() || name.size() > 255 || name == "." || name == ".." || name.back() == '.' ||
            name.back() == ' ' || name.front() == ' ')
            return false;
        return std::none_of(name.begin(), name.end(), [](unsigned char c) {
            return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                   c == '<' || c == '>' || c == '|';
        });
    }

    bool IsWithin(const fs::path& path, const fs::path& ancestor)
    {
        auto a = ancestor.begin();
        auto p = path.begin();
        for (; a != ancestor.end(); ++a, ++p)
        {
            if (a->empty())
                continue;
            if (p == path.end() || *a != *p)
                return false;
        }
        return true;
    }

    void Selection::Click(const fs::path& item, bool ctrl, bool shift, const std::vector<fs::path>& order)
    {
        if (shift && !m_Anchor.empty())
        {
            const auto from = std::find(order.begin(), order.end(), m_Anchor);
            const auto to   = std::find(order.begin(), order.end(), item);
            if (from != order.end() && to != order.end())
            {
                if (!ctrl)
                    m_Items.clear();
                const auto [first, last] = std::minmax(from, to);
                m_Items.insert(first, last + 1);
                return;
            }
        }
        if (ctrl)
        {
            if (!m_Items.erase(item))
                m_Items.insert(item);
            m_Anchor = item;
            return;
        }
        Set(item);
    }

    void Selection::Set(const fs::path& item)
    {
        m_Items  = {item};
        m_Anchor = item;
    }

    void Selection::SetAll(const std::vector<fs::path>& items)
    {
        m_Items = std::set<fs::path>(items.begin(), items.end());
        if (!items.empty() && !m_Items.count(m_Anchor))
            m_Anchor = items.front();
    }

    void Selection::Clear()
    {
        m_Items.clear();
        m_Anchor.clear();
    }

    void Selection::Retain(const std::vector<fs::path>& present)
    {
        const std::set<fs::path> keep(present.begin(), present.end());
        for (auto it = m_Items.begin(); it != m_Items.end();)
            it = keep.count(*it) ? std::next(it) : m_Items.erase(it);
        if (!keep.count(m_Anchor))
            m_Anchor.clear();
    }

    std::vector<fs::path> Selection::Ordered(const std::vector<fs::path>& order) const
    {
        std::vector<fs::path> result;
        for (const auto& item : order)
            if (m_Items.count(item))
                result.push_back(item);
        // Selected items that are not displayed (e.g. filtered out) keep their order last.
        for (const auto& item : m_Items)
            if (std::find(order.begin(), order.end(), item) == order.end())
                result.push_back(item);
        return result;
    }
} // namespace Kans::Browser
