#pragma once
#include "Kans3D/Core/Application.h"
#include "Kans3D/Core/Base/Base.h"
#include "Kans3D/Core/Debug/FatalError.h"
#include "Kans3D/Core/Debug/Instrumentor.h"
#include "Kans3D/RHI/Backend.h"

extern Kans::Application* Kans::createApplication(int argc, char** argv);

namespace Kans
{
    int Main(int argc, char** argv)
    {
        Log::Init();
        Log::StartFileLogging();
        FatalError::Install();
        RenderHardware::SetStartupArguments(argc, argv);

        const auto lifecycle = [&] {
            PROFILE_BEGIN_SESSION("startup", "ProfileSpecication/startup-profile.json");
            Application* app = Kans::createApplication(argc, argv);
            PROFILE_END_SESSION();

            PROFILE_BEGIN_SESSION("runtime", "ProfileSpecication/runtime-profile.json");
            app->run();
            PROFILE_END_SESSION();

            PROFILE_BEGIN_SESSION("shutdown", "ProfileSpecication/shutdown-profile.json");
            delete app;
            PROFILE_END_SESSION();
        };
        // No catch here: an exception nothing handles reaches std::terminate (FatalError's handler)
        // before the stack unwinds, so the report and the minidump show the throw site, and a
        // debugger still stops there.
        lifecycle();

        FatalError::Shutdown();
        Log::ShutDown();
        return 0;
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
