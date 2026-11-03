//#include "pch.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <share.h>

namespace thanosray::log {

//static FILE* g_file = nullptr;
//static LONG  g_lines = 0;
//static const LONG kMaxLines = 8000;
//static char g_fileName[MAX_PATH] = "vkproxy.log";

static FILE* g_file = nullptr;
static LONG  g_lines = 0;
static const LONG kMaxLines = 64000;
static std::mutex g_fileMtx;

static char g_fileName[MAX_PATH] = { 0 };

static void EnsureOpen()
{
    if (g_file) return;

    // Build a per-process logfile name next to our DLL to avoid overwrite / cwd confusion.
    if (g_fileName[0] == '\0') {
        char dllPath[MAX_PATH] = {};
        HMODULE hm = nullptr;

        // Get the module that contains this function (Log.cpp), i.e. your vulkan-1.dll proxy
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&EnsureOpen, &hm))
        {
            GetModuleFileNameA(hm, dllPath, MAX_PATH);
        }

        // Strip filename -> directory
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        DWORD pid = GetCurrentProcessId();

        // vkproxy_<pid>.log in the same folder as the DLL
        sprintf_s(g_fileName, "%svkproxy_%lu.log", dllPath[0] ? dllPath : "", (unsigned long)pid);
    }

    g_file = _fsopen(g_fileName, "w", _SH_DENYNO);
    if (!g_file) return;

    // Unbuffered: keep lines even if the game crashes.
    setvbuf(g_file, nullptr, _IONBF, 0);

    // First line tells you exactly which process this is.
    fprintf(g_file, "=== vkproxy init pid=%lu ===\n", (unsigned long)GetCurrentProcessId());
}


void SetLogFileName(const char* fileName)
{
    if (!fileName || !*fileName) return;
    if (g_file) return; // too late (already opened)

    strncpy_s(g_fileName, fileName, sizeof(g_fileName) - 1);
}

void Shutdown()
{
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void Logf(const char* fmt, ...)
{
    // Prevent log explosion.
    if (InterlockedIncrement(&g_lines) > kMaxLines) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_fileMtx);
    EnsureOpen();
    if (!g_file) return;

    va_list va;
    va_start(va, fmt);
    vfprintf(g_file, fmt, va);
    va_end(va);
    fputc('\n', g_file);
}

static std::mutex g_onceMtx;
static std::unordered_set<uint64_t> g_onceKeys;

static uint64_t HashKey64(const char* s)
{
    // FNV-1a 64-bit
    uint64_t h = 1469598103934665603ull;
    if (!s) return h;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 1099511628211ull;
    }
    return h;
}

bool LogOncef(const char* uniqueKey, const char* fmt, ...)
{
    const uint64_t hk = HashKey64(uniqueKey ? uniqueKey : "");
    {
        std::lock_guard<std::mutex> lock(g_onceMtx);
        if (g_onceKeys.find(hk) != g_onceKeys.end()) {
            return false;
        }
        g_onceKeys.insert(hk);
    }

    if (InterlockedIncrement(&g_lines) > kMaxLines) return false;
    std::lock_guard<std::mutex> lock(g_fileMtx);
    EnsureOpen();
    if (!g_file) return false;

    va_list va;
    va_start(va, fmt);
    vfprintf(g_file, fmt, va);
    va_end(va);
    fputc('\n', g_file);
    return true;
}

} // namespace thanosray::log
