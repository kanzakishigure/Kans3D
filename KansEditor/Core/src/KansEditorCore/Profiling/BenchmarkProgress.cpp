#include "KansEditorCore/Profiling/BenchmarkProgress.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <vector>

namespace Kans::Benchmark
{
    namespace fs = std::filesystem;
    namespace
    {
        constexpr std::string_view Magic = "kans-benchmark-progress";
        constexpr const char*      Phases[] {"loading", "warmup", "measuring", "writing", "done"};

        std::optional<uint32_t> Number(std::string_view text)
        {
            uint32_t   value  = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
                return std::nullopt;
            return value;
        }
    } // namespace

    const char* PhaseLabel(Phase phase)
    {
        switch (phase)
        {
            case Phase::Loading:
                return "Loading";
            case Phase::Warmup:
                return "Warming up";
            case Phase::Measuring:
                return "Measuring";
            case Phase::Writing:
                return "Writing report";
            case Phase::Done:
                return "Done";
        }
        return "";
    }

    std::string FormatProgress(const Progress& progress)
    {
        return std::string(Magic) + " 1 " + Phases[size_t(progress.Stage)] + " " + std::to_string(progress.Frame) +
               " " + std::to_string(progress.Total) + "\n";
    }

    std::optional<Progress> ParseProgress(std::string_view text)
    {
        if (text.empty() || text.back() != '\n')
            return std::nullopt;
        text.remove_suffix(1);
        std::vector<std::string_view> fields;
        for (size_t start = 0; start <= text.size();)
        {
            const auto end = (std::min)(text.find(' ', start), text.size());
            fields.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (fields.size() != 5 || fields[0] != Magic || fields[1] != "1")
            return std::nullopt;
        Progress progress;
        size_t   phase = 0;
        while (phase < std::size(Phases) && fields[2] != Phases[phase])
            ++phase;
        const auto frame = Number(fields[3]), total = Number(fields[4]);
        if (phase == std::size(Phases) || !frame || !total || *frame > *total)
            return std::nullopt;
        progress.Stage = Phase(phase);
        progress.Frame = *frame;
        progress.Total = *total;
        return progress;
    }

    bool WriteProgress(const fs::path& file, const Progress& progress)
    {
        auto temporary = file;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << FormatProgress(progress);
            if (!out.flush())
                return false;
        }
        std::error_code error;
        fs::rename(temporary, file, error);
        if (error)
            fs::remove(temporary, error);
        return !error;
    }

    std::optional<Progress> ReadProgress(const fs::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return std::nullopt;
        char       text[128];
        const auto read = input.read(text, sizeof(text)).gcount();
        if (read <= 0 || size_t(read) == sizeof(text))
            return std::nullopt;
        return ParseProgress({text, size_t(read)});
    }

    bool ProgressWriter::ShouldWrite(const std::optional<Progress>& last, const Progress& next)
    {
        return !last || last->Stage != next.Stage || last->Total != next.Total || next.Frame == next.Total ||
               next.Frame >= last->Frame + Interval;
    }

    void ProgressWriter::Report(Phase phase, uint32_t frame, uint32_t total)
    {
        const Progress next {phase, frame, total};
        if (m_File.empty() || !ShouldWrite(m_Written, next))
            return;
        // A failed replacement keeps the old value so the next frame tries again.
        if (WriteProgress(m_File, next))
            m_Written = next;
    }
} // namespace Kans::Benchmark
