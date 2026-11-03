#include "NodeCanvas.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include "Kans3D/ImGui/EditorTheme.h"

namespace Kans::NodeCanvas
{
    // ---------------------------------------------------------------------- view

    void View::ZoomAt(glm::vec2 screen, float zoom)
    {
        const glm::vec2 anchor = ToCanvas(screen);
        Zoom                   = std::clamp(zoom, MinZoom, MaxZoom);
        Pan                    = screen - Origin - anchor * Zoom;
    }

    void View::Frame(glm::vec2 min, glm::vec2 max, float margin)
    {
        const glm::vec2 extent = glm::max(max - min, glm::vec2(1));
        const glm::vec2 room   = glm::max(Size - 2.0f * margin, glm::vec2(1));
        Zoom                   = std::clamp(std::min({room.x / extent.x, room.y / extent.y, 1.0f}), MinZoom, MaxZoom);
        Pan                    = Size * 0.5f - (min + max) * 0.5f * Zoom;
    }

    // ---------------------------------------------------------------------- geometry

    const Node* Graph::Find(const std::string& id) const
    {
        for (const auto& n : Nodes)
            if (n.Id == id)
                return &n;
        return nullptr;
    }

    namespace
    {
        float TextWidth(const std::string& text) { return float(text.size()) * Metrics::CharWidth; }
        const std::string& LabelOf(const Pin& p) { return p.Label.empty() ? p.Name : p.Label; }
        float TitleBlock(const Node& n) { return Metrics::TitleHeight + (n.Subtitle.empty() ? 0.0f : 16.0f); }
        bool  Inside(glm::vec2 p, glm::vec2 min, glm::vec2 max)
        {
            return p.x >= min.x && p.y >= min.y && p.x <= max.x && p.y <= max.y;
        }
        float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
        {
            const glm::vec2 ab = b - a;
            const float     t  = glm::dot(ab, ab) > 0 ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0;
            return glm::length(p - (a + ab * t));
        }
        glm::vec2 Bezier(const glm::vec2 c[4], float t)
        {
            const float u = 1 - t;
            return u * u * u * c[0] + 3 * u * u * t * c[1] + 3 * u * t * t * c[2] + t * t * t * c[3];
        }
    } // namespace

    NodeLayout Layout(const Node& node)
    {
        NodeLayout l;
        for (size_t i = 0; i < node.Inputs.size(); ++i)
            if (!node.Collapsed || node.Inputs[i].Connected)
                l.Inputs.push_back(i);
        for (size_t i = 0; i < node.Outputs.size(); ++i)
            if (!node.Collapsed || node.Outputs[i].Connected)
                l.Outputs.push_back(i);
        if (node.Reroute)
        {
            l.Min = node.Position - glm::vec2(Metrics::RerouteRadius);
            l.Max = node.Position + glm::vec2(Metrics::RerouteRadius);
            l.InputPositions.assign(l.Inputs.size(), node.Position);
            l.OutputPositions.assign(l.Outputs.size(), node.Position);
            return l;
        }
        float inputs = 0, outputs = 0;
        for (auto i : l.Inputs)
        {
            const auto& p = node.Inputs[i];
            inputs        = std::max(inputs, Metrics::PinRadius * 2 + 6 + TextWidth(LabelOf(p)) +
                                          (p.ValueWidth > 0 && !p.Connected ? p.ValueWidth + 6 : 0.0f));
        }
        for (auto i : l.Outputs)
            outputs = std::max(outputs, Metrics::PinRadius * 2 + 6 + TextWidth(LabelOf(node.Outputs[i])));
        float width = std::max({Metrics::MinWidth,
                                TextWidth(node.Title) + 2 * Metrics::Padding + Metrics::BadgeWidth,
                                TextWidth(node.Subtitle) * 0.9f + 2 * Metrics::Padding,
                                inputs + outputs + 2 * Metrics::Padding + 12});
        width            = std::min(width, Metrics::MaxWidth);
        const size_t rows = std::max(l.Inputs.size(), l.Outputs.size());
        const float  top  = TitleBlock(node);
        l.Min             = node.Position;
        l.Max             = node.Position + glm::vec2(width, top + float(rows) * Metrics::RowHeight + 6);
        for (size_t k = 0; k < l.Inputs.size(); ++k)
            l.InputPositions.push_back({l.Min.x, l.Min.y + top + (float(k) + 0.5f) * Metrics::RowHeight});
        for (size_t k = 0; k < l.Outputs.size(); ++k)
            l.OutputPositions.push_back({l.Max.x, l.Min.y + top + (float(k) + 0.5f) * Metrics::RowHeight});
        return l;
    }

