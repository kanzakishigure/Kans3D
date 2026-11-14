#pragma once
#include <imgui_internal.h>

namespace Kans
{
    class ImGuiRecovery
    {
    public:
        ImGuiRecovery() { ImGui::ErrorRecoveryStoreState(&m_State); }
        void Recover() const
        {
            auto&      io              = ImGui::GetIO();
            const bool assertEnabled   = io.ConfigErrorRecoveryEnableAssert;
            const bool tooltipEnabled  = io.ConfigErrorRecoveryEnableTooltip;
            const bool recoveryEnabled = io.ConfigErrorRecovery;
            const bool debugLogEnabled = io.ConfigErrorRecoveryEnableDebugLog;
            // The C++ exception is already reported. Recovery must not assert on the
            // missing End/Pop calls caused by unwinding, or open a tooltip mid-recovery.
            io.ConfigErrorRecoveryEnableAssert   = false;
            io.ConfigErrorRecoveryEnableTooltip  = false;
            io.ConfigErrorRecovery               = true;
            io.ConfigErrorRecoveryEnableDebugLog = true;
            ImGui::ErrorRecoveryTryToRecoverState(&m_State);
            io.ConfigErrorRecoveryEnableAssert   = assertEnabled;
            io.ConfigErrorRecoveryEnableTooltip  = tooltipEnabled;
            io.ConfigErrorRecovery               = recoveryEnabled;
            io.ConfigErrorRecoveryEnableDebugLog = debugLogEnabled;
        }

    private:
        ImGuiErrorRecoveryState m_State;
    };
} // namespace Kans
