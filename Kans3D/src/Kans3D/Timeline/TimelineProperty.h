#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <glm/glm.hpp>
namespace Kans
{
    class Scene;
    class Entity;
    // Stable paths; vector fields may select a trailing .x/.y/.z/.w (or .r/.g/.b/.a).
    // Material.<slot>.<parameter> uses per-instance overrides, never edits the asset.
    bool TimelineProperty(Scene&, Entity, const std::string& path, glm::vec4& value, bool write);
    std::optional<std::pair<std::string_view, std::string_view>> MaterialParameterPath(std::string_view path);
    bool TimelineHasComponent(Entity, const std::string& name);
} // namespace Kans
