#include "RemotePlayerManager.h"

#include "EngineRuntime.h"
#include "NativeVisualCalls.h"
#include <cmath>
#include "Logger.h"
#include "MotionReplication.h"
#include "NetworkManager.h"
#include "PlayerAppearanceRuntime.h"
#include "PlayerLifecycleSync.h"
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
    constexpr ULONGLONG kWorldStableDelayMs = 1200;
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
        std::uint32_t GamePlayerIndex = 0;
        std::uint32_t ObjectHandle = 0;
        std::uint32_t PersistentActorHandle = 0;
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t ActorStateHandle = 0;

        uintptr_t PlayerContext = 0;
        uintptr_t RuntimeObject = 0;
        uintptr_t TransformComponent = 0;
        uintptr_t ActorStateComponent = 0;
        uintptr_t EquipmentComponent = 0;
        uintptr_t AppearanceComponent = 0;
        uintptr_t RigSwapComponent = 0;
        uintptr_t ActorStateEntry = 0;

        NetworkManager::PlayerSnapshot Snapshot{};
        bool HasSnapshot = false;

        GameState::Vec3 Target{};
        std::uint64_t LastSeenMs = 0;
        ULONGLONG LastSpawnAttempt = 0;
        ULONGLONG LastAppearanceRefresh = 0;
        unsigned int SpawnAttempts = 0;
        unsigned int AppearanceRefreshPasses = 0;

        bool SpawnQueued = false;
        bool Connected = false;
        bool Manual = false;
        bool PlayerPostSetupApplied = false;
        bool PlayerContextFound = false;
        bool PlayerContextBound = false;
        bool PlayerContextActivated = false;
        bool AdditionalPlayerStateApplied = false;
        bool ComponentCloneApplied = false;
        bool ComponentFinalizeApplied = false;
        bool LateActorFinalizeApplied = false;
        bool AppearanceRefreshApplied = false;
        bool ActorStateResolved = false;
        std::string SpawnStatus;
    };

    std::mutex g_mutex;
    std::map<std::uint32_t, Proxy> g_proxies;
    std::vector<std::uint32_t> g_spawnQueue;
    std::atomic<bool> g_spawnInProgress{ false };
    std::atomic<bool> g_worldReady{ false };
    uintptr_t g_worldLocalRuntime = 0;
    std::uint32_t g_worldLocalHandle = 0;
    ULONGLONG g_worldStableSince = 0;
    std::uint32_t g_arrivedJoin = 0; // render thread; once per explicit Join
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

    bool ApplyRotation(uintptr_t transform, const float* rotation)
    {
        if (!EngineRuntime::IsActiveComponent(transform)) return false;
        __try
        {
            // PartLocation +34 is the quaternion used by F4F6A0 and F3BCF0.
            // Recompute its derived matrix/basis and dirty PartRendering.
            for (unsigned i = 0; i < 4; ++i)
                *reinterpret_cast<float*>(transform + 0x34 + i * 4) = rotation[i];
            using Rebuild = void(__thiscall*)(void*);
            reinterpret_cast<Rebuild>(EngineRuntime::RuntimeAddress(0x00F3BCF0))(
                reinterpret_cast<void*>(transform));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void RefreshRenderTransform(uintptr_t rendering)
    {
        if (!EngineRuntime::IsActiveComponent(rendering)) return;
        __try
        {
            // Publish the corrected C6 transform into the render tree now,
            // including frames when the ordinary component scheduler is paused.
            NativeVisualCalls::UpdateWorldTransform(EngineRuntime::RuntimeAddress(0x00F3DBD0),
                reinterpret_cast<void*>(rendering));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void ClearProxyRuntime(Proxy& proxy)
    {
        proxy.ObjectHandle = 0;
        proxy.PersistentActorHandle = 0;
        proxy.DefinitionHandle = 0;
        proxy.ActorStateHandle = 0;
        proxy.PlayerContext = 0;
        proxy.RuntimeObject = 0;
        proxy.TransformComponent = 0;
        proxy.ActorStateComponent = 0;
        proxy.EquipmentComponent = 0;
        proxy.AppearanceComponent = 0;
        proxy.RigSwapComponent = 0;
        proxy.ActorStateEntry = 0;
        proxy.PlayerPostSetupApplied = false;
        proxy.PlayerContextFound = false;
        proxy.PlayerContextBound = false;
        proxy.PlayerContextActivated = false;
        proxy.AdditionalPlayerStateApplied = false;
        proxy.ComponentCloneApplied = false;
        proxy.ComponentFinalizeApplied = false;
        proxy.LateActorFinalizeApplied = false;
        proxy.AppearanceRefreshApplied = false;
        proxy.ActorStateResolved = false;
        proxy.LastAppearanceRefresh = 0;
        proxy.AppearanceRefreshPasses = 0;
        proxy.SpawnQueued = false;
    }

    void ResetProxyForWorldTransition(Proxy& proxy)
    {
        ClearProxyRuntime(proxy);
        proxy.SpawnAttempts = 0;
        proxy.LastSpawnAttempt = 0;
        proxy.SpawnStatus = "Waiting for the local world to finish loading";
    }

    struct LocalWorldIdentity
    {
        bool Valid = false;
        std::uint32_t Handle = 0;
        uintptr_t Runtime = 0;
        uintptr_t Transform = 0;
    };

    LocalWorldIdentity CaptureLocalWorldIdentity()
    {
        LocalWorldIdentity identity{};
        if (!PositionTracker::HasPosition() || PositionTracker::GetConfidence() != 100)
            return identity;

        identity.Handle = PositionTracker::GetObjectHandle();
        identity.Runtime = PositionTracker::GetRuntimeObject();
        identity.Transform = PositionTracker::GetTransformComponent();
        if (!identity.Handle || !identity.Runtime || !identity.Transform)
            return identity;

        if (EngineRuntime::ResolveActorHandle(identity.Handle) != identity.Runtime ||
            !EngineRuntime::IsActiveObject(identity.Runtime) ||
            EngineRuntime::GetComponent(identity.Runtime, 6, true) != identity.Transform)
        {
            identity = {};
            return identity;
        }

        identity.Valid = true;
        return identity;
    }

    bool UpdateWorldGate()
    {
        const LocalWorldIdentity local = CaptureLocalWorldIdentity();
        const ULONGLONG now = GetTickCount64();
        std::vector<std::uint32_t> invalidatedPlayers;
        bool resetMotion = false;
        bool transitionDetected = false;
        bool becameReady = false;
        bool ready = false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            if (!local.Valid)
            {
                if (g_worldReady.load(std::memory_order_relaxed) ||
                    g_worldLocalRuntime || g_worldLocalHandle)
                {
                    for (auto& pair : g_proxies)
                    {
                        if (pair.second.ObjectHandle || pair.second.RuntimeObject)
                            invalidatedPlayers.push_back(pair.first);
                        ResetProxyForWorldTransition(pair.second);
                    }
                    g_spawnQueue.clear();
                    resetMotion = true;
                    transitionDetected = true;
                }

                g_worldLocalRuntime = 0;
                g_worldLocalHandle = 0;
                g_worldStableSince = 0;
                g_worldReady.store(false, std::memory_order_release);
                g_status = "Waiting for local world/runtime";
            }
            else if (local.Runtime != g_worldLocalRuntime || local.Handle != g_worldLocalHandle)
            {
                for (auto& pair : g_proxies)
                {
                    if (pair.second.ObjectHandle || pair.second.RuntimeObject)
                        invalidatedPlayers.push_back(pair.first);
                    ResetProxyForWorldTransition(pair.second);
                }
                g_spawnQueue.clear();

                g_worldLocalRuntime = local.Runtime;
                g_worldLocalHandle = local.Handle;
                g_worldStableSince = now;
                g_worldReady.store(false, std::memory_order_release);
                g_status = "Local world changed; waiting for runtime to stabilize";
                resetMotion = true;
                transitionDetected = true;
            }
            else
            {
                ready = g_worldReady.load(std::memory_order_relaxed);
                if (!ready && g_worldStableSince && now - g_worldStableSince >= kWorldStableDelayMs)
                {
                    g_worldReady.store(true, std::memory_order_release);
                    ready = true;
                    becameReady = true;
                    g_status = "Local world stable; remote proxies may spawn";
                }
            }
        }

        if (resetMotion)
            MotionReplication::ResetForWorldTransition();

        if (transitionDetected)
        {
            Logger::Write(
                Logger::Level::Info,
                "World transition detected; remote runtime/controller references suspended");
        }
        if (becameReady)
        {
            Logger::Write(
                Logger::Level::Success,
                "Local world stable; remote player spawning/motion resumed");
        }

        for (const std::uint32_t playerId : invalidatedPlayers)
        {
            MotionReplication::InvalidateRemoteController(playerId);
            PlayerLifecycleSync::NotifyProxyRemoved(playerId);
        }

        return ready || g_worldReady.load(std::memory_order_acquire);
    }

    std::uint32_t AssignGamePlayerIndexLocked(std::uint32_t playerId)
    {
        auto existing = g_proxies.find(playerId);
        if (existing != g_proxies.end() && existing->second.GamePlayerIndex)
            return existing->second.GamePlayerIndex;

        auto inUse = [](std::uint32_t index)
        {
            for (const auto& pair : g_proxies)
            {
                if (pair.second.GamePlayerIndex == index)
                    return true;
            }
            return false;
        };

        // These are logical network/player slots only. Do not commandeer a
        // Player_Manager context owned by the game; those contexts participate
        // in level-load lifecycle and can destroy/rebind our proxy.
        for (std::uint32_t index = 1; index < 16; ++index)
        {
            if (!inUse(index))
                return index;
        }
        return 0;
    }

    bool SpawnProxy(std::uint32_t playerId)
    {
        PlayerProxySpawner::SpawnOptions options{};
        bool manual = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_proxies.find(playerId);
            if (it == g_proxies.end())
                return false;

            auto& proxy = it->second;
            manual = proxy.Manual;
            if (!manual && !proxy.GamePlayerIndex)
                proxy.GamePlayerIndex = AssignGamePlayerIndexLocked(playerId);

            options.GamePlayerIndex = proxy.GamePlayerIndex;
            options.TryBindPlayerContext = false;
            if (proxy.HasSnapshot)
            {
                // Runtime definition/setup handles belong to the sender's
                // process. Create a local player actor, then apply asset-keyed
                // peer visuals. Never interpret a foreign registry index here.
                options.PreferredDefinitionHandle = 0;
                options.PlayerSetupValue = 0;
            }
        }

        const PlayerProxySpawner::SpawnResult spawned =
            PlayerProxySpawner::SpawnFromLocalPlayer(options);
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
            proxy.GamePlayerIndex = spawned.GamePlayerIndex;
            proxy.ObjectHandle = spawned.ObjectHandle;
            proxy.PersistentActorHandle = spawned.PersistentActorHandle;
            proxy.DefinitionHandle = spawned.DefinitionHandle;
            proxy.ActorStateHandle = spawned.ActorStateHandle;
            proxy.PlayerContext = spawned.PlayerContext;
            proxy.RuntimeObject = spawned.RuntimeObject;
            proxy.TransformComponent = spawned.TransformComponent;
            proxy.ActorStateComponent = spawned.ActorStateComponent;
            proxy.EquipmentComponent = spawned.EquipmentComponent;
            proxy.AppearanceComponent = spawned.AppearanceComponent;
            proxy.RigSwapComponent = spawned.RigSwapComponent;
            proxy.ActorStateEntry = spawned.ActorStateEntry;
            proxy.PlayerPostSetupApplied = spawned.PlayerPostSetupApplied;
            proxy.PlayerContextFound = spawned.PlayerContextFound;
            proxy.PlayerContextBound = spawned.PlayerContextBound;
            proxy.PlayerContextActivated = spawned.PlayerContextActivated;
            proxy.AdditionalPlayerStateApplied = spawned.AdditionalPlayerStateApplied;
            proxy.ComponentCloneApplied = spawned.ComponentCloneApplied;
            proxy.ComponentFinalizeApplied = spawned.ComponentFinalizeApplied;
            proxy.LateActorFinalizeApplied = spawned.LateActorFinalizeApplied;
            proxy.AppearanceRefreshApplied = spawned.AppearanceRefreshApplied;
            proxy.ActorStateResolved = spawned.ActorStateResolved;
            proxy.LastAppearanceRefresh = GetTickCount64();
            proxy.AppearanceRefreshPasses = 1u;
            proxy.SpawnStatus = spawned.Status;
            proxy.SpawnQueued = false;

            g_status = "Player-style remote proxy active: " + spawned.Status;
        }

        MotionReplication::InvalidateRemoteController(playerId);
        PlayerLifecycleSync::NotifyProxySpawn(playerId);

        Logger::WriteFormat(
            Logger::Level::Success,
            "Remote player %u proxy active: gameSlot=%u handle=0x%08X object=0x%08X transform=0x%08X stateComp=0x%08X equipment=0x%08X appearance=0x%08X rigSwap=0x%08X visualImport=%s visualActivate=%s nativeFinalize=%s visualRebuild=%s playerCtx=0x%08X bound=%s",
            playerId,
            spawned.GamePlayerIndex,
            spawned.ObjectHandle,
            static_cast<unsigned int>(spawned.RuntimeObject),
            static_cast<unsigned int>(spawned.TransformComponent),
            static_cast<unsigned int>(spawned.ActorStateComponent),
            static_cast<unsigned int>(spawned.EquipmentComponent),
            static_cast<unsigned int>(spawned.AppearanceComponent),
            static_cast<unsigned int>(spawned.RigSwapComponent),
            spawned.ComponentCloneApplied ? "yes" : "no",
            spawned.ComponentFinalizeApplied ? "yes" : "no",
            spawned.LateActorFinalizeApplied ? "yes" : "no",
            spawned.AppearanceRefreshApplied ? "yes" : "no",
            static_cast<unsigned int>(spawned.PlayerContext),
            spawned.PlayerContextBound ? "yes" : "no");
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
        diagnostics.GamePlayerIndex = proxy.GamePlayerIndex;
        diagnostics.ObjectHandle = proxy.ObjectHandle;
        diagnostics.PersistentActorHandle = proxy.PersistentActorHandle;
        diagnostics.DefinitionHandle = proxy.DefinitionHandle;
        diagnostics.ActorStateHandle = proxy.ActorStateHandle;
        diagnostics.SnapshotRevision = proxy.Snapshot.Revision;
        diagnostics.PlayerContext = proxy.PlayerContext;
        diagnostics.RuntimeObject = proxy.RuntimeObject;
        diagnostics.TransformComponent = proxy.TransformComponent;
        diagnostics.ActorStateComponent = proxy.ActorStateComponent;
        diagnostics.EquipmentComponent = proxy.EquipmentComponent;
        diagnostics.AppearanceComponent = proxy.AppearanceComponent;
        diagnostics.RigSwapComponent = proxy.RigSwapComponent;
        diagnostics.ActorStateEntry = proxy.ActorStateEntry;
        diagnostics.Connected = proxy.Connected;
        diagnostics.HasSnapshot = proxy.HasSnapshot;
        diagnostics.PlayerPostSetupApplied = proxy.PlayerPostSetupApplied;
        diagnostics.PlayerContextFound = proxy.PlayerContextFound;
        diagnostics.PlayerContextBound = proxy.PlayerContextBound;
        diagnostics.PlayerContextActivated = proxy.PlayerContextActivated;
        diagnostics.AdditionalPlayerStateApplied = proxy.AdditionalPlayerStateApplied;
        diagnostics.ComponentCloneApplied = proxy.ComponentCloneApplied;
        diagnostics.ComponentFinalizeApplied = proxy.ComponentFinalizeApplied;
        diagnostics.LateActorFinalizeApplied = proxy.LateActorFinalizeApplied;
        diagnostics.AppearanceRefreshApplied = proxy.AppearanceRefreshApplied;
        diagnostics.ActorStateResolved = proxy.ActorStateResolved;
        diagnostics.AppearanceRefreshPasses = proxy.AppearanceRefreshPasses;
        diagnostics.Manual = proxy.Manual;
        diagnostics.SpawnStatus = proxy.SpawnStatus;
        return diagnostics;
    }
}

