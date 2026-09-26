#include "RemotePlayerManager.h"

#include "EngineRuntime.h"
#include "Logger.h"
#include "MotionReplication.h"
#include "NetworkManager.h"
#include "PlayerProxySpawner.h"
#include "PositionTracker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
    constexpr uintptr_t kSetActorWorldPositionIda = 0x00F5DD50;
    constexpr std::uint64_t kRemoteTransformTimeoutMs = 3000;
    constexpr ULONGLONG kSpawnRetryMs = 5000;
    constexpr unsigned int kMaxAutomaticSpawnAttempts = 3;
    constexpr std::uint32_t kManualProxyPlayerId = 0xFFFFFFFEu;

    using SetActorWorldPositionFn = int(__thiscall*)(void*, const void*);

    struct EngineVec4
    {
        float X;
        float Y;
        float Z;
        float W;
    };

    struct Proxy
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

        GameState::Vec3 Target{};
        std::uint64_t LastSeenMs = 0;
        ULONGLONG LastSpawnAttempt = 0;
        unsigned int SpawnAttempts = 0;

        bool SpawnQueued = false;
        bool Connected = false;
        bool Manual = false;
        bool PlayerPostSetupApplied = false;
        bool ActorStateResolved = false;
        std::string SpawnStatus;
    };

    std::mutex g_mutex;
    std::map<std::uint32_t, Proxy> g_proxies;
    std::vector<std::uint32_t> g_spawnQueue;
    std::atomic<bool> g_spawnInProgress{ false };
    bool g_manualDummy = false;
    GameState::Vec3 g_dummyPosition{};
    std::string g_status = "Waiting for remote players";

    bool CallSetWorldPosition(uintptr_t transform, const EngineVec4& position)
    {
        const auto setPosition = reinterpret_cast<SetActorWorldPositionFn>(
            EngineRuntime::RuntimeAddress(kSetActorWorldPositionIda));
        if (!setPosition || !transform)
            return false;

        __try
        {
            setPosition(reinterpret_cast<void*>(transform), &position);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ClearProxyRuntime(Proxy& proxy)
    {
        proxy.ObjectHandle = 0;
        proxy.PersistentActorHandle = 0;
        proxy.DefinitionHandle = 0;
        proxy.ActorStateHandle = 0;
        proxy.RuntimeObject = 0;
        proxy.TransformComponent = 0;
        proxy.ActorStateComponent = 0;
        proxy.AppearanceComponent = 0;
        proxy.ActorStateEntry = 0;
        proxy.PlayerPostSetupApplied = false;
        proxy.ActorStateResolved = false;
        proxy.SpawnQueued = false;
    }

    bool SpawnProxy(std::uint32_t playerId)
    {
        const PlayerProxySpawner::SpawnResult spawned = PlayerProxySpawner::SpawnFromLocalPlayer();
        if (!spawned.Success)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_proxies.find(playerId);
            if (it != g_proxies.end())
            {
                it->second.SpawnQueued = false;
                it->second.SpawnStatus = spawned.Status;
            }
            g_status = "Player-style proxy spawn failed: " + spawned.Status;
            Logger::WriteFormat(
                Logger::Level::Error,
                "Remote player %u spawn failed: %s",
                playerId,
                spawned.Status.c_str());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto& proxy = g_proxies[playerId];
            proxy.PlayerId = playerId;
            proxy.ObjectHandle = spawned.ObjectHandle;
            proxy.PersistentActorHandle = spawned.PersistentActorHandle;
            proxy.DefinitionHandle = spawned.DefinitionHandle;
            proxy.ActorStateHandle = spawned.ActorStateHandle;
            proxy.RuntimeObject = spawned.RuntimeObject;
            proxy.TransformComponent = spawned.TransformComponent;
            proxy.ActorStateComponent = spawned.ActorStateComponent;
            proxy.AppearanceComponent = spawned.AppearanceComponent;
            proxy.ActorStateEntry = spawned.ActorStateEntry;
            proxy.PlayerPostSetupApplied = spawned.PlayerPostSetupApplied;
            proxy.ActorStateResolved = spawned.ActorStateResolved;
            proxy.SpawnStatus = spawned.Status;
            proxy.SpawnQueued = false;

            g_status = "Player-style remote proxy active: " + spawned.Status;
        }

        MotionReplication::InvalidateRemoteController(playerId);

        Logger::WriteFormat(
            Logger::Level::Success,
            "Remote player %u proxy active: handle=0x%08X object=0x%08X transform=0x%08X stateComp=0x%08X appearanceComp=0x%08X",
            playerId,
            spawned.ObjectHandle,
            static_cast<unsigned int>(spawned.RuntimeObject),
            static_cast<unsigned int>(spawned.TransformComponent),
            static_cast<unsigned int>(spawned.ActorStateComponent),
            static_cast<unsigned int>(spawned.AppearanceComponent));
        return true;
    }

    bool MoveProxy(std::uint32_t playerId, uintptr_t transform, const GameState::Vec3& target)
    {
        const EngineVec4 position{ target.X, target.Y, target.Z, 1.0f };
        if (CallSetWorldPosition(transform, position))
            return true;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_proxies.find(playerId);
            if (it != g_proxies.end())
            {
                ClearProxyRuntime(it->second);
                it->second.SpawnStatus = "Transform became invalid; respawn queued on next retry";
            }
        }

        MotionReplication::InvalidateRemoteController(playerId);
        Logger::WriteFormat(
            Logger::Level::Error,
            "Remote player %u proxy became invalid; respawn scheduled",
            playerId);
        return false;
    }

    std::uint32_t FirstProxyIdLocked()
    {
        for (const auto& pair : g_proxies)
        {
            if (pair.second.TransformComponent)
                return pair.first;
        }
        return 0;
    }

    RemotePlayerManager::ProxyDiagnostics BuildDiagnostics(const Proxy& proxy)
    {
        RemotePlayerManager::ProxyDiagnostics diagnostics{};
        diagnostics.PlayerId = proxy.PlayerId;
        diagnostics.ObjectHandle = proxy.ObjectHandle;
        diagnostics.PersistentActorHandle = proxy.PersistentActorHandle;
        diagnostics.DefinitionHandle = proxy.DefinitionHandle;
        diagnostics.ActorStateHandle = proxy.ActorStateHandle;
        diagnostics.RuntimeObject = proxy.RuntimeObject;
        diagnostics.TransformComponent = proxy.TransformComponent;
        diagnostics.ActorStateComponent = proxy.ActorStateComponent;
        diagnostics.AppearanceComponent = proxy.AppearanceComponent;
        diagnostics.ActorStateEntry = proxy.ActorStateEntry;
        diagnostics.Connected = proxy.Connected;
        diagnostics.PlayerPostSetupApplied = proxy.PlayerPostSetupApplied;
        diagnostics.ActorStateResolved = proxy.ActorStateResolved;
        diagnostics.Manual = proxy.Manual;
        diagnostics.SpawnStatus = proxy.SpawnStatus;
        return diagnostics;
    }
}

