#pragma once

#include <cstdint>
#include <string>

namespace PositionTracker
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

    bool HasPosition();
    Vec3 GetPosition();
    int GetConfidence();
    std::string GetStatusText();

    uintptr_t GetPlayerRoot();
    uintptr_t GetPlayerManager();
    uintptr_t GetPlayerContext();
    uint32_t GetObjectHandle();
    uint32_t GetDefinitionHandle();
    uint32_t GetPlayerSetupValue();
    uintptr_t GetRuntimeObject();
    uintptr_t GetTransformComponent();
}
