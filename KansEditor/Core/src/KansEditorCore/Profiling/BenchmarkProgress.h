#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// --benchmark-progress <file>: the benchmark process replaces `file` with one line
//   kans-benchmark-progress 1 <phase> <frame> <total>\n
// every ProgressWriter::Interval frames and on each phase change. The file is written beside
// and renamed over the old one, so a reader sees either line whole; anything else (a missing
// newline, extra text, an unknown phase) is not progress and is ignored.
namespace Kans::Benchmark
{
    enum class Phase : uint8_t
    {
        Loading,
        Warmup,
        Measuring,
        Writing,
        Done
    };
    const char* PhaseLabel(Phase phase);

    struct Progress
    {
        Phase    Stage = Phase::Loading;
        uint32_t Frame = 0, Total = 0; // Frames done in this phase, out of Total.
        bool     operator==(const Progress& other) const
        {
            return Stage == other.Stage && Frame == other.Frame && Total == other.Total;
        }
        bool operator!=(const Progress& other) const { return !(*this == other); }
    };

    std::string             FormatProgress(const Progress& progress);
    std::optional<Progress> ParseProgress(std::string_view text);
    // Best effort: false when the file could not be replaced (a reader holding it on Windows).
    bool                    WriteProgress(const std::filesystem::path& file, const Progress& progress);
    std::optional<Progress> ReadProgress(const std::filesystem::path& file);

    // Writes on a phase change, on the phase's last frame, and every Interval frames between.
    class ProgressWriter
    {
    public:
        static constexpr uint32_t Interval = 10;
        explicit ProgressWriter(std::filesystem::path file) : m_File(std::move(file)) {}
        static bool ShouldWrite(const std::optional<Progress>& last, const Progress& next);
        void        Report(Phase phase, uint32_t frame, uint32_t total);

    private:
        std::filesystem::path   m_File; // Empty: no progress requested.
        std::optional<Progress> m_Written;
    };
} // namespace Kans::Benchmark
