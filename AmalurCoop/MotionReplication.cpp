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
#include <deque>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <cstring>
#include <cmath>
#include <map>

namespace
{
    constexpr uintptr_t kIdaImageBase = 0x00400000;
    constexpr uintptr_t kMotionRequestIda = 0x00D1D5D0;
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

    struct NativeBlend { float Values[6]{}; std::uint8_t Enabled = 0; std::uint8_t Padding[3]{}; };
    static_assert(sizeof(NativeBlend) == 28);
    // D1D5D0: ECX=rig asset, five stack arguments, RET 14h. These local
    // pointers and resource handles never cross the network.
    struct NativeRequest
    {
        std::uint32_t Owner = 0;
        int ResourceA = 0, ResourceB = 0;
        std::uint32_t Hash = 0, PreviousHash = 0;
        std::int32_t Variants[4]{ -1, -1, -1, -1 };
        float Weights[4]{ 1, 0, 0, 0 };
        std::uint32_t ActorHandle = 0, StartOffset = 0;
        float Speed = 1;
        NativeBlend Blend{};
        std::uint32_t Callback[2]{};
        int Catalog = 0, CatalogIndex = -1;
        std::uint8_t Flag = 0, Padding[3]{};
    };
    static_assert(sizeof(NativeRequest) == 0x70);
    static_assert(offsetof(NativeRequest, Catalog) == 0x64);
    using MotionRequestFn = void(__thiscall*)(void*, const NativeRequest*, int, int, void*, std::uint32_t*);
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

    // These are the only controller caches the clip-queue hook reads. They are populated on
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
    std::array<NetworkManager::MotionEventPacket, 8> g_latestMotion{}; // protected by outgoing mutex
    ULONGLONG g_nextStateResend = 0;
    struct AppliedMotion
    {
        uintptr_t Controller = 0;
        std::uint32_t Sequence = 0;
        NetworkManager::MotionEventPacket Packet{};
    };
    std::map<std::pair<std::uint32_t, std::uint8_t>, AppliedMotion> g_appliedMotion; // render thread only
    std::atomic<std::uint64_t> g_hookCalls{ 0 }, g_localRequests{ 0 };


    // Render-thread-only staging; network ingress timestamps prevent old attacks
    // from replaying after a long pause or delayed proxy creation.
    std::deque<NetworkManager::MotionEventPacket> g_pendingIncoming;
    ULONGLONG g_lastMotionSummary = 0;
    ULONGLONG g_nextLocalControllerRefresh = 0;
    ULONGLONG g_nextNetworkCacheRefresh = 0;
    ULONGLONG g_lastLocalControllerDiagnostic = 0;
    ULONGLONG g_lastRemoteControllerLog = 0;
    ULONGLONG g_lastReplayDropLog = 0;

    thread_local bool g_applyingRemoteMotion = false;

    uintptr_t RuntimeAddress(uintptr_t idaAddress)
    {
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return (base && idaAddress >= kIdaImageBase)
            ? base + (idaAddress - kIdaImageBase)
            : 0;
    }

