#pragma once

#include <cstdint>
#include <string>

namespace MotionReplication
{
    struct Stats
    {
        bool HookInstalled = false;
        std::uint32_t LastHash = 0;
        std::uint32_t LastSequence = 0;
        std::uint64_t SentPackets = 0;
        std::uint64_t ReceivedPackets = 0;
        std::uint64_t DroppedPackets = 0;
        std::uint64_t ReplayCount = 0;

        int ControllerComponentIndex = -1;
        int LocalActorStateIndex = -1;
        int RemoteActorStateIndex = -1;

        uintptr_t LocalActorStateEntry = 0;
        uintptr_t RemoteActorStateEntry = 0;
        uintptr_t RemoteActorStateComponent = 0;
        std::uint32_t RemoteActorStateHandle = 0;

        uintptr_t LocalController = 0;
        uintptr_t LastRemoteController = 0;
        uintptr_t LocalControllerQueue = 0;
        uintptr_t RemoteControllerQueue = 0;

        std::uint32_t LocalControllerA8 = 0;
        std::uint32_t LocalControllerAC = 0;
        std::uint32_t LocalControllerB4 = 0;
        std::uint32_t RemoteControllerA8 = 0;
        std::uint32_t RemoteControllerAC = 0;
        std::uint32_t RemoteControllerB4 = 0;

        std::string Status;
    };

    void Initialize();
    void Shutdown();

    // Called from the render thread before ImGui rendering. Engine replay is
    // kept off the UDP/update worker so motion requests enter through the same
    // thread as the verified render-thread actor operations.
    void PumpRemoteMotionOnGameThread();

    // A newly spawned proxy must resolve a fresh actor-state/controller graph.
    void InvalidateRemoteController(std::uint32_t actorNetId);

    Stats GetStats();
}
