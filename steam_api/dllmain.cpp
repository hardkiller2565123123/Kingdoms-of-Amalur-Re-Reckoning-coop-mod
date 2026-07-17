#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <fstream>

static HMODULE g_realSteam = nullptr;
static HMODULE g_amalurCoop = nullptr;

static std::string GetProxyFolder()
{
    char path[MAX_PATH]{};
    HMODULE module = nullptr;

    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetProxyFolder),
        &module
    );

    GetModuleFileNameA(module, path, MAX_PATH);

    std::string fullPath = path;
    size_t slash = fullPath.find_last_of("\\/");

    return slash == std::string::npos ? "." : fullPath.substr(0, slash);
}

static std::string GetModFolder()
{
    return GetProxyFolder() + "\\AmalurCoop";
}

static std::string GetRealSteamPath()
{
    return GetModFolder() + "\\SteamAPI.dll";
}

static std::string GetAmalurCoopPath()
{
    return GetModFolder() + "\\AmalurCoop.dll";
}

static std::string GetLogPath()
{
    return GetModFolder() + "\\SteamProxy.log";
}

static void EnsureFolders()
{
    CreateDirectoryA(GetModFolder().c_str(), nullptr);
}

static void Log(const std::string& text)
{
    EnsureFolders();
    std::ofstream file(GetLogPath(), std::ios::app);
    file << text << "\n";
}

static FARPROC GetRealExport(const char* name)
{
    if (!g_realSteam)
    {
        Log(std::string("[SteamProxy] Real Steam DLL not loaded for export: ") + name);
        return nullptr;
    }

    FARPROC proc = GetProcAddress(g_realSteam, name);

    if (!proc)
        Log(std::string("[SteamProxy] Missing export: ") + name);

    return proc;
}

static void LoadRealSteam()
{
    std::string path = GetRealSteamPath();

    g_realSteam = LoadLibraryA(path.c_str());

    if (g_realSteam)
        Log("[SteamProxy] Loaded original Steam DLL: " + path);
    else
        Log("[SteamProxy] FAILED to load original Steam DLL: " + path);
}

static void LoadAmalurCoop()
{
    std::string path = GetAmalurCoopPath();

    g_amalurCoop = LoadLibraryA(path.c_str());

    if (g_amalurCoop)
        Log("[SteamProxy] Loaded AmalurCoop.dll: " + path);
    else
        Log("[SteamProxy] FAILED to load AmalurCoop.dll: " + path);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        EnsureFolders();
        Log("[SteamProxy] Attached");
        LoadRealSteam();
        LoadAmalurCoop();
        break;

    case DLL_PROCESS_DETACH:
        Log("[SteamProxy] Detached");

        if (g_amalurCoop)
        {
            FreeLibrary(g_amalurCoop);
            g_amalurCoop = nullptr;
        }

        if (g_realSteam)
        {
            FreeLibrary(g_realSteam);
            g_realSteam = nullptr;
        }

        break;
    }

    return TRUE;
}

