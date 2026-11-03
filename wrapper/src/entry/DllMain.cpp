//#include "pch.h"

#include "../vk/VkProxy.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../log/Log.h"
using namespace thanosray::log;

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    static bool once = false;
    if (!once) { once = true; Logf("[BUILD-ID] ..."); }



    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        thanosray::vk::Initialize();
    } else if (reason == DLL_PROCESS_DETACH) {
        thanosray::vk::Shutdown();
    }
    return TRUE;
}


