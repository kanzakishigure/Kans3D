#pragma once
#include <vector>
#include <glm/glm.hpp>
namespace Kans
{
    struct GlobalWindComponent
    {
        glm::vec3 Direction {1, 0, 0};
        float     Strength = 0, TurbulenceAmplitude = 0, TurbulenceFrequency = 1;
    };
    struct FieldImpulse
    {
        glm::vec3 Position {0}, Impulse {0};
        float     Radius = 0;
    };
    class ForceFieldSet
    {
    public:
        void Clear()
        {
            m_Winds.clear();
            m_Impulses.clear();
        }
        void                             AddGlobalWind(const GlobalWindComponent& wind) { m_Winds.push_back(wind); }
        void                             AddImpulse(const FieldImpulse& impulse) { m_Impulses.push_back(impulse); }
        glm::vec3                        SampleWind(glm::vec3 position, double time) const;
        const std::vector<FieldImpulse>& GetImpulses() const { return m_Impulses; }

    private:
        std::vector<GlobalWindComponent> m_Winds;
        std::vector<FieldImpulse>        m_Impulses;
    };
} // namespace Kans
