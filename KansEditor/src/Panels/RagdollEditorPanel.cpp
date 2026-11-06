#include "RagdollEditorPanel.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Scene.h"
#include "Components/ComponentEditors.h"
#include "Physics/ShapeHandles.h"
#include "UI/EditorIcons.h"
#include "UI/ModalInput.h"
#include "UI/PropertyGrid.h"
#include "ViewportClient.h"

namespace Kans
{
    namespace Edit = RagdollEditing;

    RagdollPreviewRenderer::RagdollPreviewRenderer()  = default;
    RagdollPreviewRenderer::~RagdollPreviewRenderer() = default;

    void RagdollPreviewRenderer::SetAssetManager(AssetManager* assets)
    {
        m_Assets = assets;
        if (m_Client)
            m_Client->SetAssetManager(assets);
    }

    void RagdollPreviewRenderer::Reset()
    {
        m_Client.reset();
        m_Assets = nullptr;
    }

    uintptr_t RagdollPreviewRenderer::Render(const Ref<Scene>&             scene,
                                             const EditorCamera&           camera,
                                             glm::uvec2                    size,
                                             const std::vector<GizmoDraw>& gizmo,
                                             const Scene*                  environment)
    {
        if (!scene || !m_Assets)
            return 0;
        if (!m_Client)
        {
            m_Client = std::make_unique<ViewportClient>();
            m_Client->SetAssetManager(m_Assets);
        }
        if (m_Client->GetScene() != scene)
        {
            if (environment && scene->GetRenderWorld().EnvironmentPath.empty())
            {
                scene->GetRenderWorld().EnvironmentPath = environment->GetRenderWorld().EnvironmentPath;
                scene->GetRenderWorld().EnvironmentRoot = environment->GetRenderWorld().EnvironmentRoot;
            }
            m_Client->SetScene(scene);
            // No map to light it (the edited scene has none): the renderer's defaults.
            if (!scene->GetRenderWorld().EnvironmentPath.empty())
                m_Client->PrepareEnvironment(m_EnvironmentSizes);
        }
        // Outside a run the model shows its bind pose.
        if (!scene->GetPhysicsWorld())
            scene->UpdateModelPoses();
        m_Client->Resize(size);
        m_Client->SetGizmo(gizmo);
        m_Client->RenderEditor(camera);
        return m_Client->GetOutput();
    }

    namespace
    {
        ImU32 Color(glm::vec4 c) { return ImGui::ColorConvertFloat4ToU32({c.r, c.g, c.b, c.a}); }

        bool Degrees(const char* label, float& value, float min, float max, const float* reset = nullptr)
        {
            return PropertyGrid::Float(label, value, 0.5f, min, max, "%.1f°", reset);
        }

        bool OffsetRows(Physics::ShapeOffset& offset)
        {
            bool      changed = PropertyGrid::Vec3("Center", offset.Position, 0.0f, 0.005f);
            glm::vec3 euler   = glm::degrees(glm::eulerAngles(offset.Rotation));
            if (PropertyGrid::Vec3("Rotation", euler, 0.0f, 0.5f))
                offset.Rotation = glm::quat(glm::radians(euler)), changed = true;
            return changed;
        }

        // A world matrix's position and rotation written into an offset (its scale kept).
        void SetFromMatrix(Physics::ShapeOffset& offset, const glm::mat4& local)
        {
            offset.Position = glm::vec3(local[3]);
            glm::mat3 rotation(1.0f);
            for (int i = 0; i < 3; ++i)
            {
                const glm::vec3 axis = glm::vec3(local[i]);
                if (glm::length(axis) > 1e-8f)
                    rotation[i] = glm::normalize(axis);
            }
            offset.Rotation = glm::normalize(glm::quat_cast(rotation));
        }
    } // namespace

    // ---------------------------------------------------------------------- opening

    bool RagdollEditorPanel::Open(const std::filesystem::path& file, std::optional<Edit::PreviewSource> source)
    {
        auto loaded = Edit::LoadRagdollFile(file);
        if (const auto* error = std::get_if<AssetError>(&loaded))
        {
            m_Status = error->Message;
            return false;
        }
        auto data = std::get<RagdollData>(std::move(loaded));
        // A character of another skeleton cannot preview this ragdoll.
        if (source && data.Skeleton.IsValid() && source->Skeleton != data.Skeleton)
            source.reset();
        if (!source)
        {
            Edit::PreviewSource found;
            found.Skeleton = data.Skeleton;
            if (m_Assets && data.Skeleton.IsValid())
            {
                auto skeleton = m_Assets->Load<SkeletonAsset>(data.Skeleton);
                if (auto* asset = std::get_if<Ref<const SkeletonAsset>>(&skeleton))
                    found.SkeletonSource = *asset;
                // The model the skeleton came from, to show its meshes.
                if (auto registry = m_Assets->GetRegistry())
                    for (const auto& product : registry->GetAllProducts())
                        if (product.Metadata.Type == AssetType::Model &&
                            product.Metadata.assetID.GetSourceID() == data.Skeleton.GetSourceID() &&
                            product.Target == m_Assets->GetTarget())
                            found.Model = product.Metadata.assetID;
            }
            source = found;
        }
        m_Creating = false;
        m_Session.Begin(file, std::move(data));
        m_Source = *source;
        m_Preview.SetSource(m_Source, m_Assets);
        m_Framed = false;
        m_Handles.Reset();
        m_Modal.Reset();
        m_Selected.clear();
        if (!m_Session.GetData().Bodies.empty())
            Select(m_Session.GetData().Bodies.front().Bone);
        m_Status = m_Source.SkeletonSource ? std::string() : "The skeleton did not load; limits and preview are unavailable";
        m_WindowOpen = true;
        return true;
    }

    void RagdollEditorPanel::BeginCreate(const std::filesystem::path&                      directory,
                                         const std::string&                                stem,
                                         const Edit::PreviewSource&                        source,
                                         Ref<const MeshAsset>                              mesh,
                                         std::function<void(const std::filesystem::path&)> created)
    {
        if (m_Session.IsOpen())
            m_Session.Close();
        m_Creating        = true;
        m_ShowGenerate    = true;
        m_CreateDirectory = directory;
        m_CreateStem      = stem;
        m_Created         = std::move(created);
        m_Source          = source;
        m_Mesh            = std::move(mesh);
        m_Options         = {};
        m_Preview.SetSource(m_Source, m_Assets);
        m_Framed     = false;
        m_WindowOpen = true;
        m_Status.clear();
        m_Selected.clear();
    }

