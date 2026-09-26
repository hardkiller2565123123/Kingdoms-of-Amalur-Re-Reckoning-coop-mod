#include "PositionTracker.h"

#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <sstream>

namespace
{
    constexpr uintptr_t kIdaImageBase = 0x00400000;
    constexpr uintptr_t kPlayerRootGlobalIda = 0x019FD5E4;
    constexpr uintptr_t kRuntimeRootGlobalIda = 0x019FEC38;

    constexpr uintptr_t kPlayerManagerOffset = 0x164C;
    constexpr uintptr_t kPlayerContextObjectHandleOffset = 0x1EC;
    constexpr uintptr_t kPlayerSetupValueOffset = 0x2BC;
    constexpr uintptr_t kPlayerIndexOffset = 0x100;

    constexpr uintptr_t kObjectRegistryOffset = 0x2238;
    constexpr uintptr_t kObjectTableOffset = 0x0C;
    constexpr uintptr_t kHandleTableOffset = 0x1C;
    constexpr uintptr_t kObjectCountOffset = 0x20;
    constexpr uintptr_t kFallbackHandleOffset = 0x54;

    constexpr uintptr_t kObjectFlagsOffset = 0x10C;
    constexpr uintptr_t kActorDefinitionHandleOffset = 0xEC;
    constexpr uintptr_t kComponentArrayOffset = 0x3C;
    constexpr unsigned int kTransformComponentId = 6;

    constexpr uintptr_t kTransformFlagsOffset = 0x20;
    constexpr uintptr_t kTransformXOffset = 0x24;
    constexpr uintptr_t kTransformYOffset = 0x28;
    constexpr uintptr_t kTransformZOffset = 0x2C;

    std::atomic<bool> g_running{ false };
    std::mutex g_mutex;

    PositionTracker::Vec3 g_position{};
    uintptr_t g_playerRoot = 0;
    uintptr_t g_playerManager = 0;
    uintptr_t g_playerContext = 0;
    uint32_t g_objectHandle = 0;
    uint32_t g_definitionHandle = 0;
    uint32_t g_playerSetupValue = 0;
    uintptr_t g_runtimeObject = 0;
    uintptr_t g_transformComponent = 0;
    int g_confidence = 0;
    std::string g_status = "Not initialized";
    DWORD g_lastLogTick = 0;
    DWORD g_lastTitleTick = 0;

    uintptr_t RuntimeAddress(uintptr_t idaAddress)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return base ? base + (idaAddress - kIdaImageBase) : 0;
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

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (address + sizeof(T) > regionEnd)
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

    bool IsValidCoordinate(float value)
    {
        return std::isfinite(value) && value > -1000000.0f && value < 1000000.0f;
    }

    uintptr_t FindPlayerContext(uintptr_t manager, int playerIndex)
    {
        uintptr_t entries = 0;
        int count = 0;
        if (!SafeRead(manager + 0x4, entries) || !SafeRead(manager + 0x8, count))
            return 0;

        if (!entries || count <= 0 || count > 64)
            return 0;

        for (int i = 0; i < count; ++i)
        {
            uintptr_t entry = 0;
            int index = -1;
            if (!SafeRead(entries + static_cast<uintptr_t>(i) * 4, entry) || !entry)
                continue;

            if (SafeRead(entry + kPlayerIndexOffset, index) && index == playerIndex)
                return entry;
        }

        return 0;
    }

    uintptr_t ResolveObjectHandle(uintptr_t registry, uint32_t requestedHandle)
    {
        auto resolveOne = [registry](uint32_t handle) -> uintptr_t
        {
            if ((handle & 0x0FFF0000u) == 0)
                return 0;

            const uint16_t index = static_cast<uint16_t>(handle);
            uint32_t count = 0;
            uintptr_t handleTable = 0;
            uintptr_t objectTable = 0;

            if (!SafeRead(registry + kObjectCountOffset, count) || index >= count)
                return 0;
            if (!SafeRead(registry + kHandleTableOffset, handleTable) || !handleTable)
                return 0;
            if (!SafeRead(registry + kObjectTableOffset, objectTable) || !objectTable)
                return 0;

            uint32_t storedHandle = 0;
            if (!SafeRead(handleTable + static_cast<uintptr_t>(index) * 4, storedHandle) || storedHandle != handle)
                return 0;

            uintptr_t object = 0;
            if (!SafeRead(objectTable + static_cast<uintptr_t>(index) * 4, object))
                return 0;

            return object;
        };

        if (const uintptr_t object = resolveOne(requestedHandle))
            return object;

        uint32_t fallbackHandle = 0;
        if (SafeRead(registry + kFallbackHandleOffset, fallbackHandle))
            return resolveOne(fallbackHandle);

        return 0;
    }

    void PublishFailure(const char* status)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_position = {};
        g_playerContext = 0;
        g_objectHandle = 0;
        g_definitionHandle = 0;
        g_playerSetupValue = 0;
        g_runtimeObject = 0;
        g_transformComponent = 0;
        g_confidence = 0;
        g_status = status;
    }

    void UpdateConsoleTitle(const PositionTracker::Vec3& position)
    {
        const DWORD now = GetTickCount();
        if (now - g_lastTitleTick < 500)
            return;

        g_lastTitleTick = now;
        char title[256]{};
        sprintf_s(title, "AmalurCoop // X %.2f Y %.2f Z %.2f", position.X, position.Y, position.Z);
        SetConsoleTitleA(title);
    }
}

namespace PositionTracker
{
    void Initialize()
    {
        g_running.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = "Waiting for local player";
        }

