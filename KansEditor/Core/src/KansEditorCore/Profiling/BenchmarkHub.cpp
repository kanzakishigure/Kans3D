#include "KansEditorCore/Profiling/BenchmarkHub.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include "Kans3D/Asset/Storage/ProductFiles.h"

namespace Kans::Benchmark
{
    namespace fs = std::filesystem;
    namespace
    {
        constexpr const char* StatisticFields[] {"mean", "median", "p95", "p99", "max"};

        // A missing key of a const node is an invalid node: most queries on it throw.
        bool IsMap(const YAML::Node& node) { return node && node.IsMap(); }
        // node[a][b]...: an undefined node when any step is missing, never an invalid one.
        YAML::Node Get(const YAML::Node& node, std::initializer_list<std::string> path)
        {
            YAML::Node current = node;
            for (const auto& key : path)
            {
                const YAML::Node& view = current;
                if (!IsMap(view))
                    return YAML::Node(YAML::NodeType::Undefined);
                const YAML::Node next = view[key];
                if (!next)
                    return YAML::Node(YAML::NodeType::Undefined);
                current.reset(next);
            }
            return current;
        }

        template<class T>
        T Field(const YAML::Node& node, const char* name, T fallback, T lower, T upper)
        {
            try
            {
                const auto value = node[name];
                if (!value || !value.IsScalar())
                    return fallback;
                const T parsed = value.as<T>();
                return parsed < lower || parsed > upper ? fallback : parsed;
            }
            catch (const std::exception&)
            {
                return fallback;
            }
        }