    void RagdollEditorPanel::RequestClose(std::function<void()> closed)
    {
        if (m_Creating)
        {
            m_Creating = m_ShowGenerate = false;
            m_Preview.Reset();
            if (closed)
                closed();
            return;
        }
        m_Session.RequestClose([this, closed] {
            m_Preview.Reset();
            m_Selected.clear();
            if (closed)
                closed();
        });
    }

    void RagdollEditorPanel::Select(const std::string& bone, Selection kind)
    {
        m_Selected  = {bone};
        m_Kind      = kind;
        m_Primitive = 0;
    }

    bool RagdollEditorPanel::Save()
    {
        if (!m_Session.IsOpen())
            return false;
        if (auto error = m_Session.Save())
        {
            m_Status = error->Message;
            return false;
        }
        m_Status = "Saved";
        if (m_Saved)
            m_Saved(m_Session.GetFile());
        return true;
    }

    bool RagdollEditorPanel::Undo()
    {
        m_Handles.Reset();
        return m_Session.Undo();
    }

    bool RagdollEditorPanel::Redo()
    {
        m_Handles.Reset();
        return m_Session.Redo();
    }

    void RagdollEditorPanel::Edited(const RagdollData& data) { m_Session.Change(data); }

    // ---------------------------------------------------------------------- window

    void RagdollEditorPanel::Draw()
    {
        if (!IsOpen())
            return;
        ImGui::SetNextWindowSize({1100.0f, 640.0f}, ImGuiCond_FirstUseEver);
        bool        open  = true;
        std::string title = std::string(ICON_FA_PERSON_FALLING "  Ragdoll Editor");
        if (m_Session.IsOpen())
            title += "  " + m_Session.GetFile().filename().u8string() + (m_Session.IsDirty() ? " *" : "");
        title += "###RagdollEditor";
        if (ImGui::Begin(title.c_str(), &open))
            DrawContents();
        else
            m_Focused = false;
        ImGui::End();
        if (!open)
            RequestClose();
        DrawClosePrompt();
    }

