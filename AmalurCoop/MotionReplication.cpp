#include "MotionReplication.h"

#include "EngineRuntime.h"
#include "Logger.h"
#include "MinHook.h"
#include "NetworkManager.h"
#include "PositionTracker.h"
#include "RemotePlayerManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace
{
    constexpr uintptr_t kIdaImageBase = 0x00400000;
    constexpr uintptr_t kMotionRequestIda = 0x00D1D840;
    constexpr uintptr_t kActorStateManagerGlobalIda = 0x019FDF54;
    constexpr uintptr_t kActorStateEntriesOffset = 0xC4;
    constexpr uintptr_t kActorStateCountOffset = 0xC8;
    constexpr uintptr_t kActorStateControllerOffset = 0x44;
    constexpr uintptr_t kActorStateHandleOffset = 0x9C;
    constexpr uintptr_t kControllerQueueOffset = 0x14;
    constexpr uintptr_t kControllerA8Offset = 0xA8;
    constexpr uintptr_t kControllerACOffset = 0xAC;
    constexpr uintptr_t kControllerB4Offset = 0xB4;
    constexpr unsigned int kActorStateComponentId = 7;
    constexpr int kMaxActorStateEntries = 8192;
    constexpr int kMaxIncomingMotionPerPump = 6;
    constexpr int kMaxOutgoingMotionPerPump = 6;
    constexpr std::size_t kMaxQueuedOutgoingMotion = 64;
    constexpr ULONGLONG kLocalControllerRefreshMs = 250;
    constexpr ULONGLONG kNetworkCacheRefreshMs = 100;
    constexpr ULONGLONG kDiagnosticLogIntervalMs = 10000;
    constexpr bool kEnableMotionHook = true;

    using MotionRequestFn = int(__thiscall*)(
        void* controller,
        std::uint32_t motionHash,
        std::int32_t requestedVariant,
        std::int32_t parameterA,
        std::uint8_t flagA,
        uintptr_t localContext,
        std::int32_t parameterB);

    MotionRequestFn oMotionRequest = nullptr;
    void* g_hookTarget = nullptr;

    std::atomic<bool> g_hookInstalled{ false };
    std::atomic<std::uint32_t> g_packetSequence{ 0 };
    std::atomic<std::uint32_t> g_lastHash{ 0 };
    std::atomic<std::uint32_t> g_lastSequence{ 0 };
    std::atomic<std::uint64_t> g_sentPackets{ 0 };
    std::atomic<std::uint64_t> g_receivedPackets{ 0 };
    std::atomic<std::uint64_t> g_droppedPackets{ 0 };
    std::atomic<std::uint64_t> g_replayCount{ 0 };
    std::atomic<int> g_localActorStateIndex{ -1 };
    std::atomic<int> g_remoteActorStateIndex{ -1 };

    // These are the only values the D1D840 hook reads. They are populated on
    // the render/game thread so the hook never scans actor-state memory.
    std::atomic<uintptr_t> g_localControllerFast{ 0 };
    std::atomic<std::uint32_t> g_localPlayerIdFast{ 0 };
    std::atomic<bool> g_canSendMotionFast{ false };

    std::mutex g_stateMutex;
    uintptr_t g_localRuntimeObject = 0;
    uintptr_t g_localController = 0;
    uintptr_t g_lastRemoteController = 0;
    uintptr_t g_lastRemoteRuntimeObject = 0;
    std::uint32_t g_lastRemoteActorNetId = 0;
    uintptr_t g_localActorStateEntry = 0;
    uintptr_t g_remoteActorStateEntry = 0;
    uintptr_t g_remoteActorStateComponent = 0;
    std::uint32_t g_remoteActorStateHandle = 0;
    uintptr_t g_localControllerQueue = 0;
    uintptr_t g_remoteControllerQueue = 0;
    std::uint32_t g_localControllerA8 = 0;
    std::uint32_t g_localControllerAC = 0;
    std::uint32_t g_localControllerB4 = 0;
    std::uint32_t g_remoteControllerA8 = 0;
    std::uint32_t g_remoteControllerAC = 0;
    std::uint32_t g_remoteControllerB4 = 0;
    std::string g_status = "Motion hook not initialized";

    std::mutex g_outgoingMutex;
    std::array<NetworkManager::MotionEventPacket, kMaxQueuedOutgoingMotion> g_outgoingMotion{};
    std::size_t g_outgoingReadIndex = 0;
    std::size_t g_outgoingCount = 0;

    ULONGLONG g_nextLocalControllerRefresh = 0;
    ULONGLONG g_nextNetworkCacheRefresh = 0;
    ULONGLONG g_lastLocalControllerDiagnostic = 0;
    ULONGLONG g_lastRemoteControllerLog = 0;
    ULONGLONG g_lastReplayDropLog = 0;

    thread_local bool g_applyingRemoteMotion = false;

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

    bool IsReadableObject(uintptr_t address)
    {
        std::uint32_t ignored = 0;
        return SafeRead(address, ignored);
    }

    void SetStatus(const std::string& status)
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_status = status;
    }

    bool ReadActorStateTable(uintptr_t& entries, int& count)
    {
        entries = 0;
        count = 0;

        uintptr_t manager = 0;
        if (!SafeRead(RuntimeAddress(kActorStateManagerGlobalIda), manager) || !manager)
            return false;

        if (!SafeRead(manager + kActorStateEntriesOffset, entries) || !entries ||
            !SafeRead(manager + kActorStateCountOffset, count) ||
            count <= 0 || count > kMaxActorStateEntries)
        {
            return false;
        }

        return true;
    }

    uintptr_t ResolveActorStateEntryFromHandle(std::uint32_t handle, int* outIndex = nullptr)
    {
        uintptr_t entries = 0;
        int count = 0;
        if (!ReadActorStateTable(entries, count) ||
            handle >= static_cast<std::uint32_t>(count))
        {
            return 0;
        }

        uintptr_t entry = 0;
        if (!SafeRead(entries + static_cast<uintptr_t>(handle) * sizeof(uintptr_t), entry) ||
            !entry)
        {
            return 0;
        }

        if (outIndex)
            *outIndex = static_cast<int>(handle);
        return entry;
    }

    void ReadControllerFields(
        uintptr_t controller,
        uintptr_t& queue,
        std::uint32_t& valueA8,
        std::uint32_t& valueAC,
        std::uint32_t& valueB4)
    {
        queue = controller ? controller + kControllerQueueOffset : 0;
        valueA8 = 0;
        valueAC = 0;
        valueB4 = 0;
        if (!controller)
            return;

        SafeRead(controller + kControllerA8Offset, valueA8);
        SafeRead(controller + kControllerACOffset, valueAC);
        SafeRead(controller + kControllerB4Offset, valueB4);
    }

    void ClearLocalControllerCache(const char* status)
    {
        g_localControllerFast.store(0, std::memory_order_release);
        g_localActorStateIndex.store(-1, std::memory_order_release);

        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_localRuntimeObject = 0;
        g_localController = 0;
        g_localActorStateEntry = 0;
        g_localControllerQueue = 0;
        g_localControllerA8 = 0;
        g_localControllerAC = 0;
        g_localControllerB4 = 0;
        if (status)
            g_status = status;
    }

    void AdoptLocalController(
        uintptr_t runtimeObject,
        uintptr_t controller,
        int actorStateIndex,
        uintptr_t actorStateEntry)
    {
        if (!runtimeObject || !controller)
            return;

        uintptr_t queue = 0;
        std::uint32_t valueA8 = 0;
        std::uint32_t valueAC = 0;
        std::uint32_t valueB4 = 0;
        ReadControllerFields(controller, queue, valueA8, valueAC, valueB4);

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            changed = g_localRuntimeObject != runtimeObject ||
                g_localController != controller ||
                g_localActorStateEntry != actorStateEntry;

            g_localRuntimeObject = runtimeObject;
            g_localController = controller;
            g_localActorStateEntry = actorStateEntry;
            g_localControllerQueue = queue;
            g_localControllerA8 = valueA8;
            g_localControllerAC = valueAC;
            g_localControllerB4 = valueB4;
            g_status = "Local Component-7 motion controller cached";
        }

        g_localActorStateIndex.store(actorStateIndex, std::memory_order_release);
        g_localControllerFast.store(controller, std::memory_order_release);

        // This log is emitted only when the verified cached controller changes.
        // No table scan occurs before it.
        if (changed)
        {
            Logger::WriteFormat(
                Logger::Level::Success,
                "Motion local controller cached: controller=0x%08X actorState=%d entry=0x%08X A8=0x%08X AC=0x%08X B4=0x%08X source=Component7",
                static_cast<unsigned int>(controller),
                actorStateIndex,
                static_cast<unsigned int>(actorStateEntry),
                valueA8,
                valueAC,
                valueB4);
        }
    }

    void RefreshLocalControllerCache()
    {
        const ULONGLONG now = GetTickCount64();
        if (now < g_nextLocalControllerRefresh)
            return;
        g_nextLocalControllerRefresh = now + kLocalControllerRefreshMs;

        if (!PositionTracker::HasPosition())
        {
            if (g_localControllerFast.load(std::memory_order_acquire) != 0)
                ClearLocalControllerCache("Waiting for local player runtime");
            return;
        }

        const uintptr_t runtimeObject = PositionTracker::GetRuntimeObject();
        if (!runtimeObject)
        {
            if (g_localControllerFast.load(std::memory_order_acquire) != 0)
                ClearLocalControllerCache("Waiting for local player runtime");
            return;
        }

        const uintptr_t actorStateComponent = EngineRuntime::GetComponent(
            runtimeObject, kActorStateComponentId, true);
        std::uint32_t actorStateHandle = 0;
        if (!actorStateComponent ||
            !EngineRuntime::Read(actorStateComponent + kActorStateHandleOffset, actorStateHandle) ||
            actorStateHandle < 2)
        {
            if (now - g_lastLocalControllerDiagnostic >= kDiagnosticLogIntervalMs)
            {
                g_lastLocalControllerDiagnostic = now;
                Logger::WriteFormat(
                    Logger::Level::Debug,
                    "Motion local controller pending: object=0x%08X c7=0x%08X stateHandle=0x%08X",
                    static_cast<unsigned int>(runtimeObject),
                    static_cast<unsigned int>(actorStateComponent),
                    actorStateHandle);
            }
            return;
        }

        int actorStateIndex = -1;
        const uintptr_t actorStateEntry = ResolveActorStateEntryFromHandle(
            actorStateHandle, &actorStateIndex);
        const uintptr_t controller = actorStateEntry
            ? actorStateEntry + kActorStateControllerOffset
            : 0;

        if (!controller || !IsReadableObject(controller))
        {
            if (now - g_lastLocalControllerDiagnostic >= kDiagnosticLogIntervalMs)
            {
                g_lastLocalControllerDiagnostic = now;
                Logger::WriteFormat(
                    Logger::Level::Debug,
                    "Motion local controller pending: stateHandle=0x%08X entry=0x%08X",
                    actorStateHandle,
                    static_cast<unsigned int>(actorStateEntry));
            }
            return;
        }

        AdoptLocalController(runtimeObject, controller, actorStateIndex, actorStateEntry);
    }

    void RefreshNetworkFastState()
    {
        const ULONGLONG now = GetTickCount64();
        if (now < g_nextNetworkCacheRefresh)
            return;
        g_nextNetworkCacheRefresh = now + kNetworkCacheRefreshMs;

        const NetworkManager::Mode mode = NetworkManager::GetMode();
        if (mode == NetworkManager::Mode::Offline)
        {
            g_localPlayerIdFast.store(0, std::memory_order_release);
            g_canSendMotionFast.store(false, std::memory_order_release);
            return;
        }

        const NetworkManager::Stats stats = NetworkManager::GetStats();
        g_localPlayerIdFast.store(stats.LocalPlayerId, std::memory_order_release);
        g_canSendMotionFast.store(
            stats.LocalPlayerId != 0 && stats.ConnectedPeers != 0,
            std::memory_order_release);
    }

    bool IsLocalControllerFast(void* controller)
    {
        if (g_applyingRemoteMotion || !controller)
            return false;

        const uintptr_t cached = g_localControllerFast.load(std::memory_order_acquire);
        return cached != 0 && cached == reinterpret_cast<uintptr_t>(controller);
    }

    bool TryQueueOutgoingMotion(const NetworkManager::MotionEventPacket& packet)
    {
        // Never wait inside the game hook. If the render thread owns this tiny
        // queue for a moment, drop this animation event instead of stalling.
        std::unique_lock<std::mutex> lock(g_outgoingMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return false;

        if (g_outgoingCount == kMaxQueuedOutgoingMotion)
        {
            g_outgoingReadIndex =
                (g_outgoingReadIndex + 1) % kMaxQueuedOutgoingMotion;
            --g_outgoingCount;
        }

        const std::size_t writeIndex =
            (g_outgoingReadIndex + g_outgoingCount) % kMaxQueuedOutgoingMotion;
        g_outgoingMotion[writeIndex] = packet;
        ++g_outgoingCount;
        return true;
    }

    bool TryPopOutgoingMotion(NetworkManager::MotionEventPacket& packet)
    {
        std::lock_guard<std::mutex> lock(g_outgoingMutex);
        if (g_outgoingCount == 0)
            return false;

        packet = g_outgoingMotion[g_outgoingReadIndex];
        g_outgoingReadIndex =
            (g_outgoingReadIndex + 1) % kMaxQueuedOutgoingMotion;
        --g_outgoingCount;
        return true;
    }

    void CaptureLocalMotion(
        void* controller,
        std::uint32_t motionHash,
        std::int32_t requestedVariant,
        std::int32_t parameterA,
        std::int32_t parameterB,
        std::uint8_t flagA)
    {
        // Hot-hook rule: no actor-state scans, no VirtualQuery loops, no logger,
        // no network mutex, and no blocking locks from inside sub_D1D840.
        if (!g_canSendMotionFast.load(std::memory_order_acquire) ||
            !IsLocalControllerFast(controller))
        {
            return;
        }

        const std::uint32_t actorNetId = g_localPlayerIdFast.load(std::memory_order_acquire);
        if (!actorNetId)
            return;

        NetworkManager::MotionEventPacket packet{};
        packet.ActorNetId = actorNetId;
        packet.MotionHash = motionHash;
        packet.RequestedVariant = requestedVariant;
        packet.ParameterA = parameterA;
        packet.ParameterB = parameterB;
        packet.SenderTick = static_cast<std::uint32_t>(GetTickCount());
        packet.PacketSequence = ++g_packetSequence;
        packet.FlagA = flagA;

        g_lastHash.store(packet.MotionHash, std::memory_order_release);
        g_lastSequence.store(packet.PacketSequence, std::memory_order_release);

        if (!TryQueueOutgoingMotion(packet))
            ++g_droppedPackets;
    }

    int __fastcall hMotionRequest(
        void* controller,
        void*,
        std::uint32_t motionHash,
        std::int32_t requestedVariant,
        std::int32_t parameterA,
        std::uint8_t flagA,
        uintptr_t localContext,
        std::int32_t parameterB)
    {
        CaptureLocalMotion(
            controller,
            motionHash,
            requestedVariant,
            parameterA,
            parameterB,
            flagA);

        return oMotionRequest(
            controller,
            motionHash,
            requestedVariant,
            parameterA,
            flagA,
            localContext,
            parameterB);
    }

    uintptr_t ResolveRemoteController(std::uint32_t actorNetId)
    {
        const uintptr_t localControllerSnapshot =
            g_localControllerFast.load(std::memory_order_acquire);

        const uintptr_t runtimeObject = RemotePlayerManager::GetProxyRuntimeObject(actorNetId);
        const uintptr_t actorStateComponent =
            RemotePlayerManager::GetProxyActorStateComponent(actorNetId);
        std::uint32_t actorStateHandle =
            RemotePlayerManager::GetProxyActorStateHandle(actorNetId);

        if (!runtimeObject)
        {
            SetStatus("Remote motion waiting for visible proxy");
            return 0;
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (g_lastRemoteController &&
                g_lastRemoteActorNetId == actorNetId &&
                g_lastRemoteRuntimeObject == runtimeObject &&
                g_lastRemoteController != localControllerSnapshot &&
                IsReadableObject(g_lastRemoteController))
            {
                return g_lastRemoteController;
            }
        }

        if (!actorStateHandle && actorStateComponent)
            SafeRead(actorStateComponent + kActorStateHandleOffset, actorStateHandle);

        // Verified direct path only. The old fallback scanned every actor-state
        // entry and hundreds of bytes inside entries, which is what caused the
        // multi-second freezes seen beside the old "controller resolved" log.
        if (actorStateHandle >= 2)
        {
            int directIndex = -1;
            const uintptr_t actorStateEntry = ResolveActorStateEntryFromHandle(
                actorStateHandle, &directIndex);
            const uintptr_t controller = actorStateEntry
                ? actorStateEntry + kActorStateControllerOffset
                : 0;

            if (controller &&
                controller != localControllerSnapshot &&
                IsReadableObject(controller))
            {
                uintptr_t queue = 0;
                std::uint32_t valueA8 = 0;
                std::uint32_t valueAC = 0;
                std::uint32_t valueB4 = 0;
                ReadControllerFields(controller, queue, valueA8, valueAC, valueB4);

                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_lastRemoteController = controller;
                    g_lastRemoteRuntimeObject = runtimeObject;
                    g_lastRemoteActorNetId = actorNetId;
                    g_remoteActorStateEntry = actorStateEntry;
                    g_remoteActorStateComponent = actorStateComponent;
                    g_remoteActorStateHandle = actorStateHandle;
                    g_remoteControllerQueue = queue;
                    g_remoteControllerA8 = valueA8;
                    g_remoteControllerAC = valueAC;
                    g_remoteControllerB4 = valueB4;
                    g_status = "Remote Component-7 motion controller cached";
                }

                g_remoteActorStateIndex.store(directIndex, std::memory_order_release);
                return controller;
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastRemoteControllerLog >= kDiagnosticLogIntervalMs)
        {
            g_lastRemoteControllerLog = now;
            Logger::WriteFormat(
                Logger::Level::Debug,
                "Remote motion controller unavailable: proxyObject=0x%08X c7=0x%08X stateHandle=0x%08X (full scans disabled)",
                static_cast<unsigned int>(runtimeObject),
                static_cast<unsigned int>(actorStateComponent),
                actorStateHandle);
        }

        SetStatus("Remote motion controller unavailable");
        return 0;
    }

    bool GuardedReplay(uintptr_t controller, const NetworkManager::MotionEventPacket& packet)
    {
        if (!oMotionRequest || !controller)
            return false;

        __try
        {
            oMotionRequest(
                reinterpret_cast<void*>(controller),
                packet.MotionHash,
                packet.RequestedVariant,
                packet.ParameterA,
                packet.FlagA,
                0,
                packet.ParameterB);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void FlushOutgoingMotion()
    {
        for (int processed = 0; processed < kMaxOutgoingMotionPerPump; ++processed)
        {
            NetworkManager::MotionEventPacket packet{};
            if (!TryPopOutgoingMotion(packet))
                break;

            if (NetworkManager::SendMotionEvent(packet))
            {
                ++g_sentPackets;
            }
            else
            {
                ++g_droppedPackets;
            }
        }
    }
}

namespace MotionReplication
{
    void Initialize()
    {
        if (g_hookInstalled.load(std::memory_order_acquire))
            return;

        if (!kEnableMotionHook)
        {
            SetStatus("Motion hook disabled");
            Logger::Write(
                Logger::Level::Warning,
                "Motion replication hook disabled by build-time safety switch");
            return;
        }

        const uintptr_t target = RuntimeAddress(kMotionRequestIda);
        if (!target)
        {
            SetStatus("Motion hook target address unavailable");
            Logger::Write(
                Logger::Level::Error,
                "Motion replication hook skipped: sub_D1D840 target address unavailable");
            return;
        }

        g_hookTarget = reinterpret_cast<void*>(target);

        MH_STATUS status = MH_CreateHook(
            g_hookTarget,
            reinterpret_cast<void*>(&hMotionRequest),
            reinterpret_cast<void**>(&oMotionRequest));
        if (status != MH_OK)
        {
            SetStatus("Motion hook create failed");
            Logger::WriteFormat(
                Logger::Level::Error,
                "Motion replication hook create failed: status=%d target=0x%08X",
                static_cast<int>(status),
                static_cast<unsigned int>(target));
            return;
        }

        status = MH_EnableHook(g_hookTarget);
        if (status != MH_OK)
        {
            SetStatus("Motion hook enable failed");
            Logger::WriteFormat(
                Logger::Level::Error,
                "Motion replication hook enable failed: status=%d target=0x%08X",
                static_cast<int>(status),
                static_cast<unsigned int>(target));
            MH_RemoveHook(g_hookTarget);
            g_hookTarget = nullptr;
            oMotionRequest = nullptr;
            return;
        }

        g_hookInstalled.store(true, std::memory_order_release);
        SetStatus("Motion hook installed - no-scan mode");
        Logger::WriteFormat(
            Logger::Level::Success,
            "Motion replication hook installed: sub_D1D840 runtime=0x%08X (no-scan hot path)",
            static_cast<unsigned int>(target));
    }

    void Shutdown()
    {
        if (!g_hookInstalled.exchange(false, std::memory_order_acq_rel))
            return;

        if (g_hookTarget)
        {
            MH_DisableHook(g_hookTarget);
            MH_RemoveHook(g_hookTarget);
        }

        {
            std::lock_guard<std::mutex> lock(g_outgoingMutex);
            g_outgoingReadIndex = 0;
            g_outgoingCount = 0;
        }

        g_localControllerFast.store(0, std::memory_order_release);
        g_localPlayerIdFast.store(0, std::memory_order_release);
        g_canSendMotionFast.store(false, std::memory_order_release);
        g_hookTarget = nullptr;
        oMotionRequest = nullptr;
        SetStatus("Motion hook stopped");
        Logger::Write("Motion replication hook stopped");
    }

    void PumpRemoteMotionOnGameThread()
    {
        if (!g_hookInstalled.load(std::memory_order_acquire) || !oMotionRequest)
            return;

        // All controller discovery and network work happens here instead of in
        // D1D840. The hook itself is now a constant-time pointer comparison and
        // a non-blocking queue attempt.
        RefreshNetworkFastState();
        RefreshLocalControllerCache();
        FlushOutgoingMotion();

        int processed = 0;
        NetworkManager::MotionEventPacket packet{};

        while (processed++ < kMaxIncomingMotionPerPump &&
               NetworkManager::PopMotionEvent(packet))
        {
            ++g_receivedPackets;
            g_lastHash.store(packet.MotionHash, std::memory_order_release);
            g_lastSequence.store(packet.PacketSequence, std::memory_order_release);

            if (packet.ActorNetId != 0 &&
                packet.ActorNetId == g_localPlayerIdFast.load(std::memory_order_acquire))
            {
                continue;
            }

            const uintptr_t controller = ResolveRemoteController(packet.ActorNetId);
            if (!controller)
            {
                ++g_droppedPackets;
                continue;
            }

            g_applyingRemoteMotion = true;
            const bool replayed = GuardedReplay(controller, packet);
            g_applyingRemoteMotion = false;

            if (replayed)
            {
                ++g_replayCount;
                SetStatus("Remote motion replayed");
            }
            else
            {
                ++g_droppedPackets;
                SetStatus("Remote motion replay failed");

                const ULONGLONG now = GetTickCount64();
                if (now - g_lastReplayDropLog >= 2000)
                {
                    g_lastReplayDropLog = now;
                    Logger::WriteFormat(
                        Logger::Level::Error,
                        "Motion replay failed actor=%u hash=0x%08X controller=0x%08X",
                        packet.ActorNetId,
                        packet.MotionHash,
                        static_cast<unsigned int>(controller));
                }
            }
        }
    }

    void InvalidateRemoteController(std::uint32_t actorNetId)
    {
        (void)actorNetId;
        g_remoteActorStateIndex.store(-1, std::memory_order_release);

        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_lastRemoteController = 0;
        g_lastRemoteRuntimeObject = 0;
        g_lastRemoteActorNetId = 0;
        g_remoteActorStateEntry = 0;
        g_remoteActorStateComponent = 0;
        g_remoteActorStateHandle = 0;
        g_remoteControllerQueue = 0;
        g_remoteControllerA8 = 0;
        g_remoteControllerAC = 0;
        g_remoteControllerB4 = 0;
        g_status = "Remote proxy spawned; waiting for Component-7 controller";
    }

    Stats GetStats()
    {
        Stats stats{};
        stats.HookInstalled = g_hookInstalled.load(std::memory_order_acquire);
        stats.LastHash = g_lastHash.load(std::memory_order_acquire);
        stats.LastSequence = g_lastSequence.load(std::memory_order_acquire);
        stats.SentPackets = g_sentPackets.load(std::memory_order_acquire);
        stats.ReceivedPackets = g_receivedPackets.load(std::memory_order_acquire);
        stats.DroppedPackets = g_droppedPackets.load(std::memory_order_acquire);
        stats.ReplayCount = g_replayCount.load(std::memory_order_acquire);
        stats.ControllerComponentIndex = -1;
        stats.LocalActorStateIndex = g_localActorStateIndex.load(std::memory_order_acquire);
        stats.RemoteActorStateIndex = g_remoteActorStateIndex.load(std::memory_order_acquire);

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            stats.LocalController = g_localController;
            stats.LastRemoteController = g_lastRemoteController;
            stats.LocalActorStateEntry = g_localActorStateEntry;
            stats.RemoteActorStateEntry = g_remoteActorStateEntry;
            stats.RemoteActorStateComponent = g_remoteActorStateComponent;
            stats.RemoteActorStateHandle = g_remoteActorStateHandle;
            stats.LocalControllerQueue = g_localControllerQueue;
            stats.RemoteControllerQueue = g_remoteControllerQueue;
            stats.LocalControllerA8 = g_localControllerA8;
            stats.LocalControllerAC = g_localControllerAC;
            stats.LocalControllerB4 = g_localControllerB4;
            stats.RemoteControllerA8 = g_remoteControllerA8;
            stats.RemoteControllerAC = g_remoteControllerAC;
            stats.RemoteControllerB4 = g_remoteControllerB4;
            stats.Status = g_status;
        }

        return stats;
    }
}
