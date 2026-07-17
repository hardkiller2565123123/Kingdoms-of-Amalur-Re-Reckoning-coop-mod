#pragma once

#include "GameState.h"

#include <cstdint>
#include <string>

namespace RemotePlayerManager
{
    void Initialize();
    void Shutdown();
    void Update();
    // Executes pending actor creation on the game render thread.
    void PumpGameThread();

    // Queues a safe existing inactive runtime record and uses it as the visible
    // remote-player proxy. This does not clone the local player object.
    void SpawnDummyNearLocalPlayer();
    void ClearDummy();
    bool HasDummy();

    bool HasVisibleProxy();
    std::uint32_t GetProxyObjectHandle();
    std::string GetStatusText();

    GameState::Vec3 GetDummyPosition();
    void PrintDummy();
}
