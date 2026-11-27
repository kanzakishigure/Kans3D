#include "ProjectSettingsPanel.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Components/ComponentEditors.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        // Text drawn bottom-up from `position` (its baseline left), for column headings.
        void VerticalText(ImVec2 position, const char* text, ImU32 color)
        {
            ImDrawList* draw  = ImGui::GetWindowDrawList();
            const int   first = draw->VtxBuffer.Size;
            // Glyphs are clipped before they are turned: widen the clip to the right by the text's
            // length (the turned text ends left of that, inside the window's own bounds).
            const ImVec2 size = ImGui::CalcTextSize(text);
            ImVec2       min = draw->GetClipRectMin(), max = draw->GetClipRectMax();
            max.x += size.x;
            draw->PushClipRect(min, max, false);
            draw->AddText(position, color, text);
            draw->PopClipRect();
            // Rotate the glyphs a quarter turn counterclockwise about `position`.
            for (int i = first; i < draw->VtxBuffer.Size; ++i)
            {
                ImDrawVert& v  = draw->VtxBuffer[i];
                const float dx = v.pos.x - position.x, dy = v.pos.y - position.y;
                v.pos = {position.x + dy, position.y - dx};
            }
        }
    } // namespace

    void ProjectSettingsPanel::Bind(const std::filesystem::path& projectFile)
    {
        m_Bound = false;
        m_Status.clear();
        if (projectFile.empty())
            return;
        if (auto error = m_Physics.Load(projectFile))
        {
            m_Status = error->Message;
            return;
        }
        m_AudioStatus.clear();
        if (auto error = m_Audio.Load(projectFile))
            m_AudioStatus = error->Message;
        m_Bound = true;
    }

    void ProjectSettingsPanel::SetAssetChoices(const std::vector<EditorAssetChoice>& choices)
    {
        m_HRTFs.clear();
        for (const auto& choice : choices)
            if (choice.Type == AssetType::AudioHRTF)
                m_HRTFs.push_back(choice);
    }

    void ProjectSettingsPanel::Draw()
    {
        if (!m_Bound)
        {
            ImGui::TextDisabled("%s", m_Status.empty() ? "No project." : m_Status.c_str());
            return;
        }
        // Pages on the left (one so far), the page on the right.
        const float list = EditorTheme::Px(140.0f);
        if (ImGui::BeginChild("##Pages", {list, 0}, ImGuiChildFlags_Borders))
        {
            if (ImGui::Selectable(ICON_FA_ATOM "  Physics", m_Page == Page::Physics))
                m_Page = Page::Physics;
            if (ImGui::Selectable(ICON_FA_VOLUME_HIGH "  Audio", m_Page == Page::Audio))
                m_Page = Page::Audio;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##Page", {0, 0}))
        {
            if (m_Page == Page::Audio)
                DrawAudio();
            else
                DrawPhysics();
        }
        ImGui::EndChild();
        // Written once the edit ends, so a drag saves once.
        if (m_SavePending && m_Physics.IsDirty() && !ImGui::IsAnyItemActive())
        {
            m_SavePending = false; // A failed write is retried with the next edit, not every frame.
            if (auto error = m_Physics.Save())
                m_Status = "Not saved: " + error->Message;
            else
                m_Status.clear();
        }
        if (m_AudioSavePending && m_Audio.IsDirty() && !ImGui::IsAnyItemActive())
        {
            m_AudioSavePending = false;
            if (auto error = m_Audio.Save())
                m_AudioStatus = "Not saved: " + error->Message;
            else
                m_AudioStatus.clear();
        }
    }

    void ProjectSettingsPanel::DrawAudio()
    {
        namespace Spatial                         = AudioSpatialEditing;
        const auto& p                             = EditorTheme::Colors();
        auto        values                        = m_Audio.Get();
        bool        changed                       = false;
        m_AudioBoxes                              = {};
        ComponentEditors::LastAudioSpatialBoxes() = {};
        const auto last = [] { return ImRect {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; };
        if (!m_AudioStatus.empty())
            ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_AudioStatus.c_str());
        ImGui::TextDisabled("Saved in %s. The editor's audio engine takes changes at once; sources and events "
                            "override them per sound.",
                            m_Audio.GetFile().filename().u8string().c_str());
        if (PropertyGrid::BeginGroup("Spatializer"))
        {
            const bool modified = values.DefaultSpatializer != AudioSpatializer::Panning;
            if (PropertyGrid::Row("Default spatializer",
                                  "What sources and events set to Inherit play with: Panning (speaker gains) or HRTF "
                                  "(binaural, for headphones; stereo output only)",
                                  modified))
            {
                ImGui::PushID("DefaultSpatializer");
                const char* names[]      = {"Panning", "HRTF"};
                const int   index        = int(values.DefaultSpatializer);
                const bool  open         = ImGui::BeginCombo("##value", names[index]);
                m_AudioBoxes.Spatializer = last();
                if (open)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        if (ImGui::Selectable(names[i], i == index) && i != index)
                            values.DefaultSpatializer = AudioSpatializer(i), changed = true;
                        m_AudioBoxes.SpatializerItems.emplace_back(i, last());
                    }
                    ImGui::EndCombo();
                }
                if (PropertyGrid::RowReset(modified))
                    values.DefaultSpatializer = AudioSpatializer::Panning, changed = true;
                ImGui::PopID();
            }
            const bool hrtfModified = values.HRTF.IsValid();
            if (PropertyGrid::Row("HRTF",
                                  "The head-related transfer function HRTF voices render with: Steam Audio's built-in "
                                  "one, or an imported .sofa file",
                                  hrtfModified))
            {
                ImGui::PushID("HRTF");
                const EditorAssetChoice* current = nullptr;
                for (const auto& choice : m_HRTFs)
                    if (choice.ID == values.HRTF)
                        current = &choice;
                const std::string preview = current ? std::filesystem::u8path(current->Name).stem().u8string() :
                                            values.HRTF.IsValid() ?
                                                      std::string(ICON_FA_TRIANGLE_EXCLAMATION " (missing)") :
                                                      std::string("Built-in");
                const bool        open    = ImGui::BeginCombo("##value", preview.c_str());
                m_AudioBoxes.HRTF         = last();
                if (open)
                {
                    if (ImGui::Selectable("Built-in", !values.HRTF.IsValid()) && values.HRTF.IsValid())
                        values.HRTF = {}, changed = true;
                    m_AudioBoxes.HRTFItems.emplace_back(AssetID {}, last());
                    for (const auto& choice : m_HRTFs)
                    {
                        ImGui::PushID(choice.ID.ToString().c_str());
                        if (ImGui::Selectable(std::filesystem::u8path(choice.Name).stem().u8string().c_str(),
                                              choice.ID == values.HRTF) &&
                            choice.ID != values.HRTF)
                            values.HRTF = choice.ID, changed = true;
                        m_AudioBoxes.HRTFItems.emplace_back(choice.ID, last());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", choice.Name.c_str());
                        ImGui::PopID();
                    }
                    if (m_HRTFs.empty())
                        ImGui::TextDisabled("Import a .sofa file to add one");
                    ImGui::EndCombo();
                }
                if (PropertyGrid::RowReset(hrtfModified))
                    values.HRTF = {}, changed = true;
                ImGui::PopID();
            }
            if (m_Engine && m_Engine->IsInitialized() && !m_Engine->IsHRTFAvailable())
                m_AudioBoxes.HRTFNote = "Steam Audio is not available: HRTF voices are panned.";
            else if (!m_Audio.GetHRTFError().empty())
                m_AudioBoxes.HRTFNote =
                    "Could not load the HRTF (" + m_Audio.GetHRTFError() + "); the built-in one plays.";
            if (!m_AudioBoxes.HRTFNote.empty() && PropertyGrid::Row(""))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, p.Warning);
                ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", m_AudioBoxes.HRTFNote.c_str());
                ImGui::PopStyleColor();
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Default occlusion"))
        {
            changed |= ComponentEditors::AudioOcclusionRows(
                values.DefaultOcclusion, nullptr, AudioOcclusionSettings {}, "the built-in default");
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Occlusion budget"))
        {
            int       rays    = int(values.MaxOcclusionRaysPerFrame);
            const int raysDef = int(DefaultMaxOcclusionRaysPerFrame);
            if (PropertyGrid::Row("Max occlusion rays per frame",
                                  "Rays the main thread casts per frame at most, shared by every occluded voice "
                                  "(the most audible first); voices over the budget wait for a later frame",
                                  rays != raysDef))
            {
                ImGui::PushID("Rays");
                if (ImGui::DragInt("##value",
                                   &rays,
                                   0.25f,
                                   1,
                                   int(Spatial::MaxRaysPerFrameLimit),
                                   "%d",
                                   ImGuiSliderFlags_AlwaysClamp))
                    values.MaxOcclusionRaysPerFrame = uint32_t(rays), changed = true;
                m_AudioBoxes.Rays = last();
                if (PropertyGrid::RowReset(rays != raysDef))
                    values.MaxOcclusionRaysPerFrame = DefaultMaxOcclusionRaysPerFrame, changed = true;
                ImGui::PopID();
            }
            int       stale    = int(values.OcclusionStaleFrames);
            const int staleDef = int(DefaultOcclusionStaleFrames);
            if (PropertyGrid::Row("Occlusion stale frames",
                                  "How many frames a voice keeps its last occlusion result before it is cast again; "
                                  "higher saves rays, lower follows moving walls sooner",
                                  stale != staleDef))
            {
                ImGui::PushID("Stale");
                if (ImGui::DragInt("##value",
                                   &stale,
                                   0.1f,
                                   1,
                                   int(Spatial::MaxStaleFramesLimit),
                                   "%d frames",
                                   ImGuiSliderFlags_AlwaysClamp))
                    values.OcclusionStaleFrames = uint32_t(stale), changed = true;
                m_AudioBoxes.Stale = last();
                if (PropertyGrid::RowReset(stale != staleDef))
                    values.OcclusionStaleFrames = DefaultOcclusionStaleFrames, changed = true;
                ImGui::PopID();
            }
            PropertyGrid::EndGroup();
        }
        if (changed)
        {
            if (auto error = m_Audio.Apply(values, m_Engine, m_Assets))
                m_AudioStatus = *error;
            else
            {
                m_AudioSavePending = true;
                if (m_AudioChanged)
                    m_AudioChanged(m_Audio.Get());
            }
        }
    }

    void ProjectSettingsPanel::DrawPhysics()
    {
        static const PhysicsSettings defaults;
        auto                         settings = m_Physics.Get();
        bool                         changed  = false;
        if (!m_Status.empty())
            ImGui::TextColored(EditorTheme::Colors().Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Status.c_str());
        ImGui::TextDisabled("Saved in %s. A running scene keeps the settings it started with.",
                            m_Physics.GetFile().filename().u8string().c_str());
        if (PropertyGrid::BeginGroup("Simulation"))
        {
            changed |= PropertyGrid::Vec3("Gravity", settings.Gravity, defaults.Gravity, 0.05f);
            // The fixed step as a rate, which is how it is usually chosen.
            float       rate        = float(1.0 / settings.FixedTimeStep);
            const float rateDefault = float(1.0 / defaults.FixedTimeStep);
            if (PropertyGrid::Float("Fixed rate", rate, 0.5f, 1.0f, 1000.0f, "%.1f Hz", &rateDefault))
            {
                settings.FixedTimeStep = 1.0 / double(std::clamp(rate, 1.0f, 1000.0f));
                changed                = true;
            }
            int       substeps        = int(settings.MaxSubStepsPerFrame);
            const int substepsDefault = int(defaults.MaxSubStepsPerFrame);
            if (PropertyGrid::Int("Max steps per frame", substeps, 1, 64, &substepsDefault))
                settings.MaxSubStepsPerFrame = uint32_t(std::clamp(substeps, 1, 64)), changed = true;
            int position = int(settings.SolverPositionIterations), velocity = int(settings.SolverVelocityIterations);
            const int positionDefault = int(defaults.SolverPositionIterations),
                      velocityDefault = int(defaults.SolverVelocityIterations);
            if (PropertyGrid::Int("Position iterations", position, 1, 255, &positionDefault))
                settings.SolverPositionIterations = uint32_t(std::clamp(position, 1, 255)), changed = true;
            if (PropertyGrid::Int("Velocity iterations", velocity, 1, 255, &velocityDefault))
                settings.SolverVelocityIterations = uint32_t(std::clamp(velocity, 1, 255)), changed = true;
            changed |= PropertyGrid::Float("Bounce threshold",
                                           settings.BounceThreshold,
                                           0.01f,
                                           0.0f,
                                           100.0f,
                                           "%.2f m/s",
                                           &defaults.BounceThreshold);
            changed |= PropertyGrid::Float(
                "Sleep threshold", settings.SleepThreshold, 0.0005f, 0.0f, 10.0f, "%.4f", &defaults.SleepThreshold);
            int       workers        = int(settings.WorkerThreads);
            const int workersDefault = int(defaults.WorkerThreads);
            if (PropertyGrid::Int("Worker threads", workers, 0, 64, &workersDefault))
                settings.WorkerThreads = uint32_t(std::clamp(workers, 0, 64)), changed = true;
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Contacts and queries"))
        {
            int       contacts        = int(settings.MaxContactsPerPair);
            const int contactsDefault = int(defaults.MaxContactsPerPair);
            if (PropertyGrid::Int("Max contacts per pair", contacts, 1, 4096, &contactsDefault))
                settings.MaxContactsPerPair = uint32_t(std::clamp(contacts, 1, 4096)), changed = true;
            changed |= PropertyGrid::Checkbox(
                "Queries hit triggers", settings.QueriesHitTriggers, &defaults.QueriesHitTriggers);
            changed |= PropertyGrid::Checkbox(
                "Kinematic contact pairs", settings.EnableKinematicPairs, &defaults.EnableKinematicPairs);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Default material"))
        {
            changed |= ComponentEditors::PhysicsMaterialRows(settings.DefaultMaterial);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Debug drawing", nullptr, nullptr, false))
        {
            // Saved with the project (games draw them too); the viewport's View menu adds its own.
            const std::pair<PhysicsDebugFlags, const char*> flags[] = {
                {PhysicsDebugFlags::Colliders, "Colliders"},
                {PhysicsDebugFlags::CentersOfMass, "Centers of mass"},
                {PhysicsDebugFlags::Sleeping, "Sleeping bodies"},
                {PhysicsDebugFlags::Contacts, "Contacts"},
                {PhysicsDebugFlags::Joints, "Joints"},
            };
            for (const auto& [flag, name] : flags)
            {
                bool       on    = uint32_t(settings.DebugDraw) & uint32_t(flag);
                const bool reset = false;
                if (PropertyGrid::Checkbox(name, on, &reset))
                {
                    settings.DebugDraw = PhysicsDebugFlags(on ? uint32_t(settings.DebugDraw) | uint32_t(flag) :
                                                                uint32_t(settings.DebugDraw) & ~uint32_t(flag));
                    changed            = true;
                }
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Layers", nullptr, nullptr, false))
        {
            for (uint32_t i = 0; i < 32; ++i)
            {
                ImGui::PushID(int(i));
                const std::string label = "Layer " + std::to_string(i);
                if (PropertyGrid::Row(label.c_str(), nullptr, settings.LayerNames[i] != defaults.LayerNames[i]))
                    changed |= ImGui::InputText("##name", &settings.LayerNames[i]);
                ImGui::PopID();
            }
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Layer collision matrix"))
        {
            PropertyGrid::BeginFree();
            DrawCollisionMatrix(settings, changed);
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (changed)
        {
            if (auto error = m_Physics.Apply(settings))
                m_Status = *error;
            else
                m_SavePending = true;
        }
    }

    void ProjectSettingsPanel::DrawCollisionMatrix(PhysicsSettings& settings, bool& changed)
    {
        ImGui::Checkbox("Show all 32 layers", &m_AllLayers);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Otherwise only renamed layers (and Default) are shown");
        const auto layers = PhysicsEditing::MatrixLayers(settings, m_AllLayers);
        // Rows list the layers top-down, columns right-to-left, so each pair has one box (Unity's triangle).
        float labelWidth = 0.0f;
        for (uint32_t layer : layers)
            labelWidth = std::max(labelWidth, ImGui::CalcTextSize(settings.LayerNames[layer].c_str()).x);
        const float  cell   = ImGui::GetFrameHeight();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImU32  text   = ImGui::GetColorU32(ImGuiCol_Text);
        const float  header = labelWidth + EditorTheme::Px(6.0f);
        const float  left   = origin.x + labelWidth + EditorTheme::Px(8.0f);
        const size_t count  = layers.size();
        // Column headings: names standing on the grid.
        for (size_t c = 0; c < count; ++c)
        {
            const uint32_t layer = layers[count - 1 - c];
            VerticalText({left + c * cell + (cell - ImGui::GetTextLineHeight()) * 0.5f, origin.y + header},
                         settings.LayerNames[layer].c_str(),
                         text);
        }
        for (size_t r = 0; r < count; ++r)
        {
            const uint32_t row = layers[r];
            const float    y   = origin.y + header + EditorTheme::Px(4.0f) + r * cell;
            ImGui::SetCursorScreenPos({origin.x + labelWidth - ImGui::CalcTextSize(settings.LayerNames[row].c_str()).x,
                                       y + ImGui::GetStyle().FramePadding.y});
            ImGui::TextUnformatted(settings.LayerNames[row].c_str());
            // Row r has the columns down to its own layer.
            for (size_t c = 0; c + r < count; ++c)
            {
                const uint32_t column = layers[count - 1 - c];
                ImGui::SetCursorScreenPos({left + c * cell, y});
                ImGui::PushID(int(row * 32 + column));
                bool collide = settings.LayersCollide(row, column);
                if (ImGui::Checkbox("##pair", &collide))
                {
                    PhysicsEditing::ToggleLayerCollision(settings, row, column);
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s / %s", settings.LayerNames[row].c_str(), settings.LayerNames[column].c_str());
                ImGui::PopID();
            }
        }
        ImGui::SetCursorScreenPos({origin.x, origin.y + header + EditorTheme::Px(4.0f) + count * cell});
        ImGui::Dummy({left - origin.x + count * cell, EditorTheme::Px(4.0f)});
    }
} // namespace Kans
