#include "AudioAttenuationEditorPanel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Kans3D/ImGui/EditorTheme.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/UI/SourceBuild.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    using namespace AudioAttenuationEditing;
    using EditorTheme::Px;

    namespace
    {
        ImRect LastItem() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

        bool ToolButton(const char* label, const char* tooltip, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Button(label);
            ImGui::EndDisabled();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        // A row of buttons, one per choice, the chosen one lit; `boxes` receives each button.
        bool ChoiceRow(const char*       label,
                       const char* const names[],
                       int               count,
                       int&              index,
                       ImRect*           boxes,
                       const char*       tooltip = nullptr)
        {
            if (!PropertyGrid::Row(label, tooltip))
                return false;
            const auto& p       = EditorTheme::Colors();
            bool        changed = false;
            ImGui::PushID(label);
            const float spacing = ImGui::GetStyle().ItemSpacing.x * 0.5f;
            const float width   = (ImGui::CalcItemWidth() - spacing * float(count - 1)) / float(count);
            for (int i = 0; i < count; ++i)
            {
                if (i)
                    ImGui::SameLine(0.0f, spacing);
                const bool chosen = i == index;
                if (chosen)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, p.Selection);
                    ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
                }
                if (ImGui::Button(names[i], {width, 0}) && !chosen)
                    index = i, changed = true;
                if (chosen)
                    ImGui::PopStyleColor(2);
                if (boxes)
                    boxes[i] = LastItem();
            }
            ImGui::PopID();
            return changed;
        }

        // A float row keeping its field's box; true while the value changed this frame.
        bool FloatRow(const char* label,
                      float&      value,
                      float       speed,
                      float       lower,
                      float       upper,
                      const char* format,
                      ImRect&     box,
                      const char* tooltip = nullptr)
        {
            if (!PropertyGrid::Row(label, tooltip))
                return false;
            ImGui::PushID(label);
            const bool changed =
                ImGui::DragFloat("##value", &value, speed, lower, upper, format, ImGuiSliderFlags_AlwaysClamp);
            box = LastItem();
            ImGui::PopID();
            return changed;
        }

        // 1, 2 or 5 times a power of ten, near `rough`.
        float NiceStep(float rough)
        {
            if (!(rough > 0.0f) || !std::isfinite(rough))
                return 1.0f;
            const float power    = std::pow(10.0f, std::floor(std::log10(rough)));
            const float fraction = rough / power;
            return power * (fraction < 1.5f ? 1.0f : fraction < 3.5f ? 2.0f : fraction < 7.5f ? 5.0f : 10.0f);
        }

        std::string ValueText(CurveKind kind, float value)
        {
            char text[48];
            if (kind == CurveKind::Volume)
                std::snprintf(text, sizeof(text), "%.3f (%s)", value, GainText(value).c_str());
            else if (kind == CurveKind::AirAbsorption)
                return CutoffText(value);
            else
                std::snprintf(text, sizeof(text), "%.2f", value);
            return text;
        }

        constexpr const char* KindNames[3] = {"Volume", "Air absorption", "Spread"};
    } // namespace

    // ------------------------------------------------------------------ files

    bool AudioAttenuationEditorPanel::Open(const std::filesystem::path& file)
    {
        auto loaded = LoadAttenuationFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            m_Status = file.filename().u8string() + ": " + error->Message;
            return false;
        }
        // Switching files keeps valid edits instead of dropping them.
        if (m_Opened && m_File != file && m_Document.IsDirty() && !m_Document.GetError())
            Save();
        m_Opened = true;
        m_File   = file;
        m_Pending.reset();
        m_Document.Reset(std::get<AudioAttenuationData>(std::move(loaded)));
        m_Status.clear();
        m_Kind = CurveKind::Volume;
        m_Selected.reset();
        m_DragKey.reset();
        m_DragProbe = m_Panning = false;
        m_ViewSet               = false;
        m_Probe                 = CurveExtent(m_Document.Get(), m_Kind) * 0.5f;
        return true;
    }

    bool AudioAttenuationEditorPanel::Save()
    {
        if (!m_Opened)
            return false;
        if (auto error = m_Document.GetError())
        {
            m_Status = "Not saved: " + *error;
            return false;
        }
        if (m_Service && m_Service->IsReadOnly(m_File))
        {
            m_Status = "Plugin content is read-only.";
            return false;
        }
        if (auto error = SaveAttenuationFile(m_File, m_Document.Get()))
        {
            m_Status = error->Message;
            return false;
        }
        m_Document.MarkSaved();
        m_Status = BuildSavedSource(m_Service, m_File);
        return true;
    }

    bool AudioAttenuationEditorPanel::Undo()
    {
        m_Pending.reset();
        m_DragKey.reset();
        const bool undone = m_Document.Undo();
        ClampSelection();
        return undone;
    }

    bool AudioAttenuationEditorPanel::Redo()
    {
        m_Pending.reset();
        m_DragKey.reset();
        const bool redone = m_Document.Redo();
        ClampSelection();
        return redone;
    }

    // ------------------------------------------------------------------ view and probe

    void AudioAttenuationEditorPanel::SetKind(CurveKind kind)
    {
        if (kind == m_Kind)
            return;
        m_Kind = kind;
        m_Selected.reset();
        m_DragKey.reset();
        FitView();
    }

    void AudioAttenuationEditorPanel::FitView()
    {
        m_View    = AutoView(Current(), m_Kind);
        m_ViewSet = true;
    }

    void AudioAttenuationEditorPanel::ClampSelection()
    {
        const auto* keys = CurveOf(m_Document.Get(), m_Kind);
        if (m_Selected && (!keys || *m_Selected >= keys->size()))
            m_Selected.reset();
    }

    void AudioAttenuationEditorPanel::SetProbeDistance(float distance)
    {
        m_Probe = std::isfinite(distance) ? std::max(0.0f, distance) : m_Probe;
    }

    std::string AudioAttenuationEditorPanel::GetProbeText() const { return ProbeText(m_Document.Get(), m_Probe); }

    ImVec2 AudioAttenuationEditorPanel::ToScreen(float distance, float value) const
    {
        const auto at = ToPlot(m_View, {m_PlotOrigin.x, m_PlotOrigin.y}, {m_PlotSize.x, m_PlotSize.y}, distance, value);
        return {at.x, at.y};
    }

    // ------------------------------------------------------------------ edits

    void AudioAttenuationEditorPanel::Edit(const std::function<void(AudioAttenuationData&)>& change)
    {
        auto next = Current();
        change(next);
        m_PendingContinuous = (m_Pending ? m_PendingContinuous : true) && ImGui::IsAnyItemActive();
        m_Pending           = std::move(next);
    }

    void AudioAttenuationEditorPanel::CommitPending()
    {
        if (!m_Pending)
            return;
        auto data = std::move(*m_Pending);
        m_Pending.reset();
        m_Document.Commit(std::move(data), m_PendingContinuous);
    }

    // ------------------------------------------------------------------ drawing

    void AudioAttenuationEditorPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_Boxes   = {};
        if (!m_Opened)
        {
            ImGui::TextDisabled(
                "%s", m_Status.empty() ? "Open a .audioattenuation file from the Content Browser." : m_Status.c_str());
            return;
        }
        const auto& io = ImGui::GetIO();
        if (m_Focused && io.KeyCtrl && !io.WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false) && m_Document.IsDirty())
                Save();
        }
        DrawHeader();
        DrawTabs();
        DrawGraph();
        DrawKeyFields();
        DrawVolume();
        DrawAirAbsorption();
        DrawSpread();
        DrawCone();
        CommitPending();
        if (!ImGui::IsAnyItemActive())
        {
            m_Document.EndGesture();
            m_DragKey.reset();
            m_DragProbe = false;
        }
        ClampSelection();
    }

    void AudioAttenuationEditorPanel::DrawHeader()
    {
        const auto& p    = EditorTheme::Colors();
        std::string name = m_File.filename().u8string();
        if (m_Document.IsDirty())
            name += " *";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.TypeAudio, ICON_FA_CHART_LINE);
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_File.u8string().c_str());
        const auto error = m_Document.GetError();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_FLOPPY_DISK " Save",
                       error ? "Fix the problem above to save" : "Write the file and rebuild it (Ctrl+S)",
                       m_Document.IsDirty() && !error))
            Save();
        m_Boxes.Save = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_ROTATE_LEFT "##undo", "Undo (Ctrl+Z)", m_Document.CanUndo()))
            Undo();
        m_Boxes.Undo = LastItem();
        ImGui::SameLine();
        if (ToolButton(ICON_FA_ROTATE_RIGHT "##redo", "Redo (Ctrl+Y)", m_Document.CanRedo()))
            Redo();
        m_Boxes.Redo = LastItem();
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
        // Why the asset cannot be saved.
        if (error)
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Text, p.Danger);
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", error->c_str());
            ImGui::PopStyleColor();
            m_Boxes.Validation = {start, ImGui::GetItemRectMax()};
        }
    }

    void AudioAttenuationEditorPanel::DrawTabs()
    {
        const auto& p = EditorTheme::Colors();
        for (int i = 0; i < 3; ++i)
        {
            if (i)
                ImGui::SameLine(0.0f, Px(2.0f));
            const bool chosen = int(m_Kind) == i;
            if (chosen)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, p.Selection);
                ImGui::PushStyleColor(ImGuiCol_Text, p.Text);
            }
            if (ImGui::Button(KindNames[i]))
                SetKind(CurveKind(i));
            if (chosen)
                ImGui::PopStyleColor(2);
            m_Boxes.Tabs[i] = LastItem();
        }
        ImGui::SameLine();
        if (ToolButton(ICON_FA_EXPAND " Fit", "Show the whole curve (wheel zooms, middle drag pans)"))
            FitView();
        m_Boxes.Fit = LastItem();
    }

    void AudioAttenuationEditorPanel::DrawGraph()
    {
        const auto& p  = EditorTheme::Colors();
        const auto& io = ImGui::GetIO();
        if (!m_ViewSet)
            FitView();
        const CurveKind kind = m_Kind;
        // Room for the value labels on the left and the distance labels below.
        const float  left = Px(48.0f), bottom = Px(18.0f), pad = Px(6.0f);
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const ImVec2 extent = {std::max(Px(120.0f), ImGui::GetContentRegionAvail().x), Px(230.0f)};
        ImGui::InvisibleButton("##plot",
                               extent,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        if (hovered)
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        m_PlotOrigin = {start.x + left, start.y + pad};
        m_PlotSize   = {extent.x - left - pad, extent.y - bottom - pad * 2.0f};
        const ImVec2 plotEnd {m_PlotOrigin.x + m_PlotSize.x, m_PlotOrigin.y + m_PlotSize.y};
        m_Boxes.Plot = {m_PlotOrigin, plotEnd};
        const glm::vec2 origin {m_PlotOrigin.x, m_PlotOrigin.y}, size {m_PlotSize.x, m_PlotSize.y};

        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(m_PlotOrigin, plotEnd, ImGui::GetColorU32(p.Inset), Px(3.0f));
        draw->PushClipRect(start, {start.x + extent.x, start.y + extent.y}, true);
        const ImU32 grid = ImGui::GetColorU32(ImVec4(p.Line.x, p.Line.y, p.Line.z, 0.6f));
        const ImU32 dim  = ImGui::GetColorU32(p.TextDim);

        // Distance ticks.
        const float width = m_View.DistanceEnd - m_View.DistanceStart;
        const float step  = NiceStep(width / std::max(2.0f, m_PlotSize.x / Px(90.0f)));
        for (float d = std::ceil(m_View.DistanceStart / step) * step; d <= m_View.DistanceEnd + step * 1e-3f; d += step)
        {
            const float x = ToScreen(d, 0.0f).x;
            draw->AddLine({x, m_PlotOrigin.y}, {x, plotEnd.y}, grid);
            char label[32];
            std::snprintf(label, sizeof(label), step < 1.0f ? "%.1f m" : "%.0f m", d);
            const ImVec2 text = ImGui::CalcTextSize(label);
            draw->AddText(
                {std::clamp(x - text.x * 0.5f, m_PlotOrigin.x, plotEnd.x - text.x), plotEnd.y + Px(2.0f)}, dim, label);
        }
        // Value ticks: gain quarters, decades of Hz, spread quarters.
        std::vector<std::pair<float, std::string>> ticks;
        if (kind == CurveKind::AirAbsorption)
            ticks = {{20.0f, "20"}, {100.0f, "100"}, {1000.0f, "1k"}, {10000.0f, "10k"}};
        else
            ticks = {{0.0f, "0"}, {0.25f, "0.25"}, {0.5f, "0.5"}, {0.75f, "0.75"}, {1.0f, "1"}};
        for (const auto& [value, label] : ticks)
        {
            const float y = ToScreen(m_View.DistanceStart, value).y;
            draw->AddLine({m_PlotOrigin.x, y}, {plotEnd.x, y}, grid);
            const ImVec2 text = ImGui::CalcTextSize(label.c_str());
            draw->AddText({m_PlotOrigin.x - text.x - Px(4.0f), y - text.y * 0.5f}, dim, label.c_str());
        }

        // The curve, sampled as the engine evaluates it.
        const auto&         data   = Current();
        const int           points = std::clamp(int(m_PlotSize.x / Px(2.0f)), 64, 400);
        std::vector<ImVec2> line(static_cast<size_t>(points));
        for (int i = 0; i < points; ++i)
        {
            const float distance = m_View.DistanceStart + width * float(i) / float(points - 1);
            line[size_t(i)]      = ToScreen(distance, Sample(data, kind, distance));
        }
        draw->PushClipRect(m_PlotOrigin, plotEnd, true);
        draw->AddPolyline(line.data(), points, ImGui::GetColorU32(p.TypeAudio), 0, Px(2.0f));
        m_Boxes.PlotPoints = size_t(points);

        // Keys.
        const auto*           keys  = CurveOf(data, kind);
        const ImVec2          mouse = io.MousePos;
        std::optional<size_t> hot;
        float                 best = Px(7.0f) * Px(7.0f);
        if (keys)
            for (size_t i = 0; i < keys->size(); ++i)
            {
                const ImVec2 at = ToScreen((*keys)[i].Distance, (*keys)[i].Value);
                m_Boxes.Keys.push_back(at);
                const float dx = at.x - mouse.x, dy = at.y - mouse.y;
                if (hovered && dx * dx + dy * dy <= best)
                    best = dx * dx + dy * dy, hot = i;
            }
        for (size_t i = 0; i < m_Boxes.Keys.size(); ++i)
        {
            const bool  selected = m_Selected == i;
            const ImU32 fill     = ImGui::GetColorU32(selected ? p.Accent : hot == i ? p.Text : p.TypeAudio);
            draw->AddCircleFilled(m_Boxes.Keys[i], Px(selected ? 5.0f : 4.0f), fill);
            draw->AddCircle(m_Boxes.Keys[i], Px(selected ? 5.0f : 4.0f), ImGui::GetColorU32(p.Panel), 0, 1.0f);
        }

        // The probe: a marker at a distance, read out below the graph.
        const float probeX = ToScreen(m_Probe, 0.0f).x;
        draw->AddLine({probeX, m_PlotOrigin.y}, {probeX, plotEnd.y}, ImGui::GetColorU32(p.Warning), Px(1.5f));
        draw->AddTriangleFilled({probeX - Px(5.0f), m_PlotOrigin.y},
                                {probeX + Px(5.0f), m_PlotOrigin.y},
                                {probeX, m_PlotOrigin.y + Px(7.0f)},
                                ImGui::GetColorU32(p.Warning));
        const ImVec2 probePoint = ToScreen(m_Probe, Sample(data, kind, m_Probe));
        draw->AddCircleFilled(probePoint, Px(3.5f), ImGui::GetColorU32(p.Warning));
        m_Boxes.Probe = {{probeX - Px(5.0f), m_PlotOrigin.y}, {probeX + Px(5.0f), plotEnd.y}};
        if (!keys)
        {
            const char*  hint = kind == CurveKind::Volume ?
                                    "Built-in model: choose Curve keys under Volume to edit keys" :
                                kind == CurveKind::AirAbsorption ? "Air absorption is off (no filter)" :
                                                                   "Spread is off (a point source)";
            const ImVec2 text = ImGui::CalcTextSize(hint);
            draw->AddText({plotEnd.x - text.x - Px(6.0f), m_PlotOrigin.y + Px(6.0f)}, dim, hint);
        }
        draw->PopClipRect();
        draw->PopClipRect();

        // Interaction.
        const glm::vec2 under     = FromPlot(m_View, origin, size, {mouse.x, mouse.y});
        const bool      nearProbe = hovered && std::abs(mouse.x - probeX) <= Px(5.0f);
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hot)
            {
                // The key keeps its offset from the pointer, so grabbing it off-centre does not move it.
                m_Selected = m_DragKey = hot;
                m_Grab                 = {m_Boxes.Keys[*hot].x - mouse.x, m_Boxes.Keys[*hot].y - mouse.y};
            }
            else if (nearProbe)
                m_DragProbe = true;
            else if (keys)
            {
                // A new key under the pointer, then dragged like any other until release.
                auto       next  = data;
                const auto index = InsertKey(*CurveOf(next, kind), {under.x, under.y}, kind);
                if (index)
                {
                    Edit([next](AudioAttenuationData& edited) { edited = next; });
                    m_Selected = m_DragKey = index;
                    m_Grab                 = {0.0f, 0.0f};
                }
            }
            else
            {
                // Nothing to edit here: the probe jumps to the click.
                SetProbeDistance(under.x);
                m_DragProbe = true;
            }
        }
        if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // A click selects; the key moves once the pointer has been dragged past ImGui's threshold.
            if (m_DragKey && keys && *m_DragKey < keys->size() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const size_t    index  = *m_DragKey;
                const glm::vec2 target = FromPlot(m_View, origin, size, {mouse.x + m_Grab.x, mouse.y + m_Grab.y});
                Edit([kind, index, target](AudioAttenuationData& edited) {
                    if (auto* curve = CurveOf(edited, kind))
                        MoveKey(*curve, index, target.x, target.y, kind);
                });
            }
            else if (m_DragProbe)
                SetProbeDistance(under.x);
        }
        // Right-click or Delete removes a key.
        std::optional<size_t> remove;
        if (hovered && hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            remove = hot;
        else if ((hovered || m_Focused) && m_Selected && keys && !io.WantTextInput &&
                 ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            remove = m_Selected;
        if (remove)
        {
            const size_t index = *remove;
            Edit([kind, index](AudioAttenuationData& edited) {
                if (auto* curve = CurveOf(edited, kind))
                    RemoveKey(*curve, index);
            });
            m_Selected.reset();
            m_DragKey.reset();
            hot.reset();
        }
        // Wheel zooms the distances about the pointer; middle drag pans.
        if (hovered && io.MouseWheel != 0.0f)
            m_View = Zoom(m_View, under.x, std::pow(1.2f, io.MouseWheel));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            m_Panning = true;
        if (m_Panning)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                m_View = Pan(m_View, -io.MouseDelta.x * width / std::max(1.0f, m_PlotSize.x));
            else
                m_Panning = false;
        }
        if (hovered && !active)
        {
            if (hot && keys && *hot < keys->size())
                ImGui::SetTooltip("Key %zu: %.2f m, %s\nDrag to move, right-click or Delete to remove",
                                  *hot + 1,
                                  (*keys)[*hot].Distance,
                                  ValueText(kind, (*keys)[*hot].Value).c_str());
            else if (nearProbe)
                ImGui::SetTooltip("Probe: drag to read the curves at another distance");
            else if (keys)
                ImGui::SetTooltip("Click to add a key at %.2f m", std::max(0.0f, under.x));
        }

        // The probe's readout.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.Warning, ICON_FA_CROSSHAIRS);
        ImGui::SameLine();
        ImGui::TextUnformatted(ProbeText(data, m_Probe).c_str());
        m_Boxes.ProbeReadout = LastItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What a listener hears at the probe's distance: gain, air absorption cutoff, spread");
    }

    void AudioAttenuationEditorPanel::DrawKeyFields()
    {
        const auto* keys = CurveOf(Current(), m_Kind);
        if (!keys || !m_Selected || *m_Selected >= keys->size())
            return;
        if (!PropertyGrid::BeginGroup("Selected key"))
            return;
        const size_t    index    = *m_Selected;
        const CurveKind kind     = m_Kind;
        const auto      key      = (*keys)[index];
        const auto      limit    = DistanceLimits(*keys, index);
        float           distance = key.Distance;
        if (FloatRow("Distance",
                     distance,
                     0.01f,
                     limit.x,
                     limit.y,
                     "%.3f m",
                     m_Boxes.KeyDistance,
                     "Between the neighbouring keys"))
            Edit([kind, index, distance](AudioAttenuationData& edited) {
                if (auto* curve = CurveOf(edited, kind); curve && index < curve->size())
                    MoveKey(*curve, index, distance, (*curve)[index].Value, kind);
            });
        const auto  values = ValueLimits(*keys, index, kind);
        float       value  = key.Value;
        const char* format = kind == CurveKind::AirAbsorption ? "%.0f Hz" : "%.3f";
        if (FloatRow(kind == CurveKind::Volume        ? "Gain" :
                     kind == CurveKind::AirAbsorption ? "Cutoff" :
                                                        "Spread",
                     value,
                     kind == CurveKind::AirAbsorption ? 10.0f : 0.005f,
                     values.x,
                     values.y,
                     format,
                     m_Boxes.KeyValue))
            Edit([kind, index, value](AudioAttenuationData& edited) {
                if (auto* curve = CurveOf(edited, kind); curve && index < curve->size())
                    MoveKey(*curve, index, (*curve)[index].Distance, value, kind);
            });
        const char* modes[] = {"Linear", "Smooth"};
        int         mode    = int(key.Interpolation);
        if (ChoiceRow("Interpolation",
                      modes,
                      2,
                      mode,
                      m_Boxes.KeyInterpolation,
                      "How the curve runs from this key to the next (Smooth never overshoots)"))
            Edit([kind, index, mode](AudioAttenuationData& edited) {
                if (auto* curve = CurveOf(edited, kind); curve && index < curve->size())
                    (*curve)[index].Interpolation = AudioCurveInterpolation(mode);
            });
        if (kind == CurveKind::Volume && PropertyGrid::Row("", nullptr))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", GainText(key.Value).c_str());
        }
        PropertyGrid::EndGroup();
    }

    void AudioAttenuationEditorPanel::DrawVolume()
    {
        if (!PropertyGrid::BeginGroup("Volume"))
            return;
        const auto& data    = Current();
        const char* modes[] = {"Curve keys", "Built-in model"};
        int         mode    = IsPreset(data) ? 1 : 0;
        if (ChoiceRow("Mode",
                      modes,
                      2,
                      mode,
                      m_Boxes.VolumeMode,
                      "Keys drawn in the graph, or one of the built-in distance models"))
        {
            Edit([mode](AudioAttenuationData& edited) { SetPresetMode(edited, mode == 1); });
            if (m_Kind == CurveKind::Volume)
                m_Selected.reset();
        }
        if (const auto* preset = std::get_if<AudioAttenuationPreset>(&data.Volume))
        {
            const char* models[] = {"Inverse", "Linear", "Exponential"};
            int         model    = std::clamp(int(preset->Model) - 1, 0, 2);
            if (ChoiceRow("Model", models, 3, model, m_Boxes.Model))
                Edit([model](AudioAttenuationData& edited) {
                    std::get<AudioAttenuationPreset>(edited.Volume).Model = AudioAttenuationModel(model + 1);
                });
            // The fields are not clamped against each other: a max below the min shows why it cannot be saved.
            float       values[3]  = {preset->MinDistance, preset->MaxDistance, preset->Rolloff};
            const char* labels[3]  = {"Min distance", "Max distance", "Rolloff"};
            const char* formats[3] = {"%.2f m", "%.2f m", "%.2f"};
            const char* tips[3]    = {"Full volume up to here", "The model stops falling here", "How fast it falls"};
            for (int i = 0; i < 3; ++i)
                if (FloatRow(labels[i],
                             values[i],
                             i == 1 ? 0.5f : 0.01f,
                             0.0f,
                             100000.0f,
                             formats[i],
                             m_Boxes.PresetFields[i],
                             tips[i]))
                {
                    const float value = values[i];
                    Edit([i, value](AudioAttenuationData& edited) {
                        auto& target = std::get<AudioAttenuationPreset>(edited.Volume);
                        (i == 0 ? target.MinDistance : i == 1 ? target.MaxDistance : target.Rolloff) = value;
                    });
                }
        }
        else if (PropertyGrid::Row("Keys"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%zu (edit them in the graph's Volume tab)",
                                std::get<std::vector<AudioCurveKey>>(data.Volume).size());
        }
        PropertyGrid::EndGroup();
    }

    void AudioAttenuationEditorPanel::DrawAirAbsorption()
    {
        if (!PropertyGrid::BeginGroup("Air Absorption"))
            return;
        const auto& data    = Current();
        const char* modes[] = {"Off", "Near-far range", "Curve"};
        int         mode    = int(AirModeOf(data));
        if (ChoiceRow("Mode",
                      modes,
                      3,
                      mode,
                      m_Boxes.AirMode,
                      "A low-pass whose cutoff falls with distance: off, falling linearly over a range, or keys"))
        {
            Edit([mode](AudioAttenuationData& edited) { SetAirMode(edited, AirMode(mode)); });
            if (m_Kind == CurveKind::AirAbsorption)
                m_Selected.reset();
            if (mode == int(AirMode::Curve))
                SetKind(CurveKind::AirAbsorption);
        }
        if (AirModeOf(data) == AirMode::Range)
        {
            const auto  range     = AirRangeOf(data);
            float       values[4] = {range.StartDistance, range.EndDistance, range.NearCutoffHz, range.FarCutoffHz};
            const char* labels[4] = {"Start distance", "End distance", "Near cutoff", "Far cutoff"};
            for (int i = 0; i < 4; ++i)
                if (FloatRow(labels[i],
                             values[i],
                             i < 2 ? 0.1f : 10.0f,
                             i < 2 ? 0.0f : 20.0f,
                             i < 2 ? 100000.0f : 24000.0f,
                             i < 2 ? "%.2f m" : "%.0f Hz",
                             m_Boxes.AirRange[i]))
                {
                    AirRange next               = range;
                    (i == 0 ? next.StartDistance :
                     i == 1 ? next.EndDistance :
                     i == 2 ? next.NearCutoffHz :
                              next.FarCutoffHz) = values[i];
                    Edit([next](AudioAttenuationData& edited) { SetAirRange(edited, next); });
                }
        }
        else if (AirModeOf(data) == AirMode::Curve && PropertyGrid::Row("Keys"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%zu (edit them in the graph's Air absorption tab)", data.AirAbsorption.size());
        }
        PropertyGrid::EndGroup();
    }

    void AudioAttenuationEditorPanel::DrawSpread()
    {
        if (!PropertyGrid::BeginGroup("Spread"))
            return;
        const auto& data    = Current();
        const char* modes[] = {"Off", "Curve"};
        int         mode    = data.Spread.empty() ? 0 : 1;
        if (ChoiceRow("Mode",
                      modes,
                      2,
                      mode,
                      m_Boxes.SpreadMode,
                      "How wide the sound is by distance: 1 fills the speakers, 0 is a point"))
        {
            Edit([mode](AudioAttenuationData& edited) { SetSpread(edited, mode == 1); });
            if (m_Kind == CurveKind::Spread)
                m_Selected.reset();
            if (mode == 1)
                SetKind(CurveKind::Spread);
        }
        PropertyGrid::EndGroup();
    }

    void AudioAttenuationEditorPanel::DrawCone()
    {
        const auto& data = Current();
        bool        cone = data.Cone.has_value(), toggled = false;
        // The header's toggle turns the override on (the source's own cone applies while it is off).
        const bool open = PropertyGrid::BeginGroup("Cone", &cone, &toggled);
        // The heading's toggle: a box 6 px shorter than the heading, after the chevron.
        ImVec2 headingStart, headingEnd;
        if (PropertyGrid::LastGroupHeading(headingStart, headingEnd))
        {
            const float side = headingEnd.y - headingStart.y - Px(6.0f);
            m_Boxes.Cone     = {{headingStart.x + Px(18.0f), headingStart.y + Px(3.0f)},
                                {headingStart.x + Px(18.0f) + side, headingStart.y + Px(3.0f) + side}};
        }
        if (open)
        {
            if (data.Cone)
            {
                const auto& c     = *data.Cone;
                float       inner = c.InnerAngle, outer = c.OuterAngle, gain = c.OuterGain;
                ImRect      ignored;
                if (FloatRow("Inner angle", inner, 0.5f, 0.0f, 360.0f, "%.1f°", ignored))
                    Edit([inner](AudioAttenuationData& edited) {
                        if (edited.Cone)
                            edited.Cone->InnerAngle = inner;
                    });
                if (FloatRow("Outer angle", outer, 0.5f, 0.0f, 360.0f, "%.1f°", ignored))
                    Edit([outer](AudioAttenuationData& edited) {
                        if (edited.Cone)
                            edited.Cone->OuterAngle = outer;
                    });
                if (FloatRow("Outer gain", gain, 0.01f, 0.0f, 1.0f, "%.2f", ignored))
                    Edit([gain](AudioAttenuationData& edited) {
                        if (edited.Cone)
                            edited.Cone->OuterGain = gain;
                    });
            }
            else if (PropertyGrid::Row(""))
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Off: each source keeps its own cone");
            }
            PropertyGrid::EndGroup();
        }
        if (toggled)
            Edit([cone](AudioAttenuationData& edited) {
                if (cone)
                    edited.Cone = AudioAttenuationCone {90.0f, 180.0f, 0.25f};
                else
                    edited.Cone.reset();
            });
        if (PropertyGrid::BeginGroup("Doppler"))
        {
            float doppler = data.DopplerFactor;
            if (FloatRow("Doppler factor",
                         doppler,
                         0.01f,
                         0.0f,
                         10.0f,
                         "%.2f",
                         m_Boxes.Doppler,
                         "How strongly motion shifts the pitch (0: off, 1: physical)"))
                Edit([doppler](AudioAttenuationData& edited) { edited.DopplerFactor = doppler; });
            PropertyGrid::EndGroup();
        }
    }
} // namespace Kans
