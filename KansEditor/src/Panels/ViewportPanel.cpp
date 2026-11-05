#include "ViewportPanel.h"
#include <imgui_internal.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Animation/AnimatorComponent.h"
#include "Kans3D/Core/Math/Math.h"
#include "Kans3D/Editor/DropPlacement.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Input/Input.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Components.h"
#include "Gizmo/ViewAxes.h"
#include "UI/BloomSettingsWidget.h"
#include "UI/EditorIcons.h"
#include "UI/MaterialSlotRow.h"
#include "UI/ModalInput.h"
#include "UI/PropertyGrid.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace
    {
        struct GameResolution
        {
            const char* Name;
            uint32_t    Width = 0, Height = 0;
        };
        const std::vector<GameResolution>& GameResolutions()
        {
            static const std::vector<GameResolution> list {{"Fit the panel"},
                                                           {"1920 x 1080", 1920, 1080},
                                                           {"1280 x 720", 1280, 720},
                                                           {"2560 x 1440", 2560, 1440},
                                                           {"1080 x 1920 (portrait)", 1080, 1920},
                                                           {"750 x 1334 (phone)", 750, 1334}};
            return list;
        }
    } // namespace

    using Widgets::OverlayGroup;
    using Widgets::ToolButton;

    void ViewportPanel::SetContext(Ref<Scene> scene, ViewportClient* client)
    {
        m_Scene  = std::move(scene);
        m_Client = client;
        m_Size   = {0, 0}; // Re-applies the viewport size to the new scene.
    }

    void ViewportPanel::Draw()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        m_Hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (!m_Scene || !m_Client)
        {
            ImGui::TextDisabled("No scene.");
            return;
        }
        const ImVec2 size = ImGui::GetContentRegionAvail();
        m_Min             = ImGui::GetCursorScreenPos();
        m_Max             = {m_Min.x + size.x, m_Min.y + size.y};
        // Resize and render in this frame, so the image always matches the panel. Through the game
        // camera a chosen resolution renders at that size and is shown letterboxed.
        m_ImageMin  = m_Min;
        m_ImageSize = size;
        if (const auto& r = GameResolutions()[size_t(m_GameResolution)]; UsesSceneCamera() && r.Width > 0)
        {
            const float scale = std::min(size.x / r.Width, size.y / r.Height);
            m_ImageSize       = {std::max(1.0f, r.Width * scale), std::max(1.0f, r.Height * scale)};
            m_ImageMin        = {m_Min.x + (size.x - m_ImageSize.x) * 0.5f, m_Min.y + (size.y - m_ImageSize.y) * 0.5f};
            Resize({float(r.Width), float(r.Height)});
        }
        else
            Resize(size);
        // Outline the selection with its descendants' meshes; the renderer resolves
        // them after this frame's asset updates.
        Entity selected = m_Hierarchy.GetSelected();
        // Element editing draws its own selection instead of the entity outline.
        const bool editing = m_MeshTool.IsEditing(selected) && !UsesSceneCamera();
        ViewHighlights highlights;
        if (selected && !UsesSceneCamera() && !editing)
        {
            highlights.Selected.push_back(selected.GetUUID());
            for (auto entity : m_Hierarchy.GetSelection())
                if (entity != selected)
                    highlights.Selected.push_back(entity.GetUUID());
        }
        if (m_HighlightSource && !UsesSceneCamera() && !editing)
            m_HighlightSource(highlights);
        if (m_DropSlot)
            highlights.Highlights.push_back({m_DropSlot->first, m_DropSlot->second, Widgets::SlotHoverColor});
        m_Client->SetHighlights(std::move(highlights));
        Render();

        const auto output = m_Client->GetOutput();
        if (m_ImageSize.x != size.x || m_ImageSize.y != size.y)
        {
            ImGui::GetWindowDrawList()->AddRectFilled(m_Min, m_Max, IM_COL32(0, 0, 0, 255));
            ImGui::SetCursorScreenPos(m_ImageMin);
        }
        ImGui::Image(static_cast<ImTextureID>(output), m_ImageSize, ImVec2(0, 1), ImVec2(1, 0));
        m_ImageHovered = ImGui::IsItemHovered();
        if (m_Playing)
        {
            // A colored frame marks play mode; changes made now are discarded on stop.
            ImGui::GetWindowDrawList()->AddRect(m_Min, m_Max, IM_COL32(60, 170, 255, 255), 0.0f, 0, 3.0f);
        }
        AcceptDrop();
        DrawSceneOverlays();
        m_ModalThisFrame = m_Modal.IsActive();
        BeginHandles();
        DrawPhysicsOverlay();
        // A particle entity's VFX handles edit (or show) its system asset, outside play.
        if (!m_Playing && !editing)
            VfxHandles(selected);
        // Cloth painting takes the left button over the cloth (selection and the gizmo wait).
        if (m_ClothPaint.IsActive() && !UsesSceneCamera() && !m_Playing)
        {
            const ClothPaintTool::View view {m_Min, m_Max, &m_Camera, m_Scene.get()};
            m_ClothPaint.DrawOverlay(view);
            m_ClothPaint.Update(view, m_ImageHovered && !ImGui::IsAnyItemHovered());
        }
        if (editing)
        {
            m_MeshTool.DrawOverlay(selected, EditView());
            if (m_MeshTool.Update(selected, EditView()) == MeshEditTool::Release::Missed)
            {
                // A click on no element may pick another entity; empty space keeps this one.
                Entity hit = EntityAt(ImGui::GetIO().MousePos);
                if (hit && hit != selected)
                    m_Hierarchy.SetSelected(hit);
            }
        }
        DrawToolbar();
        DrawViewAxes();
        // After the toolbar and the axis widget, so the gizmo knows whether they have the pointer.
        DrawGizmo();
        DrawStatsOverlay();
        // After the overlays, so clicks on their widgets do not navigate.
        HandleNavigation();
        HandleShortcuts();
    }

    UIViewport ViewportPanel::GameViewport() const
    {
        // The image's screen rectangle; the scale maps it to the rendered pixels.
        return {{m_ImageMin.x, m_ImageMin.y},
                {m_ImageSize.x, m_ImageSize.y},
                {float(m_Size.x) / std::max(1.0f, m_ImageSize.x), float(m_Size.y) / std::max(1.0f, m_ImageSize.y)}};
    }

    void ViewportPanel::GameInput(UIInputState& input) const
    {
        const ImGuiIO& io   = ImGui::GetIO();
        input.Pointer       = {io.MousePos.x, io.MousePos.y};
        input.PointerInside = m_ImageHovered;
        for (int b = 0; b < int(input.Buttons.size()) && b < ImGuiMouseButton_COUNT; ++b)
        {
            // Presses count over the image; releases anywhere, so a drag can end outside it.
            input.ButtonDown[b] = m_ImageHovered && io.MouseClicked[b];
            input.ButtonUp[b]   = io.MouseReleased[b];
            input.Buttons[b]    = io.MouseDown[b] && (input.Buttons[b] || input.ButtonDown[b]);
        }
        if (m_ImageHovered)
            input.Scroll += glm::vec2(io.MouseWheelH, io.MouseWheel);
    }

    void ViewportPanel::Resize(ImVec2 size)
    {
        const glm::uvec2 pixels(std::max(1.0f, size.x), std::max(1.0f, size.y));
        if (pixels == m_Size)
            return;
        m_Size = pixels;
        m_Client->Resize(pixels);
        m_Camera.SetViewportSize(pixels.x, pixels.y);
        m_Scene->OnViewportResize(pixels.x, pixels.y);
    }

    bool ViewportPanel::UsesSceneCamera() const
    {
        // Without a primary camera, play mode still shows the editor view.
        // A timeline preview can also look through the camera its cuts make primary.
        return ((m_Playing && !m_UseEditorCameraInPlay) || m_PreviewSceneCamera) && m_Scene &&
               m_Scene->GetCameraEntity();
    }

    glm::mat4 ViewportPanel::ViewProjection() const
    {
        if (!UsesSceneCamera())
            return m_Camera.GetViewProjection();
        Entity camera = m_Scene->GetCameraEntity();
        return camera.GetComponent<CameraComponent>().SceneCamera.GetProjectionMatrix() *
               glm::inverse(m_Scene->GetWorldTransform(camera));
    }

    void ViewportPanel::Render()
    {
        // The game camera shows what the game renders: no editor grid or outlines.
        m_Client->SetViewMode(m_ViewMode);
        m_Client->SetGrid(m_Grid);
        if (!UsesSceneCamera() || !m_Client->RenderGame())
            m_Client->RenderEditor(m_Camera);
    }

    MeshEditTool::View ViewportPanel::EditView() { return {m_Min, m_Max, &m_Camera, m_Scene.get()}; }

    void ViewportPanel::PickAt(ImVec2 mouse)
    {
        Entity hit = EntityAt(mouse);
        // Ctrl adds to the selection or removes from it; a Ctrl-click on nothing keeps it.
        if (ImGui::GetIO().KeyCtrl)
            m_Hierarchy.ToggleSelected(hit);
        else
            m_Hierarchy.SetSelected(hit);
    }

    Entity ViewportPanel::EntityAt(ImVec2 mouse)
    {
        // Overlay icons (lights, cameras) take precedence over meshes.
        float best = 14.0f * 14.0f;
        UUID  icon = 0;
        for (const auto& [center, id] : m_Icons)
        {
            const float dx = center.x - mouse.x, dy = center.y - mouse.y;
            if (dx * dx + dy * dy < best)
                best = dx * dx + dy * dy, icon = id;
        }
        if (icon)
            return m_Scene->GetEntityByUUID(icon);
        // The framebuffer's origin is its bottom-left corner.
        const glm::uvec2 pixel(uint32_t(mouse.x - m_Min.x), uint32_t(m_Max.y - mouse.y - 1.0f));
        const auto       hit  = m_Client->Pick(ViewProjection(), pixel);
        Entity           part = m_Scene->GetEntityByUUID(hit.EntityID);
        Entity           root = m_Scene->GetEntityByUUID(hit.InstanceRoot);
        // The first click on a model selects the whole instance; clicking again inside
        // the selected instance selects the part under the cursor.
        Entity selected = m_Hierarchy.GetSelected();
        if (root && !(selected && m_Scene->GetPrefabInstanceRoot(selected) == root))
            part = root;
        return part;
    }

    void ViewportPanel::AcceptDrop()
    {
        if (!ImGui::BeginDragDropTarget())
        {
            m_DragFile.clear();
            m_DropSlot.reset();
            return;
        }
        // Accepted every frame of the hover, so the model can be previewed where it would land.
        const auto* data = ImGui::AcceptDragDropPayload("asset_payload",
                                                        ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                            ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (data)
        {
            const auto file = std::filesystem::u8path(std::string(static_cast<const char*>(data->Data), size_t(data->DataSize)));
            if (file.extension() == ".material")
            {
                DragMaterial(file, data->IsDelivery());
                ImGui::EndDragDropTarget();
                return;
            }
            if (file != m_DragFile)
            {
                m_DragFile  = file;
                m_DragModel = m_ResolveModel && file.extension() != ".kans" ? m_ResolveModel(file) : AssetID {};
            }
            std::optional<glm::vec3> position;
            if (file.extension() != ".kans" && !UsesSceneCamera())
            {
                position = DropPosition(ImGui::GetMousePos(), m_DragModel);
                if (m_DragModel.IsValid())
                    m_Client->SetPreview(m_DragModel, glm::translate(glm::mat4(1.0f), *position));
            }
            if (data->IsDelivery())
            {
                m_DragFile.clear();
                if (m_FileDrop)
                    m_FileDrop(file, position);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ScenePickHit ViewportPanel::SurfaceAt(ImVec2 mouse)
    {
        // The picker uses a bottom-left pixel origin.
        const glm::vec2 pixel(mouse.x - m_Min.x, mouse.y - m_Min.y);
        return m_Client->Pick(ViewProjection(),
                              {uint32_t(std::clamp(pixel.x, 0.0f, float(m_Size.x - 1))),
                               uint32_t(std::clamp(float(m_Size.y) - 1.0f - pixel.y, 0.0f, float(m_Size.y - 1)))});
    }

    void ViewportPanel::DragMaterial(const std::filesystem::path& file, bool delivered)
    {
        m_DropSlot.reset();
        Entity part;
        if (!UsesSceneCamera() && m_Scene)
        {
            const auto hit = SurfaceAt(ImGui::GetMousePos());
            part           = hit.MaterialSlot ? m_Scene->GetEntityByUUID(hit.EntityID) : Entity {};
            if (part)
                m_DropSlot = std::make_pair(hit.EntityID, *hit.MaterialSlot);
        }
        const auto material = file.stem().u8string();
        if (m_DropSlot)
        {
            const auto  slots = m_Scene->GetMeshSlots(part);
            const auto  slot  = m_DropSlot->second;
            std::string name  = slot < slots.size() && !slots[slot].Name.empty() ? slots[slot].Name :
                                                                                   "Slot " + std::to_string(slot + 1);
            ImGui::SetTooltip("%s  " ICON_FA_ARROW_LEFT "  %s\n%s",
                              name.c_str(),
                              material.c_str(),
                              part.GetComponent<TagComponent>().Tag.c_str());
        }
        else
            ImGui::SetTooltip("Drop %s on a model part to use it there", material.c_str());
        if (delivered)
        {
            if (m_DropSlot && m_MaterialDrop)
                m_MaterialDrop(part, m_DropSlot->second, file);
            m_DropSlot.reset();
            m_DragFile.clear();
        }
    }

    glm::vec3 ViewportPanel::DropPosition(ImVec2 mouse, AssetID model)
    {
        const glm::vec2 pixel(mouse.x - m_Min.x, mouse.y - m_Min.y);
        const auto [origin, direction] = m_Camera.ScreenRay(pixel);
        // The surface under the cursor.
        const auto hit = SurfaceAt(mouse);
        Bounds bounds;
        if (model.IsValid() && m_Client->GetAssetCache())
            bounds = m_Client->GetAssetCache()->GetModelBounds(model);
        return ComputeDropPosition(origin, direction, hit.Position, bounds, m_Camera.GetDistance());
    }

    // ---------------------------------------------------------------------- navigation

    void ViewportPanel::HandleNavigation()
    {
        const ImGuiIO& io        = ImGui::GetIO();
        const bool     overCube  = ImGui::IsMouseHoveringRect(m_CubeMin, m_CubeMax, false);
        const bool     overImage = m_ImageHovered && !ImGui::IsAnyItemHovered() && !overCube;
        // A modal transform owns the buttons (its clicks confirm or cancel it).
        if (m_Navigation == Navigation::None && overImage && !m_ModalThisFrame)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !m_SwallowRightClick)
                m_Navigation = Navigation::Look;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                m_Navigation = Navigation::Pan;
            else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                m_Navigation = Navigation::Orbit;
            if (m_Navigation == Navigation::Look)
                Input::SetCursorMode(CursorMode::Disable);
            if (m_Navigation != Navigation::None)
                ImGui::SetWindowFocus();
            // A plain left click selects what is under the cursor.
            else if (!io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_Gizmo.WantsPointer() &&
                     !m_MeshTool.GetGizmo().WantsPointer() && !m_Handles.WantsPointer())
            {
                if (m_MeshTool.IsEditing(m_Hierarchy.GetSelected()) && !UsesSceneCamera())
                    m_MeshTool.Press(io.MousePos);
                else if (!IsPaintingCloth())
                    PickAt(io.MousePos);
            }
        }

        switch (m_Navigation)
        {
            case Navigation::Look:
            {
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
                    break;
                m_Camera.Look({io.MouseDelta.x, io.MouseDelta.y});
                const auto axis = [](ImGuiKey positive, ImGuiKey negative) {
                    return (ImGui::IsKeyDown(positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(negative) ? 1.0f : 0.0f);
                };
                const glm::vec3 direction(axis(ImGuiKey_D, ImGuiKey_A), axis(ImGuiKey_E, ImGuiKey_Q), axis(ImGuiKey_W, ImGuiKey_S));
                if (direction != glm::vec3(0.0f))
                    m_Camera.Fly(direction * (io.KeyShift ? 3.0f : 1.0f), io.DeltaTime);
                if (io.MouseWheel != 0.0f)
                    m_Camera.SetFlySpeed(m_Camera.GetFlySpeed() * std::pow(1.2f, io.MouseWheel));
                return;
            }
            case Navigation::Orbit:
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    m_Camera.Orbit({io.MouseDelta.x, io.MouseDelta.y});
                    return;
                }
                break;
            case Navigation::Pan:
                if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                {
                    m_Camera.Pan({io.MouseDelta.x, io.MouseDelta.y});
                    return;
                }
                break;
            case Navigation::None:
                if (overImage && io.MouseWheel != 0.0f)
                    m_Camera.Dolly(io.MouseWheel);
                return;
        }
        // The navigating button was released.
        if (m_Navigation == Navigation::Look)
            Input::SetCursorMode(CursorMode::Normal);
        m_Navigation = Navigation::None;
    }

    void ViewportPanel::HandleShortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        // A modal transform takes the keys (axis letters, digits, Tab, Backspace) this frame.
        if ((!m_Hovered && !m_Focused) || io.WantTextInput || m_Navigation != Navigation::None || m_ModalThisFrame)
            return;
        m_MeshTool.HandleShortcuts(m_Hierarchy.GetSelected());
        if (!m_MeshTool.IsEditing(m_Hierarchy.GetSelected()))
        {
            // Entity actions on the selection, as in the Hierarchy.
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
                m_Hierarchy.DuplicateSelection();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                m_Hierarchy.CopySelection();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
                m_Hierarchy.CutSelection();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                m_Hierarchy.Paste();
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                m_Hierarchy.DeleteSelection();
        }
        if (io.KeyCtrl)
            return;
        // The tool stays while a drag is in progress.
        if (!IsGizmoDragging())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
                m_Tool = Tool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_W, false))
                m_Tool = Tool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false))
                m_Tool = Tool::Rotate;
            // Over the view R rotates modally (Blender); elsewhere, or by preference, it picks the scale tool.
            if (ImGui::IsKeyPressed(ImGuiKey_R, false) && (m_RPicksScaleTool || !m_ImageHovered))
                m_Tool = Tool::Scale;
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                m_Tool = Tool::Transform;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
            FocusSelection();
    }

    void ViewportPanel::FocusSelection()
    {
        Entity selected = m_Hierarchy.GetSelected();
        if (!selected || !m_Scene)
            return;
        // Frame the selected entity with its children.
        Bounds                      bounds;
        std::function<void(Entity)> visit = [&](Entity entity) {
            // World bounds already include the entity's meshes and descendants; entities
            // without geometry (lights, cameras, empty entities) contribute their position.
            // A prefab's empty nodes (e.g. skeleton joints) are not framed: only what renders.
            const Bounds world = m_Scene->GetWorldBounds(entity);
            if (world.IsValid())
                bounds.Expand(world);
            else if (!entity.HasComponent<PrefabOwnedComponent>())
                bounds.Expand(glm::vec3(m_Scene->GetWorldTransform(entity)[3]));
            for (Entity child : m_Scene->GetChildren(entity))
                visit(child);
        };
        visit(selected);
        m_Camera.Focus(bounds.Center(), std::max(bounds.Radius(), 0.5f));
    }

    // ---------------------------------------------------------------------- overlays

    bool ViewportPanel::ToScreen(const glm::vec3& point, ImVec2& screen) const
    {
        const glm::vec4 clip = ViewProjection() * glm::vec4(point, 1.0f);
        if (clip.w <= 1e-4f)
            return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        screen = {m_Min.x + (ndc.x * 0.5f + 0.5f) * (m_Max.x - m_Min.x),
                  m_Min.y + (0.5f - ndc.y * 0.5f) * (m_Max.y - m_Min.y)};
        return true;
    }

    void ViewportPanel::DrawSceneOverlays()
    {
        m_Icons.clear();
        if (!m_ShowIcons || UsesSceneCamera())
            return;
        ImDrawList*  draw     = ImGui::GetWindowDrawList();
        const UUID   selected = m_Hierarchy.GetSelected() ? m_Hierarchy.GetSelected().GetUUID() : UUID(0);
        const ImU32  line     = IM_COL32(230, 230, 230, 170);
        const ImU32  accent   = IM_COL32(255, 160, 40, 255);
        const auto   segment  = [&](const glm::vec3& a, const glm::vec3& b, ImU32 color) {
            ImVec2 sa, sb;
            if (ToScreen(a, sa) && ToScreen(b, sb))
                draw->AddLine(sa, sb, color, 1.5f);
        };
        // The selected animated model's skeleton: a line from each joint to its parent.
        if (Entity picked = m_Hierarchy.GetSelected())
        {
            Entity root = m_Scene->GetPrefabInstanceRoot(picked);
            root        = root ? root : picked;
            if (root.HasComponent<AnimatorComponent>())
            {
                const auto& animator = root.GetComponent<AnimatorComponent>();
                if (animator.SkeletonSource)
                {
                    const auto& joints = animator.SkeletonSource->GetData().Joints;
                    for (size_t j = 0; j < joints.size() && j < animator.JointEntities.size(); ++j)
                    {
                        const int32_t parent = joints[j].Parent;
                        if (parent < 0 || size_t(parent) >= animator.JointEntities.size())
                            continue;
                        Entity bone = m_Scene->GetEntityByUUID(animator.JointEntities[j]);
                        Entity up   = m_Scene->GetEntityByUUID(animator.JointEntities[parent]);
                        if (!bone || !up)
                            continue;
                        const glm::vec3 a = glm::vec3(m_Scene->GetWorldTransform(up)[3]);
                        const glm::vec3 b = glm::vec3(m_Scene->GetWorldTransform(bone)[3]);
                        ImVec2          sa, sb;
                        if (ToScreen(a, sa) && ToScreen(b, sb))
                        {
                            draw->AddLine(sa, sb, IM_COL32(90, 200, 255, 220), 2.0f);
                            draw->AddCircleFilled(sb, 2.5f, IM_COL32(90, 200, 255, 255));
                        }
                    }
                }
            }
        }
        draw->PushClipRect(m_Min, m_Max, true);
        std::function<void(Entity)> visit = [&](Entity entity) {
            for (Entity child : m_Scene->GetChildren(entity))
                visit(child);
            const char* icon = entity.HasComponent<CameraComponent>()     ? ICON_FA_VIDEO :
                               entity.HasComponent<DirLightComponent>()   ? ICON_FA_SUN :
                               entity.HasComponent<PointLightComponent>() ? ICON_FA_LIGHTBULB :
                                                                            nullptr;
            if (!icon)
                return;
            const glm::mat4 world  = m_Scene->GetWorldTransform(entity);
            const glm::vec3 origin = glm::vec3(world[3]);
            const bool      chosen = entity.GetUUID() == selected;
            const ImU32     color  = chosen ? accent : line;
            if (entity.HasComponent<CameraComponent>())
            {
                // Frustum to a short display distance.
                auto&           camera = entity.GetComponent<CameraComponent>().SceneCamera;
                const float     aspect = std::max(camera.GetAspectRatio(), 0.01f);
                const glm::mat4 projection =
                    camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective ?
                        glm::perspective(glm::radians(camera.GetPerspectiveVerticalFOV()), aspect, 0.1f, 2.0f) :
                        glm::ortho(-camera.GetOrthographicSize() * aspect * 0.5f,
                                   camera.GetOrthographicSize() * aspect * 0.5f,
                                   -camera.GetOrthographicSize() * 0.5f,
                                   camera.GetOrthographicSize() * 0.5f,
                                   0.0f,
                                   2.0f);
                const glm::mat4 inverse = world * glm::inverse(projection);
                glm::vec3       corners[8];
                for (int i = 0; i < 8; ++i)
                {
                    const glm::vec4 point = inverse * glm::vec4((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1, 1);
                    corners[i]            = glm::vec3(point) / point.w;
                }
                const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                          {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& edge : edges)
                    segment(corners[edge[0]], corners[edge[1]], color);
            }
            if (entity.HasComponent<DirLightComponent>())
            {
                // The light's direction, rotated with the entity.
                glm::vec3 direction = entity.GetComponent<DirLightComponent>().Direction;
                direction = glm::length(direction) > 1e-4f ? glm::normalize(glm::mat3(world) * direction) : glm::vec3(0, -1, 0);
                segment(origin, origin + direction * 1.5f, color);
            }
            ImVec2 center;
            if (!ToScreen(origin, center))
                return;
            draw->AddCircleFilled(center, 12.0f, chosen ? IM_COL32(90, 55, 10, 230) : IM_COL32(25, 25, 25, 200));
            draw->AddCircle(center, 12.0f, color, 0, 1.5f);
            const ImVec2 size = ImGui::CalcTextSize(icon);
            draw->AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, color, icon);
            m_Icons.emplace_back(center, entity.GetUUID());
        };
        for (Entity entity : m_Scene->GetRootEntities())
            visit(entity);
        draw->PopClipRect();
    }

    void ViewportPanel::DrawViewAxes()
    {
        using EditorTheme::Px;
        m_CubeMin = m_CubeMax = {};
        const ImGuiIO& io = ImGui::GetIO();
        // An alignment in progress turns the camera a step each frame.
        if (m_ViewAnimation)
        {
            constexpr float ViewAnimationSeconds = 0.25f;
            m_ViewAnimation->Time += io.DeltaTime / ViewAnimationSeconds;
            if (m_ViewAnimation->Time >= 1.0f)
            {
                static const std::pair<EditorCamera::View, const char*> names[] = {
                    {EditorCamera::View::Top, "Top"},     {EditorCamera::View::Bottom, "Bottom"},
                    {EditorCamera::View::Front, "Front"}, {EditorCamera::View::Back, "Back"},
                    {EditorCamera::View::Right, "Right"}, {EditorCamera::View::Left, "Left"}};
                m_Camera.SetView(m_ViewAnimation->Target);
                for (const auto& [view, name] : names)
                    if (view == m_ViewAnimation->Target)
                        m_ViewName = name;
                m_ViewAnimation.reset();
            }
            else
            {
                const auto [yaw, pitch] = Gizmo::InterpolateView(m_ViewAnimation->From, m_ViewAnimation->To, m_ViewAnimation->Time);
                m_Camera.SetRotation(yaw, pitch);
            }
        }
        if (UsesSceneCamera())
            return;
        const float     radius = Px(36.0f), ball = Px(9.0f);
        const glm::vec2 centre(m_Max.x - Px(10.0f) - radius - ball - Px(4.0f), m_Min.y + Px(50.0f) + radius + ball + Px(4.0f));
        const float     reach = radius + ball + Px(4.0f);
        m_CubeMin             = {centre.x - reach, centre.y - reach};
        m_CubeMax             = {centre.x + reach, centre.y + reach};
        const auto      balls = Gizmo::ViewAxisBalls(m_Camera.GetViewMatrix(), centre, radius);
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        const bool      over =
            ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(m_CubeMin, m_CubeMax, false) &&
            glm::length(mouse - centre) <= reach && !IsGizmoDragging() && m_Navigation == Navigation::None;
        const int hovered = over && !m_AxisOrbiting ? Gizmo::PickViewAxisBall(balls, mouse, ball) : -1;

        // Press: a ball, or the background (which orbits when dragged).
        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_AxisPressActive = true;
            m_AxisPressed     = hovered;
            m_AxisPress       = mouse;
            m_AxisOrbiting    = false;
        }
        if (m_AxisPressActive)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (!m_AxisOrbiting && glm::length(mouse - m_AxisPress) > Px(3.0f))
                    m_AxisOrbiting = true, m_ViewAnimation.reset();
                if (m_AxisOrbiting)
                    m_Camera.Orbit({io.MouseDelta.x, io.MouseDelta.y});
            }
            else
            {
                if (!m_AxisOrbiting && m_AxisPressed >= 0 && hovered == m_AxisPressed)
                {
                    // Looks from the ball's side; the side already looked from flips to the opposite.
                    int  axis     = balls[size_t(m_AxisPressed)].Axis;
                    bool negative = balls[size_t(m_AxisPressed)].Negative;
                    if (Gizmo::IsLookingFrom(m_Camera, axis, negative))
                        negative = !negative;
                    const auto target = Gizmo::BallView(axis, negative);
                    m_ViewAnimation   = ViewAnimation {{m_Camera.GetYaw(), m_Camera.GetPitch()}, EditorCamera::ViewAngles(target), 0.0f, target};
                }
                m_AxisPressActive = m_AxisOrbiting = false;
                m_AxisPressed     = -1;
            }
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (over || m_AxisPressActive)
            draw->AddCircleFilled({centre.x, centre.y}, reach, IM_COL32(255, 255, 255, 28), 48);
        static const ImU32 colors[3]   = {IM_COL32(235, 64, 70, 255), IM_COL32(115, 205, 46, 255), IM_COL32(62, 132, 245, 255)};
        static const char* labels[3]   = {"X", "Y", "Z"};
        for (int i = 0; i < 6; ++i)
        {
            const auto&  b     = balls[size_t(i)];
            const ImVec2 at {b.Screen.x, b.Screen.y};
            const ImU32  color = colors[b.Axis];
            if (!b.Negative)
                draw->AddLine({centre.x, centre.y}, at, color, Px(2.0f));
            if (b.Negative)
            {
                // Negative directions: a darker disc with the axis colour as its rim.
                draw->AddCircleFilled(at, ball * 0.85f, (color & 0x00FFFFFF) | (110u << 24), 24);
                draw->AddCircle(at, ball * 0.85f, color, 24, Px(1.5f));
            }
            else
            {
                draw->AddCircleFilled(at, ball, color, 24);
                const ImVec2 size = ImGui::CalcTextSize(labels[b.Axis]);
                draw->AddText({at.x - size.x * 0.5f, at.y - size.y * 0.5f}, IM_COL32(20, 20, 24, 255), labels[b.Axis]);
            }
            if (i == hovered)
                draw->AddCircle(at, ball + Px(1.5f), IM_COL32(255, 255, 255, 230), 24, Px(2.0f));
        }
        if (hovered >= 0)
        {
            const auto& b = balls[size_t(hovered)];
            ImGui::SetTooltip("View from %s%s (click again for the opposite side; drag to orbit)", b.Negative ? "-" : "+",
                              labels[b.Axis]);
        }
    }

    void ViewportPanel::DrawStatsOverlay()
    {
        if (!m_ShowStats)
            return;
        const auto&    stats = m_Client->GetLastResult().Stats;
        const ImGuiIO& io    = ImGui::GetIO();
        OverlayGroup({m_Min.x + EditorTheme::Px(10.0f), m_Max.y - EditorTheme::Px(96.0f)}, [&] {
            ImGui::Text("%.0f FPS  %.2f ms", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
            ImGui::Text("%u x %u", m_Size.x, m_Size.y);
            ImGui::Text("%u draw calls", stats.DrawCalls);
            ImGui::Text("%llu triangles", static_cast<unsigned long long>(stats.Triangles));
        });
    }

    // ---------------------------------------------------------------------- gizmo and toolbar

    Gizmo::Settings ViewportPanel::GizmoSettings() const
    {
        Gizmo::Settings settings;
        settings.Size          = EditorTheme::Px(m_GizmoSize);
        settings.PickRadius    = EditorTheme::Px(10.0f);
        settings.Snap          = m_GizmoSnap;
        settings.TranslateStep = m_SnapMove;
        settings.RotateStep    = m_SnapRotate;
        settings.ScaleStep     = m_SnapScale;
        return settings;
    }

    Gizmo::View ViewportPanel::GizmoView() const
    {
        return Gizmo::View::From(m_Camera, {m_Min.x, m_Min.y}, {m_Max.x - m_Min.x, m_Max.y - m_Min.y});
    }

    void ViewportPanel::BeginHandles()
    {
        const ImGuiIO& io = ImGui::GetIO();
        Gizmo::Input   input;
        input.Mouse      = {io.MousePos.x, io.MousePos.y};
        input.Down       = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        input.Pressed    = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        input.Released   = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        input.Cancel     = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        input.InvertSnap = io.KeyCtrl;
        input.Alt        = io.KeyAlt;
        // Alt starts orbiting, so it only changes a drag already running (a box face's other side).
        input.Allowed = m_ImageHovered && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() && !io.KeyAlt &&
                        m_Navigation == Navigation::None && !m_AxisPressActive &&
                        !ImGui::IsMouseHoveringRect(m_CubeMin, m_CubeMax, false) && !m_Modal.IsActive() &&
                        !m_Gizmo.IsDragging() && !m_MeshTool.GetGizmo().IsDragging() && !UsesSceneCamera();
        m_Handles.Begin(GizmoView(), input, GizmoSettings());
        m_HandleInput         = input;
        m_HandlesWereDragging = m_Handles.IsDragging() || (m_VfxHandles && m_VfxHandles->IsDragging());
        m_VfxDeclared         = false;
    }

    void ViewportPanel::VfxHandles(Entity selected)
    {
        if (!m_VfxHandles || !selected || !selected.HasComponent<ParticleSystemComponent>() || UsesSceneCamera())
            return;
        const auto&             c = selected.GetComponent<ParticleSystemComponent>();
        VfxGizmos::Pointer      pointer {GizmoView(), m_HandleInput, GizmoSettings()};
        // ImGui's Shift and the axis keys choose a point handle's constraint as the drag starts.
        pointer.Input.Shift   = ImGui::GetIO().KeyShift;
        pointer.Input.AxisKey = ImGui::IsKeyDown(ImGuiKey_X) ? 0 : ImGui::IsKeyDown(ImGuiKey_Y) ? 1 : ImGui::IsKeyDown(ImGuiKey_Z) ? 2 : -1;
        m_HandleInput         = pointer.Input;
        m_VfxHandles->Declare(m_Handles, pointer, selected.GetUUID(), c.System, m_Scene->GetWorldTransform(selected));
        m_VfxDeclared = true;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(m_Min, m_Max, true);
        for (const auto& line : m_VfxHandles->GetGizmos().Lattice())
            OverlaySegment(draw, line.From, line.To, line.Color, 1.0f);
        draw->PopClipRect();
    }

    void ViewportPanel::EndVfxHandles(std::vector<GizmoDraw>& draws)
    {
        if (!m_VfxHandles)
            return;
        if (!m_VfxDeclared)
        {
            // Nothing declared this frame (deselected, playing): drop a drag of it.
            if (m_VfxHandles->IsDragging())
                m_VfxHandles->Reset();
            return;
        }
        m_VfxHandles->Update(m_Handles, {GizmoView(), m_HandleInput, GizmoSettings()});
        m_VfxHandles->GetGizmos().AppendDraws(draws);
        const auto& gizmos = m_VfxHandles->GetGizmos();
        DrawGizmoReadout(gizmos.Readout(), gizmos.ReadoutPosition());
        if (!gizmos.Message().empty() && gizmos.IsDragging())
            ImGui::SetTooltip("%s", gizmos.Message().c_str());
    }

    bool ViewportPanel::StartModal(Gizmo::Operation op, bool extrude)
    {
        Entity          selected = m_Hierarchy.GetSelected();
        const auto      view     = GizmoView();
        const glm::vec2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        Gizmo::Pivot    pivot;
        glm::mat3       local(1.0f);
        if (m_MeshTool.IsEditing(selected))
        {
            const auto first = m_MeshTool.SelectionPivot(selected, EditView(), m_Orientation);
            const auto own   = m_MeshTool.SelectionPivot(selected, EditView(), Gizmo::Orientation::Local);
            if (!first || !own || !m_MeshTool.BeginTransform(selected, extrude))
                return false;
            pivot         = *first;
            local         = own->Axes;
            m_ModalTarget = ModalTarget::Mesh;
        }
        else
        {
            const auto targets = m_Hierarchy.TopMostSelected();
            if (extrude || targets.empty())
                return false;
            pivot = Gizmo::SelectionPivot(*m_Scene, targets, selected, m_PivotMode, m_Orientation, view);
            local = Gizmo::ResolveAxes(Gizmo::Orientation::Local, m_Scene->GetWorldTransform(selected), view);
            m_GizmoTargets.Begin(*m_Scene, targets);
            m_ModalTarget = ModalTarget::Entities;
        }
        m_Gizmo.Reset();
        m_Modal.Start(op, view, pivot, m_Orientation, local, GizmoSettings(), mouse);
        return true;
    }

    bool ViewportPanel::UpdateModal(std::vector<GizmoDraw>& draws)
    {
        const ImGuiIO& io       = ImGui::GetIO();
        Entity         selected = m_Hierarchy.GetSelected();
        const bool     editing  = m_MeshTool.IsEditing(selected);
        if (!m_Modal.IsActive())
        {
            const bool ready = selected && m_ImageHovered && !UsesSceneCamera() && !IsPaintingCloth() && !io.WantTextInput &&
                               !io.KeyCtrl && !io.KeyAlt && m_Navigation == Navigation::None && !ImGui::IsAnyItemActive() &&
                               !m_Gizmo.IsDragging() && !m_MeshTool.GetGizmo().IsDragging() && !m_Handles.IsDragging() &&
                               !ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (!ready)
                return false;
            const auto mode    = m_MeshTool.GetMode();
            bool       started = false;
            if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                started = StartModal(Gizmo::Operation::Translate, false);
            else if (!m_RPicksScaleTool && ImGui::IsKeyPressed(ImGuiKey_R, false))
                started = StartModal(Gizmo::Operation::Rotate, false);
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                started = StartModal(Gizmo::Operation::Scale, false);
            else if (editing && (mode == MeshEditTool::Mode::Face || mode == MeshEditTool::Mode::Edge) &&
                     ImGui::IsKeyPressed(ImGuiKey_E, false))
                started = StartModal(Gizmo::Operation::Translate, true); // Extrude, then move.
            if (!started)
                return false;
            m_ModalThisFrame = true;
            m_Modal.AppendDraws(draws);
            return true;
        }
        m_ModalThisFrame = true;
        // The selection or the editor view went away: put things back.
        const bool lost  = !selected || UsesSceneCamera() || (m_ModalTarget == ModalTarget::Mesh && !editing);
        const auto input = Widgets::ModalTransformInput(lost);
        const auto  event = m_Modal.Update(GizmoView(), input);
        const auto& delta = m_Modal.GetDelta();
        switch (event)
        {
            case Gizmo::Event::Dragged:
            case Gizmo::Event::Committed:
                if (m_ModalTarget == ModalTarget::Entities)
                    m_GizmoTargets.Apply(delta);
                else if (m_ModalTarget == ModalTarget::Mesh)
                    m_MeshTool.ApplyDelta(selected, EditView(), delta);
                if (event == Gizmo::Event::Committed)
                {
                    // One undo step: the editor records once the keys and buttons are up.
                    m_GizmoTargets.Clear();
                    if (m_ModalTarget == ModalTarget::Mesh)
                        m_MeshTool.EndTransform(selected, true);
                }
                break;
            case Gizmo::Event::Cancelled:
                if (m_ModalTarget == ModalTarget::Entities && m_GizmoTargets.IsActive())
                    m_GizmoTargets.Restore();
                m_GizmoTargets.Clear();
                if (m_ModalTarget == ModalTarget::Mesh)
                    m_MeshTool.EndTransform(selected, false);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    m_SwallowRightClick = true;
                break;
            default:
                break;
        }
        if (!m_Modal.IsActive())
            m_ModalTarget = ModalTarget::None;
        m_Modal.AppendDraws(draws);
        return true;
    }

    void ViewportPanel::DrawModalStatus()
    {
        // The status line along the bottom of the view, like Blender's header text.
        Widgets::DrawModalTransformStatus(m_Modal, {m_Min.x + EditorTheme::Px(10.0f), m_Max.y - EditorTheme::Px(10.0f)},
                                          EditorTheme::Px(1.0f));
    }

    void ViewportPanel::DrawGizmo()
    {
        m_SwallowRightClick = false;
        // A right click that cancelled a handle drag does not start looking around either.
        if (m_HandlesWereDragging && !m_Handles.IsDragging() && !(m_VfxHandles && m_VfxHandles->IsDragging()) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            m_SwallowRightClick = true;
        std::vector<GizmoDraw> draws;
        const bool             modal    = UpdateModal(draws);
        Entity                 selected = m_Hierarchy.GetSelected();
        // While a modal transform runs the gizmo hides, as in Blender.
        const bool shown = !modal && selected && m_Tool != Tool::Select && !UsesSceneCamera() && !IsPaintingCloth();
        if (!shown)
        {
            // The selection went away (or the tool changed) mid-drag: put things back.
            if (!modal)
            {
                if (m_Gizmo.IsDragging() && m_GizmoTargets.IsActive())
                    m_GizmoTargets.Restore();
                m_GizmoTargets.Clear();
            }
            m_Gizmo.Reset();
            m_Handles.End();
            EndVfxHandles(draws);
            m_Handles.AppendDraws(draws);
            m_Client->SetGizmo(std::move(draws));
            DrawModalStatus();
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        Gizmo::Input   input;
        input.Mouse      = {io.MousePos.x, io.MousePos.y};
        input.Down       = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        input.Pressed    = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        input.Released   = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        input.Cancel     = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        input.InvertSnap = io.KeyCtrl;
        // A toolbar control, the axis widget or navigation has the pointer; a handle being
        // dragged, or winning the hover, too (resolved below).
        input.Allowed = m_ImageHovered && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() && !io.KeyAlt &&
                        m_Navigation == Navigation::None && !m_AxisPressActive &&
                        !ImGui::IsMouseHoveringRect(m_CubeMin, m_CubeMax, false);
        const bool dragging = IsGizmoDragging();
        const auto mode     = m_Tool == Tool::Rotate    ? Gizmo::Mode::Rotate :
                              m_Tool == Tool::Scale     ? Gizmo::Mode::Scale :
                              m_Tool == Tool::Transform ? Gizmo::Mode::Combined :
                                                          Gizmo::Mode::Translate;
        const auto settings = GizmoSettings();
        const auto view     = GizmoView();
        const Gizmo::TransformGizmo* gizmo = &m_Gizmo;
        if (m_MeshTool.IsEditing(selected))
        {
            m_Handles.End();
            EndVfxHandles(draws);
            input.Allowed = input.Allowed && !m_Handles.IsDragging();
            // In element modes the gizmo moves the selected vertices, edges or faces.
            m_MeshTool.Manipulate(selected, EditView(), mode, m_Orientation, settings, input, io.KeyShift);
            m_MeshTool.AppendGizmoDraws(draws);
            gizmo = &m_MeshTool.GetGizmo();
        }
        else
        {
            // Entities scale along their own axes, so the scale handles follow the active entity.
            const auto orientation = mode == Gizmo::Mode::Scale ? Gizmo::Orientation::Local : m_Orientation;
            const auto targets     = m_Hierarchy.TopMostSelected();
            const auto pivot       = Gizmo::SelectionPivot(*m_Scene, targets, selected, m_PivotMode, orientation, view);
            // The gizmo takes part in the handles' hover resolution: one of them is hot.
            if (!m_Gizmo.IsDragging() && input.Allowed)
            {
                const auto hit = m_Gizmo.Pick(view, pivot, mode, settings, input.Mouse);
                if (hit.Id != Gizmo::Handle::None)
                    m_Handles.Offer(GizmoHandleId, hit.Depth, hit.Distance, Gizmo::HandlePriority(hit.Id));
            }
            m_Handles.End();
            EndVfxHandles(draws);
            if (!m_Gizmo.IsDragging())
                input.Allowed = input.Allowed && !m_Handles.IsDragging() && !(m_VfxHandles && m_VfxHandles->IsDragging()) &&
                                (m_Handles.Hot() == 0 || m_Handles.Hot() == GizmoHandleId);
            switch (m_Gizmo.Update(view, pivot, mode, settings, input))
            {
                case Gizmo::Event::Began:
                    m_GizmoTargets.Begin(*m_Scene, targets);
                    break;
                case Gizmo::Event::Dragged:
                    m_GizmoTargets.Apply(m_Gizmo.GetDelta());
                    break;
                case Gizmo::Event::Committed:
                    // The editor records the step once the button is up (EditorLayer::TrackSceneEdits).
                    m_GizmoTargets.Apply(m_Gizmo.GetDelta());
                    m_GizmoTargets.Clear();
                    break;
                case Gizmo::Event::Cancelled:
                    m_GizmoTargets.Restore();
                    m_GizmoTargets.Clear();
                    break;
                case Gizmo::Event::None:
                    break;
            }
            m_Gizmo.AppendDraws(draws);
        }
        m_Handles.AppendDraws(draws);
        if (dragging && !IsGizmoDragging() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            m_SwallowRightClick = true;
        m_Client->SetGizmo(std::move(draws));
        DrawGizmoReadout(gizmo->Readout(), gizmo->ReadoutPosition());
    }

    void ViewportPanel::DrawGizmoReadout(const std::string& text, glm::vec2 at)
    {
        if (text.empty())
            return;
        using EditorTheme::Px;
        ImDrawList*  draw = ImGui::GetWindowDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pad {Px(6.0f), Px(3.0f)};
        draw->AddRectFilled({at.x, at.y}, {at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2}, IM_COL32(18, 18, 22, 215),
                            Px(4.0f));
        draw->AddText({at.x + pad.x, at.y + pad.y}, IM_COL32(240, 240, 240, 255), text.c_str());
    }

    void ViewportPanel::DrawToolbar()
    {
        using EditorTheme::Px;
        const float top  = m_Min.y + Px(8.0f);
        const float left = m_Min.x + Px(10.0f);
        // The right-hand group starts where last frame's width puts it. The left groups collapse
        // to fit before it: first tight (icons only, closer together), then the element modes and
        // the secondary-motion preview move into an overflow menu.
        const float rightStart = m_Max.x - Px(10.0f) - m_RightToolbarWidth;
        const float available  = rightStart - Px(6.0f) - left;
        int         level      = 0;
        while (level < 2 && m_ToolbarWidths[level] > available)
            ++level;
        const bool  tight = level >= 1;
        const float gap   = tight ? Px(3.0f) : Px(6.0f);

        // Left: transform tools; space and snapping; editable-mesh element modes.
        float x = OverlayGroup({left, top}, [&] {
            const std::pair<Tool, std::pair<const char*, const char*>> tools[] = {
                {Tool::Select, {ICON_FA_POINTER, "Select (Q)"}},
                {Tool::Move, {ICON_FA_ARROWS_MOVE, "Move (W; G over the view moves from the pointer)"}},
                {Tool::Rotate, {ICON_FA_ROTATE, "Rotate (E; R over the view rotates from the pointer)"}},
                {Tool::Scale, {ICON_FA_SCALE, "Scale (R away from the view; S over it scales from the pointer)"}},
                {Tool::Transform, {ICON_FA_EXPAND, "Move, rotate and scale (Y)"}},
            };
            bool first = true;
            for (const auto& [type, info] : tools)
            {
                if (!first)
                    ImGui::SameLine();
                first = false;
                if (ToolButton(info.first, info.second, m_Tool == type, !IsGizmoDragging()))
                    m_Tool = type;
            }
        }) + gap;
        x = OverlayGroup({x, top}, [&] {
            // Orientation: local, world, view (each click moves to the next).
            const auto orientation = m_Orientation;
            const char* icon  = orientation == Gizmo::Orientation::Local ? ICON_FA_CUBE :
                                orientation == Gizmo::Orientation::World ? ICON_FA_GLOBE :
                                                                           ICON_FA_VIDEO;
            const char* label = orientation == Gizmo::Orientation::Local ? "Local" :
                                orientation == Gizmo::Orientation::World ? "World" :
                                                                           "View";
            const char* tip   = orientation == Gizmo::Orientation::Local ? "Local axes (click for world)" :
                                orientation == Gizmo::Orientation::World ? "World axes (click for view)" :
                                                                           "View axes (click for local)";
            const bool clicked = tight ? ToolButton(icon, tip) : Widgets::TextButton(icon, label, tip);
            if (clicked && !IsGizmoDragging())
                m_Orientation = orientation == Gizmo::Orientation::Local ? Gizmo::Orientation::World :
                                orientation == Gizmo::Orientation::World ? Gizmo::Orientation::View :
                                                                           Gizmo::Orientation::Local;
            ImGui::SameLine();
            // Pivot: the selection's median point or the active entity.
            const bool median = m_PivotMode == Gizmo::PivotMode::Median;
            if (ToolButton(median ? ICON_FA_CIRCLE_DOT : ICON_FA_THUMBTACK,
                           median ? "Pivot: the selection's median point (click for the active entity)" :
                                    "Pivot: the active entity (click for the median point)",
                           false,
                           !IsGizmoDragging()))
                m_PivotMode = median ? Gizmo::PivotMode::Active : Gizmo::PivotMode::Median;
            ImGui::SameLine();
            if (ToolButton(ICON_FA_MAGNET, "Snap (or hold Ctrl while dragging)", m_GizmoSnap))
                m_GizmoSnap = !m_GizmoSnap;
            ImGui::SameLine();
            float*      snapStep = m_Tool == Tool::Rotate ? &m_SnapRotate :
                                   m_Tool == Tool::Scale  ? &m_SnapScale :
                                                            &m_SnapMove;
            const char* format   = m_Tool == Tool::Rotate ? "%.0f°" : "%.2f";
            ImGui::SetNextItemWidth(tight ? Px(48.0f) : Px(64.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {tight ? Px(4.0f) : Px(6.0f), (Px(26.0f) - ImGui::GetFontSize()) * 0.5f});
            ImGui::DragFloat("##snap", snapStep, 0.05f, 0.01f, 360.0f, format);
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Snap step for the current tool");
        }) + gap;
        // Secondary motion: spring bones and cloth run on the edit scene's animated pose.
        const auto motion = [&] {
            const bool previewing = m_Scene->IsSecondaryMotionPreviewEnabled();
            if (ToolButton(ICON_FA_WIND,
                           previewing ? "Preview secondary motion (on): springs and cloth follow the animated pose" :
                                        "Preview secondary motion: run springs and cloth on the edit scene",
                           previewing,
                           !m_Playing))
                SecondaryMotionEditing::SetPreview(*m_Scene, !previewing);
            ImGui::SameLine();
            if (ToolButton(ICON_FA_ROTATE_LEFT, "Reset the secondary-motion preview", false, previewing && !m_Playing))
                m_Scene->ResetSecondaryMotion();
        };
        float leftEnd = x;
        if (level < 2)
        {
            x       = OverlayGroup({x, top}, [&] { m_MeshTool.DrawModeButtons(m_Hierarchy.GetSelected()); }) + gap;
            leftEnd = OverlayGroup({x, top}, motion);
        }
        else
            leftEnd = OverlayGroup({x, top}, [&] {
                if (ToolButton(ICON_FA_ELLIPSIS_VERTICAL, "More: element modes and the secondary-motion preview"))
                    ImGui::OpenPopup("##ViewportMore");
                if (ImGui::BeginPopup("##ViewportMore"))
                {
                    m_MeshTool.DrawModeButtons(m_Hierarchy.GetSelected());
                    motion();
                    ImGui::EndPopup();
                }
            });
        m_ToolbarWidths[level] = leftEnd - left;
        m_ToolbarLayout        = {leftEnd, rightStart, level};
        if (!UsesSceneCamera())
        {
            if (IsPaintingCloth())
                m_ClothPaint.DrawPanel({m_Min.x + Px(10.0f), m_Min.y + Px(48.0f)},
                                       ClothPaintTool::View {m_Min, m_Max, &m_Camera, m_Scene.get()});
            else
                m_MeshTool.DrawPanel(m_Hierarchy.GetSelected(), {m_Min.x + Px(10.0f), m_Min.y + Px(48.0f)});
        }

        // Right: camera view, framing and fly speed, display options. Right-aligned with
        // last frame's width, since the labels change.
        const float right = m_Max.x - Px(10.0f);
        const float start = right - m_RightToolbarWidth;
        const float end   = OverlayGroup({start, top}, [&] {
            const char* view = m_Camera.IsOrthographic() ? m_ViewName : "Perspective";
            if (Widgets::TextButton(m_Camera.IsOrthographic() ? ICON_FA_VECTOR_SQUARE : ICON_FA_VIDEO, view, "Camera view",
                                    false, true))
                ImGui::OpenPopup("##CameraView");
            if (ImGui::BeginPopup("##CameraView"))
            {
                const std::pair<EditorCamera::View, const char*> views[] = {
                    {EditorCamera::View::Perspective, "Perspective"},
                    {EditorCamera::View::Top, "Top"},
                    {EditorCamera::View::Bottom, "Bottom"},
                    {EditorCamera::View::Front, "Front"},
                    {EditorCamera::View::Back, "Back"},
                    {EditorCamera::View::Right, "Right"},
                    {EditorCamera::View::Left, "Left"},
                };
                for (const auto& [choice, name] : views)
                    if (ImGui::MenuItem(name))
                    {
                        m_Camera.SetView(choice);
                        m_ViewName = name;
                    }
                ImGui::Separator();
                bool orthographic = m_Camera.IsOrthographic();
                if (ImGui::MenuItem("Orthographic", nullptr, &orthographic))
                    m_Camera.SetOrthographic(orthographic);
                // Exposure, tone mapping and bloom are the scene's (Properties > Post Processing).
                ImGui::Separator();
                if (PropertyGrid::Begin("##EditorCameraPost", 96.0f))
                {
                    auto antiAliasing = m_Camera.GetAntiAliasing();
                    if (DrawAntiAliasing(antiAliasing))
                        m_Camera.SetAntiAliasing(antiAliasing);
                    PropertyGrid::End();
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ToolButton(ICON_FA_CROSSHAIRS, "Frame the selection (F)", false, bool(m_Hierarchy.GetSelected())))
                FocusSelection();
            ImGui::SameLine();
            float speed = m_Camera.GetFlySpeed();
            ImGui::SetNextItemWidth(Px(100.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {Px(6.0f), (Px(26.0f) - ImGui::GetFontSize()) * 0.5f});
            const bool speedChanged = ImGui::SliderFloat("##speed",
                                   &speed,
                                   EditorCamera::MinFlySpeed,
                                   EditorCamera::MaxFlySpeed,
                                   "%.1f m/s",
                                   ImGuiSliderFlags_Logarithmic);
            ImGui::PopStyleVar();
            if (speedChanged)
                m_Camera.SetFlySpeed(speed);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fly speed (right mouse + WASD/QE, Shift for faster); the wheel adjusts it while flying");
            ImGui::SameLine();
            const char* modeNames[] = {"Lit", "Base color", "Normals", "Wireframe", "Cascades"};
            const int   modeIndex   = std::clamp(int(m_ViewMode), 0, 4);
            if (Widgets::TextButton(ICON_FA_EYE, modeNames[modeIndex], "Display mode and view options", false, true))
                ImGui::OpenPopup("##ViewOptions");
            if (ImGui::BeginPopup("##ViewOptions"))
            {
                ImGui::TextDisabled("Display");
                const std::pair<SceneViewMode, const char*> modes[] = {{SceneViewMode::Lit, "Lit"},
                                                                       {SceneViewMode::BaseColor, "Base color"},
                                                                       {SceneViewMode::Normals, "Normals"},
                                                                       {SceneViewMode::Wireframe, "Wireframe"},
                                                                       {SceneViewMode::ShadowCascades, "Shadow cascades"}};
                for (const auto& [mode, name] : modes)
                    if (ImGui::RadioButton(name, m_ViewMode == mode))
                        m_ViewMode = mode;
                ImGui::Separator();
                auto  resource = m_Scene->GetRenderWorld().Settings;
                auto& state    = resource.Piplinestate;
                const auto label = [](const char* icon, const char* text) { return std::string(icon) + "  " + text; };
                ImGui::Checkbox(label(ICON_FA_LIGHTBULB, "Light and camera icons").c_str(), &m_ShowIcons);
                ImGui::Checkbox(label(ICON_FA_CHART_LINE, "Statistics").c_str(), &m_ShowStats);
                ImGui::SetNextItemWidth(EditorTheme::Px(120.0f));
                ImGui::SliderFloat("Gizmo size", &m_GizmoSize, 60.0f, 220.0f, "%.0f px");
                ImGui::Checkbox(label(ICON_FA_SCALE, "R picks the scale tool over the view").c_str(), &m_RPicksScaleTool);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Off: R over the view rotates from the pointer (G moves, S scales, as in Blender).\n"
                                      "On: R is the scale tool's key everywhere; G and S stay modal.");
                ImGui::Checkbox(label(ICON_FA_BORDER_ALL, "Grid").c_str(), &state.EnableGridShader);
                if (state.EnableGridShader && PropertyGrid::Begin("##Grid", 96.0f))
                {
                    PropertyGrid::Float("Cell size", m_Grid.CellSize, 0.01f, 0.01f, 1000.0f, "%.2f m");
                    int major = int(m_Grid.MajorEvery);
                    PropertyGrid::Row("Major every", "Every Nth line is a major line");
                    if (ImGui::DragInt("##major", &major, 0.1f, 2, 100))
                        m_Grid.MajorEvery = uint32_t(std::clamp(major, 2, 100));
                    float opacity = m_Grid.MajorColor.a / GridSettings {}.MajorColor.a;
                    if (PropertyGrid::Float("Opacity", opacity, 0.01f, 0.0f, 2.0f))
                    {
                        // One control scales every line's opacity from its default.
                        const GridSettings defaults;
                        m_Grid.MinorColor.a = std::min(1.0f, defaults.MinorColor.a * opacity);
                        m_Grid.MajorColor.a = std::min(1.0f, defaults.MajorColor.a * opacity);
                        m_Grid.XAxisColor.a = std::min(1.0f, defaults.XAxisColor.a * opacity);
                        m_Grid.YAxisColor.a = std::min(1.0f, defaults.YAxisColor.a * opacity);
                        m_Grid.ZAxisColor.a = std::min(1.0f, defaults.ZAxisColor.a * opacity);
                    }
                    PropertyGrid::Float("Line width", m_Grid.LineWidth, 0.05f, 0.5f, 4.0f, "%.2f px");
                    PropertyGrid::Float("Fade distance", m_Grid.FadeDistance, 1.0f, 5.0f, 10000.0f, "%.0f m");
                    PropertyGrid::End();
                }
                ImGui::Checkbox(label(ICON_FA_DRAW_POLYGON, "Outline (all models)").c_str(), &state.EnableOutline);
                ImGui::Checkbox(label(ICON_FA_VECTOR_SQUARE, "Stencil (all models)").c_str(), &state.EnableStencil);
                ImGui::Checkbox(label(ICON_FA_CROSSHAIRS, "Debug normals").c_str(), &state.EnableDebugNormal);
                m_Scene->GetRenderWorld().Settings = resource;
                ImGui::Separator();
                DrawPhysicsMenu();
                if (m_Playing)
                {
                    ImGui::Separator();
                    ImGui::Checkbox("Editor camera while playing", &m_UseEditorCameraInPlay);
                    // The game view's size, for checking screen UI at other resolutions.
                    ImGui::SetNextItemWidth(EditorTheme::Px(180.0f));
                    if (ImGui::BeginCombo("Game resolution", GameResolutions()[size_t(m_GameResolution)].Name))
                    {
                        for (size_t i = 0; i < GameResolutions().size(); ++i)
                            if (ImGui::Selectable(GameResolutions()[i].Name, int(i) == m_GameResolution))
                                m_GameResolution = int(i);
                        ImGui::EndCombo();
                    }
                }
                ImGui::EndPopup();
            }
        });
        m_RightToolbarWidth = end - start;
    }
} // namespace Kans
