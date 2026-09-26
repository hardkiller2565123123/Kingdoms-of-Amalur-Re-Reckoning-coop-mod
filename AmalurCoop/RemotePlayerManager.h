#pragma once

#include "GameState.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace RemotePlayerManager
{
    struct ProxyDiagnostics
    {
        std::uint32_t PlayerId = 0;
        std::uint32_t ObjectHandle = 0;
        std::uint32_t PersistentActorHandle = 0;
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t ActorStateHandle = 0;

        uintptr_t RuntimeObject = 0;
        uintptr_t TransformComponent = 0;
        uintptr_t ActorStateComponent = 0;
        uintptr_t AppearanceComponent = 0;
        uintptr_t ActorStateEntry = 0;

        bool Connected = false;
        bool PlayerPostSetupApplied = false;
        bool ActorStateResolved = false;
        bool Manual = false;
        std::string SpawnStatus;
    };

    void Initialize();
    void Shutdown();
    void Update();
    void PumpGameThread();

    void SpawnDummyNearLocalPlayer();
    void ClearDummy();
    bool HasDummy();

    bool HasVisibleProxy();
    std::size_t GetVisibleProxyCount();
    bool HasProxyForPlayer(std::uint32_t playerId);

    std::uint32_t GetProxyObjectHandle();
    uintptr_t GetProxyRuntimeObject();
    uintptr_t GetProxyTransformComponent();
    uintptr_t GetProxyActorStateComponent();
    uintptr_t GetProxyAppearanceComponent();

    std::uint32_t GetProxyObjectHandle(std::uint32_t playerId);
    uintptr_t GetProxyRuntimeObject(std::uint32_t playerId);
    uintptr_t GetProxyTransformComponent(std::uint32_t playerId);
    uintptr_t GetProxyActorStateComponent(std::uint32_t playerId);
    uintptr_t GetProxyAppearanceComponent(std::uint32_t playerId);
    std::uint32_t GetProxyActorStateHandle(std::uint32_t playerId);

    ProxyDiagnostics GetFirstProxyDiagnostics();
    ProxyDiagnostics GetProxyDiagnostics(std::uint32_t playerId);

    std::string GetStatusText();
    GameState::Vec3 GetDummyPosition();
    void PrintDummy();
}
