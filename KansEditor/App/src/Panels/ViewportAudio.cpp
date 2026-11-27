#include <imgui.h>
#include <cmath>
#include <functional>
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Audio/AudioEngine.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/Audio/AudioEditing.h"
#include "UI/EditorIcons.h"
#include "ViewportPanel.h"

namespace Kans
{
    namespace
    {
        // The smallest radius, half extent or length a handle leaves (metres in the local frame).
        constexpr float SmallestSize = 0.01f;

        glm::vec4 Vec(const ImVec4& c) { return {c.x, c.y, c.z, c.w}; }
        ImU32 Color(glm::vec4 c, float alpha) { return ImGui::ColorConvertFloat4ToU32({c.r, c.g, c.b, c.a * alpha}); }

        // The attenuation asset a source plays with: its own, else its event's (null while loading
        // or without one).
        Ref<const AudioAttenuationAsset> AttenuationOf(AssetManager* assets, const AudioSourceComponent& c)
        {
            if (!assets)
                return nullptr;
            AssetID id = c.AttenuationAsset;
            if (!id.IsValid() && c.Event.IsValid())
                if (const auto event = assets->Request<AudioEventAsset>(c.Event).Asset)
                    id = event->GetData().AttenuationAsset;
            return id.IsValid() ? assets->Request<AudioAttenuationAsset>(id).Asset : nullptr;
        }
    } // namespace

