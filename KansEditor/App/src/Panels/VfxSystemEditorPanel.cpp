#include "VfxSystemEditorPanel.h"
#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Panels/VfxModuleEditing.h"
#include "UI/CurveEditor.h"
#include "UI/EditorIcons.h"
#include "UI/GradientEditor.h"
#include "UI/PropertyGrid.h"
#include "KansEditorCore/UI/SourceBuild.h"

namespace Kans
{
    namespace fs = std::filesystem;
    using namespace VfxSystemEditing;
    using Vfx::Stage;

    namespace
    {
        constexpr const char* UsePayload       = "KANS_VFX_MODULE_USE";
        constexpr const char* ParameterPayload = "KANS_VFX_SYSTEM_PARAMETER";
        constexpr Vfx::Type   AllTypes[]       = {Vfx::Type::Float, Vfx::Type::Vec2, Vfx::Type::Vec3, Vfx::Type::Vec4, Vfx::Type::Int,
                                                  Vfx::Type::Bool,  Vfx::Type::Color, Vfx::Type::Quat, Vfx::Type::Position};
        const char* StageTitle(Stage s)
        {
            static const char* labels[] = {"System Spawn",   "System Update",   "Emitter Spawn", "Emitter Update",
                                           "Particle Spawn", "Particle Update", "Event Handlers", "Simulation Stages"};
            return labels[int(s)];
        }
        std::string Short(const std::string& name)
        {
            auto dot = name.find('.');
            return dot == std::string::npos ? name : name.substr(dot + 1);
        }
        Vfx::Value Zero(Vfx::Type t)
        {
            return t == Vfx::Type::Int ? Vfx::Value::Int(0) : t == Vfx::Type::Bool ? Vfx::Value::Bool(false) : Vfx::Value::Vector({}, t);
        }
        ImVec4 FromAbgr(uint32_t c) { return ImGui::ColorConvertU32ToFloat4(c); }
        glm::vec3 SunDirection(float yaw, float pitch)
        {
            const float y = glm::radians(yaw), p = glm::radians(pitch);
            return {std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
        }
        const char* RendererName(Vfx::RendererKind k)
        {
            switch (k)
            {
                case Vfx::RendererKind::Sprite:
                    return "Sprite Renderer";
                case Vfx::RendererKind::Mesh:
                    return "Mesh Renderer";
                case Vfx::RendererKind::Ribbon:
                    return "Ribbon Renderer";
                default:
                    return "Light Renderer";
            }
        }
        template<size_t N>
        bool EnumCombo(const char* label, const char* const (&items)[N], int& value)
        {
            int index = std::clamp(value, 0, int(N) - 1);
            if (PropertyGrid::Combo(label, items, int(N), index))
            {
                value = index;
                return true;
            }
            return false;
        }
    } // namespace

    // ---------------------------------------------------------------------- asset pickers

    std::optional<AssetID> VfxSystemEditorPanel::DroppedAsset(const ImGuiPayload& payload, AssetType type)
    {
        std::error_code error;
        const auto      file = fs::weakly_canonical(
            fs::u8path(std::string(static_cast<const char*>(payload.Data), size_t(payload.DataSize))), error);
        if (error || !m_AssetChoices)
            return std::nullopt;
        std::optional<AssetType> other;
        for (const auto& choice : m_AssetChoices())
            if (choice.SourcePath == file)
            {
                if (choice.Type == type)
                    return choice.ID;
                other = choice.Type;
            }
        m_Status = other ? *AssetTypeMismatch(type, *other) : file.filename().u8string() + " is not an imported asset";
        return std::nullopt;
    }

    bool VfxSystemEditorPanel::AssetCombo(const char* id, AssetType type, AssetID& asset, float width)
    {
        ImGui::PushID(id);
        bool        changed = false;
        std::string preview = asset.IsValid() ? "(missing) " + asset.ToString() : "None";
        if (m_AssetChoices)
            for (const auto& choice : m_AssetChoices())
                if (choice.ID == asset)
                    preview = choice.Name;
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##asset", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
            {
                m_AssetFilter.clear();
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter", &m_AssetFilter);
            if (ImGui::Selectable("None", !asset.IsValid()) && asset.IsValid())
                asset = {}, changed = true;
            std::string needle = m_AssetFilter;
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (m_AssetChoices)
                for (const auto& choice : m_AssetChoices())
                {
                    if (choice.Type != type)
                        continue;
                    std::string name = choice.Name;
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    if (!needle.empty() && name.find(needle) == std::string::npos)
                        continue;
                    const bool selected = choice.ID == asset;
                    if (ImGui::Selectable(choice.Name.c_str(), selected) && !selected)
                        asset = choice.ID, changed = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nA %s asset; drop one from the Content Browser.", preview.c_str(), AssetTypeLabel(type).c_str());
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("asset_payload"))
                if (auto dropped = DroppedAsset(*payload, type); dropped && *dropped != asset)
                    asset = *dropped, changed = true;
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return changed;
    }

    bool VfxSystemEditorPanel::AssetRow(const char* label, AssetType type, AssetID& asset)
    {
        if (!PropertyGrid::Row(label, (AssetTypeLabel(type) + " asset; pick one or drop it from the Content Browser").c_str()))
            return false;
        return AssetCombo(label, type, asset, -FLT_MIN);
    }

    // ---------------------------------------------------------------------- document

    bool VfxSystemEditorPanel::Open(const fs::path& file)
    {
        if (auto error = m_Document.Open(file))
        {
            m_Status = *error;
            return false;
        }
        Refresh();
        m_IsOpen     = true;
        m_WindowOpen = true;
        m_Framed     = false;
        m_Status     = m_Document.ReadOnly() ? "Built-in template: read-only." : "";
        const auto& s = m_Document.Data();
        m_Selected = m_Document.GetMode() == Mode::Emitter ? std::string(EmitterModeHandle) :
                     s.Emitters.empty()                    ? std::string(SystemNode) :
                                                             s.Emitters.front().Guid;
        m_Positions = OverviewLayout(s);
        m_Overview.SetSelection({m_Selected});
        m_Diagnostics.clear();
        m_SeenRevision = m_CompiledRevision = size_t(-1);
        m_Product.reset();
        m_Preview       = std::make_unique<VfxPreview::Session>();
        m_PreviewKey    = {size_t(-1), 0};
        m_PreviewFramed = false;
        m_Preview->SetSun(m_Sun, SunDirection(m_SunYaw, m_SunPitch));
        m_GizmoUse.clear();
        m_GizmoGrid.clear();
        m_Gizmos.Reset();
        m_Handles.Reset();
        return true;
    }

    void VfxSystemEditorPanel::Refresh()
    {
        m_Library.Load(m_ProjectModules ? m_ProjectModules() : std::vector<std::pair<AssetID, fs::path>> {});
        m_Context.Modules = &m_Library;
    }

    NodeCanvas::Graph VfxSystemEditorPanel::BuildOverview() const
    {
        auto g = VfxSystemEditing::BuildOverview(m_Document.Data(), m_Context, m_Positions, m_Diagnostics);
        // Runtime state from the preview: live counts on the nodes, and solo or mute.
        const auto stats = m_Preview->Stats();
        for (auto& n : g.Nodes)
        {
            if (m_Preview->IsMuted(n.Id) || (!m_Preview->Solo().empty() && m_Preview->Solo() != n.Id && n.Id != SystemNode))
            {
                n.Body.push_back(m_Preview->IsMuted(n.Id) ? "Muted in preview" : "Hidden by solo");
                n.Accent = 0xff808080;
            }
            else if (m_Preview->Solo() == n.Id)
                n.Body.push_back("Solo in preview");
            for (const auto& x : stats)
                if (x.Handle == n.Id)
                    n.Body.push_back(std::to_string(x.Count) + " / " + std::to_string(x.Capacity) + " particles");
        }
        if (m_Document.GetMode() == Mode::Emitter && !g.Nodes.empty())
            g.Nodes.erase(g.Nodes.begin()); // One emitter: no system node.
        return g;
    }

    void VfxSystemEditorPanel::Select(const std::string& emitter)
    {
        m_Selected = emitter;
        m_Overview.SetSelection({emitter});
    }

    std::string VfxSystemEditorPanel::Handle() const { return m_Selected == SystemNode ? std::string() : m_Selected; }

    void VfxSystemEditorPanel::Edited()
    {
        if (!m_Document.ReadOnly())
            m_Document.Commit();
    }

    Result VfxSystemEditorPanel::Apply(const std::function<Result(Vfx::System&, const Context&)>& edit, bool continuous)
    {
        if (ReadOnly())
            return Result::Refused("Built-in templates are read-only; duplicate it to edit it.");
        auto r = edit(m_Document.Data(), m_Context);
        if (!r)
        {
            m_Status = r.Message;
            return r;
        }
        m_Status.clear();
        if (continuous)
            m_ValueEdit = true;
        else
        {
            m_ValueEdit = false;
            Edited();
        }
        return r;
    }

    void VfxSystemEditorPanel::FinishEdit()
    {
        m_ValueEdit = false;
        Edited();
    }

    bool VfxSystemEditorPanel::Save()
    {
        if (auto error = m_Document.Save())
        {
            m_Status = *error;
            return false;
        }
        m_Status = BuildSavedSource(m_Service, m_Document.File());
        return true;
    }

    bool VfxSystemEditorPanel::Undo()
    {
        if (m_Gizmos.IsDragging())
            return false; // A handle drag is one step; it ends first.
        return m_Document.Undo();
    }
    bool VfxSystemEditorPanel::Redo()
    {
        if (m_Gizmos.IsDragging())
            return false;
        return m_Document.Redo();
    }

    std::optional<fs::path> VfxSystemEditorPanel::DuplicateToProject(const fs::path& directory)
    {
        auto copy = VfxSystemEditing::Duplicate(m_Document, directory);
        if (const auto* error = std::get_if<std::string>(&copy))
        {
            m_Status = *error;
            return std::nullopt;
        }
        const auto file = std::get<fs::path>(copy);
        m_Status        = "Duplicated to " + file.u8string();
        if (m_Open)
            m_Open(file);
        return file;
    }

    // ---------------------------------------------------------------------- search

    void VfxSystemEditorPanel::OpenModuleSearch(const StackRef& stack, size_t index)
    {
        m_Search         = {};
        m_Search.Open    = true;
        m_Search.Focus   = true;
        m_Search.Stack   = stack;
        m_Search.Index   = index;
        m_Search.Dynamic = false;
    }

    void VfxSystemEditorPanel::OpenDynamicInputSearch(const std::string& use, const InputPath& path)
    {
        m_Search         = {};
        m_Search.Open    = true;
        m_Search.Focus   = true;
        m_Search.Dynamic = true;
        m_Search.Use     = use;
        m_Search.Path    = path;
        if (const auto* h = FindHandle(m_Document.Data(), Handle()))
            try
            {
                const auto e = Effective(*h, m_Context);
                if (auto ref = StackOf(e, use))
                    m_Search.Stack = *ref;
                if (const auto* u = FindUse(e, use))
                    m_Search.Kind = InputType(m_Context, *u, path).value_or(Vfx::Type::Float);
            }
            catch (const std::exception&)
            {}
    }

    std::vector<ModuleEntry> VfxSystemEditorPanel::SearchResults() const
    {
        const Stage stage = m_Search.Stack.Kind;
        return m_Search.Dynamic ? SearchDynamicInputs(m_Library, stage, m_Search.Kind, m_Search.Query) :
                                  SearchModules(m_Library, stage, m_Search.Query);
    }

    Result VfxSystemEditorPanel::ChooseSearch(const ModuleEntry& entry)
    {
        const auto handle = Handle();
        Result     r;
        if (m_Search.Dynamic)
            r = Apply([&](Vfx::System& s, const Context& c) { return SetDynamicInput(s, c, handle, m_Search.Use, m_Search.Path, entry.Id); });
        else
        {
            r = Apply([&](Vfx::System& s, const Context& c) { return AddModule(s, c, handle, m_Search.Stack, entry.Id, m_Search.Index); });
            if (r)
                Expand(r.Value);
        }
        m_Search.Open = false;
        return r;
    }

    // ---------------------------------------------------------------------- curves and gradients

    void VfxSystemEditorPanel::OpenCurveEditor(const std::string& use, const InputPath& path)
    {
        m_CurveEdit            = {};
        m_CurveEdit.Open       = true;
        m_CurveEdit.Requested  = true;
        m_CurveEdit.IsGradient = false;
        m_CurveEdit.Use        = use;
        m_CurveEdit.Path       = path;
        if (const auto* h = FindHandle(m_Document.Data(), Handle()))
            try
            {
                const auto e = Effective(*h, m_Context);
                if (const auto* u = FindUse(e, use))
                {
                    auto input = GetInput(*u, path);
                    Vfx::InterfaceBinding b;
                    if (input && input->Kind == Vfx::InputKind::DataInterface)
                        b = input->Interface;
                    else if (const auto* m = m_Library.Find(u->Script))
                        for (const auto& info : ModuleInputs(m->Data))
                            if (info.Spec.Name == path.back())
                                b = info.DefaultBinding;
                    m_CurveEdit.Curve = CurveFromBinding(b);
                }
            }
            catch (const std::exception&)
            {}
    }

    void VfxSystemEditorPanel::OpenGradientEditor(const std::string& use, const InputPath& path)
    {
        OpenCurveEditor(use, path);
        m_CurveEdit.IsGradient = true;
        Vfx::InterfaceBinding b;
        b.Kind = "Gradient";
        for (const auto& key : m_CurveEdit.Curve.Keys)
        {
            b.Times.push_back(float(key.Time));
            b.Values.push_back(key.Value);
        }
        m_CurveEdit.Gradient = Gradient::FromBinding(b);
    }

    Result VfxSystemEditorPanel::SetEditedGradient(const Gradient& gradient, bool continuous)
    {
        m_CurveEdit.Gradient = gradient;
        Vfx::Input input;
        input.Kind      = Vfx::InputKind::DataInterface;
        input.Interface = gradient.Bake();
        const auto handle = Handle();
        return Apply([&](Vfx::System& s, const Context& c) { return SetInput(s, c, handle, m_CurveEdit.Use, m_CurveEdit.Path, input); },
                     continuous);
    }

    Result VfxSystemEditorPanel::SetEditedCurve(const TimelineCurve& curve, bool continuous)
    {
        m_CurveEdit.Curve = curve;
        Vfx::Input input;
        input.Kind      = Vfx::InputKind::DataInterface;
        input.Interface = BindingFromCurve(curve);
        const auto handle = Handle();
        return Apply([&](Vfx::System& s, const Context& c) { return SetInput(s, c, handle, m_CurveEdit.Use, m_CurveEdit.Path, input); },
                     continuous);
    }

    // ---------------------------------------------------------------------- compile

    void VfxSystemEditorPanel::Tick(double now)
    {
        m_Now = now;
        if (m_Compile.valid() && m_Compile.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto result = m_Compile.get();
            m_Diagnostics.clear();
            for (const auto& d : result.Errors)
                m_Diagnostics.push_back(Locate(d, m_Document.Data(), m_Context));
            for (const auto& h : m_Document.Data().Emitters)
                for (const auto& p : ListParameters(m_Document.Data(), m_Context, h.Guid))
                    if (!p.Warning.empty())
                    {
                        StackDiagnostic w;
                        w.Emitter = h.Guid;
                        w.Message = p.Name + ": " + p.Warning;
                        w.Warning = true;
                        m_Diagnostics.push_back(std::move(w));
                    }
            m_CompiledRevision = m_PendingRevision;
            m_CompiledSystem   = std::move(m_PendingSystem);
            if (result)
            {
                m_Product = std::move(result.Product);
                m_CompileFailure.clear();
            }
            else
            {
                m_Product.reset();
                m_CompileFailure = result.Errors.empty() ? "The system does not compile" : result.Errors.front().Message;
            }
        }
        if (m_Document.Revision() != m_SeenRevision)
        {
            m_SeenRevision = m_Document.Revision();
            m_EditTime     = now;
        }
        if (!m_Compile.valid() && m_CompiledRevision != m_SeenRevision && now - m_EditTime >= CompileDelay)
        {
            m_PendingRevision = m_SeenRevision;
            m_PendingSystem   = m_Document.Data();
            m_Compile         = std::async(std::launch::async, [system = m_Document.Data(), modules = m_Library.Resolver(),
                                                        emitters = m_Context.ResolveEmitter()] {
                try
                {
                    return Vfx::Compile(system, emitters, modules);
                }
                catch (const std::exception& e)
                {
                    Vfx::CompileResult r;
                    Vfx::Diagnostic    d;
                    d.Message = e.what();
                    r.Errors  = {d};
                    return r;
                }
            });
        }
        UpdatePreviewProduct();
    }

    void VfxSystemEditorPanel::UpdatePreviewProduct()
    {
        if (m_CompiledRevision == size_t(-1))
            return;
        const std::pair<size_t, uint64_t> key {m_CompiledRevision, m_Preview->MaskRevision()};
        if (m_PreviewCompile.valid())
        {
            if (m_PreviewCompile.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            auto result  = m_PreviewCompile.get();
            m_PreviewKey = m_PreviewCompileKey;
            if (result)
                m_Preview->SetProduct(std::move(result.Product));
            else
                m_Preview->SetOutdated(result.Errors.empty() ? "The preview does not compile" : result.Errors.front().Message);
        }
        if (m_PreviewKey == key)
            return;
        if (!m_Product)
        {
            m_Preview->SetOutdated(m_CompileFailure);
            m_PreviewKey = key;
        }
        else if (!m_Preview->HasMask())
        {
            m_Preview->SetProduct(*m_Product);
            m_PreviewKey = key;
        }
        else
        {
            m_PreviewCompileKey = key;
            m_PreviewCompile    = std::async(std::launch::async, [system = m_Preview->Masked(m_CompiledSystem),
                                                               modules = m_Library.Resolver(), emitters = m_Context.ResolveEmitter()] {
                try
                {
                    return Vfx::Compile(system, emitters, modules);
                }
                catch (const std::exception& e)
                {
                    Vfx::CompileResult r;
                    Vfx::Diagnostic    d;
                    d.Message = e.what();
                    r.Errors  = {d};
                    return r;
                }
            });
        }
    }

    void VfxSystemEditorPanel::WaitForCompile()
    {
        if (m_Compile.valid())
            m_Compile.wait();
        Tick(m_EditTime);
        if (m_PreviewCompile.valid())
        {
            m_PreviewCompile.wait();
            Tick(m_EditTime);
        }
    }

    void VfxSystemEditorPanel::SetMuted(const std::string& handle, bool muted)
    {
        m_Preview->SetMuted(handle, muted);
        UpdatePreviewProduct();
    }

    void VfxSystemEditorPanel::SetSolo(const std::string& handle)
    {
        m_Preview->SetSolo(handle);
        UpdatePreviewProduct();
    }

    std::vector<size_t> VfxSystemEditorPanel::SheetRows() const
    {
        const auto& capture = m_Preview->LastCapture();
        if (!capture)
            return {};
        auto rows = m_Sheet.SortColumn >= 0 ?
                        VfxPreview::SortRows(*capture, size_t(m_Sheet.SortColumn), uint32_t(m_Sheet.SortComponent), m_Sheet.Ascending) :
                        VfxPreview::SortRows(*capture, size_t(-1), 0, true);
        if (m_Sheet.FilterColumn >= 0)
            rows = VfxPreview::FilterRows(*capture, rows, size_t(m_Sheet.FilterColumn), uint32_t(m_Sheet.FilterComponent),
                                          m_Sheet.FilterMin, m_Sheet.FilterMax);
        return rows;
    }

    void VfxSystemEditorPanel::ActivateDiagnostic(size_t index)
    {
        if (index >= m_Diagnostics.size())
            return;
        const auto& d = m_Diagnostics[index];
        if (!d.Emitter.empty() && FindHandle(m_Document.Data(), d.Emitter))
            Select(d.Emitter);
        else if (d.Emitter.empty() && m_Document.GetMode() == Mode::System)
            Select(SystemNode);
        if (!d.Use.empty())
            Expand(d.Use);
        m_Highlight = {d.Use, d.Input, m_Now + 2.0, true};
    }

    // ---------------------------------------------------------------------- close

    void VfxSystemEditorPanel::RequestClose(std::function<void()> closed)
    {
        m_Closing.Request(m_IsOpen && m_Document.IsDirty(), "closing " + m_Document.File().filename().u8string(), [this, closed] {
            m_IsOpen = false;
            if (closed)
                closed();
        });
        m_ClosePromptRequested = m_Closing.Asking();
    }

    void VfxSystemEditorPanel::ResolveClose(UnsavedChanges::Choice choice)
    {
        m_Closing.Resolve(choice, [this] { return Save() ? UnsavedChanges::SaveResult::Saved : UnsavedChanges::SaveResult::Failed; });
    }

    // ---------------------------------------------------------------------- overview host

    bool VfxSystemEditorPanel::CanConnect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input, std::string* reason)
    {
        const auto refuse = [&](const std::string& why) {
            if (reason)
                *reason = why;
            return false;
        };
        if (output.Node == SystemNode || input.Node == SystemNode)
            return refuse("The system node has no events");
        if (output.Node == input.Node)
            return refuse("An emitter cannot handle its own events here");
        const auto& s = m_Document.Data();
        if (EventChannels(s, m_Context, output.Node).empty() && EventLayouts(s, m_Context, output.Node).empty())
        {
            const auto* h = FindHandle(s, output.Node);
            return refuse((h ? h->Name : output.Node) + " writes no events; add an event generator module first");
        }
        return true;
    }

    void VfxSystemEditorPanel::Connect(const NodeCanvas::PinRef& output, const NodeCanvas::PinRef& input)
    {
        auto r = Apply([&](Vfx::System& s, const Context& c) { return ConnectEmitters(s, c, output.Node, input.Node); });
        if (r)
        {
            Select(input.Node);
            m_Status = "Added an event handler";
        }
    }

    void VfxSystemEditorPanel::Disconnect(const NodeCanvas::PinRef&)
    {
        m_Status = "Links show event handlers and particle readers; remove them in the emitter's stack.";
    }

    void VfxSystemEditorPanel::DisconnectLink(size_t) { Disconnect({}); }

    void VfxSystemEditorPanel::Translate(const std::vector<std::string>& nodes, const std::vector<std::string>&, glm::vec2 delta)
    {
        const auto layout = OverviewLayout(m_Document.Data());
        for (const auto& n : nodes)
        {
            auto at = m_Positions.find(n);
            if (at == m_Positions.end())
                at = m_Positions.emplace(n, layout.count(n) ? layout.at(n) : glm::vec2(0)).first;
            at->second += delta;
        }
    }

    void VfxSystemEditorPanel::RequestSearch(const std::optional<NodeCanvas::PinRef>&, glm::vec2)
    {
        if (m_Document.GetMode() == Mode::System && !ReadOnly())
            m_AddEmitterRequested = true;
    }

    void VfxSystemEditorPanel::ContextMenu(const NodeCanvas::Hit& hit, glm::vec2)
    {
        if (hit.Type == NodeCanvas::Hit::Kind::Node && hit.Id != SystemNode && m_Document.GetMode() == Mode::System)
        {
            m_MenuEmitter          = hit.Id;
            m_EmitterMenuRequested = true;
        }
    }

    // ---------------------------------------------------------------------- drawing

    std::string VfxSystemEditorPanel::WindowName() const
    {
        return std::string(ICON_FA_WAND_MAGIC_SPARKLES "  ") + m_Document.File().filename().u8string() +
               (m_Document.IsDirty() ? " *" : "") + "###VfxSystem" + m_Document.File().u8string();
    }

    void VfxSystemEditorPanel::Draw()
    {
        if (!m_IsOpen)
            return;
        ImGui::SetNextWindowSize({1360.0f, 820.0f}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin(WindowName().c_str(), &open))
            DrawContents();
        else
            m_Focused = false;
        ImGui::End();
        if (!open)
            RequestClose();
        if (m_ClosePromptRequested)
        {
            ImGui::OpenPopup("Unsaved VFX system");
            m_ClosePromptRequested = false;
        }
        if (ImGui::BeginPopupModal("Unsaved VFX system", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Save the changes to %s before closing?", m_Document.File().filename().u8string().c_str());
            if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
                ResolveClose(UnsavedChanges::Choice::Save), ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Don't save"))
                ResolveClose(UnsavedChanges::Choice::Discard), ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ResolveClose(UnsavedChanges::Choice::Cancel), ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void VfxSystemEditorPanel::DrawContents()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        Tick(ImGui::GetTime());
        DrawToolbar();
        if (!m_IsOpen)
        {
            ImGui::TextDisabled("%s", m_Status.empty() ? "Open a .vfxsystem or .vfxemitter from the Content Browser." : m_Status.c_str());
            return;
        }
        using EditorTheme::Px;
        const ImVec2 avail    = ImGui::GetContentRegionAvail();
        const float  spacing  = ImGui::GetStyle().ItemSpacing.y;
        const float  overview = m_Document.GetMode() == Mode::Emitter ? Px(150.0f) : std::max(Px(160.0f), avail.y * 0.26f);
        const float  output   = m_ShowSpreadsheet ? std::max(Px(180.0f), avail.y * 0.3f) : Px(76.0f);
        const float  body     = std::max(avail.y - overview - output - 2 * spacing, Px(120.0f));
        const float  left     = std::floor(avail.x * 0.62f);
        // Left column: the overview over the stack. Right column: the preview over the parameters.
        ImGui::BeginGroup();
        DrawOverview(left, overview);
        ImGui::BeginChild("##stack", {left, body}, ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(ReadOnly());
        DrawStack();
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float column  = overview + body + spacing;
        const float preview = std::floor(std::max(overview, column * 0.58f));
        DrawPreview(preview);
        ImGui::BeginChild("##parameters", {0, std::max(Px(40.0f), column - preview - spacing)}, ImGuiChildFlags_Borders);
        DrawPreviewControls();
        ImGui::BeginDisabled(ReadOnly());
        DrawParameters();
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::EndGroup();
        // Bottom: the compile output or the attribute spreadsheet.
        if (ImGui::SmallButton(m_ShowSpreadsheet ? "Output" : ICON_FA_TABLE_COLUMNS "  Spreadsheet"))
            m_ShowSpreadsheet = !m_ShowSpreadsheet;
        ImGui::SameLine();
        if (m_ShowSpreadsheet)
            DrawSpreadsheet();
        else
            DrawOutput();
        if (m_ValueEdit && !ImGui::IsAnyItemActive())
            FinishEdit();
        DrawSearch();
        DrawCurvePopup();
        DrawMenus();
        Shortcuts();
    }

    void VfxSystemEditorPanel::DrawToolbar()
    {
        if (!m_IsOpen)
            return;
        const auto& palette = EditorTheme::Colors();
        ImGui::TextUnformatted(ICON_FA_WAND_MAGIC_SPARKLES);
        ImGui::SameLine();
        ImGui::TextUnformatted((m_Document.File().filename().u8string() + (m_Document.IsDirty() ? "  *" : "")).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_Document.GetMode() == Mode::Emitter ? "Emitter" : "System");
        ImGui::SameLine();
        if (ReadOnly())
        {
            ImGui::TextColored(palette.Warning, ICON_FA_LOCK "  Read-only built-in");
            ImGui::SameLine();
            ImGui::BeginDisabled(m_ProjectDirectory.empty());
            if (ImGui::Button(ICON_FA_COPY "  Duplicate to project"))
                DuplicateToProject(m_ProjectDirectory);
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::BeginDisabled(!m_Document.IsDirty());
            if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
                Save();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_Document.CanUndo());
            if (ImGui::Button("Undo"))
                Undo();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_Document.CanRedo());
            if (ImGui::Button("Redo"))
                Redo();
            ImGui::EndDisabled();
            if (m_Document.GetMode() == Mode::System)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PLUS "  Emitter"))
                    m_AddEmitterRequested = true;
            }
        }
        ImGui::SameLine();
        size_t errors = 0, warnings = 0;
        for (const auto& d : m_Diagnostics)
            (d.Warning ? warnings : errors)++;
        if (IsCompiling() || m_CompiledRevision != m_Document.Revision())
            ImGui::TextDisabled("Compiling...");
        else if (errors)
            ImGui::TextColored(palette.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %zu error%s", errors, errors == 1 ? "" : "s");
        else
            ImGui::TextColored(palette.Success, warnings ? "Compiles (%zu warnings)" : "Compiles", warnings);
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
    }

