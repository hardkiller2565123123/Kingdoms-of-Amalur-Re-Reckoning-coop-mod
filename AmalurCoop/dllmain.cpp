#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "AmalurCoop.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        AmalurCoop::Start(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Do not wait or run complex cleanup while the Windows loader lock is held.
        // The worker thread performs orderly cleanup when the DLL is explicitly unloaded;
        // normal process termination releases remaining resources automatically.
        AmalurCoop::Stop();
    }

    return TRUE;
}
