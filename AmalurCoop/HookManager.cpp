#include "HookManager.h"

#include "Logger.h"
#include "MinHook.h"
#include "MotionReplication.h"

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

        Logger::Write("HookManager initializing");

        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        {
            Logger::Write("MinHook initialize failed: " + std::to_string(static_cast<int>(status)));
            return;
        }

        MotionReplication::Initialize();

        g_ready.store(true, std::memory_order_release);
        Logger::Write(Logger::Level::Success,
            "HookManager ready");
    }

    void Shutdown()
    {
        if (!g_ready.exchange(false, std::memory_order_acq_rel))
            return;

        MotionReplication::Shutdown();
        MH_Uninitialize();
        Logger::Write("HookManager shutdown");
    }

    bool IsReady()
    {
        return g_ready.load(std::memory_order_acquire);
    }
}