    template <typename T>
    bool SafeRead(uintptr_t address, T& value)
    {
        value = {};
        return EngineRuntime::Read(address, value);
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

    void SeedDefaultMotion(uintptr_t controller);

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
        if (changed)
        {
            // Seed before publishing this controller to the hook, so an actual
            // first transition cannot be overwritten by our idle seed.
            g_localControllerFast.store(0, std::memory_order_release);
            SeedDefaultMotion(controller);
        }
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
            ClearLocalControllerCache("Waiting for current render controller");
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
            ClearLocalControllerCache("Waiting for current render controller");
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

        // Graph evaluation can enqueue the same clip every frame. Giving each
        // copy a new serial makes the receiver clear/restart it continuously.
        // Preserve the last serial until the actual clip/blend/track changes.
        const auto& previous = g_latestMotion[packet.Animation.Track];
        if (previous.MotionHash == packet.MotionHash &&
            previous.RequestedVariant == packet.RequestedVariant &&
            previous.ParameterA == packet.ParameterA && previous.ParameterB == packet.ParameterB && previous.FlagA == packet.FlagA &&
            std::memcmp(&previous.Animation, &packet.Animation, sizeof(packet.Animation)) == 0)
            return true;

        if (g_outgoingCount == kMaxQueuedOutgoingMotion)
        {
            g_outgoingReadIndex =
                (g_outgoingReadIndex + 1) % kMaxQueuedOutgoingMotion;
            --g_outgoingCount;
        }

        const std::size_t writeIndex =
            (g_outgoingReadIndex + g_outgoingCount) % kMaxQueuedOutgoingMotion;
        g_latestMotion[packet.Animation.Track] = packet;
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

    // Key conversion is bounded native registry lookup, with no asset loading.
    std::uint32_t ResourceKey(int handle, unsigned category)
    {
        if (handle < 2 || handle > 32767) return 0;
        std::uint32_t descriptor = static_cast<std::uint32_t>(handle) | (category << 16);
        using Key = std::uint32_t(__thiscall*)(void*);
        return reinterpret_cast<Key>(RuntimeAddress(0x00A6FE10))(&descriptor);
    }

    bool CaptureRequestValues(void* controller, int resourceA, int resourceB,
        const NativeBlend* blend, AmalurNet::AnimationRequest* out)
    {
        __try
        {
            out->RigKey = ResourceKey(*reinterpret_cast<int*>(
                reinterpret_cast<uintptr_t>(controller) + 0xA4), 7);
            out->ResourceKeyA = ResourceKey(resourceA, 8);
            out->ResourceKeyB = ResourceKey(resourceB, 8);
            if (!out->RigKey || (resourceA >= 2 && !out->ResourceKeyA) ||
                (resourceB >= 2 && !out->ResourceKeyB) || !blend) return false;
            // C8B3B0 initializes only Enabled. Other fields may be uninitialized
            // when disabled, so do not copy them into a packet.
            out->BlendEnabled = blend->Enabled != 0;
            if (out->BlendEnabled)
            {
                std::memcpy(out->Blend, blend->Values, sizeof(out->Blend));
                for (const auto value : out->Blend) if (!std::isfinite(value)) return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void SeedDefaultMotion(uintptr_t controller)
    {
        NetworkManager::MotionEventPacket packet{};
        NativeBlend blend{};
        if (CaptureRequestValues(reinterpret_cast<void*>(controller), 0, 0, &blend, &packet.Animation) &&
            SafeRead(controller + 0xB0, packet.MotionHash) && packet.MotionHash)
        {
            // D3F050's native default request. Seed a stationary newly joined
            // player even if its idle started before controller discovery.
            packet.RequestedVariant = -1;
            const float speed = 1;
            std::memcpy(&packet.ParameterA, &speed, sizeof(speed));
            packet.PacketSequence = ++g_packetSequence;
            packet.SenderTick = GetTickCount();
        }
        std::lock_guard<std::mutex> lock(g_outgoingMutex);
        // Do not overwrite an event captured concurrently after adoption.
        if (g_latestMotion[0].PacketSequence && packet.PacketSequence &&
            AmalurNet::IsNewer(g_latestMotion[0].PacketSequence, packet.PacketSequence)) return;
        g_outgoingReadIndex = g_outgoingCount = 0;
        g_latestMotion = {};
        g_latestMotion[packet.Animation.Track] = packet;
        g_nextStateResend = 0;
    }

    bool CaptureQueuedRequest(uintptr_t controller, const NativeRequest* request,
        uintptr_t queue, NetworkManager::MotionEventPacket* packet)
    {
        __try
        {
            const auto owner = *reinterpret_cast<std::uint32_t*>(controller + 0xA8);
            const auto tracks = *reinterpret_cast<uintptr_t*>(controller + 0x60);
            const int count = *reinterpret_cast<int*>(controller + 0x64);
            if (!owner || request->Owner != owner || request->ActorHandle != *reinterpret_cast<std::uint32_t*>(controller + 0xB4) || !tracks || count <= 0 || count > 64 || queue < tracks + 4) return false;
            const auto offset = queue - tracks - 4;
            if (offset % 0x5C || offset / 0x5C >= static_cast<unsigned>(count) || offset / 0x5C >= 8) return false;
            if (!CaptureRequestValues(reinterpret_cast<void*>(controller), request->ResourceA,
                request->ResourceB, &request->Blend, &packet->Animation)) return false;
            packet->MotionHash = request->Hash;
            packet->FlagA = request->Flag != 0;
            packet->Animation.StartOffset = request->StartOffset;
            packet->Animation.CatalogKey = ResourceKey(request->Catalog, 5);
            if (!packet->Animation.CatalogKey) return false;
            packet->Animation.Track = static_cast<std::uint8_t>(offset / 0x5C);
            std::memcpy(&packet->ParameterA, &request->Speed, sizeof(float));
            const int capacity = *reinterpret_cast<int*>(queue + 8);
            const int write = *reinterpret_cast<int*>(queue + 0x18);
            const int queued = *reinterpret_cast<int*>(queue + 0x1C);
            const auto entries = *reinterpret_cast<uintptr_t*>(queue + 4);
            if (!entries || capacity <= 0 || capacity > 4096 || write < 0 || write >= capacity || queued <= 0) return false;
            const auto node = *reinterpret_cast<uintptr_t*>(entries + ((write + capacity - 1) % capacity) * 4);
            if (!node) return false;
            // This is a rig state index, not a process handle. The evaluator
            // uses it for loop/transition semantics; forcing every clip to
            // state zero made locomotion use the wrong playback policy.
            packet->ParameterB = *reinterpret_cast<const int*>(node + 0x38);
            std::memcpy(packet->Animation.Variants, reinterpret_cast<void*>(node + 8), 16);
            std::memcpy(packet->Animation.Weights, reinterpret_cast<void*>(node + 0x18), 16);
            packet->Animation.SelectedVariants = 1;
            packet->RequestedVariant = packet->Animation.Variants[0];
            AmalurNet::Packet check{};
            check.MotionHash = packet->MotionHash; check.ParameterA = packet->ParameterA; check.ParameterB = packet->ParameterB;
            check.FlagA = packet->FlagA; check.RequestedVariant = packet->RequestedVariant;
            std::memcpy(check.VisualData, &packet->Animation, sizeof(packet->Animation));
            return AmalurNet::ValidAnimation(check);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void __fastcall hMotionRequest(void* rig, void*, const NativeRequest* request,
        int state, int transition, void* queue, std::uint32_t* output)
    {
        // Call through exactly once. Capture after selection, while the caller's
        // descriptor is still alive, so both peers use the same random variants.
        ++g_hookCalls;
        oMotionRequest(rig, request, state, transition, queue, output);
        if (g_applyingRemoteMotion) return;
        const auto controller = g_localControllerFast.load(std::memory_order_acquire);
        if (!controller) return;
        NetworkManager::MotionEventPacket packet{};
        if (!CaptureQueuedRequest(controller, request, reinterpret_cast<uintptr_t>(queue), &packet)) return;
        ++g_localRequests;
        packet.ActorNetId = g_localPlayerIdFast.load(std::memory_order_acquire);
        packet.PacketSequence = ++g_packetSequence;
        packet.SenderTick = GetTickCount();
        g_lastHash.store(packet.MotionHash, std::memory_order_release);
        g_lastSequence.store(packet.PacketSequence, std::memory_order_release);
        if (!TryQueueOutgoingMotion(packet)) ++g_droppedPackets;
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

        // Resolve the current C7 handle on every replay: a readable cached
        // controller can belong to a render tree which has already been replaced.
        actorStateHandle = 0;
        if (!EngineRuntime::IsActiveObject(runtimeObject) ||
            EngineRuntime::GetComponent(runtimeObject, 7) != actorStateComponent)
            return 0;
        if (actorStateComponent)
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

    // Resolve resources locally; the queue node takes its own optional references.
    int ResolveResource(unsigned category, std::uint32_t key)
    {
        if (!key) return 0;
        auto* manager = *reinterpret_cast<void**>(RuntimeAddress(
            category == 5 ? 0x019F4D44 : category == 7 ? 0x019F4D4C : 0x019F4D50));
        if (!manager) return -1;
        using Resolve = int(__thiscall*)(void*, std::uint32_t);
        using Lookup = void*(__thiscall*)(void*, int, int);
        const int handle = reinterpret_cast<Resolve>(RuntimeAddress(0x00A70B80))(manager, key);
        if (handle < 2 || !reinterpret_cast<Lookup>(RuntimeAddress(0x00A71E80))(manager, handle, 1)) return -1;
        const auto flags = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<uintptr_t>(manager) + 0x28);
        return (flags[handle] & 0x14) == 4 ? handle : -1;
    }

    bool GuardedReplay(uintptr_t controller, const NetworkManager::MotionEventPacket& packet,
        const NetworkManager::MotionEventPacket* previous)
    {
        if (!oMotionRequest || !controller || !packet.Animation.RigKey || packet.Animation.Track >= 8) return false;
        __try
        {
            NativeRequest request{};
            request.Owner = *reinterpret_cast<std::uint32_t*>(controller + 0xA8);
            request.ActorHandle = *reinterpret_cast<std::uint32_t*>(controller + 0xB4);
            if (!request.Owner || *reinterpret_cast<int*>(controller + 0xAC) < 2) return false;
            const int rig = ResolveResource(7, packet.Animation.RigKey);
            request.ResourceA = ResolveResource(8, packet.Animation.ResourceKeyA);
            request.ResourceB = ResolveResource(8, packet.Animation.ResourceKeyB);
            if (rig < 2 || request.ResourceA < 0 || request.ResourceB < 0) return false;
            const int currentRig = *reinterpret_cast<int*>(controller + 0xA4);
            if (currentRig >= 2 && currentRig != rig) return false;
            if (currentRig < 2)
            {
                using Bind = void(__thiscall*)(void*, const int*);
                reinterpret_cast<Bind>(RuntimeAddress(0x00CB45F0))(reinterpret_cast<void*>(controller), &rig);
            }
            using Lookup = void*(__thiscall*)(void*, int, int);
            void* manager = *reinterpret_cast<void**>(RuntimeAddress(0x019F4D4C));
            void* rigAsset = reinterpret_cast<Lookup>(RuntimeAddress(0x00A71E80))(manager, rig, 1);
            if (!rigAsset) return false;
            const auto rigAddress = reinterpret_cast<uintptr_t>(rigAsset);
            const int stateCount = *reinterpret_cast<const int*>(rigAddress + 4);
            if (packet.ParameterB < -1 || (packet.ParameterB >= 0 &&
                (stateCount <= packet.ParameterB || stateCount > 65536 ||
                 !*reinterpret_cast<uintptr_t*>(rigAddress + 8)))) return false;
            request.Hash = packet.MotionHash;
            request.StartOffset = packet.Animation.StartOffset;
            request.Flag = packet.FlagA;
            std::memcpy(&request.Speed, &packet.ParameterA, 4);
            request.Blend.Enabled = packet.Animation.BlendEnabled;
            std::memcpy(request.Blend.Values, packet.Animation.Blend, sizeof(request.Blend.Values));
            if (packet.Animation.SelectedVariants)
            {
                std::memcpy(request.Variants, packet.Animation.Variants, 16);
                std::memcpy(request.Weights, packet.Animation.Weights, 16);
            }
            // Resolve the clip against the receiver's model. Never trust a
            // sender's catalog handle/index or copy its callback pointers.
            if (packet.Animation.CatalogKey)
            {
                request.Catalog = ResolveResource(5, packet.Animation.CatalogKey);
                if (request.Catalog < 2) return false;
                // Only borrow catalogs already owned by this receiver's model.
                // Graph active-mask differences must not reject a valid clip,
                // but the queue must not outlive an unowned resource either.
                const int modelHandle = *reinterpret_cast<int*>(request.Owner + 0x198);
                using LookupModel = uintptr_t(__thiscall*)(void*, int);
                auto* modelManager = *reinterpret_cast<void**>(RuntimeAddress(0x019FDF54));
                const auto model = reinterpret_cast<LookupModel>(RuntimeAddress(0x00A6DF80))(modelManager, modelHandle);
                if (!model) return false;
                bool owned = *reinterpret_cast<int*>(request.Owner + 0x1B4) == request.Catalog;
                const int catalogCount = *reinterpret_cast<int*>(model + 0x190);
                const auto catalogs = *reinterpret_cast<const int**>(model + 0x18C);
                if (catalogCount < 0 || catalogCount > 4096 || (catalogCount && !catalogs)) return false;
                for (int i = 0; !owned && i < catalogCount; ++i) owned = catalogs[i] == request.Catalog;
                if (!owned) return false;
                auto* assets = *reinterpret_cast<void**>(RuntimeAddress(0x019F4D44));
                const auto catalog = reinterpret_cast<uintptr_t>(reinterpret_cast<Lookup>(RuntimeAddress(0x00A71E80))(assets, request.Catalog, 1));
                if (!catalog) return false;
                const int namesCount = *reinterpret_cast<int*>(catalog + 4);
                const auto names = *reinterpret_cast<const std::uint32_t**>(catalog + 8);
                const int entryCount = *reinterpret_cast<int*>(catalog + 0x10);
                const auto entries = *reinterpret_cast<uintptr_t*>(catalog + 0x14);
                if (!names || !entries || namesCount <= 0 || namesCount > 65536 || entryCount < namesCount) return false;
                for (int i = 0; i < namesCount; ++i)
                    if (names[i] == request.Hash) { request.CatalogIndex = i; break; }
                if (request.CatalogIndex < 0) return false;
                const int variants = *reinterpret_cast<int*>(entries + request.CatalogIndex * 16);
                if (variants <= 0 || variants > 65536) return false;
                for (int variant : request.Variants) if (variant < -1 || variant >= variants) return false;
            }
            else
            {
                using ResolveClip = bool(__thiscall*)(void*, NativeRequest*);
                if (!reinterpret_cast<ResolveClip>(RuntimeAddress(0x00CE71D0))(rigAsset, &request)) return false;
            }
            // A queued clip alone is insufficient: D464D6 skips D44790 when
            // the owner's catalog activation counts are all zero. Visual-only
            // loading does not restore these transient counts. Use the native
            // activation function so its resource-cache bookkeeping is balanced.
            {
                using LookupModel = uintptr_t(__thiscall*)(void*, int);
                auto* modelManager = *reinterpret_cast<void**>(RuntimeAddress(0x019FDF54));
                const int modelHandle = *reinterpret_cast<int*>(request.Owner + 0x198);
                const auto model = reinterpret_cast<LookupModel>(RuntimeAddress(0x00A6DF80))(modelManager, modelHandle);
                if (!model) return false;
                const int count = *reinterpret_cast<int*>(model + 0x190);
                const auto catalogs = *reinterpret_cast<const int**>(model + 0x18C);
                const int activeCount = *reinterpret_cast<int*>(request.Owner + 0x1A8);
                const auto active = *reinterpret_cast<const std::uint8_t**>(request.Owner + 0x1A4);
                if (count <= 0 || count > 4096 || activeCount < count || !catalogs || !active) return false;
                int index = -1;
                for (int i = 0; i < count; ++i)
                    if (catalogs[i] == request.Catalog) { index = i; break; }
                if (index < 0) return false;
                if (!active[index])
                {
                    using ActivateCatalog = void(__thiscall*)(void*, const int*, bool);
                    reinterpret_cast<ActivateCatalog>(RuntimeAddress(0x00CCB840))(
                        reinterpret_cast<void*>(request.Owner), &request.Catalog, false);
                    if (!active[index]) return false;
                    Logger::WriteFormat(Logger::Level::Debug,
                        "Remote animation catalog activated actor=%u catalog=0x%08X slot=%d",
                        packet.ActorNetId, packet.Animation.CatalogKey, index);
                }
            }
            int count = *reinterpret_cast<int*>(controller + 0x64);
            if (count < 0 || count > 64) return false;
            while (count <= packet.Animation.Track)
            {
                using CreateTrack = void(__thiscall*)(void*, int);
                reinterpret_cast<CreateTrack>(RuntimeAddress(0x00D3EBF0))(reinterpret_cast<void*>(controller), count);
                const int next = *reinterpret_cast<int*>(controller + 0x64);
                if (next <= count || next > 64) return false;
                count = next;
            }
            const auto tracks = *reinterpret_cast<uintptr_t*>(controller + 0x60);
            if (!tracks) return false;
            // Mirror D3ECC5/D3ECD1 and D1D856/D1D862. D32260 sets
            // these controller summary flags at initialization; bypassing the
            // normal request wrapper must also perform its flag transition.
            *reinterpret_cast<std::uint16_t*>(controller + 0xB8) &= static_cast<std::uint16_t>(~0x18u);
            const auto queue = tracks + packet.Animation.Track * 0x5C + 4;
            // D405C0 consumes the queue into controller+14's active clip list.
            // Updating only queued nodes restarts already-playing blend clips.
            // D0F380 maps catalog/index/variant to +34/+60/+64, group to +5C,
            // speed to +74 and variant weight to +78. Only reuse the complete
            // current group after an unchanged request identity; never seek it.
            if (previous && previous->MotionHash == packet.MotionHash &&
                previous->ParameterB == packet.ParameterB && previous->FlagA == packet.FlagA &&
                previous->Animation.RigKey == packet.Animation.RigKey &&
                previous->Animation.ResourceKeyA == packet.Animation.ResourceKeyA &&
                previous->Animation.ResourceKeyB == packet.Animation.ResourceKeyB &&
                previous->Animation.CatalogKey == packet.Animation.CatalogKey &&
                previous->Animation.StartOffset == packet.Animation.StartOffset &&
                previous->Animation.SelectedVariants && packet.Animation.SelectedVariants &&
                previous->Animation.BlendEnabled == packet.Animation.BlendEnabled &&
                !std::memcmp(previous->Animation.Blend, packet.Animation.Blend, sizeof(packet.Animation.Blend)) &&
                !std::memcmp(previous->Animation.Variants, packet.Animation.Variants, sizeof(packet.Animation.Variants)) &&
                *reinterpret_cast<int*>(queue + 0x1C) == 0)
            {
                const auto track = queue - 4;
                const int groups = *reinterpret_cast<int*>(track + 0x3C);
                const auto groupIds = *reinterpret_cast<const int**>(track + 0x38);
                const int activeCount = *reinterpret_cast<int*>(controller + 0x2C);
                const auto activeClips = *reinterpret_cast<const uintptr_t**>(controller + 0x28);
                if (groups > 0 && groups <= 4096 && groupIds && activeCount > 0 && activeCount <= 4096 && activeClips)
                {
                    const int group = groupIds[groups - 1];
                    uintptr_t matches[4]{};
                    bool complete = group >= 0;
                    unsigned expected = 0, found = 0;
                    for (int variant : request.Variants) if (variant >= 0) ++expected;
                    for (int i = 0; complete && i < activeCount; ++i)
                    {
                        const auto clip = activeClips[i];
                        if (!clip || *reinterpret_cast<int*>(clip + 0x5C) != group) continue;
                        int slot = -1;
                        for (int v = 0; v < 4; ++v)
                            if (request.Variants[v] >= 0 && request.Variants[v] == *reinterpret_cast<int*>(clip + 0x64)) { slot = v; break; }
                        if (slot < 0 || matches[slot] || (*reinterpret_cast<unsigned short*>(clip + 0x80) & 2) ||
                            *reinterpret_cast<int*>(clip + 0x34) != request.Catalog ||
                            *reinterpret_cast<int*>(clip + 0x60) != request.CatalogIndex ||
                            *reinterpret_cast<int*>(clip + 0x38) != request.ResourceA ||
                            *reinterpret_cast<int*>(clip + 0x3C) != request.ResourceB) { complete = false; break; }
                        matches[slot] = clip; ++found;
                    }
                    if (complete && expected && found == expected)
                    {
                        for (int v = 0; v < 4; ++v) if (matches[v])
                        {
                            *reinterpret_cast<float*>(matches[v] + 0x74) = request.Speed;
                            *reinterpret_cast<float*>(matches[v] + 0x78) = request.Weights[v];
                        }
                        return true;
                    }
                }
            }
            // Blend weights/speed can change each frame without changing the
            // clip. Update the sole current queue node in place so its playback
            // clock survives. Only reuse an exact catalog/variant/resource match.
            const int queued = *reinterpret_cast<int*>(queue + 0x1C);
            const int capacity = *reinterpret_cast<int*>(queue + 8);
            const int read = *reinterpret_cast<int*>(queue + 0x14);
            const auto slots = *reinterpret_cast<uintptr_t*>(queue + 4);
            if (queued == 1 && slots && capacity > 0 && capacity <= 4096 && read >= 0 && read < capacity)
            {
                const auto node = *reinterpret_cast<uintptr_t*>(slots + read * 4);
                if (node && *reinterpret_cast<int*>(node + 0x38) == packet.ParameterB &&
                    *reinterpret_cast<int*>(node + 4) == request.CatalogIndex &&
                    *reinterpret_cast<int*>(node + 0x2C) == request.Catalog &&
                    *reinterpret_cast<int*>(node + 0x30) == request.ResourceA &&
                    *reinterpret_cast<int*>(node + 0x34) == request.ResourceB &&
                    *reinterpret_cast<std::uint32_t*>(node + 0x40) == request.StartOffset &&
                    *reinterpret_cast<std::uint8_t*>(node + 0x68) == request.Flag &&
                    !std::memcmp(reinterpret_cast<void*>(node + 8), request.Variants, 16))
                {
                    std::memcpy(reinterpret_cast<void*>(node + 0x18), request.Weights, 16);
                    std::memcpy(reinterpret_cast<void*>(node + 0x28), &request.Speed, 4);
                    std::memcpy(reinterpret_cast<void*>(node + 0x44), &request.Blend, sizeof(request.Blend));
                    return true;
                }
            }
            using Clear = void(__thiscall*)(void*);
            reinterpret_cast<Clear>(RuntimeAddress(0x00CE6910))(reinterpret_cast<void*>(queue));
            // D1D5D0 retains its own optional resource references. Our stack
            // descriptor borrows handles and must not retain/release them again.
            oMotionRequest(rigAsset, &request, packet.ParameterB, -1, reinterpret_cast<void*>(queue), nullptr);
            *reinterpret_cast<std::uint8_t*>(queue + 0x20) |= 1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void FlushOutgoingMotion()
    {
        if (!g_canSendMotionFast.load(std::memory_order_acquire)) return;
        bool sent = false;
        for (int processed = 0; processed < kMaxOutgoingMotionPerPump; ++processed)
        {
            NetworkManager::MotionEventPacket packet{};
            if (!TryPopOutgoingMotion(packet)) break;
            if (GetTickCount() - packet.SenderTick > 750) continue;
            if (NetworkManager::SendMotionEvent(packet)) { ++g_sentPackets; sent = true; }
            else
            {
                // Network worker is busy: retry this event next frame instead
                // of blocking Present or losing a short attack transition.
                std::lock_guard<std::mutex> lock(g_outgoingMutex);
                if (g_outgoingCount < kMaxQueuedOutgoingMotion)
                {
                    g_outgoingReadIndex = (g_outgoingReadIndex + kMaxQueuedOutgoingMotion - 1) % kMaxQueuedOutgoingMotion;
                    g_outgoingMotion[g_outgoingReadIndex] = packet; ++g_outgoingCount;
                }
                else ++g_droppedPackets;
                break;
            }
        }
        const auto now = GetTickCount64();
        if (sent) g_nextStateResend = now + 1000;
        if (now < g_nextStateResend) return;
        g_nextStateResend = now + 1000;
        std::array<NetworkManager::MotionEventPacket, 8> latest{};
        {
            std::lock_guard<std::mutex> lock(g_outgoingMutex);
            latest = g_latestMotion;
        }
        for (const auto& state : latest)
            if (state.MotionHash && NetworkManager::SendMotionEvent(state)) ++g_sentPackets;
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
                "Motion replication hook skipped: sub_D1D5D0 target address unavailable");
            return;
        }

        const std::uint8_t expected[]{ 0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x18 };
        std::uint8_t actual[sizeof(expected)]{};
        if (!EngineRuntime::ReadBytes(target, actual, sizeof(actual)) ||
            std::memcmp(actual, expected, sizeof(actual)))
        {
            SetStatus("Animation ABI signature mismatch");
            Logger::Write(Logger::Level::Error, "D1D5D0 signature mismatch; animation hook not installed");
            return;
        }
        const std::uint8_t catalogExpected[]{ 0x83, 0xEC, 0x08, 0x53, 0x55, 0x8B, 0xE9 };
        std::uint8_t catalogActual[sizeof(catalogExpected)]{};
        if (!EngineRuntime::ReadBytes(RuntimeAddress(0x00CCB840), catalogActual, sizeof(catalogActual)) ||
            std::memcmp(catalogActual, catalogExpected, sizeof(catalogActual)))
        {
            SetStatus("Catalog activation ABI mismatch");
            Logger::Write(Logger::Level::Error, "CCB840 signature mismatch; animation hook not installed");
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
            "Motion replication hook installed: sub_D1D5D0 runtime=0x%08X (no-scan hot path)",
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
            g_latestMotion = {};
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
        // D1D5D0. The hook does an owner comparison, bounded resource-key
        // lookups for the local player only, and a non-blocking queue attempt.
        RefreshNetworkFastState();
        RefreshLocalControllerCache();
        FlushOutgoingMotion();

        NetworkManager::MotionEventPacket packet{};
        const auto now = GetTickCount64();
        for (unsigned read = 0; read < 32 && NetworkManager::PopMotionEvent(packet); ++read)
        {
            ++g_receivedPackets;
            if (!packet.ReceivedAtMs) packet.ReceivedAtMs = now;
            if (g_pendingIncoming.size() >= 128) { g_pendingIncoming.pop_front(); ++g_droppedPackets; }
            g_pendingIncoming.push_back(packet);
        }
        const auto attempts = (std::min)(g_pendingIncoming.size(), static_cast<std::size_t>(kMaxIncomingMotionPerPump));
        for (std::size_t processed = 0; processed < attempts; ++processed)
        {
            packet = g_pendingIncoming.front(); g_pendingIncoming.pop_front();
            if (now - packet.ReceivedAtMs > 750) { ++g_droppedPackets; continue; }
            g_lastHash.store(packet.MotionHash, std::memory_order_release);
            g_lastSequence.store(packet.PacketSequence, std::memory_order_release);

            if (packet.ActorNetId != 0 &&
                packet.ActorNetId == g_localPlayerIdFast.load(std::memory_order_acquire))
            {
                continue;
            }

            const auto proxy = RemotePlayerManager::GetProxyDiagnostics(packet.ActorNetId);
            if (!proxy.Connected) { ++g_droppedPackets; continue; }
            const uintptr_t controller = proxy.AppearanceRefreshApplied ? ResolveRemoteController(packet.ActorNetId) : 0;
            if (!controller)
            {
                g_pendingIncoming.push_back(packet);
                continue;
            }

            const auto applied = g_appliedMotion.find({ packet.ActorNetId, packet.Animation.Track });
            if (applied != g_appliedMotion.end() && applied->second.Controller == controller &&
                (packet.PacketSequence == applied->second.Sequence ||
                 !AmalurNet::IsNewer(packet.PacketSequence, applied->second.Sequence))) continue;

            g_applyingRemoteMotion = true;
            const auto* previous = applied != g_appliedMotion.end() && applied->second.Controller == controller
                ? &applied->second.Packet : nullptr;
            const bool replayed = GuardedReplay(controller, packet, previous);
            g_applyingRemoteMotion = false;

            if (replayed)
            {
                g_appliedMotion[{ packet.ActorNetId, packet.Animation.Track }] = { controller, packet.PacketSequence, packet };
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
                        Logger::Level::Warning,
                        "Motion replay failed actor=%u hash=0x%08X controller=0x%08X",
                        packet.ActorNetId,
                        packet.MotionHash,
                        static_cast<unsigned int>(controller));
                }
            }
        }
        if (now - g_lastMotionSummary >= 5000)
        {
            g_lastMotionSummary = now;
            Logger::WriteFormat(Logger::Level::Debug,
                "Motion counters sent=%llu received=%llu replayed=%llu dropped=%llu pending=%u local=0x%08X hook=%llu localRequests=%llu",
                g_sentPackets.load(), g_receivedPackets.load(), g_replayCount.load(), g_droppedPackets.load(),
                static_cast<unsigned>(g_pendingIncoming.size()), static_cast<unsigned>(g_localControllerFast.load()), g_hookCalls.load(), g_localRequests.load());
        }
    }

    void InvalidateRemoteController(std::uint32_t actorNetId)
    {
        for (auto it = g_appliedMotion.begin(); it != g_appliedMotion.end();)
            if (it->first.first == actorNetId) it = g_appliedMotion.erase(it); else ++it;
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

    void ResetForWorldTransition()
    {
        g_pendingIncoming.clear();
        g_appliedMotion.clear();
        g_nextStateResend = 0;
        g_localControllerFast.store(0, std::memory_order_release);
        g_localPlayerIdFast.store(0, std::memory_order_release);
        g_canSendMotionFast.store(false, std::memory_order_release);
        g_localActorStateIndex.store(-1, std::memory_order_release);
        g_remoteActorStateIndex.store(-1, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(g_outgoingMutex);
            g_outgoingReadIndex = 0;
            g_outgoingCount = 0;
            g_latestMotion = {};
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_localRuntimeObject = 0;
            g_localController = 0;
            g_lastRemoteController = 0;
            g_lastRemoteRuntimeObject = 0;
            g_lastRemoteActorNetId = 0;
            g_localActorStateEntry = 0;
            g_remoteActorStateEntry = 0;
            g_remoteActorStateComponent = 0;
            g_remoteActorStateHandle = 0;
            g_localControllerQueue = 0;
            g_remoteControllerQueue = 0;
            g_localControllerA8 = 0;
            g_localControllerAC = 0;
            g_localControllerB4 = 0;
            g_remoteControllerA8 = 0;
            g_remoteControllerAC = 0;
            g_remoteControllerB4 = 0;
            g_status = "World transition - motion controller caches cleared";
        }

        g_nextLocalControllerRefresh = 0;
        g_nextNetworkCacheRefresh = 0;

        // Drop old incoming animation events. They reference the previous
        // world's actor/controller graph and must never be replayed after load.
        NetworkManager::MotionEventPacket packet{};
        int drained = 0;
        while (drained++ < 256 && NetworkManager::PopMotionEvent(packet))
        {
        }
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
