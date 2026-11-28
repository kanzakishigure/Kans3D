#pragma once
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Base/Base.h"
#include "Kans3D/Core/Debug/FatalError.h"
#include "Kans3D/Core/Debug/Profiler.h"
#include "Kans3D/RHI/Backend.h"

extern Kans::Application* Kans::createApplication(int argc, char** argv);

namespace Kans
{
    int Main(int argc, char** argv)
    {
        KANS_PROFILE_THREAD("Main");
        Log::Init();
        Log::StartFileLogging();
        FatalError::Install();
        RenderHardware::SetStartupArguments(argc, argv);

        const auto lifecycle = [&] {
            Application* app;
            {
                KANS_PROFILE_SCOPE("Startup");
                app = Kans::createApplication(argc, argv);
            }
            app->run();
            {
                KANS_PROFILE_SCOPE("Shutdown");
                delete app;
            }
        };
        // No catch here: an exception nothing handles reaches std::terminate (FatalError's handler)
        // before the stack unwinds, so the report and the minidump show the throw site, and a
        // debugger still stops there.
        int exitCode = 0;
#ifdef KANS_ENTRY_HOOK
        if (const auto result = KANS_ENTRY_HOOK(argc, argv))
            exitCode = *result;
        else
#endif
            lifecycle();

        FatalError::Shutdown();
        Log::ShutDown();
        return exitCode;
    }
} // namespace Kans

#ifdef PLATFORM_WINDOWS

#ifdef KS_DIST
INT APIENTRY WinMain(HINSTANCE hINSTANCE, HINSTANCE hPrevINSTANCE, PSTR lpCmdLine, INT nCmdshow)
{
    return Kans::Main(__argc, __argv);
}
#else
int main(int argc, char** argv) { return Kans::Main(argc, argv); }
#endif
#endif
#ifdef PLATFORM_LINUX

int main(int argc, char** argv) { return Kans::Main(argc, argv); }

#endif