namespace RemotePlayerManager
{
    void SetVisualSyncResult(std::uint32_t playerId, std::uint32_t revision, bool applied)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        if (it == g_proxies.end()) return;
        it->second.AppearanceRefreshApplied = applied;
        it->second.SpawnStatus = applied ? "Peer visual revision " + std::to_string(revision) + " applied" :
            "Peer visual apply failed; inspect asset/build diagnostics";
    }
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_proxies.clear();
        g_spawnQueue.clear();
        g_worldReady.store(false, std::memory_order_release);
        g_worldLocalRuntime = 0;
        g_worldLocalHandle = 0;
        g_worldStableSince = 0;
        g_manualDummy = false;
        g_dummyPosition = {};
        g_status = "Waiting for local world/runtime";
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
        // Never touch remote actor/controller pointers while the game is
        // unloading or rebuilding the local world.
        if (!UpdateWorldGate())
            return;

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
        PlayerLifecycleSync::PumpGameThread();
        // Replay against the final controller, after spawning/appearance loads.
        MotionReplication::PumpRemoteMotionOnGameThread();
    }

    void Update()
    {
        const auto peers = NetworkManager::GetPeers();
        const ULONGLONG now = GetTickCount64();
        const bool worldReady = UpdateWorldGate();
        const auto join = NetworkManager::GetJoinGeneration();
        if (worldReady && join && join != g_arrivedJoin && NetworkManager::GetMode() == NetworkManager::Mode::Client)
        {
            for (const auto& host : peers)
            {
                if (host.PlayerId != 1 || !host.Connected || !host.HasTransform ||
                    !host.LastTransformMs || now - host.LastTransformMs > kRemoteTransformTimeoutMs) continue;
                const auto transform = PositionTracker::GetTransformComponent();
                const EngineVec4 arrival{ host.Position.X + 100.0f, host.Position.Y, host.Position.Z, 1.0f };
                if (EngineRuntime::IsActiveComponent(transform) &&
                    std::isfinite(arrival.X) && std::isfinite(arrival.Y) && std::isfinite(arrival.Z) &&
                    CallSetWorldPosition(transform, arrival))
                {
                    if (host.HasRotation) ApplyRotation(transform, host.Rotation);
                    g_arrivedJoin = join;
                    Logger::Write("Joined character positioned beside host after local world became ready");
                }
                break;
            }
        }
        std::set<std::uint32_t> activeIds;

        struct Move
        {
            std::uint32_t PlayerId;
            uintptr_t Transform;
            GameState::Vec3 Target;
            float Rotation[4]{ 0, 0, 0, 1 };
            bool HasRotation = false;
        };
        std::vector<Move> moves;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            for (const auto& peer : peers)
            {
                if (!peer.PlayerId || !peer.Connected ||
                    (!peer.HasTransform && !peer.HasSnapshot))
                {
                    continue;
                }

                activeIds.insert(peer.PlayerId);
                auto& proxy = g_proxies[peer.PlayerId];
                proxy.PlayerId = peer.PlayerId;
                proxy.Connected = true;
                proxy.Manual = false;
                proxy.LastSeenMs = peer.LastSeenMs;
                proxy.HasSnapshot = peer.HasSnapshot;
                if (peer.HasSnapshot)
                    proxy.Snapshot = peer.Snapshot;
                if (!proxy.GamePlayerIndex)
                    proxy.GamePlayerIndex = AssignGamePlayerIndexLocked(peer.PlayerId);
                if (peer.HasTransform)
                    proxy.Target = { peer.Position.X, peer.Position.Y, peer.Position.Z };

                const bool freshTransform = peer.HasTransform &&
                    now - peer.LastTransformMs <= kRemoteTransformTimeoutMs;
                if (worldReady && freshTransform && !proxy.TransformComponent && !proxy.SpawnQueued &&
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

                if (worldReady && freshTransform && proxy.TransformComponent)
                {
                    moves.push_back({ peer.PlayerId, proxy.TransformComponent, proxy.Target });
                    moves.back().HasRotation = peer.HasRotation;
                    for (unsigned i = 0; i < 4; ++i) moves.back().Rotation[i] = peer.Rotation[i];
                }
            }

            if (g_manualDummy)
            {
                activeIds.insert(kManualProxyPlayerId);
                auto& proxy = g_proxies[kManualProxyPlayerId];
                proxy.PlayerId = kManualProxyPlayerId;
                proxy.GamePlayerIndex = 0;
                proxy.Connected = true;
                proxy.Manual = true;
                proxy.LastSeenMs = now;
                proxy.Target = g_dummyPosition;

                if (worldReady && proxy.TransformComponent)
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

            if (!worldReady)
            {
                g_status = "Remote peers retained; waiting for local world/runtime";
            }
            else if (activeIds.empty())
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
        {
            const auto handle = GetProxyObjectHandle(move.PlayerId);
            const auto runtime = EngineRuntime::ResolveActorHandle(handle);
            if (!runtime || EngineRuntime::GetComponent(runtime, 6) != move.Transform) continue;
            float current[8]{};
            if (!EngineRuntime::ReadBytes(move.Transform + 0x24, current, sizeof(current))) continue;
            const bool moved = std::fabs(current[0] - move.Target.X) > 0.01f ||
                std::fabs(current[1] - move.Target.Y) > 0.01f || std::fabs(current[2] - move.Target.Z) > 0.01f;
            float rotationDelta = 0, oppositeDelta = 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                rotationDelta += std::fabs(current[4 + i] - move.Rotation[i]);
                oppositeDelta += std::fabs(current[4 + i] + move.Rotation[i]);
            }
            const bool turned = move.HasRotation && rotationDelta > 0.0001f && oppositeDelta > 0.0001f;
            if (turned) ApplyRotation(move.Transform, move.Rotation);
            if (moved) MoveProxy(move.PlayerId, move.Transform, move.Target);
            if (moved || turned)
                RefreshRenderTransform(EngineRuntime::GetComponent(runtime, 7));
        }
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

    std::vector<std::uint32_t> GetActiveProxyPlayerIds()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::uint32_t> result;
        result.reserve(g_proxies.size());
        for (const auto& pair : g_proxies)
        {
            const auto& proxy = pair.second;
            if (proxy.Connected && proxy.ObjectHandle && proxy.RuntimeObject)
                result.push_back(pair.first);
        }
        return result;
    }

    bool RefreshProxyRuntimeState(std::uint32_t playerId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_proxies.find(playerId);
        if (it == g_proxies.end())
            return false;

        auto& proxy = it->second;
        if (!proxy.ObjectHandle)
            return false;

        const uintptr_t runtimeObject = EngineRuntime::ResolveActorHandle(proxy.ObjectHandle);
        if (!runtimeObject || !EngineRuntime::IsActiveObject(runtimeObject))
            return false;

        proxy.RuntimeObject = runtimeObject;
        proxy.TransformComponent = EngineRuntime::GetComponent(runtimeObject, 6, true);
        proxy.ActorStateComponent = EngineRuntime::GetComponent(runtimeObject, 7, true);
        proxy.EquipmentComponent = EngineRuntime::GetComponent(runtimeObject, 9, true);
        proxy.AppearanceComponent = EngineRuntime::GetComponent(runtimeObject, 7, true);
        proxy.RigSwapComponent = EngineRuntime::GetComponent(runtimeObject, 42, false);

        proxy.ActorStateHandle = 0;
        if (proxy.ActorStateComponent)
            EngineRuntime::Read(proxy.ActorStateComponent + 0x9C, proxy.ActorStateHandle);

        proxy.ActorStateResolved = proxy.ActorStateHandle >= 2;
        return proxy.TransformComponent != 0;
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
