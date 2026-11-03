#include "VfxModuleEditorPanel.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <sstream>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"
#include "UI/SourceBuild.h"

namespace Kans
{
    namespace fs = std::filesystem;
    using namespace VfxModuleEditing;
    using NodeCanvas::PinRef;
    using NodeCanvas::Side;

    namespace
    {
        // Shared by every open module editor.
        std::string& Clipboard()
        {
            static std::string text;
            return text;
        }
        constexpr const char* StageLabels[] = {"System Spawn", "System Update", "Emitter Spawn", "Emitter Update",
                                               "Particle Spawn", "Particle Update", "Event Handler", "Simulation Stage"};
        constexpr const char* UsageLabels[] = {"Module", "Function", "Dynamic Input"};
        constexpr Vfx::Type   AllTypes[]    = {Vfx::Type::Float, Vfx::Type::Vec2, Vfx::Type::Vec3, Vfx::Type::Vec4, Vfx::Type::Int,
                                               Vfx::Type::Bool, Vfx::Type::Color, Vfx::Type::Quat, Vfx::Type::Position, Vfx::Type::IVec3};
        bool TypeCombo(const char* id, Vfx::Type& kind, const std::vector<Vfx::Type>& offered)
        {
            bool changed = false;
            if (ImGui::BeginCombo(id, Vfx::TypeName(kind)))
            {
                for (auto t : offered)
                    if (ImGui::Selectable(Vfx::TypeName(t), t == kind))
                        kind = t, changed = true;
                ImGui::EndCombo();
            }
            return changed;
        }
        Vfx::Value Zero(Vfx::Type t)
        {
            return t == Vfx::Type::Int ? Vfx::Value::Int(0) : t == Vfx::Type::Bool ? Vfx::Value::Bool(false) : Vfx::Value::Vector({}, t);
        }
    } // namespace

    // ---------------------------------------------------------------------- document

    bool VfxModuleEditorPanel::Open(const fs::path& file)
    {
        if (auto error = m_Document.Open(file))
        {
            m_Status = *error;
            return false;
        }
        m_IsOpen     = true;
        m_WindowOpen = true;
        m_Framed     = false;
        m_Status     = m_Document.ReadOnly() ? "Built-in module: read-only." : "";
        m_Canvas.ClearSelection();
        m_Diagnostics.clear();
        m_SeenRevision = m_CompiledRevision = size_t(-1);
        m_Canvas.FrameAll(BuildGraph());
        return true;
    }

    NodeCanvas::Graph VfxModuleEditorPanel::BuildGraph() const
    {
        return VfxModuleEditing::BuildGraph(m_Document.Data(), m_Diagnostics);
    }

    void VfxModuleEditorPanel::Edited()
    {
        if (!m_Document.ReadOnly())
            m_Document.Commit();
    }

    bool VfxModuleEditorPanel::Save()
    {
        if (auto error = m_Document.Save())
        {
            m_Status = *error;
            return false;
        }
        m_Status = BuildSavedSource(m_Service, m_Document.File());
        return true;
    }

    bool VfxModuleEditorPanel::Undo()
    {
        m_Canvas.SetSelection({});
        return m_Document.Undo();
    }

    bool VfxModuleEditorPanel::Redo()
    {
        m_Canvas.SetSelection({});
        return m_Document.Redo();
    }

    void VfxModuleEditorPanel::CopySelection()
    {
        if (!m_Canvas.Selection().empty())
            Clipboard() = Copy(m_Document.Data(), m_Canvas.Selection());
    }

    void VfxModuleEditorPanel::CutSelection()
    {
        CopySelection();
        DeleteSelection();
    }

    void VfxModuleEditorPanel::PasteAt(glm::vec2 position)
    {
        if (ReadOnly() || Clipboard().empty())
            return;
        // Places the clip's top-left node at `position`.
        glm::vec2 min(1e30f);
        try
        {
            auto clip = Vfx::DecodeModule(YAML::Load(Clipboard()));
            for (const auto& [id, p] : clip.Editor.Positions)
                min = glm::min(min, p);
        }
        catch (const std::exception&)
        {
            return;
        }
        if (min.x > 1e29f)
            min = position;
        auto created = Paste(m_Document.Data(), Clipboard(), position - min);
        m_Canvas.SetSelection({created.begin(), created.end()});
        Edited();
    }

    void VfxModuleEditorPanel::DuplicateSelection()
    {
        if (ReadOnly() || m_Canvas.Selection().empty())
            return;
        auto created = Paste(m_Document.Data(), Copy(m_Document.Data(), m_Canvas.Selection()), glm::vec2(DuplicateOffset));
        m_Canvas.SetSelection({created.begin(), created.end()});
        Edited();
    }

    void VfxModuleEditorPanel::DeleteSelection()
    {
        if (ReadOnly())
            return;
        auto& comments = m_Document.Data().Editor.Comments;
        comments.erase(std::remove_if(comments.begin(), comments.end(),
                                      [&](const auto& c) { return m_Canvas.SelectedComments().count(c.Guid) > 0; }),
                       comments.end());
        DeleteNodes(m_Document.Data(), m_Canvas.Selection());
        m_Canvas.SetSelection({});
        Edited();
    }