    void ViewportPanel::DrawAudioMenu()
    {
        ImGui::TextDisabled("Audio");
        ImGui::Checkbox(ICON_FA_VOLUME_HIGH "  Audio sources and zones", &m_ShowAllAudio);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw every audio source's shape and range and every reverb zone, not only the "
                              "selection's");
        ImGui::Checkbox(ICON_FA_EAR_LISTEN "  Occlusion rays (playing)", &m_ShowOcclusionRays);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw the rays audio occlusion casts from each listener to its sources: green "
                              "where clear, red past the first hit, hit points marked");
    }

    void ViewportPanel::UpdateOcclusionRays()
    {
        // The engine's debug hook records the rays while the toggle is on; queries run while the scene
        // plays (edit mode casts none).
        AudioEngine* engine = m_Scene ? m_Scene->GetAudioEngine() : nullptr;
        if (m_ShowOcclusionRays && engine && engine->IsInitialized())
            m_OcclusionRays.Install(*engine);
        else if (m_OcclusionRays.GetEngine())
            m_OcclusionRays.Remove();
    }

    void ViewportPanel::DrawOcclusionRays(ImDrawList* draw)
    {
        if (!m_OcclusionRays.GetEngine())
            return;
        const auto&     colors         = EditorTheme::Colors();
        const ImU32     clear          = ImGui::ColorConvertFloat4ToU32(colors.Success);
        const ImU32     blocked        = ImGui::ColorConvertFloat4ToU32(colors.Danger);
        const glm::mat4 viewProjection = ViewProjection();
        for (size_t i = 0; i < m_OcclusionRays.Size(); ++i)
        {
            const auto& ray = m_OcclusionRays.At(i);
            OverlaySegment(draw, ray.Start, ray.ClearEnd(), clear, EditorTheme::Px(1.5f));
            if (!ray.Blocked())
                continue;
            OverlaySegment(draw, ray.ClearEnd(), ray.End, blocked, EditorTheme::Px(1.5f));
            for (uint32_t h = 0; h < ray.HitCount; ++h)
            {
                const glm::vec4 clip = viewProjection * glm::vec4(ray.Hits[h], 1.0f);
                if (clip.w <= 1e-3f)
                    continue;
                const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                const ImVec2    at {m_Min.x + (ndc.x * 0.5f + 0.5f) * (m_Max.x - m_Min.x),
                                    m_Min.y + (0.5f - ndc.y * 0.5f) * (m_Max.y - m_Min.y)};
                draw->AddCircleFilled(at, EditorTheme::Px(3.5f), blocked);
                draw->AddCircle(at, EditorTheme::Px(3.5f), IM_COL32(20, 20, 20, 255), 0, 1.0f);
            }
        }
        m_OcclusionRays.EndFrame();
    }

    void ViewportPanel::DrawAudioOverlay()
    {
        UpdateOcclusionRays();
        if (!m_Scene || UsesSceneCamera())
            return;
        Scene&     scene    = *m_Scene;
        const auto hidden   = m_Client->HiddenInEditor();
        Entity     selected = m_Hierarchy.GetSelected();
        if (selected && hidden.count(selected.GetUUID()))
            selected = {};
        const auto&     colors = EditorTheme::Colors();
        const glm::vec4 audio  = Vec(colors.TypeAudio);
        const glm::vec4 zone   = Vec(colors.Accent);
        const glm::vec4 cone   = Vec(colors.Warning);
        ImDrawList*     draw   = ImGui::GetWindowDrawList();
        draw->PushClipRect(m_Min, m_Max, true);
        // A source: its shape solid, the full-volume range bright, the outer range faint, the cone.
        // A zone: its shape solid-edged, the fade band's outer edge faint. Unselected ones are dimmer.
        const auto drawEntity = [&](Entity e, bool chosen) {
            const glm::mat4                         world  = scene.GetWorldTransform(e);
            const float                             strong = chosen ? 1.0f : 0.55f;
            std::vector<AudioEditing::GizmoSegment> segments;
            if (e.HasComponent<AudioSourceComponent>())
            {
                const auto& c     = e.GetComponent<AudioSourceComponent>();
                const auto  asset = AttenuationOf(m_Assets, c);
                segments          = AudioEditing::SourceGizmo(c, world, asset ? &asset->GetData() : nullptr);
            }
            if (e.HasComponent<AudioReverbZoneComponent>())
            {
                const auto zoneSegments =
                    AudioEditing::ReverbZoneGizmo(e.GetComponent<AudioReverbZoneComponent>(), world);
                segments.insert(segments.end(), zoneSegments.begin(), zoneSegments.end());
            }
            for (const auto& s : segments)
            {
                using Part      = AudioEditing::GizmoPart;
                glm::vec4 color = audio;
                float     alpha = strong, width = 1.5f;
                switch (s.Part)
                {
                    case Part::Shape:
                        width = 2.0f;
                        break;
                    case Part::MinDistance: // Full volume inside: as bright as the shape.
                        break;
                    case Part::MaxDistance:
                        alpha *= 0.4f;
                        break;
                    case Part::InnerCone:
                        color = cone;
                        break;
                    case Part::OuterCone:
                        color = cone, alpha *= 0.5f;
                        break;
                    case Part::ZoneShape:
                        color = zone, width = 2.0f;
                        break;
                    case Part::ZoneFade:
                        color = zone, alpha *= 0.4f;
                        break;
                }
                OverlaySegment(draw, s.A, s.B, Color(color, alpha), EditorTheme::Px(width));
            }
        };
        if (m_ShowAllAudio)
        {
            std::function<void(Entity)> walk = [&](Entity e) {
                if (e != selected && !hidden.count(e.GetUUID()) &&
                    (e.HasComponent<AudioSourceComponent>() || e.HasComponent<AudioReverbZoneComponent>()))
                    drawEntity(e, false);
                for (Entity child : scene.GetChildren(e))
                    walk(child);
            };
            for (Entity root : scene.GetRootEntities())
                walk(root);
        }
        if (selected)
            drawEntity(selected, true);
        DrawOcclusionRays(draw);
        draw->PopClipRect();
        // Handles edit authored data, so not while the scene runs.
        if (selected && !m_Playing && !m_MeshTool.IsEditing(selected) && !IsPaintingCloth())
            AudioHandles(selected);
    }

    void ViewportPanel::AudioHandles(Entity selected)
    {
        auto&           handles = m_Handles;
        const glm::mat4 world   = m_Scene->GetWorldTransform(selected);
        const uint64_t  owner   = uint64_t(selected.GetUUID());
        // Stable IDs: the entity, the kind of handle (past the physics kinds) and its index.
        const auto id = [&](uint64_t kind, uint64_t index = 0) {
            return Gizmo::MakeHandleId(Gizmo::MakeHandleId(owner, 0x6175646900ull + kind), index);
        };
        const auto tip = [](const Gizmo::HandleResult& r, const char* text) {
            if (r.Hovered && !r.Active && text)
                ImGui::SetTooltip("%s", text);
        };
        const auto style = [](glm::vec4 color, Gizmo::KnobShape shape) {
            Gizmo::HandleStyle s;
            s.Color = color;
            s.Knob  = shape;
            return s;
        };
        const glm::vec4 audio = Vec(EditorTheme::Colors().TypeAudio);
        const glm::vec4 zone  = Vec(EditorTheme::Colors().Accent);

        // A source's shape: sphere radii, box faces (both sides: the shape stays centred on its
        // entity), the line's two ends.
        if (selected.HasComponent<AudioSourceComponent>())
        {
            const AudioEmitterShape start  = selected.GetComponent<AudioSourceComponent>().Shape;
            AudioEmitterShape       values = start;
            Gizmo::HandleResult     r;
            if (start.Type == AudioSourceShape::Sphere)
            {
                Gizmo::SphereHandle h;
                h.Frame       = world;
                h.MinRadius   = SmallestSize;
                h.Style       = style(audio, Gizmo::KnobShape::Sphere);
                float radius  = std::abs(values.Radius);
                r             = handles.Sphere(id(1), h, radius);
                values.Radius = radius;
                tip(r, "Drag to resize the source's sphere");
            }
            else if (start.Type == AudioSourceShape::Box)
            {
                Gizmo::BoxHandle h;
                h.Frame      = world;
                h.MinHalf    = SmallestSize;
                h.LockCenter = true;
                h.Style      = style(audio, Gizmo::KnobShape::Cube);
                glm::vec3 centre(0.0f), half = glm::abs(values.HalfExtents);
                r                  = handles.Box(id(2), h, centre, half);
                values.HalfExtents = half;
                tip(r, "Drag to resize the source's box");
            }
            else if (start.Type == AudioSourceShape::Line)
            {
                for (int end = 0; end < 2; ++end)
                {
                    const auto          frame = AudioEditing::LineLengthHandle(world, end == 0);
                    Gizmo::LinearHandle h;
                    h.Origin        = frame.Origin;
                    h.Direction     = frame.Direction;
                    h.Min           = SmallestSize * 0.5f;
                    h.Style         = style(audio, Gizmo::KnobShape::Sphere);
                    float      half = std::abs(values.Length) * 0.5f;
                    const auto e    = handles.Linear(id(3, uint64_t(end)), h, half);
                    if (e.Changed)
                        values.Length = half * 2.0f;
                    tip(e, "Drag to set the line's length (it stays centred on the entity)");
                    r |= e;
                }
            }
            if (r.Started)
                handles.Stash(start);
            if (r.Cancelled)
                if (const auto* kept = handles.Stashed<AudioEmitterShape>())
                    values = *kept;
            if (r.Changed)
                selected.PatchComponent<AudioSourceComponent>([&](AudioSourceComponent& c) { c.Shape = values; });
        }

        // A reverb zone: its shape, and the fade distance outward from its surface along +X.
        if (selected.HasComponent<AudioReverbZoneComponent>())
        {
            const AudioReverbZoneComponent start  = selected.GetComponent<AudioReverbZoneComponent>();
            AudioReverbZoneComponent       values = start;
            Gizmo::HandleResult            r;
            if (start.Shape == AudioReverbZoneShape::Sphere)
            {
                Gizmo::SphereHandle h;
                h.Frame       = world;
                h.MinRadius   = SmallestSize;
                h.Style       = style(zone, Gizmo::KnobShape::Sphere);
                float radius  = std::abs(values.Radius);
                r             = handles.Sphere(id(4), h, radius);
                values.Radius = radius;
            }
            else
            {
                Gizmo::BoxHandle h;
                h.Frame      = world;
                h.MinHalf    = SmallestSize;
                h.LockCenter = true;
                h.Style      = style(zone, Gizmo::KnobShape::Cube);
                glm::vec3 centre(0.0f), half = glm::abs(values.HalfExtents);
                r                  = handles.Box(id(5), h, centre, half);
                values.HalfExtents = half;
            }
            tip(r, "Drag to resize the zone");
            {
                const auto          frame = AudioEditing::ZoneFadeHandle(start, world);
                Gizmo::LinearHandle h;
                h.Origin        = frame.Origin;
                h.Direction     = frame.Direction;
                h.Min           = 0.0f;
                h.Style         = style(zone * glm::vec4(1, 1, 1, 0.7f), Gizmo::KnobShape::Sphere);
                float      fade = std::abs(values.FadeDistance);
                const auto f    = handles.Linear(id(6), h, fade);
                if (f.Changed)
                    values.FadeDistance = fade;
                tip(f, "Drag to set the fade distance (the send fades in over this band)");
                r |= f;
            }
            if (r.Started)
                handles.Stash(start);
            if (r.Cancelled)
                if (const auto* kept = handles.Stashed<AudioReverbZoneComponent>())
                    values = *kept;
            if (r.Changed)
                selected.PatchComponent<AudioReverbZoneComponent>([&](AudioReverbZoneComponent& c) { c = values; });
        }
    }
} // namespace Kans
