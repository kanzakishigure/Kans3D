#include "KansEditorCore/Profiling/BenchmarkReport.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "KansEditorCore/Profiling/FrameProfilerModel.h"

namespace Kans::Benchmark
{
    using namespace FrameProfiling;
    namespace
    {
        YAML::Node Statistics(std::vector<double>& values)
        {
            const auto stats = Describe(values);
            if (!stats)
                return YAML::Node(YAML::NodeType::Null);
            YAML::Node fields;
            fields["samples"] = stats->Samples;
            fields["mean"]    = stats->Mean;
            fields["median"]  = stats->Median;
            fields["p95"]     = stats->P95;
            fields["p99"]     = stats->P99;
            fields["max"]     = stats->Maximum;
            return fields;
        }

        YAML::Node OptionalNumber(std::optional<double> value)
        {
            return value ? YAML::Node(*value) : YAML::Node(YAML::NodeType::Null);
        }

        void WriteString(std::ostream& out, std::string_view value)
        {
            constexpr char hex[] = "0123456789abcdef";
            out.put('"');
            for (const unsigned char byte : value)
            {
                if (byte == '"' || byte == '\\')
                    out << '\\' << char(byte);
                else if (byte < 32)
                    out << "\\u00" << hex[byte >> 4] << hex[byte & 15];
                else
                    out.put(char(byte));
            }
            out.put('"');
        }

        void WriteJson(std::ostream& out, const YAML::Node& node, size_t depth = 0)
        {
            if (!node || node.IsNull())
            {
                out << "null";
                return;
            }
            if (node.IsScalar())
            {
                if (node.Tag() == "!")
                    WriteString(out, node.Scalar());
                else
                    out << node.Scalar();
                return;
            }
            const bool object = node.IsMap();
            out << (object ? '{' : '[');
            bool first = true;
            for (auto entry : node)
            {
                if (!first)
                    out << ',';
                out << '\n' << std::string((depth + 1) * 2, ' ');
                first = false;
                if (object)
                {
                    WriteString(out, entry.first.Scalar());
                    out << ": ";
                    WriteJson(out, entry.second, depth + 1);
                }
                else
                    WriteJson(out, entry, depth + 1);
            }
            if (!first)
                out << '\n' << std::string(depth * 2, ' ');
            out << (object ? '}' : ']');
        }

        struct FrameTimes
        {
            std::vector<double> Cpu, Gpu, Fence, Acquire, Present, Wait;
        };
        struct PassTimes
        {
            std::vector<double> Cpu, Gpu;
            size_t              ExecutedFrames = 0;
        };

        struct Totals
        {
            std::map<std::string, uint64_t>                                                                 Work;
            std::array<RenderHardware::CategoryDrawStatistics, size_t(RenderHardware::DrawCategory::Count)> Draws {};
            std::array<uint64_t, size_t(ShadowCacheMissReason::Count)>                                      Misses {};
            uint64_t VramPeak = 0;

            void Add(const FrameSample& frame)
            {
                const auto& work = frame.Device();
                Work["draw_calls"] += frame.DrawCalls;
                Work["triangles"] += frame.Triangles;
                Work["drawn_sections"] += frame.Drawn;
                Work["visible_sections"] += frame.InFrustum;
                Work["section_proxies"] += frame.SectionProxies;
                Work["directional_lights"] += frame.DirectionalLights;
                Work["point_lights"] += frame.PointLights;
                Work["spot_lights"] += frame.SpotLights;
                Work["area_lights"] += frame.AreaLights;
                Work["shadowed_lights"] += frame.ShadowedLights;
                Work["shadow_passes"] += frame.ShadowPasses;
                Work["shadow_cache_hits"] += frame.ShadowCacheHits;
                Work["shadow_cache_misses"] += frame.ShadowCacheMisses;
                Work["shadow_full_redraws"] += frame.ShadowFullRedraws;
                Work["pipeline_binds"] += work.PipelineBinds;
                Work["descriptor_set_binds"] += work.DescriptorSetBinds;
                Work["buffer_uploads"] += work.BufferUploads;
                Work["buffer_upload_bytes"] += work.BufferUploadBytes;
                Work["texture_uploads"] += work.TextureUploads;
                Work["texture_upload_bytes"] += work.TextureUploadBytes;
                Work["shader_compiles"] += frame.ShaderCompiles();
                Work["pipeline_compiles"] += frame.PipelineCompiles();
                for (size_t i = 0; i < Draws.size(); ++i)
                    Draws[i] += frame.Categories[i];
                for (size_t i = 1; i < Misses.size(); ++i)
                    Misses[i] += frame.ShadowMissReasons[i];
                VramPeak = (std::max)(VramPeak, frame.VramUsageBytes());
            }