    void RagdollEditorPanel::DrawClosePrompt()
    {
        if (m_Session.IsAskingToClose() && !ImGui::IsPopupOpen("Unsaved ragdoll"))
            ImGui::OpenPopup("Unsaved ragdoll");
        if (ImGui::BeginPopupModal("Unsaved ragdoll", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Save the changes to %s before closing?", m_Session.GetFile().filename().u8string().c_str());
            if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
                m_Session.ResolveClose(UnsavedChanges::Choice::Save, [this] { return Save(); }), ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Don't save"))
                m_Session.ResolveClose(UnsavedChanges::Choice::Discard), ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                m_Session.ResolveClose(UnsavedChanges::Choice::Cancel), ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void RagdollEditorPanel::DrawContents()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        DrawToolbar();
        if (m_Creating || m_ShowGenerate)
            DrawGenerate();
        if (!m_Session.IsOpen() && !m_Creating)
        {
            ImGui::TextDisabled("Open a .ragdoll from the Content Browser or a Ragdoll component.");
            return;
        }
        const float  treeWidth = EditorTheme::Px(220.0f), detailsWidth = EditorTheme::Px(330.0f);
        const ImVec2 avail     = ImGui::GetContentRegionAvail();
        const float  spacing   = ImGui::GetStyle().ItemSpacing.x;
        ImGui::BeginChild("##tree", {treeWidth, avail.y}, ImGuiChildFlags_Borders);
        DrawTree();
        ImGui::EndChild();
        ImGui::SameLine();
        const float viewWidth = std::max(EditorTheme::Px(120.0f), avail.x - treeWidth - detailsWidth - 2 * spacing);
        ImGui::BeginChild("##view", {viewWidth, avail.y}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
        DrawViewport({viewWidth, avail.y});
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##details", {0.0f, avail.y});
        DrawDetails();
        ImGui::EndChild();
        // A finished interaction (a drag, a field, the gizmo, a modal G/R) becomes one undo step.
        if (m_Session.IsOpen() && !m_Handles.IsDragging() && !m_Modal.IsActive() && !ImGui::IsAnyItemActive() &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_Session.Commit();
        if (m_Focused && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && !m_Modal.IsActive())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                Save();
            else if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                ImGui::GetIO().KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
        }
    }

    // ---------------------------------------------------------------------- toolbar

    void RagdollEditorPanel::DrawToolbar()
    {
        const bool open = m_Session.IsOpen();
        ImGui::BeginDisabled(!open || !m_Session.IsDirty());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
            Save();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Session.CanUndo());
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##undo"))
            Undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo (Ctrl+Z)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Session.CanRedo());
        if (ImGui::Button(ICON_FA_ROTATE "##redo"))
            Redo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Redo (Ctrl+Y)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!open || !Skeleton());
        if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES "  Regenerate..."))
            m_ShowGenerate = !m_ShowGenerate;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool simulating = m_Preview.IsSimulating();
        ImGui::BeginDisabled(!open || !Skeleton());
        if (ImGui::Button(simulating ? ICON_FA_STOP "  Reset" : ICON_FA_PLAY "  Simulate"))
        {
            if (simulating)
                m_Preview.Reset();
            else
                m_Preview.Simulate(m_Session.GetData());
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Drops the ragdoll from the current pose onto a ground plane in the preview only");
        ImGui::SameLine();
        bool matching = m_Preview.IsPoseMatching();
        ImGui::BeginDisabled(!m_Preview.CanPoseMatch());
        if (ImGui::Checkbox("Pose matching", &matching))
            m_Preview.SetPoseMatching(matching);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_Preview.CanPoseMatch() ? "Drives follow the animated pose while simulating" :
                                                         "Needs a model with an Animator controller");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CROSSHAIRS "##frame"))
            m_Framed = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Frame the skeleton");
        // Validation: what the engine says about this data.
        if (open)
        {
            const auto problem = Edit::Problem(m_Session.GetData(), Skeleton(), m_Source.Skeleton);
            if (!problem.empty())
                ImGui::TextColored(ComponentEditors::ErrorColor, ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem.c_str());
            for (const auto& error : m_Preview.GetErrors())
                ImGui::TextColored(ComponentEditors::ErrorColor, "Preview: %s", error.c_str());
        }
        if (!m_Status.empty())
            ImGui::TextDisabled("%s", m_Status.c_str());
    }

    // ---------------------------------------------------------------------- generation

    void RagdollEditorPanel::DrawGenerate()
    {
        const SkeletonData* skeleton = Skeleton();
        if (!PropertyGrid::BeginGroup(m_Creating ? "Generate ragdoll" : "Regenerate"))
            return;
        static const RagdollGenerationOptions defaults;
        PropertyGrid::Float("Minimum bone length", m_Options.MinimumBoneLength, 0.001f, 0.0f, 10.0f, "%.3f m",
                            &defaults.MinimumBoneLength);
        int       shape = m_Options.PreferBox ? 1 : 0;
        const int capsule = 0;
        const char* shapes[] = {"Capsules", "Boxes"};
        if (PropertyGrid::Combo("Primitive", shapes, 2, shape, &capsule))
            m_Options.PreferBox = shape == 1;
        PropertyGrid::Float("Total mass", m_Options.TotalMass, 0.5f, 0.1f, 1.0e5f, "%.1f kg", &defaults.TotalMass);
        PropertyGrid::Float("Radius fraction", m_Options.RadiusFraction, 0.005f, 0.01f, 2.0f, "%.3f", &defaults.RadiusFraction);
        Degrees("Twist low", m_Options.DefaultLimits.TwistLow, -Edit::MaxTwist, m_Options.DefaultLimits.TwistHigh);
        Degrees("Twist high", m_Options.DefaultLimits.TwistHigh, m_Options.DefaultLimits.TwistLow, Edit::MaxTwist);
        Degrees("Swing Y", m_Options.DefaultLimits.SwingY, Edit::MinSwing, Edit::MaxSwing);
        Degrees("Swing Z", m_Options.DefaultLimits.SwingZ, Edit::MinSwing, Edit::MaxSwing);
        PropertyGrid::Text("Skip bones",
                           m_Options.SkipBones.empty() ? std::string("None (tick bones in the tree)") :
                                                         std::to_string(m_Options.SkipBones.size()) + " ticked in the tree",
                           m_Options.SkipBones.empty());
        if (PropertyGrid::Row(""))
        {
            ImGui::BeginDisabled(!skeleton);
            if (m_Creating)
            {
                if (ImGui::SmallButton(ICON_FA_PLUS "  Create"))
                {
                    const MeshData* mesh      = m_Mesh ? &m_Mesh->GetData() : nullptr;
                    auto            generated = Edit::Generate(*skeleton, m_Source.Skeleton, mesh, m_Options);
                    if (auto* error = std::get_if<std::string>(&generated))
                        m_Status = *error;
                    else
                    {
                        auto file = Edit::CreateRagdollFile(m_CreateDirectory, m_CreateStem, std::get<RagdollData>(generated));
                        if (auto* error = std::get_if<AssetError>(&file))
                            m_Status = error->Message;
                        else
                        {
                            const auto path    = std::get<std::filesystem::path>(file);
                            const auto created = m_Created;
                            const auto source  = m_Source;
                            m_ShowGenerate     = false;
                            Open(path, source);
                            if (m_Saved)
                                m_Saved(path);
                            if (created)
                                created(path);
                        }
                    }
                }
            }
            else
            {
                const auto regenerate = [&](const std::vector<std::string>& bones) {
                    const MeshData* mesh = m_Mesh ? &m_Mesh->GetData() : nullptr;
                    auto result = Edit::Regenerate(m_Session.GetData(), *skeleton, m_Source.Skeleton, mesh, m_Options, bones);
                    if (auto* error = std::get_if<std::string>(&result))
                        m_Status = *error;
                    else
                        m_Session.Apply(bones.empty() ? "Regenerate all" : "Regenerate selected", [&](RagdollData& d) {
                            d = std::get<RagdollData>(result);
                            return true;
                        });
                };
                if (ImGui::SmallButton("Regenerate all"))
                    regenerate({});
                ImGui::SameLine();
                ImGui::BeginDisabled(m_Selected.empty());
                if (ImGui::SmallButton("Regenerate selected"))
                    regenerate(m_Selected);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Close"))
                    m_ShowGenerate = false;
            }
            ImGui::EndDisabled();
        }
        PropertyGrid::EndGroup();
    }

    // ---------------------------------------------------------------------- tree

    void RagdollEditorPanel::DrawTree()
    {
        const SkeletonData* skeleton = Skeleton();
        if (!skeleton)
        {
            ImGui::TextDisabled("No skeleton");
            if (m_Session.IsOpen())
                for (const auto& body : m_Session.GetData().Bodies)
                    if (ImGui::Selectable(body.Bone.c_str(), !m_Selected.empty() && m_Selected.front() == body.Bone))
                        Select(body.Bone);
            return;
        }
        const RagdollData* data = m_Session.IsOpen() ? &m_Session.GetData() : nullptr;
        // Children lists, in skeleton order.
        std::vector<std::vector<int32_t>> children(skeleton->Joints.size());
        std::vector<int32_t>              roots;
        for (size_t i = 0; i < skeleton->Joints.size(); ++i)
        {
            const int32_t parent = skeleton->Joints[i].Parent;
            (parent >= 0 ? children[size_t(parent)] : roots).push_back(int32_t(i));
        }
        const bool                       skipping = m_ShowGenerate || m_Creating;
        std::function<void(int32_t)>     node     = [&](int32_t j) {
            const auto&      bone        = skeleton->Joints[size_t(j)].Name;
            const bool       body        = data && Edit::FindBody(*data, bone) >= 0;
            const bool       constraint  = data && Edit::FindConstraint(*data, bone) >= 0;
            const bool       selected    = std::find(m_Selected.begin(), m_Selected.end(), bone) != m_Selected.end();
            ImGuiTreeNodeFlags flags     = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
            if (children[size_t(j)].empty())
                flags |= ImGuiTreeNodeFlags_Leaf;
            if (selected)
                flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(j);
            if (skipping)
            {
                bool skip = std::find(m_Options.SkipBones.begin(), m_Options.SkipBones.end(), bone) != m_Options.SkipBones.end();
                if (ImGui::Checkbox("##skip", &skip))
                {
                    if (skip)
                        m_Options.SkipBones.push_back(bone);
                    else
                        m_Options.SkipBones.erase(std::remove(m_Options.SkipBones.begin(), m_Options.SkipBones.end(), bone),
                                                  m_Options.SkipBones.end());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Skip this bone when generating");
                ImGui::SameLine();
            }
            std::string label = bone;
            if (body)
                label = std::string(ICON_FA_CAPSULES " ") + label;
            const bool open = ImGui::TreeNodeEx("##bone", flags, "%s", label.c_str());
            if (constraint)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(ICON_FA_LINK);
                if (ImGui::IsItemClicked())
                    Select(bone, Selection::Constraint);
            }
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            {
                if (ImGui::GetIO().KeyCtrl)
                {
                    if (selected)
                        m_Selected.erase(std::remove(m_Selected.begin(), m_Selected.end(), bone), m_Selected.end());
                    else
                        m_Selected.push_back(bone);
                }
                else
                    Select(bone, Selection::Body);
            }
            if (data && ImGui::BeginPopupContextItem("##boneMenu"))
            {
                if (!selected)
                    Select(bone);
                if (ImGui::MenuItem(ICON_FA_PLUS "  Add body", nullptr, false, !body))
                    m_Session.Apply("Add body", [&](RagdollData& d) {
                        return Edit::AddBody(d, *skeleton, bone, m_Options.DefaultLimits);
                    });
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove body", nullptr, false, body))
                    m_Session.Apply("Remove body", [&](RagdollData& d) { return Edit::RemoveBody(d, bone); });
                if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES "  Regenerate selected"))
                {
                    m_ShowGenerate = true;
                    const MeshData* mesh   = m_Mesh ? &m_Mesh->GetData() : nullptr;
                    auto            result = Edit::Regenerate(*data, *skeleton, m_Source.Skeleton, mesh, m_Options, m_Selected);
                    if (auto* error = std::get_if<std::string>(&result))
                        m_Status = *error;
                    else
                        m_Session.Apply("Regenerate selected", [&](RagdollData& d) {
                            d = std::get<RagdollData>(result);
                            return true;
                        });
                }
                const bool pair = m_Selected.size() == 2;
                if (ImGui::MenuItem(ICON_FA_LINK_SLASH "  Disable collision between", nullptr, false,
                                    pair && !Edit::IsCollisionDisabled(*data, m_Selected[0], m_Selected[1])))
                    m_Session.Apply("Disable collision", [&](RagdollData& d) {
                        return Edit::DisableCollision(d, m_Selected[0], m_Selected[1]);
                    });
                if (ImGui::MenuItem(ICON_FA_LINK "  Enable collision between", nullptr, false,
                                    pair && Edit::IsCollisionDisabled(*data, m_Selected[0], m_Selected[1])))
                    m_Session.Apply("Enable collision", [&](RagdollData& d) {
                        return Edit::EnableCollision(d, m_Selected[0], m_Selected[1]);
                    });
                ImGui::EndPopup();
            }
            if (open)
            {
                for (int32_t child : children[size_t(j)])
                    node(child);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        for (int32_t root : roots)
            node(root);
    }

    // ---------------------------------------------------------------------- viewport

    bool RagdollEditorPanel::ToScreen(glm::vec3 point, ImVec2& out) const
    {
        const glm::vec4 clip = m_Camera.GetViewProjection() * glm::vec4(point, 1.0f);
        if (clip.w <= 1e-4f)
            return false;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = {m_Min.x + (ndc.x * 0.5f + 0.5f) * (m_Max.x - m_Min.x), m_Min.y + (0.5f - ndc.y * 0.5f) * (m_Max.y - m_Min.y)};
        return true;
    }

    void RagdollEditorPanel::Frame()
    {
        const SkeletonData* skeleton = Skeleton();
        glm::vec3           low(0.0f), high(0.0f);
        if (skeleton && !skeleton->Joints.empty())
        {
            const auto frames = Edit::BindFrames(*skeleton);
            low = high = glm::vec3(frames.front()[3]);
            for (const auto& f : frames)
                low = glm::min(low, glm::vec3(f[3])), high = glm::max(high, glm::vec3(f[3]));
        }
        m_Camera.Focus((low + high) * 0.5f, std::max(0.25f, glm::length(high - low) * 0.75f));
        m_Framed = true;
    }

    void RagdollEditorPanel::DrawViewport(ImVec2 size)
    {
        const ImGuiIO& io = ImGui::GetIO();
        m_Min             = ImGui::GetCursorScreenPos();
        m_Max             = {m_Min.x + size.x, m_Min.y + size.y};
        m_Camera.SetViewportSize(uint32_t(std::max(1.0f, size.x)), uint32_t(std::max(1.0f, size.y)));
        if (!m_Framed)
            Frame();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        m_Preview.Step(io.DeltaTime);
        // The preview scene rendered, or a plain backdrop.
        // Last frame's gizmo handles; this frame sets them again when the gizmo still shows.
        const std::vector<GizmoDraw> handles = std::move(m_GizmoDraws);
        m_GizmoDraws.clear();
        const uintptr_t image =
            m_Render && m_Preview.GetScene() ?
                m_Render(m_Preview.GetScene(), m_Camera, {uint32_t(std::max(1.0f, size.x)), uint32_t(std::max(1.0f, size.y))},
                         handles) :
                0;
        if (image)
        {
            ImGui::Image(static_cast<ImTextureID>(image), size, ImVec2(0, 1), ImVec2(1, 0));
            ImGui::SetCursorScreenPos(m_Min);
        }
        else
            draw->AddRectFilled(m_Min, m_Max, IM_COL32(30, 32, 36, 255));
        ImGui::InvisibleButton("##viewInput", size, ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && io.MouseWheel != 0.0f)
            m_Camera.Dolly(io.MouseWheel);
        // A modal transform's right and middle clicks cancel it or pick its axis.
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !m_Modal.IsActive())
            m_Camera.Orbit({io.MouseDelta.x, io.MouseDelta.y});
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && !m_Modal.IsActive())
            m_Camera.Pan({io.MouseDelta.x, io.MouseDelta.y});

        const SkeletonData* skeleton = Skeleton();
        DebugDraw           lines;
        const bool          simulating = m_Preview.IsSimulating();
        const RagdollData*  data       = m_Session.IsOpen() ? &m_Session.GetData() : nullptr;
        const std::string   primary    = m_Selected.empty() ? std::string() : m_Selected.front();
        if (skeleton && !simulating)
        {
            // The bare skeleton: a line to each joint from its parent.
            const auto frames = Edit::BindFrames(*skeleton);
            for (size_t i = 0; i < skeleton->Joints.size(); ++i)
                if (skeleton->Joints[i].Parent >= 0)
                    lines.Line(glm::vec3(frames[size_t(skeleton->Joints[i].Parent)][3]), glm::vec3(frames[i][3]),
                               {0.75f, 0.75f, 0.8f, 0.8f});
        }
        float limitRadius = 0.1f;
        if (skeleton)
            limitRadius = std::max(0.05f, m_Camera.GetDistance() * 0.1f);
        if (data)
            for (const auto& body : data->Bodies)
            {
                const auto frame = m_Preview.BodyFrame(body.Bone);
                if (!frame)
                    continue;
                const bool chosen = std::find(m_Selected.begin(), m_Selected.end(), body.Bone) != m_Selected.end();
                Edit::DrawBody(lines, *frame, body, chosen ? Edit::SelectedBodyColor : Edit::BodyColor);
                if (const int c = Edit::FindConstraint(*data, body.Bone); c >= 0 && (body.Bone == primary || simulating))
                    Edit::DrawLimits(lines, Edit::ConstraintFrame(*frame, data->Constraints[size_t(c)]),
                                     data->Constraints[size_t(c)], limitRadius);
            }
        draw->PushClipRect(m_Min, m_Max, true);
        for (const auto& line : lines.GetLines())
        {
            ImVec2 a, b;
            if (ToScreen(line.A, a) && ToScreen(line.B, b))
                draw->AddLine(a, b, Color(line.Color), 1.5f);
        }
        draw->PopClipRect();

        // Handles and the gizmo share one hover resolution; G and R transform the primitive modally.
        const auto view = Gizmo::View::From(m_Camera, {m_Min.x, m_Min.y}, {m_Max.x - m_Min.x, m_Max.y - m_Min.y});
        Gizmo::Settings settings;
        settings.Size       = EditorTheme::Px(80.0f);
        settings.PickRadius = EditorTheme::Px(10.0f);
        Gizmo::Input input;
        input.Mouse      = {io.MousePos.x, io.MousePos.y};
        input.Down       = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        input.Pressed    = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        input.Released   = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        input.Cancel     = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        input.InvertSnap = io.KeyCtrl;
        input.Alt        = io.KeyAlt;
        Gizmo::Input handleInput = input;
        handleInput.Allowed      = hovered && !io.KeyAlt && !m_Modal.IsActive() && !m_Gizmo.IsDragging();
        m_Handles.Begin(view, handleInput, settings);
        if (!data || simulating || !skeleton)
        {
            m_Handles.End();
            m_Handles.Reset();
            if (m_Modal.IsActive())
            {
                m_Modal.Reset();
                if (data)
                    Edited(m_ModalStart);
            }
            return;
        }

        // Click a body to select it.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_GizmoWasUsing && !m_Handles.WantsPointer() &&
            !m_ModalWasActive)
        {
            float       best = 24.0f;
            std::string picked;
            for (const auto& body : data->Bodies)
                if (const auto frame = m_Preview.BodyFrame(body.Bone))
                    for (const auto& shape : body.Shapes)
                    {
                        ImVec2 at;
                        if (ToScreen(glm::vec3(*frame * glm::vec4(Edit::OffsetOf(shape).Position, 1.0f)), at))
                        {
                            const float d = std::hypot(at.x - io.MousePos.x, at.y - io.MousePos.y);
                            if (d < best)
                                best = d, picked = body.Bone;
                        }
                    }
            if (!picked.empty())
                Select(picked);
        }

        const int  body      = primary.empty() ? -1 : Edit::FindBody(*data, primary);
        const auto boneFrame = primary.empty() ? std::nullopt : m_Preview.BodyFrame(primary);
        const bool hasPrimitive =
            body >= 0 && boneFrame && m_Primitive < int(data->Bodies[size_t(body)].Shapes.size());
        // Primitive resize handles (box faces: Alt resizes both sides).
        if (hasPrimitive && m_Kind == Selection::Body)
        {
            auto shape = data->Bodies[size_t(body)].Shapes[size_t(m_Primitive)];
            const Gizmo::HandleResult r = std::visit(
                [&](auto& s) {
                    return ShapeHandles::Edit(m_Handles, Gizmo::MakeHandleId(100, uint64_t(m_Primitive)), *boneFrame, s,
                                              Edit::SelectedBodyColor);
                },
                shape);
            if (r.Changed)
            {
                RagdollData copy                                    = m_Session.GetData();
                copy.Bodies[size_t(body)].Shapes[size_t(m_Primitive)] = shape;
                Edited(copy);
            }
            if (r.Hovered && !r.Active)
                ImGui::SetTooltip("Drag to resize (Alt: both sides)");
        }
        // Limit handles: twist ends on the arc, swing Y and Z on the cone, turned from where they
        // were grabbed; a cancel puts the data from the drag's start back.
        const int constraint = primary.empty() ? -1 : Edit::FindConstraint(*data, primary);
        if (constraint >= 0 && boneFrame && m_Kind == Selection::Constraint)
        {
            const auto&     c     = data->Constraints[size_t(constraint)];
            const glm::mat4 frame = Edit::ConstraintFrame(*boneFrame, c);
            const glm::vec3 centre(frame[3]);
            const auto limit = [&](uint64_t id, glm::vec3 axis, glm::vec3 reference, glm::vec3 knob, float degrees,
                                   std::optional<float> low, const char* tip, auto&& write) {
                Gizmo::AngleHandle h;
                h.Center     = centre;
                h.Axis       = axis;
                h.Reference  = reference;
                h.Radius     = glm::length(knob - centre);
                h.Min        = low;
                h.Style.Color = Edit::LimitColor;
                const auto r = m_Handles.Angle(Gizmo::MakeHandleId(200, id), h, degrees);
                if (r.Started)
                    m_Handles.Stash(m_Session.GetData());
                if (r.Changed)
                    if (const RagdollData* start = m_Handles.Stashed<RagdollData>())
                    {
                        RagdollData copy = *start;
                        if (!r.Cancelled)
                            write(copy.Constraints[size_t(constraint)], degrees);
                        Edited(copy);
                    }
                if (r.Hovered && !r.Active)
                    ImGui::SetTooltip("%s", tip);
            };
            const glm::vec3 x(frame[0]), y(frame[1]), z(frame[2]);
            limit(0, x, y, Edit::TwistPoint(frame, c.TwistLow, limitRadius * 0.6f), c.TwistLow, std::nullopt,
                  "Drag to set the twist low limit", [](RagdollConstraint& k, float a) { Edit::SetTwistLimit(k, false, a); });
            limit(1, x, y, Edit::TwistPoint(frame, c.TwistHigh, limitRadius * 0.6f), c.TwistHigh, std::nullopt,
                  "Drag to set the twist high limit", [](RagdollConstraint& k, float a) { Edit::SetTwistLimit(k, true, a); });
            limit(2, y, x, Edit::SwingPoint(frame, 1, c.SwingY, limitRadius), c.SwingY, 0.0f, "Drag to set swing Y",
                  [](RagdollConstraint& k, float a) { Edit::SetSwingLimit(k, 1, a); });
            limit(3, z, x, Edit::SwingPoint(frame, 2, c.SwingZ, limitRadius), c.SwingZ, 0.0f, "Drag to set swing Z",
                  [](RagdollConstraint& k, float a) { Edit::SetSwingLimit(k, 2, a); });
        }

        // The move/rotate gizmo on the primitive's offset or the joint frame, in local axes.
        m_GizmoWasUsing = false;
        const bool joint = m_GizmoOperation == 3;
        const bool gizmoShown =
            m_GizmoOperation != 0 && boneFrame && !m_Handles.IsDragging() && !m_Modal.IsActive() &&
            ((joint && constraint >= 0) || (!joint && hasPrimitive));
        // The primitive's (or the joint frame's) world transform.
        const auto placed = [&](bool jointFrame) {
            const Physics::ShapeOffset offset =
                jointFrame ? data->Constraints[size_t(constraint)].Frame :
                             Edit::OffsetOf(data->Bodies[size_t(body)].Shapes[size_t(m_Primitive)]);
            return *boneFrame * glm::translate(glm::mat4(1.0f), offset.Position) * glm::mat4_cast(offset.Rotation);
        };
        // Writes a moved world transform back into the primitive's offset or the joint frame.
        const auto write = [&](RagdollData copy, bool jointFrame, const glm::mat4& moved) {
            if (jointFrame)
                SetFromMatrix(copy.Constraints[size_t(constraint)].Frame, glm::inverse(*boneFrame) * moved);
            else
            {
                auto changed = Edit::OffsetOf(copy.Bodies[size_t(body)].Shapes[size_t(m_Primitive)]);
                SetFromMatrix(changed, glm::inverse(*boneFrame) * moved);
                Edit::SetOffset(copy.Bodies[size_t(body)].Shapes[size_t(m_Primitive)], changed);
            }
            Edited(copy);
        };
        if (gizmoShown)
        {
            const glm::mat4 world = placed(joint);
            Gizmo::Pivot    pivot;
            pivot.Position  = glm::vec3(world[3]);
            pivot.Axes      = Gizmo::ResolveAxes(Gizmo::Orientation::Local, world, view);
            const auto mode = m_GizmoOperation == 1 ? Gizmo::Mode::Translate : Gizmo::Mode::Rotate;
            input.Allowed   = hovered;
            if (!m_Gizmo.IsDragging() && input.Allowed)
                if (const auto hit = m_Gizmo.Pick(view, pivot, mode, settings, input.Mouse); hit.Id != Gizmo::Handle::None)
                    m_Handles.Offer(GizmoHandleId, hit.Depth, hit.Distance, Gizmo::HandlePriority(hit.Id));
            m_Handles.End();
            if (!m_Gizmo.IsDragging())
                input.Allowed = input.Allowed && (m_Handles.Hot() == 0 || m_Handles.Hot() == GizmoHandleId);
            const auto event = m_Gizmo.Update(view, pivot, mode, settings, input);
            m_GizmoWasUsing  = m_Gizmo.WantsPointer();
            if (event == Gizmo::Event::Began)
            {
                m_GizmoStart      = *data;
                m_GizmoStartWorld = world;
            }
            else if (event == Gizmo::Event::Cancelled)
                Edited(m_GizmoStart);
            else if (event == Gizmo::Event::Dragged || event == Gizmo::Event::Committed)
                write(m_GizmoStart, joint, m_Gizmo.GetDelta().Matrix() * m_GizmoStartWorld);
            m_Gizmo.AppendDraws(m_GizmoDraws);
            if (const std::string text = m_Gizmo.Readout(); !text.empty())
            {
                const glm::vec2 at = m_Gizmo.ReadoutPosition();
                const ImVec2    sz = ImGui::CalcTextSize(text.c_str());
                draw->AddRectFilled({at.x, at.y}, {at.x + sz.x + 12.0f, at.y + sz.y + 6.0f}, IM_COL32(18, 18, 22, 215), 4.0f);
                draw->AddText({at.x + 6.0f, at.y + 3.0f}, IM_COL32(240, 240, 240, 255), text.c_str());
            }
        }
        else
        {
            m_Gizmo.Reset();
            m_Handles.End();
        }
        m_Handles.AppendDraws(m_GizmoDraws);

        // Modal G (move) and R (rotate) on the selected primitive's offset, from the pointer.
        m_ModalWasActive = m_Modal.IsActive();
        if (!m_Modal.IsActive() && hovered && hasPrimitive && m_Kind == Selection::Body && !m_Handles.IsDragging() &&
            !m_Gizmo.IsDragging() && !io.KeyCtrl && !io.KeyAlt && !io.WantTextInput &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const bool move = ImGui::IsKeyPressed(ImGuiKey_G, false), turn = ImGui::IsKeyPressed(ImGuiKey_R, false);
            if (move || turn)
            {
                const glm::mat4 world = placed(false);
                Gizmo::Pivot    pivot;
                pivot.Position    = glm::vec3(world[3]);
                pivot.Axes        = Gizmo::ResolveAxes(Gizmo::Orientation::Local, world, view);
                m_ModalStart      = *data;
                m_ModalStartWorld = world;
                m_Modal.Start(move ? Gizmo::Operation::Translate : Gizmo::Operation::Rotate, view, pivot,
                              Gizmo::Orientation::Local, pivot.Axes, settings, input.Mouse);
                m_ModalWasActive = true;
            }
        }
        else if (m_Modal.IsActive())
        {
            const auto event = m_Modal.Update(view, Widgets::ModalTransformInput(!hasPrimitive || m_Kind != Selection::Body));
            if (event == Gizmo::Event::Cancelled)
                Edited(m_ModalStart);
            else if (event == Gizmo::Event::Dragged || event == Gizmo::Event::Committed)
            {
                if (m_Modal.GetDelta().Op == Gizmo::Operation::Scale)
                    Edited(m_ModalStart); // Offsets do not scale: S does nothing here.
                else
                    write(m_ModalStart, false, m_Modal.GetDelta().Matrix() * m_ModalStartWorld);
            }
        }
        m_Modal.AppendDraws(m_GizmoDraws);
        Widgets::DrawModalTransformStatus(m_Modal, {m_Min.x + 8.0f, m_Max.y - 8.0f});
    }

    // ---------------------------------------------------------------------- details

    void RagdollEditorPanel::DrawDetails()
    {
        if (!m_Session.IsOpen())
            return;
        RagdollData        data     = m_Session.GetData();
        bool               changed  = false;
        const std::string  primary  = m_Selected.empty() ? std::string() : m_Selected.front();
        const SkeletonData* skeleton = Skeleton();

        if (PropertyGrid::BeginGroup("Ragdoll"))
        {
            const auto labels = PhysicsEditing::LayerLabels(KansFileSystem::GetProjectPhysicsSettings());
            std::vector<const char*> items;
            for (const auto& l : labels)
                items.push_back(l.c_str());
            int layer = int(data.DefaultLayer);
            const int zero = 0;
            if (PropertyGrid::Combo("Default layer", items.data(), int(items.size()), layer, &zero))
                data.DefaultLayer = uint32_t(layer), changed = true;
            PropertyGrid::Text("Bodies", std::to_string(data.Bodies.size()) + " (at most 64)", true);
            PropertyGrid::EndGroup();
        }
        if (primary.empty())
            PropertyGrid::Text("", "Select a bone in the tree or a body in the view", true);
        const int body = primary.empty() ? -1 : Edit::FindBody(data, primary);
        if (!primary.empty() && body < 0 && PropertyGrid::BeginGroup(primary.c_str()))
        {
            PropertyGrid::Text("", "No body on this bone", true);
            if (PropertyGrid::Row("") && skeleton && ImGui::SmallButton(ICON_FA_PLUS "  Add body"))
                changed |= Edit::AddBody(data, *skeleton, primary, m_Options.DefaultLimits);
            PropertyGrid::EndGroup();
        }
        if (body >= 0)
        {
            static const RagdollBody defaults;
            auto&                    b = data.Bodies[size_t(body)];
            if (PropertyGrid::BeginGroup((std::string(ICON_FA_CAPSULES "  Body  ") + primary).c_str()))
            {
                for (size_t i = 0; i < b.Shapes.size(); ++i)
                {
                    ImGui::PushID(int(i));
                    auto&       shape = b.Shapes[i];
                    const auto  kind  = Edit::KindOf(shape);
                    std::string title = std::string(Edit::KindName(kind)) + " " + std::to_string(i + 1);
                    if (PropertyGrid::BeginGroup(title.c_str()))
                    {
                        if (PropertyGrid::Row(""))
                        {
                            if (ImGui::RadioButton("Edit in view", m_Primitive == int(i) && m_Kind == Selection::Body))
                                m_Primitive = int(i), m_Kind = Selection::Body;
                        }
                        const char* kinds[] = {"Sphere", "Capsule", "Box"};
                        int         k       = int(kind);
                        if (PropertyGrid::Combo("Type", kinds, 3, k))
                            shape = Edit::Convert(shape, Edit::PrimitiveKind(k)), changed = true;
                        std::visit(
                            [&](auto& s) {
                                using T = std::decay_t<decltype(s)>;
                                if constexpr (std::is_same_v<T, Physics::Sphere>)
                                    changed |= PropertyGrid::Float("Radius", s.Radius, 0.002f, PhysicsEditing::MinimumSize, 10.0f, "%.3f m");
                                else if constexpr (std::is_same_v<T, Physics::Capsule>)
                                {
                                    changed |= PropertyGrid::Float("Radius", s.Radius, 0.002f, PhysicsEditing::MinimumSize, 10.0f, "%.3f m");
                                    changed |= PropertyGrid::Float("Half height", s.HalfHeight, 0.002f, 0.0f, 10.0f, "%.3f m");
                                    const char* axes[] = {"X", "Y", "Z"};
                                    int         axis   = int(s.Direction);
                                    if (PropertyGrid::Combo("Direction", axes, 3, axis))
                                        s.Direction = Physics::Axis(axis), changed = true;
                                }
                                else
                                {
                                    glm::vec3 sizeV = glm::abs(s.HalfExtents) * 2.0f;
                                    if (PropertyGrid::Vec3("Size", sizeV, 0.1f, 0.005f))
                                        s.HalfExtents = glm::max(sizeV * 0.5f, glm::vec3(PhysicsEditing::MinimumSize)), changed = true;
                                }
                                changed |= OffsetRows(s.Offset);
                            },
                            shape);
                        if (PropertyGrid::Row("") && b.Shapes.size() > 1 && ImGui::SmallButton(ICON_FA_TRASH "  Remove primitive"))
                        {
                            b.Shapes.erase(b.Shapes.begin() + std::ptrdiff_t(i));
                            m_Primitive = 0;
                            changed     = true;
                            PropertyGrid::EndGroup();
                            ImGui::PopID();
                            break;
                        }
                        PropertyGrid::EndGroup();
                    }
                    ImGui::PopID();
                }
                if (PropertyGrid::Row("", "Bodies may combine several primitives"))
                {
                    if (ImGui::SmallButton(ICON_FA_PLUS "  Add primitive"))
                        ImGui::OpenPopup("##addPrimitive");
                    if (ImGui::BeginPopup("##addPrimitive"))
                    {
                        const char* kinds[] = {"Sphere", "Capsule", "Box"};
                        for (int k = 0; k < 3; ++k)
                            if (ImGui::MenuItem(kinds[k]))
                            {
                                b.Shapes.push_back(Edit::NewPrimitive(Edit::PrimitiveKind(k), 0.1f));
                                m_Primitive = int(b.Shapes.size()) - 1;
                                changed     = true;
                            }
                        ImGui::EndPopup();
                    }
                }
                if (PropertyGrid::Row("Gizmo", "Move or rotate the selected primitive in the view"))
                {
                    if (ImGui::RadioButton("Off", m_GizmoOperation == 0))
                        m_GizmoOperation = 0;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Move", m_GizmoOperation == 1))
                        m_GizmoOperation = 1, m_Kind = Selection::Body;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Rotate", m_GizmoOperation == 2))
                        m_GizmoOperation = 2, m_Kind = Selection::Body;
                }
                changed |= PropertyGrid::Checkbox("Mass from density", b.UseDensity, &defaults.UseDensity);
                if (b.UseDensity)
                    changed |= PropertyGrid::Float("Density", b.Density, 1.0f, 0.001f, 1.0e5f, "%.0f kg/m³", &defaults.Density);
                else
                    changed |= PropertyGrid::Float("Mass", b.Mass, 0.05f, 0.001f, 1.0e5f, "%.2f kg");
                changed |= PropertyGrid::Float(
                    "Linear damping", b.LinearDamping, 0.005f, 0.0f, 100.0f, "%.3f", &defaults.LinearDamping);
                changed |= PropertyGrid::Float(
                    "Angular damping", b.AngularDamping, 0.005f, 0.0f, 100.0f, "%.3f", &defaults.AngularDamping);
                // Physics material: a published .physmat, or the project default.
                if (PropertyGrid::Row("Material", "A physics material asset; empty uses the project default", b.Material.IsValid()))
                {
                    std::string current = "Project default";
                    for (const auto& choice : m_Choices)
                        if (choice.ID == b.Material)
                            current = choice.Name;
                    if (ImGui::BeginCombo("##material", current.c_str()))
                    {
                        if (ImGui::Selectable("Project default", !b.Material.IsValid()))
                            b.Material = {}, changed = true;
                        for (const auto& choice : m_Choices)
                            if (choice.Type == AssetType::PhysicsMaterial &&
                                ImGui::Selectable(choice.Name.c_str(), choice.ID == b.Material))
                                b.Material = choice.ID, changed = true;
                        ImGui::EndCombo();
                    }
                }
                {
                    const auto labels = PhysicsEditing::LayerLabels(KansFileSystem::GetProjectPhysicsSettings());
                    const int  index  = PhysicsEditing::ColliderLayerIndex(b.Layer);
                    if (PropertyGrid::Row("Layer", "The ragdoll's default layer, or this body's own", index != 0))
                        if (ImGui::BeginCombo("##layer", index == 0 ? "Ragdoll default" : labels[size_t(index - 1)].c_str(),
                                              ImGuiComboFlags_HeightLarge))
                        {
                            if (ImGui::Selectable("Ragdoll default", index == 0))
                                b.Layer.reset(), changed = true;
                            for (int i = 1; i <= 32; ++i)
                                if (ImGui::Selectable(labels[size_t(i - 1)].c_str(), i == index))
                                    b.Layer = PhysicsEditing::ColliderLayerFromIndex(i), changed = true;
                            ImGui::EndCombo();
                        }
                }
                if (PropertyGrid::Row("") && ImGui::SmallButton(ICON_FA_TRASH "  Remove body"))
                    changed |= Edit::RemoveBody(data, primary);
                PropertyGrid::EndGroup();
            }
        }
        const int constraint = primary.empty() ? -1 : Edit::FindConstraint(data, primary);
        if (constraint >= 0)
        {
            static const RagdollConstraint defaults;
            auto&                          c = data.Constraints[size_t(constraint)];
            if (PropertyGrid::BeginGroup((std::string(ICON_FA_LINK "  Constraint  ") + primary).c_str()))
            {
                if (PropertyGrid::Row(""))
                    if (ImGui::RadioButton("Edit limits in view", m_Kind == Selection::Constraint))
                        m_Kind = Selection::Constraint;
                changed |= Degrees("Twist low", c.TwistLow, -Edit::MaxTwist, c.TwistHigh, &defaults.TwistLow);
                changed |= Degrees("Twist high", c.TwistHigh, c.TwistLow, Edit::MaxTwist, &defaults.TwistHigh);
                changed |= Degrees("Swing Y", c.SwingY, Edit::MinSwing, Edit::MaxSwing, &defaults.SwingY);
                changed |= Degrees("Swing Z", c.SwingZ, Edit::MinSwing, Edit::MaxSwing, &defaults.SwingZ);
                changed |= PropertyGrid::Float(
                    "Limit stiffness", c.LimitStiffness, 1.0f, 0.0f, 1.0e9f, c.LimitStiffness > 0 ? "%.1f" : "Hard", &defaults.LimitStiffness);
                changed |= PropertyGrid::Float(
                    "Limit damping", c.LimitDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.LimitDamping);
                changed |= PropertyGrid::Float(
                    "Drive strength", c.DriveStrength, 1.0f, 0.0f, 1.0e9f, "%.1f", &defaults.DriveStrength);
                changed |= PropertyGrid::Float(
                    "Drive damping", c.DriveDamping, 0.1f, 0.0f, 1.0e9f, "%.2f", &defaults.DriveDamping);
                glm::vec3 euler = glm::degrees(glm::eulerAngles(c.Frame.Rotation));
                if (PropertyGrid::Vec3("Frame rotation", euler, 0.0f, 0.5f))
                    c.Frame.Rotation = glm::quat(glm::radians(euler)), changed = true;
                changed |= PropertyGrid::Vec3("Frame position", c.Frame.Position, 0.0f, 0.005f);
                if (PropertyGrid::Row("", "Rotate the joint frame in the view (+X twists)"))
                {
                    bool rotating = m_GizmoOperation == 3;
                    if (ImGui::Checkbox("Rotate frame in view", &rotating))
                        m_GizmoOperation = rotating ? 3 : 0, m_Kind = Selection::Constraint;
                }
                PropertyGrid::EndGroup();
            }
        }
        if (PropertyGrid::BeginGroup("Disabled collision pairs", nullptr, nullptr, !data.DisabledCollisionPairs.empty()))
        {
            for (size_t i = 0; i < data.DisabledCollisionPairs.size(); ++i)
            {
                const auto& pair = data.DisabledCollisionPairs[i];
                ImGui::PushID(int(i));
                const std::string label = pair.first + "  ↔  " + pair.second;
                if (PropertyGrid::Row(""))
                {
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_XMARK "##remove"))
                    {
                        data.DisabledCollisionPairs.erase(data.DisabledCollisionPairs.begin() + std::ptrdiff_t(i));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
            if (PropertyGrid::Row("", "Ctrl+click two bodies in the tree, then add the pair"))
            {
                const bool can = m_Selected.size() == 2 && Edit::FindBody(data, m_Selected[0]) >= 0 &&
                                 Edit::FindBody(data, m_Selected[1]) >= 0 &&
                                 !Edit::IsCollisionDisabled(data, m_Selected[0], m_Selected[1]);
                ImGui::BeginDisabled(!can);
                if (ImGui::SmallButton(ICON_FA_LINK_SLASH "  Disable collision between the selected"))
                    changed |= Edit::DisableCollision(data, m_Selected[0], m_Selected[1]);
                ImGui::EndDisabled();
            }
            PropertyGrid::EndGroup();
        }
        if (changed)
            Edited(data);
    }
} // namespace Kans
