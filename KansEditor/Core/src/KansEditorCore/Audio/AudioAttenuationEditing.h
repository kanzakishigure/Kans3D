#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <glm/vec2.hpp>
#include "Kans3D/Audio/AudioAttenuation.h"

// The attenuation editor's model, without ImGui: key edits that keep a curve's distances strictly
// increasing, the curve view's mapping (distance on X, value on Y, log-Hz for air absorption),
// the editing modes (curve keys or a built-in model; air absorption off, a near-far range or a
// curve), the probe readout, the ranges the viewport draws, the edited asset with its undo
// history, and new attenuation files.
namespace Kans::AudioAttenuationEditing
{
    // ---- Curves ----
    enum class CurveKind
    {
        Volume,
        AirAbsorption,
        Spread
    };
    // The keys of a curve; null for the volume curve while it is a built-in model.
    const std::vector<AudioCurveKey>* CurveOf(const AudioAttenuationData& data, CurveKind kind);
    std::vector<AudioCurveKey>*       CurveOf(AudioAttenuationData& data, CurveKind kind);
    // The values a curve's keys take: [0, 1] for volume and spread, [20, 24000] Hz for air absorption.
    float LowestValue(CurveKind kind);
    float HighestValue(CurveKind kind);

    // Neighbouring keys stay at least this far apart (metres).
    inline constexpr float KeyGap = 0.001f;
    // Inserts `key` where its distance belongs; none (nothing changed) when a key is already within
    // KeyGap of it or the distance is negative or not finite. The value is clamped to the kind's
    // range (and, for air absorption, between its neighbours, which keeps the cutoff nonincreasing).
    std::optional<size_t> InsertKey(std::vector<AudioCurveKey>& keys, AudioCurveKey key, CurveKind kind);
    bool                  RemoveKey(std::vector<AudioCurveKey>& keys, size_t index);
    // Moves a key to `distance` and `value`, clamped so it cannot reach or pass its neighbours (and
    // stays at or above 0 m) and its value stays in range. False when the index is out of range.
    bool MoveKey(std::vector<AudioCurveKey>& keys, size_t index, float distance, float value, CurveKind kind);
    // The distances a key may take: strictly between its neighbours (KeyGap inside them).
    glm::vec2 DistanceLimits(const std::vector<AudioCurveKey>& keys, size_t index);
    // The values a key may take (air absorption: between its neighbours' cutoffs).
    glm::vec2 ValueLimits(const std::vector<AudioCurveKey>& keys, size_t index, CurveKind kind);

    // ---- Modes ----
    bool IsPreset(const AudioAttenuationData& data);
    // Switches the volume to a built-in model (the default Inverse preset when it was keys) or to
    // keys sampled from the model (smooth, at its min and max distance and between them).
    void SetPresetMode(AudioAttenuationData& data, bool preset);
    // Keys following a model from its min to its max distance.
    std::vector<AudioCurveKey> KeysFromPreset(const AudioAttenuationPreset& preset);

    enum class AirMode
    {
        Off,   // No filter.
        Range, // A cutoff falling linearly from a near distance to a far one.
        Curve  // Any number of keys.
    };
    struct AirRange
    {
        float StartDistance = 10, EndDistance = 200, NearCutoffHz = 20000, FarCutoffHz = 2000;
    };
    // Off when there are no keys, Range for exactly two linear keys, Curve otherwise.
    AirMode  AirModeOf(const AudioAttenuationData& data);
    AirRange AirRangeOf(const AudioAttenuationData& data);
    void     SetAirRange(AudioAttenuationData& data, const AirRange& range);
    // Off clears the keys; Range keeps the first and last key (or the default range); Curve keeps
    // the keys (or starts from the default range).
    void SetAirMode(AudioAttenuationData& data, AirMode mode);
    // Spread off (a point source) or a curve starting wide and narrowing to a point.
    void SetSpread(AudioAttenuationData& data, bool on);

