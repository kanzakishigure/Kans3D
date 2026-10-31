#include "kspch.h"
#include "ScriptUI.h"
#include <cmath>
#include <cstring>
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Scene/Entity.h"
#include "ScriptEngine.h"

namespace Kans::ScriptUI
{
    namespace
    {
        using Scripting::Value;
        using Scripting::WidgetAddress;
        struct Event
        {
            Scripting::UIEventData Data;
            std::string            Path, Key, Text;
        };
        std::vector<Event> Pending, Frame;
        uint64_t           Seen = 0;
        Scene*             SceneContext() { return ScriptEngine::GetSceneContext(); }
        Entity             Resolve(uint64_t id)
        {
            auto s = SceneContext();
            return s ? s->GetEntityByUUID(id) : Entity {};
        }
        int Copy(const std::string& s, char* buffer, int capacity)
        {
            if (buffer && capacity > int(s.size()))
                std::memcpy(buffer, s.c_str(), s.size() + 1);
            return int(s.size() + 1);
        }
        AssetID Asset(const char* text)
        {
            const std::string s  = text ? text : "";
            auto              id = AssetID::FromString(s);
            if (!s.empty() && s != AssetID().ToString() && !id.IsValid())
                throw std::invalid_argument("Invalid asset handle");
            return id;
        }
        glm::vec2 Vec2(const Value& v) { return {v.X, v.Y}; }
        glm::vec4 Vec4(const Value& v) { return {v.X, v.Y, v.Z, v.W}; }
        void      Store(Value& v, glm::vec2 x)
        {
            v.X = x.x;
            v.Y = x.y;
        }
        void Store(Value& v, glm::vec4 x)
        {
            v.X = x.x;
            v.Y = x.y;
            v.Z = x.z;
            v.W = x.w;
        }
        bool Finite(const Value& v)
        {
            return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z) && std::isfinite(v.W);
        }
        Event Pack(const UITarget& t)
        {
            Event out;
            out.Data.Entity = t.Entity;
            out.Data.Screen = t.Screen;
            out.Path        = t.Path;
            out.Key         = t.Key;
            return out;
        }
        void Remember()
        {
            auto s = SceneContext();
            if (!s)
                return;
            for (const auto& e : s->UI().Events())
            {
                if (e.Sequence <= Seen)
                    continue;
                Seen               = e.Sequence;
                auto out           = Pack(e.Target);
                out.Data.Type      = int(e.Type);
                out.Data.PositionX = e.Position.x;
                out.Data.PositionY = e.Position.y;
                out.Data.DeltaX    = e.Delta.x;
                out.Data.DeltaY    = e.Delta.y;
                if (e.Value && !e.Value.IsNull())
                {
                    switch (e.WidgetType)
                    {
                        case UIWidgetType::Toggle:
                            out.Data.ValueKind       = 1;
                            out.Data.Payload.Integer = e.Value.as<bool>();
                            break;
                        case UIWidgetType::Slider:
                            out.Data.ValueKind = 2;
                            out.Data.Payload.X = e.Value.as<float>();
                            break;
                        case UIWidgetType::ScrollView:
                            out.Data.ValueKind = 3;
                            Store(out.Data.Payload, UIVec2(e.Value));
                            break;
                        case UIWidgetType::Text:
                        case UIWidgetType::InputField:
                            out.Data.ValueKind = 4;
                            out.Text           = e.Value.as<std::string>();
                            break;
                        default:
                            break;
                    }
                }
                Pending.push_back(std::move(out));
            }
        }
        template<class T>
        int Add(Entity e)
        {
            if (!e.HasComponent<T>())
                e.AddComponent<T>();
            return 1;
        }
        int32_t KANS_SCRIPT_CALL AddComponent(uint64_t id, uint32_t type) noexcept
        {
            try
            {
                auto e = Resolve(id);
                if (!e)
                    return 0;
                switch (Scripting::ComponentType(type))
                {
                    case Scripting::ComponentType::CharacterControllerComponent:
                        return Add<CharacterControllerComponent>(e);
                    case Scripting::ComponentType::SpringBoneComponent:
                        return Add<SpringBoneComponent>(e);
                    case Scripting::ComponentType::RagdollComponent:
                        return Add<RagdollComponent>(e);
                    case Scripting::ComponentType::VehicleComponent:
                        return Add<VehicleComponent>(e);
                    case Scripting::ComponentType::ForceFieldComponent:
                        return Add<ForceFieldComponent>(e);
                    case Scripting::ComponentType::WindZoneComponent:
                        return Add<WindZoneComponent>(e);
                    case Scripting::ComponentType::ClothComponent:
                        return Add<ClothComponent>(e);
                    case Scripting::ComponentType::MotionColliderComponent:
                        return Add<MotionColliderComponent>(e);
                    case Scripting::ComponentType::JointComponent:
                        return Add<JointComponent>(e);
                    case Scripting::ComponentType::RigidBodyComponent:
                        return Add<RigidBodyComponent>(e);
                    case Scripting::ComponentType::BoxColliderComponent:
                        return Add<BoxColliderComponent>(e);
                    case Scripting::ComponentType::SphereColliderComponent:
                        return Add<SphereColliderComponent>(e);
                    case Scripting::ComponentType::CapsuleColliderComponent:
                        return Add<CapsuleColliderComponent>(e);
                    case Scripting::ComponentType::MeshColliderComponent:
                        return Add<MeshColliderComponent>(e);
                    case Scripting::ComponentType::AnimatorComponent:
                        return Add<AnimatorComponent>(e);
                    case Scripting::ComponentType::TransformComponent:
                        return Add<TransformComponent>(e);
                    case Scripting::ComponentType::CameraComponent:
                        return Add<CameraComponent>(e);
                    case Scripting::ComponentType::ScriptComponent:
                        return Add<ScriptComponent>(e);
                    case Scripting::ComponentType::TimelinePlayerComponent:
                        return Add<TimelinePlayerComponent>(e);
                    case Scripting::ComponentType::TextComponent:
                        return Add<TextComponent>(e);
                    case Scripting::ComponentType::SpriteRendererComponent:
                        return Add<SpriteRendererComponent>(e);
                    case Scripting::ComponentType::UIScreenComponent:
                        return Add<UIScreenComponent>(e);
                    case Scripting::ComponentType::UIWorldComponent:
                        return Add<UIWorldComponent>(e);
                    default:
                        return 0;
                }
            }
            catch (...)
            {
                return -1;
            }
        }
        int32_t KANS_SCRIPT_CALL DestroyEntity(uint64_t id) noexcept
        {
            try
            {
                auto e = Resolve(id);
                if (!e)
                    return 0;
                SceneContext()->DestroyEntity(e);
                return 1;
            }
            catch (...)
            {
                return -1;
            }
        }
        template<class T, class F>
        int Component(Entity e, bool write, F operation)
        {
            if (!e.HasComponent<T>())
                return 0;
            auto copy   = e.GetComponent<T>();
            int  result = operation(copy);
            if (write && result > 0)
                e.SetComponent<T>(copy);
            return result;
        }
        int32_t KANS_SCRIPT_CALL ComponentProperty(uint64_t    id,
                                                   uint32_t    type,
                                                   const char* property,
                                                   int32_t     write,
                                                   Value*      v,
                                                   char*       text,
                                                   int32_t     capacity) noexcept
        {
            try
            {
                auto e = Resolve(id);
                if (!e || !v || !property)
                    return 0;
                if (write && !Finite(*v))
                    return -1;
                std::string p(property);
                auto        number = [&](float& x) {
                    if (write)
                        x = v->X;
                    else
                        v->X = x;
                    return 1;
                };
                auto integer = [&](auto& x) {
                    if (write)
                        x = static_cast<std::decay_t<decltype(x)>>(v->Integer);
                    else
                        v->Integer = int64_t(x);
                    return 1;
                };
                auto color = [&](glm::vec4& x) {
                    if (write)
                        x = Vec4(*v);
                    else
                        Store(*v, x);
                    return 1;
                };
                auto asset = [&](AssetID& x) {
                    if (write)
                    {
                        x = Asset(text);
                        return 1;
                    }
                    return Copy(x.ToString(), text, capacity);
                };
                if (type == uint32_t(Scripting::ComponentType::TextComponent))
                    return Component<TextComponent>(e, write, [&](auto& c) {
                        if (p == "Text")
                        {
                            if (write)
                            {
                                c.Text = text ? text : "";
                                return 1;
                            }
                            return Copy(c.Text, text, capacity);
                        }
                        if (p == "Color")
                            return color(c.Color);
                        if (p == "FontSize")
                            return write && v->X <= 0 ? -1 : number(c.Size);
                        if (p == "WrapWidth")
                            return write && v->X < 0 ? -1 : number(c.WrapWidth);
                        if (p == "HorizontalAlignment")
                            return write && (v->Integer < 0 || v->Integer > 2) ? -1 : integer(c.Alignment.Horizontal);
                        if (p == "VerticalAlignment")
                            return write && (v->Integer < 0 || v->Integer > 3) ? -1 : integer(c.Alignment.Vertical);
                        if (p == "Billboard")
                            return write && (v->Integer < 0 || v->Integer > 2) ? -1 : integer(c.Billboard);
                        if (p == "OutlineWidth")
                            return write && v->X < 0 ? -1 : number(c.Outline.Width);
                        if (p == "OutlineColor")
                            return color(c.Outline.Color);
                        if (p == "ShadowColor")
                            return color(c.Shadow.Color);
                        if (p == "ShadowSoftness")
                            return write && v->X < 0 ? -1 : number(c.Shadow.Softness);
                        if (p == "ShadowOffset")
                        {
                            if (write)
                                c.Shadow.Offset = Vec2(*v);
                            else
                                Store(*v, c.Shadow.Offset);
                            return 1;
                        }
                        return -1;
                    });
                if (type == uint32_t(Scripting::ComponentType::SpriteRendererComponent))
                    return Component<SpriteRendererComponent>(e, write, [&](auto& c) {
                        if (p == "Texture")
                            return asset(c.Texture);
                        if (p == "Color")
                            return color(c.Color);
                        if (p == "UVRect")
                            return color(c.UVRect);
                        if (p == "FlipX")
                            return integer(c.FlipX);
                        if (p == "FlipY")
                            return integer(c.FlipY);
                        if (p == "SortingOrder")
                            return integer(c.SortingOrder);
                        return -1;
                    });
                auto surface = [&](auto& c) {
                    if (p == "Asset")
                        return asset(c.UI);
                    if (p == "Visible")
                        return integer(c.Visible);
                    if (p == "SortOrder")
                        return integer(c.SortOrder);
                    return -1;
                };
                if (type == uint32_t(Scripting::ComponentType::UIScreenComponent))
                    return Component<UIScreenComponent>(e, write, surface);
                if (type == uint32_t(Scripting::ComponentType::UIWorldComponent))
                    return Component<UIWorldComponent>(e, write, [&](auto& c) {
                        if (p == "Billboard")
                            return write && (v->Integer < 0 || v->Integer > 2) ? -1 : integer(c.Billboard);
                        if (p == "AlwaysOnTop")
                            return integer(c.AlwaysOnTop);
                        return surface(c);
                    });
                return -1;
            }
            catch (...)
            {
                return -1;
            }
        }
        std::shared_ptr<UIInstance> Instance(const WidgetAddress* address)
        {
            if (!address)
                return {};
            auto e = Resolve(address->Entity);
            return e ? SceneContext()->GetRenderWorld().GetUIInstance(e, !address->Screen) : nullptr;
        }
        UITarget Target(const WidgetAddress& a, const std::shared_ptr<UIInstance>& i, const UIRuntimeWidget& w)
        {
            return {i, w.Path, w.Key, UUID(a.Entity), a.Screen != 0};
        }
        int32_t KANS_SCRIPT_CALL WidgetProperty(const WidgetAddress* a,
                                                const char*          property,
                                                int32_t              write,
                                                Value*               v,
                                                char*                text,
                                                int32_t              capacity) noexcept
        {
            try
            {
                auto i = Instance(a);
                if (!i || !i->Data() || !a->Path || !v || !property)
                    return 0;
                const std::string path(a->Path), p(property);
                // A path remains usable through transactional asset refresh, even if the source ID changes.
                auto w = i->Find(path);
                if (!w)
                    return 0;
                if (write && !Finite(*v))
                    return -1;
                if (p == "EventValid")
                {
                    v->Integer = w->Key == (a->Key ? a->Key : "");
                    return 1;
                }
                if (p == "Key")
                    return Copy(w ? w->Key : "", text, capacity);
                if (p == "Type")
                {
                    v->Integer = path.empty() ? -1 : int(w->Data.Type);
                    return 1;
                }
                if (p == "Name")
                    return Copy(w ? w->Data.Name : "", text, capacity);
                if (p == "ChildCount")
                {
                    v->Integer = path.empty() ? i->Widgets().size() : w->Children.size();
                    return 1;
                }
                if (p == "ChildPath")
                {
                    auto& children = path.empty() ? i->Widgets() : w->Children;
                    return v->Integer >= 0 && size_t(v->Integer) < children.size() ?
                               Copy(children[size_t(v->Integer)].Path, text, capacity) :
                               -1;
                }
                if (!w)
                    return -1; // The virtual canvas root exposes lookup and children only.
                auto t = Target(*a, i, *w);
                if (p == "Focus")
                {
                    if (path.empty())
                        return -1;
                    bool ok = SceneContext()->UI().SetFocus(t);
                    Remember();
                    return ok ? 1 : -1;
                }
                if (p == "IsFocused")
                {
                    const auto& f = SceneContext()->UI().Focus();
                    v->Integer = f.Entity == t.Entity && f.Screen == t.Screen && f.Path == t.Path && f.Key == t.Key &&
                                 f.Instance.lock() == i;
                    return 1;
                }
                if (p == "Position" || p == "Size" || p == "Rect")
                {
                    auto canvas = a->Screen ? SceneContext()->GetUIViewport().Pixels() : i->Data()->ReferenceResolution;
                    if (canvas.x <= 1 || canvas.y <= 1)
                        canvas = i->Data()->ReferenceResolution;
                    i->Update(canvas);
                    if (!write)
                    {
                        if (p == "Rect")
                            Store(*v, w->Bounds);
                        else
                            Store(*v,
                                  p == "Position" ? glm::vec2(w->Bounds) :
                                                    glm::vec2(w->Bounds.z - w->Bounds.x, w->Bounds.w - w->Bounds.y));
                        return 1;
                    }
                    if (p == "Rect")
                        return -1;
                    auto rect = w->Data.Rect;
                    if (path.empty())
                        (p == "Position" ? rect.Position : rect.Size) = Vec2(*v);
                    else
                    {
                        // Translate canvas-pixel edits back to anchored local rect coordinates.
                        auto        basis = glm::mat2(w->Transform);
                        const float scale = i->PixelScale();
                        if (scale <= 0)
                            return -1;
                        if (p == "Position")
                        {
                            float     angle = glm::radians(rect.Rotation), c = std::cos(angle), sn = std::sin(angle);
                            glm::mat2 local(c * rect.Scale.x, sn * rect.Scale.x, -sn * rect.Scale.y, c * rect.Scale.y);
                            if (std::abs(glm::determinant(local)) < 1e-8f)
                                return -1;
                            auto parent = basis * glm::inverse(local);
                            if (std::abs(glm::determinant(parent)) < 1e-8f)
                                return -1;
                            auto delta = glm::inverse(parent) * (Vec2(*v) - glm::vec2(w->Bounds)) / scale;
                            for (int axis = 0; axis < 2; ++axis)
                                if (rect.AnchorsMin[axis] == rect.AnchorsMax[axis])
                                    rect.Position[axis] += delta[axis];
                                else
                                {
                                    rect.Offsets[axis] += delta[axis];
                                    rect.Offsets[axis + 2] -= delta[axis];
                                }
                        }
                        else
                        {
                            for (int col = 0; col < 2; ++col)
                                for (int row = 0; row < 2; ++row)
                                    basis[col][row] = std::abs(basis[col][row]);
                            if (std::abs(glm::determinant(basis)) < 1e-8f)
                                return -1;
                            auto size = glm::inverse(basis) * Vec2(*v);
                            if (size.x < 0 || size.y < 0)
                                return -1;
                            for (int axis = 0; axis < 2; ++axis)
                                if (rect.AnchorsMin[axis] == rect.AnchorsMax[axis])
                                    rect.Size[axis] = size[axis] / scale;
                                else
                                    rect.Offsets[axis + 2] += (w->Size[axis] - size[axis]) / scale;
                        }
                    }
                    return i->SetRect(path, rect) ? 1 : -1;
                }
                if (p == "Placeholder")
                {
                    if (w->Data.Type != UIWidgetType::InputField)
                        return -1;
                    auto name  = UIProperty<std::string>(w->Data, "Placeholder", "");
                    auto child = name.empty() ? nullptr : i->Find(path + '/' + name);
                    if (!child && !name.empty())
                        child = i->Find(name);
                    if (!write)
                        return Copy(child ? UIProperty<std::string>(child->Data, "Text", "") : "", text, capacity);
                    if (!child)
                    {
                        UIWidget placeholder;
                        placeholder.ID                 = w->Data.ID ^ (uint64_t(1) << 63);
                        placeholder.Name               = "__ScriptPlaceholder";
                        placeholder.Type               = UIWidgetType::Text;
                        placeholder.Rect.Pivot         = {0, 0};
                        placeholder.Rect.AnchorsMax    = {1, 1};
                        placeholder.Rect.Size          = {0, 0};
                        placeholder.Properties["Size"] = UIProperty<float>(w->Data, "Size", 16);
                        placeholder.Properties["Text"] = text ? text : "";
                        if (!i->AddChild(path, std::move(placeholder)) ||
                            !i->SetProperty(path, "Placeholder", YAML::Node("__ScriptPlaceholder")))
                            return -1;
                        return 1;
                    }
                    return i->SetText(child->Path, text ? text : "") ? 1 : -1;
                }
                const std::string nativeName = p == "FontSize" ? "Size" : p;
                const bool        str        = p == "Text" || p == "Texture";
                const bool        flag       = p == "Visible" || p == "Enabled" || p == "Interactable" || p == "IsOn";
                const bool        integer    = p == "CharacterLimit";
                const bool        vector4    = p == "Color" || p == "UVRect";
                const bool        vector2    = p == "ScrollPosition";
                const bool scalar = p == "Opacity" || p == "FontSize" || p == "Value" || p == "Min" || p == "Max";
                if (!str && !flag && !integer && !vector4 && !vector2 && !scalar)
                    return -1;
                if (write)
                {
                    YAML::Node n;
                    if (str)
                        n = text ? text : "";
                    else if (flag)
                        n = v->Integer != 0;
                    else if (integer)
                        n = v->Integer;
                    else if (vector4)
                        n = UIEncode(Vec4(*v));
                    else if (vector2)
                        n = UIEncode(Vec2(*v));
                    else
                        n = v->X;
                    bool isValue = p == "IsOn" || p == "Value" || p == "ScrollPosition" || p == "Text";
                    bool ok      = isValue ? SceneContext()->UI().SetValue(t, n) : i->SetProperty(path, nativeName, n);
                    Remember();
                    return ok ? 1 : -1;
                }
                if (p == "Visible")
                    v->Integer = w->Data.Visible;
                else if (p == "Enabled")
                    v->Integer = w->Data.Enabled;
                else if (flag)
                    v->Integer =
                        UIProperty<bool>(w->Data, nativeName, p == "Interactable" && UIInteractive(w->Data.Type));
                else if (integer)
                    v->Integer = UIProperty<int>(w->Data, nativeName, 0);
                else if (str)
                    return Copy(UIProperty<std::string>(w->Data, nativeName, ""), text, capacity);
                else if (vector4)
                    Store(*v,
                          UIVec4(w->Data.Properties[nativeName], p == "UVRect" ? glm::vec4(0, 0, 1, 1) : glm::vec4(1)));
                else if (vector2)
                    Store(*v, UIVec2(w->Data.Properties[nativeName]));
                else
                    v->X = UIProperty<float>(w->Data,
                                             nativeName,
                                             p == "Opacity" || p == "Max" ? 1 :
                                             p == "FontSize"              ? 16 :
                                                                            0);
                return 1;
            }
            catch (...)
            {
                return -1;
            }
        }
        int32_t KANS_SCRIPT_CALL
        ReadUIEvent(int32_t index, Scripting::UIEventData* out, int32_t field, char* text, int32_t capacity) noexcept
        {
            try
            {
                if (!out || !SceneContext())
                    return 0;
                Event        focused;
                const Event* event = nullptr;
                if (index == -1)
                {
                    const auto& f = SceneContext()->UI().Focus();
                    if (!f)
                        return 0;
                    WidgetAddress address {uint64_t(f.Entity), f.Screen ? 1 : 0, 0, f.Path.c_str(), f.Key.c_str()};
                    auto          instance = Instance(&address);
                    auto          widget   = instance ? instance->Find(f.Path) : nullptr;
                    if (!widget || widget->Key != f.Key || instance != f.Instance.lock())
                        return 0;
                    focused = Pack(f);
                    event   = &focused;
                }
                else if (index >= 0 && size_t(index) < Frame.size())
                    event = &Frame[size_t(index)];
                if (!event)
                    return 0;
                *out = event->Data;
                return Copy(field == 0 ? event->Path : field == 1 ? event->Key : event->Text, text, capacity);
            }
            catch (...)
            {
                return -1;
            }
        }
        int32_t KANS_SCRIPT_CALL UIUtility(int32_t operation, const WidgetAddress* a, Value* v) noexcept
        {
            try
            {
                auto s = SceneContext();
                if (!s || !v)
                    return 0;
                if (operation == 4)
                {
                    s->UI().SetFocus({});
                    Remember();
                    return 1;
                }
                if (operation == 5)
                    return s->UI().IsPointerOverUI();
                if (operation == 2)
                {
                    auto view = MakeRuntimeView(*s, glm::uvec2(s->GetUIViewport().Pixels()));
                    if (!view)
                        return 0;
                    const auto& c   = view->Scene.sceneCamera;
                    auto        eye = c.viewMatrix * glm::vec4(v->X, v->Y, v->Z, 1);
                    if (eye.z >= 0)
                        return 0;
                    auto clip = c.camera.GetProjectionMatrix() * eye;
                    if (clip.w <= 0)
                        return 0;
                    auto p = glm::vec2(clip) / clip.w;
                    p      = glm::vec2((p.x + 1) * .5f, (1 - p.y) * .5f) * s->GetUIViewport().Size +
                             s->GetUIViewport().Origin;
                    Store(*v, p);
                    return 1;
                }
                auto i = Instance(a);
                if (!i || !i->Data() || !a->Screen)
                    return 0;
                const auto& viewport = s->GetUIViewport();
                if (operation == 0)
                    Store(*v, viewport.Map(Vec2(*v)));
                else if (operation == 1)
                {
                    if (viewport.Scale.x <= 0 || viewport.Scale.y <= 0)
                        return 0;
                    Store(*v, Vec2(*v) / viewport.Scale + viewport.Origin);
                }
                else
                    return 0;
                return 1;
            }
            catch (...)
            {
                return -1;
            }
        }
    } // namespace
    void Install(Scripting::NativeApi& api)
    {
        api.AddComponent      = &AddComponent;
        api.DestroyEntity     = &DestroyEntity;
        api.ComponentProperty = &ComponentProperty;
        api.WidgetProperty    = &WidgetProperty;
        api.ReadUIEvent       = &ReadUIEvent;
        api.UIUtility         = &UIUtility;
    }
    void BeginFrame()
    {
        Remember();
        Frame.clear();
        Frame.swap(Pending);
    }
    void Reset()
    {
        Seen = 0;
        Pending.clear();
        Frame.clear();
    }
} // namespace Kans::ScriptUI
