#include "RemotePlayerManager.h"

#include "Logger.h"
#include "NetworkManager.h"
#include "PositionTracker.h"
#include "RuntimeInspector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
    constexpr uintptr_t kIdaImageBase = 0x00400000;
    constexpr uintptr_t kSetActorWorldPositionIda = 0x00F5DD50;
    constexpr uintptr_t kCloneActorFromHandleIda = 0x00FC6220;
    constexpr uintptr_t kRuntimeRootGlobalIda = 0x019FEC38;

    constexpr uintptr_t kObjectRegistryOffset = 0x2238;
    constexpr uintptr_t kObjectTableOffset = 0x0C;
    constexpr uintptr_t kHandleTableOffset = 0x1C;
    constexpr uintptr_t kObjectCountOffset = 0x20;
    constexpr uintptr_t kObjectFlagsOffset = 0x10C;
    constexpr uintptr_t kComponentArrayOffset = 0x3C;
    constexpr unsigned int kTransformComponentId = 6;
    constexpr uintptr_t kTransformFlagsOffset = 0x20;

    constexpr ULONGLONG kQueueRetryDelayMs = 5000;
    constexpr ULONGLONG kProxyAcquireTimeoutMs = 15000;
    constexpr std::uint64_t kRemoteTransformTimeoutMs = 3000;

    using SetActorWorldPositionFn = int(__thiscall*)(void* transformComponent, const void* position);
    using CloneActorFromHandleFn = int*(__stdcall*)(int* outHandle, int sourceActorHandle);

    struct EngineVec4
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float W = 1.0f;
    };

    std::mutex g_mutex;
    bool g_hasDummy = false;
    bool g_manualDummy = false;
    GameState::Vec3 g_dummyPosition{};
    GameState::Vec3 g_targetPosition{};

    int g_proxyRecordIndex = -1;
    std::uint32_t g_proxyObjectHandle = 0;
    uintptr_t g_proxyRuntimeObject = 0;
    uintptr_t g_proxyTransformComponent = 0;

    ULONGLONG g_lastQueueAttempt = 0;
    ULONGLONG g_queueStartedAt = 0;
    ULONGLONG g_lastMoveFailureLog = 0;
    ULONGLONG g_lastRemoteStateLog = 0;
    ULONGLONG g_lastProxySearchLog = 0;
    bool g_usingExistingActor = false;
    std::atomic<bool> g_spawnRequested{ false };
    std::atomic<bool> g_spawnInProgress{ false };
    ULONGLONG g_lastDirectSpawnAttempt = 0;
    std::string g_status = "Waiting for a second player";

    uintptr_t RuntimeAddress(uintptr_t idaAddress)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return (base && idaAddress >= kIdaImageBase)
            ? base + (idaAddress - kIdaImageBase)
            : 0;
    }

    template <typename T>
    bool SafeRead(uintptr_t address, T& value)
    {
        value = {};
        if (!address)
            return false;

        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;

        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (address + sizeof(T) > end)
            return false;

        __try
        {
            value = *reinterpret_cast<const T*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            value = {};
            return false;
        }
    }

    bool GuardedSetPosition(
        SetActorWorldPositionFn function,
        void* transformComponent,
        const EngineVec4* position)
    {
        if (!function || !transformComponent || !position)
            return false;

        __try
        {
            function(transformComponent, position);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ResetProxyLocked(const char* status)
    {
        g_proxyRecordIndex = -1;
        g_proxyObjectHandle = 0;
        g_proxyRuntimeObject = 0;
        g_proxyTransformComponent = 0;
        g_queueStartedAt = 0;
        g_usingExistingActor = false;
        if (status)
            g_status = status;
    }

    int SelectProxyCandidate(const std::vector<RuntimeInspector::Record>& records)
    {
        const auto valid = [](const RuntimeInspector::Record& record)
        {
            return !record.IsLocalPlayer &&
                   !record.Instantiated &&
                   !record.Pending &&
                   record.ObjectHandle == 0 &&
                   record.Address != 0 &&
                   record.DefinitionReference != 0 &&
                   (record.Flags & 0x3u) == 0;
        };

        // Prefer an unused record with the same definition as the local actor.
        // If the current area has no such record, use another inactive actor
        // record so the first two-player test can still produce a visible body.
        for (const auto& record : records)
            if (valid(record) && record.SameDefinitionAsLocal)
                return record.Index;

        for (const auto& record : records)
            if (valid(record))
                return record.Index;

        return -1;
    }

    uintptr_t ResolveActorHandle(std::uint32_t handle)
    {
        if ((handle & 0x0FFF0000u) == 0)
            return 0;

        uintptr_t runtimeRoot = 0;
        if (!SafeRead(RuntimeAddress(kRuntimeRootGlobalIda), runtimeRoot) || !runtimeRoot)
            return 0;

        const uintptr_t registry = runtimeRoot + kObjectRegistryOffset;
        const std::uint16_t index = static_cast<std::uint16_t>(handle);
        std::uint32_t count = 0;
        uintptr_t handleTable = 0;
        uintptr_t objectTable = 0;
        if (!SafeRead(registry + kObjectCountOffset, count) || index >= count ||
            !SafeRead(registry + kHandleTableOffset, handleTable) || !handleTable ||
            !SafeRead(registry + kObjectTableOffset, objectTable) || !objectTable)
            return 0;

        std::uint32_t stored = 0;
        uintptr_t object = 0;
        if (!SafeRead(handleTable + static_cast<uintptr_t>(index) * 4, stored) || stored != handle ||
            !SafeRead(objectTable + static_cast<uintptr_t>(index) * 4, object))
            return 0;
        return object;
    }

    bool GuardedCloneActor(CloneActorFromHandleFn function, int* outHandle, int sourceHandle)
    {
        if (!function || !outHandle || !sourceHandle)
            return false;
        __try
        {
            function(outHandle, sourceHandle);
            return *outHandle != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *outHandle = 0;
            return false;
        }
    }

    bool CreateDirectProxyOnGameThread()
    {
        const std::uint32_t localHandle = PositionTracker::GetObjectHandle();
        if (!localHandle)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Direct proxy spawn waiting for local player";
            return false;
        }

        const auto clone = reinterpret_cast<CloneActorFromHandleFn>(
            RuntimeAddress(kCloneActorFromHandleIda));
        int newHandle = 0;
        Logger::WriteFormat(Logger::Level::Warning,
            "Remote proxy direct clone starting: source handle=0x%08X", localHandle);
        if (!GuardedCloneActor(clone, &newHandle, static_cast<int>(localHandle)))
        {
            Logger::Write(Logger::Level::Error,
                "Remote proxy direct clone failed or raised an exception");
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Direct actor clone failed; will retry";
            return false;
        }

        if (static_cast<std::uint32_t>(newHandle) == localHandle)
        {
            Logger::Write(Logger::Level::Error,
                "Remote proxy clone returned the local-player handle; rejected");
            return false;
        }

        const uintptr_t object = ResolveActorHandle(static_cast<std::uint32_t>(newHandle));
        uintptr_t transform = 0;
        std::uint8_t actorFlags = 0;
        std::uint8_t transformFlags = 0;
        if (!object ||
            !SafeRead(object + kObjectFlagsOffset, actorFlags) || (actorFlags & 1) == 0 ||
            !SafeRead(object + kComponentArrayOffset + kTransformComponentId * sizeof(uintptr_t), transform) || !transform ||
            !SafeRead(transform + kTransformFlagsOffset, transformFlags) || (transformFlags & 1) == 0)
        {
            Logger::WriteFormat(Logger::Level::Error,
                "Remote proxy clone created handle=0x%08X but actor/component resolution failed object=0x%08X transform=0x%08X",
                newHandle, static_cast<unsigned int>(object), static_cast<unsigned int>(transform));
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Clone created but Component 6 was unavailable";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_proxyRecordIndex = -1;
            g_proxyObjectHandle = static_cast<std::uint32_t>(newHandle);
            g_proxyRuntimeObject = object;
            g_proxyTransformComponent = transform;
            g_usingExistingActor = false;
            g_status = "Visible remote-player clone active";
        }
        Logger::WriteFormat(Logger::Level::Success,
            "Remote proxy direct clone active: handle=0x%08X object=0x%08X transform=0x%08X",
            newHandle, static_cast<unsigned int>(object), static_cast<unsigned int>(transform));
        return true;
    }

    bool TryQueueProxy()
    {
        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_proxyRecordIndex >= 0 || now - g_lastQueueAttempt < kQueueRetryDelayMs)
                return false;
            g_lastQueueAttempt = now;
            g_status = "Searching for an inactive actor record";
        }

        RuntimeInspector::RefreshNow();
        const auto records = RuntimeInspector::GetRecords();
        const int candidate = SelectProxyCandidate(records);
        if (candidate < 0)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status = "No inactive actor record; trying an existing area actor";
            }
            Logger::Write(Logger::Level::Warning,
                "Remote proxy: no safe inactive runtime record is available");
            return false;
        }

        if (!RuntimeInspector::QueueExistingRecord(candidate, false))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Proxy queue failed: " + RuntimeInspector::GetLastActionText();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_proxyRecordIndex = candidate;
            g_queueStartedAt = now;
            g_status = "Proxy record queued; waiting for actor creation";
        }

        Logger::WriteFormat(
            Logger::Level::Warning,
            "Remote proxy: queued runtime record #%d for visible-player test",
            candidate);
        return true;
    }

    bool TryAcquireQueuedProxy()
    {
        int recordIndex = -1;
        ULONGLONG queueStartedAt = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            recordIndex = g_proxyRecordIndex;
            queueStartedAt = g_queueStartedAt;
            if (recordIndex < 0 || g_proxyTransformComponent)
                return g_proxyTransformComponent != 0;
        }

        RuntimeInspector::RefreshNow();
        const auto records = RuntimeInspector::GetRecords();
        for (const auto& record : records)
        {
            if (record.Index != recordIndex)
                continue;

            if (!record.Instantiated || !record.RuntimeObject || !record.ObjectHandle)
                break;

            std::uint8_t actorFlags = 0;
            uintptr_t transform = 0;
            std::uint8_t transformFlags = 0;
            if (!SafeRead(record.RuntimeObject + kObjectFlagsOffset, actorFlags) || (actorFlags & 1) == 0 ||
                !SafeRead(record.RuntimeObject + kComponentArrayOffset + kTransformComponentId * sizeof(uintptr_t), transform) || !transform ||
                !SafeRead(transform + kTransformFlagsOffset, transformFlags) || (transformFlags & 1) == 0)
            {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_proxyObjectHandle = record.ObjectHandle;
                g_proxyRuntimeObject = record.RuntimeObject;
                g_proxyTransformComponent = transform;
                g_status = "Visible remote-player proxy active";
            }

            Logger::WriteFormat(
                Logger::Level::Success,
                "Remote proxy active: record=%d handle=0x%08X object=0x%08X transform=0x%08X",
                record.Index,
                record.ObjectHandle,
                static_cast<unsigned int>(record.RuntimeObject),
                static_cast<unsigned int>(transform));
            return true;
        }

        if (queueStartedAt && GetTickCount64() - queueStartedAt > kProxyAcquireTimeoutMs)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ResetProxyLocked("Queued actor did not instantiate; another record will be tried");
        }
        return false;
    }


    bool TryAcquireExistingActorProxy()
    {
        RuntimeInspector::RefreshNow();
        const auto records = RuntimeInspector::GetRecords();

        // First visible proof fallback: borrow an already-instantiated, non-local
        // actor when the current area has no inactive record that can be queued.
        // Prefer a second actor using the local player's definition, then any
        // active non-local actor with a valid Component 6 transform.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const auto& record : records)
            {
                if (record.IsLocalPlayer || !record.Instantiated ||
                    !record.RuntimeObject || !record.ObjectHandle)
                    continue;
                if (pass == 0 && !record.SameDefinitionAsLocal)
                    continue;

                std::uint8_t actorFlags = 0;
                uintptr_t transform = 0;
                std::uint8_t transformFlags = 0;
                if (!SafeRead(record.RuntimeObject + kObjectFlagsOffset, actorFlags) ||
                    (actorFlags & 1) == 0 ||
                    !SafeRead(record.RuntimeObject + kComponentArrayOffset +
                        kTransformComponentId * sizeof(uintptr_t), transform) ||
                    !transform ||
                    !SafeRead(transform + kTransformFlagsOffset, transformFlags) ||
                    (transformFlags & 1) == 0)
                {
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_proxyRecordIndex = record.Index;
                    g_proxyObjectHandle = record.ObjectHandle;
                    g_proxyRuntimeObject = record.RuntimeObject;
                    g_proxyTransformComponent = transform;
                    g_usingExistingActor = true;
                    g_status = "Visible proxy active (using an existing area actor)";
                }

                Logger::WriteFormat(
                    Logger::Level::Success,
                    "Remote proxy fallback active: record=%d handle=0x%08X object=0x%08X transform=0x%08X sameDefinition=%d",
                    record.Index, record.ObjectHandle,
                    static_cast<unsigned int>(record.RuntimeObject),
                    static_cast<unsigned int>(transform),
                    record.SameDefinitionAsLocal ? 1 : 0);
                return true;
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastProxySearchLog >= 3000)
        {
            g_lastProxySearchLog = now;
            Logger::WriteFormat(
                Logger::Level::Warning,
                "Remote proxy search found no usable actor (runtime records=%u)",
                static_cast<unsigned int>(records.size()));
        }
        return false;
    }

    bool MoveVisibleProxy(const GameState::Vec3& position)
    {
        uintptr_t transform = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            transform = g_proxyTransformComponent;
        }
        if (!transform)
            return false;

        const auto function = reinterpret_cast<SetActorWorldPositionFn>(
            RuntimeAddress(kSetActorWorldPositionIda));
        const EngineVec4 enginePosition{ position.X, position.Y, position.Z, 1.0f };
        if (!GuardedSetPosition(function, reinterpret_cast<void*>(transform), &enginePosition))
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_lastMoveFailureLog >= 2000)
            {
                g_lastMoveFailureLog = now;
                Logger::Write(Logger::Level::Error,
                    "Remote proxy position call raised an exception; proxy reference cleared");
            }

            std::lock_guard<std::mutex> lock(g_mutex);
            ResetProxyLocked("Proxy transform became invalid; reacquiring actor");
            return false;
        }
        return true;
    }
}