    void VfxModuleEditorPanel::CommentSelection()
    {
        if (ReadOnly())
            return;
        const auto graph = BuildGraph();
        glm::vec2  min, max;
        if (m_Canvas.Selection().empty() || !NodeCanvas::Bounds(graph, m_Canvas.Selection(), min, max))
        {
            min = m_Canvas.GetView().ToCanvas(m_Canvas.GetView().Origin + m_Canvas.GetView().Size * 0.5f) - glm::vec2(160, 100);
            max = min + glm::vec2(320, 200);
        }
        const glm::vec2 pad(24, 24 + NodeCanvas::Metrics::CommentTitle);
        auto id = AddComment(m_Document.Data(), "Comment", min - pad, max - min + pad + glm::vec2(24, 24));
        m_Canvas.SetSelection({}, {id});
        Edited();
    }

    void VfxModuleEditorPanel::ToggleCollapsed(const std::string& node)
    {
        if (ReadOnly())
            return;
        auto& collapsed = m_Document.Data().Editor.Collapsed;
        if (!collapsed.erase(node))
            collapsed.insert(node);
        Edited();
    }

    // ---------------------------------------------------------------------- host

    bool VfxModuleEditorPanel::CanConnect(const PinRef& output, const PinRef& input, std::string* reason)
    {
        return CanLink(m_Document.Data(), output, input, reason);
    }

    void VfxModuleEditorPanel::Connect(const PinRef& output, const PinRef& input)
    {
        if (!ReadOnly() && Link(m_Document.Data(), output, input))
            Edited();
    }

    void VfxModuleEditorPanel::Disconnect(const PinRef& pin)
    {
        if (ReadOnly())
            return;
        Unlink(m_Document.Data(), pin);
        Edited();
    }

    void VfxModuleEditorPanel::DisconnectLink(size_t link)
    {
        auto& links = m_Document.Data().Links;
        if (ReadOnly() || link >= links.size())
            return;
        links.erase(links.begin() + std::ptrdiff_t(link));
        Edited();
    }

    void VfxModuleEditorPanel::MoveLinks(const PinRef& from, const PinRef& to)
    {
        if (ReadOnly() || from.PinSide != to.PinSide)
            return;
        auto&                    m = m_Document.Data();
        std::vector<Vfx::Link>   moved;
        for (const auto& l : m.Links)
            if (from.PinSide == Side::Output ? (l.From == from.Node && l.Output == from.Pin) : (l.To == from.Node && l.Input == from.Pin))
                moved.push_back(l);
        Unlink(m, from);
        for (const auto& l : moved)
            from.PinSide == Side::Output ? Link(m, to, {l.To, l.Input, Side::Input}) : Link(m, {l.From, l.Output, Side::Output}, to);
        Edited();
    }

    void VfxModuleEditorPanel::InsertReroute(size_t link, glm::vec2 position)
    {
        if (ReadOnly())
            return;
        auto id = VfxModuleEditing::InsertReroute(m_Document.Data(), link, position);
        if (!id.empty())
            m_Canvas.SetSelection({id});
        Edited();
    }

    void VfxModuleEditorPanel::Translate(const std::vector<std::string>& nodes, const std::vector<std::string>& comments,
                                         glm::vec2 delta)
    {
        if (!ReadOnly())
            VfxModuleEditing::Translate(m_Document.Data(), nodes, comments, delta); // Committed by EndEdit.
    }

    void VfxModuleEditorPanel::ResizeComment(const std::string& id, glm::vec2 position, glm::vec2 size)
    {
        if (ReadOnly())
            return;
        for (auto& c : m_Document.Data().Editor.Comments)
            if (c.Guid == id)
                c.Position = position, c.Size = size;
    }

    void VfxModuleEditorPanel::EndEdit() { Edited(); }

    void VfxModuleEditorPanel::RequestSearch(const std::optional<PinRef>& from, glm::vec2 position)
    {
        OpenSearch(from, position);
    }

    void VfxModuleEditorPanel::ContextMenu(const NodeCanvas::Hit& hit, glm::vec2 position)
    {
        m_ContextHit       = hit;
        m_ContextPosition  = position;
        m_ContextRequested = true;
        if (hit.Type == NodeCanvas::Hit::Kind::Node && !m_Canvas.IsSelected(hit.Id))
            m_Canvas.SetSelection({hit.Id});
    }

    // ---------------------------------------------------------------------- search

    void VfxModuleEditorPanel::OpenSearch(const std::optional<PinRef>& from, glm::vec2 position)
    {
        if (ReadOnly())
            return;
        m_Search.Open      = true;
        m_Search.Focus     = true;
        m_Search.From      = from;
        m_Search.Position  = position;
        m_Search.Query     = {};
        m_Search.Highlight = 0;
    }

    std::vector<SearchEntry> VfxModuleEditorPanel::SearchResults() const
    {
        std::optional<SearchFilter> filter;
        if (m_Search.From)
            if (auto type = PinType(m_Document.Data(), *m_Search.From))
                filter = SearchFilter {m_Search.From->PinSide, *type};
        return Search(m_Document.Data(), m_Search.Query, filter);
    }

    std::string VfxModuleEditorPanel::ChooseSearch(const SearchEntry& entry)
    {
        if (ReadOnly())
            return {};
        auto id = Create(m_Document.Data(), entry, m_Search.Position, m_Search.From);
        m_Search.Open = false;
        m_Canvas.SetSelection({id});
        Edited();
        return id;
    }

