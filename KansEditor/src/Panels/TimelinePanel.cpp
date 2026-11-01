#include "TimelinePanel.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <misc/cpp/imgui_stdlib.h>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Editor/EditorAssetImportService.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "Kans3D/Timeline/TimelineProduct.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "UI/SourceBuild.h"

namespace Kans
{
    namespace
    {
        constexpr float NameWidth = 220.0f, InspectorWidth = 280.0f, RulerHeight = 24.0f;

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            result = std::filesystem::weakly_canonical(path, error);
            return error ? path : result;
        }

        std::string Seconds(double t)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f s", t);
            return text;
        }

        // Inserts (or replaces) the key at `time`, keeping the keys ordered.
        void SetKey(TimelineCurve& curve, double time, glm::vec4 value)
        {
            for (auto& key : curve.Keys)
                if (std::abs(key.Time - time) < 1e-6)
                {
                    key.Value = value;
                    return;
                }
            TimelineKey key;
            key.Time  = time;
            key.Value = value;
            curve.Keys.insert(std::upper_bound(curve.Keys.begin(),
                                               curve.Keys.end(),
                                               time,
                                               [](double t, const TimelineKey& k) { return t < k.Time; }),
                              key);
        }

        void SortKeys(TimelineCurve& curve)
        {
            std::stable_sort(
                curve.Keys.begin(), curve.Keys.end(), [](const auto& a, const auto& b) { return a.Time < b.Time; });
        }

        // Key times of a transform track (its three curves keyed together or apart).
        std::vector<double> KeyTimes(const TimelineTransformTrack& track)
        {
            std::vector<double> times;
            for (const auto* curve : {&track.Position, &track.Rotation, &track.Scale})
                for (const auto& key : curve->Keys)
                    if (std::none_of(
                            times.begin(), times.end(), [&](double t) { return std::abs(t - key.Time) < 1e-6; }))
                        times.push_back(key.Time);
            std::sort(times.begin(), times.end());
            return times;
        }

        TimelineKey* KeyAt(TimelineCurve& curve, double time)
        {
            for (auto& key : curve.Keys)
                if (std::abs(key.Time - time) < 1e-6)
                    return &key;
            return nullptr;
        }

        ImU32 Color(const ImVec4& color, float alpha = 1.0f)
        {
            return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
        }
    } // namespace

    TimelinePlayerComponent* TimelinePanel::Component() const
    {
        auto* scene = GetScene();
        if (!scene || !m_Owner)
            return nullptr;
        Entity owner = scene->GetEntityByUUID(m_Owner);
        return owner && owner.HasComponent<TimelinePlayerComponent>() ? &owner.GetComponent<TimelinePlayerComponent>() :
                                                                        nullptr;
    }

    Entity TimelinePanel::BoundEntity(const std::string& binding) const
    {
        auto* c = Component();
        if (!c)
            return {};
        auto found = c->Bindings.find(binding);
        return found == c->Bindings.end() ? Entity {} : GetScene()->GetEntityByUUID(found->second);
    }

    void TimelinePanel::Open(Entity owner)
    {
        if (!owner || !owner.HasComponent<TimelinePlayerComponent>())
            return;
        if (owner.GetUUID() == m_Owner && owner.GetComponent<TimelinePlayerComponent>().Timeline == m_Asset)
            return;
        StopPreview();
        m_Owner    = owner.GetUUID();
        m_Scene    = GetScene();
        m_Asset    = owner.GetComponent<TimelinePlayerComponent>().Timeline;
        m_Loaded   = false;
        m_Selected = {};
        m_Time = m_Scroll = 0;
        Load();
    }

    void TimelinePanel::Load()
    {
        m_File.clear();
        m_Data  = {};
        m_Dirty = false;
        m_Status.clear();
        for (const auto& choice : m_Choices)
            if (choice.ID == m_Asset && choice.Type == AssetType::Timeline)
                m_File = choice.SourcePath;
        if (m_File.empty())
            return;
        try
        {
            m_Data   = DecodeTimelineNode(YAML::LoadFile(m_File.string()));
            m_Loaded = true;
        }
        catch (const std::exception& e)
        {
            m_Status = "Cannot read " + m_File.filename().u8string() + ": " + e.what();
        }
        m_PlayerDirty = true;
    }

    void TimelinePanel::Save()
    {
        if (m_File.empty())
            return;
        StopPreview();
        if (auto problem = ValidateTimeline(m_Data); std::holds_alternative<AssetError>(problem))
        {
            m_Status = "Not saved: " + std::get<AssetError>(problem).Message;
            return;
        }
        YAML::Emitter out;
        out << EncodeTimelineNode(m_Data);
        {
            std::ofstream file(m_File, std::ios::binary | std::ios::trunc);
            file << out.c_str() << '\n';
            if (!file)
            {
                m_Status = "Cannot write " + m_File.filename().u8string();
                return;
            }
        }
        m_Dirty  = false;
        m_Status = BuildSavedSource(m_Service, m_File);
    }

    void TimelinePanel::CreateAsset()
    {
        Entity owner = GetScene()->GetEntityByUUID(m_Owner);
        auto   name  = owner.GetComponent<TagComponent>().Tag;
        if (name.empty())
            name = "Timeline";
        const auto folder = KansFileSystem::GetAssetFolder();
        auto       file   = folder / std::filesystem::u8path(name + ".timeline");
        for (int i = 2; std::filesystem::exists(file); ++i)
            file = folder / std::filesystem::u8path(name + std::to_string(i) + ".timeline");
        TimelineData data;
        data.Length = 5;
        YAML::Emitter out;
        out << EncodeTimelineNode(data);
        std::ofstream(file, std::ios::binary) << out.c_str() << '\n';
        m_File        = file;
        m_PendingFile = Canonical(file);
        m_Data        = data;
        m_Loaded      = true;
        m_Dirty       = false;
        Save(); // Builds it; the asset is assigned once the build publishes it.
    }

    void TimelinePanel::Changed()
    {
        m_Dirty       = true;
        m_PlayerDirty = true;
    }

    void TimelinePanel::SetPlayhead(double time)
    {
        const double clamped = std::clamp(time, 0.0, m_Data.Length);
        if (clamped != m_Time)
            m_NeedsEvaluate = true;
        m_Time = clamped;
    }

    void TimelinePanel::SetPreview(bool preview)
    {
        if (preview == m_Previewing)
            return;
        if (!preview)
        {
            StopPreview();
            return;
        }
        m_Previewing    = true;
        m_PlayerDirty   = true;
        m_NeedsEvaluate = true;
        m_Bases.clear();
        if (auto* c = Component())
            for (const auto& [binding, id] : c->Bindings)
                if (Entity e = GetScene()->GetEntityByUUID(id))
                    m_Bases[id] = e.GetComponent<TransformComponent>();
        UpdatePreview();
    }

    void TimelinePanel::StopPreview()
    {
        m_Playing = false;
        if (m_Player)
            m_Player->RestoreBoundState();
        m_Player.reset();
        m_Bases.clear();
        m_Previewing = false;
        if (m_ViewingCamera && m_ViewThrough)
            m_ViewThrough(false);
        m_ViewingCamera = false;
    }

    void TimelinePanel::StopPreviewIfHidden()
    {
        if (m_Previewing && m_LastDrawnFrame < ImGui::GetFrameCount() - 1)
            StopPreview();
    }

    void TimelinePanel::UpdatePreview()
    {
        if (!m_Previewing || !Component() || !m_Loaded)
            return;
        if (!m_Player || m_PlayerDirty)
        {
            if (m_Player)
                m_Player->RestoreBoundState();
            m_Player        = std::make_unique<TimelinePlayer>(*GetScene(), m_Owner, CreateRef<TimelineAsset>(m_Data));
            m_PlayerDirty   = false;
            m_NeedsEvaluate = true;
        }
        if (m_NeedsEvaluate)
        {
            m_NeedsEvaluate = false;
            if (!m_Player->Evaluate(m_Time))
                m_Status = m_Player->Error() ? m_Player->Error()->Message : "Preview failed.";
            else if (m_Status.rfind("Preview", 0) == 0)
                m_Status.clear();
        }
    }

    bool TimelinePanel::KeyTransform(Entity entity)
    {
        auto* c = Component();
        if (!c || !entity)
            return false;
        std::string binding;
        for (const auto& [id, uuid] : c->Bindings)
            if (uuid == entity.GetUUID())
                binding = id;
        if (binding.empty())
            return false;
        auto track = std::find_if(
            m_Data.Transforms.begin(), m_Data.Transforms.end(), [&](const auto& t) { return t.Binding == binding; });
        if (track == m_Data.Transforms.end())
        {
            // Keyed from the editor: absolute values, so a key shows exactly what was posed.
            m_Data.Transforms.push_back({binding, false});
            track = std::prev(m_Data.Transforms.end());
        }
        const auto& now      = entity.GetComponent<TransformComponent>();
        glm::vec3   position = now.Position, rotation = now.Rotation, scale = now.Scale;
        if (track->Relative)
        {
            // The offset from where preview started, in that transform's space.
            const auto base  = m_Bases.count(entity.GetUUID()) ? m_Bases.at(entity.GetUUID()) : now;
            const auto delta = JointTransform::FromMatrix(glm::inverse(base.GetTransform()) * now.GetTransform());
            position         = delta.Translation;
            rotation         = glm::eulerAngles(delta.Rotation);
            scale            = delta.Scale;
        }
        const double t = Snap(m_Time);
        SetKey(track->Position, t, glm::vec4(position, 0));
        SetKey(track->Rotation, t, glm::vec4(rotation, 0));
        SetKey(track->Scale, t, glm::vec4(scale, 0));
        m_Selected = {RowKind::Transform, size_t(track - m_Data.Transforms.begin()), 0, t};
        Changed();
        return true;
    }

    std::vector<TimelinePanel::Row> TimelinePanel::Rows() const
    {
        std::vector<Row> rows;
        for (size_t b = 0; b < m_Data.Bindings.size(); ++b)
        {
            const auto& id = m_Data.Bindings[b].ID;
            rows.push_back({RowKind::Binding, b, id});
            for (size_t i = 0; i < m_Data.Transforms.size(); ++i)
                if (m_Data.Transforms[i].Binding == id)
                    rows.push_back({RowKind::Transform, i, id});
            for (size_t i = 0; i < m_Data.Animations.size(); ++i)
                if (m_Data.Animations[i].Binding == id)
                    rows.push_back({RowKind::Animation, i, id});
            for (size_t i = 0; i < m_Data.Activations.size(); ++i)
                if (m_Data.Activations[i].Binding == id)
                    rows.push_back({RowKind::Activation, i, id});
            for (size_t i = 0; i < m_Data.Particles.size(); ++i)
                if (m_Data.Particles[i].Binding == id)
                    rows.push_back({RowKind::Particle, i, id});
            for (size_t i = 0; i < m_Data.Properties.size(); ++i)
                if (m_Data.Properties[i].Binding == id)
                    rows.push_back({RowKind::Property, i, id});
        }
        rows.push_back({RowKind::Events, 0, {}});
        rows.push_back({RowKind::Cuts, 0, {}});
        return rows;
    }

    double TimelinePanel::Snap(double time) const
    {
        const double fps = m_Data.FrameRate > 0 ? m_Data.FrameRate : 30;
        return m_Snap ? std::round(time * fps) / fps : time;
    }
    float  TimelinePanel::ToX(double time) const { return m_CanvasX + float((time - m_Scroll) * m_Zoom); }
    double TimelinePanel::ToTime(float x) const { return m_Scroll + (x - m_CanvasX) / m_Zoom; }

    void TimelinePanel::Draw()
    {
        m_LastDrawnFrame = ImGui::GetFrameCount();
        Scene* scene     = GetScene();
        if (scene != m_Scene)
        {
            // The project or scene changed: the old owner and its preview are gone.
            m_Player.reset();
            m_Previewing = false;
            m_Owner      = 0;
            m_Scene      = scene;
        }
        // Follows a selected Timeline Player; selecting the bound actors keeps the timeline open.
        Entity selected = m_Hierarchy.GetSelected();
        if (selected && selected.HasComponent<TimelinePlayerComponent>() && selected.GetUUID() != m_Owner)
            Open(selected);
        auto* c = Component();
        if (!c)
        {
            m_Owner = 0;
            ImGui::TextDisabled("Select an entity with a Timeline Player to edit its timeline.");
            return;
        }
        // A new timeline file is assigned once its build has published it.
        if (!m_PendingFile.empty())
            for (const auto& choice : m_Choices)
                if (choice.Type == AssetType::Timeline && Canonical(choice.SourcePath) == m_PendingFile)
                {
                    GetScene()->GetEntityByUUID(m_Owner).PatchComponent<TimelinePlayerComponent>(
                        [&](auto& player) { player.Timeline = choice.ID; });
                    m_Asset = choice.ID;
                    m_PendingFile.clear();
                }
        if (c->Timeline != m_Asset && m_PendingFile.empty())
        {
            StopPreview();
            m_Asset = c->Timeline;
            Load();
        }
        if (!m_Loaded && m_File.empty() && !m_Asset.IsValid() && m_PendingFile.empty())
        {
            ImGui::TextDisabled("This Timeline Player has no timeline yet.");
            if (ImGui::Button(ICON_FA_PLUS "  Create timeline"))
                CreateAsset();
            return;
        }
        if (!m_Loaded)
        {
            if (m_File.empty())
                Load(); // The asset list may have arrived since.
            ImGui::TextDisabled("%s", m_Status.empty() ? "Loading the timeline..." : m_Status.c_str());
            return;
        }

        if (m_Playing)
        {
            double next = m_Time + ImGui::GetIO().DeltaTime * std::max(0.0, c->Speed);
            if (next >= m_Data.Length)
                next = c->Loop ? std::fmod(next, std::max(m_Data.Length, 1e-6)) : m_Data.Length, m_Playing = c->Loop;
            SetPlayhead(next);
        }
        DrawToolbar();
        UpdatePreview();

        const float rowHeight = ImGui::GetFrameHeight() + EditorTheme::Px(4.0f);
        const auto  rows      = Rows();
        const float inspector = EditorTheme::Px(InspectorWidth);
        if (ImGui::BeginChild("##tracks", {ImGui::GetContentRegionAvail().x - inspector, 0}, ImGuiChildFlags_Borders))
        {
            const float names = EditorTheme::Px(NameWidth);
            ImGui::BeginChild("##names", {names, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            DrawNames(rows, rowHeight);
            ImGui::EndChild();
            ImGui::SameLine(0, 0);
            ImGui::BeginChild("##canvas",
                              {0, 0},
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawCanvas(rows, rowHeight);
            ImGui::EndChild();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##inspector", {0, 0}, ImGuiChildFlags_Borders))
            DrawInspector();
        ImGui::EndChild();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                DeleteSelection();
            if (ImGui::IsKeyPressed(ImGuiKey_Space))
                m_Playing = !m_Playing, SetPreview(true);
        }
    }

    void TimelinePanel::DrawToolbar()
    {
        auto* c = Component();
        ImGui::BeginDisabled(!m_Dirty);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", m_File.filename().u8string().c_str(), m_Dirty ? " *" : "");
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_BACKWARD_FAST))
            SetPlayhead(0);
        ImGui::SameLine();
        if (ImGui::Button(m_Playing ? ICON_FA_PAUSE : ICON_FA_PLAY))
        {
            m_Playing = !m_Playing;
            if (m_Playing && m_Time >= m_Data.Length)
                SetPlayhead(0);
            SetPreview(true); // Playing shows the timeline in the scene.
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FORWARD_FAST))
            SetPlayhead(m_Data.Length);
        ImGui::SameLine();
        const double fps = m_Data.FrameRate > 0 ? m_Data.FrameRate : 30;
        ImGui::Text("%s  " ICON_FA_FILM " %d", Seconds(m_Time).c_str(), int(std::lround(m_Time * fps)));
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        bool preview = m_Previewing;
        if (ImGui::Checkbox("Preview", &preview))
            SetPreview(preview);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Shows the timeline in the scene at the playhead; the scene is restored when preview ends.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Previewing);
        bool camera = m_ViewingCamera;
        if (ImGui::Checkbox(ICON_FA_VIDEO " Cuts", &camera) && m_ViewThrough)
            m_ViewThrough(m_ViewingCamera = camera);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("View the preview through the camera the cuts make primary");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_MAGNET, &m_Snap);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snap to frames");
        ImGui::SameLine();
        float length = float(m_Data.Length), rate = float(m_Data.FrameRate);
        ImGui::SetNextItemWidth(EditorTheme::Px(70.0f));
        if (ImGui::DragFloat("##length", &length, 0.05f, 0.1f, 3600.0f, "%.2f s"))
            m_Data.Length = std::max(0.1f, length), Changed();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Length");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(EditorTheme::Px(60.0f));
        if (ImGui::DragFloat("##fps", &rate, 0.1f, 1.0f, 240.0f, "%.0f fps"))
            m_Data.FrameRate = std::max(1.0f, rate), Changed();
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
        (void)c;
    }

    void TimelinePanel::DrawNames(const std::vector<Row>& rows, float rowHeight)
    {
        const auto& p    = EditorTheme::Colors();
        auto*       draw = ImGui::GetWindowDrawList();
        // Rows sit at the same fixed pitch as the canvas, so names and tracks line up.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  w      = ImGui::GetContentRegionAvail().x;
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const auto& row = rows[r];
            ImGui::PushID(int(r));
            const ImVec2 min {origin.x, origin.y + EditorTheme::Px(RulerHeight) + r * rowHeight};
            const bool   header =
                row.Kind == RowKind::Binding || row.Kind == RowKind::Events || row.Kind == RowKind::Cuts;
            if (header)
                draw->AddRectFilled(min, {min.x + w, min.y + rowHeight - 1}, Color(p.Hover), EditorTheme::Px(3.0f));
            std::string label;
            switch (row.Kind)
            {
                case RowKind::Binding: {
                    const auto& b      = m_Data.Bindings[row.Track];
                    Entity      entity = BoundEntity(b.ID);
                    label = std::string(b.Kind == TimelineBindingKind::Spawnable ? ICON_FA_CUBES : ICON_FA_CUBE) +
                            "  " + b.Name +
                            (b.Kind == TimelineBindingKind::Spawnable ? "" :
                             entity ? "  (" + entity.GetComponent<TagComponent>().Tag + ")" :
                                      "  (unbound)");
                    break;
                }
                case RowKind::Transform:
                    label = "    Transform";
                    break;
                case RowKind::Animation:
                    label = "    Animation";
                    break;
                case RowKind::Activation:
                    label = "    Active";
                    break;
                case RowKind::Particle:
                    label = "    Particles";
                    break;
                case RowKind::Property:
                    label = "    " + m_Data.Properties[row.Track].Path;
                    break;
                case RowKind::Events:
                    label = ICON_FA_FLAG "  Events";
                    break;
                case RowKind::Cuts:
                    label = ICON_FA_VIDEO "  Camera cuts";
                    break;
            }
            ImGui::SetCursorScreenPos(
                {min.x + EditorTheme::Px(6.0f), min.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f});
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            if (row.Kind == RowKind::Transform)
            {
                // Keys the bound entity's current transform at the playhead.
                ImGui::SameLine(w - ImGui::GetFrameHeight());
                if (ImGui::SmallButton(ICON_FA_KEY))
                    KeyTransform(BoundEntity(row.Binding));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Key the entity's transform at the playhead");
            }
            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton("##row", {w, rowHeight}, ImGuiButtonFlags_MouseButtonRight);
            if (ImGui::BeginPopupContextItem("##rowmenu"))
            {
                if (row.Kind == RowKind::Binding)
                {
                    DrawAddTrackMenu(row.Binding);
                    ImGui::Separator();
                    Entity picked = m_Hierarchy.GetSelected();
                    if (ImGui::MenuItem("Bind the selected entity", nullptr, false, bool(picked)))
                        GetScene()->GetEntityByUUID(m_Owner).PatchComponent<TimelinePlayerComponent>(
                            [&](auto& c) { c.Bindings[row.Binding] = picked.GetUUID(); }),
                            m_PlayerDirty = true;
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove binding and its tracks"))
                    {
                        const auto id    = row.Binding;
                        const auto erase = [&](auto& tracks) {
                            tracks.erase(std::remove_if(tracks.begin(),
                                                        tracks.end(),
                                                        [&](const auto& t) { return t.Binding == id; }),
                                         tracks.end());
                        };
                        erase(m_Data.Transforms), erase(m_Data.Animations), erase(m_Data.Activations),
                            erase(m_Data.Properties), erase(m_Data.Particles);
                        m_Data.Bindings.erase(m_Data.Bindings.begin() + long(row.Track));
                        m_Selected = {};
                        Changed();
                    }
                }
                else if (row.Kind != RowKind::Events && row.Kind != RowKind::Cuts &&
                         ImGui::MenuItem(ICON_FA_TRASH "  Remove track"))
                {
                    switch (row.Kind)
                    {
                        case RowKind::Transform:
                            m_Data.Transforms.erase(m_Data.Transforms.begin() + long(row.Track));
                            break;
                        case RowKind::Animation:
                            m_Data.Animations.erase(m_Data.Animations.begin() + long(row.Track));
                            break;
                        case RowKind::Activation:
                            m_Data.Activations.erase(m_Data.Activations.begin() + long(row.Track));
                            break;
                        case RowKind::Particle:
                            m_Data.Particles.erase(m_Data.Particles.begin() + long(row.Track));
                            break;
                        case RowKind::Property:
                            m_Data.Properties.erase(m_Data.Properties.begin() + long(row.Track));
                            break;
                        default:
                            break;
                    }
                    m_Selected = {};
                    Changed();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        // Dropping an entity from the Hierarchy binds it.
        ImGui::SetCursorScreenPos({origin.x, origin.y + EditorTheme::Px(RulerHeight) + rows.size() * rowHeight});
        const ImVec2 rest = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##drop", {std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())});
        if (rows.size() <= 2)
            draw->AddText(
                {ImGui::GetItemRectMin().x + EditorTheme::Px(6.0f), ImGui::GetItemRectMin().y + EditorTheme::Px(4.0f)},
                Color(p.TextDim),
                "Drop entities here to animate them");
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("entity_payload");
                payload && payload->DataSize == sizeof(uint64_t))
                if (Entity e = GetScene()->GetEntityByUUID(UUID(*static_cast<const uint64_t*>(payload->Data))))
                {
                    const auto  name = e.GetComponent<TagComponent>().Tag;
                    std::string id   = name.empty() ? "Actor" : name;
                    for (int i = 2; std::any_of(
                             m_Data.Bindings.begin(), m_Data.Bindings.end(), [&](const auto& b) { return b.ID == id; });
                         ++i)
                        id = name + std::to_string(i);
                    TimelineBinding binding;
                    binding.ID   = id;
                    binding.Name = name.empty() ? id : name;
                    m_Data.Bindings.push_back(binding);
                    GetScene()->GetEntityByUUID(m_Owner).PatchComponent<TimelinePlayerComponent>(
                        [&](auto& c) { c.Bindings[id] = e.GetUUID(); });
                    Changed();
                }
            ImGui::EndDragDropTarget();
        }
    }

    void TimelinePanel::DrawAddTrackMenu(const std::string& binding)
    {
        if (ImGui::MenuItem(ICON_FA_ARROWS_MOVE "  Transform track"))
            m_Data.Transforms.push_back({binding, false}), Changed();
        if (ImGui::MenuItem(ICON_FA_PERSON_RUNNING "  Animation track"))
            m_Data.Animations.push_back({binding}), Changed();
        if (ImGui::MenuItem(ICON_FA_EYE "  Activation track"))
            m_Data.Activations.push_back({binding, {{0, m_Data.Length}}}), Changed();
        if (ImGui::BeginMenu(ICON_FA_SLIDERS "  Property track"))
        {
            ImGui::SetNextItemWidth(EditorTheme::Px(200.0f));
            ImGui::InputTextWithHint(
                "##path", "Light.Intensity, Material.0.Glow", m_PropertyPath, sizeof(m_PropertyPath));
            if (ImGui::Button("Add") && m_PropertyPath[0])
            {
                TimelinePropertyTrack track;
                track.Binding = binding;
                track.Path    = m_PropertyPath;
                m_Data.Properties.push_back(track);
                Changed();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
    }

    void TimelinePanel::DrawRuler(ImVec2 origin, float width)
    {
        const auto& p      = EditorTheme::Colors();
        auto*       draw   = ImGui::GetWindowDrawList();
        const float height = EditorTheme::Px(RulerHeight);
        draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, Color(p.Raised));
        // A tick every frame when zoomed in, otherwise every 1, 5, 10... frames, labelled in seconds.
        const double fps  = m_Data.FrameRate > 0 ? m_Data.FrameRate : 30;
        double       step = 1.0 / fps;
        const double steps[] {1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        for (double s : steps)
        {
            step = s / fps;
            if (step * m_Zoom >= EditorTheme::Px(8.0f))
                break;
        }
        // Labels at the first whole number of frames (1, 2, 5, 10...) at least 64 px apart.
        double label = step;
        for (double s : steps)
        {
            label = s / fps;
            if (label * m_Zoom >= EditorTheme::Px(64.0f))
                break;
        }
        label = std::max(label, step);
        for (double t = std::floor(m_Scroll / step) * step; ToX(t) < origin.x + width; t += step)
        {
            if (t < 0)
                continue;
            const float  x      = ToX(t);
            const double frames = std::round(t * fps), every = std::round(label * fps);
            const bool   major = std::fmod(frames, every) == 0;
            draw->AddLine({x, origin.y + height * (major ? 0.35f : 0.7f)},
                          {x, origin.y + height},
                          Color(p.TextDim, major ? 0.8f : 0.4f));
            if (major)
                draw->AddText({x + 3, origin.y + 1}, Color(p.TextDim), Seconds(t).c_str());
        }
        // The end of the timeline.
        const float end = ToX(m_Data.Length);
        draw->AddLine({end, origin.y}, {end, origin.y + height}, Color(p.Danger, 0.7f), 2.0f);
    }

    void TimelinePanel::DrawCanvas(const std::vector<Row>& rows, float rowHeight)
    {
        const auto&    p      = EditorTheme::Colors();
        const ImGuiIO& io     = ImGui::GetIO();
        auto*          draw   = ImGui::GetWindowDrawList();
        const ImVec2   origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = std::max(ImGui::GetContentRegionAvail().y, 1.0f);
        m_CanvasX = origin.x + EditorTheme::Px(8.0f);
        // Wheel pans, Ctrl+wheel zooms around the mouse.
        if (ImGui::IsWindowHovered())
        {
            if (io.KeyCtrl && io.MouseWheel != 0)
            {
                const double at = ToTime(io.MousePos.x);
                m_Zoom          = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), 10.0f, 2000.0f);
                m_Scroll        = at - (io.MousePos.x - m_CanvasX) / m_Zoom;
            }
            else if (io.MouseWheel != 0 || io.MouseWheelH != 0)
                m_Scroll -= (io.MouseWheelH + io.MouseWheel) * 40.0 / m_Zoom;
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                m_Scroll -= io.MouseDelta.x / m_Zoom;
            m_Scroll = std::max(-0.5, m_Scroll);
        }
        DrawRuler(origin, width);
        const float rulerHeight = EditorTheme::Px(RulerHeight);
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##ruler", {width, rulerHeight});
        if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_Drag = DragKind::Playhead;
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const ImVec2 min {origin.x, origin.y + rulerHeight + r * rowHeight},
                max {origin.x + width, min.y + rowHeight};
            if (r % 2)
                draw->AddRectFilled(min, max, Color(p.Raised, 0.35f));
            ImGui::PushID(int(r));
            DrawRow(rows[r], min, max);
            ImGui::PopID();
        }
        // Dragging, with snapping to frames.
        if (m_Drag != DragKind::None)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                // Keys stay ordered once a drag ends.
                for (auto& t : m_Data.Transforms)
                    SortKeys(t.Position), SortKeys(t.Rotation), SortKeys(t.Scale);
                for (auto& t : m_Data.Properties)
                    SortKeys(t.Curve);
                std::stable_sort(m_Data.Events.begin(), m_Data.Events.end(), [](const auto& a, const auto& b) {
                    return a.Time < b.Time;
                });
                m_Drag = DragKind::None;
            }
            else if (m_Drag == DragKind::Playhead)
                SetPlayhead(Snap(ToTime(io.MousePos.x)));
            else if (io.MouseDelta.x != 0)
            {
                const double t    = std::clamp(Snap(ToTime(io.MousePos.x) - m_DragOffset), 0.0, m_Data.Length);
                auto&        sel  = m_DragTarget;
                const auto   move = [&](double& start, double& end) {
                    const double length = end - start;
                    start               = std::clamp(t, 0.0, m_Data.Length - length);
                    end                 = start + length;
                };
                switch (sel.Kind)
                {
                    case RowKind::Transform: {
                        auto& track = m_Data.Transforms[sel.Track];
                        for (auto* curve : {&track.Position, &track.Rotation, &track.Scale})
                            if (auto* key = KeyAt(*curve, sel.Time))
                                key->Time = t;
                        sel.Time = m_Selected.Time = t;
                        break;
                    }
                    case RowKind::Property:
                        m_Data.Properties[sel.Track].Curve.Keys[sel.Item].Time = t;
                        break;
                    case RowKind::Animation: {
                        auto& clip = m_Data.Animations[sel.Track].Clips[sel.Item];
                        if (m_Drag == DragKind::Move)
                            move(clip.Start, clip.End);
                        else if (m_Drag == DragKind::Start && t < clip.End)
                            clip.ClipIn = std::max(0.0, clip.ClipIn + (t - clip.Start) * clip.Speed), clip.Start = t;
                        else if (m_Drag == DragKind::End && t > clip.Start)
                            clip.End = t;
                        else if (m_Drag == DragKind::BlendIn)
                            clip.BlendIn = std::clamp(t - clip.Start, 0.0, clip.End - clip.Start);
                        else if (m_Drag == DragKind::BlendOut)
                            clip.BlendOut = std::clamp(clip.End - t, 0.0, clip.End - clip.Start);
                        break;
                    }
                    case RowKind::Activation: {
                        auto& range = m_Data.Activations[sel.Track].Ranges[sel.Item];
                        if (m_Drag == DragKind::Move)
                            move(range.Start, range.End);
                        else if (m_Drag == DragKind::Start && t < range.End)
                            range.Start = t;
                        else if (m_Drag == DragKind::End && t > range.Start)
                            range.End = t;
                        break;
                    }
                    case RowKind::Events:
                        m_Data.Events[sel.Item].Time = t;
                        break;
                    case RowKind::Cuts: {
                        auto& cut = m_Data.CameraCuts[sel.Item];
                        if (m_Drag == DragKind::Move)
                            move(cut.Start, cut.End);
                        else if (m_Drag == DragKind::Start && t < cut.End)
                            cut.Start = t;
                        else if (m_Drag == DragKind::End && t > cut.Start)
                            cut.End = t;
                        break;
                    }
                    default:
                        break;
                }
                Changed();
            }
        }
        // The playhead over everything.
        const float x = ToX(m_Time);
        draw->AddLine({x, origin.y}, {x, origin.y + height}, Color(p.Accent), 2.0f);
        draw->AddTriangleFilled({x - 6, origin.y}, {x + 6, origin.y}, {x, origin.y + 8}, Color(p.Accent));
    }

    void TimelinePanel::DrawRow(const Row& row, ImVec2 min, ImVec2 max)
    {
        const auto& p    = EditorTheme::Colors();
        auto*       draw = ImGui::GetWindowDrawList();
        const float mid = (min.y + max.y) * 0.5f, half = (max.y - min.y) * 0.3f;
        const bool  hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
        const auto& io      = ImGui::GetIO();
        draw->PushClipRect(min, max, true);
        // Starts dragging `target` when the item under the mouse was pressed.
        const auto press = [&](Selection target, DragKind kind, double anchor) {
            m_Selected   = target;
            m_DragTarget = target;
            m_Drag       = kind;
            m_DragOffset = ToTime(io.MousePos.x) - anchor;
        };
        const auto diamond = [&](double time, bool selected) {
            const float x = ToX(time), r = half * 0.9f;
            draw->AddQuadFilled(
                {x, mid - r}, {x + r, mid}, {x, mid + r}, {x - r, mid}, Color(selected ? p.Accent : p.Text, 0.9f));
            return std::abs(io.MousePos.x - x) <= r + 2 && std::abs(io.MousePos.y - mid) <= r + 2;
        };
        const auto bar = [&](double start, double end, const ImVec4& color, bool selected, const char* text) {
            const ImVec2 a {ToX(start), mid - half * 1.4f}, b {ToX(end), mid + half * 1.4f};
            draw->AddRectFilled(a, b, Color(color, selected ? 0.75f : 0.45f), EditorTheme::Px(3.0f));
            if (selected)
                draw->AddRect(a, b, Color(p.Text, 0.8f), EditorTheme::Px(3.0f));
            if (text)
            {
                draw->PushClipRect(a, b, true);
                draw->AddText({a.x + 4, mid - ImGui::GetFontSize() * 0.5f}, Color(p.Text), text);
                draw->PopClipRect();
            }
            // Which part the mouse is over: an edge (5 px) or the body.
            if (!hovered || io.MousePos.x < a.x - 3 || io.MousePos.x > b.x + 3)
                return DragKind::None;
            if (io.MousePos.x < a.x + 5)
                return DragKind::Start;
            if (io.MousePos.x > b.x - 5)
                return DragKind::End;
            return DragKind::Move;
        };
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_Drag == DragKind::None;
        switch (row.Kind)
        {
            case RowKind::Transform: {
                const auto& track = m_Data.Transforms[row.Track];
                for (double t : KeyTimes(track))
                {
                    const bool selected = m_Selected.Kind == RowKind::Transform && m_Selected.Track == row.Track &&
                                          std::abs(m_Selected.Time - t) < 1e-6;
                    if (diamond(t, selected) && clicked)
                        press({RowKind::Transform, row.Track, 0, t}, DragKind::Move, t);
                }
                break;
            }
            case RowKind::Property: {
                const auto& keys = m_Data.Properties[row.Track].Curve.Keys;
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    const bool selected =
                        m_Selected.Kind == RowKind::Property && m_Selected.Track == row.Track && m_Selected.Item == i;
                    if (diamond(keys[i].Time, selected) && clicked)
                        press({RowKind::Property, row.Track, i, keys[i].Time}, DragKind::Move, keys[i].Time);
                }
                break;
            }
            case RowKind::Animation: {
                const auto& clips = m_Data.Animations[row.Track].Clips;
                for (size_t i = 0; i < clips.size(); ++i)
                {
                    const auto& clip = clips[i];
                    const bool  selected =
                        m_Selected.Kind == RowKind::Animation && m_Selected.Track == row.Track && m_Selected.Item == i;
                    std::string name = "Clip";
                    for (const auto& choice : m_Choices)
                        if (choice.ID == clip.Clip)
                            name = choice.Name;
                    auto part = bar(clip.Start, clip.End, p.TypeModel, selected, name.c_str());
                    // Blend handles: triangles at the start and end, dragged inwards.
                    const float in = ToX(clip.Start + clip.BlendIn), out = ToX(clip.End - clip.BlendOut);
                    draw->AddTriangleFilled({ToX(clip.Start), mid + half * 1.4f},
                                            {in, mid - half * 1.4f},
                                            {in, mid + half * 1.4f},
                                            Color(p.Text, 0.25f));
                    draw->AddTriangleFilled({out, mid - half * 1.4f},
                                            {ToX(clip.End), mid + half * 1.4f},
                                            {out, mid + half * 1.4f},
                                            Color(p.Text, 0.25f));
                    if (hovered && io.KeyAlt && part != DragKind::None)
                        part = io.MousePos.x < (ToX(clip.Start) + ToX(clip.End)) * 0.5f ? DragKind::BlendIn :
                                                                                          DragKind::BlendOut;
                    if (part != DragKind::None && clicked)
                        press({RowKind::Animation, row.Track, i, clip.Start},
                              part,
                              part == DragKind::End ? clip.End : clip.Start);
                    if (part == DragKind::Start || part == DragKind::End)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                break;
            }
            case RowKind::Activation: {
                const auto& ranges = m_Data.Activations[row.Track].Ranges;
                for (size_t i = 0; i < ranges.size(); ++i)
                {
                    const bool selected =
                        m_Selected.Kind == RowKind::Activation && m_Selected.Track == row.Track && m_Selected.Item == i;
                    auto part = bar(ranges[i].Start, ranges[i].End, p.Success, selected, "Active");
                    if (part != DragKind::None && clicked)
                        press({RowKind::Activation, row.Track, i, ranges[i].Start},
                              part,
                              part == DragKind::End ? ranges[i].End : ranges[i].Start);
                }
                break;
            }
            case RowKind::Events:
                for (size_t i = 0; i < m_Data.Events.size(); ++i)
                {
                    const auto& e        = m_Data.Events[i];
                    const bool  selected = m_Selected.Kind == RowKind::Events && m_Selected.Item == i;
                    const float x        = ToX(e.Time);
                    draw->AddLine({x, min.y + 2}, {x, max.y - 2}, Color(p.Warning), 2.0f);
                    draw->AddText({x + 4, mid - ImGui::GetFontSize() * 0.5f},
                                  Color(selected ? p.Text : p.Warning),
                                  e.Name.c_str());
                    if (hovered && std::abs(io.MousePos.x - x) < 6 && clicked)
                        press({RowKind::Events, 0, i, e.Time}, DragKind::Move, e.Time);
                }
                break;
            case RowKind::Cuts:
                for (size_t i = 0; i < m_Data.CameraCuts.size(); ++i)
                {
                    const auto& cut      = m_Data.CameraCuts[i];
                    const bool  selected = m_Selected.Kind == RowKind::Cuts && m_Selected.Item == i;
                    std::string name     = ICON_FA_VIDEO " ";
                    for (const auto& b : m_Data.Bindings)
                        if (b.ID == cut.Binding)
                            name += b.Name;
                    auto part = bar(cut.Start, cut.End, p.TypeScene, selected, name.c_str());
                    if (part != DragKind::None && clicked)
                        press({RowKind::Cuts, 0, i, cut.Start}, part, part == DragKind::End ? cut.End : cut.Start);
                }
                break;
            case RowKind::Binding:
                if (const auto& b = m_Data.Bindings[row.Track]; b.Kind == TimelineBindingKind::Spawnable)
                {
                    const auto range = TimelineBindingRange(m_Data, b);
                    bar(range.Start, range.End, p.TextDim, false, "Spawned");
                }
                break;
        }
        draw->PopClipRect();
        // A click on empty space moves the playhead; a right-click adds at the playhead.
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##rowcanvas", {max.x - min.x, max.y - min.y}, ImGuiButtonFlags_MouseButtonRight);
        if (clicked && m_Drag == DragKind::None)
        {
            m_Selected = {};
            m_Drag     = DragKind::Playhead;
        }
        if (ImGui::BeginPopupContextItem("##add"))
        {
            const double t = Snap(m_Time);
            switch (row.Kind)
            {
                case RowKind::Binding:
                    DrawAddTrackMenu(row.Binding);
                    break;
                case RowKind::Transform:
                    if (ImGui::MenuItem(ICON_FA_KEY "  Key the transform here"))
                        KeyTransform(BoundEntity(row.Binding));
                    break;
                case RowKind::Property:
                    if (ImGui::MenuItem(ICON_FA_KEY "  Add key here"))
                    {
                        auto&     track = m_Data.Properties[row.Track];
                        glm::vec4 value = track.Curve.Sample(t);
                        SetKey(track.Curve, t, value);
                        Changed();
                    }
                    break;
                case RowKind::Animation:
                    if (ImGui::BeginMenu(ICON_FA_PLUS "  Add clip here"))
                    {
                        for (const auto& choice : m_Choices)
                            if (choice.Type == AssetType::AnimationClip && ImGui::MenuItem(choice.Name.c_str()))
                            {
                                double length = 1;
                                if (m_Assets)
                                    if (auto status = m_Assets->Request<AnimationClipAsset>(choice.ID);
                                        status.IsReady())
                                        length = std::max(status.Asset->GetData().Duration, 1e-3);
                                TimelineAnimationSection clip;
                                clip.Clip     = choice.ID;
                                clip.Start    = t;
                                clip.End      = std::min(t + length, std::max(m_Data.Length, t + length));
                                m_Data.Length = std::max(m_Data.Length, clip.End);
                                m_Data.Animations[row.Track].Clips.push_back(clip);
                                Changed();
                            }
                        ImGui::EndMenu();
                    }
                    break;
                case RowKind::Activation:
                    if (ImGui::MenuItem(ICON_FA_PLUS "  Add active range here"))
                        m_Data.Activations[row.Track].Ranges.push_back({t, std::min(m_Data.Length, t + 1)}), Changed();
                    break;
                case RowKind::Events:
                    if (ImGui::MenuItem(ICON_FA_PLUS "  Add event here"))
                    {
                        TimelineEvent e;
                        e.Time = t;
                        e.Name = "Event" + std::to_string(m_Data.Events.size());
                        m_Data.Events.push_back(e);
                        Changed();
                    }
                    break;
                case RowKind::Cuts:
                    if (ImGui::BeginMenu(ICON_FA_PLUS "  Add cut here"))
                    {
                        for (const auto& b : m_Data.Bindings)
                        {
                            Entity e = BoundEntity(b.ID);
                            if (e && e.HasComponent<CameraComponent>() && ImGui::MenuItem(b.Name.c_str()))
                                m_Data.CameraCuts.push_back({t, std::min(m_Data.Length, t + 1), b.ID}), Changed();
                        }
                        ImGui::TextDisabled("Bind a camera entity to cut to it.");
                        ImGui::EndMenu();
                    }
                    break;
            }
            ImGui::EndPopup();
        }
    }

    bool TimelinePanel::AssetCombo(const char* label, AssetType type, AssetID& id)
    {
        if (!PropertyGrid::Row(label))
            return false;
        std::string current = id.IsValid() ? id.ToString() : "(none)";
        for (const auto& choice : m_Choices)
            if (choice.ID == id)
                current = choice.Name;
        bool changed = false;
        ImGui::PushID(label);
        if (ImGui::BeginCombo("##asset", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (const auto& choice : m_Choices)
                if (choice.Type == type && ImGui::Selectable(choice.Name.c_str(), choice.ID == id))
                    id = choice.ID, changed = true;
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    void TimelinePanel::DrawInspector()
    {
        bool       changed = false;
        auto&      s       = m_Selected;
        const auto time    = [&](const char* label, double& value) {
            float v = float(value);
            if (PropertyGrid::Float(label, v, 0.01f, 0.0f, float(m_Data.Length), "%.3f s"))
                value = std::clamp(double(v), 0.0, m_Data.Length), changed = true;
        };
        const auto curveKey = [&](TimelineKey& key, int channels) {
            float values[4] {key.Value.x, key.Value.y, key.Value.z, key.Value.w};
            if (PropertyGrid::Row("Value"))
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragScalarN("##value", ImGuiDataType_Float, values, channels, 0.01f))
                    key.Value = {values[0], values[1], values[2], values[3]}, changed = true;
            }
            const char* modes[] {"Constant", "Linear", "Bezier"};
            int         mode = int(key.Interpolation);
            if (PropertyGrid::Combo("Interpolation", modes, 3, mode))
                key.Interpolation = TimelineInterpolation(mode), changed = true;
            if (key.Interpolation == TimelineInterpolation::Bezier)
            {
                // Slopes (value per second) leaving this key and arriving at it.
                float out[4] {key.OutTangent.x, key.OutTangent.y, key.OutTangent.z, key.OutTangent.w};
                float in[4] {key.InTangent.x, key.InTangent.y, key.InTangent.z, key.InTangent.w};
                if (PropertyGrid::Row("Out slope"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragScalarN("##out", ImGuiDataType_Float, out, channels, 0.01f))
                        key.OutTangent = {out[0], out[1], out[2], out[3]}, changed = true;
                }
                if (PropertyGrid::Row("In slope"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragScalarN("##in", ImGuiDataType_Float, in, channels, 0.01f))
                        key.InTangent = {in[0], in[1], in[2], in[3]}, changed = true;
                }
            }
        };
        if (!s.Valid())
        {
            ImGui::TextDisabled("Select a key, clip, range, event or cut.");
            ImGui::Spacing();
            if (PropertyGrid::BeginGroup("Bindings"))
            {
                for (auto& b : m_Data.Bindings)
                {
                    ImGui::PushID(b.ID.c_str());
                    if (PropertyGrid::Row(b.ID.c_str()))
                    {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        changed |= ImGui::InputText("##name", &b.Name);
                    }
                    ImGui::PopID();
                }
                PropertyGrid::EndGroup();
            }
        }
        else
            switch (s.Kind)
            {
                case RowKind::Transform: {
                    auto& track = m_Data.Transforms[s.Track];
                    if (PropertyGrid::BeginGroup("Transform key"))
                    {
                        double t = s.Time;
                        time("Time", t);
                        if (t != s.Time)
                        {
                            for (auto* curve : {&track.Position, &track.Rotation, &track.Scale})
                                if (auto* key = KeyAt(*curve, s.Time))
                                    key->Time = t, SortKeys(*curve);
                            s.Time = t;
                        }
                        changed |= PropertyGrid::Checkbox("Relative", track.Relative);
                        const char*    names[] {"Position", "Rotation", "Scale"};
                        TimelineCurve* curves[] {&track.Position, &track.Rotation, &track.Scale};
                        for (int i = 0; i < 3; ++i)
                            if (auto* key = KeyAt(*curves[i], s.Time))
                            {
                                ImGui::PushID(i);
                                PropertyGrid::Text(names[i], "", true);
                                curveKey(*key, 3);
                                ImGui::PopID();
                            }
                        PropertyGrid::EndGroup();
                    }
                    break;
                }
                case RowKind::Property:
                    if (PropertyGrid::BeginGroup(m_Data.Properties[s.Track].Path.c_str()))
                    {
                        auto& key = m_Data.Properties[s.Track].Curve.Keys[s.Item];
                        time("Time", key.Time);
                        curveKey(key, 4);
                        PropertyGrid::EndGroup();
                    }
                    break;
                case RowKind::Animation:
                    if (PropertyGrid::BeginGroup("Clip"))
                    {
                        auto& clip = m_Data.Animations[s.Track].Clips[s.Item];
                        changed |= AssetCombo("Clip", AssetType::AnimationClip, clip.Clip);
                        time("Start", clip.Start);
                        time("End", clip.End);
                        float       values[] {float(clip.ClipIn),
                                              float(clip.ClipOut),
                                              float(clip.Speed),
                                              float(clip.BlendIn),
                                              float(clip.BlendOut)};
                        const char* labels[] {"Clip in", "Clip out", "Speed", "Blend in", "Blend out"};
                        for (int i = 0; i < 5; ++i)
                            if (PropertyGrid::Float(
                                    labels[i], values[i], 0.01f, 0.0f, 100.0f, i == 2 ? "%.2fx" : "%.3f s"))
                                changed = true;
                        clip.ClipIn = values[0], clip.ClipOut = values[1], clip.Speed = std::max(0.01f, values[2]);
                        clip.BlendIn = values[3], clip.BlendOut = values[4];
                        if (PropertyGrid::Row("Slot"))
                        {
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            changed |= ImGui::InputTextWithHint("##slot", "Timeline", &m_Data.Animations[s.Track].Slot);
                        }
                        PropertyGrid::EndGroup();
                    }
                    break;
                case RowKind::Activation:
                    if (PropertyGrid::BeginGroup("Active range"))
                    {
                        auto& range = m_Data.Activations[s.Track].Ranges[s.Item];
                        time("Start", range.Start);
                        time("End", range.End);
                        PropertyGrid::EndGroup();
                    }
                    break;
                case RowKind::Events:
                    if (PropertyGrid::BeginGroup("Event"))
                    {
                        auto& e = m_Data.Events[s.Item];
                        time("Time", e.Time);
                        const auto text = [&](const char* label, std::string& value, const char* hint) {
                            if (PropertyGrid::Row(label))
                            {
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                changed |= ImGui::InputTextWithHint((std::string("##") + label).c_str(), hint, &value);
                            }
                        };
                        text("Name", e.Name, "");
                        text("Payload", e.Payload, "string passed along");
                        text("Binding", e.Binding, "binding ID (optional)");
                        text("Method", e.Method, "void Method(string) on its script");
                        PropertyGrid::EndGroup();
                    }
                    break;
                case RowKind::Cuts:
                    if (PropertyGrid::BeginGroup("Camera cut"))
                    {
                        auto& cut = m_Data.CameraCuts[s.Item];
                        time("Start", cut.Start);
                        time("End", cut.End);
                        if (PropertyGrid::Row("Camera"))
                        {
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            if (ImGui::BeginCombo("##camera", cut.Binding.c_str()))
                            {
                                for (const auto& b : m_Data.Bindings)
                                    if (ImGui::Selectable(b.Name.c_str(), b.ID == cut.Binding))
                                        cut.Binding = b.ID, changed = true;
                                ImGui::EndCombo();
                            }
                        }
                        PropertyGrid::EndGroup();
                    }
                    break;
                default:
                    break;
            }
        if (s.Valid())
        {
            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_TRASH "  Delete", {-FLT_MIN, 0}))
                DeleteSelection();
        }
        if (changed)
            Changed();
    }

    void TimelinePanel::DeleteSelection()
    {
        auto& s = m_Selected;
        if (!s.Valid())
            return;
        switch (s.Kind)
        {
            case RowKind::Transform: {
                auto& track = m_Data.Transforms[s.Track];
                for (auto* curve : {&track.Position, &track.Rotation, &track.Scale})
                    curve->Keys.erase(std::remove_if(curve->Keys.begin(),
                                                     curve->Keys.end(),
                                                     [&](const auto& k) { return std::abs(k.Time - s.Time) < 1e-6; }),
                                      curve->Keys.end());
                break;
            }
            case RowKind::Property:
                m_Data.Properties[s.Track].Curve.Keys.erase(m_Data.Properties[s.Track].Curve.Keys.begin() +
                                                            long(s.Item));
                break;
            case RowKind::Animation:
                m_Data.Animations[s.Track].Clips.erase(m_Data.Animations[s.Track].Clips.begin() + long(s.Item));
                break;
            case RowKind::Activation:
                m_Data.Activations[s.Track].Ranges.erase(m_Data.Activations[s.Track].Ranges.begin() + long(s.Item));
                break;
            case RowKind::Events:
                m_Data.Events.erase(m_Data.Events.begin() + long(s.Item));
                break;
            case RowKind::Cuts:
                m_Data.CameraCuts.erase(m_Data.CameraCuts.begin() + long(s.Item));
                break;
            default:
                break;
        }
        s = {};
        Changed();
    }
} // namespace Kans
