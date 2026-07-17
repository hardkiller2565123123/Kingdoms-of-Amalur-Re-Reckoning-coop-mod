#pragma once

#include <Windows.h>

namespace GameState
{
    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    void Initialize();
    void Shutdown();
    void Update();

    void AutoDetectPlayerPosition();
    void ClearPlayerPositionAddress();
    void SetPlayerPositionAddress(uintptr_t address);
    uintptr_t GetPlayerPositionAddress();

    bool HasPlayerPosition();
    Vec3 GetPlayerPosition();

    void PrintState();
}