    // ---------------------------------------------------------------------- compile

    void VfxModuleEditorPanel::Tick(double now)
    {
        if (m_Compile.valid() && m_Compile.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            m_Diagnostics = m_Compile.get();
            for (auto& w : Warnings(m_CompileModule))
                m_Diagnostics.push_back(std::move(w));
            m_CompiledRevision = m_PendingRevision;
        }
        if (m_Document.Revision() != m_SeenRevision)
        {
            m_SeenRevision = m_Document.Revision();
            m_EditTime     = now;
        }
        // Debounced: compile once edits pause, never while one is still running.
        if (!m_Compile.valid() && m_CompiledRevision != m_SeenRevision && now - m_EditTime >= CompileDelay)
        {
            m_PendingRevision = m_SeenRevision;
            m_CompileModule   = m_Document.Data();
            m_Compile         = std::async(std::launch::async, [module = m_CompileModule, resolve = m_Resolve] {
                return TestCompile(module, resolve);
            });
        }
    }

    void VfxModuleEditorPanel::WaitForCompile()
    {
        if (m_Compile.valid())
            m_Compile.wait();
        Tick(m_EditTime); // Collects the result without starting another compile.
    }

    void VfxModuleEditorPanel::ActivateDiagnostic(size_t index)
    {
        if (index >= m_Diagnostics.size())
            return;
        const auto& d = m_Diagnostics[index];
        if (!d.Node.empty() && FindNode(m_Document.Data(), d.Node))
            m_Canvas.FocusNode(BuildGraph(), d.Node, d.Pin);
    }

    // ---------------------------------------------------------------------- inputs

    bool VfxModuleEditorPanel::RequestRemoveInput(const std::string& name)
    {
        if (ReadOnly())
            return false;
        if (!InputUsers(m_Document.Data(), name).empty())
        {
            m_PendingRemoval   = name;
            m_RemovalRequested = true;
            return false;
        }
        RemoveInput(m_Document.Data(), name);
        Edited();
        return true;
    }

    void VfxModuleEditorPanel::ConfirmRemoveInput(bool remove)
    {
        if (remove && !m_PendingRemoval.empty())
        {
            RemoveInput(m_Document.Data(), m_PendingRemoval);
            m_Canvas.SetSelection({});
            Edited();
        }
        m_PendingRemoval.clear();
    }

    bool VfxModuleEditorPanel::RenameInput(const std::string& from, const std::string& to)
    {
        if (ReadOnly())
            return false;
        if (auto error = VfxModuleEditing::RenameInput(m_Document.Data(), from, to))
        {
            m_Status = *error;
            return false;
        }
        Edited();
        return true;
    }

