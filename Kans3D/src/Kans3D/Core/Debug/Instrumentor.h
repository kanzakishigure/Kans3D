#pragma once
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

#include "Kans3D/Core/Log/Log.h"
namespace KansTools
{
    struct ProfileResult
    {
        std::string Name;
        long long   Start, End;
        uint32_t    ThreadID;
    };

    struct InstrumentationSession
    {
        std::string Name;
    };

    class Instrumentor
    {
    private:
        InstrumentationSession* m_CurrentSession;
        std::ofstream           m_OutputStream;
        int                     m_ProfileCount;

    public:
        Instrumentor() : m_CurrentSession(nullptr), m_ProfileCount(0) {}

        void BeginSession(const std::string& name, const std::string& filepath = "results.json")
        {
            m_OutputStream.open(filepath);
            WriteHeader();
            m_CurrentSession = new InstrumentationSession {name};
        }

        void EndSession()
        {
            WriteFooter();
            m_OutputStream.close();
            delete m_CurrentSession;
            m_CurrentSession = nullptr;
            m_ProfileCount   = 0;
        }

        void WriteProfile(const ProfileResult& result)
        {
            if (m_ProfileCount++ > 0)
                m_OutputStream << ",";

            std::string name = result.Name;
            std::replace(name.begin(), name.end(), '"', '\'');

            m_OutputStream << "{";
            m_OutputStream << "\"cat\":\"function\",";
            m_OutputStream << "\"dur\":" << (result.End - result.Start) << ',';
            m_OutputStream << "\"name\":\"" << name << "\",";
            m_OutputStream << "\"ph\":\"X\",";
            m_OutputStream << "\"pid\":0,";
            m_OutputStream << "\"tid\":" << result.ThreadID << ",";
            m_OutputStream << "\"ts\":" << result.Start;
            m_OutputStream << "}";

            m_OutputStream.flush();
        }

        void WriteHeader()
        {
            m_OutputStream << "{\"otherData\": {},\"traceEvents\":[";
            m_OutputStream.flush();
        }

        void WriteFooter()
        {
            m_OutputStream << "]}";
            m_OutputStream.flush();
        }

        static Instrumentor& Get()
        {
            static Instrumentor instance;
            return instance;
        }
    };

    //////////////////////////////////////////////////////////////////////////

    class InstrumentationTimer
    {
    public:
        InstrumentationTimer(const char* name) : m_Name(name), m_Stoped(false)
        {
            m_StartTimePoint = std::chrono::high_resolution_clock::now();
        }
        ~InstrumentationTimer() { stop(); }

    private:
        void stop()
        {
            auto      stoptimepoint = std::chrono::high_resolution_clock::now();
            long long start =
                std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimePoint).time_since_epoch().count();
            long long end =
                std::chrono::time_point_cast<std::chrono::microseconds>(stoptimepoint).time_since_epoch().count();

            m_Stoped       = true;
            float duration = (end - start) * 0.001f;
            Instrumentor::Get().WriteProfile({m_Name, start, end, 0});
        }

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimePoint;
        bool                                                        m_Stoped;
        const char*                                                 m_Name;
    };

    class InstrumentationTimerLogger
    {
    public:
        InstrumentationTimerLogger(const char* name) : m_Name(name), m_Stoped(false)
        {
            m_StartTimePoint = std::chrono::high_resolution_clock::now();
        }
        ~InstrumentationTimerLogger() { stop(); }
        void stop()
        {
            auto      stoptimepoint = std::chrono::high_resolution_clock::now();
            long long start =
                std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimePoint).time_since_epoch().count();
            long long end =
                std::chrono::time_point_cast<std::chrono::microseconds>(stoptimepoint).time_since_epoch().count();

            m_Stoped       = true;
            float duration = (end - start) * 0.000001f;
            CORE_ERROR_TAG("Instrumentor", "{} takes time : {}s", m_Name, duration);
        }

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimePoint;
        bool                                                        m_Stoped;
        const char*                                                 m_Name;
    };
} // namespace KansTools

#ifdef KS_DEBUG
#define KS_PROFILE
#endif // NATIVE PROFILE

#ifdef KS_PROFILE
#define PROFILE_BEGIN_SESSION(name, filepath) ::KansTools::Instrumentor::Get().BeginSession(name, filepath);
#define PROFILE_END_SESSION() ::KansTools::Instrumentor::Get().EndSession();
#define PROFILE_FUCTION() PROFILE_SCOPE(__FUNCTION__);

#define PROFILE_SCOPE(name) ::KansTools::InstrumentationTimer timer##__Line__(name);
#define PROFILE_SCOPE_LOG(name) ::KansTools::InstrumentationTimerLogger timerlogger##__Line__(name);
#else
#define PROFILE_BEGIN_SESSION(name, filepath)
#define PROFILE_END_SESSION()
#define PROFILE_FUCTION()

#define PROFILE_SCOPE(name)
#define PROFILE_SCOPE_LOG(name)
#endif