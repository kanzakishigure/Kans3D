#include "AudioAttenuationEditing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>

namespace Kans::AudioAttenuationEditing
{
    namespace
    {
        using Curve = std::vector<AudioCurveKey>;

        constexpr float FarthestDistance = 1.0e6f; // A last key's upper limit.
        constexpr float SmallestWidth    = 0.01f;  // The curve view never shows less (metres).

        bool SameKeys(const Curve& a, const Curve& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].Distance != b[i].Distance || a[i].Value != b[i].Value ||
                    a[i].Interpolation != b[i].Interpolation)
                    return false;
            return true;
        }

        float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }
    } // namespace

    // ------------------------------------------------------------------ curves

    const Curve* CurveOf(const AudioAttenuationData& data, CurveKind kind)
    {
        switch (kind)
        {
            case CurveKind::Volume:
                return std::get_if<Curve>(&data.Volume);
            case CurveKind::AirAbsorption:
                return &data.AirAbsorption;
            case CurveKind::Spread:
                return &data.Spread;
        }
        return nullptr;
    }

    Curve* CurveOf(AudioAttenuationData& data, CurveKind kind)
    {
        return const_cast<Curve*>(CurveOf(static_cast<const AudioAttenuationData&>(data), kind));
    }

    float LowestValue(CurveKind kind) { return kind == CurveKind::AirAbsorption ? 20.0f : 0.0f; }
    float HighestValue(CurveKind kind) { return kind == CurveKind::AirAbsorption ? 24000.0f : 1.0f; }

    glm::vec2 DistanceLimits(const Curve& keys, size_t index)
    {
        if (index >= keys.size())
            return {0.0f, 0.0f};
        const float lower = index > 0 ? keys[index - 1].Distance + KeyGap : 0.0f;
        const float upper = index + 1 < keys.size() ? keys[index + 1].Distance - KeyGap : FarthestDistance;
        return {lower, std::max(lower, upper)};
    }

    glm::vec2 ValueLimits(const Curve& keys, size_t index, CurveKind kind)
    {
        float lower = LowestValue(kind), upper = HighestValue(kind);
        // The cutoff may not rise with distance: a key stays between its neighbours' cutoffs.
        if (kind == CurveKind::AirAbsorption && index < keys.size())
        {
            if (index > 0)
                upper = std::min(upper, keys[index - 1].Value);
            if (index + 1 < keys.size())
                lower = std::max(lower, keys[index + 1].Value);
        }
        return {lower, std::max(lower, upper)};
    }

    std::optional<size_t> InsertKey(Curve& keys, AudioCurveKey key, CurveKind kind)
    {
        if (!std::isfinite(key.Distance) || key.Distance < 0.0f)
            return std::nullopt;
        const auto at = std::upper_bound(
            keys.begin(), keys.end(), key.Distance, [](float d, const AudioCurveKey& k) { return d < k.Distance; });
        const size_t index = size_t(at - keys.begin());
        if (index > 0 && key.Distance - keys[index - 1].Distance < KeyGap)
            return std::nullopt;
        if (index < keys.size() && keys[index].Distance - key.Distance < KeyGap)
            return std::nullopt;
        // A new key continues the segment it splits.
        key.Interpolation = index > 0           ? keys[index - 1].Interpolation :
                            index < keys.size() ? keys[index].Interpolation :
                                                  key.Interpolation;
        keys.insert(at, key);
        const auto limits = ValueLimits(keys, index, kind);
        keys[index].Value = std::clamp(Finite(key.Value, limits.y), limits.x, limits.y);
        return index;
    }

    bool RemoveKey(Curve& keys, size_t index)
    {
        if (index >= keys.size())
            return false;
        keys.erase(keys.begin() + std::ptrdiff_t(index));
        return true;
    }

    bool MoveKey(Curve& keys, size_t index, float distance, float value, CurveKind kind)
    {
        if (index >= keys.size())
            return false;
        const auto distances = DistanceLimits(keys, index);
        const auto values    = ValueLimits(keys, index, kind);
        auto&      key       = keys[index];
        key.Distance         = std::clamp(Finite(distance, key.Distance), distances.x, distances.y);
        key.Value            = std::clamp(Finite(value, key.Value), values.x, values.y);
        return true;
    }

    // ------------------------------------------------------------------ modes

    bool IsPreset(const AudioAttenuationData& data)
    {
        return std::holds_alternative<AudioAttenuationPreset>(data.Volume);
    }

    Curve KeysFromPreset(const AudioAttenuationPreset& preset)
    {
        const float start = std::max(preset.MinDistance, KeyGap);
        const float end   = std::max(preset.MaxDistance, start);
        Curve       keys;
        if (!(end - start >= KeyGap))
        {
            keys.push_back({start, std::clamp(EvaluateAttenuationPreset(preset, start), 0.0f, 1.0f)});
            return keys;
        }
        // Log-spaced between the model's ends: the models fall fastest near the minimum.
        constexpr int Steps = 10;
        for (int i = 0; i <= Steps; ++i)
        {
            const float t        = float(i) / float(Steps);
            float       distance = start * std::pow(end / start, t);
            if (i == Steps)
                distance = end;
            if (!keys.empty() && distance - keys.back().Distance < KeyGap)
                continue;
            keys.push_back({distance,
                            std::clamp(EvaluateAttenuationPreset(preset, distance), 0.0f, 1.0f),
                            AudioCurveInterpolation::Smooth});
        }
        return keys;
    }

    void SetPresetMode(AudioAttenuationData& data, bool preset)
    {
        if (preset == IsPreset(data))
            return;
        if (preset)
            data.Volume = AudioAttenuationPreset {};
        else
            data.Volume = KeysFromPreset(std::get<AudioAttenuationPreset>(data.Volume));
    }

    AirMode AirModeOf(const AudioAttenuationData& data)
    {
        const auto& keys = data.AirAbsorption;
        if (keys.empty())
            return AirMode::Off;
        if (keys.size() == 2 && keys[0].Interpolation == AudioCurveInterpolation::Linear &&
            keys[1].Interpolation == AudioCurveInterpolation::Linear)
            return AirMode::Range;
        return AirMode::Curve;
    }

    AirRange AirRangeOf(const AudioAttenuationData& data)
    {
        const auto& keys = data.AirAbsorption;
        if (keys.empty())
            return {};
        return {keys.front().Distance, keys.back().Distance, keys.front().Value, keys.back().Value};
    }

    void SetAirRange(AudioAttenuationData& data, const AirRange& range)
    {
        data.AirAbsorption = {{range.StartDistance, range.NearCutoffHz, AudioCurveInterpolation::Linear},
                              {range.EndDistance, range.FarCutoffHz, AudioCurveInterpolation::Linear}};
    }

    void SetAirMode(AudioAttenuationData& data, AirMode mode)
    {
        if (mode == AirModeOf(data))
            return;
        switch (mode)
        {
            case AirMode::Off:
                data.AirAbsorption.clear();
                return;
            case AirMode::Range:
                SetAirRange(data, data.AirAbsorption.size() >= 2 ? AirRangeOf(data) : AirRange {});
                return;
            case AirMode::Curve: {
                // The same line with a key in its middle, smooth (on a line a smooth curve stays straight).
                const AirRange range  = data.AirAbsorption.size() >= 2 ? AirRangeOf(data) : AirRange {};
                const float    middle = (range.StartDistance + range.EndDistance) * 0.5f;
                data.AirAbsorption    = {
                    {range.StartDistance, range.NearCutoffHz, AudioCurveInterpolation::Smooth},
                    {middle, (range.NearCutoffHz + range.FarCutoffHz) * 0.5f, AudioCurveInterpolation::Smooth},
                    {range.EndDistance, range.FarCutoffHz, AudioCurveInterpolation::Smooth}};
                if (!(range.EndDistance - range.StartDistance >= 2 * KeyGap))
                    data.AirAbsorption.erase(data.AirAbsorption.begin() + 1);
                return;
            }
        }
    }

    void SetSpread(AudioAttenuationData& data, bool on)
    {
        if (!on)
            data.Spread.clear();
        else if (data.Spread.empty())
            data.Spread = {{0.5f, 1.0f, AudioCurveInterpolation::Smooth},
                           {10.0f, 0.0f, AudioCurveInterpolation::Smooth}};
    }

    // ------------------------------------------------------------------ curve view

    float CurveExtent(const AudioAttenuationData& data, CurveKind kind)
    {
        float extent = 0.0f;
        if (const auto* keys = CurveOf(data, kind); keys && !keys->empty())
            extent = keys->back().Distance;
        else if (const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume))
            extent = preset->MaxDistance;
        else if (const auto* volume = std::get_if<Curve>(&data.Volume); volume && !volume->empty())
            extent = volume->back().Distance;
        return std::max(1.0f, Finite(extent, 1.0f));
    }

    CurveView AutoView(const AudioAttenuationData& data, CurveKind kind)
    {
        CurveView view;
        view.DistanceStart = 0.0f;
        view.DistanceEnd   = CurveExtent(data, kind) * 1.15f;
        view.LogValue      = kind == CurveKind::AirAbsorption;
        view.ValueLow      = view.LogValue ? std::log10(LowestValue(kind)) : LowestValue(kind);
        view.ValueHigh     = view.LogValue ? std::log10(HighestValue(kind)) : HighestValue(kind);
        return view;
    }

    glm::vec2 ToPlot(const CurveView& view, glm::vec2 origin, glm::vec2 size, float distance, float value)
    {
        const float width  = view.DistanceEnd - view.DistanceStart;
        const float height = view.ValueHigh - view.ValueLow;
        const float v      = view.LogValue ? std::log10(std::max(value, 1e-6f)) : value;
        const float x      = width > 0.0f ? (distance - view.DistanceStart) / width : 0.0f;
        const float y      = height > 0.0f ? (v - view.ValueLow) / height : 0.0f;
        return {origin.x + x * size.x, origin.y + (1.0f - y) * size.y};
    }

    glm::vec2 FromPlot(const CurveView& view, glm::vec2 origin, glm::vec2 size, glm::vec2 position)
    {
        const float x = size.x > 0.0f ? (position.x - origin.x) / size.x : 0.0f;
        const float y = size.y > 0.0f ? 1.0f - (position.y - origin.y) / size.y : 0.0f;
        const float v = view.ValueLow + y * (view.ValueHigh - view.ValueLow);
        return {view.DistanceStart + x * (view.DistanceEnd - view.DistanceStart),
                view.LogValue ? std::pow(10.0f, v) : v};
    }

    CurveView Zoom(const CurveView& view, float anchor, float factor)
    {
        if (!(factor > 0.0f) || !std::isfinite(factor))
            return view;
        CurveView   next   = view;
        const float width  = std::max(SmallestWidth, (view.DistanceEnd - view.DistanceStart) / factor);
        const float share  = view.DistanceEnd > view.DistanceStart ?
                                 (anchor - view.DistanceStart) / (view.DistanceEnd - view.DistanceStart) :
                                 0.0f;
        next.DistanceStart = std::max(0.0f, anchor - share * width);
        next.DistanceEnd   = next.DistanceStart + width;
        return next;
    }

    CurveView Pan(const CurveView& view, float delta)
    {
        CurveView   next   = view;
        const float width  = view.DistanceEnd - view.DistanceStart;
        next.DistanceStart = std::max(0.0f, view.DistanceStart + Finite(delta, 0.0f));
        next.DistanceEnd   = next.DistanceStart + width;
        return next;
    }

    float Sample(const AudioAttenuationData& data, CurveKind kind, float distance)
    {
        const auto sample = EvaluateAttenuation(data, distance);
        return kind == CurveKind::Volume        ? sample.Gain :
               kind == CurveKind::AirAbsorption ? sample.CutoffHz :
                                                  sample.Spread;
    }

    // ------------------------------------------------------------------ readouts

    std::string GainText(float gain)
    {
        if (!(gain > 0.0f))
            return "-inf dB";
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f dB", 20.0f * std::log10(gain));
        return text;
    }

    std::string CutoffText(float hertz)
    {
        if (!(hertz > 0.0f))
            return "bypass";
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f Hz", hertz);
        return text;
    }

    std::string ProbeText(const AudioAttenuationData& data, float distance)
    {
        const auto sample = EvaluateAttenuation(data, distance);
        char       text[128];
        std::snprintf(text,
                      sizeof(text),
                      "%.2f m: %s, %s, spread %.2f",
                      distance,
                      GainText(sample.Gain).c_str(),
                      CutoffText(sample.CutoffHz).c_str(),
                      sample.Spread);
        return text;
    }

    // ------------------------------------------------------------------ ranges

    std::optional<float> SilentDistance(const Curve& keys)
    {
        for (const auto& key : keys)
            if (key.Value <= 0.0f)
                return key.Distance;
        return std::nullopt;
    }

    Range RangeOf(const AudioAttenuationData& data)
    {
        if (const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume))
            return RangeOf(preset->Model, preset->MinDistance, preset->MaxDistance);
        const auto& keys = std::get<Curve>(data.Volume);
        Range       range;
        if (keys.empty())
            return range;
        range.Inner = keys.front().Distance;
        for (size_t i = 1; i < keys.size() && keys[i].Value == keys.front().Value; ++i)
            range.Inner = keys[i].Distance;
        range.Outer    = SilentDistance(keys).value_or(keys.back().Distance);
        range.HasOuter = range.Outer > range.Inner;
        return range;
    }

    Range RangeOf(const AudioAttenuationModel model, float minDistance, float maxDistance)
    {
        Range range;
        range.Inner    = minDistance;
        range.Outer    = maxDistance;
        range.HasOuter = model != AudioAttenuationModel::None && maxDistance > minDistance;
        return range;
    }

    // ------------------------------------------------------------------ the edited asset

    bool Equal(const AudioAttenuationData& a, const AudioAttenuationData& b)
    {
        if (a.Volume.index() != b.Volume.index())
            return false;
        if (const auto* pa = std::get_if<AudioAttenuationPreset>(&a.Volume))
        {
            const auto& pb = std::get<AudioAttenuationPreset>(b.Volume);
            if (pa->Model != pb.Model || pa->MinDistance != pb.MinDistance || pa->MaxDistance != pb.MaxDistance ||
                pa->Rolloff != pb.Rolloff)
                return false;
        }
        else if (!SameKeys(std::get<Curve>(a.Volume), std::get<Curve>(b.Volume)))
            return false;
        if (!SameKeys(a.AirAbsorption, b.AirAbsorption) || !SameKeys(a.Spread, b.Spread))
            return false;
        if (a.Cone.has_value() != b.Cone.has_value())
            return false;
        if (a.Cone && (a.Cone->InnerAngle != b.Cone->InnerAngle || a.Cone->OuterAngle != b.Cone->OuterAngle ||
                       a.Cone->OuterGain != b.Cone->OuterGain))
            return false;
        return a.DopplerFactor == b.DopplerFactor;
    }

    void AttenuationDocument::Reset(AudioAttenuationData data, bool saved)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_GestureOpen = false;
        m_Saved.reset();
        Set(std::move(data));
        if (saved)
            m_Saved = m_Data;
    }

    bool AttenuationDocument::Commit(AudioAttenuationData next, bool continuous)
    {
        if (Equal(next, m_Data))
            return false;
        // A gesture already in progress has its starting state on the undo stack.
        if (!(continuous && m_GestureOpen))
        {
            m_Undo.push_back(m_Data);
            if (m_Undo.size() > MaxHistory)
                m_Undo.erase(m_Undo.begin());
        }
        m_GestureOpen = continuous;
        m_Redo.clear();
        Set(std::move(next));
        return true;
    }

    bool AttenuationDocument::Undo()
    {
        if (m_Undo.empty())
            return false;
        m_GestureOpen = false;
        m_Redo.push_back(m_Data);
        auto previous = std::move(m_Undo.back());
        m_Undo.pop_back();
        Set(std::move(previous));
        return true;
    }

    bool AttenuationDocument::Redo()
    {
        if (m_Redo.empty())
            return false;
        m_GestureOpen = false;
        m_Undo.push_back(m_Data);
        auto next = std::move(m_Redo.back());
        m_Redo.pop_back();
        Set(std::move(next));
        return true;
    }

    std::optional<std::string> AttenuationDocument::GetError() const
    {
        if (auto error = ValidateAudioAttenuation(m_Data))
            return error->Message;
        return std::nullopt;
    }

    void AttenuationDocument::Set(AudioAttenuationData data)
    {
        m_Data = std::move(data);
        ++m_Revision;
    }

    // ------------------------------------------------------------------ files

    std::filesystem::path UniqueAttenuationFile(const std::filesystem::path& directory, const std::string& stem)
    {
        const std::string base = stem.empty() ? std::string("NewAudioAttenuation") : stem;
        auto              file = directory / std::filesystem::u8path(base + ".audioattenuation");
        std::error_code   error;
        for (int i = 2; std::filesystem::exists(file, error); ++i)
            file = directory / std::filesystem::u8path(base + " " + std::to_string(i) + ".audioattenuation");
        return file;
    }

    std::optional<AssetError> SaveAttenuationFile(const std::filesystem::path& file, const AudioAttenuationData& data)
    {
        auto text = SerializeAudioAttenuation(data);
        if (const auto* error = std::get_if<AssetError>(&text))
            return *error;
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!(stream << std::get<std::string>(text)) || !stream.flush())
            return AssetError {AssetErrorCode::IoError, "Cannot write " + file.u8string()};
        return std::nullopt;
    }

    std::variant<AudioAttenuationData, AssetError> LoadAttenuationFile(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return AssetError {AssetErrorCode::IoError, "Cannot read " + file.filename().u8string()};
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return ParseAudioAttenuation(text);
    }

    std::variant<std::filesystem::path, AssetError> CreateAttenuationFile(const std::filesystem::path& directory,
                                                                          const std::string&           stem,
                                                                          const AudioAttenuationData&  data)
    {
        const auto file = UniqueAttenuationFile(directory, stem);
        if (auto error = SaveAttenuationFile(file, data))
            return *error;
        return file;
    }
} // namespace Kans::AudioAttenuationEditing
