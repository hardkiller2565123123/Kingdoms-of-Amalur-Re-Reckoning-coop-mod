#include "DInputProxy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <dinput.h>

#include <atomic>
#include <mutex>
#include <string>

namespace
{
    using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    using DllCanUnloadNowFn = HRESULT (WINAPI*)();
    using DllGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
    using DllRegisterServerFn = HRESULT (WINAPI*)();
    using DllUnregisterServerFn = HRESULT (WINAPI*)();

    std::once_flag g_loadOnce;
    HMODULE g_realModule = nullptr;

    DirectInput8CreateFn g_directInput8Create = nullptr;
    DllCanUnloadNowFn g_dllCanUnloadNow = nullptr;
    DllGetClassObjectFn g_dllGetClassObject = nullptr;
    DllRegisterServerFn g_dllRegisterServer = nullptr;
    DllUnregisterServerFn g_dllUnregisterServer = nullptr;

    std::atomic<bool> g_ready{ false };

    void DebugTrace(const char* message)
    {
        if (message)
            OutputDebugStringA(message);
    }

    void LoadRealDInput8()
    {
        wchar_t systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);

        if (length == 0 || length >= MAX_PATH)
        {
            DebugTrace("[AmalurCoop] GetSystemDirectoryW failed for dinput8.dll\n");
            return;
        }

        std::wstring path(systemDirectory, length);
        path += L"\\dinput8.dll";

        g_realModule = LoadLibraryW(path.c_str());
        if (!g_realModule)
        {
            DebugTrace("[AmalurCoop] Failed to load system dinput8.dll\n");
            return;
        }

        g_directInput8Create = reinterpret_cast<DirectInput8CreateFn>(
            GetProcAddress(g_realModule, "DirectInput8Create"));
        g_dllCanUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(
            GetProcAddress(g_realModule, "DllCanUnloadNow"));
        g_dllGetClassObject = reinterpret_cast<DllGetClassObjectFn>(
            GetProcAddress(g_realModule, "DllGetClassObject"));
        g_dllRegisterServer = reinterpret_cast<DllRegisterServerFn>(
            GetProcAddress(g_realModule, "DllRegisterServer"));
        g_dllUnregisterServer = reinterpret_cast<DllUnregisterServerFn>(
            GetProcAddress(g_realModule, "DllUnregisterServer"));

        g_ready.store(g_directInput8Create != nullptr, std::memory_order_release);

        if (g_ready.load(std::memory_order_acquire))
            DebugTrace("[AmalurCoop] System dinput8.dll loaded and forwarding\n");
        else
            DebugTrace("[AmalurCoop] DirectInput8Create export is missing\n");
    }

    void EnsureLoaded()
    {
        std::call_once(g_loadOnce, LoadRealDInput8);
    }
}

namespace DInputProxy
{
    bool Initialize()
    {
        EnsureLoaded();
        return g_ready.load(std::memory_order_acquire);
    }

    bool IsReady()
    {
        EnsureLoaded();
        return g_ready.load(std::memory_order_acquire);
    }

    void Shutdown()
    {
        // Keep the real DLL loaded until process termination. Unloading it while
        // DirectInput COM objects still exist can leave stale vtables in the game.
    }
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    REFIID interfaceId,
    LPVOID* output,
    LPUNKNOWN outer)
{
    EnsureLoaded();

    if (output)
        *output = nullptr;

    if (!g_directInput8Create)
        return DIERR_NOTINITIALIZED;

    return g_directInput8Create(instance, version, interfaceId, output, outer);
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    EnsureLoaded();
    return g_dllCanUnloadNow ? g_dllCanUnloadNow() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(
    REFCLSID classId,
    REFIID interfaceId,
    LPVOID* output)
{
    EnsureLoaded();

    if (output)
        *output = nullptr;

    return g_dllGetClassObject
        ? g_dllGetClassObject(classId, interfaceId, output)
        : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    EnsureLoaded();
    return g_dllRegisterServer ? g_dllRegisterServer() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    EnsureLoaded();
    return g_dllUnregisterServer ? g_dllUnregisterServer() : E_NOTIMPL;
}
