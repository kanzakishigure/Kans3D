#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// Content browser state without UI: directory listings, sorting, filtering and
// selection.
namespace Kans::Browser
{
    namespace fs = std::filesystem;

    enum class AssetKind
    {
        Folder,
        Model,
        Texture,
        Material,
        Shader,
        Scene,
        Audio,
        Font,
        UI,
        Timeline,
        Action,
        Expression,
        PhysicsMaterial,
        Cloth,
        Other,
    };
    constexpr int AssetKindCount = int(AssetKind::Other) + 1;

    [[nodiscard]] AssetKind   KindOf(const fs::path& path, bool directory);
    [[nodiscard]] const char* KindName(AssetKind kind);
    [[nodiscard]] std::string Lowercase(std::string text);

    struct Entry
    {
        fs::path            Path; // Absolute.
        std::string         Name; // UTF-8 file name.
        std::string         Extension; // Lowercase, with the dot.
        bool                Directory = false;
        AssetKind           Kind      = AssetKind::Other;
        uint64_t            Size      = 0;
        fs::file_time_type  Modified {};
    };

    // Files the browser never shows: asset sidecars and their temporaries, and
    // hidden dot files.
    [[nodiscard]] bool IsHidden(const fs::path& path);

    // Lists directories on demand and re-reads a listing when the directory
    // changes on disk (checked at most every `interval`) or after Invalidate().
    class DirectoryCache
    {
    public:
        explicit DirectoryCache(std::chrono::milliseconds interval = std::chrono::milliseconds(500)) :
            m_Interval(interval)
        {
        }

        // Folders first, then files, each by name.
        const std::vector<Entry>& List(const fs::path& directory);
        bool                      HasSubdirectories(const fs::path& directory);
        void                      Invalidate();
        // Changes when any listing is re-read; lets callers refresh derived data.
        uint64_t Generation() const { return m_Generation; }

    private:
        struct Listing
        {
            fs::file_time_type                    WriteTime {};
            std::chrono::steady_clock::time_point Checked {};
            std::vector<Entry>                    Entries;
        };
        std::chrono::milliseconds   m_Interval;
        std::map<fs::path, Listing> m_Listings;
        uint64_t                    m_Generation = 0;
    };

    enum class SortKey
    {
        Name,
        Type,
        Size,
        Modified,
    };

    // Folders always come first.
    void SortEntries(std::vector<Entry>& entries, SortKey key, bool ascending);

    struct Filter
    {
        static_assert(AssetKindCount == 15, "Every kind starts shown: extend Kinds below.");
        std::string Text; // Case-insensitive substring of the name.
        bool        Kinds[AssetKindCount] =
            {true, true, true, true, true, true, true, true, true, true, true, true, true, true, true};

        bool Matches(const Entry& entry) const;
        bool IsActive() const;
        // Type pills. With every file type shown, clicking one shows only that type;
        // otherwise a click toggles it, and turning the last one off shows them all
        // again. Folders are always shown.
        void TogglePill(AssetKind kind);
        bool ShowsAllTypes() const;
    };

    // A path in `directory` that does not exist yet: "name.ext", "name (1).ext", ...
    [[nodiscard]] fs::path UniqueChildPath(const fs::path& directory, const std::string& stem, const std::string& extension);
    // A file or folder name without separators, reserved names or control characters.
    [[nodiscard]] bool ValidName(const std::string& name);
    // True when `path` is `ancestor` or inside it.
    [[nodiscard]] bool IsWithin(const fs::path& path, const fs::path& ancestor);

    // Multi-selection with a range anchor, in the order items are displayed.
    class Selection
    {
    public:
        // Plain click selects one item, Ctrl toggles, Shift selects the range from
        // the anchor in `order`.
        void Click(const fs::path& item, bool ctrl, bool shift, const std::vector<fs::path>& order);
        void Set(const fs::path& item);
        void SetAll(const std::vector<fs::path>& items);
        void Clear();
        // Drops items that are no longer present.
        void Retain(const std::vector<fs::path>& present);

        bool                      Contains(const fs::path& item) const { return m_Items.count(item) != 0; }
        bool                      Empty() const { return m_Items.empty(); }
        size_t                    Size() const { return m_Items.size(); }
        const fs::path&           Anchor() const { return m_Anchor; }
        // Selected items in `order` order.
        std::vector<fs::path> Ordered(const std::vector<fs::path>& order) const;
        const std::set<fs::path>& Items() const { return m_Items; }

    private:
        std::set<fs::path> m_Items;
        fs::path           m_Anchor;
    };
} // namespace Kans::Browser