        std::optional<double> Number(const YAML::Node& node)
        {
            if (!node || !node.IsScalar())
                return std::nullopt;
            try
            {
                const double value = node.as<double>();
                return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        std::optional<double> MeanOf(const YAML::Node& statistics)
        {
            return statistics && statistics.IsMap() ? Number(statistics["mean"]) : std::nullopt;
        }

        YAML::Node OptionalNumber(std::optional<double> value)
        {
            return value ? YAML::Node(*value) : YAML::Node(YAML::NodeType::Null);
        }

        // Each field the median of the runs that have the statistic; samples summed.
        YAML::Node CombineStatistics(const std::vector<YAML::Node>& runs)
        {
            std::vector<YAML::Node> present;
            for (const auto& statistics : runs)
                if (statistics && statistics.IsMap())
                    present.push_back(statistics);
            if (present.empty())
                return YAML::Node(YAML::NodeType::Null);
            YAML::Node combined;
            uint64_t   samples = 0;
            for (const auto& statistics : present)
                samples += statistics["samples"].as<uint64_t>(0);
            combined["samples"] = samples;
            for (const auto* field : StatisticFields)
            {
                std::vector<double> values;
                for (const auto& statistics : present)
                    if (const auto value = Number(statistics[field]))
                        values.push_back(*value);
                combined[field] = OptionalNumber(values.empty() ? std::nullopt : std::optional<double>(Median(values)));
            }
            return combined;
        }

        std::vector<YAML::Node>
        Children(const std::vector<YAML::Node>& runs, const char* group, const std::string& name)
        {
            std::vector<YAML::Node> nodes;
            for (const auto& run : runs)
            {
                const YAML::Node& constant = run;
                nodes.push_back(Get(constant, {group, name}));
            }
            return nodes;
        }

        std::vector<double> RunValues(const std::vector<YAML::Node>& runs, const char* metric, const char* field)
        {
            std::vector<double> values;
            for (const YAML::Node& run : runs)
                if (const auto value = Number(Get(run, {"application", metric, field})))
                    values.push_back(*value);
            return values;
        }

        std::optional<Distribution> DistributionOf(const YAML::Node& statistics)
        {
            if (!statistics || !statistics.IsMap())
                return std::nullopt;
            const auto value = [&](const char* name) { return Number(statistics[name]).value_or(0.0); };
            return Distribution {statistics["samples"].as<size_t>(0),
                                 value("mean"),
                                 value("median"),
                                 value("p95"),
                                 value("p99"),
                                 value("max")};
        }

        YAML::Node Copy(const YAML::Node& node) { return node ? YAML::Clone(node) : YAML::Node(YAML::NodeType::Null); }

        std::string Text(const YAML::Node& node) { return node && node.IsScalar() ? node.Scalar() : std::string(); }
    } // namespace

    fs::path HubSettingsFile(const fs::path& projectRoot, const fs::path& cacheDirectory)
    {
        return (projectRoot / cacheDirectory / "Editor" / "Benchmark.yaml").lexically_normal();
    }

    HubSettings LoadHubSettings(const fs::path& file)
    {
        HubSettings settings;
        YAML::Node  node;
        try
        {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return settings;
            node = YAML::Load(input);
        }
        catch (const std::exception&)
        {
            return settings;
        }
        if (!IsMap(node))
            return settings;
        const HubSettings defaults;
        const auto        scene = Text(node["scene"]);
        if (!scene.empty() && (scene.rfind("builtin:", 0) != 0 || scene == ComplexScene))
            settings.Scene = scene;
        settings.Width             = Field<uint32_t>(node, "width", defaults.Width, 1, 16384);
        settings.Height            = Field<uint32_t>(node, "height", defaults.Height, 1, 16384);
        settings.Frames            = Field<uint32_t>(node, "frames", defaults.Frames, 1, 10000);
        settings.Warmup            = Field<uint32_t>(node, "warmup", defaults.Warmup, 0, 10000);
        settings.FramesInFlight =
            Field<uint32_t>(node, "frames_in_flight", defaults.FramesInFlight, 1, MaxFramesInFlight);
        settings.Repeats           = Field<uint32_t>(node, "repeats", defaults.Repeats, 1, MaxRepeats);
        settings.ThresholdPercent  = Field<double>(node, "threshold_percent", defaults.ThresholdPercent, 0, 1000);
        settings.Complex.Instances = Field<uint32_t>(node, "complex_instances", defaults.Complex.Instances, 1, 100000);
        settings.Complex.Lights    = Field<uint32_t>(node, "complex_lights", defaults.Complex.Lights, 0, 256);
        settings.Complex.ShadowedLights =
            Field<uint32_t>(node, "complex_shadowed_lights", defaults.Complex.ShadowedLights, 0, 8);
        settings.Complex.Seed = Field<uint32_t>(node, "complex_seed", defaults.Complex.Seed, 0, UINT32_MAX);
        if (settings.Complex.ShadowedLights > settings.Complex.Lights)
            settings.Complex.ShadowedLights = settings.Complex.Lights;
        settings.Baseline = fs::u8path(Text(node["baseline"]));
        return settings;
    }

    std::optional<std::string> SaveHubSettings(const fs::path& file, const HubSettings& settings)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "scene" << YAML::Value << YAML::DoubleQuoted << settings.Scene;
        out << YAML::Key << "width" << YAML::Value << settings.Width;
        out << YAML::Key << "height" << YAML::Value << settings.Height;
        out << YAML::Key << "frames" << YAML::Value << settings.Frames;
        out << YAML::Key << "warmup" << YAML::Value << settings.Warmup;
        out << YAML::Key << "frames_in_flight" << YAML::Value << settings.FramesInFlight;
        out << YAML::Key << "repeats" << YAML::Value << settings.Repeats;
        out << YAML::Key << "threshold_percent" << YAML::Value << settings.ThresholdPercent;
        out << YAML::Key << "complex_instances" << YAML::Value << settings.Complex.Instances;
        out << YAML::Key << "complex_lights" << YAML::Value << settings.Complex.Lights;
        out << YAML::Key << "complex_shadowed_lights" << YAML::Value << settings.Complex.ShadowedLights;
        out << YAML::Key << "complex_seed" << YAML::Value << settings.Complex.Seed;
        out << YAML::Key << "baseline" << YAML::Value << YAML::DoubleQuoted << settings.Baseline.generic_u8string();
        out << YAML::EndMap;
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << out.c_str() << '\n';
        if (!output.flush())
            return "Cannot write " + file.u8string();
        return std::nullopt;
    }