extern "C" __declspec(dllexport) bool SteamAPI_Init()
{
    Log("[SteamProxy] SteamAPI_Init");

    using Fn = bool(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_Init"));

    return fn ? fn() : true;
}

extern "C" __declspec(dllexport) bool SteamAPI_InitSafe()
{
    Log("[SteamProxy] SteamAPI_InitSafe");

    using Fn = bool(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_InitSafe"));

    return fn ? fn() : SteamAPI_Init();
}

extern "C" __declspec(dllexport) void SteamAPI_Shutdown()
{
    Log("[SteamProxy] SteamAPI_Shutdown");

    using Fn = void(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_Shutdown"));

    if (fn)
        fn();
}

extern "C" __declspec(dllexport) void SteamAPI_RunCallbacks()
{
    using Fn = void(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_RunCallbacks"));

    if (fn)
        fn();
}

extern "C" __declspec(dllexport) void SteamAPI_ReleaseCurrentThreadMemory()
{
    using Fn = void(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_ReleaseCurrentThreadMemory"));

    if (fn)
        fn();
}

extern "C" __declspec(dllexport) bool SteamAPI_IsSteamRunning()
{
    using Fn = bool(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_IsSteamRunning"));

    return fn ? fn() : true;
}

extern "C" __declspec(dllexport) bool SteamAPI_RestartAppIfNecessary(unsigned int appId)
{
    using Fn = bool(*)(unsigned int);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_RestartAppIfNecessary"));

    return fn ? fn(appId) : false;
}

extern "C" __declspec(dllexport) int SteamAPI_GetHSteamUser()
{
    using Fn = int(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_GetHSteamUser"));

    return fn ? fn() : 1;
}

extern "C" __declspec(dllexport) int SteamAPI_GetHSteamPipe()
{
    using Fn = int(*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_GetHSteamPipe"));

    return fn ? fn() : 1;
}

extern "C" __declspec(dllexport) const char* SteamAPI_GetSteamInstallPath()
{
    using Fn = const char* (*)();
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_GetSteamInstallPath"));

    return fn ? fn() : "";
}

extern "C" __declspec(dllexport) void SteamAPI_RegisterCallback(void* callback, int callbackId)
{
    using Fn = void(*)(void*, int);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_RegisterCallback"));

    if (fn)
        fn(callback, callbackId);
}

extern "C" __declspec(dllexport) void SteamAPI_UnregisterCallback(void* callback)
{
    using Fn = void(*)(void*);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_UnregisterCallback"));

    if (fn)
        fn(callback);
}

extern "C" __declspec(dllexport) void SteamAPI_RegisterCallResult(void* callback, unsigned long long call)
{
    using Fn = void(*)(void*, unsigned long long);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_RegisterCallResult"));

    if (fn)
        fn(callback, call);
}

extern "C" __declspec(dllexport) void SteamAPI_UnregisterCallResult(void* callback, unsigned long long call)
{
    using Fn = void(*)(void*, unsigned long long);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamAPI_UnregisterCallResult"));

    if (fn)
        fn(callback, call);
}

extern "C" __declspec(dllexport) void* SteamInternal_ContextInit(void* contextInitData)
{
    Log("[SteamProxy] SteamInternal_ContextInit");

    using Fn = void* (*)(void*);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamInternal_ContextInit"));

    return fn ? fn(contextInitData) : nullptr;
}

extern "C" __declspec(dllexport) void* SteamInternal_CreateInterface(const char* version)
{
    Log("[SteamProxy] SteamInternal_CreateInterface");

    using Fn = void* (*)(const char*);
    Fn fn = reinterpret_cast<Fn>(GetRealExport("SteamInternal_CreateInterface"));

    return fn ? fn(version) : nullptr;
}

#define FORWARD_PTR(name)                                  \
extern "C" __declspec(dllexport) void* name()              \
{                                                          \
    using Fn = void*(*)();                                 \
    Fn fn = reinterpret_cast<Fn>(GetRealExport(#name));    \
    if (fn) return fn();                                   \
    Log("[SteamProxy] Dummy pointer returned for " #name); \
    return nullptr;                                        \
}

FORWARD_PTR(SteamUser)
FORWARD_PTR(SteamFriends)
FORWARD_PTR(SteamUtils)
FORWARD_PTR(SteamMatchmaking)
FORWARD_PTR(SteamUserStats)
FORWARD_PTR(SteamApps)
FORWARD_PTR(SteamNetworking)
FORWARD_PTR(SteamRemoteStorage)
FORWARD_PTR(SteamController)
FORWARD_PTR(SteamInput)
FORWARD_PTR(SteamHTTP)
FORWARD_PTR(SteamUGC)
FORWARD_PTR(SteamInventory)
FORWARD_PTR(SteamVideo)
FORWARD_PTR(SteamScreenshots)
FORWARD_PTR(SteamMusic)
FORWARD_PTR(SteamMusicRemote)
FORWARD_PTR(SteamHTMLSurface)