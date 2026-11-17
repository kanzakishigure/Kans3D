#include "kspch.h"
#include "AnimationProduct.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
namespace Kans
{
    namespace
    {
        void Require(bool value)
        {
            if (!value)
                throw std::runtime_error("Invalid animation Product fields");
        }
        void       Sequence(const YAML::Node& n, size_t max = 1000000) { Require(n.IsSequence() && n.size() <= max); }
        YAML::Node Vec(glm::vec3 v)
        {
            YAML::Node n;
            for (int i = 0; i < 3; ++i)
                n.push_back(v[i]);
            return n;
        }
        YAML::Node Quat(glm::quat q)
        {
            YAML::Node n;
            // Use named components: GLM quaternion operator[] order depends on its build configuration.
            for (float value : {q.x, q.y, q.z, q.w})
                n.push_back(value);
            return n;
        }
        glm::vec3 ReadVec(const YAML::Node& n)
        {
            Sequence(n, 3);
            Require(n.size() == 3);
            return {n[0].as<float>(), n[1].as<float>(), n[2].as<float>()};
        }
        glm::quat ReadQuat(const YAML::Node& n)
        {
            Sequence(n, 4);
            Require(n.size() == 4);
            return {n[3].as<float>(), n[0].as<float>(), n[1].as<float>(), n[2].as<float>()};
        }
        YAML::Node Transform(const JointTransform& t)
        {
            YAML::Node n;
            n["t"] = Vec(t.Translation);
            n["r"] = Quat(t.Rotation);
            n["s"] = Vec(t.Scale);
            return n;
        }
        JointTransform ReadTransform(const YAML::Node& n)
        {
            return {ReadVec(n["t"]), ReadQuat(n["r"]), ReadVec(n["s"])};
        }
        YAML::Node Target(const IKTarget& t)
        {
            YAML::Node n;
            n["joint"]    = t.Joint;
            n["entity"]   = t.Entity;
            n["position"] = Vec(t.Position);
            n["rotation"] = Quat(t.Rotation);
            return n;
        }
        IKTarget ReadTarget(const YAML::Node& n)
        {
            return {n["entity"].as<uint64_t>(),
                    ReadVec(n["position"]),
                    ReadQuat(n["rotation"]),
                    n["joint"] ? n["joint"].as<std::string>() : std::string()};
        }
        AssetID ReadID(const YAML::Node& n)
        {
            auto text = n.as<std::string>();
            auto id   = AssetID::FromString(text);
            Require(id.ToString() == text);
            return id;
        }
        template<class T>
        YAML::Node Track(const AnimationTrack<T>& t)
        {
            YAML::Node n;
            n["interpolation"] = int(t.Interpolation);
            n["keys"]          = YAML::Node(YAML::NodeType::Sequence);
            for (size_t i = 0; i < t.KeyCount(); ++i)
            {
                const auto k = t.KeyAt(i);
                YAML::Node a;
                a.push_back(k.Time);
                if constexpr (std::is_same_v<T, glm::quat>)
                    a.push_back(Quat(k.Value));
                else if constexpr (std::is_same_v<T, float>)
                    a.push_back(k.Value);
                else
                    a.push_back(Vec(k.Value));
                n["keys"].push_back(a);
            }
            return n;
        }
        template<class T>
        AnimationTrack<T> ReadTrack(const YAML::Node& n)
        {
            AnimationTrack<T> t;
            t.Interpolation = TrackInterpolation(n["interpolation"].as<unsigned>());
            Sequence(n["keys"]);
            for (const auto& k : n["keys"])
            {
                Sequence(k, 2);
                Require(k.size() == 2);
                if constexpr (std::is_same_v<T, glm::quat>)
                    t.Keys.push_back({k[0].as<double>(), ReadQuat(k[1])});
                else if constexpr (std::is_same_v<T, float>)
                    t.Keys.push_back({k[0].as<double>(), k[1].as<float>()});
                else
                    t.Keys.push_back({k[0].as<double>(), ReadVec(k[1])});
            }
            return t;
        }
        YAML::Node ShapeKeys(const std::vector<ShapeKey>& keys)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& k : keys)
            {
                YAML::Node v;
                v["name"]   = k.Name;
                v["weight"] = k.DefaultWeight;
                n.push_back(v);
            }
            return n;
        }
        std::vector<ShapeKey> ReadShapeKeys(const YAML::Node& n)
        {
            std::vector<ShapeKey> keys;
            if (!n)
                return keys; // v1 products have no morph channels.
            Sequence(n, 65536);
            for (const auto& v : n)
                keys.push_back({v["name"].as<std::string>(), v["weight"].as<float>()});
            return keys;
        }
        YAML::Node ShapeKeys(const std::vector<ShapeKeyWeight>& keys)
        {
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& k : keys)
            {
                YAML::Node v;
                v["name"]   = k.Name;
                v["weight"] = k.Weight;
                n.push_back(v);
            }
            return n;
        }
        std::vector<ShapeKeyWeight> ReadShapeKeyWeights(const YAML::Node& n)
        {
            std::vector<ShapeKeyWeight> keys;
            if (!n)
                return keys;
            Sequence(n, 65536);
            for (const auto& v : n)
                keys.push_back({v["name"].as<std::string>(), v["weight"].as<float>()});
            return keys;
        }
        YAML::Node Clip(const AnimationClipData& c)
        {
            YAML::Node n;
            n["name"]       = c.Name;
            n["source"]     = c.SourceName;
            n["duration"]   = c.Duration;
            n["frame_rate"] = c.FrameRate;
            n["tracks"]     = YAML::Node(YAML::NodeType::Sequence);
            n["events"]     = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& t : c.Tracks)
            {
                YAML::Node v;
                v["joint"]       = t.Joint;
                v["translation"] = Track(t.Translation);
                v["rotation"]    = Track(t.Rotation);
                v["scale"]       = Track(t.Scale);
                n["tracks"].push_back(v);
            }
            n["morph_tracks"] = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& t : c.MorphTracks)
            {
                YAML::Node v;
                v["shape_key"] = t.ShapeKey;
                v["weight"]    = Track(t.Weight);
                n["morph_tracks"].push_back(v);
            }
            for (const auto& e : c.Events)
            {
                YAML::Node v;
                v["time"]    = e.Time;
                v["name"]    = e.Name;
                v["payload"] = e.Payload;
                n["events"].push_back(v);
            }
            return n;
        }
        void Valid(const AnimationValidation& v)
        {
            if (auto* e = std::get_if<AssetError>(&v))
                throw std::runtime_error(e->Message);
        }
        AnimationClipData ReadClip(const YAML::Node& n)
        {
            AnimationClipData c;
            c.Name       = n["name"].as<std::string>();
            c.SourceName = n["source"].as<std::string>();
            c.Duration   = n["duration"].as<double>();
            c.FrameRate  = n["frame_rate"].as<double>();
            Sequence(n["tracks"], 65536);
            Sequence(n["events"]);
            for (const auto& v : n["tracks"])
            {
                JointAnimationTrack t;
                t.Joint       = v["joint"].as<std::string>();
                t.Translation = ReadTrack<glm::vec3>(v["translation"]);
                t.Rotation    = ReadTrack<glm::quat>(v["rotation"]);
                t.Scale       = ReadTrack<glm::vec3>(v["scale"]);
                c.Tracks.push_back(std::move(t));
            }
            for (const auto& v : n["events"])
                c.Events.push_back(
                    {v["time"].as<double>(), v["name"].as<std::string>(), v["payload"].as<std::string>()});
            if (n["morph_tracks"])
            {
                Sequence(n["morph_tracks"], 65536);
                for (const auto& v : n["morph_tracks"])
                    c.MorphTracks.push_back({v["shape_key"].as<std::string>(), ReadTrack<float>(v["weight"])});
            }
            Valid(ValidateAnimationClip(c));
            return c;
        }
        AnimationProductBytes Bytes(const char* magic, const YAML::Node& data)
        {
            YAML::Node root;
            root["magic"]   = magic;
            root["version"] = 2;
            root["data"]    = data;
            YAML::Emitter out;
            out.SetFloatPrecision(9);
            out.SetDoublePrecision(17);
            out << root;
            Require(out.good() && out.size() <= 64 * 1024 * 1024);
            std::vector<std::byte> bytes(out.size());
            std::memcpy(bytes.data(), out.c_str(), out.size());
            return bytes;
        }
        YAML::Node Root(const std::byte* data, size_t size, const char* magic)
        {
            Require(data && size && size <= 64 * 1024 * 1024);
            auto docs = YAML::LoadAll(std::string(reinterpret_cast<const char*>(data), size));
            Require(docs.size() == 1);
            auto root = docs.front();
            Require(root.IsMap() && root.size() == 3 && root["magic"].as<std::string>() == magic);
            Require(root["version"].as<unsigned>() == 1 || root["version"].as<unsigned>() == 2);
            return root["data"];
        }
        // Long baked FBX clips must not construct millions of YAML nodes. KCLP v3
        // stores explicit little-endian words; small/legacy clips retain their YAML codec.
        struct ClipWriter
        {
            std::vector<std::byte>                                   Data;
            std::unordered_map<const std::vector<double>*, uint32_t> TableIDs;
            void                                                     U16(uint16_t v)
            {
                Data.push_back(std::byte(v & 255));
                Data.push_back(std::byte(v >> 8));
            }
            void U32(uint32_t v)
            {
                for (int i = 0; i < 4; ++i)
                    Data.push_back(std::byte((v >> (i * 8)) & 255));
            }
            void F32(float v)
            {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                U32(bits);
            }
            void F64(double v)
            {
                uint64_t bits;
                std::memcpy(&bits, &v, 8);
                U32(uint32_t(bits));
                U32(uint32_t(bits >> 32));
            }
            void Text(const std::string& v)
            {
                U32(uint32_t(v.size()));
                for (auto c : v)
                    Data.push_back(std::byte(uint8_t(c)));
            }
            template<class T>
            void Value(const T& value)
            {
                if constexpr (std::is_same_v<T, float>)
                    F32(value);
                else if constexpr (std::is_same_v<T, glm::quat>)
                {
                    F32(value.x);
                    F32(value.y);
                    F32(value.z);
                    F32(value.w);
                }
                else
                {
                    F32(value.x);
                    F32(value.y);
                    F32(value.z);
                }
            }
            template<class T>
            void Track(const AnimationTrack<T>& track, bool compact = false)
            {
                if (compact)
                {
                    U32(uint32_t(track.Interpolation));
                    U32(track.PackedCount);
                    U32(track.TimeEncoding);
                    F64(track.Start);
                    F64(track.Interval);
                    U32(track.TimeTable ? TableIDs.at(track.TimeTable.get()) : UINT32_MAX);
                    if (track.TimeEncoding == 4)
                        for (auto index : track.TimeIndices16)
                            U16(index);
                    if (track.TimeEncoding == 5)
                        for (auto index : track.TimeIndices32)
                            U32(index);
                    U32(uint32_t(track.Values.size()));
                    for (const auto& value : track.Values)
                        Value(value);
                    U32(uint32_t(track.RunEnds.size()));
                    for (auto end : track.RunEnds)
                        U32(end);
                    U32(track.RootEndpoints ? 1 : 0);
                    if (track.RootEndpoints)
                        for (const auto& endpoint : *track.RootEndpoints)
                            Value(endpoint);
                    return;
                }
                U32(uint32_t(track.Interpolation));
                U32(uint32_t(track.KeyCount()));
                for (size_t i = 0; i < track.KeyCount(); ++i)
                {
                    const auto key = track.KeyAt(i);
                    F64(key.Time);
                    Value(key.Value);
                }
            }
        };
        struct ClipReader
        {
            const std::byte*                                               Data;
            size_t                                                         Size, At = 0;
            const std::vector<std::shared_ptr<const std::vector<double>>>* Tables = nullptr;
            uint16_t                                                       U16()
            {
                Require(At <= Size && Size - At >= 2);
                const auto low = uint16_t(Data[At++]);
                return low | (uint16_t(Data[At++]) << 8);
            }
            uint32_t U32()
            {
                Require(At <= Size && Size - At >= 4);
                uint32_t v = 0;
                for (int i = 0; i < 4; ++i)
                    v |= uint32_t(Data[At++]) << (8 * i);
                return v;
            }
            float F32()
            {
                auto  bits = U32();
                float v;
                std::memcpy(&v, &bits, 4);
                return v;
            }
            double F64()
            {
                uint64_t bits = U32();
                bits |= uint64_t(U32()) << 32;
                double v;
                std::memcpy(&v, &bits, 8);
                return v;
            }
            uint32_t Count(uint32_t maximum, size_t minimumBytes)
            {
                auto count = U32();
                Require(count <= maximum && count <= (Size - At) / minimumBytes);
                return count;
            }
            std::string Text()
            {
                const auto  size = Count(4096, 1);
                std::string result(reinterpret_cast<const char*>(Data + At), size);
                At += size;
                return result;
            }
            template<class T>
            T Value()
            {
                if constexpr (std::is_same_v<T, float>)
                    return F32();
                else if constexpr (std::is_same_v<T, glm::quat>)
                {
                    const float x = F32(), y = F32(), z = F32(), w = F32();
                    return {w, x, y, z};
                }
                else
                {
                    const float x = F32(), y = F32(), z = F32();
                    return {x, y, z};
                }
            }
            template<class T>
            AnimationTrack<T> Track(bool compact = false, bool shared = false)
            {
                AnimationTrack<T> result;
                const auto        interpolation = U32();
                Require(interpolation <= uint32_t(TrackInterpolation::Spherical));
                result.Interpolation        = TrackInterpolation(interpolation);
                constexpr size_t components = std::is_same_v<T, float> ? 1 : std::is_same_v<T, glm::quat> ? 4 : 3;
                if (compact)
                {
                    result.PackedCount = U32();
                    Require(result.PackedCount <= 1000000);
                    result.TimeEncoding = U32();
                    Require(result.TimeEncoding <= (shared ? 5u : 2u));
                    result.Start    = F64();
                    result.Interval = F64();
                    if (shared)
                    {
                        const auto table = U32();
                        if (result.TimeEncoding >= 3)
                        {
                            Require(Tables && table < Tables->size());
                            result.TimeTable = (*Tables)[table];
                            if (result.TimeEncoding == 4)
                            {
                                Require(result.PackedCount <= (Size - At) / 2);
                                result.TimeIndices16.reserve(result.PackedCount);
                                for (uint32_t i = 0; i < result.PackedCount; ++i)
                                    result.TimeIndices16.push_back(U16());
                            }
                            if (result.TimeEncoding == 5)
                            {
                                Require(result.PackedCount <= (Size - At) / 4);
                                result.TimeIndices32.reserve(result.PackedCount);
                                for (uint32_t i = 0; i < result.PackedCount; ++i)
                                    result.TimeIndices32.push_back(U32());
                            }
                        }
                        else
                            Require(table == UINT32_MAX && (!result.PackedCount || result.TimeEncoding != 0));
                    }
                    else
                    {
                        const auto count = Count(result.PackedCount, 8);
                        result.Times.reserve(count);
                        for (uint32_t i = 0; i < count; ++i)
                            result.Times.push_back(F64());
                    }
                    auto count = Count(result.PackedCount, components * 4);
                    result.Values.reserve(count);
                    for (uint32_t i = 0; i < count; ++i)
                        result.Values.push_back(Value<T>());
                    count = Count(result.PackedCount, 4);
                    result.RunEnds.reserve(count);
                    for (uint32_t i = 0; i < count; ++i)
                        result.RunEnds.push_back(U32());
                    if (shared)
                    {
                        const auto endpoints = U32();
                        Require(endpoints <= 1);
                        if (endpoints)
                        {
                            const auto first = Value<T>(), last = Value<T>();
                            result.RootEndpoints = std::array<T, 2> {first, last};
                        }
                    }
                    if (!result.PackedCount)
                        Require(result.Values.empty() && result.Times.empty() && result.RunEnds.empty() &&
                                !result.TimeTable);
                    return result;
                }
                const auto count = Count(1000000, 8 + components * 4);
                result.Keys.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    AnimationKey<T> key;
                    key.Time  = F64();
                    key.Value = Value<T>();
                    result.Keys.push_back(key);
                }
                return result;
            }
        };
        struct ClipTimePool
        {
            std::vector<std::shared_ptr<const std::vector<double>>> Tables;
            std::unordered_map<uint64_t, std::vector<size_t>>       Buckets;
            std::shared_ptr<const std::vector<double>>              Intern(std::vector<double> times)
            {
                uint64_t hash = 1469598103934665603ull;
                for (double time : times)
                {
                    uint64_t bits;
                    std::memcpy(&bits, &time, 8);
                    hash = (hash ^ bits) * 1099511628211ull;
                }
                auto& bucket = Buckets[hash];
                for (auto index : bucket)
                    if (Tables[index]->size() == times.size() &&
                        std::memcmp(Tables[index]->data(), times.data(), times.size() * 8) == 0)
                        return Tables[index];
                bucket.push_back(Tables.size());
                Tables.push_back(std::make_shared<const std::vector<double>>(std::move(times)));
                return Tables.back();
            }
            template<class T>
            void Share(AnimationTrack<T>& track, const AnimationTrack<T>& source)
            {
                const auto count = track.KeyCount();
                if (!count || track.TimeEncoding == 1 || track.TimeEncoding == 2)
                    return;
                if (count == 1)
                {
                    track.Start        = track.KeyTime(0);
                    track.Interval     = 1;
                    track.TimeEncoding = 1;
                    track.TimeTable.reset();
                    track.TimeIndices16.clear();
                    track.TimeIndices32.clear();
                    const double reconstructed = track.Start + 0.;
                    if (std::memcmp(&reconstructed, &track.Start, sizeof(double)) != 0)
                    {
                        // -0 + 0 reconstructs +0. A one-entry table preserves that sign bit.
                        track.TimeTable    = Intern({track.Start});
                        track.TimeEncoding = 3;
                    }
                    std::vector<double>().swap(track.Times);
                    return;
                }
                std::vector<double> times;
                if (source.TimeTable)
                    times = *source.TimeTable;
                else
                {
                    times.reserve(source.KeyCount());
                    for (size_t i = 0; i < source.KeyCount(); ++i)
                        times.push_back(source.KeyTime(i));
                }
                auto                  table = Intern(std::move(times));
                std::vector<uint32_t> indices;
                indices.reserve(count);
                size_t index = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    const double time = track.KeyTime(i);
                    while (index < table->size() && (*table)[index] < time)
                        ++index;
                    Require(index < table->size() && std::memcmp(&(*table)[index], &time, 8) == 0);
                    indices.push_back(uint32_t(index++));
                }
                track.TimeTable = std::move(table);
                track.TimeIndices16.clear();
                track.TimeIndices32.clear();
                track.TimeEncoding = count == track.TimeTable->size() ? 3 : track.TimeTable->size() <= 65536 ? 4 : 5;
                if (track.TimeEncoding == 4)
                    track.TimeIndices16.assign(indices.begin(), indices.end());
                if (track.TimeEncoding == 5)
                    track.TimeIndices32 = std::move(indices);
                std::vector<double>().swap(track.Times);
            }
        };
        void ShareClipTimes(AnimationClipData& clip, const AnimationClipData& source)
        {
            ClipTimePool pool;
            for (size_t t = 0; t < clip.Tracks.size(); ++t)
            {
                pool.Share(clip.Tracks[t].Translation, source.Tracks[t].Translation);
                pool.Share(clip.Tracks[t].Rotation, source.Tracks[t].Rotation);
                pool.Share(clip.Tracks[t].Scale, source.Tracks[t].Scale);
            }
            for (size_t t = 0; t < clip.MorphTracks.size(); ++t)
                pool.Share(clip.MorphTracks[t].Weight, source.MorphTracks[t].Weight);
            clip.TimeTables = std::move(pool.Tables);
        }
        std::vector<std::byte> PackedClip(const AnimationClipData& c, bool forceOff = false)
        {
            ClipWriter out;
            out.U32(0x504c434b);
            const bool compact = !forceOff && c.Compression.Mode != AnimationCompressionMode::Off;
            out.U32(compact ? 5 : 3);
            out.Text(c.Name);
            out.Text(c.SourceName);
            out.F64(c.Duration);
            out.F64(c.FrameRate);
            if (compact)
            {
                out.U32(uint32_t(c.TimeTables.size()));
                for (const auto& table : c.TimeTables)
                {
                    out.TableIDs.emplace(table.get(), uint32_t(out.TableIDs.size()));
                    out.U32(uint32_t(table->size()));
                    for (double time : *table)
                        out.F64(time);
                }
            }
            out.U32(uint32_t(c.Tracks.size()));
            for (const auto& t : c.Tracks)
            {
                out.Text(t.Joint);
                out.Track(t.Translation, compact);
                out.Track(t.Rotation, compact);
                out.Track(t.Scale, compact);
            }
            out.U32(uint32_t(c.MorphTracks.size()));
            for (const auto& t : c.MorphTracks)
            {
                out.Text(t.ShapeKey);
                out.Track(t.Weight, compact);
            }
            out.U32(uint32_t(c.Events.size()));
            for (const auto& e : c.Events)
            {
                out.F64(e.Time);
                out.Text(e.Name);
                out.Text(e.Payload);
            }
            if (compact)
            {
                const auto& stats = c.Compression;
                out.U32(uint32_t(stats.Mode));
                out.U32(uint32_t(stats.UncompressedBytes));
                out.U32(uint32_t(stats.CompressedBytes));
                out.F32(stats.MaxPositionError);
                out.F64(stats.MaxPositionTime);
                out.Text(stats.Joint);
                out.F32(stats.MaxMorphError);
                out.Text(stats.ShapeKey);
            }
            Require(out.Data.size() <= 512 * 1024 * 1024);
            return std::move(out.Data);
        }
        void SetOffStatistics(AnimationClipData& clip, size_t size)
        {
            clip.Compression.UncompressedBytes = clip.Compression.CompressedBytes = size;
            clip.Compression.Joint    = clip.Tracks.empty() ? "" : clip.Tracks[0].Joint;
            clip.Compression.ShapeKey = clip.MorphTracks.empty() ? "" : clip.MorphTracks[0].ShapeKey;
        }
        AnimationClipData ReadPackedClip(const std::byte* bytes, size_t size)
        {
            Require(size <= 512 * 1024 * 1024);
            ClipReader in {bytes, size};
            Require(in.U32() == 0x504c434b);
            const auto version = in.U32();
            Require(version == 3 || version == 4 || version == 5);
            const bool        compact = version >= 4;
            AnimationClipData c;
            c.Name       = in.Text();
            c.SourceName = in.Text();
            c.Duration   = in.F64();
            c.FrameRate  = in.F64();
            if (version >= 5)
            {
                const auto tables = in.Count(262144, 4);
                c.TimeTables.reserve(tables);
                for (uint32_t t = 0; t < tables; ++t)
                {
                    const auto count = in.Count(1000000, 8);
                    auto       times = std::make_shared<std::vector<double>>();
                    times->reserve(count);
                    double previous = -1;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const double time = in.F64();
                        Require(std::isfinite(time) && time >= 0 && time <= c.Duration && time > previous);
                        times->push_back(time);
                        previous = time;
                    }
                    c.TimeTables.push_back(std::move(times));
                }
                in.Tables = &c.TimeTables;
            }
            auto count = in.Count(65536, 28);
            c.Tracks.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                JointAnimationTrack t;
                t.Joint       = in.Text();
                t.Translation = in.Track<glm::vec3>(compact, version >= 5);
                t.Rotation    = in.Track<glm::quat>(compact, version >= 5);
                t.Scale       = in.Track<glm::vec3>(compact, version >= 5);
                c.Tracks.push_back(std::move(t));
            }
            count = in.Count(65536, 12);
            c.MorphTracks.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                MorphAnimationTrack t;
                t.ShapeKey = in.Text();
                t.Weight   = in.Track<float>(compact, version >= 5);
                c.MorphTracks.push_back(std::move(t));
            }
            count = in.Count(1000000, 16);
            c.Events.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                AnimationEvent e;
                e.Time    = in.F64();
                e.Name    = in.Text();
                e.Payload = in.Text();
                c.Events.push_back(std::move(e));
            }
            if (compact)
            {
                auto&      stats = c.Compression;
                const auto mode  = in.U32();
                Require(mode >= 1 && mode <= 3);
                stats.Mode              = AnimationCompressionMode(mode);
                stats.UncompressedBytes = in.U32();
                stats.CompressedBytes   = in.U32();
                stats.MaxPositionError  = in.F32();
                stats.MaxPositionTime   = in.F64();
                stats.Joint             = in.Text();
                stats.MaxMorphError     = in.F32();
                stats.ShapeKey          = in.Text();
                Require(stats.CompressedBytes == size && stats.UncompressedBytes > 0 &&
                        std::isfinite(stats.MaxPositionError) && stats.MaxPositionError >= 0 &&
                        std::isfinite(stats.MaxMorphError) && stats.MaxMorphError >= 0 &&
                        std::isfinite(stats.MaxPositionTime) && stats.MaxPositionTime >= 0 &&
                        stats.MaxPositionTime <= c.Duration);
            }
            else
                SetOffStatistics(c, size);
            Require(in.At == size);
            Valid(ValidateAnimationClip(c));
            return c;
        }
        std::vector<std::byte> OffClip(const AnimationClipData& clip)
        {
            size_t keys = 0;
            for (const auto& t : clip.Tracks)
                keys += t.Translation.KeyCount() + t.Rotation.KeyCount() + t.Scale.KeyCount();
            for (const auto& t : clip.MorphTracks)
                keys += t.Weight.KeyCount();
            if (keys > 100000)
                return PackedClip(clip, true);
            auto encoded = Bytes("KCLP", Clip(clip));
            if (auto* error = std::get_if<AssetError>(&encoded))
                throw std::runtime_error(error->Message);
            return std::get<std::vector<std::byte>>(std::move(encoded));
        }
        size_t OffClipSize(const AnimationClipData& clip)
        {
            size_t keys = 0;
            for (const auto& track : clip.Tracks)
                keys += track.Translation.KeyCount() + track.Rotation.KeyCount() + track.Scale.KeyCount();
            for (const auto& track : clip.MorphTracks)
                keys += track.Weight.KeyCount();
            if (keys <= 100000)
                return OffClip(clip).size();
            // Packed v3 has a fixed-width record for every logical key. Count the
            // exact Off encoding without serializing a potentially 100+ MB temporary.
            auto text      = [](const std::string& value) { return 4 + value.size(); };
            auto trackSize = [](const auto& track) {
                using T                     = decltype(track.KeyValue(0));
                constexpr size_t components = std::is_same_v<T, float> ? 1 : std::is_same_v<T, glm::quat> ? 4 : 3;
                return 8 + track.KeyCount() * (8 + components * 4);
            };
            size_t size = 8 + text(clip.Name) + text(clip.SourceName) + 16 + 4;
            for (const auto& track : clip.Tracks)
                size += text(track.Joint) + trackSize(track.Translation) + trackSize(track.Rotation) +
                        trackSize(track.Scale);
            size += 4;
            for (const auto& track : clip.MorphTracks)
                size += text(track.ShapeKey) + trackSize(track.Weight);
            size += 4;
            for (const auto& event : clip.Events)
                size += 8 + text(event.Name) + text(event.Payload);
            Require(size <= 512 * 1024 * 1024);
            return size;
        }
        AssetError Error(const std::exception& e) { return {AssetErrorCode::InvalidData, e.what()}; }
    } // namespace
    YAML::Node EncodeAnimationControllerNode(const AnimationControllerData& c)
    {
        Valid(ValidateAnimationController(c));
        YAML::Node n;
        n["root_motion_joint"]               = c.RootMotionJoint;
        n["root_motion_planar"]              = c.RootMotionPlanar;
        n["root_motion_keep_initial_offset"] = c.RootMotionKeepInitialOffset;
        n["parameters"]                      = YAML::Node(YAML::NodeType::Sequence);
        n["motions"]                         = YAML::Node(YAML::NodeType::Sequence);
        n["layers"]                          = YAML::Node(YAML::NodeType::Sequence);
        if (!c.Expressions.empty())
            n["expressions"] = YAML::Node(YAML::NodeType::Sequence);
        if (!c.TwoBoneConstraints.empty())
            n["two_bone_ik"] = YAML::Node(YAML::NodeType::Sequence);
        if (!c.LookAtConstraints.empty())
            n["look_at_ik"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& ik : c.TwoBoneConstraints)
        {
            YAML::Node v;
            v["name"]            = ik.Name;
            v["root"]            = ik.Root;
            v["middle"]          = ik.Middle;
            v["end"]             = ik.End;
            v["target"]          = Target(ik.Target);
            v["pole_entity"]     = ik.PoleEntity;
            v["pole_direction"]  = Vec(ik.PoleDirection);
            v["weight"]          = ik.Weight;
            v["rotation_weight"] = ik.RotationWeight;
            v["stretch"]         = ik.Stretch;
            n["two_bone_ik"].push_back(v);
        }
        for (const auto& ik : c.LookAtConstraints)
        {
            YAML::Node v;
            v["name"]        = ik.Name;
            v["target"]      = Target(ik.Target);
            v["world_up"]    = Vec(ik.WorldUp);
            v["weight"]      = ik.Weight;
            v["angle_limit"] = ik.AngleLimit;
            v["bones"]       = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& b : ik.Bones)
            {
                YAML::Node a;
                a["joint"]   = b.Joint;
                a["aim_child"] = b.AimChild;
                a["weight"]  = b.Weight;
                a["forward"] = Vec(b.Forward);
                a["up"]      = Vec(b.Up);
                v["bones"].push_back(a);
            }
            n["look_at_ik"].push_back(v);
        }
        for (const auto& p : c.Expressions)
        {
            YAML::Node v;
            v["name"]    = p.Name;
            v["weights"] = ShapeKeys(p.Weights);
            n["expressions"].push_back(v);
        }
        for (const auto& p : c.Parameters)
        {
            YAML::Node v;
            v["name"]    = p.Name;
            v["type"]    = int(p.Type);
            v["default"] = p.Default;
            n["parameters"].push_back(v);
        }
        for (const auto& m : c.Motions)
        {
            YAML::Node v;
            v["type"] = int(m.Type);
            v["clip"] = m.Clip.ToString();
            if (m.InlineClip)
                v["inline_clip"] = Clip(*m.InlineClip);
            v["strip_prefix"] = m.Mapping.StripPrefix;
            v["mapping"]      = m.Mapping.Names;
            v["x"]            = m.ParameterX;
            v["y"]            = m.ParameterY;
            v["children"]     = YAML::Node(YAML::NodeType::Sequence);
            for (const auto& child : m.Children)
            {
                YAML::Node a;
                a["motion"]    = child.Motion;
                a["threshold"] = child.Threshold;
                a["x"]         = child.Position.x;
                a["y"]         = child.Position.y;
                v["children"].push_back(a);
            }
            n["motions"].push_back(v);
        }
        for (const auto& l : c.Layers)
        {
            YAML::Node v;
            v["name"] = l.Name;
            if (!l.Slot.empty())
                v["slot"] = l.Slot;
            v["weight"]         = l.Weight;
            v["mode"]           = int(l.Mode);
            v["entry"]          = l.EntryState;
            v["mask"]           = YAML::Node(YAML::NodeType::Sequence);
            v["states"]         = YAML::Node(YAML::NodeType::Sequence);
            v["transitions"]    = YAML::Node(YAML::NodeType::Sequence);
            v["shape_key_mask"] = ShapeKeys(l.ShapeKeyMask);
            for (const auto& m : l.Mask)
            {
                YAML::Node a;
                a["joint"]       = m.Joint;
                a["weight"]      = m.Weight;
                a["descendants"] = m.IncludeChildren;
                v["mask"].push_back(a);
            }
            for (const auto& s : l.States)
            {
                YAML::Node a;
                a["name"]   = s.Name;
                a["motion"] = s.Motion;
                a["speed"]  = s.Speed;
                a["loop"]   = s.Loop;
                if (s.EditorPosition)
                {
                    a["editor_x"] = s.EditorPosition->x;
                    a["editor_y"] = s.EditorPosition->y;
                }
                v["states"].push_back(a);
            }
            for (const auto& t : l.Transitions)
            {
                YAML::Node a;
                a["source"]       = t.Source;
                a["target"]       = t.Target;
                a["duration"]     = t.Duration;
                a["interruption"] = int(t.Interruption);
                a["self"]         = t.CanTransitionToSelf;
                if (t.ExitTime)
                    a["exit"] = *t.ExitTime;
                a["conditions"] = YAML::Node(YAML::NodeType::Sequence);
                for (const auto& cond : t.Conditions)
                {
                    YAML::Node b;
                    b["parameter"] = cond.Parameter;
                    b["op"]        = int(cond.Op);
                    b["value"]     = cond.Value;
                    a["conditions"].push_back(b);
                }
                v["transitions"].push_back(a);
            }
            n["layers"].push_back(v);
        }
        return n;
    }
    AnimationControllerData DecodeAnimationControllerNode(const YAML::Node& n)
    {
        AnimationControllerData c;
        c.RootMotionJoint  = n["root_motion_joint"].as<std::string>();
        c.RootMotionPlanar = n["root_motion_planar"] ? n["root_motion_planar"].as<bool>() : false;
        c.RootMotionKeepInitialOffset =
            n["root_motion_keep_initial_offset"] ? n["root_motion_keep_initial_offset"].as<bool>() : false;
        Sequence(n["parameters"], 4096);
        Sequence(n["motions"], 4096);
        Sequence(n["layers"], 64);
        if (n["expressions"])
        {
            Sequence(n["expressions"], 4096);
            for (const auto& p : n["expressions"])
                c.Expressions.push_back({p["name"].as<std::string>(), ReadShapeKeyWeights(p["weights"])});
        }
        if (n["two_bone_ik"])
        {
            Sequence(n["two_bone_ik"], 256);
            for (const auto& v : n["two_bone_ik"])
            {
                TwoBoneIK ik;
                ik.Name           = v["name"].as<std::string>();
                ik.Root           = v["root"].as<std::string>();
                ik.Middle         = v["middle"].as<std::string>();
                ik.End            = v["end"].as<std::string>();
                ik.Target         = ReadTarget(v["target"]);
                ik.PoleEntity     = v["pole_entity"].as<uint64_t>();
                ik.PoleDirection  = ReadVec(v["pole_direction"]);
                ik.Weight         = v["weight"].as<float>();
                ik.RotationWeight = v["rotation_weight"].as<float>();
                ik.Stretch        = v["stretch"].as<bool>();
                c.TwoBoneConstraints.push_back(std::move(ik));
            }
        }
        if (n["look_at_ik"])
        {
            Sequence(n["look_at_ik"], 256);
            for (const auto& v : n["look_at_ik"])
            {
                LookAtIK ik;
                ik.Name       = v["name"].as<std::string>();
                ik.Target     = ReadTarget(v["target"]);
                ik.WorldUp    = ReadVec(v["world_up"]);
                ik.Weight     = v["weight"].as<float>();
                ik.AngleLimit = v["angle_limit"].as<float>();
                Sequence(v["bones"], 64);
                for (const auto& a : v["bones"])
                    ik.Bones.push_back({a["joint"].as<std::string>(),
                                        a["weight"].as<float>(),
                                        ReadVec(a["forward"]),
                                        ReadVec(a["up"]),
                                        a["aim_child"] ? a["aim_child"].as<std::string>() : std::string()});
                c.LookAtConstraints.push_back(std::move(ik));
            }
        }
        for (const auto& v : n["parameters"])
            c.Parameters.push_back({v["name"].as<std::string>(),
                                    AnimationParameterType(v["type"].as<unsigned>()),
                                    v["default"].as<double>()});
        for (const auto& v : n["motions"])
        {
            AnimationMotion m;
            m.Type = AnimationMotionType(v["type"].as<unsigned>());
            m.Clip = ReadID(v["clip"]);
            if (v["inline_clip"])
                m.InlineClip = ReadClip(v["inline_clip"]);
            m.Mapping.StripPrefix = v["strip_prefix"].as<std::string>();
            m.Mapping.Names       = v["mapping"].as<std::map<std::string, std::string>>();
            m.ParameterX          = v["x"].as<std::string>();
            m.ParameterY          = v["y"].as<std::string>();
            Sequence(v["children"], 256);
            for (const auto& a : v["children"])
                m.Children.push_back(
                    {a["motion"].as<uint32_t>(), a["threshold"].as<float>(), {a["x"].as<float>(), a["y"].as<float>()}});
            c.Motions.push_back(std::move(m));
        }
        for (const auto& v : n["layers"])
        {
            AnimationLayer l;
            if (v["slot"])
                l.Slot = v["slot"].as<std::string>();
            l.ShapeKeyMask = ReadShapeKeyWeights(v["shape_key_mask"]);
            l.Name         = v["name"].as<std::string>();
            l.Weight       = v["weight"].as<float>();
            l.Mode         = AnimationLayerMode(v["mode"].as<unsigned>());
            l.EntryState   = v["entry"].as<uint32_t>();
            Sequence(v["mask"], 65536);
            Sequence(v["states"], 4096);
            Sequence(v["transitions"], 16384);
            for (const auto& a : v["mask"])
                l.Mask.push_back({a["joint"].as<std::string>(), a["weight"].as<float>(), a["descendants"].as<bool>()});
            for (const auto& a : v["states"])
            {
                l.States.push_back({a["name"].as<std::string>(),
                                    a["motion"].as<uint32_t>(),
                                    a["speed"].as<double>(),
                                    a["loop"].as<bool>()});
                if (a["editor_x"] && a["editor_y"])
                    l.States.back().EditorPosition = glm::vec2(a["editor_x"].as<float>(), a["editor_y"].as<float>());
            }
            for (const auto& a : v["transitions"])
            {
                AnimationTransition t;
                t.Source              = a["source"].as<int32_t>();
                t.Target              = a["target"].as<uint32_t>();
                t.Duration            = a["duration"].as<double>();
                t.Interruption        = AnimationInterruption(a["interruption"].as<unsigned>());
                t.CanTransitionToSelf = a["self"].as<bool>();
                if (a["exit"])
                    t.ExitTime = a["exit"].as<double>();
                Sequence(a["conditions"], 4096);
                for (const auto& b : a["conditions"])
                    t.Conditions.push_back({b["parameter"].as<std::string>(),
                                            AnimationConditionOp(b["op"].as<unsigned>()),
                                            b["value"].as<double>()});
                l.Transitions.push_back(std::move(t));
            }
            c.Layers.push_back(std::move(l));
        }
        Valid(ValidateAnimationController(c));
        if (!c.Expressions.empty() && Log::GetCoreLogger())
            CORE_WARN("Controller Expressions are legacy; move presets to an Expression asset");
        return c;
    }
    AnimationProductBytes EncodeSkeletonProduct(const SkeletonData& s)
    {
        try
        {
            Valid(ValidateSkeleton(s));
            YAML::Node n(YAML::NodeType::Sequence);
            for (const auto& j : s.Joints)
            {
                YAML::Node v;
                v["name"]   = j.Name;
                v["path"]   = j.Path;
                v["node"]   = j.NodeKey;
                v["parent"] = j.Parent;
                v["rest"]   = Transform(j.RestLocal);
                v["bind"]   = Transform(j.BindLocal);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        v["inverse_bind"].push_back(j.InverseBind[c][r]);
                n.push_back(v);
            }
            YAML::Node data;
            data["joints"]     = n;
            data["shape_keys"] = ShapeKeys(s.ShapeKeys);
            return Bytes("KSKL", data);
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    namespace
    {
        template<class T>
        bool SameBits(const T& a, const T& b)
        {
            return std::memcmp(&a, &b, sizeof(T)) == 0;
        }
        template<class T>
        AnimationTrack<T> CompactTrack(const AnimationTrack<T>& source)
        {
            AnimationTrack<T> result;
            result.Interpolation = source.Interpolation;
            result.RootEndpoints = source.RootEndpoints;
            const auto count     = source.KeyCount();
            result.PackedCount   = uint32_t(count);
            if (!count)
                return result;
            result.Start = source.KeyTime(0);
            if (count > 1)
            {
                // Try multiplication and division, accepting a formula only if EVERY
                // reconstructed double is bit-identical (including the endpoints).
                for (uint32_t encoding : {1u, 2u})
                {
                    result.TimeEncoding = encoding;
                    const double span   = source.KeyTime(count - 1) - result.Start;
                    result.Interval     = encoding == 1 ? span / double(count - 1) : double(count - 1) / span;
                    bool exact          = std::isfinite(result.Interval) && result.Interval > 0;
                    for (size_t i = 0; exact && i < count; ++i)
                        exact = SameBits(source.KeyTime(i), result.KeyTime(i));
                    if (exact)
                        break;
                    result.TimeEncoding = 0;
                }
            }
            if (!result.TimeEncoding)
            {
                result.Times.reserve(count);
                for (size_t i = 0; i < count; ++i)
                    result.Times.push_back(source.KeyTime(i));
            }
            std::vector<T>        values;
            std::vector<uint32_t> ends;
            for (size_t i = 0; i < count; ++i)
            {
                const auto value = source.KeyValue(i);
                if (values.empty() || !SameBits(values.back(), value))
                {
                    values.push_back(value);
                    ends.push_back(uint32_t(i + 1));
                }
                else
                    ends.back() = uint32_t(i + 1);
            }
            if (values.size() * (sizeof(T) + 4) < count * sizeof(T))
            {
                result.Values.assign(values.begin(), values.end());
                result.RunEnds.assign(ends.begin(), ends.end());
            }
            else
            {
                result.Values.reserve(count);
                for (size_t i = 0; i < count; ++i)
                    result.Values.push_back(source.KeyValue(i));
            }
            return result;
        }
        template<class T>
        bool TrackVaries(const AnimationTrack<T>& track)
        {
            for (size_t i = 1; i < track.KeyCount(); ++i)
                if (!SameBits(track.KeyValue(0), track.KeyValue(i)))
                    return true;
            return false;
        }
        template<class T>
        float TrackErrorSquared(const T& a, const T& b)
        {
            if constexpr (std::is_same_v<T, float>)
                return (a - b) * (a - b);
            else if constexpr (std::is_same_v<T, glm::quat>)
            {
                const auto delta = a - (glm::dot(a, b) < 0 ? -b : b);
                return 4 * glm::dot(delta, delta); // Angular chord metric; final certification uses model-space points.
            }
            else
                return glm::dot(a - b, a - b);
        }
        template<class T>
        AnimationTrack<T> ReduceTrack(const AnimationTrack<T>& source, float tolerance, float constantTolerance = -1)
        {
            if (constantTolerance < 0)
                constantTolerance = tolerance;
            const size_t count = source.KeyCount();
            if (!count)
                return CompactTrack(source);
            T constant = source.KeyValue(0);
            if constexpr (!std::is_same_v<T, glm::quat>)
            {
                T low = constant, high = constant;
                for (size_t i = 1; i < count; ++i)
                {
                    if constexpr (std::is_same_v<T, float>)
                    {
                        low  = std::min(low, source.KeyValue(i));
                        high = std::max(high, source.KeyValue(i));
                    }
                    else
                    {
                        low  = glm::min(low, source.KeyValue(i));
                        high = glm::max(high, source.KeyValue(i));
                    }
                }
                constant = low + (high - low) * .5f;
            }
            std::vector<AnimationKey<T>> samples;
            samples.reserve(count * 2 - 1);
            for (size_t i = 0; i < count; ++i)
            {
                const auto a = source.KeyAt(i);
                const auto b = source.KeyAt(std::min(i + 1, count - 1));
                samples.push_back({a.Time,
                                   source.Interpolation == TrackInterpolation::Step || i == 0 || i + 1 == count ?
                                       a.Value :
                                       InterpolateAnimationValues(a.Value, b.Value, 0.f, source.Interpolation)});
                if (i + 1 < count)
                {
                    const double time = a.Time + (b.Time - a.Time) * .5;
                    samples.push_back(
                        {time,
                         source.Interpolation == TrackInterpolation::Step ?
                             a.Value :
                             InterpolateAnimationValues(
                                 a.Value, b.Value, float((time - a.Time) / (b.Time - a.Time)), source.Interpolation)});
                }
            }
            bool isConstant = true;
            for (const auto& sample : samples)
                if (TrackErrorSquared(sample.Value, constant) > constantTolerance * constantTolerance)
                {
                    isConstant = false;
                    break;
                }
            if (isConstant)
            {
                AnimationTrack<T> result;
                result.Interpolation = source.Interpolation;
                result.RootEndpoints = source.RootEndpoints;
                if constexpr (!std::is_same_v<T, float>)
                    if (!result.RootEndpoints &&
                        (!SameBits(constant, source.KeyValue(0)) || !SameBits(constant, source.KeyValue(count - 1))))
                        result.RootEndpoints = std::array<T, 2> {source.KeyValue(0), source.KeyValue(count - 1)};
                result.Keys.push_back({source.KeyTime(0), constant});
                return CompactTrack(result);
            }
            if (count < 3 || source.Interpolation == TrackInterpolation::Step)
                return CompactTrack(source);
            std::vector<uint8_t> keep(count);
            keep.front() = keep.back() = 1;
            std::vector<std::pair<size_t, size_t>> pending {{0, count - 1}};
            while (!pending.empty())
            {
                const auto [first, last] = pending.back();
                pending.pop_back();
                if (last == first + 1)
                    continue;
                const auto a = source.KeyAt(first), b = source.KeyAt(last);
                auto       fitMode      = source.Interpolation;
                float      fitTolerance = tolerance;
                if constexpr (std::is_same_v<T, glm::quat>)
                {
                    const auto  delta  = a.Value - (glm::dot(a.Value, b.Value) < 0 ? -b.Value : b.Value);
                    const float chord2 = glm::dot(delta, delta);
                    const float normError =
                        std::abs(glm::dot(a.Value, a.Value) - 1) + std::abs(glm::dot(b.Value, b.Value) - 1);
                    const float allowance = 4 * (chord2 * std::sqrt(chord2) + normError);
                    // Close quaternions permit cheaper normalized-linear fitting. Reserve
                    // a cubic chord/norm allowance; certification still samples the actual
                    // spherical curve, so this heuristic cannot bypass the error contract.
                    if (fitMode == TrackInterpolation::Spherical && tolerance > 1e-6f && chord2 < .01f &&
                        allowance < tolerance * .25f)
                    {
                        fitMode = TrackInterpolation::Linear;
                        fitTolerance -= allowance;
                    }
                }
                float  worst = fitTolerance * fitTolerance;
                size_t split = 0;
                for (size_t i = first * 2 + 1; i < last * 2; ++i)
                {
                    const auto fitted = InterpolateAnimationValues(
                        a.Value, b.Value, float((samples[i].Time - a.Time) / (b.Time - a.Time)), fitMode);
                    const float error = TrackErrorSquared(samples[i].Value, fitted);
                    if (error > worst)
                    {
                        worst = error;
                        split = std::clamp((i + 1) / 2, first + 1, last - 1);
                    }
                }
                if (split)
                {
                    // Alternating high-frequency data puts the largest residual next to
                    // an endpoint. Balance those splits to avoid quadratic RDP work.
                    if (last - first > 16 && std::min(split - first, last - split) < (last - first) / 8)
                        split = first + (last - first) / 2;
                    keep[split] = 1;
                    pending.push_back({split, last});
                    pending.push_back({first, split});
                }
            }
            AnimationTrack<T> result;
            result.Interpolation = source.Interpolation;
            result.RootEndpoints = source.RootEndpoints;
            for (size_t i = 0; i < count; ++i)
                if (keep[i])
                    result.Keys.push_back(source.KeyAt(i));
            auto reduced = CompactTrack(result);
            if (result.Keys.size() < count)
            {
                auto         full = CompactTrack(source);
                const size_t indices =
                    reduced.TimeEncoding || reduced.KeyCount() == 1 ? 0 : reduced.KeyCount() * (count <= 65536 ? 2 : 4);
                // Nearly dense subsets can cost more than the full table reference
                // (or exact implicit cadence). Choose by bytes after actual fitting.
                if (reduced.Values.size() * sizeof(T) + reduced.RunEnds.size() * 4 + indices >=
                    full.Values.size() * sizeof(T) + full.RunEnds.size() * 4)
                    return full;
            }
            return reduced;
        }
        template<class F>
        void EachTrack(AnimationClipData& clip, F&& f)
        {
            for (auto& track : clip.Tracks)
            {
                f(track.Translation);
                f(track.Rotation);
                f(track.Scale);
            }
            for (auto& track : clip.MorphTracks)
                f(track.Weight);
        }
        float CompressionYaw(glm::quat rotation)
        {
            const auto forward = rotation * glm::vec3(0, 0, 1);
            if (forward.x * forward.x + forward.z * forward.z > 1e-12f)
                return std::atan2(forward.x, forward.z);
            const auto right = rotation * glm::vec3(1, 0, 0);
            return std::atan2(-right.z, right.x);
        }
        struct RootErrorBound
        {
            float Translation = 0, Rotation = 0, Radius = 0;
            bool  IncludePlanar(const JointTransform& a, const JointTransform& b, float shell, float budget)
            {
                const auto delta = a.Translation - b.Translation;
                Translation      = std::max(Translation, glm::length(glm::vec2(delta.x, delta.z)));
                const float yaw =
                    std::remainder(CompressionYaw(a.Rotation) - CompressionYaw(b.Rotation), glm::two_pi<float>());
                // Frobenius yaw error <= sqrt(2)*|deltaYaw|; 1.5 reserves numerical headroom.
                Rotation = std::max(Rotation, 1.5f * std::abs(yaw));
                Radius   = std::max(Radius, glm::length(glm::vec2(a.Translation.x, a.Translation.z)));
                return std::isfinite(Translation) && std::isfinite(Rotation) &&
                       4 * (Translation + Rotation * (Radius + shell)) <= budget;
            }
            bool Include(JointTransform a, JointTransform b, float shell, float budget)
            {
                Translation      = std::max(Translation, glm::length(a.Translation - b.Translation));
                const auto delta = glm::mat3_cast(a.Rotation) - glm::mat3_cast(b.Rotation);
                Rotation         = std::max(Rotation,
                                            std::sqrt(glm::dot(delta[0], delta[0]) + glm::dot(delta[1], delta[1]) +
                                                      glm::dot(delta[2], delta[2])));
                Radius           = std::max(Radius, glm::length(a.Translation));
                // For rigid root deltas B*inverse(A), ||delta R|| <= 2*max ||delta R(t)||
                // and ||delta t|| <= 2*max ||delta t(t)|| + ||delta R||*max ||t(t)||.
                // Frobenius bounds the operator norm. A factor of 4 reserves numerical
                // headroom and also bounds relative rest/first-frame references. Endpoints
                // are retained, so looping cannot accumulate a changed cycle transform.
                return std::isfinite(Translation) && std::isfinite(Rotation) &&
                       4 * (Translation + Rotation * (Radius + shell)) <= budget;
            }
        };
        struct ClipErrorMeasurement
        {
            const SkeletonData&                        Skeleton;
            const AnimationClipData&                   Source;
            BoundAnimationClip                         Binding;
            std::vector<double>                        Times;
            std::vector<glm::mat4>                     Original, Candidate, RestModel;
            std::vector<std::array<RootErrorBound, 2>> RootBounds;
            std::vector<float>                         RootRadius;
            std::vector<uint8_t>                       FailingJoints, FailingMorphs, EqualModel;
            std::vector<float>                         JointErrors, MorphErrors;
            std::vector<double>                        JointErrorTimes;
            ClipErrorMeasurement(const SkeletonData& skeleton, const AnimationClipData& clip) :
                Skeleton(skeleton), Source(clip), Binding(BindAnimationClip(skeleton, clip)),
                Original(skeleton.Joints.size()), Candidate(skeleton.Joints.size()), RestModel(skeleton.Joints.size()),
                RootBounds(skeleton.Joints.size()), RootRadius(skeleton.Joints.size()),
                FailingJoints(skeleton.Joints.size()), FailingMorphs(clip.MorphTracks.size()),
                EqualModel(skeleton.Joints.size()), JointErrors(skeleton.Joints.size()),
                MorphErrors(clip.MorphTracks.size()), JointErrorTimes(skeleton.Joints.size())
            {
                for (size_t j = 0; j < skeleton.Joints.size(); ++j)
                {
                    const auto& joint = skeleton.Joints[j];
                    RestModel[j]      = joint.Parent < 0 ? joint.RestLocal.Matrix() :
                                                           RestModel[joint.Parent] * joint.RestLocal.Matrix();
                }
                for (size_t j = 0; j < skeleton.Joints.size(); ++j)
                {
                    const auto& joint = skeleton.Joints[j];
                    RootRadius[j]     = glm::length(glm::vec3(RestModel[j][3]));
                    const auto track  = Binding.JointTracks[j];
                    if (track < 0)
                        continue;
                    const auto& t = clip.Tracks[track];
                    for (size_t k = 0; k < t.Translation.KeyCount(); ++k)
                    {
                        const auto local = t.Translation.KeyValue(k);
                        const auto model =
                            joint.Parent < 0 ? local : glm::vec3(RestModel[joint.Parent] * glm::vec4(local, 1));
                        RootRadius[j] = std::max(RootRadius[j], glm::length(model));
                    }
                }
                std::vector<double> previousTimes;
                auto                add = [&](const auto& track) {
                    bool same = previousTimes.size() == track.KeyCount();
                    for (size_t i = 0; same && i < track.KeyCount(); ++i)
                        same = SameBits(previousTimes[i], track.KeyTime(i));
                    if (same)
                        return;
                    previousTimes.clear();
                    for (size_t i = 0; i < track.KeyCount(); ++i)
                    {
                        const auto time = track.KeyTime(i);
                        Times.push_back(time);
                        previousTimes.push_back(time);
                    }
                };
                for (const auto& t : clip.Tracks)
                {
                    add(t.Translation);
                    add(t.Rotation);
                    add(t.Scale);
                }
                for (const auto& t : clip.MorphTracks)
                    add(t.Weight);
                Times.push_back(0);
                Times.push_back(clip.Duration);
                std::sort(Times.begin(), Times.end());
                Times.erase(std::unique(Times.begin(), Times.end()), Times.end());
                const size_t count  = Times.size();
                const double frames = clip.Duration * clip.FrameRate;
                Require(std::isfinite(frames) && frames <= 1000000);
                bool bakedFrames = count >= size_t(std::llround(frames)) + 1;
                for (size_t i = 1; bakedFrames && i < count; ++i)
                    bakedFrames = Times[i] - Times[i - 1] <= 1.01 / clip.FrameRate;
                Times.reserve(count * 2 - 1);
                for (size_t i = 1; i < count; ++i)
                    Times.push_back(Times[i - 1] + (Times[i] - Times[i - 1]) * .5);
                // Fully baked sources define actual frame timestamps (including FBX tick
                // conversion). Only infer nominal frame times for sparse source curves.
                double previous = 0;
                for (uint32_t frame = 1; !bakedFrames && frame <= uint32_t(frames); ++frame)
                {
                    const double at = double(frame) / clip.FrameRate;
                    Times.push_back(at);
                    Times.push_back(previous + (at - previous) * .5);
                    previous = at;
                }
                if (!bakedFrames && previous < clip.Duration)
                    Times.push_back(previous + (clip.Duration - previous) * .5);
                std::sort(Times.begin(), Times.end());
                Times.erase(std::unique(Times.begin(), Times.end()), Times.end());
            }
            bool Measure(const AnimationClipData&            clip,
                         const AnimationCompressionSettings& settings,
                         AnimationCompressionStatistics&     stats,
                         const std::vector<uint8_t>*         changedTracks = nullptr,
                         const std::vector<uint8_t>*         changedMorphs = nullptr)
            {
                std::vector<uint8_t> active(Skeleton.Joints.size()), needed(active.size()), local(active.size());
                for (size_t j = 0; j < active.size(); ++j)
                {
                    const auto track  = Binding.JointTracks[j];
                    local[j]          = !changedTracks || (track >= 0 && (*changedTracks)[track]);
                    const auto parent = Skeleton.Joints[j].Parent;
                    needed[j] = active[j] = local[j] || (parent >= 0 && active[parent]);
                    if (active[j])
                    {
                        FailingJoints[j]   = 0;
                        JointErrors[j]     = 0;
                        JointErrorTimes[j] = 0;
                    }
                }
                for (size_t j = needed.size(); j-- > 0;)
                    if (needed[j] && Skeleton.Joints[j].Parent >= 0)
                        needed[Skeleton.Joints[j].Parent] = 1;
                for (size_t k = 0; k < MorphErrors.size(); ++k)
                    if (!changedMorphs || (*changedMorphs)[k])
                    {
                        FailingMorphs[k] = 0;
                        MorphErrors[k]   = 0;
                    }
                auto failJoint = [&](size_t j) { FailingJoints[j] = 1; };
                auto failMorph = [&](size_t k) { FailingMorphs[k] = 1; };
                for (size_t j = 0; j < RootBounds.size(); ++j)
                {
                    if (local[j])
                    {
                        RootBounds[j] = {};
                        for (auto& bound : RootBounds[j])
                            bound.Radius = RootRadius[j];
                    }
                }
                stats.MaxPositionError = stats.MaxMorphError = 0;
                stats.MaxPositionTime                        = 0;
                stats.Joint                                  = Skeleton.Joints.empty() ? "" : Skeleton.Joints[0].Name;
                stats.ShapeKey = Source.MorphTracks.empty() ? "" : Source.MorphTracks[0].ShapeKey;
                for (double time : Times)
                {
                    for (size_t j = 0; j < Skeleton.Joints.size(); ++j)
                    {
                        if (!needed[j])
                            continue;
                        const auto& joint = Skeleton.Joints[j];
                        auto        a = joint.RestLocal, b = a;
                        const auto  track = Binding.JointTracks[j];
                        if (track >= 0)
                        {
                            a = SampleAnimationJoint(a, Source.Tracks[track], time);
                            b = SampleAnimationJoint(b, clip.Tracks[track], time);
                        }
                        const bool equal = SameBits(a.Translation, b.Translation) && SameBits(a.Rotation, b.Rotation) &&
                                           SameBits(a.Scale, b.Scale);
                        const auto localA = a.Matrix();
                        const auto localB = equal ? localA : b.Matrix();
                        if (local[j] && track >= 0 && !equal)
                        {
                            // Root extraction evaluates against the rest parent rather than
                            // animated ancestors. Bound every selectable root in both modes.
                            const auto parent = joint.Parent < 0 ? glm::mat4(1) : RestModel[joint.Parent];
                            try
                            {
                                const auto endpoint = time <= 0               ? AnimationSampleEndpoint::First :
                                                      time >= Source.Duration ? AnimationSampleEndpoint::Last :
                                                                                AnimationSampleEndpoint::Interpolated;
                                const auto rootA =
                                    endpoint == AnimationSampleEndpoint::Interpolated ?
                                        localA :
                                        SampleAnimationJoint(joint.RestLocal, Source.Tracks[track], time, endpoint)
                                            .Matrix();
                                const auto rootB =
                                    endpoint == AnimationSampleEndpoint::Interpolated ?
                                        localB :
                                        SampleAnimationJoint(joint.RestLocal, clip.Tracks[track], time, endpoint)
                                            .Matrix();
                                const auto ra = JointTransform::FromMatrix(parent * rootA);
                                const auto rb = JointTransform::FromMatrix(parent * rootB);
                                if (!RootBounds[j][0].Include(ra, rb, settings.Shell, settings.Position) ||
                                    !RootBounds[j][1].IncludePlanar(ra, rb, settings.Shell, settings.Position))
                                    failJoint(j);
                            }
                            catch (const std::invalid_argument&)
                            {
                                failJoint(j); // Refine this path when root extraction becomes singular.
                            }
                        }
                        EqualModel[j] = equal && (joint.Parent < 0 || EqualModel[joint.Parent]);
                        Original[j]   = joint.Parent < 0 ? localA : Original[joint.Parent] * localA;
                        Candidate[j]  = EqualModel[j]    ? Original[j] :
                                        joint.Parent < 0 ? localB :
                                                           Candidate[joint.Parent] * localB;
                        if (!active[j] || EqualModel[j])
                            continue;
                        const auto originA = glm::vec3(Original[j][3]), originB = glm::vec3(Candidate[j][3]);
                        float      error = glm::length(originA - originB);
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            // An affine shell point has only one nonzero local axis.
                            const auto offsetA = glm::vec3(Original[j][axis]) * settings.Shell;
                            const auto offsetB = glm::vec3(Candidate[j][axis]) * settings.Shell;
                            error = std::max(error, glm::length((originA + offsetA) - (originB + offsetB)));
                            error = std::max(error, glm::length((originA - offsetA) - (originB - offsetB)));
                        }
                        if (!std::isfinite(error))
                            failJoint(j);
                        if (error > JointErrors[j])
                        {
                            JointErrors[j]     = error;
                            JointErrorTimes[j] = time;
                        }
                        if (error > settings.Position)
                            failJoint(j);
                    }
                    // Include unbound morph channels too; their authored values must survive rebinding.
                    for (size_t k = 0; k < Source.MorphTracks.size(); ++k)
                    {
                        if (changedMorphs && !(*changedMorphs)[k])
                            continue;
                        const auto& track = Source.MorphTracks[k];
                        const float error = std::abs(SampleAnimationTrack(track.Weight, time, 0.f) -
                                                     SampleAnimationTrack(clip.MorphTracks[k].Weight, time, 0.f));
                        if (!std::isfinite(error))
                            failMorph(k);
                        MorphErrors[k] = std::max(MorphErrors[k], error);
                        if (error > settings.Morph)
                            failMorph(k);
                    }
                }
                for (size_t j = 0; j < JointErrors.size(); ++j)
                    if (JointErrors[j] > stats.MaxPositionError)
                    {
                        stats.MaxPositionError = JointErrors[j];
                        stats.MaxPositionTime  = JointErrorTimes[j];
                        stats.Joint            = Skeleton.Joints[j].Name;
                    }
                for (size_t k = 0; k < MorphErrors.size(); ++k)
                    if (MorphErrors[k] > stats.MaxMorphError)
                    {
                        stats.MaxMorphError = MorphErrors[k];
                        stats.ShapeKey      = Source.MorphTracks[k].ShapeKey;
                    }
                return std::none_of(FailingJoints.begin(), FailingJoints.end(), [](auto v) { return v != 0; }) &&
                       std::none_of(FailingMorphs.begin(), FailingMorphs.end(), [](auto v) { return v != 0; });
            }
        };
        struct PerTrackReduction
        {
            const ClipErrorMeasurement&         Metric;
            const AnimationCompressionSettings& Settings;
            std::vector<std::array<float, 3>>   Tolerances, ConstantTolerances;
            std::vector<float>                  Factors, MorphFactors;
            std::vector<uint8_t>                ChangedTracks, ChangedMorphs;
            PerTrackReduction(const ClipErrorMeasurement& metric, const AnimationCompressionSettings& settings) :
                Metric(metric), Settings(settings), Tolerances(metric.Source.Tracks.size()),
                ConstantTolerances(metric.Source.Tracks.size()), Factors(metric.Source.Tracks.size(), 1),
                MorphFactors(metric.Source.MorphTracks.size(), 1)
            {
                const auto&        joints = metric.Skeleton.Joints;
                std::vector<float> translation(joints.size()), scale(joints.size()), gain(joints.size()),
                    reach(joints.size(), settings.Shell);
                std::vector<uint32_t> depth(joints.size());
                auto                  maxScale = [](glm::vec3 value) {
                    return std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
                };
                for (size_t j = 0; j < joints.size(); ++j)
                {
                    const auto t   = metric.Binding.JointTracks[j];
                    translation[j] = glm::length(joints[j].RestLocal.Translation);
                    scale[j]       = maxScale(joints[j].RestLocal.Scale);
                    bool dynamic   = false;
                    if (t >= 0)
                    {
                        const auto& track = metric.Source.Tracks[t];
                        if (track.Translation.KeyCount())
                            translation[j] = 0;
                        if (track.Scale.KeyCount())
                            scale[j] = 0;
                        for (size_t k = 0; k < track.Translation.KeyCount(); ++k)
                            translation[j] = std::max(translation[j], glm::length(track.Translation.KeyValue(k)));
                        for (size_t k = 0; k < track.Scale.KeyCount(); ++k)
                            scale[j] = std::max(scale[j], maxScale(track.Scale.KeyValue(k)));
                        dynamic =
                            TrackVaries(track.Translation) || TrackVaries(track.Rotation) || TrackVaries(track.Scale);
                    }
                    const auto parent = joints[j].Parent;
                    gain[j]           = parent < 0 ? 1 : gain[parent] * scale[parent];
                    depth[j]          = (parent < 0 ? 0 : depth[parent]) + uint32_t(dynamic);
                }
                for (size_t j = joints.size(); j-- > 0;)
                    if (joints[j].Parent >= 0)
                    {
                        reach[joints[j].Parent] =
                            std::max(reach[joints[j].Parent], translation[j] + scale[j] * reach[j]);
                        depth[joints[j].Parent] = std::max(depth[joints[j].Parent], depth[j]);
                    }
                for (size_t j = 0; j < joints.size(); ++j)
                {
                    const auto t = metric.Binding.JointTracks[j];
                    if (t < 0)
                        continue;
                    // Divide by animated path length, not depth squared. Ancestor scale
                    // gain and descendant lever arms account for actual error propagation.
                    // Three channels reserve 3/4 of a path budget before certification.
                    const float d      = float(std::max(1u, depth[j]));
                    const float budget = settings.Position / (4 * d);
                    auto        divide = [](float numerator, float denominator) {
                        return numerator / std::max(denominator, 1e-20f);
                    };
                    const float rootRadius = metric.RootRadius[j] + settings.Shell;
                    Tolerances[t]          = {divide(std::min(budget, settings.Position / 16), gain[j]),
                                              std::min(divide(budget, gain[j] * scale[j] * reach[j]),
                                                       divide(settings.Position, 32 * gain[j] * rootRadius)),
                                              divide(budget, gain[j] * reach[j])};
                    // Try nearly constant channels against their individual propagated
                    // contribution first. The full hierarchy certificate accepts harmless
                    // deep-chain noise instead of letting a conservative path share block it.
                    // Failed contributions are tightened together with the ordinary fit.
                    const float constantBudget = settings.Position / 8;
                    ConstantTolerances[t]      = {
                        std::max(Tolerances[t][0], divide(std::min(constantBudget, settings.Position / 16), gain[j])),
                        std::max(Tolerances[t][1],
                                 std::min(divide(constantBudget, gain[j] * scale[j] * reach[j]),
                                          divide(settings.Position, 32 * gain[j] * rootRadius))),
                        std::max(Tolerances[t][2], divide(constantBudget, gain[j] * reach[j]))};
                }
            }
            void FitJoint(size_t t, AnimationClipData& result) const
            {
                const auto& track     = Metric.Source.Tracks[t];
                const auto& tolerance = Tolerances[t];
                result.Tracks[t]      = {
                    track.Joint,
                    ReduceTrack(track.Translation, tolerance[0] * Factors[t], ConstantTolerances[t][0] * Factors[t]),
                    ReduceTrack(track.Scale, tolerance[2] * Factors[t], ConstantTolerances[t][2] * Factors[t]),
                    ReduceTrack(track.Rotation, tolerance[1] * Factors[t], ConstantTolerances[t][1] * Factors[t])};
            }
            void FitMorph(size_t t, AnimationClipData& result) const
            {
                const auto& track     = Metric.Source.MorphTracks[t];
                result.MorphTracks[t] = {track.ShapeKey,
                                         ReduceTrack(track.Weight, Settings.Morph * .9f * MorphFactors[t])};
            }
            AnimationClipData Fit() const
            {
                AnimationClipData result;
                const auto&       source = Metric.Source;
                result.Name              = source.Name;
                result.SourceName        = source.SourceName;
                result.Duration          = source.Duration;
                result.FrameRate         = source.FrameRate;
                result.Events            = source.Events;
                result.Tracks.resize(source.Tracks.size());
                result.MorphTracks.resize(source.MorphTracks.size());
                for (size_t t = 0; t < source.Tracks.size(); ++t)
                    FitJoint(t, result);
                for (size_t t = 0; t < source.MorphTracks.size(); ++t)
                    FitMorph(t, result);
                return result;
            }
            void Tighten(AnimationClipData& result)
            {
                ChangedTracks.assign(Factors.size(), 0);
                ChangedMorphs.assign(MorphFactors.size(), 0);
                auto& dirty = ChangedTracks;
                for (size_t j = 0; j < Metric.FailingJoints.size(); ++j)
                    if (Metric.FailingJoints[j])
                        for (int32_t ancestor = int32_t(j); ancestor >= 0;
                             ancestor         = Metric.Skeleton.Joints[ancestor].Parent)
                        {
                            const auto t = Metric.Binding.JointTracks[ancestor];
                            if (t >= 0)
                                dirty[t] = 1;
                        }
                for (size_t t = 0; t < dirty.size(); ++t)
                    if (dirty[t])
                    {
                        Factors[t] *= .25f;
                        FitJoint(t, result);
                    }
                for (size_t t = 0; t < MorphFactors.size(); ++t)
                    if (Metric.FailingMorphs[t])
                    {
                        ChangedMorphs[t] = 1;
                        MorphFactors[t] *= .25f;
                        FitMorph(t, result);
                    }
            }
        };
    } // namespace

    std::variant<AnimationCompressionSettings, AssetError>
    ParseAnimationCompressionSettings(const std::map<std::string, std::string>& values,
                                      const std::string&                        clipObjectKey)
    {
        AnimationCompressionSettings result;
        const std::string            bases[] = {
            "animation.compression", "animation.error.position", "animation.error.shell", "animation.error.morph"};
        // Validate even overrides for other clips; a typo must never silently turn compression off.
        for (const auto& [key, value] : values)
            for (size_t field = 0; field < 4; ++field)
                if (key == bases[field] || key.compare(0, bases[field].size() + 1, bases[field] + ".") == 0)
                {
                    if (key != bases[field] && key.size() == bases[field].size() + 1)
                        return AssetError {AssetErrorCode::InvalidArgument, "Empty clip object key: " + key};
                    if (key != bases[field] && value == "inherit")
                        continue;
                    if (field == 0)
                    {
                        if (value != "off" && value != "lossless" && value != "optimal" && value != "custom")
                            return AssetError {AssetErrorCode::InvalidArgument,
                                               "Invalid animation compression: " + value};
                    }
                    else
                    {
                        char*      end    = nullptr;
                        const auto number = std::strtof(value.c_str(), &end);
                        if (value.empty() || end != value.c_str() + value.size() || !std::isfinite(number) ||
                            number < 0)
                            return AssetError {AssetErrorCode::InvalidArgument,
                                               "Invalid animation error tolerance: " + key};
                    }
                }
        auto get = [&](size_t field) -> const std::string* {
            auto found = clipObjectKey.empty() ? values.end() : values.find(bases[field] + "." + clipObjectKey);
            if (found == values.end() || found->second == "inherit")
                found = values.find(bases[field]);
            return found == values.end() ? nullptr : &found->second;
        };
        if (auto value = get(0))
            result.Mode = *value == "off"      ? AnimationCompressionMode::Off :
                          *value == "lossless" ? AnimationCompressionMode::Lossless :
                          *value == "optimal"  ? AnimationCompressionMode::Optimal :
                                                 AnimationCompressionMode::Custom;
        if (result.Mode == AnimationCompressionMode::Custom)
        {
            if (auto value = get(1))
                result.Position = std::strtof(value->c_str(), nullptr);
            if (auto value = get(2))
                result.Shell = std::strtof(value->c_str(), nullptr);
            if (auto value = get(3))
                result.Morph = std::strtof(value->c_str(), nullptr);
        }
        return result;
    }

    AnimationProductBytes EncodeAnimationClipProduct(const AnimationClipData&            source,
                                                     const SkeletonData&                 skeleton,
                                                     const AnimationCompressionSettings& requested)
    {
        if (requested.Mode > AnimationCompressionMode::Custom || !std::isfinite(requested.Position) ||
            requested.Position < 0 || !std::isfinite(requested.Shell) || requested.Shell < 0 ||
            !std::isfinite(requested.Morph) || requested.Morph < 0)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid animation compression settings"};
        try
        {
            Valid(ValidateAnimationClip(source));
            if (requested.Mode == AnimationCompressionMode::Off)
                return OffClip(source);
            Valid(ValidateSkeleton(skeleton));
            // The import binding is part of the error contract: rejecting ambiguous/unresolved
            // joint tracks is safer than certifying an error for a track that was never evaluated.
            const auto binding = BindAnimationClip(skeleton, source);
            for (size_t t = 0; requested.Mode != AnimationCompressionMode::Lossless && t < source.Tracks.size(); ++t)
                Require(std::find(binding.JointTracks.begin(), binding.JointTracks.end(), int32_t(t)) !=
                        binding.JointTracks.end());
            const size_t                 offBytes = OffClipSize(source);
            AnimationCompressionSettings settings = requested;
            if (settings.Mode == AnimationCompressionMode::Optimal)
            {
                const AnimationCompressionSettings defaults;
                settings.Position = defaults.Position;
                settings.Shell    = defaults.Shell;
                settings.Morph    = defaults.Morph;
            }
            AnimationClipData              result;
            AnimationCompressionStatistics stats;
            stats.Mode              = settings.Mode;
            stats.UncompressedBytes = offBytes;
            stats.Joint             = skeleton.Joints.empty() ? "" : skeleton.Joints[0].Name;
            stats.ShapeKey          = source.MorphTracks.empty() ? "" : source.MorphTracks[0].ShapeKey;
            if (settings.Mode == AnimationCompressionMode::Optimal || settings.Mode == AnimationCompressionMode::Custom)
            {
                ClipErrorMeasurement metric(skeleton, source);
                PerTrackReduction    reduction(metric, settings);
                result         = reduction.Fit();
                bool certified = metric.Measure(result, settings, stats);
                for (unsigned pass = 0; !certified && pass < 24; ++pass)
                {
                    reduction.Tighten(result);
                    certified =
                        metric.Measure(result, settings, stats, &reduction.ChangedTracks, &reduction.ChangedMorphs);
                }
                Require(certified);
            }
            else
            {
                result = source;
                EachTrack(result, [](auto& track) { track = CompactTrack(track); });
            }
            ShareClipTimes(result, source);
            result.Compression                 = stats;
            auto bytes                         = PackedClip(result);
            result.Compression.CompressedBytes = bytes.size();
            return PackedClip(result);
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    AnimationProductBytes EncodeAnimationClipProduct(const AnimationClipData& c)
    {
        try
        {
            Valid(ValidateAnimationClip(c));
            if (c.Compression.Mode != AnimationCompressionMode::Off)
            {
                auto shared = c;
                ShareClipTimes(shared, c);
                auto bytes                         = PackedClip(shared);
                shared.Compression.CompressedBytes = bytes.size();
                return PackedClip(shared);
            }
            return OffClip(c);
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    AnimationProductBytes EncodeAnimationControllerProduct(const AnimationControllerData& c)
    {
        try
        {
            return Bytes("KACT", EncodeAnimationControllerNode(c));
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    std::variant<SkeletonData, AssetError> DecodeSkeletonProduct(const std::byte* bytes, size_t size)
    {
        try
        {
            auto         n = Root(bytes, size, "KSKL");
            SkeletonData s;
            if (n.IsMap())
            {
                s.ShapeKeys = ReadShapeKeys(n["shape_keys"]);
                n           = n["joints"];
            }
            Sequence(n, 65536);
            for (const auto& v : n)
            {
                SkeletonJoint j;
                j.Name      = v["name"].as<std::string>();
                j.Path      = v["path"].as<std::string>();
                j.NodeKey   = v["node"].as<std::string>();
                j.Parent    = v["parent"].as<int32_t>();
                j.RestLocal = ReadTransform(v["rest"]);
                j.BindLocal = ReadTransform(v["bind"]);
                Sequence(v["inverse_bind"], 16);
                Require(v["inverse_bind"].size() == 16);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        j.InverseBind[c][r] = v["inverse_bind"][c * 4 + r].as<float>();
                s.Joints.push_back(std::move(j));
            }
            Valid(ValidateSkeleton(s));
            return s;
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    std::variant<AnimationClipData, AssetError> DecodeAnimationClipProduct(const std::byte* bytes, size_t size)
    {
        try
        {
            if (bytes && size >= 4 && std::memcmp(bytes, "KCLP", 4) == 0)
                return ReadPackedClip(bytes, size);
            auto clip = ReadClip(Root(bytes, size, "KCLP"));
            SetOffStatistics(clip, size);
            return clip;
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    std::variant<AnimationControllerData, AssetError> DecodeAnimationControllerProduct(const std::byte* bytes,
                                                                                       size_t           size)
    {
        try
        {
            return DecodeAnimationControllerNode(Root(bytes, size, "KACT"));
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    YAML::Node EncodeExpressionNode(const ExpressionData& data)
    {
        Require(data.Presets.size() <= 4096);
        for (const auto& preset : data.Presets)
            Require(preset.Weights.size() <= 65536);
        AnimationControllerData c;
        c.Expressions = data.Presets;
        c.Motions.emplace_back().InlineClip.emplace();
        c.Layers.emplace_back().States.push_back({"Rest", 0});
        Valid(ValidateAnimationController(c));
        YAML::Node n;
        n["presets"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& preset : data.Presets)
        {
            YAML::Node p;
            p["name"]    = preset.Name;
            p["weights"] = ShapeKeys(preset.Weights);
            n["presets"].push_back(p);
        }
        return n;
    }
    ExpressionData DecodeExpressionNode(const YAML::Node& n)
    {
        Require(n.IsMap() && n.size() == 1 && n["presets"].IsSequence() && n["presets"].size() <= 4096);
        ExpressionData data;
        for (const auto& p : n["presets"])
        {
            Require(p.IsMap() && p.size() == 2);
            data.Presets.push_back({p["name"].as<std::string>(), ReadShapeKeyWeights(p["weights"])});
        }
        EncodeExpressionNode(data); // Shared validation, including duplicate names/non-finite weights.
        return data;
    }
    AnimationProductBytes EncodeExpressionProduct(const ExpressionData& data)
    {
        try
        {
            return Bytes("KEXP", EncodeExpressionNode(data));
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
    std::variant<ExpressionData, AssetError> DecodeExpressionProduct(const std::byte* bytes, size_t size)
    {
        try
        {
            return DecodeExpressionNode(Root(bytes, size, "KEXP"));
        }
        catch (const std::exception& e)
        {
            return Error(e);
        }
    }
} // namespace Kans