    void VfxSystemEditorPanel::DrawOverview(float width, float height)
    {
        const auto graph = BuildOverview();
        if (!m_Framed)
        {
            auto& view = m_Overview.GetView();
            view.Size  = {width, height};
            m_Overview.FrameAll(graph);
            m_Framed = true;
        }
        NodeCanvas::Draw("##overview", graph, m_Overview, *this, {width, height});
        // A single selected node chooses the stack.
        const auto& selection = m_Overview.Selection();
        if (selection.size() == 1 && *selection.begin() != m_Selected)
            m_Selected = *selection.begin();
    }

    bool VfxSystemEditorPanel::ValueEditor(const char* id, Vfx::Value& value, Vfx::Type kind, const Vfx::Parameter* spec)
    {
        using Vfx::Type;
        ImGui::PushID(id);
        bool changed = false;
        if (spec && kind == Type::Int && !spec->EnumNames.empty())
        {
            const auto& names = spec->EnumNames;
            int         index = std::clamp(value.Integer, 0, int(names.size()) - 1);
            if (ImGui::BeginCombo("##e", names[size_t(index)].c_str()))
            {
                for (size_t k = 0; k < names.size(); ++k)
                    if (ImGui::Selectable(names[k].c_str(), int(k) == value.Integer))
                        value.Integer = int(k), changed = true;
                ImGui::EndCombo();
            }
        }
        else
            switch (kind)
            {
                case Type::Float:
                    if (spec && spec->Range)
                        changed = ImGui::SliderFloat("##v", &value.Data.x, spec->Range->x, spec->Range->y, "%.3g");
                    else
                        changed = ImGui::DragFloat("##v", &value.Data.x, 0.01f, 0, 0, "%.3g");
                    if (changed)
                        value.Data = glm::vec4(value.Data.x);
                    break;
                case Type::Int:
                    if (spec && spec->Range)
                        changed = ImGui::SliderInt("##v", &value.Integer, int(spec->Range->x), int(spec->Range->y));
                    else
                        changed = ImGui::DragInt("##v", &value.Integer);
                    break;
                case Type::Bool:
                {
                    bool b        = value.Integer != 0;
                    changed       = ImGui::Checkbox("##v", &b);
                    value.Integer = b;
                    break;
                }
                case Type::Vec2:
                    changed = ImGui::DragFloat2("##v", &value.Data.x, 0.01f, 0, 0, "%.3g");
                    break;
                case Type::Vec3:
                case Type::Position:
                    changed = ImGui::DragFloat3("##v", &value.Data.x, 0.01f, 0, 0, "%.3g");
                    break;
                case Type::Vec4:
                case Type::Quat:
                    changed = ImGui::DragFloat4("##v", &value.Data.x, 0.01f, 0, 0, "%.3g");
                    break;
                case Type::Color:
                    changed = ImGui::ColorEdit4("##v", &value.Data.x, ImGuiColorEditFlags_Float);
                    break;
                case Type::IVec3:
                {
                    glm::ivec3 v(value.Data);
                    changed = ImGui::DragInt3("##v", &v.x);
                    if (changed)
                        value.Data = glm::vec4(glm::vec3(v), 0);
                    break;
                }
            }
        ImGui::PopID();
        value.Kind = kind;
        return changed && !ReadOnly();
    }

    // ---------------------------------------------------------------------- stack

