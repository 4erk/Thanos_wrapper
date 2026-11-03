//#include "pch.h"
#include "PathUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

namespace thanosray::io {

bool BuildSiblingPath(wchar_t* outPath, size_t outPathCount, const wchar_t* siblingFileName)
{
    if (!outPath || outPathCount == 0) return false;
    outPath[0] = 0;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&BuildSiblingPath, &self)) {
        return false;
    }

    wchar_t dllPath[MAX_PATH] = {0};
    if (!GetModuleFileNameW(self, dllPath, MAX_PATH)) {
        return false;
    }

    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (!lastSlash) return false;
    *(lastSlash + 1) = 0;

    if (wcslen(dllPath) + wcslen(siblingFileName) + 1 >= outPathCount) return false;
    wcscpy_s(outPath, outPathCount, dllPath);
    wcscat_s(outPath, outPathCount, siblingFileName);
    return true;
}

} // namespace thanosray::io
