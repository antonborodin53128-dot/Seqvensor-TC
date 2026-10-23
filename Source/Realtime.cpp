#include "Realtime.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

namespace
{
    // declared here (not taken from the SDK headers) so that the build does not depend on the SDK version
    struct PowerThrottlingState
    {
        ULONG Version, ControlMask, StateMask;
    };
    constexpr int kProcessPowerThrottling = 4;   // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
    constexpr int kThreadPowerThrottling = 3;    // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
    constexpr ULONG kExecutionSpeed = 0x1, kIgnoreTimerResolution = 0x4;

    using SetProcessInfoFn = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    using SetThreadInfoFn = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    using AvSetFn = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
    using AvPrioFn = BOOL (WINAPI*) (HANDLE, int);

    FARPROC kernel (const char* name)
    {
        return GetProcAddress (GetModuleHandleW (L"kernel32.dll"), name);
    }
}

void tcrt::boostProcess()
{
    SetPriorityClass (GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    if (auto fn = (SetProcessInfoFn) kernel ("SetProcessInformation"))
    {
        PowerThrottlingState s { 1, kExecutionSpeed | kIgnoreTimerResolution, 0 };   // control these, state = off
        fn (GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof (s));
    }
}

void tcrt::boostAudioThread()
{
    thread_local bool done = false;
    if (done)
        return;
    done = true;

    if (auto fn = (SetThreadInfoFn) kernel ("SetThreadInformation"))
    {
        PowerThrottlingState s { 1, kExecutionSpeed, 0 };
        fn (GetCurrentThread(), kThreadPowerThrottling, &s, sizeof (s));
    }
    static HMODULE avrt = LoadLibraryW (L"avrt.dll");
    if (avrt != nullptr)
    {
        auto setChar = (AvSetFn) GetProcAddress (avrt, "AvSetMmThreadCharacteristicsW");
        auto setPrio = (AvPrioFn) GetProcAddress (avrt, "AvSetMmThreadPriority");
        DWORD task = 0;
        if (setChar != nullptr)
            if (HANDLE h = setChar (L"Pro Audio", &task))
                if (setPrio != nullptr)
                    setPrio (h, 1);   // AVRT_PRIORITY_HIGH
    }
}

std::int64_t tcrt::availableMemoryBytes()
{
    MEMORYSTATUSEX m {};
    m.dwLength = sizeof (m);
    if (GlobalMemoryStatusEx (&m))
        return (std::int64_t) m.ullAvailPhys;
    return 4LL * 1024 * 1024 * 1024;
}

#else
std::int64_t tcrt::availableMemoryBytes() { return 4LL * 1024 * 1024 * 1024; }
void tcrt::boostProcess() {}
void tcrt::boostAudioThread() {}
#endif