    void VfxSystemEditorPanel::DrawStack()
    {
        auto& s = m_Document.Data();
        if (m_Selected == SystemNode || (m_Selected.empty() && m_Document.GetMode() == Mode::System))
        {
            DrawSystemProperties();
            return;
        }
        const auto* handle = FindHandle(s, m_Selected);
        if (!handle)
        {
            ImGui::TextDisabled("Select an emitter in the overview.");
            return;
        }
        Vfx::Emitter effective;
        try
        {
            effective = Effective(*handle, m_Context);
        }
        catch (const std::exception& e)
        {
            ImGui::TextColored(EditorTheme::Colors().Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %s", e.what());
            return;
        }
        // Copies: edits below may change the handle.
        const Vfx::EmitterHandle h = *handle;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.1f);
        ImGui::TextUnformatted(h.Name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        const auto layer = LayerOf(h);
        ImGui::TextDisabled("%s", layer == Layer::Own ? "Emitter" : layer == Layer::InlineChild ? "Inherited emitter (overrides)" :
                                                                                                    "Referenced emitter (handle overrides)");
        DrawEmitterProperties(h, effective);
        for (auto stage : {Stage::EmitterSpawn, Stage::EmitterUpdate, Stage::ParticleSpawn, Stage::ParticleUpdate})
            DrawStackSection(h, effective, {stage, {}}, StageTitle(stage));
        DrawHandlers(h, effective);
        DrawSimulationStages(h, effective);
        DrawRenderers(h, effective);
    }

    void VfxSystemEditorPanel::DrawSystemProperties()
    {
        auto& s = m_Document.Data();
        if (!PropertyGrid::BeginGroup("System Properties"))
            return;
        float warmup = s.WarmupTime, tick = s.WarmupTickDelta, maxDelta = s.MaxDeltaTime;
        const float zero = 0, thirty = 1.f / 30, tenth = 0.1f;
        if (PropertyGrid::Float("Warmup time", warmup, 0.05f, 0.0f, 600.0f, "%.2f s", &zero))
            Apply([&](Vfx::System& x, const Context&) { x.WarmupTime = std::max(0.0f, warmup); return Result::Done(); }, true);
        if (PropertyGrid::Float("Warmup tick", tick, 0.001f, 0.001f, 1.0f, "%.4f s", &thirty))
            Apply([&](Vfx::System& x, const Context&) { x.WarmupTickDelta = std::max(0.001f, tick); return Result::Done(); }, true);
        if (PropertyGrid::Float("Max delta time", maxDelta, 0.005f, 0.001f, 1.0f, "%.3f s", &tenth))
            Apply([&](Vfx::System& x, const Context&) { x.MaxDeltaTime = std::max(0.001f, maxDelta); return Result::Done(); }, true);
        int seed = int(s.Seed);
        const int one = 1;
        if (PropertyGrid::Int("Determinism seed", seed, 0, INT32_MAX, &one))
            Apply([&](Vfx::System& x, const Context&) { x.Seed = uint32_t(std::max(0, seed)); return Result::Done(); }, true);
        bool fixed = s.FixedBounds.has_value();
        if (PropertyGrid::Checkbox("Fixed bounds", fixed))
            Apply([&](Vfx::System& x, const Context&) {
                x.FixedBounds = fixed ? std::optional<Bounds>(Bounds {glm::vec3(-1), glm::vec3(1)}) : std::nullopt;
                return Result::Done();
            });
        if (s.FixedBounds)
        {
            glm::vec3 lo = s.FixedBounds->Min, hi = s.FixedBounds->Max;
            if (PropertyGrid::Vec3("Bounds min", lo, -1.0f, 0.05f))
                Apply([&](Vfx::System& x, const Context&) { x.FixedBounds->Min = glm::min(lo, x.FixedBounds->Max); return Result::Done(); }, true);
            if (PropertyGrid::Vec3("Bounds max", hi, 1.0f, 0.05f))
                Apply([&](Vfx::System& x, const Context&) { x.FixedBounds->Max = glm::max(hi, x.FixedBounds->Min); return Result::Done(); }, true);
        }
        PropertyGrid::Text("Scalability", "Per-platform scalability is not part of the system asset yet", true);
        PropertyGrid::EndGroup();
        for (auto stage : {Stage::SystemSpawn, Stage::SystemUpdate})
        {
            auto at = s.Stages.find(stage);
            if (at == s.Stages.end() || at->second.empty())
                continue;
            if (PropertyGrid::BeginGroup(StageTitle(stage)))
            {
                for (const auto& u : at->second)
                    PropertyGrid::Text(m_Library.NameOf(u.Script).c_str(), u.Enabled ? "Enabled" : "Off", true);
                PropertyGrid::EndGroup();
            }
        }
    }

    void VfxSystemEditorPanel::DrawEmitterProperties(const Vfx::EmitterHandle& h, const Vfx::Emitter& e)
    {
        const auto handle = h.Guid;
        const auto layer  = LayerOf(h);
        if (!PropertyGrid::BeginGroup("Emitter Properties", nullptr, nullptr, false))
            return;
        auto        settings   = SettingsOf(e);
        const auto  overridden = OverriddenSettings(h);
        const auto  edit = [&](bool continuous) {
            Apply([&](Vfx::System& s, const Context& c) { return SetEmitterSettings(s, c, handle, settings); }, continuous);
        };
        // A child's overridden setting shows a reset to the parent's value.
        const auto reset = [&](const char* name) {
            if (layer == Layer::InlineChild && PropertyGrid::RowReset(overridden.count(name) > 0))
                Apply([&](Vfx::System& s, const Context& c) { return ResetEmitterSetting(s, c, handle, name); });
        };
        ImGui::BeginDisabled(layer == Layer::Reference);
        const char* targets[] = {"CPU", "GPU"};
        int         target    = int(settings.Target);
        if (PropertyGrid::Row("Simulation", "CPU or GPU simulation", overridden.count("Target") > 0))
        {
            ImGui::SetNextItemWidth(-FLT_MIN - (overridden.count("Target") ? ImGui::GetFrameHeight() + 4 : 0));
            if (ImGui::Combo("##target", &target, targets, 2))
                settings.Target = Vfx::SimTarget(target), edit(false);
            reset("Target");
        }
        const char* spaces[] = {"Local", "World"};
        int         space    = int(settings.SimulationSpace);
        if (PropertyGrid::Row("Space", "Simulate in the owner's local space or in world space", overridden.count("Space") > 0))
        {
            ImGui::SetNextItemWidth(-FLT_MIN - (overridden.count("Space") ? ImGui::GetFrameHeight() + 4 : 0));
            if (ImGui::Combo("##space", &space, spaces, 2))
                settings.SimulationSpace = Vfx::Space(space), edit(false);
            reset("Space");
        }
        int capacity = int(settings.Capacity);
        if (PropertyGrid::Row("Capacity", "The most particles alive at once", overridden.count("Capacity") > 0))
        {
            ImGui::SetNextItemWidth(-FLT_MIN - (overridden.count("Capacity") ? ImGui::GetFrameHeight() + 4 : 0));
            if (ImGui::DragInt("##capacity", &capacity, 4.0f, 1, 16777216))
                settings.Capacity = uint32_t(std::clamp(capacity, 1, 16777216)), edit(true);
            reset("Capacity");
        }
        if (PropertyGrid::Row("Deterministic", "Same seed, same result", overridden.count("Deterministic") > 0))
        {
            if (ImGui::Checkbox("##deterministic", &settings.Deterministic))
                edit(false);
            reset("Deterministic");
        }
        const char* boundsModes[] = {"Dynamic", "Fixed"};
        int         boundsMode    = settings.FixedBounds ? 1 : 0;
        if (PropertyGrid::Row("Bounds", "Fixed bounds or bounds measured from the particles", overridden.count("FixedBounds") > 0))
        {
            ImGui::SetNextItemWidth(-FLT_MIN - (overridden.count("FixedBounds") ? ImGui::GetFrameHeight() + 4 : 0));
            if (ImGui::Combo("##bounds", &boundsMode, boundsModes, 2))
            {
                settings.FixedBounds = boundsMode ? std::optional<Bounds>(Bounds {glm::vec3(-1), glm::vec3(1)}) : std::nullopt;
                edit(false);
            }
            reset("FixedBounds");
        }
        if (settings.FixedBounds)
        {
            glm::vec3 lo = settings.FixedBounds->Min, hi = settings.FixedBounds->Max;
            if (PropertyGrid::Vec3("Bounds min", lo, -1.0f, 0.05f))
                settings.FixedBounds->Min = glm::min(lo, hi), edit(true);
            if (PropertyGrid::Vec3("Bounds max", hi, 1.0f, 0.05f))
                settings.FixedBounds->Max = glm::max(hi, lo), edit(true);
        }
        const float noCull = 0;
        if (PropertyGrid::Float("Cull distance", settings.CullDistance, 0.5f, 0.0f, 100000.0f, "%.1f", &noCull))
            edit(true);
        const char* policies[] = {"Pause spawning", "Continue"};
        int         policy     = int(settings.DistancePolicy);
        if (PropertyGrid::Combo("Beyond cull", policies, 2, policy))
            settings.DistancePolicy = Vfx::CullPolicy(policy), edit(false);
        ImGui::EndDisabled();
        if (layer == Layer::InlineChild || layer == Layer::Reference)
        {
            const AssetID parent = layer == Layer::InlineChild ? h.Inline->Parent : h.Reference;
            std::string   name   = parent.ToString();
            if (m_ProjectEmitters)
                for (const auto& [id, file] : m_ProjectEmitters())
                    if (id == parent)
                        name = file.stem().u8string();
            PropertyGrid::Text(layer == Layer::InlineChild ? "Parent" : "Reference", name, true);
        }
        // Event channels this emitter writes.
        std::string channels;
        for (const auto& c : e.Channels)
            channels += (channels.empty() ? "" : ", ") + c.Name;
        if (PropertyGrid::Row("Event channels", "Channels event generator modules write to"))
        {
            ImGui::TextUnformatted(channels.empty() ? "None" : channels.c_str());
            const auto layouts = EventLayouts(m_Document.Data(), m_Context, handle);
            if (!layouts.empty() && layer == Layer::Own)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_PLUS "##channel"))
                    ImGui::OpenPopup("##addchannel");
                if (ImGui::BeginPopup("##addchannel"))
                {
                    for (const auto& l : layouts)
                        if (ImGui::MenuItem(l.c_str()))
                            Apply([&](Vfx::System& s, const Context& c) { return AddEventChannel(s, c, handle, l); });
                    ImGui::EndPopup();
                }
            }
        }
        PropertyGrid::EndGroup();
    }

    void VfxSystemEditorPanel::DrawStackSection(const Vfx::EmitterHandle& h, const Vfx::Emitter& e, const StackRef& ref,
                                                const char* title)
    {
        const auto* stack = FindStack(e, ref);
        if (!stack)
            return;
        const bool own = !title; // Handler and simulation stage stacks draw inside their own group.
        if (!own && !PropertyGrid::BeginGroup(title))
            return;
        for (size_t i = 0; i < stack->size(); ++i)
            DrawModuleRow(h, e, ref, (*stack)[i], i);
        PropertyGrid::BeginFree();
        ImGui::PushID(int(ref.Kind));
        ImGui::PushID(ref.Container.c_str());
        if (ImGui::Button(ICON_FA_PLUS "  Add module", {-FLT_MIN, 0}))
            OpenModuleSearch(ref, stack->size());
        // Dropping a row here moves it to the end of this stack.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(UsePayload))
            {
                const std::string use(static_cast<const char*>(payload->Data), size_t(payload->DataSize));
                const auto        handle = h.Guid;
                Apply([&](Vfx::System& s, const Context& c) { return MoveModule(s, c, handle, use, ref, SIZE_MAX); });
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        ImGui::PopID();
        PropertyGrid::EndFree();
        if (!own)
            PropertyGrid::EndGroup();
    }

    void VfxSystemEditorPanel::DrawModuleRow(const Vfx::EmitterHandle& h, const Vfx::Emitter& e, const StackRef& ref,
                                             const Vfx::ModuleUse& use, size_t index)
    {
        using EditorTheme::Px;
        const auto& palette = EditorTheme::Colors();
        const auto* info    = m_Library.Find(use.Script);
        const auto  name    = info ? info->Name : m_Library.NameOf(use.Script);
        const auto  origin  = ModuleOrigin(h, use.Guid);
        const auto  handle  = h.Guid;
        ImGui::PushID(use.Guid.c_str());
        const bool expanding = m_ExpandPending.erase(use.Guid) > 0;
        if (expanding)
        {
            ImGui::PushID(name.c_str());
            ImGui::GetStateStorage()->SetBool(ImGui::GetID("##open"), true);
            ImGui::PopID();
        }
        bool enabled = use.Enabled, toggled = false;
        PropertyGrid::SetNextGroupTrailer(Px(120.0f));
        const bool open = PropertyGrid::BeginGroup(name.c_str(), &enabled, &toggled, false);
        if (expanding)
            ImGui::SetScrollHereY(0.4f); // A row opened for the user (added, or a diagnostic) comes into view.
        // The selected (or newly expanded) use gets the viewport handles.
        if (expanding && VfxGizmos::HasHandles(use, m_Context))
            m_GizmoUse = use.Guid;
        ImVec2     bmin, bmax;
        if (PropertyGrid::LastGroupHeading(bmin, bmax))
        {
            auto* draw = ImGui::GetWindowDrawList();
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(bmin, bmax) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                m_GizmoUse = use.Guid;
            if (m_GizmoUse == use.Guid)
            {
                // Selected: an accent edge in the handles' colour, and a marker when it has handles.
                const ImU32 accent = ImGui::GetColorU32(ImVec4(VfxGizmos::Accent.r, VfxGizmos::Accent.g, VfxGizmos::Accent.b, 1.0f));
                draw->AddRectFilled({bmin.x - Px(1.0f), bmin.y}, {bmin.x + Px(1.0f), bmax.y}, accent);
                if (VfxGizmos::HasHandles(use, m_Context))
                {
                    const float y = bmin.y + (bmax.y - bmin.y - ImGui::GetFontSize()) * 0.5f;
                    draw->AddText({bmax.x - Px(140.0f), y}, accent, ICON_FA_ARROWS_MOVE);
                    if (ImGui::IsMouseHoveringRect({bmax.x - Px(142.0f), bmin.y}, {bmax.x - Px(124.0f), bmax.y}))
                        ImGui::SetTooltip("Its handles show in the preview");
                }
            }
            // Category colour on the left edge of the band.
            const uint32_t category = VfxModuleEditing::CategoryColor(info ? info->Data.Category : std::string());
            draw->AddRectFilled({bmin.x - Px(4.0f), bmin.y}, {bmin.x - Px(1.0f), bmax.y}, category | 0xff000000);
            // The inheritance marker and the error count at the right end.
            const char* tag = origin == Origin::Inherited  ? "Inherited" :
                              origin == Origin::Overridden ? ICON_FA_PEN " Overridden" :
                              origin == Origin::Added      ? ICON_FA_PLUS " Added" :
                                                             "";
            const ImVec4 tagColor = origin == Origin::Overridden ? palette.AccentHover : origin == Origin::Added ? palette.Success : palette.TextDim;
            const float  textY    = bmin.y + (bmax.y - bmin.y - ImGui::GetFontSize()) * 0.5f;
            if (*tag)
                draw->AddText({bmax.x - Px(24.0f) - ImGui::CalcTextSize(tag).x, textY}, ImGui::GetColorU32(tagColor), tag);
            size_t errors = 0;
            std::string message;
            for (const auto& d : m_Diagnostics)
                if (d.Emitter == handle && d.Use == use.Guid && !d.Warning)
                    ++errors, message = d.Message;
            if (errors)
            {
                const float x = bmax.x - Px(118.0f);
                draw->AddText({x, textY}, ImGui::GetColorU32(palette.Danger), ICON_FA_CIRCLE_EXCLAMATION);
                if (ImGui::IsMouseHoveringRect({x, bmin.y}, {x + Px(16.0f), bmax.y}))
                    ImGui::SetTooltip("%s", message.c_str());
            }
            if (m_Highlight.Use == use.Guid && m_Now < m_Highlight.Until)
            {
                draw->AddRect({bmin.x - Px(4.0f), bmin.y}, {bmax.x + Px(4.0f), bmax.y}, ImGui::GetColorU32(palette.Warning), Px(3.0f), 0, 2.0f);
                if (m_Highlight.Scroll && m_Highlight.Input.empty())
                {
                    ImGui::SetScrollHereY(0.3f);
                    m_Highlight.Scroll = false;
                }
            }
            // The grip: drag to reorder (within the stack, or into another stack that allows it).
            const ImRect grip({bmax.x - Px(20.0f), bmin.y}, {bmax.x, bmax.y});
            const ImGuiID gripID = ImGui::GetID("##grip");
            if (ImGui::ItemAdd(grip, gripID))
            {
                bool hovered = false, held = false;
                ImGui::ButtonBehavior(grip, gripID, &hovered, &held);
                draw->AddText({grip.Min.x + Px(6.0f), textY}, ImGui::GetColorU32(hovered || held ? palette.Text : palette.TextDim),
                              ICON_FA_ELLIPSIS_VERTICAL);
                if (hovered && !held)
                    ImGui::SetTooltip("Drag to reorder; right-click the band for more");
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload(UsePayload, use.Guid.data(), use.Guid.size());
                    ImGui::Text("%s", name.c_str());
                    ImGui::EndDragDropSource();
                }
            }
            // Drop onto a band: the dragged row goes before it.
            if (ImGui::BeginDragDropTargetCustom(ImRect(bmin, bmax), ImGui::GetID("##drop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(UsePayload))
                {
                    const std::string dragged(static_cast<const char*>(payload->Data), size_t(payload->DataSize));
                    Apply([&](Vfx::System& s, const Context& c) { return MoveModule(s, c, handle, dragged, ref, index); });
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(bmin, bmax) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                m_MenuUse          = use.Guid;
                m_UseMenuRequested = true;
            }
        }
        if (toggled)
            Apply([&](Vfx::System& s, const Context& c) { return SetModuleEnabled(s, c, handle, use.Guid, enabled); });
        if (open)
        {
            if (info)
                DrawInputs(h, e, ref, use, info->Data, {}, 0);
            else
                PropertyGrid::Text("Script", "Missing " + use.Script.ToString(), true);
            PropertyGrid::EndGroup();
        }
        ImGui::PopID();
    }

    void VfxSystemEditorPanel::DrawInputs(const Vfx::EmitterHandle& h, const Vfx::Emitter& e, const StackRef& ref,
                                          const Vfx::ModuleUse& use, const Vfx::Module& module, const InputPath& prefix, int depth)
    {
        using EditorTheme::Px;
        const auto& palette = EditorTheme::Colors();
        const auto  handle  = h.Guid;
        const auto  inputs  = ModuleInputs(module);
        std::set<std::string> spaces;
        for (const auto& info : inputs)
            if (!info.SpaceInput.empty())
                spaces.insert(info.SpaceInput);
        const auto set = [&](const InputPath& path, const Vfx::Input& value, bool continuous) {
            Apply([&](Vfx::System& s, const Context& c) { return SetInput(s, c, handle, use.Guid, path, value); }, continuous);
        };
        for (const auto& info : inputs)
        {
            const auto& spec = info.Spec;
            if (spaces.count(spec.Name))
                continue; // Drawn as the toggle of its position input.
            InputPath path = prefix;
            path.push_back(spec.Name);
            const auto current    = GetInput(use, path);
            const bool overridden = depth == 0 && InputOverridden(h, use.Guid, spec.Name);
            const bool modified   = LayerOf(h) == Layer::Own || ModuleOrigin(h, use.Guid) == Origin::Added ? current.has_value() : overridden;
            ImGui::PushID(spec.Name.c_str());
            std::string error;
            for (const auto& d : m_Diagnostics)
                if (d.Emitter == handle && d.Use == use.Guid && !d.Warning &&
                    ((depth == 0 && d.Input == spec.Name && d.Nested.empty()) ||
                     (depth == 1 && !prefix.empty() && d.Input == prefix[0] && d.Nested == spec.Name)))
                    error = d.Message;
            const std::string label = (error.empty() ? "" : ICON_FA_CIRCLE_EXCLAMATION " ") + Short(spec.Name);
            if (PropertyGrid::Row(label.c_str(), error.empty() ? Vfx::TypeName(spec.Kind) : error.c_str(), modified))
            {
                const bool highlighted = m_Highlight.Use == use.Guid && m_Highlight.Input == spec.Name && m_Now < m_Highlight.Until;
                if (highlighted)
                {
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled({at.x - Px(2.0f), at.y}, {at.x + ImGui::GetContentRegionAvail().x, at.y + ImGui::GetFrameHeight()},
                                                              ImGui::GetColorU32(ImVec4(palette.Warning.x, palette.Warning.y, palette.Warning.z, 0.25f)));
                    if (m_Highlight.Scroll)
                    {
                        ImGui::SetScrollHereY(0.3f);
                        m_Highlight.Scroll = false;
                    }
                }
                const float button = ImGui::GetFrameHeight();
                const float width  = std::max(Px(40.0f), ImGui::GetContentRegionAvail().x - button - Px(4.0f) - (modified ? button + Px(4.0f) : 0.0f));
                ImGui::SetNextItemWidth(width);
                if (!error.empty())
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImVec4(palette.Danger.x, palette.Danger.y, palette.Danger.z, 0.35f)));
                if (current && current->Kind == Vfx::InputKind::Link)
                {
                    if (ImGui::Button((ICON_FA_LINK "  " + current->Link).c_str(), {width, 0}))
                        ImGui::OpenPopup("##inputmenu");
                }
                else if (current && current->Kind == Vfx::InputKind::DynamicInput)
                {
                    if (ImGui::Button(("f(x)  " + m_Library.NameOf(current->Script)).c_str(), {width, 0}))
                        ImGui::OpenPopup("##inputmenu");
                }
                else if (info.Interface == "Curve" || info.Interface == "Gradient")
                {
                    const auto binding = current && current->Kind == Vfx::InputKind::DataInterface ? current->Interface : info.DefaultBinding;
                    const ImVec2 min   = ImGui::GetCursorScreenPos();
                    const float  high  = ImGui::GetFrameHeight();
                    ImGui::InvisibleButton("##curve", {width, high});
                    const bool clicked = ImGui::IsItemClicked();
                    auto*      draw    = ImGui::GetWindowDrawList();
                    if (info.Interface == "Gradient")
                    {
                        ImGui::SetCursorScreenPos(min);
                        GradientEditor::Preview(Gradient::FromBinding(binding), width, high);
                    }
                    else
                    {
                        draw->AddRectFilled(min, {min.x + width, min.y + high}, ImGui::GetColorU32(palette.Inset), Px(3.0f));
                        const auto curve = CurveFromBinding(binding);
                        float      low = 0, top = 1;
                        for (const auto& k : curve.Keys)
                            low = std::min(low, k.Value.x), top = std::max(top, k.Value.x);
                        ImVec2 last;
                        for (int i = 0; i <= 32; ++i)
                        {
                            const float  t = float(i) / 32;
                            const float  v = curve.Keys.empty() ? 0 : curve.Sample(t).x;
                            const ImVec2 p {min.x + t * width, min.y + high - 3 - (v - low) / std::max(top - low, 1e-4f) * (high - 6)};
                            if (i)
                                draw->AddLine(last, p, ImGui::GetColorU32(palette.Accent), 1.5f);
                            last = p;
                        }
                    }
                    if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(min, {min.x + width, min.y + high}))
                        ImGui::SetTooltip("Click to edit the %s", info.Interface == "Gradient" ? "gradient" : "curve");
                    if (clicked)
                        info.Interface == "Gradient" ? OpenGradientEditor(use.Guid, path) : OpenCurveEditor(use.Guid, path);
                    // A curve module dropped from the Content Browser references its table instead.
                    if (ImGui::BeginDragDropTargetCustom(ImRect(min, {min.x + width, min.y + high}), ImGui::GetID("##curvedrop")))
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("asset_payload"))
                            if (auto dropped = DroppedAsset(*payload, AssetType::VfxModule))
                                Apply([&](Vfx::System& s, const Context& c) {
                                    return PickInputAsset(s, c, handle, use.Guid, path, *dropped, AssetType::VfxModule);
                                });
                        ImGui::EndDragDropTarget();
                    }
                }
                else if (info.Interface == "Grid3D")
                {
                    auto binding = current && current->Kind == Vfx::InputKind::DataInterface ? current->Interface : Vfx::InterfaceBinding {};
                    binding.Kind = "Grid3D";
                    std::string label2 = binding.Source.empty() ? "Choose a grid" : Short(binding.Source) + " · " + binding.Channel;
                    if (ImGui::BeginCombo("##grid", label2.c_str()))
                    {
                        for (const auto& [name, grid] : e.DataInterfaces)
                            if (grid.Kind == "Grid3D")
                                for (const auto& attribute : grid.GridAttributes)
                                    if (ImGui::Selectable((Short(name) + " · " + attribute.Name).c_str(),
                                                          binding.Source == name && binding.Channel == attribute.Name))
                                    {
                                        auto chosen        = grid;
                                        chosen.GridAttributes.clear();
                                        chosen.Source      = name;
                                        chosen.Channel     = attribute.Name;
                                        Vfx::Input input;
                                        input.Kind      = Vfx::InputKind::DataInterface;
                                        input.Interface = chosen;
                                        set(path, input, false);
                                    }
                        ImGui::EndCombo();
                    }
                }
                else if (info.Interface == "Events")
                {
                    auto binding = current && current->Kind == Vfx::InputKind::DataInterface ? current->Interface : Vfx::InterfaceBinding {};
                    if (ImGui::BeginCombo("##channel", binding.Channel.empty() ? "Default channel" : binding.Channel.c_str()))
                    {
                        for (const auto& c : e.Channels)
                            if (ImGui::Selectable(c.Name.c_str(), binding.Channel == c.Name))
                            {
                                Vfx::Input input;
                                input.Kind              = Vfx::InputKind::DataInterface;
                                input.Interface.Kind    = "Events";
                                input.Interface.Channel = c.Name;
                                set(path, input, false);
                            }
                        ImGui::EndCombo();
                    }
                }
                else if (info.Interface == "EmitterParticles")
                {
                    auto binding = current && current->Kind == Vfx::InputKind::DataInterface ? current->Interface : Vfx::InterfaceBinding {};
                    const auto* source = FindHandle(m_Document.Data(), binding.SourceEmitter);
                    if (ImGui::BeginCombo("##source", source ? source->Name.c_str() : "Choose an emitter"))
                    {
                        for (const auto& other : m_Document.Data().Emitters)
                            if (other.Guid != handle && ImGui::Selectable(other.Name.c_str(), other.Guid == binding.SourceEmitter))
                            {
                                Vfx::Input input;
                                input.Kind                    = Vfx::InputKind::DataInterface;
                                input.Interface.Kind          = "EmitterParticles";
                                input.Interface.SourceEmitter = other.Guid;
                                set(path, input, false);
                            }
                        ImGui::EndCombo();
                    }
                }
                else if (!info.Interface.empty() && info.Interface != "ForceFields")
                {
                    // Asset-backed bindings (vector fields, meshes, textures): a picker of that type.
                    auto binding = current && current->Kind == Vfx::InputKind::DataInterface ? current->Interface : Vfx::InterfaceBinding {};
                    if (binding.Kind.empty())
                        binding.Kind = info.Interface == "Mesh" ? "StaticMesh" : info.Interface;
                    const auto type  = BindingAssetType(binding.Kind);
                    AssetID    asset = binding.Asset;
                    if (type == AssetType::None)
                        ImGui::TextDisabled("%s", binding.Kind.c_str());
                    else if (AssetCombo("##asset", type, asset, width))
                        Apply([&](Vfx::System& s, const Context& c) { return PickInputAsset(s, c, handle, use.Guid, path, asset, type); });
                }
                else if (info.Interface == "ForceFields")
                    ImGui::TextDisabled("Scene force fields");
                else
                {
                    Vfx::Value value = current && current->Kind == Vfx::InputKind::Literal ? current->Literal : spec.Default.value_or(Zero(spec.Kind));
                    float      valueWidth = width;
                    // A position with a space input gets its Local/World toggle next to it.
                    const Vfx::Parameter* spaceSpec = nullptr;
                    for (const auto& other : inputs)
                        if (other.Spec.Name == info.SpaceInput)
                            spaceSpec = &other.Spec;
                    if (spaceSpec)
                        valueWidth -= Px(64.0f);
                    ImGui::SetNextItemWidth(valueWidth);
                    if (ValueEditor("##value", value, spec.Kind, &spec))
                    {
                        Vfx::Input input;
                        input.Literal = value;
                        set(path, input, true);
                    }
                    if (spaceSpec)
                    {
                        InputPath spacePath = prefix;
                        spacePath.push_back(spaceSpec->Name);
                        auto  spaceInput = GetInput(use, spacePath);
                        int   spaceValue = spaceInput && spaceInput->Kind == Vfx::InputKind::Literal ? spaceInput->Literal.Integer :
                                                                                                        spaceSpec->Default.value_or(Vfx::Value::Int(0)).Integer;
                        ImGui::SameLine(0, Px(4.0f));
                        if (ImGui::Button((spaceSpec->EnumNames[size_t(std::clamp(spaceValue, 0, 1))] + "##space").c_str(), {Px(60.0f), 0}))
                        {
                            Vfx::Input input;
                            input.Literal = Vfx::Value::Int(1 - std::clamp(spaceValue, 0, 1));
                            set(spacePath, input, false);
                        }
                    }
                }
                if (!error.empty())
                    ImGui::PopStyleColor();
                // Parameters dropped on the value link the input.
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ParameterPayload))
                    {
                        const std::string parameter(static_cast<const char*>(payload->Data), size_t(payload->DataSize));
                        Apply([&](Vfx::System& s, const Context& c) { return LinkInput(s, c, handle, use.Guid, path, parameter); });
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine(0, Px(4.0f));
                if (ImGui::Button(ICON_FA_CARET_DOWN "##menu", {button, button}))
                    ImGui::OpenPopup("##inputmenu");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Link to a parameter, use a dynamic input or reset");
                if (ImGui::BeginPopup("##inputmenu"))
                {
                    if (ImGui::BeginMenu(ICON_FA_LINK "  Link to parameter"))
                    {
                        bool any = false;
                        for (const auto& p : ListParameters(m_Document.Data(), m_Context, handle))
                            if (p.Kind == spec.Kind && p.Namespace != "")
                            {
                                any = true;
                                if (ImGui::MenuItem(p.Name.c_str()))
                                    Apply([&](Vfx::System& s, const Context& c) { return LinkInput(s, c, handle, use.Guid, path, p.Name); });
                            }
                        if (!any)
                            ImGui::TextDisabled("No %s parameters", Vfx::TypeName(spec.Kind));
                        ImGui::EndMenu();
                    }
                    if (info.Interface.empty() && ImGui::MenuItem("f(x)  Dynamic input..."))
                        OpenDynamicInputSearch(use.Guid, path);
                    if (current && current->Kind == Vfx::InputKind::DynamicInput)
                        if (const auto* nested = m_Library.Find(current->Script); nested && nested->File.extension() == ".vfxmodule")
                            if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Open dynamic input") && m_OpenModule)
                                m_OpenModule(nested->File);
                    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Reset", nullptr, false, modified || current.has_value()))
                        Apply([&](Vfx::System& s, const Context& c) { return ResetInput(s, c, handle, use.Guid, path); });
                    ImGui::EndPopup();
                }
                if (PropertyGrid::RowReset(modified))
                    Apply([&](Vfx::System& s, const Context& c) { return ResetInput(s, c, handle, use.Guid, path); });
            }
            // A dynamic input's own inputs, nested under it.
            if (current && current->Kind == Vfx::InputKind::DynamicInput && depth < 6)
                if (const auto* nested = m_Library.Find(current->Script))
                {
                    if (PropertyGrid::BeginGroup(("f(x) " + nested->Name).c_str()))
                    {
                        DrawInputs(h, e, ref, use, nested->Data, path, depth + 1);
                        PropertyGrid::EndGroup();
                    }
                }
            ImGui::PopID();
        }
        if (inputs.empty() && depth == 0)
            PropertyGrid::Text("Inputs", "None", true);
    }

    void VfxSystemEditorPanel::DrawHandlers(const Vfx::EmitterHandle& h, const Vfx::Emitter& e)
    {
        const auto handle = h.Guid;
        auto&      s      = m_Document.Data();
        if (!PropertyGrid::BeginGroup(StageTitle(Stage::EventHandler)))
            return;
        for (const auto& handler : e.Handlers)
        {
            ImGui::PushID(handler.Guid.c_str());
            const auto* source  = FindHandle(s, handler.SourceEmitter);
            std::string title   = "From " + (source ? source->Name : handler.SourceEmitter) + " \xC2\xB7 " + handler.Channel;
            bool        enabled = handler.Enabled, toggled = false;
            if (PropertyGrid::BeginGroup(title.c_str(), &enabled, &toggled))
            {
                auto       settings = handler;
                const auto edit     = [&](bool continuous) {
                    Apply([&](Vfx::System& x, const Context& c) { return SetEventHandler(x, c, handle, settings); }, continuous);
                };
                if (PropertyGrid::Row("Source", "The emitter whose events spawn or update these particles"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##source", source ? source->Name.c_str() : "Choose an emitter"))
                    {
                        for (const auto& other : s.Emitters)
                            if (other.Guid != handle && ImGui::Selectable(other.Name.c_str(), other.Guid == handler.SourceEmitter))
                            {
                                settings.SourceEmitter = other.Guid;
                                const auto channels    = EventChannels(s, m_Context, other.Guid);
                                settings.Channel       = channels.empty() ? std::string() : channels.front();
                                edit(false);
                            }
                        ImGui::EndCombo();
                    }
                }
                if (PropertyGrid::Row("Event", "The source's event channel"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##event", handler.Channel.c_str()))
                    {
                        for (const auto& c : EventChannels(s, m_Context, handler.SourceEmitter))
                            if (ImGui::Selectable(c.c_str(), c == handler.Channel))
                                settings.Channel = c, edit(false);
                        ImGui::EndCombo();
                    }
                }
                const char* modes[] = {"Spawn particles", "Apply to every particle"};
                int         mode    = int(handler.Mode);
                if (PropertyGrid::Combo("Mode", modes, 2, mode))
                    settings.Mode = Vfx::EventMode(mode), edit(false);
                int spawn = int(std::min<uint32_t>(handler.SpawnNumber, INT32_MAX));
                if (settings.Mode == Vfx::EventMode::SpawnedParticles && PropertyGrid::Int("Spawn count", spawn, 0, 100000))
                    settings.SpawnNumber = uint32_t(std::max(0, spawn)), edit(true);
                bool limited = handler.MaxEventsPerFrame != UINT32_MAX;
                if (PropertyGrid::Checkbox("Limit events", limited))
                    settings.MaxEventsPerFrame = limited ? 64 : UINT32_MAX, edit(false);
                if (limited)
                {
                    int most = int(std::min<uint32_t>(handler.MaxEventsPerFrame, INT32_MAX));
                    if (PropertyGrid::Int("Events per frame", most, 1, 1000000))
                        settings.MaxEventsPerFrame = uint32_t(std::max(1, most)), edit(true);
                }
                DrawStackSection(h, e, {Stage::EventHandler, handler.Guid}, nullptr);
                PropertyGrid::BeginFree();
                if (ImGui::SmallButton(ICON_FA_TRASH "  Remove handler"))
                    Apply([&](Vfx::System& x, const Context& c) { return RemoveEventHandler(x, c, handle, handler.Guid); });
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            if (toggled)
            {
                auto settings    = handler;
                settings.Enabled = enabled;
                Apply([&](Vfx::System& x, const Context& c) { return SetEventHandler(x, c, handle, settings); });
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::Button(ICON_FA_PLUS "  Add event handler", {-FLT_MIN, 0}))
        {
            std::string source;
            for (const auto& other : s.Emitters)
                if (other.Guid != handle && source.empty())
                    source = other.Guid;
            const auto channels = EventChannels(s, m_Context, source);
            if (source.empty())
                m_Status = "Event handlers read another emitter's events; add a second emitter first.";
            else
                Apply([&](Vfx::System& x, const Context& c) {
                    return AddEventHandler(x, c, handle, source, channels.empty() ? std::string() : channels.front());
                });
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void VfxSystemEditorPanel::DrawSimulationStages(const Vfx::EmitterHandle& h, const Vfx::Emitter& e)
    {
        const auto handle = h.Guid;
        if (!PropertyGrid::BeginGroup(StageTitle(Stage::SimulationStage)))
            return;
        for (size_t i = 0; i < e.SimulationStages.size(); ++i)
        {
            const auto& stage = e.SimulationStages[i];
            ImGui::PushID(stage.Guid.c_str());
            std::string error;
            for (const auto& d : m_Diagnostics)
                if (d.Emitter == handle && d.SimulationStage == stage.Guid && d.Use.empty() && !d.Warning)
                    error = d.Message;
            const std::string title = (error.empty() ? "" : ICON_FA_CIRCLE_EXCLAMATION " ") + stage.Name;
            if (PropertyGrid::BeginGroup(title.c_str()))
            {
                auto       settings = stage;
                const auto edit     = [&](bool continuous) {
                    Apply([&](Vfx::System& x, const Context& c) { return SetSimulationStage(x, c, handle, settings); }, continuous);
                };
                if (!error.empty())
                    PropertyGrid::Text("Error", error);
                std::string name = stage.Name;
                if (PropertyGrid::InputText("Name", name) && !name.empty())
                    settings.Name = name, edit(true);
                if (PropertyGrid::Row("Iterate over", "Particles, or every cell of a Grid3D data interface"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const std::string current = stage.Source == Vfx::IterationSource::Particles ? "Particles" : "Grid3D " + Short(stage.DataInterface);
                    if (ImGui::BeginCombo("##source", current.c_str()))
                    {
                        if (ImGui::Selectable("Particles", stage.Source == Vfx::IterationSource::Particles))
                            settings.Source = Vfx::IterationSource::Particles, edit(false);
                        for (const auto& [diName, binding] : e.DataInterfaces)
                            if (binding.Kind == "Grid3D" &&
                                ImGui::Selectable(("Grid3D " + Short(diName)).c_str(), stage.DataInterface == diName))
                                settings.Source = Vfx::IterationSource::DataInterface, settings.DataInterface = diName, edit(false);
                        ImGui::EndCombo();
                    }
                }
                int iterations = int(stage.Iterations);
                const int one  = 1;
                if (PropertyGrid::Int("Iterations", iterations, 1, 64, &one))
                    settings.Iterations = uint32_t(std::clamp(iterations, 1, 64)), edit(true);
                if (PropertyGrid::Row("Enabled by", "A bool emitter or user parameter; empty runs the stage every frame"))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##enabled", stage.Enabled.empty() ? "Always" : stage.Enabled.c_str()))
                    {
                        if (ImGui::Selectable("Always", stage.Enabled.empty()))
                            settings.Enabled.clear(), edit(false);
                        for (const auto& p : ListParameters(m_Document.Data(), m_Context, handle))
                            if (p.Kind == Vfx::Type::Bool && (p.Namespace == "Emitter" || p.Namespace == "User") &&
                                ImGui::Selectable(p.Name.c_str(), p.Name == stage.Enabled))
                                settings.Enabled = p.Name, edit(false);
                        ImGui::EndCombo();
                    }
                }
                DrawStackSection(h, e, {Stage::SimulationStage, stage.Guid}, nullptr);
                PropertyGrid::BeginFree();
                if (ImGui::SmallButton(ICON_FA_ARROW_UP "##up"))
                    Apply([&](Vfx::System& x, const Context& c) { return MoveSimulationStage(x, c, handle, stage.Guid, -1); });
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "##down"))
                    Apply([&](Vfx::System& x, const Context& c) { return MoveSimulationStage(x, c, handle, stage.Guid, 1); });
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH "  Remove stage"))
                    Apply([&](Vfx::System& x, const Context& c) { return RemoveSimulationStage(x, c, handle, stage.Guid); });
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::Button(ICON_FA_PLUS "  Add simulation stage", {-FLT_MIN, 0}))
            ImGui::OpenPopup("##addstage");
        if (ImGui::BeginPopup("##addstage"))
        {
            if (ImGui::MenuItem("Over particles"))
                Apply([&](Vfx::System& x, const Context& c) {
                    return AddSimulationStage(x, c, handle, "ParticleStage", Vfx::IterationSource::Particles, {}, 1);
                });
            for (const auto& [diName, binding] : e.DataInterfaces)
                if (binding.Kind == "Grid3D" && ImGui::MenuItem(("Over grid " + Short(diName)).c_str()))
                    Apply([&](Vfx::System& x, const Context& c) {
                        return AddSimulationStage(x, c, handle, "GridStage", Vfx::IterationSource::DataInterface, diName, 1);
                    });
            ImGui::EndPopup();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void VfxSystemEditorPanel::DrawRenderers(const Vfx::EmitterHandle& h, const Vfx::Emitter& e)
    {
        const auto handle = h.Guid;
        if (!PropertyGrid::BeginGroup("Render"))
            return;
        for (const auto& renderer : e.Renderers)
        {
            ImGui::PushID(renderer.Guid.c_str());
            std::string error;
            for (const auto& d : m_Diagnostics)
                if (d.Emitter == handle && d.Renderer == renderer.Guid && !d.Warning)
                    error = d.Message;
            const std::string title = (error.empty() ? "" : ICON_FA_CIRCLE_EXCLAMATION " ") + std::string(RendererName(renderer.Kind));
            if (PropertyGrid::BeginGroup(title.c_str()))
            {
                auto       r    = renderer;
                const auto edit = [&](bool continuous) {
                    Apply([&](Vfx::System& x, const Context& c) { return SetRenderer(x, c, handle, r); }, continuous);
                };
                if (!error.empty())
                    PropertyGrid::Text("Error", error);
                if (AssetRow("Material", AssetType::Material, r.Material))
                    edit(false);
                if (r.Kind == Vfx::RendererKind::Mesh)
                    for (size_t k = 0; k < r.Meshes.size(); ++k)
                    {
                        ImGui::PushID(int(k));
                        if (AssetRow("Mesh", AssetType::StaticMesh, r.Meshes[k].Mesh))
                            edit(false);
                        if (PropertyGrid::Vec3("Mesh scale", r.Meshes[k].Scale, 1.0f, 0.01f))
                            edit(true);
                        ImGui::PopID();
                    }
                if (r.Kind == Vfx::RendererKind::Mesh && PropertyGrid::Row("Meshes"))
                    if (ImGui::SmallButton(ICON_FA_PLUS " Mesh"))
                        r.Meshes.push_back({}), edit(false);
                if (PropertyGrid::BeginGroup("Alignment and facing", nullptr, nullptr, false))
                {
                    if (r.Kind == Vfx::RendererKind::Ribbon)
                    {
                        const char* facing[] = {"Screen", "Custom", "Custom side"};
                        int         f        = int(r.RibbonFacingMode);
                        if (EnumCombo("Facing", facing, f))
                            r.RibbonFacingMode = Vfx::RibbonFacing(f), edit(false);
                        int tess = int(r.TessellationFactor);
                        if (PropertyGrid::Int("Tessellation", tess, 1, 16))
                            r.TessellationFactor = uint32_t(std::clamp(tess, 1, 16)), edit(true);
                        if (PropertyGrid::Float("Curve tension", r.CurveTension, 0.01f, 0.0f, 1.0f))
                            edit(true);
                    }
                    else
                    {
                        const char* facing[] = {"View plane", "Camera position", "Velocity", "Custom", "Default", "Camera plane"};
                        int         f        = int(r.FacingMode);
                        if (EnumCombo("Facing", facing, f))
                            r.FacingMode = Vfx::Facing(f), edit(false);
                        if (PropertyGrid::Checkbox("Lock axis", r.LockedAxisEnable))
                            edit(false);
                        if (r.LockedAxisEnable && PropertyGrid::Vec3("Locked axis", r.LockedAxis, 0.0f, 0.01f))
                            edit(true);
                        if (r.FacingMode == Vfx::Facing::Custom && PropertyGrid::Vec3("Facing vector", r.FacingVector, 0.0f, 0.01f))
                            edit(true);
                        glm::vec3 pivot(r.Pivot, 0);
                        if (r.Kind == Vfx::RendererKind::Sprite && PropertyGrid::Vec3("Pivot", pivot, glm::vec3(0.5f, 0.5f, 0.0f), 0.01f))
                            r.Pivot = glm::vec2(pivot), edit(true);
                    }
                    PropertyGrid::EndGroup();
                }
                if (PropertyGrid::BeginGroup("Sorting", nullptr, nullptr, false))
                {
                    const char* sorts[] = {"None", "Emitter order", "Particle depth", "Particle distance", "Custom"};
                    int         sort    = int(r.Sorting);
                    if (EnumCombo("Sort", sorts, sort))
                        r.Sorting = Vfx::Sort(sort), edit(false);
                    if (PropertyGrid::Checkbox("Ascending", r.SortAscending))
                        edit(false);
                    PropertyGrid::EndGroup();
                }
                if (r.Kind == Vfx::RendererKind::Sprite && PropertyGrid::BeginGroup("Sub images", nullptr, nullptr, false))
                {
                    int rows = int(r.Rows), columns = int(r.Columns);
                    if (PropertyGrid::Int("Rows", rows, 1, 256))
                        r.Rows = uint32_t(std::clamp(rows, 1, 256)), edit(true);
                    if (PropertyGrid::Int("Columns", columns, 1, 256))
                        r.Columns = uint32_t(std::clamp(columns, 1, 256)), edit(true);
                    if (PropertyGrid::Checkbox("Frame blending", r.FrameBlending))
                        edit(false);
                    PropertyGrid::EndGroup();
                }
                if (PropertyGrid::BeginGroup("Lighting and shadows", nullptr, nullptr, false))
                {
                    const char* normals[] = {"Camera facing", "Spherical", "Particle normal"};
                    int         normal    = int(r.NormalMode);
                    if (EnumCombo("Normals", normals, normal))
                        r.NormalMode = Vfx::LightingNormal(normal), edit(false);
                    const char* evaluation[] = {"Per pixel", "Per vertex"};
                    int         lighting     = int(r.Lighting);
                    if (EnumCombo("Lighting", evaluation, lighting))
                        r.Lighting = Vfx::LightingEvaluation(lighting), edit(false);
                    if (r.NormalMode == Vfx::LightingNormal::Spherical && PropertyGrid::Float("Curvature", r.Curvature, 0.01f, 0.0f, 1.0f))
                        edit(true);
                    if (PropertyGrid::Checkbox("Receive shadows", r.ReceiveShadows))
                        edit(false);
                    bool cast = r.CastsShadows();
                    if (PropertyGrid::Checkbox("Cast shadows", cast))
                        r.CastShadows = cast, edit(false);
                    if (PropertyGrid::Float("Depth fade", r.DepthFadeDistance, 0.01f, 0.0f, 100.0f))
                        edit(true);
                    PropertyGrid::EndGroup();
                }
                if (PropertyGrid::BeginGroup("Bindings", nullptr, nullptr, false))
                {
                    auto        parameters = ListParameters(m_Document.Data(), m_Context, handle);
                    std::vector<std::string> keys;
                    for (const auto& [k, v] : r.Bindings)
                        keys.push_back(k);
                    if (!r.Bindings.count("DirectionalTransmittance"))
                        keys.push_back("DirectionalTransmittance");
                    for (const auto& key : keys)
                    {
                        auto at = r.Bindings.find(key);
                        if (!PropertyGrid::Row(key.c_str(), "The particle attribute this renderer input reads"))
                            continue;
                        ImGui::PushID(key.c_str());
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::BeginCombo("##binding", at == r.Bindings.end() ? "None" : at->second.c_str()))
                        {
                            if (key == "DirectionalTransmittance" && ImGui::Selectable("None", at == r.Bindings.end()))
                                r.Bindings.erase(key), edit(false);
                            for (const auto& p : parameters)
                                if (p.Namespace == "Particles" && ImGui::Selectable(p.Name.c_str(), at != r.Bindings.end() && at->second == p.Name))
                                    r.Bindings[key] = p.Name, edit(false);
                            ImGui::EndCombo();
                        }
                        ImGui::PopID();
                    }
                    PropertyGrid::EndGroup();
                }
                PropertyGrid::BeginFree();
                if (ImGui::SmallButton(ICON_FA_ARROW_UP "##up"))
                    Apply([&](Vfx::System& x, const Context& c) { return MoveRenderer(x, c, handle, renderer.Guid, -1); });
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "##down"))
                    Apply([&](Vfx::System& x, const Context& c) { return MoveRenderer(x, c, handle, renderer.Guid, 1); });
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH "  Remove renderer"))
                    Apply([&](Vfx::System& x, const Context& c) { return RemoveRenderer(x, c, handle, renderer.Guid); });
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        if (ImGui::Button(ICON_FA_PLUS "  Add renderer", {-FLT_MIN, 0}))
            ImGui::OpenPopup("##addrenderer");
        if (ImGui::BeginPopup("##addrenderer"))
        {
            for (auto kind : {Vfx::RendererKind::Sprite, Vfx::RendererKind::Mesh, Vfx::RendererKind::Ribbon})
                if (ImGui::MenuItem(RendererName(kind)))
                    Apply([&](Vfx::System& x, const Context& c) { return AddRenderer(x, c, handle, kind); });
            ImGui::EndPopup();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    // ---------------------------------------------------------------------- parameters

    void VfxSystemEditorPanel::DrawParameters()
    {
        using EditorTheme::Px;
        const auto& palette    = EditorTheme::Colors();
        const auto  handle     = Handle();
        auto&       s          = m_Document.Data();
        const auto  parameters = ListParameters(s, m_Context, handle);
        ImGui::TextUnformatted(ICON_FA_SLIDERS "  Parameters");
        ImGui::SameLine();
        ImGui::TextDisabled("drag one onto an input to link it");
        const auto row = [&](const ParameterInfo& p) {
            ImGui::PushID(p.Name.c_str());
            ImGui::Selectable(Short(p.Name).c_str(), false, ImGuiSelectableFlags_AllowOverlap, {Px(150.0f), 0});
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(ParameterPayload, p.Name.data(), p.Name.size());
                ImGui::Text("%s (%s)", p.Name.c_str(), Vfx::TypeName(p.Kind));
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered())
            {
                std::string tip = p.Name + " (" + Vfx::TypeName(p.Kind) + ")";
                if (!p.Writers.empty())
                {
                    tip += "\nWritten by:";
                    for (const auto& w : p.Writers)
                        tip += " " + w;
                }
                if (!p.Readers.empty())
                {
                    tip += "\nRead by:";
                    for (const auto& r : p.Readers)
                        tip += " " + r;
                }
                if (!p.Warning.empty())
                    tip += "\n" + p.Warning;
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Vfx::TypeName(p.Kind));
            if (!p.Warning.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(palette.Warning, ICON_FA_TRIANGLE_EXCLAMATION);
            }
            std::string uses;
            if (!p.Writers.empty())
                uses += "W: " + p.Writers.front() + (p.Writers.size() > 1 ? " +" + std::to_string(p.Writers.size() - 1) : "");
            if (!p.Readers.empty())
                uses += std::string(uses.empty() ? "" : "  ") + "R: " + p.Readers.front() +
                        (p.Readers.size() > 1 ? " +" + std::to_string(p.Readers.size() - 1) : "");
            if (!uses.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", uses.c_str());
            }
            ImGui::PopID();
        };
        // User parameters: exposed to the component and C#.
        if (PropertyGrid::BeginGroup("User"))
        {
            PropertyGrid::BeginFree();
            for (const auto& p : s.UserParameters)
            {
                ImGui::PushID(p.Name.c_str());
                auto at = std::find_if(parameters.begin(), parameters.end(), [&](const auto& x) { return x.Name == p.Name; });
                if (m_RenameParameter == p.Name)
                {
                    ImGui::SetNextItemWidth(Px(150.0f));
                    ImGui::SetKeyboardFocusHere();
                    if (ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                    {
                        const std::string to = std::string("User.") + m_RenameBuffer;
                        Apply([&](Vfx::System& x, const Context&) { return RenameUserParameter(x, p.Name, to); });
                        m_RenameParameter.clear();
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                        m_RenameParameter.clear();
                }
                else if (at != parameters.end())
                {
                    row(*at);
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        m_RenameParameter = p.Name;
                        std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", Short(p.Name).c_str());
                    }
                }
                Vfx::Value value = p.Default.value_or(Zero(p.Kind));
                ImGui::SetNextItemWidth(std::max(Px(60.0f), ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 2 - Px(8.0f)));
                if (ValueEditor("##default", value, p.Kind, &p))
                    Apply([&](Vfx::System& x, const Context&) { return SetUserParameterDefault(x, p.Name, value); }, true);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_PEN "##rename"))
                {
                    m_RenameParameter = p.Name;
                    std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", Short(p.Name).c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH "##remove"))
                    Apply([&](Vfx::System& x, const Context&) { return RemoveUserParameter(x, p.Name); });
                ImGui::PopID();
            }
            if (m_Document.GetMode() == Mode::System)
            {
                if (ImGui::Button(ICON_FA_PLUS "  User parameter"))
                    ImGui::OpenPopup("##adduser");
                if (ImGui::BeginPopup("##adduser"))
                {
                    for (auto t : AllTypes)
                        if (ImGui::MenuItem(Vfx::TypeName(t)))
                            Apply([&](Vfx::System& x, const Context&) { return AddUserParameter(x, t); });
                    ImGui::EndPopup();
                }
            }
            else
                ImGui::TextDisabled("User parameters belong to systems.");
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        for (const char* ns : {"System", "Emitter", "Particles", "Engine"})
        {
            const bool particles = std::strcmp(ns, "Particles") == 0;
            if (!PropertyGrid::BeginGroup(particles ? "Particles (attributes)" : ns, nullptr, nullptr, std::strcmp(ns, "Engine") != 0))
                continue;
            PropertyGrid::BeginFree();
            for (const auto& p : parameters)
                if (p.Namespace == ns)
                    row(p);
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        // Data interfaces of the selected emitter.
        const auto* h = FindHandle(s, handle);
        if (!h || !PropertyGrid::BeginGroup("Data Interfaces"))
            return;
        Vfx::Emitter e;
        try
        {
            e = Effective(*h, m_Context);
        }
        catch (const std::exception&)
        {}
        const bool own = LayerOf(*h) == Layer::Own;
        for (const auto& [name, binding] : e.DataInterfaces)
        {
            ImGui::PushID(name.c_str());
            if (PropertyGrid::BeginGroup((Short(name) + " \xC2\xB7 " + binding.Kind).c_str(), nullptr, nullptr, false))
            {
                auto       b    = binding;
                const auto edit = [&](bool continuous) {
                    Apply([&](Vfx::System& x, const Context& c) { return SetDataInterface(x, c, handle, name, b); }, continuous);
                };
                ImGui::BeginDisabled(!own);
                if (b.Kind == "Grid3D")
                {
                    bool shown = m_GizmoGrid == name;
                    if (PropertyGrid::Checkbox("Bounds handles", shown))
                        m_GizmoGrid = shown ? name : std::string();
                    glm::vec3 resolution(b.Resolution);
                    if (PropertyGrid::Vec3("Resolution", resolution, 32.0f, 0.2f))
                        b.Resolution = glm::uvec3(glm::clamp(glm::round(resolution), glm::vec3(1), glm::vec3(128))), edit(true);
                    if (PropertyGrid::Vec3("World bounds", b.WorldBounds, 4.0f, 0.05f))
                        b.WorldBounds = glm::max(b.WorldBounds, glm::vec3(0.001f)), edit(true);
                    for (size_t k = 0; k < b.GridAttributes.size(); ++k)
                    {
                        ImGui::PushID(int(k));
                        auto& a = b.GridAttributes[k];
                        if (PropertyGrid::Row("Attribute"))
                        {
                            ImGui::SetNextItemWidth(Px(110.0f));
                            if (ImGui::InputText("##name", &a.Name, ImGuiInputTextFlags_EnterReturnsTrue))
                                edit(false);
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(Px(70.0f));
                            int kind = int(a.Kind);
                            const char* kinds[] = {"float", "vec2", "vec3", "vec4"};
                            if (ImGui::Combo("##type", &kind, kinds, 4))
                                a.Kind = Vfx::Type(kind), edit(false);
                            ImGui::SameLine();
                            if (ImGui::SmallButton(ICON_FA_XMARK) && b.GridAttributes.size() > 1)
                                b.GridAttributes.erase(b.GridAttributes.begin() + long(k)), edit(false);
                        }
                        ImGui::PopID();
                    }
                    if (PropertyGrid::Row("Attributes") && ImGui::SmallButton(ICON_FA_PLUS " Attribute"))
                        b.GridAttributes.push_back({"Attribute" + std::to_string(b.GridAttributes.size()), Vfx::Type::Float}), edit(false);
                }
                else if (b.Kind == "VectorField")
                {
                    if (AssetRow("Field asset", AssetType::VectorField, b.Asset))
                        edit(false);
                    if (PropertyGrid::Float("Intensity", b.Intensity, 0.01f))
                        edit(true);
                }
                else if (b.Kind == "StaticMesh" || b.Kind == "SkeletalMesh")
                {
                    if (AssetRow("Mesh asset", AssetType::StaticMesh, b.Asset))
                        edit(false);
                    if (PropertyGrid::InputText("Entity path", b.EntityPath))
                        edit(false);
                }
                else if (b.Kind == "Curve")
                {
                    PropertyGrid::BeginFree();
                    auto curve = CurveFromBinding(b);
                    if (CurveEditor::Draw("##curve", curve, 1, glm::vec4(0), 0.01f, "0", "1"))
                    {
                        auto baked = BindingFromCurve(curve);
                        b.Times    = baked.Times;
                        b.Values   = baked.Values;
                        edit(true);
                    }
                    PropertyGrid::EndFree();
                }
                ImGui::EndDisabled();
                PropertyGrid::BeginFree();
                if (own && ImGui::SmallButton(ICON_FA_TRASH "  Remove"))
                    Apply([&](Vfx::System& x, const Context& c) { return RemoveDataInterface(x, c, handle, name); });
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            ImGui::PopID();
        }
        PropertyGrid::BeginFree();
        ImGui::BeginDisabled(!own);
        if (ImGui::Button(ICON_FA_PLUS "  Data interface"))
            ImGui::OpenPopup("##adddi");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##adddi"))
        {
            for (const char* kind : DataInterfaceKinds)
                if (ImGui::MenuItem(kind))
                    Apply([&](Vfx::System& x, const Context& c) { return AddDataInterface(x, c, handle, kind); });
            ImGui::EndPopup();
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    // ---------------------------------------------------------------------- output

    void VfxSystemEditorPanel::DrawOutput()
    {
        const auto& palette = EditorTheme::Colors();
        ImGui::BeginChild("##output", {0, 0}, ImGuiChildFlags_Borders);
        if (m_Diagnostics.empty())
            ImGui::TextDisabled(IsCompiling() ? "Compiling..." : "No problems.");
        const auto& s = m_Document.Data();
        for (size_t i = 0; i < m_Diagnostics.size(); ++i)
        {
            const auto& d     = m_Diagnostics[i];
            std::string where;
            if (const auto* h = FindHandle(s, d.Emitter))
                where = h->Name;
            else if (d.Emitter.empty())
                where = "System";
            if (d.Stack)
                where += " \xE2\x80\xBA " + std::string(StageTitle(d.Stack->Kind));
            if (!d.Use.empty() && FindHandle(s, d.Emitter))
                try
                {
                    if (const auto* u = FindUse(Effective(*FindHandle(s, d.Emitter), m_Context), d.Use))
                        where += " \xE2\x80\xBA " + m_Library.NameOf(u->Script);
                }
                catch (const std::exception&)
                {}
            if (!d.Input.empty())
                where += " \xE2\x80\xBA " + Short(d.Input) + (d.Nested.empty() ? "" : " \xE2\x80\xBA " + Short(d.Nested));
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_Text, d.Warning ? palette.Warning : palette.Danger);
            const std::string line = std::string(d.Warning ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_XMARK) + "  " + where + ": " + d.Message;
            if (ImGui::Selectable(line.c_str()))
                ActivateDiagnostic(i);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ---------------------------------------------------------------------- preview

    void VfxSystemEditorPanel::FramePreview()
    {
        Bounds b;
        if (auto* instance = m_Preview->Instance())
        {
            if (instance->FixedBounds)
                b.Expand(instance->FixedBounds->Transformed(instance->OwnerTransform()));
            for (const auto& e : instance->Emitters())
            {
                Bounds own = e.Compiled->Settings.FixedBounds.value_or(e.DynamicBounds);
                b.Expand(e.Compiled->Settings.SimulationSpace == Vfx::Space::Local ? own.Transformed(instance->OwnerTransform()) : own);
            }
        }
        if (!b.IsValid())
        {
            b.Expand(glm::vec3(-1, 0, -1));
            b.Expand(glm::vec3(1, 2, 1));
        }
        m_PreviewCamera.Focus(b.Center(), std::max(0.5f, b.Radius()));
        m_PreviewFramed = true;
    }

    void VfxSystemEditorPanel::DrawPreviewToolbar()
    {
        using EditorTheme::Px;
        const auto& palette = EditorTheme::Colors();
        auto&       p       = *m_Preview;
        if (ImGui::Button(p.IsPlaying() ? ICON_FA_PAUSE : ICON_FA_PLAY))
            p.IsPlaying() ? p.Pause() : p.Play();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(p.IsPlaying() ? "Pause" : "Play");
        ImGui::SameLine(0, Px(2.0f));
        if (ImGui::Button(ICON_FA_FORWARD_STEP))
            p.StepFrame();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Step one fixed step");
        ImGui::SameLine(0, Px(2.0f));
        if (ImGui::Button(ICON_FA_ROTATE_LEFT))
            p.Restart();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restart with the same seed");
        ImGui::SameLine(0, Px(2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, p.Loop ? palette.Accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(ICON_FA_ROTATE))
            p.Loop = !p.Loop;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Loop at the end of the time bar");
        ImGui::SameLine();
        float speed = p.Speed();
        ImGui::SetNextItemWidth(Px(70.0f));
        if (ImGui::SliderFloat("##speed", &speed, 0.1f, 4.0f, "%.1fx", ImGuiSliderFlags_Logarithmic))
            p.SetSpeed(speed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Playback speed");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_EXPAND))
            FramePreview();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Frame the bounds");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, m_ShowHandles ? palette.Accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(ICON_FA_ARROWS_MOVE))
            m_ShowHandles = !m_ShowHandles;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Handles for the selected module, the emitter's bounds and its grids");
        ImGui::SameLine();
        if (ImGui::Button("View"))
            ImGui::OpenPopup("##previewview");
        ImGui::SameLine(0, Px(2.0f));
        if (ImGui::Button("Debug"))
            ImGui::OpenPopup("##previewdebug");
        if (p.IsOutdated())
        {
            ImGui::SameLine();
            ImGui::TextColored(palette.Warning, ICON_FA_TRIANGLE_EXCLAMATION " Outdated");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The last compile failed; the preview shows the previous one.\n%s", p.OutdatedReason().c_str());
        }
        if (ImGui::BeginPopup("##previewview"))
        {
            ImGui::MenuItem("Grid", nullptr, &m_Look.Grid);
            ImGui::MenuItem("Handles", nullptr, &m_ShowHandles);
            ImGui::MenuItem("Keep bounds centred", nullptr, &m_LockBounds);
            bool floor = p.HasFloor(), sphere = p.HasSphere();
            if (ImGui::MenuItem("Floor plane (collides)", nullptr, &floor))
                p.SetFloor(floor);
            if (ImGui::MenuItem("Sphere (collides)", nullptr, &sphere))
                p.SetSphere(sphere);
            ImGui::Separator();
            bool sunChanged = ImGui::MenuItem("Directional light", nullptr, &m_Sun);
            ImGui::BeginDisabled(!m_Sun);
            ImGui::SetNextItemWidth(Px(140.0f));
            sunChanged |= ImGui::SliderFloat("Yaw", &m_SunYaw, -180.0f, 180.0f, "%.0f deg");
            ImGui::SetNextItemWidth(Px(140.0f));
            sunChanged |= ImGui::SliderFloat("Pitch", &m_SunPitch, -89.0f, 0.0f, "%.0f deg");
            ImGui::EndDisabled();
            if (sunChanged)
                p.SetSun(m_Sun, SunDirection(m_SunYaw, m_SunPitch));
            ImGui::Separator();
            bool colourChanged = ImGui::MenuItem("Solid background", nullptr, &m_SolidBackground);
            ImGui::BeginDisabled(!m_SolidBackground);
            colourChanged |= ImGui::ColorEdit3("Background", &m_Background.x, ImGuiColorEditFlags_NoInputs);
            ImGui::EndDisabled();
            if (colourChanged)
                p.GetScene()->PatchEnvironmentSettings([&](EnvironmentSettings& env) {
                    env.ShowSkybox      = !m_SolidBackground;
                    env.BackgroundColor = m_Background;
                });
            ImGui::Separator();
            float step = p.FixedStep() * 1000.0f;
            ImGui::SetNextItemWidth(Px(140.0f));
            if (ImGui::DragFloat("Fixed step", &step, 0.1f, 1.0f, 100.0f, "%.2f ms", ImGuiSliderFlags_AlwaysClamp))
                p.SetFixedStep(step / 1000.0f);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##previewdebug"))
        {
            ImGui::MenuItem("Bounds", nullptr, &m_Debug.Bounds);
            ImGui::MenuItem("Emitter origins", nullptr, &m_Debug.Origins);
            ImGui::MenuItem("Velocity vectors", nullptr, &m_Debug.Velocity);
            ImGui::MenuItem("Collision contacts", nullptr, &m_Debug.Contacts);
            ImGui::MenuItem("Vector field arrows", nullptr, &m_Debug.VectorFields);
            ImGui::SetNextItemWidth(Px(140.0f));
            ImGui::DragFloat("Velocity scale", &m_Debug.VelocityScale, 0.005f, 0.001f, 10.0f, "%.3f s");
            ImGui::TextDisabled("GPU particle data comes from the last capture.");
            ImGui::Separator();
            // Grid3D slice: emitter, grid, attribute, axis and index.
            auto        view    = p.GetGridView();
            bool        enabled = view.has_value();
            if (ImGui::MenuItem("Grid3D slice", nullptr, &enabled))
            {
                if (enabled)
                {
                    VfxPreview::GridView v;
                    for (const auto& h : m_Document.Data().Emitters)
                        if (auto grids = p.Grids(h.Guid); !grids.empty())
                        {
                            v.Emitter = h.Guid;
                            v.Grid    = grids.front();
                            break;
                        }
                    if (const auto* product = p.Product())
                        for (const auto& e : product->Emitters)
                            if (e.Handle == v.Emitter)
                                if (auto at = e.Settings.DataInterfaces.find(v.Grid);
                                    at != e.Settings.DataInterfaces.end() && !at->second.GridAttributes.empty())
                                {
                                    v.Attribute = at->second.GridAttributes.front().Name;
                                    v.Index     = int(at->second.Resolution.z / 2);
                                }
                    view = v;
                }
                else
                    view.reset();
                p.SetGridView(view);
            }
            if (view && p.Product())
            {
                const Vfx::CompiledEmitter* emitter = nullptr;
                for (const auto& e : p.Product()->Emitters)
                    if (e.Handle == view->Emitter)
                        emitter = &e;
                if (emitter)
                {
                    auto v = *view;
                    ImGui::SetNextItemWidth(Px(160.0f));
                    if (ImGui::BeginCombo("Grid", v.Grid.c_str()))
                    {
                        for (const auto& e : p.Product()->Emitters)
                            for (const auto& g : p.Grids(e.Handle))
                                if (ImGui::Selectable((e.Settings.Name + " \xE2\x80\xBA " + g).c_str(), v.Emitter == e.Handle && v.Grid == g))
                                    v.Emitter = e.Handle, v.Grid = g;
                        ImGui::EndCombo();
                    }
                    auto at = emitter->Settings.DataInterfaces.find(v.Grid);
                    if (at != emitter->Settings.DataInterfaces.end())
                    {
                        const auto& grid = at->second;
                        ImGui::SetNextItemWidth(Px(160.0f));
                        if (ImGui::BeginCombo("Attribute", v.Attribute.c_str()))
                        {
                            for (const auto& a : grid.GridAttributes)
                                if (ImGui::Selectable(a.Name.c_str(), a.Name == v.Attribute))
                                    v.Attribute = a.Name;
                            ImGui::EndCombo();
                        }
                        uint32_t components = 1;
                        for (const auto& a : grid.GridAttributes)
                            if (a.Name == v.Attribute)
                                components = Vfx::Components(a.Kind);
                        int component = int(v.Component);
                        if (components > 1)
                        {
                            ImGui::SetNextItemWidth(Px(160.0f));
                            ImGui::SliderInt("Component", &component, 0, int(components) - 1, component == 0 ? "x" : component == 1 ? "y" : component == 2 ? "z" : "w");
                        }
                        v.Component = uint32_t(std::clamp(component, 0, int(components) - 1));
                        const char* axes[] = {"X", "Y", "Z"};
                        ImGui::SetNextItemWidth(Px(160.0f));
                        ImGui::Combo("Axis", &v.Axis, axes, 3);
                        ImGui::SetNextItemWidth(Px(160.0f));
                        ImGui::SliderInt("Slice", &v.Index, 0, int(grid.Resolution[v.Axis]) - 1);
                        ImGui::Checkbox("Inset", &m_SliceInset);
                    }
                    if (v != *view)
                        p.SetGridView(v);
                }
            }
            ImGui::EndPopup();
        }
    }

    VfxGizmos::Selection VfxSystemEditorPanel::GizmoSelection() const
    {
        VfxGizmos::Selection selection;
        selection.Emitter = Handle();
        selection.Use     = m_GizmoUse;
        selection.Grid    = m_GizmoGrid;
        return selection;
    }

    Gizmo::Input VfxSystemEditorPanel::PreviewHandleInput(ImVec2 min, ImVec2 size) const
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
        input.Shift      = io.KeyShift;
        input.AxisKey    = ImGui::IsKeyDown(ImGuiKey_X) ? 0 : ImGui::IsKeyDown(ImGuiKey_Y) ? 1 : ImGui::IsKeyDown(ImGuiKey_Z) ? 2 : -1;
        // Over the view, and no widget (nor a camera drag of the view itself) has the pointer.
        input.Allowed = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, {min.x + size.x, min.y + size.y}) &&
                        !ImGui::IsAnyItemActive();
        return input;
    }

    std::vector<GizmoDraw> VfxSystemEditorPanel::UpdatePreviewHandles(const Gizmo::Input& input, glm::vec2 min, glm::vec2 size)
    {
        std::vector<GizmoDraw> draws;
        Gizmo::Settings        settings;
        if (m_SnapSettings)
            settings = m_SnapSettings();
        else
        {
            settings.Size       = EditorTheme::Px(85.0f);
            settings.PickRadius = EditorTheme::Px(10.0f);
        }
        const VfxGizmos::Pointer pointer {Gizmo::View::From(m_PreviewCamera, min, size), input, settings};
        m_PreviewView = pointer.View;
        m_Handles.Begin(pointer.View, pointer.Input, pointer.Settings);
        const bool shown = m_IsOpen && m_ShowHandles;
        if (shown)
        {
            VfxGizmos::Options options;
            if (const auto* instance = m_Preview->Instance())
                options.Owner = instance->OwnerTransform();
            options.Editable         = !ReadOnly();
            options.LockBoundsCenter = m_LockBounds;
            m_Gizmos.Declare(m_Handles, pointer, m_Document, m_Context, GizmoSelection(), options);
        }
        m_Handles.End();
        if (shown)
        {
            m_Gizmos.Update(m_Handles, pointer);
            m_Gizmos.AppendDraws(draws);
        }
        else
            m_Gizmos.Reset();
        m_Handles.AppendDraws(draws);
        return draws;
    }

    void VfxSystemEditorPanel::DrawPreviewOverlay(ImVec2 min, ImVec2 size)
    {
        using EditorTheme::Px;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, {min.x + size.x, min.y + size.y}, true);
        const glm::mat4 viewProjection = m_PreviewCamera.GetViewProjection();
        auto            project = [&](glm::vec3 p, ImVec2& out) {
            const glm::vec4 clip = viewProjection * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-4f)
                return false;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            out = {min.x + (ndc.x * 0.5f + 0.5f) * size.x, min.y + (0.5f - ndc.y * 0.5f) * size.y};
            return true;
        };
        for (const auto& line : m_Preview->DebugLines(m_Debug))
        {
            ImVec2 a, b;
            if (project(line.From, a) && project(line.To, b))
                draw->AddLine(a, b, line.Color, Px(1.2f));
        }
        if (m_Sun)
        {
            // The light direction as an arrow from the top-right corner.
            const glm::vec3 d = SunDirection(m_SunYaw, m_SunPitch);
            const glm::vec3 r = m_PreviewCamera.Right(), u = m_PreviewCamera.Up();
            const ImVec2    c {min.x + size.x - Px(28.0f), min.y + Px(28.0f)};
            const ImVec2    tip {c.x + glm::dot(d, r) * Px(18.0f), c.y - glm::dot(d, u) * Px(18.0f)};
            draw->AddCircle(c, Px(20.0f), IM_COL32(255, 220, 120, 90));
            draw->AddLine(c, tip, IM_COL32(255, 220, 120, 255), Px(2.0f));
            draw->AddCircleFilled(tip, Px(3.0f), IM_COL32(255, 220, 120, 255));
        }
        // The Grid3D slice: its outline in the view and a colour-mapped inset.
        if (const auto& slice = m_Preview->Slice(); slice && !slice->Values.empty())
        {
            const int axis = slice->View.Axis, u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
            const glm::vec3 half = slice->WorldBounds * 0.5f;
            glm::vec3 corner[4];
            for (int k = 0; k < 4; ++k)
            {
                corner[k][axis] = -half[axis] + (float(slice->View.Index) + 0.5f) / float(slice->Resolution[axis]) * slice->WorldBounds[axis];
                corner[k][u]    = (k == 1 || k == 2) ? half[u] : -half[u];
                corner[k][v]    = (k >= 2) ? half[v] : -half[v];
            }
            const glm::mat4 owner = m_Preview->Instance() ? m_Preview->Instance()->OwnerTransform() : glm::mat4(1);
            for (int k = 0; k < 4; ++k)
            {
                ImVec2 a, b;
                if (project(glm::vec3(owner * glm::vec4(corner[k], 1)), a) && project(glm::vec3(owner * glm::vec4(corner[(k + 1) % 4], 1)), b))
                    draw->AddLine(a, b, IM_COL32(255, 160, 60, 220), Px(1.5f));
            }
            if (m_SliceInset)
            {
                const float  side  = std::min(size.y * 0.42f, Px(170.0f));
                const float  cell  = side / float(std::max(slice->Size.x, slice->Size.y));
                const ImVec2 at {min.x + Px(8.0f), min.y + size.y - Px(8.0f) - cell * float(slice->Size.y)};
                const float  range = slice->Max - slice->Min;
                for (uint32_t y = 0; y < slice->Size.y; ++y)
                    for (uint32_t x = 0; x < slice->Size.x; ++x)
                    {
                        const float t = range > 0 ? (slice->At(x, y) - slice->Min) / range : 0.5f;
                        // A blue-to-yellow ramp; v grows upwards.
                        const ImU32 c = IM_COL32(int(30 + 225 * t), int(40 + 180 * t), int(140 * (1 - t) + 40), 235);
                        const ImVec2 p0 {at.x + cell * float(x), at.y + cell * float(slice->Size.y - 1 - y)};
                        draw->AddRectFilled(p0, {p0.x + cell, p0.y + cell}, c);
                    }
                char label[128];
                std::snprintf(label, sizeof(label), "%s %c=%d  [%.3g, %.3g]", slice->View.Attribute.c_str(), "XYZ"[axis], slice->View.Index,
                              slice->Min, slice->Max);
                draw->AddText({at.x, at.y - ImGui::GetTextLineHeight() - Px(2.0f)}, IM_COL32(230, 230, 230, 255), label);
            }
        }
        // Grid3D lattices of the handles, the dragged value and why an edit was refused.
        for (const auto& line : m_Gizmos.Lattice())
        {
            ImVec2 a, b;
            if (project(line.From, a) && project(line.To, b))
                draw->AddLine(a, b, line.Color, Px(1.0f));
        }
        if (const auto& text = m_Gizmos.Readout(); !text.empty())
        {
            const glm::vec2 at  = m_Gizmos.ReadoutPosition();
            const ImVec2    box = ImGui::CalcTextSize(text.c_str());
            const ImVec2    pad {Px(6.0f), Px(3.0f)};
            draw->AddRectFilled({at.x, at.y}, {at.x + box.x + pad.x * 2, at.y + box.y + pad.y * 2}, IM_COL32(18, 18, 22, 215), Px(4.0f));
            draw->AddText({at.x + pad.x, at.y + pad.y}, IM_COL32(240, 240, 240, 255), text.c_str());
        }
        if (const auto& message = m_Gizmos.Message(); !message.empty() && !ReadOnly())
            draw->AddText({min.x + Px(8.0f), min.y + Px(6.0f)}, ImGui::GetColorU32(EditorTheme::Colors().Warning), message.c_str());
        draw->PopClipRect();
    }

    void VfxSystemEditorPanel::DrawPreviewTimeline()
    {
        using EditorTheme::Px;
        auto&       p       = *m_Preview;
        const auto& palette = EditorTheme::Colors();
        char        text[48];
        std::snprintf(text, sizeof(text), "%.2f s", p.Time());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine();
        const float durationWidth = Px(64.0f);
        const float width = std::max(Px(40.0f), ImGui::GetContentRegionAvail().x - durationWidth - ImGui::GetStyle().ItemSpacing.x);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float  h   = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##scrub", {width, h});
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled({min.x, min.y + h * 0.35f}, {min.x + width, min.y + h * 0.65f}, ImGui::GetColorU32(ImGuiCol_FrameBg), Px(2.0f));
        // Seconds as ticks.
        for (int s = 0; s <= int(p.Duration()); ++s)
        {
            const float x = min.x + width * float(s / p.Duration());
            draw->AddLine({x, min.y + h * 0.2f}, {x, min.y + h * 0.35f}, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        const float t = float(std::clamp(p.Time() / p.Duration(), 0.0, 1.0));
        draw->AddRectFilled({min.x, min.y + h * 0.35f}, {min.x + width * t, min.y + h * 0.65f}, ImGui::GetColorU32(palette.Accent), Px(2.0f));
        draw->AddCircleFilled({min.x + width * t, min.y + h * 0.5f}, h * 0.3f, ImGui::GetColorU32(palette.AccentHover));
        if (ImGui::IsItemActive())
        {
            const float x = std::clamp((ImGui::GetIO().MousePos.x - min.x) / width, 0.0f, 1.0f);
            const double target = double(x) * p.Duration();
            if (std::abs(target - p.Time()) >= p.FixedStep() * 0.5)
                p.Seek(target);
            if (ImGui::IsItemActivated())
                p.Pause();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag to seek: resets and replays in fixed steps");
        ImGui::SameLine();
        float duration = float(p.Duration());
        ImGui::SetNextItemWidth(durationWidth);
        if (ImGui::DragFloat("##duration", &duration, 0.05f, 0.1f, 600.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp))
            p.SetDuration(duration);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Time bar length");
    }

    void VfxSystemEditorPanel::DrawPreviewStats()
    {
        const auto stats = m_Preview->Stats();
        uint32_t   count = 0, capacity = 0;
        uint64_t   spawned = 0, memory = 0;
        double     cpu = 0;
        for (const auto& s : stats)
        {
            count += s.Count;
            capacity += s.Capacity;
            spawned += s.SpawnedThisFrame;
            memory += s.MemoryBytes;
            cpu += s.CpuMilliseconds;
        }
        for (const auto& s : stats)
            if (!s.Failure.empty())
                ImGui::TextDisabled("%s: Failed: %s", s.Name.c_str(), s.Failure.c_str());
        char gpu[32];
        if (m_Preview->GpuMilliseconds)
            std::snprintf(gpu, sizeof(gpu), "%.3f ms", *m_Preview->GpuMilliseconds);
        else
            std::snprintf(gpu, sizeof(gpu), "n/a");
        ImGui::TextDisabled("%u / %u particles  \xC2\xB7  +%llu  \xC2\xB7  CPU %.3f ms  \xC2\xB7  GPU %s  \xC2\xB7  %.1f KiB", count, capacity,
                            (unsigned long long)spawned, cpu, gpu, double(memory) / 1024.0);
        if (ImGui::IsItemHovered())
        {
            std::string tip;
            for (const auto& s : stats)
            {
                char line[200];
                std::snprintf(line, sizeof(line), "%s (%s): %u / %u, +%llu this frame, CPU %.3f ms, %.1f KiB\n", s.Name.c_str(),
                              s.Gpu ? "GPU" : "CPU", s.Count, s.Capacity, (unsigned long long)s.SpawnedThisFrame, s.CpuMilliseconds,
                              double(s.MemoryBytes) / 1024.0);
                tip += line;
            }
            ImGui::SetTooltip("%s", tip.empty() ? "No preview" : tip.c_str());
        }
    }

    void VfxSystemEditorPanel::DrawPreview(float height)
    {
        using EditorTheme::Px;
        ImGui::BeginChild("##preview", {0, height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPreviewToolbar();
        const ImGuiIO& io     = ImGui::GetIO();
        const float    below  = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
        const ImVec2   avail  = ImGui::GetContentRegionAvail();
        const ImVec2   size   = {std::max(avail.x, 8.0f), std::max(avail.y - below, 8.0f)};
        const ImVec2   min    = ImGui::GetCursorScreenPos();
        m_PreviewCamera.SetViewportSize(uint32_t(size.x), uint32_t(size.y));
        if (!m_PreviewFramed)
            FramePreview();
        m_Preview->Camera = m_PreviewCamera.GetPosition();
        m_Preview->Advance(io.DeltaTime);
        m_Preview->UpdateSlice(ImGui::GetTime());
        // Handles before the image: their draws go into this frame's render, and a press on one
        // takes the left button before the camera orbits.
        const bool handlesWereDragging = m_Gizmos.IsDragging();
        m_Look.Gizmo = UpdatePreviewHandles(PreviewHandleInput(min, size), {min.x, min.y}, {size.x, size.y});
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_Gizmos.IsDragging())
            m_SwallowRight = false;
        if (handlesWereDragging && !m_Gizmos.IsDragging() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            m_SwallowRight = true; // The right click cancelled a drag: it does not orbit too.
        PreviewFrame frame;
        if (m_Render)
            frame = m_Render(m_Preview->GetScene(), m_PreviewCamera, {uint32_t(size.x), uint32_t(size.y)}, m_Look);
        m_Preview->GpuMilliseconds = frame.VfxGpuMilliseconds;
        if (frame.Image)
        {
            ImGui::Image(static_cast<ImTextureID>(frame.Image), size, ImVec2(0, 1), ImVec2(1, 0));
            ImGui::SetCursorScreenPos(min);
        }
        else
        {
            const auto& c = m_SolidBackground ? m_Background : glm::vec3(0.12f, 0.125f, 0.14f);
            ImGui::GetWindowDrawList()->AddRectFilled(min, {min.x + size.x, min.y + size.y},
                                                      ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 1.0f)));
        }
        ImGui::InvisibleButton("##previewview", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
            m_PreviewCamera.Dolly(io.MouseWheel);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
            m_SwallowRight = false;
        const bool handleOwnsLeft = m_Gizmos.IsDragging() || handlesWereDragging;
        if (ImGui::IsItemActive() && ((ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !handleOwnsLeft) ||
                                      (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !m_SwallowRight)))
            m_PreviewCamera.Orbit({io.MouseDelta.x, io.MouseDelta.y});
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            m_PreviewCamera.Pan({io.MouseDelta.x, io.MouseDelta.y});
        if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F, false))
            FramePreview();
        DrawPreviewOverlay(min, size);
        DrawPreviewTimeline();
        DrawPreviewStats();
        ImGui::EndChild();
    }

    void VfxSystemEditorPanel::DrawPreviewControls()
    {
        using EditorTheme::Px;
        auto&       p = *m_Preview;
        const auto& s = m_Document.Data();
        if (!s.UserParameters.empty() && PropertyGrid::BeginGroup("Preview values"))
        {
            for (const auto& param : s.UserParameters)
            {
                ImGui::PushID(param.Name.c_str());
                const auto at         = p.UserOverrides().find(param.Name);
                const bool overridden = at != p.UserOverrides().end();
                Vfx::Value value      = overridden ? at->second : param.Default.value_or(Zero(param.Kind));
                if (PropertyGrid::Row(Short(param.Name).c_str(), "A value for the preview only; the asset keeps its default", overridden))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ValueEditor("##value", value, param.Kind, &param))
                        p.SetUserParameter(param.Name, value);
                    if (PropertyGrid::RowReset(overridden))
                        p.ClearUserParameter(param.Name);
                }
                ImGui::PopID();
            }
            PropertyGrid::EndGroup();
        }
        if (!s.Emitters.empty() && PropertyGrid::BeginGroup("Preview emitters"))
        {
            if (PropertyGrid::Row("Emit count", "Particles each Emit button adds"))
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::DragInt("##emitcount", &m_EmitCount, 1.0f, 1, 100000);
            }
            const auto stats = p.Stats();
            for (const auto& h : s.Emitters)
            {
                ImGui::PushID(h.Guid.c_str());
                if (PropertyGrid::Row(h.Name.c_str(), "Preview only: solo, mute and emit leave the asset unchanged"))
                {
                    const bool solo = p.Solo() == h.Guid, muted = p.IsMuted(h.Guid);
                    if (ImGui::SmallButton(solo ? "Solo \xE2\x97\x8F" : "Solo"))
                        SetSolo(solo ? std::string() : h.Guid);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(muted ? "Muted" : "Mute"))
                        SetMuted(h.Guid, !muted);
                    ImGui::SameLine();
                    const bool running = std::any_of(stats.begin(), stats.end(), [&](const auto& x) { return x.Handle == h.Guid; });
                    ImGui::BeginDisabled(!running);
                    if (ImGui::SmallButton("Emit"))
                        p.Emit(h.Guid, uint32_t(m_EmitCount));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Capture"))
                    {
                        p.CaptureHandle = h.Guid;
                        p.CaptureEmitter(h.Guid, p.CaptureLimit);
                        m_ShowSpreadsheet = true;
                    }
                    ImGui::EndDisabled();
                    for (const auto& x : stats)
                        if (x.Handle == h.Guid)
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled("%u", x.Count);
                        }
                }
                ImGui::PopID();
            }
            PropertyGrid::EndGroup();
        }
    }

    void VfxSystemEditorPanel::DrawSpreadsheet()
    {
        using EditorTheme::Px;
        auto& p = *m_Preview;
        ImGui::BeginChild("##sheet", {0, 0}, ImGuiChildFlags_Borders);
        // Emitter, Capture, auto capture, the row limit and the filter.
        const auto* chosen = FindHandle(m_Document.Data(), p.CaptureHandle);
        if (!chosen && !m_Document.Data().Emitters.empty())
        {
            p.CaptureHandle = m_Selected != SystemNode && FindHandle(m_Document.Data(), m_Selected) ? m_Selected : m_Document.Data().Emitters.front().Guid;
            chosen          = FindHandle(m_Document.Data(), p.CaptureHandle);
        }
        ImGui::SetNextItemWidth(Px(140.0f));
        if (ImGui::BeginCombo("##sheetemitter", chosen ? chosen->Name.c_str() : "No emitter"))
        {
            for (const auto& h : m_Document.Data().Emitters)
                if (ImGui::Selectable(h.Name.c_str(), h.Guid == p.CaptureHandle))
                    p.CaptureHandle = h.Guid;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_EYE "  Capture"))
            p.CaptureEmitter(p.CaptureHandle, p.CaptureLimit);
        ImGui::SameLine();
        ImGui::Checkbox("Auto capture on step", &p.AutoCapture);
        ImGui::SameLine();
        int limit = int(p.CaptureLimit);
        ImGui::SetNextItemWidth(Px(60.0f));
        if (ImGui::DragInt("Rows", &limit, 1.0f, 1, 4096))
            p.CaptureLimit = size_t(std::clamp(limit, 1, 4096));
        const auto& capture = p.LastCapture();
        if (!capture)
        {
            ImGui::TextDisabled("Capture reads the first particles of the emitter once.");
            ImGui::EndChild();
            return;
        }
        if (!capture->Error.empty())
            ImGui::TextColored(EditorTheme::Colors().Warning, "%s", capture->Error.c_str());
        // Filter: a column component in a range.
        ImGui::SameLine();
        bool filtering = m_Sheet.FilterColumn >= 0;
        if (ImGui::Checkbox("Filter", &filtering))
            m_Sheet.FilterColumn = filtering ? 0 : -1;
        if (filtering && !capture->Columns.empty())
        {
            m_Sheet.FilterColumn = std::min(m_Sheet.FilterColumn, int(capture->Columns.size()) - 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(150.0f));
            if (ImGui::BeginCombo("##filtercolumn", Short(capture->Columns[size_t(m_Sheet.FilterColumn)]).c_str()))
            {
                for (size_t c = 0; c < capture->Columns.size(); ++c)
                    for (uint32_t k = 0; k < VfxPreview::ColumnComponents(capture->Kinds[c]); ++k)
                    {
                        std::string name = Short(capture->Columns[c]);
                        if (VfxPreview::ColumnComponents(capture->Kinds[c]) > 1)
                            name += std::string(".") + "xyzw"[k];
                        if (ImGui::Selectable(name.c_str(), int(c) == m_Sheet.FilterColumn && int(k) == m_Sheet.FilterComponent))
                            m_Sheet.FilterColumn = int(c), m_Sheet.FilterComponent = int(k);
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(130.0f));
            ImGui::DragFloatRange2("##range", &m_Sheet.FilterMin, &m_Sheet.FilterMax, 0.01f);
        }
        const auto rows = SheetRows();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %u alive at %.2f s", rows.size(), capture->Alive, capture->Time);
        // One column per attribute component.
        std::vector<std::pair<size_t, uint32_t>> columns;
        for (size_t c = 0; c < capture->Columns.size(); ++c)
            for (uint32_t k = 0; k < VfxPreview::ColumnComponents(capture->Kinds[c]); ++k)
                columns.push_back({c, k});
        const int count = int(std::min<size_t>(columns.size() + 1, 63));
        if (count > 1 && ImGui::BeginTable("##attributes", count,
                                           ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, Px(34.0f));
            for (int i = 0; i + 1 < count; ++i)
            {
                const auto [c, k] = columns[size_t(i)];
                std::string name  = Short(capture->Columns[c]);
                if (VfxPreview::ColumnComponents(capture->Kinds[c]) > 1)
                    name += std::string(".") + "xyzw"[k];
                ImGui::TableSetupColumn(name.c_str(), ImGuiTableColumnFlags_WidthFixed, Px(78.0f));
            }
            ImGui::TableHeadersRow();
            // A table appearing shows the sheet's order; later header clicks change it.
            const bool fresh   = ImGui::GetFrameCount() - m_SheetFrame > 1;
            m_SheetFrame       = ImGui::GetFrameCount();
            if (auto* specs = ImGui::TableGetSortSpecs(); specs && fresh)
            {
                for (size_t i = 0; i < columns.size() && int(i) + 1 < count; ++i)
                    if (int(columns[i].first) == m_Sheet.SortColumn && int(columns[i].second) == m_Sheet.SortComponent)
                        ImGui::TableSetColumnSortDirection(int(i) + 1,
                                                           m_Sheet.Ascending ? ImGuiSortDirection_Ascending : ImGuiSortDirection_Descending,
                                                           false);
                specs->SpecsDirty = false;
            }
            else if (specs && specs->SpecsDirty)
            {
                if (specs->SpecsCount > 0 && specs->Specs[0].ColumnIndex > 0)
                {
                    const auto [c, k]     = columns[size_t(specs->Specs[0].ColumnIndex - 1)];
                    m_Sheet.SortColumn    = int(c);
                    m_Sheet.SortComponent = int(k);
                    m_Sheet.Ascending     = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                }
                specs->SpecsDirty = false;
            }
            for (auto r : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu", r);
                for (int i = 0; i + 1 < count; ++i)
                {
                    ImGui::TableNextColumn();
                    const auto [c, k] = columns[size_t(i)];
                    const auto& v     = capture->Rows[r][c];
                    if (v.Kind == Vfx::Type::Int || v.Kind == Vfx::Type::Bool)
                        ImGui::Text("%d", v.Integer);
                    else
                        ImGui::Text("%.4g", VfxPreview::ComponentOf(v, k));
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    // ---------------------------------------------------------------------- popups

    void VfxSystemEditorPanel::DrawSearch()
    {
        using EditorTheme::Px;
        if (m_Search.Focus)
        {
            ImGui::OpenPopup("##vfxmodulesearch");
            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
        }
        ImGui::SetNextWindowSize({Px(380.0f), Px(420.0f)}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("##vfxmodulesearch"))
        {
            m_Search.Open = m_Search.Open && m_Search.Focus;
            return;
        }
        if (m_Search.Focus)
        {
            ImGui::SetKeyboardFocusHere();
            m_Search.Focus = false;
        }
        ImGui::TextDisabled("%s", m_Search.Dynamic ? ("Dynamic input for " + Short(m_Search.Path.back())).c_str() :
                                                     ("Add module to " + std::string(StageTitle(m_Search.Stack.Kind))).c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##query", "Search modules", &m_Search.Query, ImGuiInputTextFlags_EnterReturnsTrue);
        const auto results = SearchResults();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            m_Search.Highlight = std::min(m_Search.Highlight + 1, int(results.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            m_Search.Highlight = std::max(m_Search.Highlight - 1, 0);
        m_Search.Highlight = std::clamp(m_Search.Highlight, 0, std::max(0, int(results.size()) - 1));
        std::optional<ModuleEntry> chosen;
        if (enter && !results.empty())
            chosen = results[size_t(m_Search.Highlight)];
        ImGui::BeginChild("##results", {0, 0});
        std::string category;
        for (size_t i = 0; i < results.size(); ++i)
        {
            const auto& r = results[i];
            if (m_Search.Query.empty() && r.Category != category)
            {
                category = r.Category;
                ImGui::SeparatorText(category.c_str());
            }
            ImGui::PushID(int(i));
            if (ImGui::Selectable(r.Name.c_str(), int(i) == m_Search.Highlight))
                chosen = r;
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
            ImGui::TextWrapped("%s", r.Description.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (results.empty())
            ImGui::TextDisabled("No modules fit here.");
        ImGui::EndChild();
        if (chosen)
        {
            ChooseSearch(*chosen);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_Search.Open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void VfxSystemEditorPanel::DrawCurvePopup()
    {
        using EditorTheme::Px;
        if (m_CurveEdit.Requested)
        {
            ImGui::OpenPopup("##vfxcurve");
            m_CurveEdit.Requested = false;
        }
        ImGui::SetNextWindowSize({Px(420.0f), 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopup("##vfxcurve"))
        {
            m_CurveEdit.Open = false;
            return;
        }
        ImGui::Text("%s  %s", m_CurveEdit.IsGradient ? ICON_FA_PALETTE : ICON_FA_CHART_LINE, Short(m_CurveEdit.Path.back()).c_str());
        ImGui::BeginDisabled(ReadOnly());
        if (m_CurveEdit.IsGradient)
        {
            auto g = m_CurveEdit.Gradient;
            if (GradientEditor::Draw("##gradient", g))
                SetEditedGradient(g, true);
        }
        else
        {
            auto c = m_CurveEdit.Curve;
            if (CurveEditor::Draw("##curve", c, 1, glm::vec4(0), 0.01f, "0", "1"))
                SetEditedCurve(c, true);
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void VfxSystemEditorPanel::DrawMenus()
    {
        auto& s = m_Document.Data();
        if (m_EmitterMenuRequested)
        {
            ImGui::OpenPopup("##emittermenu");
            m_EmitterMenuRequested = false;
        }
        if (ImGui::BeginPopup("##emittermenu"))
        {
            const auto  guid = m_MenuEmitter;
            const auto* h    = FindHandle(s, guid);
            if (h)
            {
                ImGui::TextDisabled("%s", h->Name.c_str());
                if (ImGui::MenuItem(ICON_FA_PEN "  Rename"))
                {
                    m_RenameEmitter = guid;
                    std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", h->Name.c_str());
                }
                if (ImGui::MenuItem(h->Enabled ? "Disable" : "Enable"))
                    Apply([&, enabled = !h->Enabled](Vfx::System& x, const Context&) { return SetEmitterEnabled(x, guid, enabled); });
                if (ImGui::MenuItem("Solo in preview", nullptr, m_Preview->Solo() == guid))
                    SetSolo(m_Preview->Solo() == guid ? std::string() : guid);
                if (ImGui::MenuItem("Mute in preview", nullptr, m_Preview->IsMuted(guid)))
                    SetMuted(guid, !m_Preview->IsMuted(guid));
                if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Draw earlier"))
                    Apply([&](Vfx::System& x, const Context&) { return MoveEmitter(x, guid, -1); });
                if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Draw later"))
                    Apply([&](Vfx::System& x, const Context&) { return MoveEmitter(x, guid, 1); });
                if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate"))
                    if (auto r = Apply([&](Vfx::System& x, const Context&) { return DuplicateEmitter(x, guid); }))
                        Select(r.Value);
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete"))
                {
                    Apply([&](Vfx::System& x, const Context&) { return DeleteEmitter(x, guid); });
                    Select(s.Emitters.empty() ? std::string(SystemNode) : s.Emitters.front().Guid);
                }
            }
            ImGui::EndPopup();
        }
        if (!m_RenameEmitter.empty())
        {
            ImGui::OpenPopup("##renameemitter");
        }
        if (ImGui::BeginPopup("##renameemitter"))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("Name", m_RenameBuffer, sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                const auto guid = m_RenameEmitter;
                const std::string name = m_RenameBuffer;
                Apply([&](Vfx::System& x, const Context&) { return RenameEmitter(x, guid, name); });
                m_RenameEmitter.clear();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_RenameEmitter.clear(), ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        else
            m_RenameEmitter.clear();
        if (m_UseMenuRequested)
        {
            ImGui::OpenPopup("##usemenu");
            m_UseMenuRequested = false;
        }
        if (ImGui::BeginPopup("##usemenu"))
        {
            const auto  use    = m_MenuUse;
            const auto  handle = Handle();
            const auto* h      = FindHandle(s, handle);
            if (h)
                try
                {
                    const auto  e     = Effective(*h, m_Context);
                    const auto  ref   = StackOf(e, use);
                    const auto* u     = FindUse(e, use);
                    const auto* info  = u ? m_Library.Find(u->Script) : nullptr;
                    if (ref && u)
                    {
                        const auto* stack = FindStack(e, *ref);
                        size_t      index = 0;
                        while (index < stack->size() && (*stack)[index].Guid != use)
                            ++index;
                        ImGui::TextDisabled("%s", m_Library.NameOf(u->Script).c_str());
                        if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Move up", nullptr, false, index > 0))
                            Apply([&](Vfx::System& x, const Context& c) { return MoveModule(x, c, handle, use, *ref, index - 1); });
                        if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Move down", nullptr, false, index + 1 < stack->size()))
                            Apply([&](Vfx::System& x, const Context& c) { return MoveModule(x, c, handle, use, *ref, index + 1); });
                        if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate"))
                            Apply([&](Vfx::System& x, const Context& c) { return DuplicateModule(x, c, handle, use); });
                        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete"))
                            Apply([&](Vfx::System& x, const Context& c) { return RemoveModule(x, c, handle, use); });
                        const auto origin = ModuleOrigin(*h, use);
                        if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Reset to parent", nullptr, false,
                                            origin == Origin::Overridden))
                            Apply([&](Vfx::System& x, const Context& c) { return ResetModule(x, c, handle, use); });
                        if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Open module", nullptr, false,
                                            info && !info->File.empty() && bool(m_OpenModule)))
                            m_OpenModule(info->File);
                    }
                }
                catch (const std::exception&)
                {}
            ImGui::EndPopup();
        }
        if (m_AddEmitterRequested)
        {
            ImGui::OpenPopup("##addemitter");
            m_AddEmitterRequested = false;
        }
        if (ImGui::BeginPopup("##addemitter"))
        {
            ImGui::TextDisabled("Add emitter");
            if (ImGui::MenuItem("Empty"))
                if (auto r = Apply([&](Vfx::System& x, const Context&) { return Result::Done(AddEmitter(x, "Emitter")); }))
                    Select(r.Value);
            if (ImGui::BeginMenu("From template"))
            {
                for (const auto& t : Templates())
                    if (ImGui::MenuItem(t.Name.c_str()))
                        try
                        {
                            auto e = Vfx::DecodeEmitter(YAML::LoadFile(t.File.string()));
                            if (auto r = Apply([&](Vfx::System& x, const Context&) { return Result::Done(AddEmitter(x, t.Name, e)); }))
                                Select(r.Value);
                        }
                        catch (const std::exception& ex)
                        {
                            m_Status = ex.what();
                        }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Inherited from emitter asset"))
            {
                bool any = false;
                if (m_ProjectEmitters)
                    for (const auto& [id, file] : m_ProjectEmitters())
                    {
                        any = true;
                        if (ImGui::MenuItem(file.stem().u8string().c_str()))
                            if (auto r = Apply([&, id = id, name = file.stem().u8string()](Vfx::System& x, const Context&) {
                                    return Result::Done(AddInheritedEmitter(x, name, id));
                                }))
                                Select(r.Value);
                    }
                if (!any)
                    ImGui::TextDisabled("No emitter assets in the project");
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }

    void VfxSystemEditorPanel::DrawPrompts() {}

    void VfxSystemEditorPanel::Shortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!m_Focused || io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return;
        if (io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                Save();
            else if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                Redo();
        }
    }

    // ---------------------------------------------------------------------- create dialog

    void VfxSystemCreateDialog::OpenSystem(const fs::path& directory)
    {
        m_Directory = directory;
        m_Requested = true;
        m_Emitter   = false;
        m_Choice    = 0;
        m_Templates = Templates();
        m_Error.clear();
        std::snprintf(m_Name, sizeof(m_Name), "%s", "NewSystem");
    }

    void VfxSystemCreateDialog::OpenEmitter(const fs::path& directory, AssetList parents)
    {
        m_Directory = directory;
        m_Requested = true;
        m_Emitter   = true;
        m_Choice    = 0;
        m_Parents   = parents ? parents() : std::vector<std::pair<AssetID, fs::path>> {};
        m_Error.clear();
        std::snprintf(m_Name, sizeof(m_Name), "%s", "NewEmitter");
    }

    std::optional<fs::path> VfxSystemCreateDialog::Draw()
    {
        const char* title = m_Emitter ? "New VFX Emitter" : "New VFX System";
        if (m_Requested)
        {
            ImGui::OpenPopup(title);
            m_Requested = false;
        }
        std::optional<fs::path> created;
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return created;
        ImGui::InputText("Name", m_Name, sizeof(m_Name));
        ImGui::SeparatorText(m_Emitter ? "Start from" : "Emitters");
        ImGui::RadioButton("Empty", &m_Choice, 0);
        if (m_Emitter)
        {
            for (size_t k = 0; k < m_Parents.size(); ++k)
                ImGui::RadioButton(("Inherit from " + m_Parents[k].second.stem().u8string()).c_str(), &m_Choice, int(k) + 1);
            if (m_Parents.empty())
                ImGui::TextDisabled("No emitter assets to inherit from yet.");
        }
        else
            for (size_t k = 0; k < m_Templates.size(); ++k)
                ImGui::RadioButton(("Template: " + m_Templates[k].Name).c_str(), &m_Choice, int(k) + 1);
        if (!m_Error.empty())
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", m_Error.c_str());
        if (ImGui::Button("Create"))
        {
            std::variant<fs::path, std::string> result;
            if (m_Emitter)
                result = CreateEmitterFile(m_Directory, m_Name, m_Choice > 0 ? m_Parents[size_t(m_Choice - 1)].first : AssetID {});
            else
                result = CreateSystemFile(m_Directory, m_Name,
                                          m_Choice > 0 ? std::vector<fs::path> {m_Templates[size_t(m_Choice - 1)].File} : std::vector<fs::path> {});
            if (auto* error = std::get_if<std::string>(&result))
                m_Error = *error;
            else
            {
                created = std::get<fs::path>(result);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return created;
    }
} // namespace Kans