namespace RemotePlayerManager
{
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "Waiting for a second player";
        Logger::Write("RemotePlayerManager initialized with visible-proxy support");
    }

    void PumpGameThread()
    {
        if (!g_spawnRequested.exchange(false, std::memory_order_acq_rel))
            return;
        if (g_spawnInProgress.exchange(true, std::memory_order_acq_rel))
            return;

        bool alreadyActive = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            alreadyActive = g_proxyTransformComponent != 0;
        }
        if (!alreadyActive)
            CreateDirectProxyOnGameThread();

        g_spawnInProgress.store(false, std::memory_order_release);
    }

    void Shutdown()
    {
        ClearDummy();
        Logger::Write("RemotePlayerManager shutdown");
    }

    void Update()
    {
        bool shouldHaveProxy = false;
        bool manualDummy = false;
        GameState::Vec3 target{};

        if (NetworkManager::HasRemoteTransform() &&
            NetworkManager::GetRemoteTransformAgeMs() <= kRemoteTransformTimeoutMs)
        {
            const NetworkManager::Vec3 remote = NetworkManager::GetRemoteTransform();
            target = { remote.X, remote.Y, remote.Z };
            shouldHaveProxy = true;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            manualDummy = g_manualDummy;
            if (!shouldHaveProxy && manualDummy)
            {
                target = g_targetPosition;
                shouldHaveProxy = true;
            }

            if (shouldHaveProxy)
            {
                g_targetPosition = target;
                if (!g_hasDummy)
                {
                    g_dummyPosition = target;
                    g_hasDummy = true;
                }
                else
                {
                    constexpr float smoothing = 0.35f;
                    g_dummyPosition.X += (target.X - g_dummyPosition.X) * smoothing;
                    g_dummyPosition.Y += (target.Y - g_dummyPosition.Y) * smoothing;
                    g_dummyPosition.Z += (target.Z - g_dummyPosition.Z) * smoothing;
                }
                target = g_dummyPosition;
            }
            else if (!g_proxyTransformComponent)
            {
                g_status = "Waiting for a second player";
            }
        }

        if (!shouldHaveProxy)
            return;

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastRemoteStateLog >= 2000)
        {
            g_lastRemoteStateLog = now;
            Logger::WriteFormat(
                Logger::Level::Debug,
                "Remote transform accepted XYZ=(%.3f, %.3f, %.3f) age=%llu ms",
                target.X, target.Y, target.Z,
                static_cast<unsigned long long>(NetworkManager::GetRemoteTransformAgeMs()));
        }

        if (!HasVisibleProxy())
        {
            if (now - g_lastDirectSpawnAttempt >= 3000)
            {
                g_lastDirectSpawnAttempt = now;
                g_spawnRequested.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status = "Direct player clone queued on render thread";
            }
            return;
        }

        if (MoveVisibleProxy(target))
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = g_usingExistingActor
                ? "Visible proxy moving (existing area actor)"
                : "Visible remote-player proxy moving";
        }
    }

    void SpawnDummyNearLocalPlayer()
    {
        if (!PositionTracker::HasPosition())
        {
            Logger::Write("Visible proxy spawn skipped: position tracker not locked");
            return;
        }

        const PositionTracker::Vec3 local = PositionTracker::GetPosition();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_targetPosition = { local.X + 150.0f, local.Y, local.Z + 150.0f };
            g_dummyPosition = g_targetPosition;
            g_hasDummy = true;
            g_manualDummy = true;
            g_status = "Manual visible-proxy test requested";
        }

        Logger::Write("Manual visible proxy requested near local player");
        g_spawnRequested.store(true, std::memory_order_release);
    }

    void ClearDummy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hasDummy = false;
        g_manualDummy = false;
        g_dummyPosition = {};
        g_targetPosition = {};
        ResetProxyLocked("Visible proxy cleared (spawned actor remains game-managed)");
    }

    bool HasDummy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_hasDummy;
    }

    bool HasVisibleProxy()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_proxyTransformComponent != 0;
    }

    std::uint32_t GetProxyObjectHandle()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_proxyObjectHandle;
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
        const bool hasDummy = HasDummy();
        const GameState::Vec3 position = GetDummyPosition();

        if (!hasDummy)
        {
            std::cout << "[Visible Proxy] No remote transform available\n";
            return;
        }

        std::cout << "[Visible Proxy Target]\n";
        std::cout << "  X: " << position.X << "\n";
        std::cout << "  Y: " << position.Y << "\n";
        std::cout << "  Z: " << position.Z << "\n";
        std::cout << "  Status: " << GetStatusText() << "\n";
    }
}
