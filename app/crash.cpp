// crash.cpp -- records crashes in %APPDATA%\Ultima III Assistant\crashes.
//
// Addresses are logged as module+offset. For "Ultima III Assistant.exe", look the
// offset up in "Ultima III Assistant.map" (written by the build) after adding
// the image base, 0x140000000.
#include "crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace crash {
namespace {

wchar_t g_dir[MAX_PATH];  // worked out up front: the handler shouldn't allocate
LPTOP_LEVEL_EXCEPTION_FILTER g_previous;

void Append(HANDLE file, const char* fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf) - 1) n = sizeof buf - 1;
    DWORD written;
    WriteFile(file, buf, static_cast<DWORD>(n), &written, nullptr);
}

// "name.dll+0x1234" for an address, or the bare address outside any module.
void Describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
            if (*p == L'\\' || *p == L'/') name = p + 1;
        char narrow[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr);
        snprintf(out, size, "%s+0x%llX", narrow,
                 static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
    }
}

void WriteStack(HANDLE file, CONTEXT context) {
#if defined(__x86_64__) || defined(_M_X64)
    for (int frame = 0; frame < 40 && context.Rip; ++frame) {
        char where[MAX_PATH + 32];
        Describe(context.Rip, where, sizeof where);
        Append(file, "  #%02d %s\r\n", frame, where);

        DWORD64 base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (function) {
            PVOID handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handlerData, &establisher,
                             nullptr);
        } else {  // a leaf function: the return address is on top of the stack
            if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64))) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
    }
#else
    (void)context;
    Append(file, "  (stack trace only on x64)\r\n");
#endif
}

// Logs what happened and writes a minidump; `info` is null for std::terminate.
void Record(EXCEPTION_POINTERS* info, const char* what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    wchar_t logPath[MAX_PATH], dumpPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%ls\\crash.log", g_dir);
    swprintf(dumpPath, MAX_PATH, L"%ls\\crash-%ls.dmp", g_dir, stamp);

    HANDLE log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        Append(log, "=== %04u-%02u-%02u %02u:%02u:%02u  thread %lu  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, GetCurrentThreadId(), what);
        if (info) {
            const EXCEPTION_RECORD& record = *info->ExceptionRecord;
            char where[MAX_PATH + 32];
            Describe(reinterpret_cast<DWORD64>(record.ExceptionAddress), where, sizeof where);
            Append(log, "Exception 0x%08lX at %s\r\n", record.ExceptionCode, where);
            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
                Append(log, "Access violation %s 0x%llX\r\n",
                       record.ExceptionInformation[0] == 0 ? "reading" : record.ExceptionInformation[0] == 1
                                                                             ? "writing"
                                                                             : "executing",
                       static_cast<unsigned long long>(record.ExceptionInformation[1]));
        }
        FlushFileBuffers(log);  // in case the stack walk itself goes wrong
    }

    HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          info ? &exception : nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }

    if (log != INVALID_HANDLE_VALUE) {
        Append(log, "Minidump: crash-%ls.dmp\r\n", stamp);
        if (info) {
            Append(log, "Stack:\r\n");
            WriteStack(log, *info->ContextRecord);
        }
        Append(log, "\r\n");
        CloseHandle(log);
    }
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) == 0) Record(info, "unhandled exception");
    return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void OnTerminate() {
    Record(nullptr, "std::terminate (uncaught C++ exception)");
    std::abort();
}

}  // namespace

void Install() {
    wchar_t appData[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n && n < MAX_PATH) {
        swprintf(g_dir, MAX_PATH, L"%ls\\Ultima III Assistant", appData);
        CreateDirectoryW(g_dir, nullptr);
        wcsncat(g_dir, L"\\crashes", MAX_PATH - wcslen(g_dir) - 1);
    } else {
        lstrcpynW(g_dir, L".", MAX_PATH);
    }
    CreateDirectoryW(g_dir, nullptr);

    g_previous = SetUnhandledExceptionFilter(OnUnhandled);
    std::set_terminate(OnTerminate);
}

}  // namespace crash
