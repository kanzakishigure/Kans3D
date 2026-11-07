#include "kspch.h"
#include "RenderFeature.h"
#include <mutex>
#include <set>
#include <stdexcept>
#include "Kans3D/Core/Log/Log.h"

namespace Kans
{
    void RenderFeatureSet::Register(std::string name, std::unique_ptr<RenderFeature> feature, bool enabled)
    {
        if (name.empty() || !feature)
            throw std::invalid_argument("RenderFeature: invalid registration");
        for (const auto& entry : m_Entries)
            if (entry.Name == name)
                throw std::invalid_argument("RenderFeature: duplicate registration '" + name + "'");
        m_Entries.push_back({std::move(name), std::move(feature), enabled});
    }
    void RenderFeatureSet::SetEnabled(const std::string& name, bool enabled)
    {
        for (auto& entry : m_Entries)
            if (entry.Name == name)
            {
                entry.Enabled = enabled;
                return;
            }
        throw std::invalid_argument("RenderFeature: unknown feature '" + name + "'");
    }
    bool RenderFeatureSet::IsEnabled(const std::string& name) const
    {
        for (const auto& entry : m_Entries)
            if (entry.Name == name)
                return entry.Enabled;
        throw std::invalid_argument("RenderFeature: unknown feature '" + name + "'");
    }
    void RenderFeatureSet::ReportPipelineFailure(const std::string& message)
    {
        static std::mutex                 mutex;
        static std::set<std::string>      logged;
        const std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(message).second && Log::GetCoreLogger())
            CORE_ERROR("Skipping render feature: {}", message);
    }
    void RenderFeatureSet::Prepare(RenderView& view, RenderFrame& frame)
    {
        for (auto& entry : m_Entries)
        {
            entry.FailedFrame = false;
            if (entry.Enabled)
                try
                {
                    entry.Feature->Prepare(view, frame);
                }
                catch (const RenderHardware::PipelineCreationError& error)
                {
                    entry.FailedFrame = true;
                    ReportPipelineFailure(error.what());
                }
        }
    }
    void RenderFeatureSet::Inject(RenderExtensionPoint point, const RenderFeatureContext& context)
    {
        for (auto& entry : m_Entries)
            if (entry.Enabled && !entry.FailedFrame && entry.Feature->RunsAt(point))
            {
                const auto first = context.Graph.PassCount();
                try
                {
                    entry.Feature->AddPasses(point, context);
                }
                catch (const RenderHardware::PipelineCreationError& error)
                {
                    entry.FailedFrame = true;
                    ReportPipelineFailure(error.what());
                }
                context.Graph.GuardPipelineFailures(
                    first,
                    [&entry](const auto& error) {
                        entry.FailedFrame = true;
                        ReportPipelineFailure(error);
                    },
                    [&entry] { return entry.FailedFrame; });
                // Order without retaining unused feature outputs. No temporary callback/list copies.
                for (size_t pass = first; pass < context.Graph.PassCount(); ++pass)
                    if (pass)
                        context.Graph.OrderAfter(GraphPass(pass), GraphPass(pass - 1));
            }
    }
} // namespace Kans
