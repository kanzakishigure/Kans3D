#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "KansEditorCore/Audio/AudioAttenuationEditing.h"

namespace Kans
{
    class EditorAssetImportService;

    // The attenuation editor: the open .audioattenuation's curves in an interactive graph (distance
    // across, value up; tabs for the volume, the air absorption cutoff on a log-Hz axis and the
    // spread), its keys (click empty space to add, drag to move between the neighbours, right-click
    // or Delete to remove, per-key Linear/Smooth, numeric fields for the selected one), the volume
    // mode (curve keys or a built-in model, plotted the same way), the Air Absorption, Spread, Cone
    // and Doppler groups, and a probe: a draggable distance marker reading out the gain, cutoff and
    // spread there. The plotted line is sampled from EvaluateAttenuation, so it is what the engine
    // plays. Every edit goes into the panel's own undo history (a drag is one step);
    // ValidateAudioAttenuation's reason shows at the top and an invalid asset is never saved. Save
    // writes the source and rebuilds it, so hot reload applies it. Wheel zooms the distances about
    // the pointer, middle drag pans, Fit shows the whole curve again.
    class AudioAttenuationEditorPanel
    {
    public:
        using CurveKind = AudioAttenuationEditing::CurveKind;

        // The project's import service (may be null).
        void Bind(EditorAssetImportService* service) { m_Service = service; }

        // Loads a .audioattenuation; false (with the reason in the status) when it cannot be read.
        bool Open(const std::filesystem::path& file);
        // Writes the file when the asset is valid; false (with the reason in the status) otherwise.
        bool Save();

        void Draw();
        bool Undo();
        bool Redo();
        bool HasFocus() const { return m_Focused; }

        AudioAttenuationEditing::AttenuationDocument&       GetDocument() { return m_Document; }
        const AudioAttenuationEditing::AttenuationDocument& GetDocument() const { return m_Document; }
        const std::filesystem::path&                        GetFile() const { return m_File; }
        const std::string&                                  GetStatus() const { return m_Status; }

        // The curve shown in the graph and its visible range.
        CurveKind                                 GetKind() const { return m_Kind; }
        void                                      SetKind(CurveKind kind);
        const AudioAttenuationEditing::CurveView& GetView() const { return m_View; }
        void SetView(const AudioAttenuationEditing::CurveView& view) { m_View = view, m_ViewSet = true; }
        // The selected key of the shown curve (none without one).
        std::optional<size_t> GetSelectedKey() const { return m_Selected; }
        // The probe's distance and what it reads there (as the engine evaluates the asset).
        float       GetProbeDistance() const { return m_Probe; }
        void        SetProbeDistance(float distance);
        std::string GetProbeText() const;
        // A curve point on the screen, as the last frame drew the graph.
        ImVec2 ToScreen(float distance, float value) const;

        // Where the last frame drew each control (screen space), for tooltips and UI tests.
        struct Boxes
        {
            ImRect              Validation, Save, Undo, Redo, Fit, Plot, Probe, ProbeReadout;
            ImRect              Tabs[3];
            ImRect              VolumeMode[2];   // Curve keys, built-in model.
            ImRect              Model[3];        // Inverse, Linear, Exponential.
            ImRect              PresetFields[3]; // Min distance, max distance, rolloff.
            ImRect              KeyDistance, KeyValue, KeyInterpolation[2];
            ImRect              AirMode[3];  // Off, range, curve.
            ImRect              AirRange[4]; // Start, end, near cutoff, far cutoff.
            ImRect              SpreadMode[2];
            ImRect              Cone, Doppler;
            std::vector<ImVec2> Keys;           // The shown curve's keys.
            size_t              PlotPoints = 0; // Points of the drawn line.
        };
        const Boxes& GetBoxes() const { return m_Boxes; }

    private:
        void DrawHeader();
        void DrawTabs();
        void DrawGraph();
        void DrawKeyFields();
        void DrawVolume();
        void DrawAirAbsorption();
        void DrawSpread();
        void DrawCone();
        // Changes a copy of the asset, committed at the end of the frame (rows keep reading the asset
        // as it was); a continuous edit (a field or key held) joins the gesture.
        void Edit(const std::function<void(AudioAttenuationData&)>& change);
        void CommitPending();
        // The asset as edited this frame (the pending copy when there is one).
        const AudioAttenuationData& Current() const { return m_Pending ? *m_Pending : m_Document.Get(); }
        void                        FitView();
        void                        ClampSelection();

        EditorAssetImportService*                    m_Service = nullptr;
        AudioAttenuationEditing::AttenuationDocument m_Document;
        std::optional<AudioAttenuationData>          m_Pending;
        bool                                         m_PendingContinuous = false;
        std::filesystem::path                        m_File;
        bool                                         m_Opened = false, m_Focused = false;
        std::string                                  m_Status;
        CurveKind                                    m_Kind = CurveKind::Volume;
        AudioAttenuationEditing::CurveView           m_View;
        bool                                         m_ViewSet = false;
        std::optional<size_t>                        m_Selected;
        // A key dragged in the graph, the probe dragged, a middle-button pan in progress.
        std::optional<size_t> m_DragKey;
        bool                  m_DragProbe = false, m_Panning = false;
        float                 m_Probe = 10.0f;
        ImVec2                m_Grab {0, 0}; // The dragged key's offset from the pointer (pixels).
        ImVec2                m_PlotOrigin {0, 0}, m_PlotSize {1, 1};
        Boxes                 m_Boxes;
    };
} // namespace Kans
