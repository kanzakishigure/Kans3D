#include "SceneHistory.h"
#include <algorithm>
#include "EditStamp.h"

namespace Kans
{
    void SceneHistory::Reset(const Scene& scene)
    {
        m_Undo.clear();
        m_Redo.clear();
        m_Current      = scene.Capture();
        m_Version      = m_NextVersion++;
        m_SavedVersion = m_Version;
    }

    bool SceneHistory::Record(const Scene& scene)
    {
        auto state = scene.Capture();
        if (state == m_Current)
            return false;
        Step step;
        step.Label         = DescribeChange(m_Current, state);
        step.Stamp         = NextEditStamp();
        step.BeforeVersion = m_Version;
        step.AfterVersion  = m_NextVersion++;
        step.Before        = std::move(m_Current);
        step.After         = state;
        m_Current          = std::move(state);
        m_Version          = step.AfterVersion;
        m_Undo.push_back(std::move(step));
        if (m_Undo.size() > Limit)
            m_Undo.pop_front();
        m_Redo.clear();
        return true;
    }

    bool SceneHistory::Undo(Scene& scene)
    {
        if (m_Undo.empty())
            return false;
        Step step = std::move(m_Undo.back());
        m_Undo.pop_back();
        scene.Restore(step.Before);
        m_Current = step.Before;
        m_Version = step.BeforeVersion;
        m_Redo.push_back(std::move(step));
        return true;
    }

    bool SceneHistory::Redo(Scene& scene)
    {
        if (m_Redo.empty())
            return false;
        Step step = std::move(m_Redo.back());
        m_Redo.pop_back();
        scene.Restore(step.After);
        m_Current = step.After;
        m_Version = step.AfterVersion;
        m_Undo.push_back(std::move(step));
        return true;
    }

    std::optional<uint64_t> SceneHistory::UndoStamp() const
    {
        return m_Undo.empty() ? std::nullopt : std::optional<uint64_t>(m_Undo.back().Stamp);
    }

    std::optional<uint64_t> SceneHistory::RedoStamp() const
    {
        return m_Redo.empty() ? std::nullopt : std::optional<uint64_t>(m_Redo.back().Stamp);
    }

    std::vector<std::string> SceneHistory::UndoLabels() const
    {
        std::vector<std::string> labels;
        for (auto it = m_Undo.rbegin(); it != m_Undo.rend(); ++it)
            labels.push_back(it->Label);
        return labels;
    }

    std::vector<std::string> SceneHistory::RedoLabels() const
    {
        std::vector<std::string> labels;
        for (auto it = m_Redo.rbegin(); it != m_Redo.rend(); ++it)
            labels.push_back(it->Label);
        return labels;
    }

    std::string DescribeChange(const SceneSnapshot& before, const SceneSnapshot& after)
    {
        std::vector<const EntitySnapshot*> added, removed, changed;
        for (const auto& entity : after.Entities)
        {
            const auto* old = before.Find(entity.ID);
            if (!old)
                added.push_back(&entity);
            else if (!(*old == entity))
                changed.push_back(&entity);
        }
        for (const auto& entity : before.Entities)
            if (!after.Find(entity.ID))
                removed.push_back(&entity);

        const auto count = [](size_t n) { return std::to_string(n) + " entities"; };
        if (!added.empty() && removed.empty())
            return "Create " + (added.size() == 1 ? added[0]->Tag : count(added.size()));
        if (!removed.empty() && added.empty())
            return "Delete " + (removed.size() == 1 ? removed[0]->Tag : count(removed.size()));
        if (!added.empty())
            return "Replace entities";
        if (changed.empty() && before.Environment != after.Environment)
            return "Edit environment";
        // A reparent also changes the old and new parents' child lists.
        for (const auto* entity : changed)
            if (entity->Relationship.Parent != before.Find(entity->ID)->Relationship.Parent)
                return "Reparent " + entity->Tag;
        if (changed.empty() || std::all_of(changed.begin(), changed.end(), [&](const EntitySnapshot* entity) {
                const auto* old = before.Find(entity->ID);
                EntitySnapshot same = *old;
                same.Relationship   = entity->Relationship;
                return same == *entity;
            }))
            return "Reorder entities";
        if (changed.size() > 1)
            return "Edit " + count(changed.size());

        const auto& now = *changed[0];
        const auto& old = *before.Find(now.ID);
        if (old.Tag != now.Tag)
            return "Rename " + old.Tag + " to " + now.Tag;
        const bool components = old.Camera.has_value() != now.Camera.has_value() ||
                                old.Sprite.has_value() != now.Sprite.has_value() ||
                                old.Model.has_value() != now.Model.has_value() ||
                                old.Editable.has_value() != now.Editable.has_value() ||
                                old.DirLight.has_value() != now.DirLight.has_value() ||
                                old.PointLight.has_value() != now.PointLight.has_value() ||
                                old.Script.has_value() != now.Script.has_value();
        if (components)
            return "Change components of " + now.Tag;
        EntitySnapshot onlyTransform = old;
        onlyTransform.Transform      = now.Transform;
        if (onlyTransform == now)
        {
            if (old.Transform.Position != now.Transform.Position && old.Transform.Rotation == now.Transform.Rotation &&
                old.Transform.Scale == now.Transform.Scale)
                return "Move " + now.Tag;
            if (old.Transform.Rotation != now.Transform.Rotation && old.Transform.Position == now.Transform.Position &&
                old.Transform.Scale == now.Transform.Scale)
                return "Rotate " + now.Tag;
            if (old.Transform.Scale != now.Transform.Scale && old.Transform.Position == now.Transform.Position &&
                old.Transform.Rotation == now.Transform.Rotation)
                return "Scale " + now.Tag;
            return "Transform " + now.Tag;
        }
        EntitySnapshot onlyMesh = old;
        onlyMesh.Editable       = now.Editable;
        if (onlyMesh == now && old.Editable && now.Editable && !(old.Editable->Mesh == now.Editable->Mesh))
            return "Edit mesh of " + now.Tag;
        return "Edit " + now.Tag;
    }
} // namespace Kans
