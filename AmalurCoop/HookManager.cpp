#include "HookManager.h"

#include "Logger.h"
#include "MinHook.h"

#include <atomic>
#include <string>

namespace
{
    std::atomic<bool> g_ready{ false };
}

namespace HookManager
{
    void Initialize()
    {
        if (g_ready.load(std::memory_order_acquire))
            return;

        Logger::Write("HookManager initializing (renderer hooks only)");

        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            Logger::Write("MinHook initialize failed: " + std::to_string(static_cast<int>(status)));
            return;
        }

        // All guessed gameplay hooks were intentionally removed. MinHook is
        // retained only for the verified ImGui renderer hooks installed by
        // ImGuiOverlay.
        g_ready.store(true, std::memory_order_release);
        Logger::Write(Logger::Level::Success,
            "HookManager ready; old broadcast/gameplay detours are disabled");
    }

    void Shutdown()
    {
        if (!g_ready.exchange(false, std::memory_order_acq_rel))
            return;

        MH_Uninitialize();
        Logger::Write("HookManager shutdown");
    }

    bool IsReady()
    {
        return g_ready.load(std::memory_order_acquire);
    }
}
