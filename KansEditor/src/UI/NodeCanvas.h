#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// A reusable node graph canvas: pan and zoom, nodes with typed pins, Bezier links, reroutes,
// comment boxes, selection and drags. The model, geometry, hit testing and interaction are
// ImGui-free (driven by an Input snapshot and a Host that owns the data); Draw() is the thin
// ImGui layer. The owner rebuilds a Graph from its data each frame and applies the Host calls.
namespace Kans::NodeCanvas
{
    // Canvas units are pixels at zoom 1.
    namespace Metrics
    {
        inline constexpr float TitleHeight = 26, RowHeight = 22, PinRadius = 4.5f, Padding = 10;
        inline constexpr float MinWidth = 120, MaxWidth = 360, CharWidth = 7.2f, BadgeWidth = 20;
        inline constexpr float RerouteRadius = 7, CommentTitle = 28, ResizeGrip = 16;
        // Screen pixels, independent of zoom and of the drawn pin size.
        inline constexpr float PinHitRadius = 8, LinkHitDistance = 5;
    } // namespace Metrics
    inline constexpr float MinZoom = 0.2f, MaxZoom = 2.0f;

    struct View
    {
        glm::vec2 Origin {0};         // Screen position of the canvas's top-left corner.
        glm::vec2 Size {800, 600};    // Screen size of the canvas.
        glm::vec2 Pan {0};            // Screen offset of canvas (0, 0) from Origin.
        float     Zoom = 1;
        glm::vec2 ToScreen(glm::vec2 p) const { return Origin + Pan + p * Zoom; }
        glm::vec2 ToCanvas(glm::vec2 s) const { return (s - Origin - Pan) / Zoom; }
        // Sets the zoom (clamped to MinZoom..MaxZoom) keeping the canvas point under `screen` fixed.
        void ZoomAt(glm::vec2 screen, float zoom);
        // Centres canvas rectangle min..max and zooms to fit it inside Size less `margin` screen
        // pixels on every side (never above 1 or outside the zoom range).
        void Frame(glm::vec2 min, glm::vec2 max, float margin = 40);
    };

    enum class Side
    {
        Input,
        Output
    };
    struct PinRef
    {
        std::string Node, Pin;
        Side        PinSide = Side::Output;
        bool        operator==(const PinRef& b) const { return Node == b.Node && Pin == b.Pin && PinSide == b.PinSide; }
        bool        operator!=(const PinRef& b) const { return !(*this == b); }
    };
    struct Pin
    {
        std::string Name, Label, Tooltip;
        uint32_t    Color     = 0xffffffff; // ImGui ABGR.
        bool        Connected = false;
        float       ValueWidth = 0; // Canvas width of an inline value editor (unconnected inputs).
    };
    enum class Badge
    {
        None,
        Warning,
        Error
    };
    struct Node
    {
        std::string      Id, Title, Subtitle;
        uint32_t         Color = 0xff505050;
        glm::vec2        Position {0};
        std::vector<Pin> Inputs, Outputs;
        bool             Collapsed = false, Reroute = false;
        Badge            Mark      = Badge::None;
        std::string      Message;
    };
    struct Link
    {
        std::string From, Output, To, Input;
        uint32_t    Color = 0xffffffff;
    };
    struct Comment
    {
        std::string Id, Title;
        glm::vec2   Position {0}, Size {320, 200};
        glm::vec4   Color {0.35f, 0.38f, 0.45f, 0.35f};
    };
    struct Graph
    {
        std::vector<Node>    Nodes; // Drawn in order: the last node is on top.
        std::vector<Link>    Links;
        std::vector<Comment> Comments;
        const Node*          Find(const std::string& id) const;
    };

    // Geometry of a node in canvas units: its rectangle and the positions of its shown pins
    // (collapsed nodes show only connected pins).
    struct NodeLayout
    {
        glm::vec2              Min {0}, Max {0};
        std::vector<size_t>    Inputs, Outputs; // Indices of the shown pins.
        std::vector<glm::vec2> InputPositions, OutputPositions;
    };
    NodeLayout               Layout(const Node& node);
    std::optional<glm::vec2> PinPosition(const Graph& graph, const PinRef& pin);
    // Bezier control points of a link from an output at `a` to an input at `b` (canvas units).
    void                     LinkCurve(glm::vec2 a, glm::vec2 b, glm::vec2 out[4]);
    // Nodes whose rectangle lies entirely inside the comment.
    std::vector<std::string> NodesInside(const Graph& graph, const Comment& comment);
    // Bounds of the given nodes (all when empty) and comments; false when there is nothing.
    bool                     Bounds(const Graph& graph, const std::set<std::string>& nodes, glm::vec2& min, glm::vec2& max);

    struct Hit
    {
        enum class Kind
        {
            None,
            Node,
            Pin,
            Link,
            CommentTitle,
            CommentResize,
            CommentBody
        };
        Kind        Type = Kind::None;
        std::string Id; // Node or comment.
        PinRef      Target;
        size_t      LinkIndex = 0;
    };
    // Topmost first: pins (within PinHitRadius screen pixels) and bodies of nodes from the top,
    // then links, then comments.
    Hit HitTest(const Graph& graph, const View& view, glm::vec2 screen);

