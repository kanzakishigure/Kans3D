#include "kspch.h"
#include "ScriptInput.h"
#include "ScriptEngine.h"

namespace Kans::ScriptInput
{
    namespace
    {
        ScriptMouseFrame           s_Mouse;
        std::optional<std::string> s_SceneLoad;

        // Queries: 0 position, 1 delta, 2 scroll (into X and Y), 3 button `button` held (Integer).
        int32_t KANS_SCRIPT_CALL MouseState(int32_t query, int32_t button, Scripting::Value* out) noexcept
        {
            if (!out)
                return -1;
            *out = {};
            switch (query)
            {
                case 0:
                    out->X = s_Mouse.Position.x, out->Y = s_Mouse.Position.y;
                    return 1;
                case 1:
                    out->X = s_Mouse.Delta.x, out->Y = s_Mouse.Delta.y;
                    return 1;
                case 2:
                    out->X = s_Mouse.Scroll.x, out->Y = s_Mouse.Scroll.y;
                    return 1;
                case 3:
                    if (button < 0 || button >= int32_t(s_Mouse.Buttons.size()))
                        return -1;
                    out->Integer = s_Mouse.Buttons[size_t(button)];
                    return 1;
                default:
                    return -1;
            }
        }
        // 1: queued; 0: no scene is playing; -1: not a project-relative .kans path.
        int32_t KANS_SCRIPT_CALL LoadScene(const char* path) noexcept
        {
            try
            {
                if (!path)
                    return -1;
                const std::filesystem::path p = std::filesystem::u8path(path);
                if (p.empty() || p.is_absolute() || p.has_root_name() || p.extension() != ".kans")
                    return -1;
                for (const auto& part : p.lexically_normal())
                    if (part == "..")
                        return -1;
                if (!ScriptEngine::GetSceneContext())
                    return 0;
                RequestSceneLoad(path);
                return 1;
            }
            catch (...)
            {
                return -1;
            }
        }
    } // namespace

    void                    SetMouse(const ScriptMouseFrame& frame) { s_Mouse = frame; }
    const ScriptMouseFrame& Mouse() { return s_Mouse; }
    void                    Reset()
    {
        s_Mouse = {};
        s_SceneLoad.reset();
    }
    void RequestSceneLoad(std::string path) { s_SceneLoad = std::move(path); }
    std::optional<std::string> TakeSceneLoad()
    {
        auto r = std::move(s_SceneLoad);
        s_SceneLoad.reset();
        return r;
    }
    void Install(Scripting::NativeApi& api)
    {
        api.MouseState = &MouseState;
        api.LoadScene  = &LoadScene;
    }
} // namespace Kans::ScriptInput