namespace RemotePlayerManager
{
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_proxies.clear();
        g_spawnQueue.clear();
        g_manualDummy = false;
        g_dummyPosition = {};
        g_status = "Waiting for remote players";
        Logger::Write(
            Logger::Level::Success,
            "RemotePlayerManager initialized with player-style FC6170 proxy spawning");
    }

    void Shutdown()
    {
        ClearDummy();
        Logger::Write("RemotePlayerManager shutdown");
    }

    void PumpGameThread()
    {
        MotionReplication::PumpRemoteMotionOnGameThread();

        if (g_spawnInProgress.exchange(true, std::memory_order_acq_rel))
            return;

        std::uint32_t playerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_spawnQueue.empty())
            {
                playerId = g_spawnQueue.front();
                g_spawnQueue.erase(g_spawnQueue.begin());
            }
        }

        if (playerId)
            SpawnProxy(playerId);

        g_spawnInProgress.store(false, std::memory_order_release);
    }

    void Update()
    {
        const auto peers = NetworkManager::GetPeers();
        const ULONGLONG now = GetTickCount64();
        std::set<std::uint32_t> activeIds;

        struct Move
        {
            std::uint32_t PlayerId;
            uintptr_t Transform;
            GameState::Vec3 Target;
        };
        std::vector<Move> moves;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            for (const auto& peer : peers)
            {
                if (!peer.PlayerId || !peer.Connected || !peer.HasTransform ||
                    now - peer.LastSeenMs > kRemoteTransformTimeoutMs)
                {
                    continue;
                }

                activeIds.insert(peer.PlayerId);
                auto& proxy = g_proxies[peer.PlayerId];
                proxy.PlayerId = peer.PlayerId;
                proxy.Connected = true;
                proxy.Manual = false;
                proxy.LastSeenMs = peer.LastSeenMs;
                proxy.Target = { peer.Position.X, peer.Position.Y, peer.Position.Z };

                if (!proxy.TransformComponent && !proxy.SpawnQueued &&
                    proxy.SpawnAttempts < kMaxAutomaticSpawnAttempts &&
                    now - proxy.LastSpawnAttempt >= kSpawnRetryMs)
                {
                    proxy.LastSpawnAttempt = now;
                    ++proxy.SpawnAttempts;
                    proxy.SpawnQueued = true;
                    proxy.SpawnStatus = "Queued player-style proxy spawn (attempt " +
                        std::to_string(proxy.SpawnAttempts) + "/" +
                        std::to_string(kMaxAutomaticSpawnAttempts) + ")";
                    g_spawnQueue.push_back(peer.PlayerId);
                }
                else if (!proxy.TransformComponent && !proxy.SpawnQueued &&
                    proxy.SpawnAttempts >= kMaxAutomaticSpawnAttempts)
                {
                    proxy.SpawnStatus = "Automatic proxy spawning paused after repeated failures";
                }

                if (proxy.TransformComponent)
                    moves.push_back({ peer.PlayerId, proxy.TransformComponent, proxy.Target });
            }

            if (g_manualDummy)
            {
                activeIds.insert(kManualProxyPlayerId);
                auto& proxy = g_proxies[kManualProxyPlayerId];
                proxy.PlayerId = kManualProxyPlayerId;
                proxy.Connected = true;
                proxy.Manual = true;
                proxy.LastSeenMs = now;
                proxy.Target = g_dummyPosition;

                if (proxy.TransformComponent)
                    moves.push_back({ kManualProxyPlayerId, proxy.TransformComponent, proxy.Target });
            }

            for (auto& pair : g_proxies)
            {
                if (!activeIds.count(pair.first))
                    pair.second.Connected = false;
            }

            std::size_t visible = 0;
            for (const auto& pair : g_proxies)
            {
                if (pair.second.Connected && pair.second.TransformComponent)
                    ++visible;
            }

            if (activeIds.empty())
            {
                g_status = "Waiting for remote players";
            }
            else
            {
                g_status = "Remote targets: " + std::to_string(activeIds.size()) +
                    ", player-style proxies: " + std::to_string(visible);
            }
        }

        for (const auto& move : moves)
            MoveProxy(move.PlayerId, move.Transform, move.Target);
    }

    void SpawnDummyNearLocalPlayer()
    {
        if (!PositionTracker::HasPosition())
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Manual proxy requires an active local player";
            return;
        }

        const auto local = PositionTracker::GetPosition();
        const ULONGLONG now = GetTickCount64();

        std::lock_guard<std::mutex> lock(g_mutex);
        g_manualDummy = true;
        g_dummyPosition = { local.X + 150.0f, local.Y, local.Z + 150.0f };

        auto& proxy = g_proxies[kManualProxyPlayerId];
        proxy.PlayerId = kManualProxyPlayerId;
        proxy.Connected = true;
        proxy.Manual = true;
        proxy.Target = g_dummyPosition;
        proxy.LastSeenMs = now;

        if (!proxy.TransformComponent && !proxy.SpawnQueued)
        {
            proxy.LastSpawnAttempt = now;
            proxy.SpawnAttempts = 1;
            proxy.SpawnQueued = true;
            proxy.SpawnStatus = "Queued manual player-style proxy spawn";
            g_spawnQueue.push_back(kManualProxyPlayerId);
        }

        g_status = "Manual player-style proxy queued near local player";
    }

    void ClearDummy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_manualDummy = false;
        g_dummyPosition = {};
        g_spawnQueue.clear();
        g_proxies.clear();
        g_status = "Proxy tracking cleared (spawned actors remain game-managed)";
    }

    bool HasDummy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_manualDummy || !g_proxies.empty();
    }

    bool HasVisibleProxy()
    {
        return GetVisibleProxyCount() != 0;
    }

    std::size_t GetVisibleProxyCount()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::size_t count = 0;
        for (const auto& pair : g_proxies)
        {
            if (pair.second.TransformComponent)
                ++count;
        }
        return count;
    }

    bool HasProxyForPlayer(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it != g_proxies.end() && it->second.TransformComponent != 0;
    }

    std::uint32_t GetProxyObjectHandle()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? g_proxies[id].ObjectHandle : 0;
    }

    uintptr_t GetProxyRuntimeObject()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? g_proxies[id].RuntimeObject : 0;
    }

    uintptr_t GetProxyTransformComponent()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? g_proxies[id].TransformComponent : 0;
    }

    uintptr_t GetProxyActorStateComponent()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? g_proxies[id].ActorStateComponent : 0;
    }

    uintptr_t GetProxyAppearanceComponent()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? g_proxies[id].AppearanceComponent : 0;
    }

    std::uint32_t GetProxyObjectHandle(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.ObjectHandle;
    }

    uintptr_t GetProxyRuntimeObject(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.RuntimeObject;
    }

    uintptr_t GetProxyTransformComponent(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.TransformComponent;
    }

    uintptr_t GetProxyActorStateComponent(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.ActorStateComponent;
    }

    uintptr_t GetProxyAppearanceComponent(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.AppearanceComponent;
    }

    std::uint32_t GetProxyActorStateHandle(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? 0 : it->second.ActorStateHandle;
    }

    ProxyDiagnostics GetFirstProxyDiagnostics()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto id = FirstProxyIdLocked();
        return id ? BuildDiagnostics(g_proxies[id]) : ProxyDiagnostics{};
    }

    ProxyDiagnostics GetProxyDiagnostics(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        return it == g_proxies.end() ? ProxyDiagnostics{} : BuildDiagnostics(it->second);
    }

    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    GameState::Vec3 GetDummyPosition()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_dummyPosition;
    }

    void PrintDummy()
    {
        std::cout << "[Remote Players] " << GetStatusText() << "\n";
    }
}