    std::optional<glm::vec2> PinPosition(const Graph& graph, const PinRef& pin)
    {
        const Node* node = graph.Find(pin.Node);
        if (!node)
            return std::nullopt;
        const auto  l     = Layout(*node);
        const auto& pins  = pin.PinSide == Side::Input ? node->Inputs : node->Outputs;
        const auto& shown = pin.PinSide == Side::Input ? l.Inputs : l.Outputs;
        const auto& at    = pin.PinSide == Side::Input ? l.InputPositions : l.OutputPositions;
        for (size_t k = 0; k < shown.size(); ++k)
            if (pins[shown[k]].Name == pin.Pin)
                return at[k];
        return std::nullopt;
    }

    void LinkCurve(glm::vec2 a, glm::vec2 b, glm::vec2 out[4])
    {
        const float d = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f);
        out[0]        = a;
        out[1]        = a + glm::vec2(d, 0);
        out[2]        = b - glm::vec2(d, 0);
        out[3]        = b;
    }

    std::vector<std::string> NodesInside(const Graph& graph, const Comment& comment)
    {
        std::vector<std::string> r;
        const glm::vec2          min = comment.Position, max = comment.Position + comment.Size;
        for (const auto& n : graph.Nodes)
        {
            const auto l = Layout(n);
            if (Inside(l.Min, min, max) && Inside(l.Max, min, max))
                r.push_back(n.Id);
        }
        return r;
    }

    bool Bounds(const Graph& graph, const std::set<std::string>& nodes, glm::vec2& min, glm::vec2& max)
    {
        bool any = false;
        min = glm::vec2(1e30f), max = glm::vec2(-1e30f);
        for (const auto& n : graph.Nodes)
            if (nodes.empty() || nodes.count(n.Id))
            {
                const auto l = Layout(n);
                min = glm::min(min, l.Min), max = glm::max(max, l.Max), any = true;
            }
        if (nodes.empty())
            for (const auto& c : graph.Comments)
                min = glm::min(min, c.Position), max = glm::max(max, c.Position + c.Size), any = true;
        return any;
    }

    Hit HitTest(const Graph& graph, const View& view, glm::vec2 screen)
    {
        Hit             hit;
        const glm::vec2 p = view.ToCanvas(screen);
        for (auto n = graph.Nodes.rbegin(); n != graph.Nodes.rend(); ++n)
        {
            const auto l    = Layout(*n);
            float      best = Metrics::PinHitRadius;
            const auto test = [&](const std::vector<size_t>& shown, const std::vector<glm::vec2>& at,
                                  const std::vector<Pin>& pins, Side side) {
                for (size_t k = 0; k < shown.size(); ++k)
                {
                    const float d = glm::length(view.ToScreen(at[k]) - screen);
                    if (d <= best)
                    {
                        best           = d;
                        hit.Type       = Hit::Kind::Pin;
                        hit.Id         = n->Id;
                        hit.Target     = {n->Id, pins[shown[k]].Name, side};
                    }
                }
            };
            // A reroute's pins share its centre; the output wins and link drops pick the other side.
            test(l.Inputs, l.InputPositions, n->Inputs, Side::Input);
            test(l.Outputs, l.OutputPositions, n->Outputs, Side::Output);
            if (hit.Type == Hit::Kind::Pin)
                return hit;
            if (Inside(p, l.Min, l.Max))
            {
                hit.Type = Hit::Kind::Node;
                hit.Id   = n->Id;
                return hit;
            }
        }
        for (size_t i = 0; i < graph.Links.size(); ++i)
        {
            const auto& link = graph.Links[i];
            auto        a    = PinPosition(graph, {link.From, link.Output, Side::Output});
            auto        b    = PinPosition(graph, {link.To, link.Input, Side::Input});
            if (!a || !b)
                continue;
            glm::vec2 c[4];
            LinkCurve(*a, *b, c);
            glm::vec2 previous = view.ToScreen(c[0]);
            for (int s = 1; s <= 24; ++s)
            {
                const glm::vec2 next = view.ToScreen(Bezier(c, float(s) / 24));
                if (SegmentDistance(screen, previous, next) <= Metrics::LinkHitDistance)
                {
                    hit.Type      = Hit::Kind::Link;
                    hit.LinkIndex = i;
                    return hit;
                }
                previous = next;
            }
        }
        for (auto c = graph.Comments.rbegin(); c != graph.Comments.rend(); ++c)
        {
            const glm::vec2 max = c->Position + c->Size;
            if (!Inside(p, c->Position, max))
                continue;
            const float grip = Metrics::ResizeGrip / view.Zoom;
            hit.Id           = c->Id;
            hit.Type         = (p.x >= max.x - grip && p.y >= max.y - grip)          ? Hit::Kind::CommentResize :
                               p.y <= c->Position.y + Metrics::CommentTitle / view.Zoom ? Hit::Kind::CommentTitle :
                                                                                      Hit::Kind::CommentBody;
            return hit;
        }
        return hit;
    }

    // ---------------------------------------------------------------------- controller

    std::string Controller::Key(const PinRef& p)
    {
        return p.Node + '\n' + p.Pin + (p.PinSide == Side::Input ? "\ni" : "\no");
    }

    void Controller::SetSelection(std::set<std::string> nodes, std::set<std::string> comments)
    {
        m_Selected         = std::move(nodes);
        m_SelectedComments = std::move(comments);
    }

    void Controller::ClearSelection()
    {
        m_Selected.clear();
        m_SelectedComments.clear();
        m_FlashNode.clear();
        m_FlashPin.clear();
    }

    PinState Controller::StateOf(const PinRef& pin) const
    {
        auto at = m_PinStates.find(Key(pin));
        return at == m_PinStates.end() ? PinState::Neutral : at->second;
    }

    std::optional<std::pair<glm::vec2, glm::vec2>> Controller::SelectionBox() const
    {
        if (m_Drag != Drag::Box)
            return std::nullopt;
        return std::make_pair(glm::min(m_PressMouse, m_Mouse), glm::max(m_PressMouse, m_Mouse));
    }

    void Controller::FrameSelection(const Graph& graph)
    {
        glm::vec2 min, max;
        if (!m_Selected.empty() && Bounds(graph, m_Selected, min, max))
            m_View.Frame(min, max);
        else
            FrameAll(graph);
    }

    void Controller::FrameAll(const Graph& graph)
    {
        glm::vec2 min, max;
        if (Bounds(graph, {}, min, max))
            m_View.Frame(min, max);
    }

    void Controller::FocusNode(const Graph& graph, const std::string& node, const std::string& pin)
    {
        SetSelection({node});
        glm::vec2 min, max;
        if (Bounds(graph, {node}, min, max))
        {
            const float zoom = m_View.Zoom;
            m_View.Frame(min, max, 120);
            m_View.Zoom = std::clamp(std::max(zoom, 0.8f), MinZoom, MaxZoom); // Readable, centred.
            m_View.Pan  = m_View.Size * 0.5f - (min + max) * 0.5f * m_View.Zoom;
        }
        m_FlashNode = node;
        m_FlashPin  = pin;
    }

    void Controller::Update(const Graph& graph, const Input& in, Host& host)
    {
        const glm::vec2 previous = m_Mouse;
        m_Mouse                  = in.Mouse;
        const glm::vec2 delta    = m_Mouse - previous;
        const bool      readOnly = host.ReadOnly();
        m_Hovered                = in.Hovered ? HitTest(graph, m_View, m_Mouse) : Hit {};

        if (in.Hovered && in.Wheel != 0 && m_Drag != Drag::Pan)
            m_View.ZoomAt(m_Mouse, m_View.Zoom * std::pow(1.15f, in.Wheel));
        if (in.FrameSelection)
            FrameSelection(graph);
        if (in.FrameAll)
            FrameAll(graph);

        if (m_Drag == Drag::None && in.Hovered && (in.Pressed[2] || (in.Pressed[0] && in.Space)))
        {
            m_Drag        = Drag::Pan;
            m_PanByMiddle = in.Pressed[2];
            return;
        }
        if (m_Drag == Drag::Pan)
        {
            m_View.Pan += delta;
            if (m_PanByMiddle ? !in.Down[2] : !in.Down[0])
                m_Drag = Drag::None;
            return;
        }

        if (m_Drag == Drag::None && in.Hovered && in.Pressed[1])
        {
            const glm::vec2 at = m_View.ToCanvas(m_Mouse);
            if (m_Hovered.Type == Hit::Kind::None || m_Hovered.Type == Hit::Kind::CommentBody)
            {
                if (!readOnly)
                    host.RequestSearch(std::nullopt, at);
            }
            else
                host.ContextMenu(m_Hovered, at);
        }

        if (m_Drag == Drag::None && in.Hovered && in.Pressed[0])
        {
            m_PressMouse     = m_Mouse;
            m_DragTotal      = m_Applied = glm::vec2(0);
            const auto& hit  = m_Hovered;
            switch (hit.Type)
            {
                case Hit::Kind::Pin:
                {
                    if (readOnly)
                        break;
                    if (in.Alt)
                    {
                        host.Disconnect(hit.Target);
                        host.EndEdit();
                        break;
                    }
                    m_LinkFrom = hit.Target;
                    const auto connected = std::any_of(graph.Links.begin(), graph.Links.end(), [&](const Link& l) {
                        return hit.Target.PinSide == Side::Output ? (l.From == hit.Target.Node && l.Output == hit.Target.Pin) :
                                                                    (l.To == hit.Target.Node && l.Input == hit.Target.Pin);
                    });
                    m_MovingLinks = in.Ctrl && connected;
                    m_Drag        = Drag::Link;
                    // Type checks for every pin, shown while dragging.
                    m_PinStates.clear();
                    for (const auto& n : graph.Nodes)
                    {
                        const auto evaluate = [&](const std::vector<Pin>& pins, Side side) {
                            for (const auto& p : pins)
                            {
                                PinRef target {n.Id, p.Name, side};
                                if (target == m_LinkFrom)
                                    continue;
                                bool ok;
                                if (m_MovingLinks)
                                {
                                    if (side != m_LinkFrom.PinSide)
                                        continue;
                                    ok = true;
                                    for (const auto& l : graph.Links)
                                    {
                                        if (side == Side::Output && l.From == m_LinkFrom.Node && l.Output == m_LinkFrom.Pin)
                                            ok = ok && host.CanConnect(target, {l.To, l.Input, Side::Input}, nullptr);
                                        if (side == Side::Input && l.To == m_LinkFrom.Node && l.Input == m_LinkFrom.Pin)
                                            ok = ok && host.CanConnect({l.From, l.Output, Side::Output}, target, nullptr);
                                    }
                                }
                                else
                                {
                                    if (side == m_LinkFrom.PinSide)
                                        continue;
                                    ok = side == Side::Input ? host.CanConnect(m_LinkFrom, target, nullptr) :
                                                               host.CanConnect(target, m_LinkFrom, nullptr);
                                }
                                m_PinStates[Key(target)] = ok ? PinState::Compatible : PinState::Incompatible;
                            }
                        };
                        evaluate(n.Inputs, Side::Input);
                        evaluate(n.Outputs, Side::Output);
                    }
                    break;
                }
                case Hit::Kind::Link:
                    if (!readOnly && in.Alt)
                    {
                        host.DisconnectLink(hit.LinkIndex);
                        host.EndEdit();
                    }
                    else if (!readOnly && in.DoubleClicked)
                    {
                        host.InsertReroute(hit.LinkIndex, m_View.ToCanvas(m_Mouse));
                        host.EndEdit();
                    }
                    break;
                case Hit::Kind::Node:
                {
                    m_FlashNode.clear(), m_FlashPin.clear();
                    if (in.Ctrl)
                    {
                        if (!m_Selected.erase(hit.Id))
                            m_Selected.insert(hit.Id);
                    }
                    else if (in.Shift)
                        m_Selected.insert(hit.Id);
                    else if (!m_Selected.count(hit.Id))
                        SetSelection({hit.Id});
                    if (m_Selected.count(hit.Id) && !readOnly)
                    {
                        m_DragNodes.assign(m_Selected.begin(), m_Selected.end());
                        m_DragComments.clear();
                        m_Anchor = graph.Find(hit.Id) ? graph.Find(hit.Id)->Position : glm::vec2(0);
                        m_Drag   = Drag::Nodes;
                    }
                    break;
                }
                case Hit::Kind::CommentTitle:
                {
                    if (in.Ctrl)
                    {
                        if (!m_SelectedComments.erase(hit.Id))
                            m_SelectedComments.insert(hit.Id);
                    }
                    else if (in.Shift)
                        m_SelectedComments.insert(hit.Id);
                    else if (!m_SelectedComments.count(hit.Id))
                        SetSelection({}, {hit.Id});
                    if (m_SelectedComments.count(hit.Id) && !readOnly)
                    {
                        // A comment carries the nodes inside it when the drag starts.
                        std::set<std::string> nodes(m_Selected.begin(), m_Selected.end());
                        for (const auto& c : graph.Comments)
                            if (m_SelectedComments.count(c.Id))
                            {
                                for (auto& id : NodesInside(graph, c))
                                    nodes.insert(id);
                                if (c.Id == hit.Id)
                                    m_Anchor = c.Position;
                            }
                        m_DragNodes.assign(nodes.begin(), nodes.end());
                        m_DragComments.assign(m_SelectedComments.begin(), m_SelectedComments.end());
                        m_Drag = Drag::Nodes;
                    }
                    break;
                }
                case Hit::Kind::CommentResize:
                    if (!readOnly)
                        for (const auto& c : graph.Comments)
                            if (c.Id == hit.Id)
                            {
                                m_Resizing    = c.Id;
                                m_ResizeStart = c.Position;
                                m_ResizeSize  = c.Size;
                                m_Drag        = Drag::Resize;
                            }
                    break;
                case Hit::Kind::None:
                case Hit::Kind::CommentBody:
                    if (!in.Shift && !in.Ctrl)
                        ClearSelection();
                    m_Drag = Drag::Box;
                    break;
            }
            return;
        }

        if (m_Drag == Drag::Nodes)
        {
            m_DragTotal += delta / m_View.Zoom;
            glm::vec2 target = m_DragTotal;
            if (GridSnap > 0)
                target = glm::round((m_Anchor + m_DragTotal) / GridSnap) * GridSnap - m_Anchor;
            const glm::vec2 step = target - m_Applied;
            if (step != glm::vec2(0))
            {
                host.Translate(m_DragNodes, m_DragComments, step);
                m_Applied = target;
            }
        }
        else if (m_Drag == Drag::Resize)
            host.ResizeComment(m_Resizing, m_ResizeStart,
                               glm::max(m_ResizeSize + (m_Mouse - m_PressMouse) / m_View.Zoom, glm::vec2(80, 60)));
        else if (m_Drag == Drag::Link)
        {
            m_Refusal.clear();
            if (m_Hovered.Type == Hit::Kind::Pin && StateOf(m_Hovered.Target) == PinState::Incompatible)
            {
                const auto& t = m_Hovered.Target;
                if (!m_MovingLinks)
                    t.PinSide == Side::Input ? host.CanConnect(m_LinkFrom, t, &m_Refusal) :
                                               host.CanConnect(t, m_LinkFrom, &m_Refusal);
                if (m_Refusal.empty())
                    m_Refusal = "Incompatible pin";
            }
        }
        if (m_Drag != Drag::None && !in.Down[0])
            Release(graph, in, host);
    }

    void Controller::Release(const Graph& graph, const Input& in, Host& host)
    {
        const Drag drag = m_Drag;
        m_Drag          = Drag::None;
        if (drag == Drag::Nodes)
        {
            if (m_Applied != glm::vec2(0))
                host.EndEdit();
        }
        else if (drag == Drag::Resize)
            host.EndEdit();
        else if (drag == Drag::Link)
        {
            Hit hit = HitTest(graph, m_View, m_Mouse);
            m_Refusal.clear();
            const Node* node = hit.Type == Hit::Kind::Pin || hit.Type == Hit::Kind::Node ? graph.Find(hit.Id) : nullptr;
            const Side  want = m_MovingLinks ? m_LinkFrom.PinSide :
                               m_LinkFrom.PinSide == Side::Input ? Side::Output : Side::Input;
            // A reroute's pins share one spot: take the side the drag needs.
            if (hit.Type == Hit::Kind::Pin && node && node->Reroute && hit.Target.PinSide != want)
            {
                const auto& pins = want == Side::Input ? node->Inputs : node->Outputs;
                if (!pins.empty())
                    hit.Target = {node->Id, pins.front().Name, want};
            }
            // Dropped on a node body: its first compatible pin.
            if (hit.Type == Hit::Kind::Node && node)
                for (const auto& p : want == Side::Input ? node->Inputs : node->Outputs)
                    if (StateOf({node->Id, p.Name, want}) == PinState::Compatible)
                    {
                        hit.Type   = Hit::Kind::Pin;
                        hit.Target = {node->Id, p.Name, want};
                        break;
                    }
            if (hit.Type == Hit::Kind::Pin)
            {
                const auto& t = hit.Target;
                if (t.PinSide == want && t != m_LinkFrom && StateOf(t) == PinState::Compatible)
                {
                    if (m_MovingLinks)
                        host.MoveLinks(m_LinkFrom, t);
                    else if (want == Side::Input)
                        host.Connect(m_LinkFrom, t);
                    else
                        host.Connect(t, m_LinkFrom);
                    host.EndEdit();
                }
            }
            else if (!m_MovingLinks && (hit.Type == Hit::Kind::None || hit.Type == Hit::Kind::CommentBody) &&
                     glm::length(m_Mouse - m_PressMouse) > 4)
                host.RequestSearch(m_LinkFrom, m_View.ToCanvas(m_Mouse));
            m_PinStates.clear();
            m_MovingLinks = false;
        }
        else if (drag == Drag::Box)
        {
            const glm::vec2 a = m_View.ToCanvas(glm::min(m_PressMouse, m_Mouse));
            const glm::vec2 b = m_View.ToCanvas(glm::max(m_PressMouse, m_Mouse));
            if (glm::length(m_Mouse - m_PressMouse) < 3)
                return; // A click on empty space only clears the selection.
            for (const auto& n : graph.Nodes)
            {
                const auto l = Layout(n);
                if (l.Max.x < a.x || l.Min.x > b.x || l.Max.y < a.y || l.Min.y > b.y)
                    continue;
                if (in.Ctrl)
                    m_Selected.erase(n.Id);
                else
                    m_Selected.insert(n.Id);
            }
            for (const auto& c : graph.Comments)
                if (Inside(c.Position, a, b) && Inside(c.Position + c.Size, a, b))
                {
                    if (in.Ctrl)
                        m_SelectedComments.erase(c.Id);
                    else
                        m_SelectedComments.insert(c.Id);
                }
        }
    }

    // ---------------------------------------------------------------------- ImGui layer

    namespace
    {
        ImVec2 V(glm::vec2 v) { return {v.x, v.y}; }
        ImU32  Fade(ImU32 color, float alpha)
        {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
            c.w *= alpha;
            return ImGui::ColorConvertFloat4ToU32(c);
        }
        ImU32 Color(const ImVec4& c, float alpha = 1.0f) { return ImGui::ColorConvertFloat4ToU32({c.x, c.y, c.z, c.w * alpha}); }
    } // namespace

    void Draw(const char* id, const Graph& graph, Controller& controller, Host& host, glm::vec2 size,
              const DrawOptions& options)
    {
        ImGui::PushID(id);
        View&           view   = controller.GetView();
        const ImVec2    cursor = ImGui::GetCursorScreenPos();
        view.Origin            = {cursor.x, cursor.y};
        view.Size              = glm::max(size, glm::vec2(1));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##canvas", V(view.Size),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const ImGuiID canvasID = ImGui::GetItemID();
        const bool    over     = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem |
                                                      ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const auto&   palette  = EditorTheme::Colors();
        ImDrawList*   draw     = ImGui::GetWindowDrawList();
        const ImVec2  min = cursor, max = {cursor.x + view.Size.x, cursor.y + view.Size.y};
        const float   zoom     = view.Zoom;
        ImFont*       font     = ImGui::GetFont();
        const float   fontSize = ImGui::GetFontSize() * zoom;
        draw->PushClipRect(min, max, true);
        draw->AddRectFilled(min, max, Color(palette.Inset));
        if (options.ShowGrid)
            for (float step : {16.0f, 128.0f})
            {
                const float spacing = step * zoom;
                if (spacing < 6)
                    continue;
                const ImU32 line  = Color(palette.Line, step > 16 ? 0.55f : 0.22f);
                const float x0    = std::fmod(view.Pan.x, spacing), y0 = std::fmod(view.Pan.y, spacing);
                for (float x = min.x + (x0 < 0 ? x0 + spacing : x0); x < max.x; x += spacing)
                    draw->AddLine({x, min.y}, {x, max.y}, line);
                for (float y = min.y + (y0 < 0 ? y0 + spacing : y0); y < max.y; y += spacing)
                    draw->AddLine({min.x, y}, {max.x, y}, line);
            }

        // Comments, behind everything.
        for (const auto& c : graph.Comments)
        {
            const ImVec2 a = V(view.ToScreen(c.Position)), b = V(view.ToScreen(c.Position + c.Size));
            const ImU32  fill = ImGui::ColorConvertFloat4ToU32({c.Color.r, c.Color.g, c.Color.b, c.Color.a});
            draw->AddRectFilled(a, b, fill, 4 * zoom);
            draw->AddRectFilled(a, {b.x, a.y + Metrics::CommentTitle}, Fade(fill | 0xff000000, 0.55f), 4 * zoom,
                                ImDrawFlags_RoundCornersTop);
            const bool selected = controller.SelectedComments().count(c.Id) > 0;
            draw->AddRect(a, b, selected ? Color(palette.Accent) : Fade(fill | 0xff000000, 0.8f), 4 * zoom, 0,
                          selected ? 2.0f : 1.0f);
            draw->AddText(font, std::max(ImGui::GetFontSize(), fontSize * 1.1f),
                          {a.x + 8, a.y + (Metrics::CommentTitle - ImGui::GetFontSize()) * 0.5f}, Color(palette.Text),
                          c.Title.c_str());
            draw->AddTriangleFilled({b.x - Metrics::ResizeGrip, b.y}, {b.x, b.y - Metrics::ResizeGrip}, b,
                                    Color(palette.TextDim, 0.6f));
        }

        // Links in their type colour.
        const float thickness = std::clamp(2.5f * zoom, 1.0f, 4.0f);
        for (size_t i = 0; i < graph.Links.size(); ++i)
        {
            const auto& link = graph.Links[i];
            auto        a    = PinPosition(graph, {link.From, link.Output, Side::Output});
            auto        b    = PinPosition(graph, {link.To, link.Input, Side::Input});
            if (!a || !b)
                continue;
            glm::vec2 c[4];
            LinkCurve(*a, *b, c);
            const bool hovered = controller.Hovered().Type == Hit::Kind::Link && controller.Hovered().LinkIndex == i;
            const bool active  = controller.IsSelected(link.From) || controller.IsSelected(link.To);
            draw->AddBezierCubic(V(view.ToScreen(c[0])), V(view.ToScreen(c[1])), V(view.ToScreen(c[2])),
                                 V(view.ToScreen(c[3])), controller.IsLinking() ? Fade(link.Color, 0.45f) : link.Color,
                                 thickness * (hovered || active ? 1.6f : 1.0f));
        }

        // Nodes, bottom to top.
        const auto pinStyle = [&](const PinRef& ref) {
            switch (controller.StateOf(ref))
            {
                case PinState::Compatible:
                    return 1.0f;
                case PinState::Incompatible:
                    return 0.2f;
                default:
                    return controller.IsLinking() ? 0.6f : 1.0f;
            }
        };
        for (const auto& n : graph.Nodes)
        {
            const auto   l = Layout(n);
            const ImVec2 a = V(view.ToScreen(l.Min)), b = V(view.ToScreen(l.Max));
            if (b.x < min.x || a.x > max.x || b.y < min.y || a.y > max.y)
                continue;
            const bool selected = controller.IsSelected(n.Id);
            const bool flashing = controller.FlashNode() == n.Id;
            if (n.Reroute)
            {
                const ImU32 colour = n.Inputs.empty() ? Color(palette.Text) : n.Inputs.front().Color;
                const ImVec2 centre = V(view.ToScreen(n.Position));
                draw->AddCircleFilled(centre, Metrics::RerouteRadius * zoom, colour);
                if (selected)
                    draw->AddCircle(centre, Metrics::RerouteRadius * zoom + 3, Color(palette.Accent), 0, 2);
                continue;
            }
            const float rounding = 5 * zoom;
            draw->AddRectFilled({a.x + 3, a.y + 4}, {b.x + 3, b.y + 4}, IM_COL32(0, 0, 0, 70), rounding);
            draw->AddRectFilled(a, b, Color(palette.Raised, 0.97f), rounding);
            const float title = Metrics::TitleHeight * zoom;
            draw->AddRectFilled(a, {b.x, a.y + title}, n.Color, rounding, ImDrawFlags_RoundCornersTop);
            draw->AddText(font, fontSize, {a.x + Metrics::Padding * zoom, a.y + (title - fontSize) * 0.5f},
                          Color(palette.Text), n.Title.c_str());
            if (!n.Subtitle.empty())
                draw->AddText(font, fontSize * 0.85f, {a.x + Metrics::Padding * zoom, a.y + title + 1 * zoom},
                              Color(palette.TextDim), n.Subtitle.c_str());
            if (n.Mark != Badge::None)
            {
                const ImVec2 centre {b.x - 12 * zoom, a.y + title * 0.5f};
                const float  r = 7 * zoom;
                draw->AddCircleFilled(centre, r, Color(n.Mark == Badge::Error ? palette.Danger : palette.Warning));
                draw->AddText(font, fontSize * 0.9f, {centre.x - fontSize * 0.13f, centre.y - fontSize * 0.45f},
                              IM_COL32(20, 20, 20, 255), "!");
                if (over && ImGui::IsMouseHoveringRect({centre.x - r, centre.y - r}, {centre.x + r, centre.y + r}))
                    ImGui::SetTooltip("%s", n.Message.c_str());
            }
            const ImU32 outline = flashing ? Color(palette.Warning) :
                                  selected ? Color(palette.Accent) :
                                  n.Mark == Badge::Error ? Color(palette.Danger, 0.8f) :
                                             IM_COL32(0, 0, 0, 120);
            draw->AddRect(a, b, outline, rounding, 0, selected || flashing ? 2.0f : 1.0f);

            const float r = Metrics::PinRadius * zoom;
            for (size_t k = 0; k < l.Inputs.size(); ++k)
            {
                const Pin&   p     = n.Inputs[l.Inputs[k]];
                const ImVec2 at    = V(view.ToScreen(l.InputPositions[k]));
                const float  alpha = pinStyle({n.Id, p.Name, Side::Input});
                if (p.Connected)
                    draw->AddCircleFilled(at, r, Fade(p.Color, alpha));
                else
                    draw->AddCircle(at, r, Fade(p.Color, alpha), 0, 1.5f);
                if (controller.StateOf({n.Id, p.Name, Side::Input}) == PinState::Compatible)
                    draw->AddCircle(at, r + 3, Fade(p.Color, 0.9f), 0, 1.5f);
                if (flashing && controller.FlashPin() == p.Name)
                    draw->AddCircle(at, r + 5, Color(palette.Warning), 0, 2);
                const char* label = (p.Label.empty() ? p.Name : p.Label).c_str();
                const float x     = at.x + r + 5 * zoom;
                draw->AddText(font, fontSize, {x, at.y - fontSize * 0.5f}, Color(palette.Text, alpha), label);
                if (!p.Connected && p.ValueWidth > 0 && options.PinValue && zoom >= 0.5f && !n.Collapsed)
                {
                    const float labelWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0, label).x;
                    ImGui::SetCursorScreenPos({x + labelWidth + 6 * zoom, at.y - (fontSize * 0.5f + 2 * zoom)});
                    ImGui::PushID(n.Id.c_str());
                    ImGui::PushID(p.Name.c_str());
                    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3 * zoom, 2 * zoom});
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {2 * zoom, 2 * zoom});
                    ImGui::BeginDisabled(host.ReadOnly());
                    options.PinValue(n, p, p.ValueWidth * zoom);
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar(2);
                    ImGui::PopFont();
                    ImGui::PopID();
                    ImGui::PopID();
                }
            }
            for (size_t k = 0; k < l.Outputs.size(); ++k)
            {
                const Pin&   p     = n.Outputs[l.Outputs[k]];
                const ImVec2 at    = V(view.ToScreen(l.OutputPositions[k]));
                const float  alpha = pinStyle({n.Id, p.Name, Side::Output});
                if (p.Connected)
                    draw->AddCircleFilled(at, r, Fade(p.Color, alpha));
                else
                    draw->AddCircle(at, r, Fade(p.Color, alpha), 0, 1.5f);
                if (controller.StateOf({n.Id, p.Name, Side::Output}) == PinState::Compatible)
                    draw->AddCircle(at, r + 3, Fade(p.Color, 0.9f), 0, 1.5f);
                if (flashing && controller.FlashPin() == p.Name)
                    draw->AddCircle(at, r + 5, Color(palette.Warning), 0, 2);
                const char* label = (p.Label.empty() ? p.Name : p.Label).c_str();
                const float w     = font->CalcTextSizeA(fontSize, FLT_MAX, 0, label).x;
                draw->AddText(font, fontSize, {at.x - r - 5 * zoom - w, at.y - fontSize * 0.5f},
                              Color(palette.Text, alpha), label);
            }
        }

        // The link being dragged, from its pin to the pointer.
        if (controller.IsLinking())
        {
            const auto& from = controller.LinkSource();
            if (auto a = PinPosition(graph, from))
            {
                const Node* node   = graph.Find(from.Node);
                ImU32       colour = Color(palette.Text);
                if (node)
                    for (const auto& p : from.PinSide == Side::Input ? node->Inputs : node->Outputs)
                        if (p.Name == from.Pin)
                            colour = p.Color;
                glm::vec2 c[4];
                const glm::vec2 m = view.ToCanvas(controller.Mouse());
                from.PinSide == Side::Output ? LinkCurve(*a, m, c) : LinkCurve(m, *a, c);
                draw->AddBezierCubic(V(view.ToScreen(c[0])), V(view.ToScreen(c[1])), V(view.ToScreen(c[2])),
                                     V(view.ToScreen(c[3])), colour, thickness);
            }
        }
        if (auto box = controller.SelectionBox())
        {
            draw->AddRectFilled(V(box->first), V(box->second), Color(palette.Accent, 0.12f));
            draw->AddRect(V(box->first), V(box->second), Color(palette.Accent, 0.8f));
        }
        draw->PopClipRect();

        // Tooltips: a pin's type, or why a dragged link cannot connect there.
        const auto& hovered = controller.Hovered();
        if (over && controller.IsLinking() && !controller.Refusal().empty())
            ImGui::SetTooltip("%s", controller.Refusal().c_str());
        else if (over && !controller.IsDragging() && hovered.Type == Hit::Kind::Pin)
            if (const Node* node = graph.Find(hovered.Target.Node))
                for (const auto& p : hovered.Target.PinSide == Side::Input ? node->Inputs : node->Outputs)
                    if (p.Name == hovered.Target.Pin && !p.Tooltip.empty())
                        ImGui::SetTooltip("%s", p.Tooltip.c_str());

        // This frame's input; widgets drawn on the canvas keep their own clicks.
        const ImGuiIO& io = ImGui::GetIO();
        Input          in;
        in.Mouse            = {io.MousePos.x, io.MousePos.y};
        const ImGuiID hoverID  = ImGui::GetHoveredID(), activeID = ImGui::GetActiveID();
        in.Hovered          = over && (hoverID == 0 || hoverID == canvasID) && (activeID == 0 || activeID == canvasID);
        in.Wheel            = in.Hovered ? io.MouseWheel : 0.0f;
        for (int b = 0; b < 3; ++b)
        {
            in.Pressed[b]  = in.Hovered && ImGui::IsMouseClicked(b);
            in.Down[b]     = io.MouseDown[b];
            in.Released[b] = ImGui::IsMouseReleased(b);
        }
        in.DoubleClicked = in.Hovered && ImGui::IsMouseDoubleClicked(0);
        in.Ctrl = io.KeyCtrl, in.Shift = io.KeyShift, in.Alt = io.KeyAlt;
        const bool keys = !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        in.Space          = keys && ImGui::IsKeyDown(ImGuiKey_Space);
        in.FrameSelection = keys && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false);
        in.FrameAll       = keys && ImGui::IsKeyPressed(ImGuiKey_Home, false);
        if (controller.IsDragging())
            in.Hovered = in.Hovered || activeID == canvasID || activeID == 0;
        controller.Update(graph, in, host);
        ImGui::SetCursorScreenPos(cursor);
        ImGui::Dummy(V(view.Size));
        ImGui::PopID();
    }
} // namespace Kans::NodeCanvas