    std::optional<std::string> SettingsProblem(const HubSettings& settings, const fs::path& projectRoot)
    {
        if (settings.Scene.empty())
            return "Choose a scene.";
        std::error_code error;
        if (settings.Scene == ComplexScene)
        {
            if (settings.Complex.ShadowedLights > settings.Complex.Lights)
                return "Shadowed lights cannot exceed lights.";
        }
        else if (!fs::is_regular_file(projectRoot / fs::u8path(settings.Scene), error))
            return "The scene " + settings.Scene + " does not exist.";
        if (settings.Width < 1 || settings.Height < 1 || settings.Width > 16384 || settings.Height > 16384)
            return "The resolution must be between 1 and 16384 on each side.";
        if (settings.Frames < 1 || settings.Frames > 10000 || settings.Warmup > 10000)
            return "Measured frames must be 1 to 10000 and warmup frames at most 10000.";
        if (settings.FramesInFlight < 1 || settings.FramesInFlight > MaxFramesInFlight)
            return "Frames in flight must be between 1 and " + std::to_string(MaxFramesInFlight) + ".";
        if (settings.Repeats < 1 || settings.Repeats > MaxRepeats)
            return "Repeat 1 to " + std::to_string(MaxRepeats) + " times.";
        if (!std::isfinite(settings.ThresholdPercent) || settings.ThresholdPercent < 0)
            return "The threshold must be a nonnegative percentage.";
        if (!settings.Baseline.empty() && !fs::is_regular_file(settings.Baseline, error))
            return "The baseline " + settings.Baseline.u8string() + " does not exist.";
        return std::nullopt;
    }

    std::vector<std::string> ListScenes(const fs::path& projectRoot, const fs::path& assetDirectory)
    {
        std::vector<std::string> scenes;
        std::error_code          error;
        for (fs::recursive_directory_iterator
                 it(projectRoot / assetDirectory, fs::directory_options::skip_permission_denied, error),
             end;
             !error && it != end;
             it.increment(error))
        {
            std::error_code entry;
            if (it->path().extension() == ".kans" && it->is_regular_file(entry))
                scenes.push_back(it->path().lexically_relative(projectRoot).generic_u8string());
        }
        std::sort(scenes.begin(), scenes.end());
        return scenes;
    }

    std::vector<std::string> RunArguments(const fs::path&    projectFile,
                                          const HubSettings& settings,
                                          const fs::path&    output,
                                          const fs::path&    progress)
    {
        std::vector<std::string> arguments {projectFile.u8string(),
                                            "--benchmark",
                                            settings.Scene,
                                            "--benchmark-frames",
                                            std::to_string(settings.Frames),
                                            "--benchmark-frames-in-flight",
                                            std::to_string(settings.FramesInFlight),
                                            "--benchmark-warmup",
                                            std::to_string(settings.Warmup),
                                            "--benchmark-resolution",
                                            std::to_string(settings.Width) + "x" + std::to_string(settings.Height),
                                            "--benchmark-out",
                                            output.u8string(),
                                            "--benchmark-progress",
                                            progress.u8string()};
        if (settings.Scene == ComplexScene)
            for (const auto& [flag, value] :
                 {std::make_pair("--benchmark-instances", settings.Complex.Instances),
                  std::make_pair("--benchmark-lights", settings.Complex.Lights),
                  std::make_pair("--benchmark-shadowed-lights", settings.Complex.ShadowedLights),
                  std::make_pair("--benchmark-seed", settings.Complex.Seed)})
            {
                arguments.push_back(flag);
                arguments.push_back(std::to_string(value));
            }
        return arguments;
    }

    fs::path BenchmarksDirectory(const fs::path& projectRoot) { return projectRoot / "Benchmarks"; }