            YAML::Node Averages(double count) const
            {
                YAML::Node fields;
                for (const auto& [name, value] : Work)
                    fields[name] = double(value) / count;
                for (size_t i = 0; i < Draws.size(); ++i)
                {
                    auto category          = fields["draw_categories"][CategoryLabel(RenderHardware::DrawCategory(i))];
                    category["draw_calls"] = double(Draws[i].DrawCalls) / count;
                    category["triangles"]  = double(Draws[i].Triangles) / count;
                    category["instances"]  = double(Draws[i].Instances) / count;
                    category["indirect_draws"] = double(Draws[i].IndirectDraws) / count;
                }
                for (size_t i = 1; i < Misses.size(); ++i)
                    fields["shadow_miss_reasons"][MissReasonLabel(ShadowCacheMissReason(i))] =
                        double(Misses[i]) / count;
                return fields;
            }
        };

        bool Equal(const YAML::Node& left, const YAML::Node& right)
        {
            if (left.Type() != right.Type() || left.size() != right.size())
                return false;
            if (left.IsScalar())
                return left.Scalar() == right.Scalar();
            if (left.IsMap())
            {
                for (auto field : left)
                {
                    const auto peer = right[field.first.Scalar()];
                    if (!peer || !Equal(field.second, peer))
                        return false;
                }
            }
            if (left.IsSequence())
                for (size_t i = 0; i < left.size(); ++i)
                    if (!Equal(left[i], right[i]))
                        return false;
            return true;
        }

        void CheckReport(const YAML::Node& node)
        {
            if (!node.IsMap() || node["schema_version"].as<uint32_t>(0) != SchemaVersion)
                throw std::runtime_error("Unsupported benchmark schema_version");
            for (const auto* name : {"machine", "settings", "application", "passes"})
                if (!node[name].IsMap())
                    throw std::runtime_error(std::string("Benchmark report requires an object named ") + name);
        }

        std::optional<double> Mean(const YAML::Node& parent, const char* metric)
        {
            const auto stats = parent[metric];
            if (stats && stats.IsNull())
                return std::nullopt;
            const double value = stats["mean"].as<double>();
            if (!std::isfinite(value) || value < 0)
                throw std::runtime_error(std::string("Invalid benchmark mean for ") + metric);
            return value;
        }
    } // namespace

