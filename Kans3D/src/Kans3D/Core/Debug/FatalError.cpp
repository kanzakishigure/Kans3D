#include "kspch.h"
#include "FatalError.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <typeinfo>
#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#else
#include <cxxabi.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace Kans::FatalError
{
    namespace
    {
        std::atomic_flag       s_Reporting = ATOMIC_FLAG_INIT;
        std::atomic<bool>      s_ShowDialog {true};
        std::terminate_handler s_PreviousTerminate = nullptr;
        bool                   s_Installed         = false;
        std::filesystem::path  s_LogPath;
#ifdef _WIN32
        HANDLE                       s_LogHandle      = INVALID_HANDLE_VALUE;
        LPTOP_LEVEL_EXCEPTION_FILTER s_PreviousFilter = nullptr;
#else
        int              s_LogFd     = -1;
        constexpr int    s_Signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
        struct sigaction s_PreviousActions[5] {};
        char             s_SignalReports[5][8192] {};
        size_t           s_SignalReportSizes[5] {};
        // An alternate stack also gives the main thread a chance to report stack exhaustion.
        alignas(16) char s_SignalStack[64 * 1024];
        stack_t s_PreviousStack {};
        bool    s_HasAlternateStack = false;
#endif

        void WriteBytes(const char* data, size_t size) noexcept
        {
#ifdef _WIN32
            if (s_LogHandle != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                // Append atomically while retaining GENERIC_WRITE for FlushFileBuffers.
                OVERLAPPED append {};
                append.Offset = append.OffsetHigh = MAXDWORD;
                WriteFile(s_LogHandle, data, static_cast<DWORD>(size), &written, &append);
                FlushFileBuffers(s_LogHandle);
            }
            const auto stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
            DWORD      written      = 0;
            WriteFile(stderrHandle, data, static_cast<DWORD>(size), &written, nullptr);
#else
            for (int fd : {s_LogFd, STDERR_FILENO})
            {
                if (fd < 0)
                    continue;
                size_t offset = 0;
                while (offset < size)
                {
                    const auto written = write(fd, data + offset, size - offset);
                    if (written > 0)
                        offset += static_cast<size_t>(written);
                    else if (written < 0 && errno == EINTR)
                        continue;
                    else
                        break;
                }
            }
#endif
        }

#ifdef _WIN32
        std::vector<std::string> StackTrace(CONTEXT context)
        {
            const HANDLE process = GetCurrentProcess();
            STACKFRAME64 frame {};
            DWORD        machine = 0;
#if defined(_M_X64) || defined(__x86_64__)
            machine                = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset    = context.Rip;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86) || defined(__i386__)
            machine                = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset    = context.Eip;
            frame.AddrFrame.Offset = context.Ebp;
            frame.AddrStack.Offset = context.Esp;
#elif defined(_M_ARM64)
            machine                = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset    = context.Pc;
            frame.AddrFrame.Offset = context.Fp;
            frame.AddrStack.Offset = context.Sp;
#endif
            frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
            std::vector<std::string> frames;
            for (int i = 0; i < 64 && frame.AddrPC.Offset; ++i)
            {
                std::ostringstream text;
                text << "0x" << std::hex << frame.AddrPC.Offset;
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] {};
                auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen               = MAX_SYM_NAME;
                DWORD64 displacement             = 0;
                if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                    text << " " << symbol->Name << "+0x" << displacement;
                IMAGEHLP_LINE64 line {};
                line.SizeOfStruct      = sizeof(line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
                    text << " (" << line.FileName << ":" << std::dec << line.LineNumber << ")";
                frames.push_back(text.str());
                if (!StackWalk64(machine,
                                 process,
                                 GetCurrentThread(),
                                 &frame,
                                 &context,
                                 nullptr,
                                 SymFunctionTableAccess64,
                                 SymGetModuleBase64,
                                 nullptr))
                    break;
            }
            return frames;
        }

        std::string WriteDump(EXCEPTION_POINTERS* exception)
        {
            auto path = s_LogPath;
            if (path.empty())
                return "unavailable (no writable log directory)";
            path.replace_extension(".dmp");
            const auto file = CreateFileW(
                path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return path.u8string() + " (creation failed: " + std::to_string(GetLastError()) + ")";
            MINIDUMP_EXCEPTION_INFORMATION info {};
            info.ThreadId          = GetCurrentThreadId();
            info.ExceptionPointers = exception;
            info.ClientPointers    = FALSE;
            const BOOL result      = MiniDumpWriteDump(GetCurrentProcess(),
                                                       GetCurrentProcessId(),
                                                       file,
                                                       MiniDumpWithIndirectlyReferencedMemory,
                                                       &info,
                                                       nullptr,
                                                       nullptr);
            const auto error       = GetLastError();
            FlushFileBuffers(file);
            CloseHandle(file);
            if (!result)
            {
                DeleteFileW(path.c_str());
                return path.u8string() + " (write failed: " + std::to_string(error) + ")";
            }
            return path.u8string();
        }

        std::wstring Wide(const std::string& text)
        {
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(length, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }
#endif

        [[noreturn]] void Report(const std::string& type,
                                 const std::string& message
#ifdef _WIN32
                                 ,
                                 EXCEPTION_POINTERS* exception = nullptr
#endif
        )
        {
#ifdef _WIN32
            CONTEXT            context {};
            EXCEPTION_RECORD   record {};
            EXCEPTION_POINTERS captured {&record, &context};
            if (!exception)
            {
                RtlCaptureContext(&context);
                // Application-defined noncontinuable exception for C++/terminate reports.
                record.ExceptionCode  = 0xE0000001;
                record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                exception             = &captured;
            }
            const auto dump   = WriteDump(exception);
            const auto frames = StackTrace(*exception->ContextRecord);
#else
            const std::string        dump = "none (Linux; stack trace recorded in log)";
            void*                    addresses[64];
            const int                count   = backtrace(addresses, 64);
            char**                   symbols = backtrace_symbols(addresses, count);
            std::vector<std::string> frames;
            for (int i = 0; i < count; ++i)
            {
                if (symbols)
                    frames.emplace_back(symbols[i]);
                else
                {
                    std::ostringstream address;
                    address << addresses[i];
                    frames.push_back(address.str());
                }
            }
            std::free(symbols);
#endif
            const auto report = FormatReport(type, message, s_LogPath, dump, frames);
            // Append a critical record directly: a fatal failure can occur while a sink mutex is held.
            WriteBytes(report.data(), report.size());
#ifndef _WIN32
            if (s_LogFd >= 0)
                fsync(s_LogFd);
#endif
#ifdef _WIN32
            if (s_ShowDialog.load())
            {
                const auto text = Wide("Kans3D encountered a fatal error and must close.\n\n" + type + ": " + message +
                                       "\n\nLog: " + s_LogPath.u8string() + "\nDump: " + dump);
                MessageBoxW(nullptr, text.c_str(), L"Kans3D fatal error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
            }
#endif
            std::_Exit(EXIT_FAILURE);
        }

        [[noreturn]] void Terminate() noexcept
        {
            if (IsDebuggerAttached())
                std::abort();
            ReportException(std::current_exception(), "std::terminate");
        }

#ifdef _WIN32
        LONG WINAPI UnhandledException(EXCEPTION_POINTERS* exception)
        {
            if (IsDebuggerAttached())
                return EXCEPTION_CONTINUE_SEARCH;
            if (s_Reporting.test_and_set())
                return EXCEPTION_CONTINUE_SEARCH;
            try
            {
                std::ostringstream message;
                message << "Unhandled Windows exception 0x" << std::hex << exception->ExceptionRecord->ExceptionCode
                        << " at " << exception->ExceptionRecord->ExceptionAddress;
                Report("SEH", message.str(), exception);
            }
            catch (...)
            {
                constexpr char failure[] = "[critical] Kans3D: SEH crash; fatal reporting failed\n";
                WriteBytes(failure, sizeof(failure) - 1);
                std::_Exit(EXIT_FAILURE);
            }
        }
#else
        void Signal(int signal, siginfo_t*, void*) noexcept
        {
            if (IsDebuggerAttached())
            {
                struct sigaction action {};
                action.sa_handler = SIG_DFL;
                sigemptyset(&action.sa_mask);
                sigaction(signal, &action, nullptr);
                kill(getpid(), signal);
                return;
            }
            if (s_Reporting.test_and_set())
                _exit(128 + signal);
            for (size_t i = 0; i < 5; ++i)
                if (s_Signals[i] == signal)
                    WriteBytes(s_SignalReports[i], s_SignalReportSizes[i]);
            // Preloaded at Install. No streams, allocation, spdlog or application locks here.
            void*     addresses[64];
            const int count = backtrace(addresses, 64);
            if (s_LogFd >= 0)
            {
                backtrace_symbols_fd(addresses, count, s_LogFd);
                fsync(s_LogFd);
            }
            backtrace_symbols_fd(addresses, count, STDERR_FILENO);
            _exit(128 + signal);
        }
#endif
    } // namespace

    bool IsDebuggerAttached() noexcept
    {
#ifdef _WIN32
        return IsDebuggerPresent() != FALSE;
#else
        // Use only signal-safe I/O: this is also checked if a debugger attaches after startup.
        const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        char       buffer[4096];
        const auto size = read(fd, buffer, sizeof(buffer));
        close(fd);
        constexpr char field[] = "TracerPid:";
        for (ssize_t i = 0; i + static_cast<ssize_t>(sizeof(field)) < size; ++i)
        {
            size_t matched = 0;
            while (matched < sizeof(field) - 1 && buffer[i + matched] == field[matched])
                ++matched;
            if (matched != sizeof(field) - 1)
                continue;
            i += matched;
            while (i < size && (buffer[i] == ' ' || buffer[i] == '\t'))
                ++i;
            return i < size && buffer[i] >= '1' && buffer[i] <= '9';
        }
        return false;
#endif
    }

    std::string FormatReport(const std::string&              type,
                             const std::string&              message,
                             const std::filesystem::path&    logPath,
                             const std::string&              dumpPath,
                             const std::vector<std::string>& frames)
    {
        std::ostringstream report;
        report << "\n[critical] Kans3D fatal error\nType: " << type << "\nMessage: " << message
               << "\nLog: " << (logPath.empty() ? "unavailable" : logPath.u8string()) << "\nDump: " << dumpPath
               << "\nStack trace:\n";
        for (size_t i = 0; i < frames.size(); ++i)
            report << "  #" << i << " " << frames[i] << '\n';
        if (frames.empty())
            report << "  <stack unavailable>\n";
        return report.str();
    }

    void SetShowDialog(bool show) noexcept { s_ShowDialog.store(show); }

    void Install()
    {
        if (s_Installed || IsDebuggerAttached())
            return;
        s_LogPath = Log::GetFilePath();
#ifdef _WIN32
        if (!s_LogPath.empty())
            s_LogHandle = CreateFileW(s_LogPath.c_str(),
                                      GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        // Only the winning fatal reporter calls DbgHelp; its APIs are single-threaded.
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        s_PreviousFilter = SetUnhandledExceptionFilter(UnhandledException);
#else
        if (!s_LogPath.empty())
            s_LogFd = open(s_LogPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        void*     warmup[1];
        const int count  = backtrace(warmup, 1);
        const int nullFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullFd >= 0)
        {
            backtrace_symbols_fd(warmup, count, nullFd);
            close(nullFd);
        }
        constexpr const char* names[] = {"SIGSEGV", "SIGABRT", "SIGFPE", "SIGILL", "SIGBUS"};
        for (size_t i = 0; i < 5; ++i)
        {
            const auto report = FormatReport(names[i],
                                             "Fatal signal (no C++ exception)",
                                             s_LogPath,
                                             "none (Linux; stack trace recorded in log)",
                                             {});
            // Put the trace after the header; all string processing is done before installing handlers.
            const auto header      = report.substr(0, report.find("  <stack unavailable>"));
            s_SignalReportSizes[i] = std::min(header.size(), sizeof(s_SignalReports[i]));
            std::memcpy(s_SignalReports[i], header.data(), s_SignalReportSizes[i]);
        }
        stack_t stack {};
        stack.ss_sp         = s_SignalStack;
        stack.ss_size       = sizeof(s_SignalStack);
        s_HasAlternateStack = sigaltstack(&stack, &s_PreviousStack) == 0;
        struct sigaction action {};
        action.sa_sigaction = Signal;
        action.sa_flags     = SA_SIGINFO | SA_ONSTACK;
        sigfillset(&action.sa_mask);
        for (size_t i = 0; i < 5; ++i)
            sigaction(s_Signals[i], &action, &s_PreviousActions[i]);
#endif
        s_Reporting.clear();
        s_PreviousTerminate = std::set_terminate(Terminate);
        s_Installed         = true;
    }

    void Shutdown() noexcept
    {
        if (!s_Installed)
            return;
        std::set_terminate(s_PreviousTerminate);
#ifdef _WIN32
        SetUnhandledExceptionFilter(s_PreviousFilter);
        SymCleanup(GetCurrentProcess());
        if (s_LogHandle != INVALID_HANDLE_VALUE)
            CloseHandle(s_LogHandle);
        s_LogHandle = INVALID_HANDLE_VALUE;
#else
        for (size_t i = 0; i < 5; ++i)
            sigaction(s_Signals[i], &s_PreviousActions[i], nullptr);
        if (s_HasAlternateStack)
            sigaltstack(&s_PreviousStack, nullptr);
        if (s_LogFd >= 0)
            close(s_LogFd);
        s_LogFd = -1;
#endif
        s_Installed = false;
    }

    [[noreturn]] void ReportException(std::exception_ptr exception, const char* origin) noexcept
    {
        if (s_Reporting.test_and_set())
            std::_Exit(EXIT_FAILURE);
        try
        {
            const auto context = std::string(origin) + ": ";
            if (exception)
            {
                try
                {
                    std::rethrow_exception(exception);
                }
                catch (const std::exception& error)
                {
                    std::string type = typeid(error).name();
#ifndef _WIN32
                    int   status    = 0;
                    char* demangled = abi::__cxa_demangle(type.c_str(), nullptr, nullptr, &status);
                    if (demangled)
                        type = demangled;
                    std::free(demangled);
#endif
                    Report(type, context + error.what());
                }
                catch (...)
                {
                    Report("unknown exception", context + "Non-standard C++ exception (no what())");
                }
            }
            Report("std::terminate", "terminate called without an active exception");
        }
        catch (...)
        {
            constexpr char failure[] = "[critical] Kans3D: fatal reporting failed\n";
            WriteBytes(failure, sizeof(failure) - 1);
            std::_Exit(EXIT_FAILURE);
        }
    }
} // namespace Kans::FatalError