    // One frame of input over the canvas.
    struct Input
    {
        glm::vec2 Mouse {0};
        float     Wheel   = 0;
        bool      Hovered = true; // The pointer is over the canvas and not over a widget on it.
        bool      Pressed[3] {}, Down[3] {}, Released[3] {}; // Left, right, middle.
        bool      DoubleClicked = false;
        bool      Ctrl = false, Shift = false, Alt = false, Space = false;
        bool      FrameSelection = false, FrameAll = false; // F and Home.
    };

    // The owner of the data. Edits arrive here; a drag sends Translate every frame and EndEdit once.
    class Host
    {
    public:
        virtual ~Host() = default;
        virtual bool ReadOnly() const { return false; }
        // Whether `output` may link to `input`; `reason` says why not.
        virtual bool CanConnect(const PinRef& output, const PinRef& input, std::string* reason) = 0;
        // Links output to input, replacing the input's link.
        virtual void Connect(const PinRef& output, const PinRef& input) = 0;
        virtual void Disconnect(const PinRef& pin)        = 0; // Every link on the pin.
        virtual void DisconnectLink(size_t link)          = 0;
        // Ctrl-drag: moves every link of `from` onto `to` (same side).
        virtual void MoveLinks(const PinRef& from, const PinRef& to) = 0;
        virtual void InsertReroute(size_t link, glm::vec2 position)  = 0;
        virtual void Translate(const std::vector<std::string>& nodes,
                               const std::vector<std::string>& comments,
                               glm::vec2                       delta)  = 0;
        virtual void ResizeComment(const std::string& id, glm::vec2 position, glm::vec2 size) = 0;
        virtual void EndEdit() {}
        // A link dropped on empty space (`from` set), or a right click on empty space.
        virtual void RequestSearch(const std::optional<PinRef>& from, glm::vec2 position) = 0;
        // A right click on a node, pin or link.
        virtual void ContextMenu(const Hit&, glm::vec2) {}
    };

    enum class PinState
    {
        Neutral,
        Compatible,
        Incompatible
    };

    class Controller
    {
    public:
        View&       GetView() { return m_View; }
        const View& GetView() const { return m_View; }

        const std::set<std::string>& Selection() const { return m_Selected; }
        const std::set<std::string>& SelectedComments() const { return m_SelectedComments; }
        void                         SetSelection(std::set<std::string> nodes, std::set<std::string> comments = {});
        void                         ClearSelection();
        bool                         IsSelected(const std::string& node) const { return m_Selected.count(node) > 0; }

        // Grid snapping of dragged nodes (0: off), in canvas units.
        float GridSnap = 0;

        void Update(const Graph& graph, const Input& input, Host& host);
        void FrameSelection(const Graph& graph);
        void FrameAll(const Graph& graph);
        // Focuses the view on one node (diagnostics) and flashes one of its pins.
        void FocusNode(const Graph& graph, const std::string& node, const std::string& pin = {});

        // Drawing state.
        bool                                         IsLinking() const { return m_Drag == Drag::Link; }
        const PinRef&                                LinkSource() const { return m_LinkFrom; }
        bool                                         IsMovingLinks() const { return m_MovingLinks; }
        glm::vec2                                    Mouse() const { return m_Mouse; }
        PinState                                     StateOf(const PinRef& pin) const;
        const std::string&                           Refusal() const { return m_Refusal; } // Hovered incompatible pin.
        std::optional<std::pair<glm::vec2, glm::vec2>> SelectionBox() const; // Screen rectangle.
        const Hit&                                   Hovered() const { return m_Hovered; }
        const std::string&                           FlashNode() const { return m_FlashNode; }
        const std::string&                           FlashPin() const { return m_FlashPin; }
        bool                                         IsDragging() const { return m_Drag != Drag::None; }

    private:
        enum class Drag
        {
            None,
            Pan,
            Nodes,
            Resize,
            Link,
            Box
        };
        static std::string Key(const PinRef& p);
        void               Release(const Graph& graph, const Input& input, Host& host);

        View                  m_View;
        std::set<std::string> m_Selected, m_SelectedComments;
        Drag                  m_Drag = Drag::None;
        glm::vec2             m_Mouse {0}, m_PressMouse {0};
        Hit                   m_Hovered;
        // Node and comment drags.
        std::vector<std::string> m_DragNodes, m_DragComments;
        glm::vec2                m_DragTotal {0}, m_Applied {0}, m_Anchor {0};
        // Comment resize.
        std::string m_Resizing;
        glm::vec2   m_ResizeStart {0}, m_ResizeSize {0};
        // Link drags.
        PinRef                          m_LinkFrom;
        bool                            m_MovingLinks = false;
        std::map<std::string, PinState> m_PinStates;
        std::string                     m_Refusal;
        bool                            m_PanByMiddle = false;
        std::string                     m_FlashNode, m_FlashPin;
    };

    // ---- ImGui layer ----
    struct DrawOptions
    {
        // Draws an unconnected input's value editor at the cursor, `width` screen pixels wide, with
        // the font already scaled; returns true while it changed the value.
        std::function<void(const Node&, const Pin&, float width)> PinValue;
        bool                                                      ShowGrid = true;
    };
    // Draws the canvas filling `size` at the cursor, then runs the controller on this frame's input.
    void Draw(const char* id, const Graph& graph, Controller& controller, Host& host, glm::vec2 size,
              const DrawOptions& options = {});
} // namespace Kans::NodeCanvas
