#pragma once

#include <cstdint>
#include <string>

namespace PlayerLifecycleSync
{
    bool Initialize();
    void Shutdown();

    // Must be called from the game/render thread. Processes at most one remote
    // player per call so expensive native rebuilds never pile up in one frame.
    void PumpGameThread();

    void NotifyProxySpawn(std::uint32_t playerId);
    void NotifyProxyRemoved(std::uint32_t playerId);
    void RequestFullRefresh();

    bool IsReady();
    std::string GetStatusText();
}