    // ---- The curve view ----
    // The visible part of a curve: distances on X, values on Y (log10 of Hz for air absorption).
    struct CurveView
    {
        float DistanceStart = 0, DistanceEnd = 100;
        float ValueLow = 0, ValueHigh = 1;
        bool  LogValue = false;
    };
    // The plot's furthest interesting distance: the last key (the model's max distance), at least 1 m.
    float CurveExtent(const AudioAttenuationData& data, CurveKind kind);
    // 0 to the extent plus a margin, over the kind's whole value range.
    CurveView AutoView(const AudioAttenuationData& data, CurveKind kind);
    // A point of the curve in the plot rectangle (`origin` the top-left corner, `size` its size).
    glm::vec2 ToPlot(const CurveView& view, glm::vec2 origin, glm::vec2 size, float distance, float value);
    // The distance and value under a plot position (x: distance, y: value).
    glm::vec2 FromPlot(const CurveView& view, glm::vec2 origin, glm::vec2 size, glm::vec2 position);
    // Zooms the distance axis by `factor` (above 1 zooms in) keeping `anchor` in place; never
    // shows negative distances or less than 1 cm.
    CurveView Zoom(const CurveView& view, float anchor, float factor);
    // Moves the distance axis by `delta` metres, never below 0.
    CurveView Pan(const CurveView& view, float delta);

    // The value of a curve kind at a distance, as the engine evaluates the asset.
    float Sample(const AudioAttenuationData& data, CurveKind kind, float distance);

    // ---- Readouts ----
    // "-6.0 dB" ("-inf dB" for silence).
    std::string GainText(float gain);
    // "4000 Hz" ("bypass" for a zero cutoff).
    std::string CutoffText(float hertz);
    // The probe: "12.50 m: -6.0 dB, 4000 Hz, spread 0.25".
    std::string ProbeText(const AudioAttenuationData& data, float distance);

    // ---- Ranges (viewport) ----
    // The distances the viewport draws: full volume inside `Inner`, silent (or the curve's end)
    // at `Outer`. HasOuter is false when the gain never changes (no attenuation).
    struct Range
    {
        float Inner = 0, Outer = 0;
        bool  HasOuter = false;
    };
    // Where a volume curve first reaches 0 (its first key at 0); none when it never does.
    std::optional<float> SilentDistance(const std::vector<AudioCurveKey>& keys);
    // A curve: from the end of its leading full-volume keys to where it first reaches 0, else its
    // last key. A model: its min and max distance.
    Range RangeOf(const AudioAttenuationData& data);
    // The source's built-in model: its min and (with a model) max distance.
    Range RangeOf(const AudioAttenuationModel model, float minDistance, float maxDistance);

    // ---- The edited asset ----
    bool Equal(const AudioAttenuationData& a, const AudioAttenuationData& b);
    class AttenuationDocument
    {
    public:
        static constexpr size_t MaxHistory = 200;

        void                        Reset(AudioAttenuationData data, bool saved = true);
        const AudioAttenuationData& Get() const { return m_Data; }
        uint64_t                    GetRevision() const { return m_Revision; }
        // False when `next` equals the current asset (nothing recorded). A continuous edit (a drag)
        // joins the open gesture, so a drag is one undo step.
        bool Commit(AudioAttenuationData next, bool continuous = false);
        void EndGesture() { m_GestureOpen = false; }
        bool CanUndo() const { return !m_Undo.empty(); }
        bool CanRedo() const { return !m_Redo.empty(); }
        bool Undo();
        bool Redo();
        bool IsDirty() const { return !m_Saved || !Equal(m_Data, *m_Saved); }
        void MarkSaved() { m_Saved = m_Data; }
        // ValidateAudioAttenuation's reason, or none when the asset can be saved.
        std::optional<std::string> GetError() const;

    private:
        void Set(AudioAttenuationData data);

        AudioAttenuationData                m_Data;
        std::optional<AudioAttenuationData> m_Saved;
        std::vector<AudioAttenuationData>   m_Undo, m_Redo;
        bool                                m_GestureOpen = false;
        uint64_t                            m_Revision    = 0;
    };

    // ---- Files ----
    // `directory`/`stem`.audioattenuation, or `stem` 2, 3... when taken.
    std::filesystem::path UniqueAttenuationFile(const std::filesystem::path& directory, const std::string& stem);
    // Writes `data` to `file`; refuses an invalid asset without touching the file.
    std::optional<AssetError> SaveAttenuationFile(const std::filesystem::path& file, const AudioAttenuationData& data);
    std::variant<AudioAttenuationData, AssetError> LoadAttenuationFile(const std::filesystem::path& file);
    // A new file (the built-in Inverse model by default) beside the others; returns its path.
    std::variant<std::filesystem::path, AssetError> CreateAttenuationFile(const std::filesystem::path& directory,
                                                                          const std::string&           stem,
                                                                          const AudioAttenuationData&  data = {});
} // namespace Kans::AudioAttenuationEditing