    std::string ResultFolderName(int64_t unixTime, std::string_view scene)
    {
        const std::time_t time = static_cast<std::time_t>(unixTime);
        std::tm           local {};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
        std::string name =
            scene == ComplexScene ? std::string(scene) : fs::u8path(std::string(scene)).stem().u8string();
        for (auto& c : name)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x80 && !std::isalnum(byte) && c != '_' && c != '-')
                c = '-';
        }
        return std::string(stamp) + "-" + (name.empty() ? std::string("scene") : name);
    }

    double Median(std::vector<double> values)
    {
        if (values.empty())
            return 0;
        std::sort(values.begin(), values.end());
        const size_t middle = values.size() / 2;
        return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
    }

    std::optional<double> SpreadPercent(const std::vector<double>& values)
    {
        if (values.empty())
            return std::nullopt;
        const double median = Median(values);
        if (median <= 0)
            return std::nullopt;
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        return 100.0 * (*high - *low) / median;
    }

    bool IsUnstable(const std::vector<double>& runMedians)
    {
        const auto spread = SpreadPercent(runMedians);
        if (!spread || runMedians.size() < 2)
            return false;
        const auto [low, high] = std::minmax_element(runMedians.begin(), runMedians.end());
        return *spread > UnstableSpreadPercent && *high - *low > AbsoluteFloorMs;
    }

    YAML::Node
    CombineRuns(const std::vector<YAML::Node>& runs, const std::vector<std::string>& files, int64_t createdUnix)
    {
        if (runs.empty() || files.size() != runs.size())
            throw std::invalid_argument("Combining benchmark runs needs one file name per run");
        for (size_t i = 1; i < runs.size(); ++i)
        {
            const auto difference = Compare(runs.front(), runs[i], 0);
            if (!difference.Mismatches.empty())
                throw std::runtime_error("Run " + std::to_string(i + 1) + " differs from run 1 in " +
                                         difference.Mismatches.front());
        }
        const YAML::Node& first = runs.front();
        YAML::Node        summary;
        summary["schema_version"]    = SchemaVersion;
        summary["machine"]           = Copy(first["machine"]);
        summary["settings"]          = Copy(first["settings"]);
        summary["detailed_counters"] = Copy(first["detailed_counters"]);
        for (const auto* metric : {"cpu_ms", "cpu_busy_ms", "gpu_ms", "gpu_frame_ms"})
            summary["application"][metric] = CombineStatistics(Children(runs, "application", metric));
        for (const auto& metric : CpuMetrics)
            summary["cpu_breakdown"][metric.Key] = CombineStatistics(Children(runs, "cpu_breakdown", metric.Key));
        if (IsMap(first["waits"]))
            for (auto wait : first["waits"])
                summary["waits"][wait.first.Scalar()] = CombineStatistics(Children(runs, "waits", wait.first.Scalar()));

        std::set<std::string> names;
        for (const YAML::Node& run : runs)
            for (auto pass : run["passes"])
                names.insert(pass.first.Scalar());
        auto                       passes = summary["passes"] = YAML::Node(YAML::NodeType::Map);
        std::optional<std::string> topPass;
        std::optional<double>      topMs;
        for (const auto& name : names)
        {
            std::vector<YAML::Node> cpu, gpu;
            std::vector<double>     executed;
            for (const YAML::Node& run : runs)
            {
                const auto pass = Get(run, {"passes", name});
                if (!pass)
                    continue;
                cpu.push_back(pass["cpu_record_ms"]);
                gpu.push_back(pass["gpu_ms"]);
                executed.push_back(pass["executed_frames"].as<double>(0));
            }
            auto entry                 = passes[name];
            entry["executed_frames"]   = uint64_t(std::llround(Median(executed)));
            entry["cpu_record_ms"]     = CombineStatistics(cpu);
            entry["gpu_ms"]            = CombineStatistics(gpu);
            const YAML::Node& combined = entry;
            if (const auto mean = MeanOf(combined["gpu_ms"]); mean && (!topMs || *mean > *topMs))
            {
                topMs   = mean;
                topPass = name;
            }
        }

        uint64_t                   shaders = 0, pipelines = 0, compileFrames = 0, vram = 0;
        std::map<std::string, int> classifications;
        YAML::Node                 hitchItems(YAML::NodeType::Sequence);
        std::optional<double>      worstCompileMs;
        uint32_t                   runNumber = 0;
        for (const YAML::Node& run : runs)
        {
            shaders += Get(run, {"compiles", "shaders"}).as<uint64_t>(0);
            pipelines += Get(run, {"compiles", "pipelines"}).as<uint64_t>(0);
            compileFrames += Get(run, {"bottleneck", "compile_frames"}).as<uint64_t>(0);
            vram = (std::max)(vram, run["vram_peak_bytes"].as<uint64_t>(0));
            ++classifications[Text(Get(run, {"bottleneck", "classification"}))];
            ++runNumber;
            if (const auto worst = Number(Get(run, {"hitches", "worst_compile_cpu_ms"}));
                worst && (!worstCompileMs || *worst > *worstCompileMs))
                worstCompileMs = worst;
            if (const auto hitches = Get(run, {"hitches", "items"}); hitches && hitches.IsSequence())
                for (const auto& item : hitches)
                {
                    auto hitch   = YAML::Clone(item);
                    hitch["run"] = runNumber;
                    hitchItems.push_back(hitch);
                }
        }

        // The representative run is the one whose frame mean is nearest the combined median.
        const auto gpuMeans = RunValues(runs, "gpu_ms", "mean"), cpuMeans = RunValues(runs, "cpu_ms", "mean");
        const bool byGpu          = gpuMeans.size() == runs.size();
        const auto target         = Median(byGpu ? gpuMeans : cpuMeans);
        size_t     representative = 0;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            const auto mean = MeanOf(Get(runs[i], {"application", byGpu ? "gpu_ms" : "cpu_ms"})).value_or(0.0);
            const auto best =
                MeanOf(Get(runs[representative], {"application", byGpu ? "gpu_ms" : "cpu_ms"})).value_or(0.0);
            if (std::abs(mean - target) < std::abs(best - target))
                representative = i;
        }
        const YAML::Node& chosen = runs[representative];
        std::string       label  = Text(Get(chosen, {"bottleneck", "classification"}));
        for (const auto& [name, count] : classifications)
            if (count > classifications[label])
                label = name;

        summary["average_counters"]              = Copy(chosen["average_counters"]);
        summary["vram_peak_bytes"]               = vram;
        summary["compiles"]["shaders"]           = shaders;
        summary["compiles"]["pipelines"]         = pipelines;
        summary["bottleneck"]["classification"]  = JsonString(label);
        summary["bottleneck"]["gpu_top_pass"]    = topPass ? JsonString(*topPass) : YAML::Node(YAML::NodeType::Null);
        summary["bottleneck"]["gpu_top_pass_ms"] = OptionalNumber(topMs);
        summary["bottleneck"]["compile_frames"]  = compileFrames;

        summary["hitches"]["compile_frames"]       = compileFrames;
        summary["hitches"]["worst_compile_cpu_ms"] = OptionalNumber(worstCompileMs);
        summary["hitches"]["items"]                = hitchItems;

        const auto  cpuMedians      = RunValues(runs, "cpu_ms", "median");
        const auto  gpuFrameMedians = RunValues(runs, "gpu_frame_ms", "median");
        const auto* gpuMetric       = gpuFrameMedians.size() == runs.size() ? "gpu_frame_ms" : "gpu_ms";
        const auto  gpuMedians =
            gpuFrameMedians.size() == runs.size() ? gpuFrameMedians : RunValues(runs, gpuMetric, "median");
        const bool cpuUnstable = IsUnstable(cpuMedians), gpuUnstable = IsUnstable(gpuMedians);
        auto section                       = summary["runs"];
        section["count"]                   = runs.size();
        section["created_unix"]            = createdUnix;
        section["method"]                  = JsonString("median_of_runs");
        section["representative_run"]      = representative + 1;
        section["unstable_spread_percent"] = UnstableSpreadPercent;
        section["absolute_floor_ms"]       = AbsoluteFloorMs;
        section["cpu_spread_percent"]            = OptionalNumber(SpreadPercent(cpuMedians));
        section["gpu_spread_percent"]            = OptionalNumber(SpreadPercent(gpuMedians));
        section["stable"]                        = !cpuUnstable && !gpuUnstable;
        section["gpu_stability_metric"]          = JsonString(gpuMetric);
        section["unstable_metrics"]              = YAML::Node(YAML::NodeType::Sequence);
        if (cpuUnstable)
            section["unstable_metrics"].push_back(JsonString("cpu_ms"));
        if (gpuUnstable)
            section["unstable_metrics"].push_back(JsonString(gpuMetric));
        section["items"]                   = YAML::Node(YAML::NodeType::Sequence);
        for (size_t i = 0; i < runs.size(); ++i)
        {
            YAML::Node item;
            item["file"]        = JsonString(files[i]);
            item["cpu_mean_ms"] = OptionalNumber(MeanOf(Get(runs[i], {"application", "cpu_ms"})));
            item["gpu_mean_ms"] = OptionalNumber(MeanOf(Get(runs[i], {"application", "gpu_ms"})));
            item["cpu_median_ms"] = OptionalNumber(Number(Get(runs[i], {"application", "cpu_ms", "median"})));
            item["gpu_median_ms"] = OptionalNumber(Number(Get(runs[i], {"application", gpuMetric, "median"})));
            section["items"].push_back(item);
        }
        return summary;
    }

    Comparison ComparisonFromJson(const YAML::Node& node)
    {
        Comparison comparison;
        comparison.Regressed = node["regressed"].as<bool>(false);
        for (auto mismatch : node["mismatches"])
            comparison.Mismatches.push_back(Text(mismatch));
        for (auto row : node["rows"])
            comparison.Rows.push_back({Text(row["name"]),
                                       Text(row["metric"]),
                                       Text(row["status"]),
                                       Number(row["baseline_ms"]),
                                       Number(row["current_ms"]),
                                       Number(row["delta_ms"]),
                                       Number(row["delta_percent"])});
        return comparison;
    }

    std::vector<HistoryEntry> ListHistory(const fs::path& benchmarksDirectory)
    {
        std::vector<HistoryEntry> entries;
        std::error_code           error;
        if (!fs::is_directory(benchmarksDirectory, error))
            return entries;
        for (fs::directory_iterator it(benchmarksDirectory, error), end; !error && it != end; it.increment(error))
        {
            std::error_code entryError;
            if (!fs::is_directory(it->symlink_status(entryError)))
                continue;
            HistoryEntry entry;
            entry.Folder  = it->path();
            entry.Summary = it->path() / "summary.json";
            if (fs::symlink_status(entry.Summary, entryError).type() == fs::file_type::not_found)
            {
                const auto historyLock = ProductStoreLock::Acquire(benchmarksDirectory / HistoryLockFile);
                if (std::holds_alternative<Scope<ProductStoreLock>>(historyLock))
                {
                    auto liveRun = ProductStoreLock::Acquire(entry.Folder / LiveRunLockFile);
                    if (auto* lock = std::get_if<Scope<ProductStoreLock>>(&liveRun))
                    {
                        // A live session can publish a summary before releasing the run lock.
                        lock->reset();
                        if (fs::symlink_status(entry.Summary, entryError).type() == fs::file_type::not_found)
                        {
                            fs::remove_all(entry.Folder, entryError);
                            continue;
                        }
                    }
                }
            }
            try
            {
                const YAML::Node summary = ReadReport(entry.Summary);
                const auto       runs    = summary["runs"];
                if (!IsMap(runs))
                    continue;
                entry.Created     = runs["created_unix"].as<int64_t>(0);
                entry.Runs        = runs["count"].as<uint32_t>(0);
                entry.Stable      = runs["stable"].as<bool>(true);
                entry.Scene       = Text(Get(summary, {"settings", "scene"}));
                entry.Gpu         = Text(Get(summary, {"machine", "gpu"}));
                entry.Width       = Get(summary, {"settings", "resolution", "width"}).as<uint32_t>(0);
                entry.Height      = Get(summary, {"settings", "resolution", "height"}).as<uint32_t>(0);
                const auto gpu    = Get(summary, {"application", "gpu_ms"});
                entry.GpuMedianMs = IsMap(gpu) ? Number(gpu["median"]) : std::nullopt;
            }
            catch (const std::exception&)
            {
                continue;
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
            if (a.Created != b.Created)
                return a.Created > b.Created;
            return a.Folder.filename().u8string() > b.Folder.filename().u8string();
        });
        return entries;
    }

    double CompletedFraction(uint32_t run, uint32_t runs, const Progress& progress, uint32_t warmup, uint32_t frames)
    {
        const double perRun = double(warmup) + double(frames);
        if (runs == 0 || run == 0 || perRun <= 0)
            return 0;
        double done = 0;
        switch (progress.Stage)
        {
            case Phase::Loading:
                break;
            case Phase::Warmup:
                done = (std::min)(progress.Frame, warmup);
                break;
            case Phase::Measuring:
                done = double(warmup) + (std::min)(progress.Frame, frames);
                break;
            case Phase::Writing:
            case Phase::Done:
                done = perRun;
                break;
        }
        return (std::min)(1.0, (double(run - 1) * perRun + done) / (double(runs) * perRun));
    }

    std::optional<double> RemainingSeconds(double elapsedSeconds, double fraction)
    {
        if (fraction <= 0 || elapsedSeconds <= 0)
            return std::nullopt;
        return elapsedSeconds * (1.0 - fraction) / fraction;
    }

    ResultView MakeResultView(const YAML::Node& node)
    {
        const YAML::Node& summary = node;
        ResultView        view;
        view.Scene      = Text(Get(summary, {"settings", "scene"}));
        view.Gpu        = Text(Get(summary, {"machine", "gpu"}));
        view.Width      = Get(summary, {"settings", "resolution", "width"}).as<uint32_t>(0);
        view.Height     = Get(summary, {"settings", "resolution", "height"}).as<uint32_t>(0);
        view.Bottleneck = Text(Get(summary, {"bottleneck", "classification"}));
        if (const auto top = Get(summary, {"bottleneck", "gpu_top_pass"}); top && top.IsScalar())
            view.TopPass = top.Scalar();
        view.TopPassMs        = Number(Get(summary, {"bottleneck", "gpu_top_pass_ms"}));
        if (const auto slots = Get(summary, {"settings", "frames_in_flight"}); slots && slots.IsScalar())
            view.FramesInFlight = slots.as<uint32_t>();
        view.CpuFrame         = DistributionOf(Get(summary, {"application", "cpu_ms"}));
        view.CpuBusy          = DistributionOf(Get(summary, {"application", "cpu_busy_ms"}));
        view.GpuFrame         = DistributionOf(Get(summary, {"application", "gpu_ms"}));
        view.GpuWholeFrame    = DistributionOf(Get(summary, {"application", "gpu_frame_ms"}));
        for (size_t stage = 0; stage < std::size(CpuMetrics); ++stage)
            view.CpuBreakdown[stage] = DistributionOf(Get(summary, {"cpu_breakdown", CpuMetrics[stage].Key}));
        view.CompileFrames  = Get(summary, {"hitches", "compile_frames"}).as<uint64_t>(0);
        view.WorstCompileMs = Number(Get(summary, {"hitches", "worst_compile_cpu_ms"}));
        if (const auto hitches = Get(summary, {"hitches", "items"}); hitches && hitches.IsSequence())
            for (const YAML::Node& item : hitches)
            {
                HitchResult hitch;
                hitch.Frame = item["frame"].as<uint64_t>(0);
                hitch.Run   = item["run"].as<uint32_t>(0);
                hitch.CpuMs = Number(item["cpu_ms"]);
                hitch.GpuMs = Number(item["gpu_frame_ms"]);
                if (!hitch.GpuMs)
                    hitch.GpuMs = Number(item["gpu_ms"]);
                hitch.Reason           = Text(item["reason"]);
                hitch.ShaderCompiles   = item["shader_compiles"].as<uint64_t>(0);
                hitch.PipelineCompiles = item["pipeline_compiles"].as<uint64_t>(0);
                hitch.OmittedNames     = item["omitted_names"].as<uint64_t>(0);
                for (const auto& name : item["shaders"])
                    hitch.Shaders.push_back(Text(name));
                for (const auto& name : item["pipelines"])
                    hitch.Pipelines.push_back(Text(name));
                view.Hitches.push_back(std::move(hitch));
            }
        view.ShaderCompiles   = Get(summary, {"compiles", "shaders"}).as<uint64_t>(0);
        view.PipelineCompiles = Get(summary, {"compiles", "pipelines"}).as<uint64_t>(0);
        for (auto pass : summary["passes"])
            view.Passes.push_back(
                {pass.first.Scalar(), MeanOf(pass.second["cpu_record_ms"]), MeanOf(pass.second["gpu_ms"])});
        const auto runs = summary["runs"];
        if (IsMap(runs))
        {
            view.Created          = runs["created_unix"].as<int64_t>(0);
            view.Stable           = runs["stable"].as<bool>(true);
            view.CpuSpreadPercent = Number(runs["cpu_spread_percent"]);
            view.GpuSpreadPercent = Number(runs["gpu_spread_percent"]);
            view.BaselineFile     = Text(runs["baseline"]);
            view.ComparisonError  = Text(runs["comparison_error"]);
            view.GpuStabilityMetric = Text(runs["gpu_stability_metric"]);
            if (const auto metrics = runs["unstable_metrics"]; metrics && metrics.IsSequence())
                for (const auto& metric : metrics)
                    view.UnstableMetrics.push_back(Text(metric));
            for (const YAML::Node& item : runs["items"])
                view.Runs.push_back({Text(item["file"]),
                                     Number(item["cpu_mean_ms"]),
                                     Number(item["gpu_mean_ms"]),
                                     Number(item["cpu_median_ms"]),
                                     Number(item["gpu_median_ms"])});
        }
        if (const auto comparison = summary["comparison"]; IsMap(comparison))
        {
            view.Compared         = ComparisonFromJson(comparison);
            view.ThresholdPercent = comparison["threshold_percent"].as<double>(0);
        }
        return view;
    }
} // namespace Kans::Benchmark