    std::optional<Distribution> Describe(std::vector<double>& samples)
    {
        if (samples.empty())
            return std::nullopt;
        std::sort(samples.begin(), samples.end());
        const auto   rank = [&](size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
        const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
        return Distribution {samples.size(), mean, rank(50), rank(95), rank(99), samples.back()};
    }

    YAML::Node JsonString(std::string_view value)
    {
        YAML::Node node {std::string(value)};
        node.SetTag("!");
        return node;
    }

    YAML::Node MakeReport(const FrameHistory& history, const YAML::Node& machine, const YAML::Node& settings)
    {
        if (!history.Size())
            throw std::invalid_argument("Benchmark has no measured frames");
        YAML::Node report;
        report["schema_version"]    = SchemaVersion;
        report["machine"]           = machine;
        report["settings"]          = settings;
        report["detailed_counters"] = history.At(0).Detailed;
        FrameTimes                       times;
        Totals                           totals;
        std::map<std::string, PassTimes> passes;
        YAML::Node                       frames(YAML::NodeType::Sequence);
        for (size_t i = 0; i < history.Size(); ++i)
        {
            const auto& frame = history.At(i);
            totals.Add(frame);
            times.Cpu.push_back(frame.CpuMilliseconds());
            times.Fence.push_back(frame.FenceWaitMilliseconds());
            times.Acquire.push_back(frame.AcquireWaitMilliseconds());
            times.Present.push_back(frame.PresentWaitMilliseconds());
            times.Wait.push_back(frame.WaitMilliseconds());
            if (frame.HasGpu)
                times.Gpu.push_back(frame.GpuMilliseconds);

            YAML::Node sample;
            sample["frame"]             = frame.Frame;
            sample["cpu_ms"]            = frame.CpuMilliseconds();
            sample["gpu_ms"]            = OptionalNumber(InputOf(frame).GpuMilliseconds);
            sample["render_cpu_ms"]     = frame.RenderMilliseconds;
            sample["fence_wait_ms"]     = frame.FenceWaitMilliseconds();
            sample["acquire_wait_ms"]   = frame.AcquireWaitMilliseconds();
            sample["present_wait_ms"]   = frame.PresentWaitMilliseconds();
            sample["draw_calls"]        = frame.DrawCalls;
            sample["triangles"]         = frame.Triangles;
            sample["shader_compiles"]   = frame.ShaderCompiles();
            sample["pipeline_compiles"] = frame.PipelineCompiles();
            frames.push_back(sample);

            // Graph IDs distinguish occurrences. Score all same-named occurrences together once per frame.
            struct NamedTiming
            {
                std::optional<double> Cpu, Gpu;
            };
            std::map<std::string, NamedTiming> names;
            for (uint32_t p = 0; p < frame.PassCount; ++p)
            {
                const auto& pass = history.Passes(i)[p];
                if (pass.Culled)
                    continue;
                auto& item = names[std::string(history.PassName(pass.ID))];
                if (pass.HasCpu())
                    item.Cpu = item.Cpu.value_or(0.0) + pass.CpuMilliseconds;
                if (pass.HasGpu())
                    item.Gpu = item.Gpu.value_or(0.0) + pass.GpuMilliseconds;
            }
            for (const auto& [name, timing] : names)
            {
                auto& series = passes[name];
                ++series.ExecutedFrames;
                if (timing.Cpu)
                    series.Cpu.push_back(*timing.Cpu);
                if (timing.Gpu)
                    series.Gpu.push_back(*timing.Gpu);
            }
        }
        report["frames"]      = frames;
        auto application      = report["application"];
        application["cpu_ms"] = Statistics(times.Cpu);
        application["gpu_ms"] = Statistics(times.Gpu);
        auto waits            = report["waits"];
        waits["total_ms"]     = Statistics(times.Wait);
        waits["fence_ms"]     = Statistics(times.Fence);
        waits["acquire_ms"]   = Statistics(times.Acquire);
        waits["present_ms"]   = Statistics(times.Present);
        auto passFields = report["passes"] = YAML::Node(YAML::NodeType::Map);
        for (auto& [name, series] : passes)
        {
            passFields[name]["executed_frames"] = series.ExecutedFrames;
            passFields[name]["cpu_record_ms"]   = Statistics(series.Cpu);
            passFields[name]["gpu_ms"]          = Statistics(series.Gpu);
        }
        report["average_counters"] = totals.Averages(double(history.Size()));
        report["vram_peak_bytes"]  = totals.VramPeak;
        const auto shaders = totals.Work.at("shader_compiles"), pipelines = totals.Work.at("pipeline_compiles");
        report["compiles"]["shaders"]   = shaders;
        report["compiles"]["pipelines"] = pipelines;
        std::vector<std::pair<GraphDebugID, double>> scratch;
        auto summary                           = Summarize(history, history.Size() - 1, history.Size(), scratch);
        summary.Average.Compiles               = shaders + pipelines;
        report["bottleneck"]["classification"] = JsonString(BottleneckLabel(Classify(summary.Average)));
        report["bottleneck"]["gpu_top_pass"] =
            summary.TopPass ? JsonString(history.PassName(summary.TopPass)) : YAML::Node(YAML::NodeType::Null);
        report["bottleneck"]["gpu_top_pass_ms"] =
            summary.TopPass ? OptionalNumber(summary.TopPassMilliseconds) : YAML::Node(YAML::NodeType::Null);
        report["bottleneck"]["compile_frames"] = summary.CompileFrames;
        return report;
    }

    YAML::Node ReadReport(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot open benchmark report " + file.u8string());
        auto report = YAML::Load(input);
        CheckReport(report);
        return report;
    }

    void WriteReport(const std::filesystem::path& file, const YAML::Node& report)
    {
        CheckReport(report);
        std::ostringstream stream;
        WriteJson(stream, report);
        stream.put('\n');
        const auto             json = stream.str();
        std::vector<std::byte> bytes(json.size());
        std::memcpy(bytes.data(), json.data(), json.size());
        auto temporary = file;
        temporary += ".tmp-" + SourceAssetID::Generate().ToString();
        auto written = WriteProductFile(temporary, bytes);
        if (const auto* error = std::get_if<AssetError>(&written))
            throw std::runtime_error(error->Message);
        struct RemoveTemporary
        {
            const std::filesystem::path& File;
            ~RemoveTemporary()
            {
                std::error_code ignored;
                std::filesystem::remove(File, ignored);
            }
        } cleanup {temporary};
        const auto replaced = ReplaceProductCatalog(temporary, file);
        if (const auto* error = std::get_if<AssetError>(&replaced))
            throw std::runtime_error(error->Message);
    }

    Comparison Compare(const YAML::Node& baseline, const YAML::Node& current, double thresholdPercent)
    {
        CheckReport(baseline);
        CheckReport(current);
        Comparison result;
        for (const auto* group : {"machine", "settings"})
        {
            const auto before = baseline[group], after = current[group];
            for (const auto& [left, right] : {std::make_pair(before, after), std::make_pair(after, before)})
                for (auto field : left)
                {
                    const auto name = field.first.Scalar();
                    if (std::string_view(group) == "machine" && name == "engine_commit")
                        continue;
                    const std::string key  = std::string(group) + "." + name;
                    const auto        peer = right[name];
                    if ((!peer || !Equal(field.second, peer)) &&
                        std::find(result.Mismatches.begin(), result.Mismatches.end(), key) == result.Mismatches.end())
                        result.Mismatches.push_back(key);
                }
        }
        const auto metric =
            [&](const std::string& name, const char* key, const YAML::Node& before, const YAML::Node& after) {
                ComparisonRow row {name, key, "unavailable", Mean(before, key), Mean(after, key), {}, {}};
                if (row.BaselineMs && row.CurrentMs)
                {
                    const double oldValue = *row.BaselineMs;
                    row.DeltaMs           = *row.CurrentMs - oldValue;
                    if (oldValue > 0)
                        row.DeltaPercent = 100.0 * *row.DeltaMs / oldValue;
                    const bool regression =
                        *row.DeltaMs > AbsoluteFloorMs && *row.DeltaMs > oldValue * (thresholdPercent / 100.0);
                    row.Status = regression ? "regression" : *row.DeltaMs < 0 ? "improvement" : "within_threshold";
                    result.Regressed |= regression;
                }
                result.Rows.push_back(std::move(row));
            };
        metric("Frame", "cpu_ms", baseline["application"], current["application"]);
        metric("Frame", "gpu_ms", baseline["application"], current["application"]);
        const auto previous = baseline["passes"], measured = current["passes"];
        for (auto pass : measured)
        {
            const auto name = pass.first.Scalar();
            const auto peer = previous[name];
            if (!peer)
                result.Rows.push_back({name, {}, "new", {}, {}, {}, {}});
            else
            {
                metric(name, "cpu_record_ms", peer, pass.second);
                metric(name, "gpu_ms", peer, pass.second);
            }
        }
        for (auto pass : previous)
            if (!measured[pass.first.Scalar()])
                result.Rows.push_back({pass.first.Scalar(), {}, "missing", {}, {}, {}, {}});
        return result;
    }

    YAML::Node ComparisonJson(const Comparison& comparison, double thresholdPercent)
    {
        YAML::Node node;
        node["threshold_percent"] = thresholdPercent;
        node["absolute_floor_ms"] = AbsoluteFloorMs;
        node["regressed"]         = comparison.Regressed;
        node["mismatches"]        = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& mismatch : comparison.Mismatches)
            node["mismatches"].push_back(JsonString(mismatch));
        node["rows"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& row : comparison.Rows)
        {
            YAML::Node entry;
            entry["name"]          = JsonString(row.Name);
            entry["metric"]        = JsonString(row.Metric);
            entry["status"]        = JsonString(row.Status);
            entry["baseline_ms"]   = OptionalNumber(row.BaselineMs);
            entry["current_ms"]    = OptionalNumber(row.CurrentMs);
            entry["delta_ms"]      = OptionalNumber(row.DeltaMs);
            entry["delta_percent"] = OptionalNumber(row.DeltaPercent);
            node["rows"].push_back(entry);
        }
        return node;
    }
} // namespace Kans::Benchmark