    std::optional<fs::path> VfxModuleEditorPanel::DuplicateToProject(const fs::path& directory)
    {
        auto copy = Duplicate(m_Document.Data(), m_Document.File(), directory);
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

    // ---------------------------------------------------------------------- close

    void VfxModuleEditorPanel::RequestClose(std::function<void()> closed)
    {
        m_Closing.Request(m_IsOpen && m_Document.IsDirty(), "closing " + m_Document.File().filename().u8string(),
                          [this, closed] {
                              m_IsOpen = false;
                              if (closed)
                                  closed();
                          });
        m_ClosePromptRequested = m_Closing.Asking();
    }

    void VfxModuleEditorPanel::ResolveClose(UnsavedChanges::Choice choice)
    {
        m_Closing.Resolve(choice, [this] { return Save() ? UnsavedChanges::SaveResult::Saved : UnsavedChanges::SaveResult::Failed; });
    }

    // ---------------------------------------------------------------------- drawing

    std::string VfxModuleEditorPanel::WindowName() const
    {
        return std::string(ICON_FA_DIAGRAM_PROJECT "  ") + m_Document.File().filename().u8string() + (m_Document.IsDirty() ? " *" : "") +
               "###VfxModule" + m_Document.File().u8string();
    }

    void VfxModuleEditorPanel::Draw()
    {
        if (!m_IsOpen)
            return;
        ImGui::SetNextWindowSize({1280.0f, 720.0f}, ImGuiCond_FirstUseEver);
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
            ImGui::OpenPopup("Unsaved VFX module");
            m_ClosePromptRequested = false;
        }
        if (ImGui::BeginPopupModal("Unsaved VFX module", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

    void VfxModuleEditorPanel::DrawContents()
    {
        m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        Tick(ImGui::GetTime());
        DrawToolbar();
        if (!m_IsOpen)
        {
            ImGui::TextDisabled("%s", m_Status.empty() ? "Open a .vfxmodule from the Content Browser." : m_Status.c_str());
            return;
        }
        using EditorTheme::Px;
        const ImVec2 avail   = ImGui::GetContentRegionAvail();
        const float  spacing = ImGui::GetStyle().ItemSpacing.x;
        const float  left = Px(250.0f), right = Px(300.0f), output = Px(130.0f);
        const float  middle = std::max(avail.x - left - right - 2 * spacing, Px(200.0f));

        ImGui::BeginChild("##left", {left, avail.y}, ImGuiChildFlags_Borders);
        DrawInputs();
        DrawParameters();
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginGroup();
        const float canvasHeight = std::max(avail.y - output - ImGui::GetStyle().ItemSpacing.y, Px(100.0f));
        const auto  graph        = BuildGraph();
        if (!m_Framed)
        {
            // Fit the graph, but stay readable: a large graph opens at its top-left corner.
            auto& view = m_Canvas.GetView();
            view.Size  = {middle, canvasHeight};
            m_Canvas.FrameAll(graph);
            glm::vec2 min, max;
            if (view.Zoom < 0.6f && NodeCanvas::Bounds(graph, {}, min, max))
            {
                view.Zoom = 0.6f;
                view.Pan  = glm::vec2(40) - min * view.Zoom;
            }
            m_Framed = true;
        }
        NodeCanvas::DrawOptions options;
        options.PinValue = [this](const NodeCanvas::Node& node, const NodeCanvas::Pin& pin, float width) {
            auto* n = FindNode(m_Document.Data(), node.Id);
            if (!n)
                return;
            for (size_t k = 0; k < n->Inputs.size(); ++k)
                if (n->Inputs[k].Name == pin.Name)
                {
                    auto&                           p     = n->Inputs[k];
                    Vfx::Value                      value = p.Default.value_or(Zero(p.Kind));
                    const auto*                     info  = Vfx::FindOperation(n->Op);
                    const std::vector<std::string>* names =
                        info && k < info->Inputs.size() && !info->Inputs[k].Enum.empty() ? &info->Inputs[k].Enum : nullptr;
                    if (ValueEditor("##v", value, p.Kind, width, names))
                    {
                        p.Default   = value;
                        m_ValueEdit = true;
                    }
                }
        };
        NodeCanvas::Draw("##graph", graph, m_Canvas, *this, {middle, canvasHeight}, options);
        // Parameters dropped from the browser: Get (or Input), Alt for Set.
        if (!ReadOnly())
        {
            const ImVec2 min = {m_Canvas.GetView().Origin.x, m_Canvas.GetView().Origin.y};
            if (ImGui::BeginDragDropTargetCustom({min, {min.x + middle, min.y + canvasHeight}}, ImGui::GetID("##graphdrop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KANS_VFX_PARAMETER"))
                {
                    const std::string name(static_cast<const char*>(payload->Data), size_t(payload->DataSize));
                    for (const auto& p : Parameters(m_Document.Data()))
                        if (p.Name == name)
                        {
                            const ImVec2 mouse = ImGui::GetMousePos();
                            auto id = CreateParameterNode(m_Document.Data(), p, m_Canvas.GetView().ToCanvas({mouse.x, mouse.y}),
                                                          ImGui::GetIO().KeyAlt);
                            m_Canvas.SetSelection({id});
                            Edited();
                            break;
                        }
                }
                ImGui::EndDragDropTarget();
            }
        }
        DrawOutput();
        ImGui::EndGroup();
        ImGui::SameLine();

        ImGui::BeginChild("##details", {0, avail.y}, ImGuiChildFlags_Borders);
        DrawDetails();
        ImGui::EndChild();

        if (m_ValueEdit && !ImGui::IsAnyItemActive())
        {
            m_ValueEdit = false;
            Edited();
        }
        DrawSearch();
        DrawContextMenu();
        DrawPrompts();
        Shortcuts();
    }

    void VfxModuleEditorPanel::DrawToolbar()
    {
        if (!m_IsOpen)
            return;
        auto&       m        = m_Document.Data();
        const auto& palette  = EditorTheme::Colors();
        ImGui::TextUnformatted(ICON_FA_DIAGRAM_PROJECT);
        ImGui::SameLine();
        ImGui::TextUnformatted((m_Document.File().filename().u8string() + (m_Document.IsDirty() ? "  *" : "")).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", UsageLabels[int(m.ScriptUsage)]);
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
        }
        ImGui::SameLine();
        bool snap = m_Canvas.GridSnap > 0;
        if (ImGui::Checkbox("Snap", &snap))
            m_Canvas.GridSnap = snap ? 16.0f : 0.0f;
        ImGui::SameLine();
        size_t errors = 0;
        for (const auto& d : m_Diagnostics)
            errors += !d.Warning;
        if (IsCompiling() || m_CompiledRevision != m_Document.Revision())
            ImGui::TextDisabled("Compiling...");
        else if (errors)
            ImGui::TextColored(palette.Danger, ICON_FA_TRIANGLE_EXCLAMATION "  %zu error%s", errors, errors == 1 ? "" : "s");
        else
            ImGui::TextColored(palette.Success, "Compiles");
        if (!m_Status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_Status.c_str());
        }
    }

    bool VfxModuleEditorPanel::ValueEditor(const char* id, Vfx::Value& value, Vfx::Type kind, float width,
                                           const std::vector<std::string>* names)
    {
        using Vfx::Type;
        ImGui::PushID(id);
        bool changed = false;
        if (kind != Type::Bool && kind != Type::Color)
            ImGui::SetNextItemWidth(width);
        if (names && kind == Type::Int)
        {
            int index = std::clamp(value.Integer, 0, int(names->size()) - 1);
            if (ImGui::BeginCombo("##e", (*names)[size_t(index)].c_str()))
            {
                for (size_t k = 0; k < names->size(); ++k)
                    if (ImGui::Selectable((*names)[k].c_str(), int(k) == value.Integer))
                        value.Integer = int(k), changed = true;
                ImGui::EndCombo();
            }
        }
        else
            switch (kind)
            {
                case Type::Float:
                    changed = ImGui::DragFloat("##v", &value.Data.x, 0.01f, 0, 0, "%.3g");
                    if (changed)
                        value.Data = glm::vec4(value.Data.x);
                    break;
                case Type::Int:
                    changed = ImGui::DragInt("##v", &value.Integer);
                    break;
                case Type::Bool:
                {
                    bool b  = value.Integer != 0;
                    changed = ImGui::Checkbox("##v", &b);
                    value.Integer = b;
                    break;
                }
                case Type::Vec2:
                    changed = ImGui::DragFloat2("##v", &value.Data.x, 0.01f, 0, 0, "%.2g");
                    break;
                case Type::Vec3:
                case Type::Position:
                    changed = ImGui::DragFloat3("##v", &value.Data.x, 0.01f, 0, 0, "%.2g");
                    break;
                case Type::Vec4:
                case Type::Quat:
                    changed = ImGui::DragFloat4("##v", &value.Data.x, 0.01f, 0, 0, "%.2g");
                    break;
                case Type::Color:
                    changed = ImGui::ColorEdit4("##v", &value.Data.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
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

    void VfxModuleEditorPanel::DrawInputs()
    {
        auto& m = m_Document.Data();
        if (!PropertyGrid::BeginGroup("Module Inputs"))
            return;
        PropertyGrid::BeginFree();
        ImGui::BeginDisabled(ReadOnly());
        for (size_t i = 0; i < m.Inputs.size(); ++i)
        {
            auto& p = m.Inputs[i];
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(TypeColor(p.Kind)));
            ImGui::Bullet();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const std::string short_ = p.Name.substr(7);
            if (m_RenameFrom == p.Name)
            {
                ImGui::SetNextItemWidth(-1);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    RenameInput(p.Name, "Module." + std::string(m_RenameBuffer));
                    m_RenameFrom.clear();
                }
                else if (ImGui::IsItemDeactivated())
                    m_RenameFrom.clear();
            }
            else if (ImGui::TreeNodeEx(short_.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                           ImGuiTreeNodeFlags_FramePadding))
            {
                ImGui::Indent();
                if (ImGui::SmallButton("Rename"))
                {
                    m_RenameFrom = p.Name;
                    std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", short_.c_str());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ARROW_UP))
                    MoveInput(m, i, -1), Edited();
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
                    MoveInput(m, i, 1), Edited();
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                    RequestRemoveInput(p.Name);
                if (i < m.Inputs.size() && m.Inputs[i].Name == p.Name)
                {
                    Vfx::Type kind = p.Kind;
                    ImGui::SetNextItemWidth(-1);
                    if (TypeCombo("##type", kind, {std::begin(AllTypes), std::end(AllTypes)}))
                        SetInputType(m, p.Name, kind), Edited();
                    Vfx::Value value = p.Default.value_or(Zero(p.Kind));
                    if (ValueEditor("##default", value, p.Kind, ImGui::GetContentRegionAvail().x,
                                    p.EnumNames.empty() ? nullptr : &p.EnumNames))
                        p.Default = value, m_ValueEdit = true;
                    bool ranged = bool(p.Range);
                    if (p.Kind == Vfx::Type::Float && ImGui::Checkbox("Range", &ranged))
                        p.Range = ranged ? std::optional<glm::vec2>(glm::vec2(0, 1)) : std::nullopt, Edited();
                    if (p.Range)
                    {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::DragFloat2("##range", &p.Range->x, 0.01f))
                            p.Range->y = std::max(p.Range->y, p.Range->x), m_ValueEdit = true;
                    }
                    if (p.Kind == Vfx::Type::Int)
                    {
                        std::string names;
                        for (const auto& n : p.EnumNames)
                            names += (names.empty() ? "" : ",") + n;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputTextWithHint("##enum", "Enum names, comma separated", &names,
                                                     ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            p.EnumNames.clear();
                            std::stringstream list(names);
                            for (std::string item; std::getline(list, item, ',');)
                                if (!item.empty())
                                    p.EnumNames.push_back(item);
                            Edited();
                        }
                    }
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (ImGui::Button(ICON_FA_PLUS "  Input"))
            ImGui::OpenPopup("##addinput");
        if (ImGui::BeginPopup("##addinput"))
        {
            for (auto t : AllTypes)
                if (ImGui::MenuItem(Vfx::TypeName(t)))
                    AddInput(m, t), Edited();
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void VfxModuleEditorPanel::DrawParameters()
    {
        if (!PropertyGrid::BeginGroup("Parameters"))
            return;
        PropertyGrid::BeginFree();
        ImGui::TextDisabled("Drag onto the graph: Get, Alt for Set");
        PropertyGrid::EndFree();
        const auto entries = Parameters(m_Document.Data());
        for (size_t i = 0; i < entries.size();)
        {
            const std::string group = entries[i].Group;
            size_t            end   = i;
            while (end < entries.size() && entries[end].Group == group)
                ++end;
            if (PropertyGrid::BeginGroup(group.c_str(), nullptr, nullptr, group == "Module" || group == "Particles"))
            {
                PropertyGrid::BeginFree();
                for (size_t k = i; k < end; ++k)
                {
                    const auto& p = entries[k];
                    ImGui::PushID(int(k));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(TypeColor(p.Kind)));
                    ImGui::Bullet();
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    const auto dot = p.Name.find('.');
                    ImGui::Selectable(dot == std::string::npos ? p.Name.c_str() : p.Name.c_str() + dot + 1);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s  %s%s", p.Name.c_str(), Vfx::TypeName(p.Kind),
                                          p.Interface.empty() ? "" : ("  (" + p.Interface + ")").c_str());
                    if (ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload("KANS_VFX_PARAMETER", p.Name.data(), p.Name.size());
                        ImGui::TextUnformatted(p.Name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
                }
                PropertyGrid::EndFree();
                PropertyGrid::EndGroup();
            }
            i = end;
        }
        PropertyGrid::EndGroup();
    }

    void VfxModuleEditorPanel::DrawDetails()
    {
        auto& m = m_Document.Data();
        if (PropertyGrid::BeginGroup("Module"))
        {
            ImGui::BeginDisabled(ReadOnly());
            int usage = int(m.ScriptUsage);
            if (PropertyGrid::Combo("Usage", UsageLabels, 3, usage))
                m.ScriptUsage = Vfx::Usage(usage), Edited();
            if (PropertyGrid::InputText("Category", m.Category))
                m_ValueEdit = true;
            if (PropertyGrid::InputText("Description", m.Description))
                m_ValueEdit = true;
            for (int s = 0; s < 8; ++s)
            {
                bool allowed = (m.AllowedStages >> s) & 1u;
                if (PropertyGrid::Checkbox(StageLabels[s], allowed))
                    m.AllowedStages = allowed ? m.AllowedStages | (1u << s) : m.AllowedStages & ~(1u << s), Edited();
            }
            ImGui::EndDisabled();
            PropertyGrid::EndGroup();
        }
        const Vfx::Node* selected = m_Canvas.Selection().size() == 1 ? FindNode(m, *m_Canvas.Selection().begin()) : nullptr;
        if (selected && PropertyGrid::BeginGroup("Node"))
        {
            auto&       n    = *FindNode(m, selected->Guid);
            const auto* info = Vfx::FindOperation(n.Op);
            ImGui::BeginDisabled(ReadOnly());
            PropertyGrid::Text("Op", n.Op);
            if (info)
                PropertyGrid::Text("About", info->Description, true);
            if (PropertyGrid::Row("Type"))
            {
                Vfx::Type kind = n.Kind;
                ImGui::SetNextItemWidth(-1);
                if (TypeCombo("##kind", kind, info ? info->Kinds : std::vector<Vfx::Type>(std::begin(AllTypes), std::end(AllTypes))))
                    SetNodeKind(m, n.Guid, kind), Edited();
            }
            if (info && info->Parameter && PropertyGrid::Row("Parameter"))
            {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##parameter", n.Parameter.c_str()))
                {
                    for (const auto& p : Parameters(m))
                    {
                        const bool fits = n.Op == "Input" ? p.ModuleInput : n.Op == "Set" ? p.Writable :
                                          n.Op == "Get"   ? !p.ModuleInput :
                                                            p.ModuleInput;
                        if (fits && ImGui::Selectable(p.Name.c_str(), p.Name == n.Parameter))
                        {
                            n.Parameter = p.Name;
                            if (n.Op == "Input" || n.Op == "Get" || n.Op == "Set")
                                SetNodeKind(m, n.Guid, p.Kind);
                            Edited();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if (info && info->Code && n.Op != "CustomGLSL" &&
                PropertyGrid::InputText(n.Op.rfind("Grid", 0) == 0 ? "Attribute" : "Name", n.Code))
                m_ValueEdit = true;
            if (n.Op == "Constant" && PropertyGrid::Row("Value"))
            {
                if (ValueEditor("##constant", n.Constant, n.Kind, ImGui::GetContentRegionAvail().x, nullptr))
                    m_ValueEdit = true;
            }
            bool collapsed = m.Editor.Collapsed.count(n.Guid) > 0;
            if (PropertyGrid::Checkbox("Collapsed", collapsed))
                ToggleCollapsed(n.Guid);
            if (n.Op == "CustomGLSL")
            {
                PropertyGrid::BeginFree();
                std::string hint = "Inputs:";
                for (size_t k = 0; k < n.Inputs.size(); ++k)
                    hint += " $" + n.Inputs[k].Name;
                ImGui::TextDisabled("%s  (an expression, or statements assigning the outputs)", hint.c_str());
                if (ImGui::InputTextMultiline("##code", &n.Code, {-1, EditorTheme::Px(110.0f)}, ImGuiInputTextFlags_AllowTabInput))
                    m_ValueEdit = true;
                ImGui::TextUnformatted("Arguments");
                for (size_t k = 0; k < n.Inputs.size(); ++k)
                {
                    ImGui::PushID(int(k));
                    ImGui::Text("$%s", n.Inputs[k].Name.c_str());
                    ImGui::SameLine(EditorTheme::Px(70.0f));
                    ImGui::SetNextItemWidth(EditorTheme::Px(110.0f));
                    Vfx::Type kind = n.Inputs[k].Kind;
                    if (TypeCombo("##argtype", kind, {std::begin(AllTypes), std::end(AllTypes)}))
                        n.Inputs[k].Kind = kind, n.Inputs[k].Default = Zero(kind), Edited();
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_TRASH))
                    {
                        const auto name = n.Inputs[k].Name;
                        Unlink(m, {n.Guid, name, Side::Input});
                        n.Inputs.erase(n.Inputs.begin() + std::ptrdiff_t(k));
                        Edited();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button(ICON_FA_PLUS "  Argument"))
                {
                    std::string name;
                    for (size_t k = n.Inputs.size();; ++k)
                    {
                        name = "Arg" + std::to_string(k);
                        if (std::none_of(n.Inputs.begin(), n.Inputs.end(), [&](const auto& p) { return p.Name == name; }))
                            break;
                    }
                    n.Inputs.push_back({name, Vfx::Type::Float, Vfx::Value::Float(0)});
                    Edited();
                }
                PropertyGrid::EndFree();
            }
            ImGui::EndDisabled();
            PropertyGrid::EndGroup();
        }
        else if (!selected && PropertyGrid::BeginGroup("Node"))
        {
            PropertyGrid::Text("Selection", m_Canvas.Selection().empty() ? "None" : std::to_string(m_Canvas.Selection().size()) + " nodes", true);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Used by", nullptr, nullptr, false))
        {
            PropertyGrid::BeginFree();
            auto users = m_Usage && !ReadOnly() ? m_Usage(m_Document.File()) : std::nullopt;
            if (ReadOnly())
                ImGui::TextDisabled("Built-in modules are not tracked by the asset registry.");
            else if (!users)
                ImGui::TextDisabled("Not tracked yet (build the project).");
            else if (users->empty())
                ImGui::TextDisabled("No emitter or system uses it.");
            else
                for (const auto& u : *users)
                    ImGui::BulletText("%s", u.c_str());
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
    }

    void VfxModuleEditorPanel::DrawOutput()
    {
        const auto& palette = EditorTheme::Colors();
        ImGui::BeginChild("##output", {m_Canvas.GetView().Size.x, 0}, ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Compile output");
        if (m_Diagnostics.empty())
            ImGui::TextDisabled(m_CompiledRevision == m_Document.Revision() ? "No problems." : "Compiling...");
        for (size_t i = 0; i < m_Diagnostics.size(); ++i)
        {
            const auto& d = m_Diagnostics[i];
            ImGui::PushID(int(i));
            ImGui::TextColored(d.Warning ? palette.Warning : palette.Danger, "%s",
                               d.Warning ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_EXCLAMATION);
            ImGui::SameLine();
            const Vfx::Node* node  = d.Node.empty() ? nullptr : FindNode(m_Document.Data(), d.Node);
            std::string      label = (node ? NodeTitle(*node) + (d.Pin.empty() ? "" : "." + d.Pin) + ": " : std::string()) + d.Message;
            if (ImGui::Selectable(label.c_str(), false))
                ActivateDiagnostic(i);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void VfxModuleEditorPanel::DrawSearch()
    {
        if (m_Search.Open && !ImGui::IsPopupOpen("##nodesearch"))
        {
            ImGui::OpenPopup("##nodesearch");
            const auto at = m_Canvas.GetView().ToScreen(m_Search.Position);
            ImGui::SetNextWindowPos({at.x, at.y});
        }
        ImGui::SetNextWindowSize({EditorTheme::Px(340.0f), EditorTheme::Px(380.0f)});
        if (!ImGui::BeginPopup("##nodesearch"))
        {
            m_Search.Open = false;
            return;
        }
        if (m_Search.Focus)
            ImGui::SetKeyboardFocusHere(), m_Search.Focus = false;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##query", ICON_FA_MAGNIFYING_GLASS "  Search nodes and parameters", &m_Search.Query))
            m_Search.Highlight = 0;
        const auto results = SearchResults();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            m_Search.Highlight = std::min(m_Search.Highlight + 1, int(results.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            m_Search.Highlight = std::max(m_Search.Highlight - 1, 0);
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
        if (m_Search.From)
            ImGui::TextDisabled("Showing nodes that link to %s", m_Search.From->Pin.c_str());
        ImGui::BeginChild("##results");
        std::string category;
        for (size_t i = 0; i < results.size(); ++i)
        {
            const auto& e = results[i];
            if (m_Search.Query.empty() && e.Category != category)
            {
                category = e.Category;
                ImGui::SeparatorText(category.c_str());
            }
            ImGui::PushID(int(i));
            const bool highlighted = int(i) == m_Search.Highlight;
            if (ImGui::Selectable(e.Label.c_str(), highlighted) || (highlighted && enter))
            {
                ChooseSearch(e);
                ImGui::CloseCurrentPopup();
                ImGui::PopID();
                break;
            }
            if (highlighted && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                ImGui::SetScrollHereY(0.5f);
            if (highlighted && ImGui::IsItemVisible())
                ImGui::SetItemTooltip("%s", e.Description.c_str());
            // The description after the label, clipped to the list.
            ImGui::SameLine(std::max(EditorTheme::Px(150.0f), ImGui::CalcTextSize(e.Label.c_str()).x + EditorTheme::Px(14.0f)));
            ImGui::TextDisabled("%s", e.Description.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            m_Search.Open = false, ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void VfxModuleEditorPanel::DrawContextMenu()
    {
        if (m_ContextRequested)
        {
            ImGui::OpenPopup("##graphcontext");
            m_ContextRequested = false;
        }
        if (!ImGui::BeginPopup("##graphcontext"))
            return;
        using Kind = NodeCanvas::Hit::Kind;
        const auto& hit = m_ContextHit;
        if (hit.Type == Kind::Node || hit.Type == Kind::Pin)
        {
            if (ImGui::MenuItem("Collapse", nullptr, m_Document.Data().Editor.Collapsed.count(hit.Id) > 0, !ReadOnly()))
                ToggleCollapsed(hit.Id);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !ReadOnly()))
                DuplicateSelection();
            if (ImGui::MenuItem("Copy", "Ctrl+C"))
                CopySelection();
            if (ImGui::MenuItem("Comment", "C", false, !ReadOnly()))
                CommentSelection();
            if (hit.Type == Kind::Pin && ImGui::MenuItem("Break links", "Alt+Click", false, !ReadOnly()))
                Disconnect(hit.Target);
            if (ImGui::MenuItem("Delete", "Del", false, !ReadOnly()))
                DeleteSelection();
        }
        else if (hit.Type == Kind::Link)
        {
            if (ImGui::MenuItem("Add reroute", "Double-click", false, !ReadOnly()))
                InsertReroute(hit.LinkIndex, m_ContextPosition);
            if (ImGui::MenuItem("Break link", "Alt+Click", false, !ReadOnly()))
                DisconnectLink(hit.LinkIndex);
        }
        else if (hit.Type == Kind::CommentTitle || hit.Type == Kind::CommentResize)
        {
            for (auto& c : m_Document.Data().Editor.Comments)
                if (c.Guid == hit.Id)
                {
                    ImGui::BeginDisabled(ReadOnly());
                    if (ImGui::InputText("Title", &c.Title))
                        m_ValueEdit = true;
                    if (ImGui::ColorEdit4("Colour", &c.Color.x, ImGuiColorEditFlags_NoInputs))
                        m_ValueEdit = true;
                    ImGui::EndDisabled();
                }
            if (ImGui::MenuItem("Delete", nullptr, false, !ReadOnly()))
                DeleteSelection();
        }
        ImGui::EndPopup();
    }

    void VfxModuleEditorPanel::DrawPrompts()
    {
        if (m_RemovalRequested)
        {
            ImGui::OpenPopup("Remove module input");
            m_RemovalRequested = false;
        }
        if (ImGui::BeginPopupModal("Remove module input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s is used by %zu node(s). Remove it and its Input nodes?", m_PendingRemoval.c_str(),
                        InputUsers(m_Document.Data(), m_PendingRemoval).size());
            if (ImGui::Button("Remove"))
                ConfirmRemoveInput(true), ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ConfirmRemoveInput(false), ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void VfxModuleEditorPanel::Shortcuts()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!m_Focused || io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return;
        if (io.KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false))
                Save();
            else if (ImGui::IsKeyPressed(ImGuiKey_Z, true))
                io.KeyShift ? Redo() : Undo();
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, true))
                Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_C, false))
                CopySelection();
            else if (ImGui::IsKeyPressed(ImGuiKey_X, false))
                CutSelection();
            else if (ImGui::IsKeyPressed(ImGuiKey_V, false))
                PasteAt(m_Canvas.GetView().ToCanvas(m_Canvas.Mouse()));
            else if (ImGui::IsKeyPressed(ImGuiKey_D, false))
                DuplicateSelection();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
            DeleteSelection();
        else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
            OpenSearch(std::nullopt, m_Canvas.GetView().ToCanvas(m_Canvas.Mouse()));
        else if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            CommentSelection();
    }

    // ---------------------------------------------------------------------- create dialog

    void VfxModuleCreateDialog::Open(const fs::path& directory)
    {
        m_Directory = directory;
        m_Requested = true;
        m_Error.clear();
    }

    std::optional<fs::path> VfxModuleCreateDialog::Draw()
    {
        if (m_Requested)
        {
            ImGui::OpenPopup("New VFX Module");
            m_Requested = false;
        }
        std::optional<fs::path> created;
        if (!ImGui::BeginPopupModal("New VFX Module", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return created;
        ImGui::InputText("Name", m_Name, sizeof(m_Name));
        ImGui::Combo("Usage", &m_Usage, UsageLabels, 3);
        ImGui::TextUnformatted("Allowed stages");
        for (int s = 0; s < 8; ++s)
        {
            bool allowed = (m_Stages >> s) & 1u;
            if (ImGui::Checkbox(StageLabels[s], &allowed))
                m_Stages = allowed ? m_Stages | (1u << s) : m_Stages & ~(1u << s);
        }
        if (!m_Error.empty())
            ImGui::TextColored(EditorTheme::Colors().Danger, "%s", m_Error.c_str());
        if (ImGui::Button("Create"))
        {
            auto result = CreateModuleFile(m_Directory, m_Name, Vfx::Usage(m_Usage), m_Stages);
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
