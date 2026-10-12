#include "MainMenuMultiplayer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include "Logger.h"
#include "MinHook.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

    GetProcAddressFn g_originalGetProcAddress = nullptr;
    std::atomic<bool> g_ready{ false };
    std::atomic<bool> g_sawHideGate{ false };
    std::atomic<bool> g_sawEnabledGate{ false };
    std::atomic<bool> g_sawEnabledNoPopupGate{ false };

    std::mutex g_hookMutex;
    std::unordered_set<void*> g_nativeGateTargets;

    constexpr const char* kShouldHideMultiplayer =
        "THQNO_Wrapper_ShouldHideMultiplayerMenu";
    constexpr const char* kIsMultiplayerEnabled =
        "THQNO_Wrapper_IsMultiplayerEnabled";
    constexpr const char* kIsMultiplayerEnabledNoPopup =
        "THQNO_Wrapper_IsMultiplayerEnabled_NoUIPopup";

    // These wrapper exports take no parameters in the frontend gate path. With
    // no arguments, __cdecl and __stdcall have identical stack behavior on x86.
    int __cdecl NativeShouldHideMultiplayerMenuFalse()
    {
        return 0;
    }

    int __cdecl NativeIsMultiplayerEnabledTrue()
    {
        return 1;
    }

    bool IsOrdinalProcName(LPCSTR procName)
    {
        return reinterpret_cast<ULONG_PTR>(procName) <= 0xFFFFu;
    }

    FARPROC ReplacementForName(LPCSTR procName)
    {
        if (!procName || IsOrdinalProcName(procName))
            return nullptr;

        if (std::strcmp(procName, kShouldHideMultiplayer) == 0)
        {
            g_sawHideGate.store(true, std::memory_order_release);
            return reinterpret_cast<FARPROC>(&NativeShouldHideMultiplayerMenuFalse);
        }

        if (std::strcmp(procName, kIsMultiplayerEnabled) == 0)
        {
            g_sawEnabledGate.store(true, std::memory_order_release);
            return reinterpret_cast<FARPROC>(&NativeIsMultiplayerEnabledTrue);
        }

        if (std::strcmp(procName, kIsMultiplayerEnabledNoPopup) == 0)
        {
            g_sawEnabledNoPopupGate.store(true, std::memory_order_release);
            return reinterpret_cast<FARPROC>(&NativeIsMultiplayerEnabledTrue);
        }

        return nullptr;
    }

    FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR procName)
    {
        if (const FARPROC replacement = ReplacementForName(procName))
            return replacement;

        return g_originalGetProcAddress
            ? g_originalGetProcAddress(module, procName)
            : nullptr;
    }

    bool InstallDirectGateHook(void* target, void* detour)
    {
        if (!target || !detour)
            return false;

        std::lock_guard<std::mutex> lock(g_hookMutex);

        if (g_nativeGateTargets.contains(target))
            return true;

        const MH_STATUS createStatus = MH_CreateHook(target, detour, nullptr);
        if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
            return false;

        const MH_STATUS enableStatus = MH_EnableHook(target);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
            return false;

        g_nativeGateTargets.insert(target);
        return true;
    }

    void HookAlreadyLoadedWrapperExports()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());

        if (snapshot == INVALID_HANDLE_VALUE)
            return;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                HMODULE module = entry.hModule;
                if (!module)
                    continue;

                FARPROC hide = ::GetProcAddress(module, kShouldHideMultiplayer);
                FARPROC enabled = ::GetProcAddress(module, kIsMultiplayerEnabled);
                FARPROC enabledNoPopup = ::GetProcAddress(module, kIsMultiplayerEnabledNoPopup);

                if (hide)
                {
                    InstallDirectGateHook(
                        reinterpret_cast<void*>(hide),
                        reinterpret_cast<void*>(&NativeShouldHideMultiplayerMenuFalse));
                    g_sawHideGate.store(true, std::memory_order_release);
                }

                if (enabled)
                {
                    InstallDirectGateHook(
                        reinterpret_cast<void*>(enabled),
                        reinterpret_cast<void*>(&NativeIsMultiplayerEnabledTrue));
                    g_sawEnabledGate.store(true, std::memory_order_release);
                }

                if (enabledNoPopup)
                {
                    InstallDirectGateHook(
                        reinterpret_cast<void*>(enabledNoPopup),
                        reinterpret_cast<void*>(&NativeIsMultiplayerEnabledTrue));
                    g_sawEnabledNoPopupGate.store(true, std::memory_order_release);
                }
            }
            while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
    }
}

namespace MainMenuMultiplayer
{
    bool Initialize()
    {
        if (g_ready.load(std::memory_order_acquire))
            return true;

        // Catch wrapper exports that may have been resolved before AmalurCoop
        // initialized. Any call through those existing pointers will now return
        // the frontend state that exposes/enables Multiplayer.
        HookAlreadyLoadedWrapperExports();

        // Catch delayed GetProcAddress resolutions too. We substitute only the
        // three exact THQNO multiplayer frontend gates and leave every other
        // exported function untouched.
        void* getProcAddressTarget = reinterpret_cast<void*>(
            ::GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));

        if (!getProcAddressTarget)
        {
            Logger::Write(
                Logger::Level::Error,
                "Native Multiplayer menu: GetProcAddress target was not found");
            return false;
        }

        MH_STATUS createStatus = MH_CreateHook(
            getProcAddressTarget,
            reinterpret_cast<void*>(&HookedGetProcAddress),
            reinterpret_cast<void**>(&g_originalGetProcAddress));

        if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        {
            Logger::Write(
                Logger::Level::Error,
                "Native Multiplayer menu: GetProcAddress hook creation failed (" +
                std::to_string(static_cast<int>(createStatus)) + ")");
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(getProcAddressTarget);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        {
            Logger::Write(
                Logger::Level::Error,
                "Native Multiplayer menu: GetProcAddress hook enable failed (" +
                std::to_string(static_cast<int>(enableStatus)) + ")");
            return false;
        }

        g_ready.store(true, std::memory_order_release);

        Logger::Write(
            Logger::Level::Success,
            "Native Multiplayer frontend gate installed (no ImGui menu; KOA owns rendering/input)");

        return true;
    }

    void Shutdown()
    {
        if (!g_ready.exchange(false, std::memory_order_acq_rel))
            return;

        void* getProcAddressTarget = reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));

        if (getProcAddressTarget)
        {
            MH_DisableHook(getProcAddressTarget);
            MH_RemoveHook(getProcAddressTarget);
        }

        std::lock_guard<std::mutex> lock(g_hookMutex);
        for (void* target : g_nativeGateTargets)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        g_nativeGateTargets.clear();

        g_originalGetProcAddress = nullptr;
    }

    bool IsReady()
    {
        return g_ready.load(std::memory_order_acquire);
    }
}