        Logger::Write(Logger::Level::Success,
            "Verified player position tracker initialized (no gameplay detour, no float scan)");
    }

    void Shutdown()
    {
        g_running.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "Stopped";
        g_confidence = 0;
        Logger::Write("PositionTracker shutdown");
    }

    void Update()
    {
        if (!g_running.load(std::memory_order_acquire))
            return;

        const uintptr_t playerGlobalAddress = RuntimeAddress(kPlayerRootGlobalIda);
        const uintptr_t runtimeGlobalAddress = RuntimeAddress(kRuntimeRootGlobalIda);

        uintptr_t playerRoot = 0;
        uintptr_t runtimeRoot = 0;
        if (!SafeRead(playerGlobalAddress, playerRoot) || !playerRoot)
        {
            PublishFailure("Waiting for player root");
            return;
        }
        if (!SafeRead(runtimeGlobalAddress, runtimeRoot) || !runtimeRoot)
        {
            PublishFailure("Waiting for runtime root");
            return;
        }

        const uintptr_t manager = playerRoot + kPlayerManagerOffset;
        const uintptr_t context = FindPlayerContext(manager, 0);
        if (!context)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_playerRoot = playerRoot;
            g_playerManager = manager;
            g_status = "Waiting for player context";
            g_confidence = 0;
            return;
        }

        uint32_t objectHandle = 0;
        if (!SafeRead(context + kPlayerContextObjectHandleOffset, objectHandle) || !objectHandle)
        {
            PublishFailure("Player object handle unavailable");
            return;
        }

        const uintptr_t registry = runtimeRoot + kObjectRegistryOffset;
        const uintptr_t object = ResolveObjectHandle(registry, objectHandle);
        if (!object)
        {
            PublishFailure("Object handle did not resolve");
            return;
        }

        uint32_t definitionHandle = 0;
        uint32_t playerSetupValue = 0;
        SafeRead(object + kActorDefinitionHandleOffset, definitionHandle);
        SafeRead(context + kPlayerSetupValueOffset, playerSetupValue);

        uint8_t objectFlags = 0;
        if (!SafeRead(object + kObjectFlagsOffset, objectFlags) || (objectFlags & 1) == 0)
        {
            PublishFailure("Player runtime object is inactive");
            return;
        }

        uintptr_t transform = 0;
        if (!SafeRead(object + kComponentArrayOffset + kTransformComponentId * 4, transform) || !transform)
        {
            PublishFailure("Transform component unavailable");
            return;
        }

        uint8_t transformFlags = 0;
        Vec3 position{};
        if (!SafeRead(transform + kTransformFlagsOffset, transformFlags) || (transformFlags & 1) == 0 ||
            !SafeRead(transform + kTransformXOffset, position.X) ||
            !SafeRead(transform + kTransformYOffset, position.Y) ||
            !SafeRead(transform + kTransformZOffset, position.Z) ||
            !IsValidCoordinate(position.X) || !IsValidCoordinate(position.Y) || !IsValidCoordinate(position.Z))
        {
            PublishFailure("Transform data is not readable");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_playerRoot = playerRoot;
            g_playerManager = manager;
            g_playerContext = context;
            g_objectHandle = objectHandle;
            g_definitionHandle = definitionHandle;
            g_playerSetupValue = playerSetupValue;
            g_runtimeObject = object;
            g_transformComponent = transform;
            g_position = position;
            g_confidence = 100;
            g_status = "Verified runtime position";
        }

        UpdateConsoleTitle(position);

        const DWORD now = GetTickCount();
        if (now - g_lastLogTick >= 5000)
        {
            g_lastLogTick = now;
            Logger::WriteFormat(Logger::Level::Debug,
                "Player runtime: root=0x%08X manager=0x%08X context=0x%08X handle=0x%08X def=0x%08X setup=0x%08X object=0x%08X transform=0x%08X XYZ=(%.3f, %.3f, %.3f)",
                static_cast<unsigned int>(playerRoot),
                static_cast<unsigned int>(manager),
                static_cast<unsigned int>(context),
                objectHandle,
                definitionHandle,
                playerSetupValue,
                static_cast<unsigned int>(object),
                static_cast<unsigned int>(transform),
                position.X, position.Y, position.Z);
        }
    }

    bool HasPosition()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_confidence == 100 && g_transformComponent != 0;
    }

    Vec3 GetPosition()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_position;
    }

    int GetConfidence()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_confidence;
    }

    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    uintptr_t GetPlayerRoot() { std::lock_guard<std::mutex> lock(g_mutex); return g_playerRoot; }
    uintptr_t GetPlayerManager() { std::lock_guard<std::mutex> lock(g_mutex); return g_playerManager; }
    uintptr_t GetPlayerContext() { std::lock_guard<std::mutex> lock(g_mutex); return g_playerContext; }
    uint32_t GetObjectHandle() { std::lock_guard<std::mutex> lock(g_mutex); return g_objectHandle; }
    uint32_t GetDefinitionHandle() { std::lock_guard<std::mutex> lock(g_mutex); return g_definitionHandle; }
    uint32_t GetPlayerSetupValue() { std::lock_guard<std::mutex> lock(g_mutex); return g_playerSetupValue; }
    uintptr_t GetRuntimeObject() { std::lock_guard<std::mutex> lock(g_mutex); return g_runtimeObject; }
    uintptr_t GetTransformComponent() { std::lock_guard<std::mutex> lock(g_mutex); return g_transformComponent; }
}
