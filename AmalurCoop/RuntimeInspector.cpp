#include "RuntimeInspector.h"

#include "Logger.h"
#include "PositionTracker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr uintptr_t kIdaImageBase =
        0x00400000;

    constexpr uintptr_t kRuntimeRecordManagerGlobalIda =
        0x019F4DF0;

    constexpr uintptr_t kRuntimeRootGlobalIda =
        0x019FEC38;

    constexpr uintptr_t kQueueInstantiationIda =
        0x00EA5240;

    constexpr uintptr_t kManagerEntriesOffset =
        0x18;

    constexpr uintptr_t kManagerRecordCountOffset =
        0x5C;

    constexpr uintptr_t kManagerPendingEntriesOffset =
        0xCC;

    constexpr uintptr_t kManagerPendingCountOffset =
        0xD0;

    constexpr uintptr_t kRecordPersistentHandleOffset =
        0x18;

    constexpr uintptr_t kRecordObjectHandleOffset =
        0x50;

    constexpr uintptr_t kRecordRuntimeStateOffset =
        0x54;

    constexpr uintptr_t kRecordFlagsOffset =
        0x68;

    constexpr uintptr_t kRecordSpawnFlagsOffset =
        0x6A;

    constexpr uintptr_t kRecordDefinitionReferenceOffset =
        0x6C;

    constexpr uintptr_t kRuntimeRootObjectRegistryOffset =
        0x2238;

    constexpr uintptr_t kObjectTableOffset =
        0x0C;

    constexpr uintptr_t kHandleTableOffset =
        0x1C;

    constexpr uintptr_t kObjectCountOffset =
        0x20;

    constexpr uintptr_t kObjectFlagsOffset =
        0x10C;

    constexpr uintptr_t kRuntimeDefinitionRawOffset =
        0xEC;

    constexpr int kMinimumRecordIndex =
        2;

    constexpr int kMaximumReasonableRecordCount =
        65536;

    constexpr int kMaximumSnapshotReserve =
        4096;

    using QueueInstantiationFn =
        int(__thiscall*)(
            void* manager,
            void* runtimeRecord,
            int recordIndex,
            int worldId,
            int mode);

    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_refreshRequested{ false };

    std::mutex g_mutex;

    uintptr_t g_manager = 0;

    int g_recordCount = 0;
    int g_localRecordIndex = -1;

    uintptr_t g_localRecordAddress = 0;

    uint32_t g_localDefinitionId = 0;

    std::vector<RuntimeInspector::Record> g_records;

    std::string g_status =
        "Not initialized";

    std::string g_lastAction =
        "No runtime action requested";

    ULONGLONG g_lastRefreshTick = 0;

    uintptr_t RuntimeAddress(
        uintptr_t idaAddress)
    {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(
                GetModuleHandleW(nullptr));

        if (!base ||
            idaAddress < kIdaImageBase)
        {
            return 0;
        }

        return
            base +
            (idaAddress - kIdaImageBase);
    }

    bool IsReadableProtection(
        DWORD protection)
    {
        if ((protection & PAGE_GUARD) != 0 ||
            (protection & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD baseProtection =
            protection & 0xFFu;

        switch (baseProtection)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;

        default:
            return false;
        }
    }

    bool IsReadableRange(
        uintptr_t address,
        size_t size)
    {
        if (!address ||
            size == 0)
        {
            return false;
        }

        if (address >
            UINTPTR_MAX - size)
        {
            return false;
        }

        const uintptr_t requestedEnd =
            address + size;

        uintptr_t cursor =
            address;

        while (cursor < requestedEnd)
        {
            MEMORY_BASIC_INFORMATION info{};

            if (!VirtualQuery(
                reinterpret_cast<const void*>(
                    cursor),
                &info,
                sizeof(info)))
            {
                return false;
            }

            if (info.State != MEM_COMMIT ||
                !IsReadableProtection(
                    info.Protect))
            {
                return false;
            }

            const uintptr_t regionBase =
                reinterpret_cast<uintptr_t>(
                    info.BaseAddress);

            if (regionBase >
                UINTPTR_MAX - info.RegionSize)
            {
                return false;
            }

            const uintptr_t regionEnd =
                regionBase +
                info.RegionSize;

            if (regionEnd <= cursor)
                return false;

            cursor =
                std::min(
                    regionEnd,
                    requestedEnd);
        }

        return true;
    }

    template <typename T>
    bool SafeRead(
        uintptr_t address,
        T& value)
    {
        value = {};

        if (!IsReadableRange(
            address,
            sizeof(T)))
        {
            return false;
        }

        __try
        {
            value =
                *reinterpret_cast<const T*>(
                    address);

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            value = {};
            return false;
        }
    }

    /*
        Keep SEH in this small helper.

        This function deliberately contains no:
        - std::string
        - std::vector
        - std::ostringstream
        - std::lock_guard
        - objects with destructors

        That prevents MSVC error C2712.
    */
    bool GuardedQueueInstantiationCall(
        QueueInstantiationFn function,
        void* manager,
        void* runtimeRecord,
        int recordIndex,
        int worldId,
        int mode,
        int* outputResult)
    {
        if (outputResult)
            *outputResult = 0;

        if (!function ||
            !manager ||
            !runtimeRecord)
        {
            return false;
        }

        __try
        {
            const int result =
                function(
                    manager,
                    runtimeRecord,
                    recordIndex,
                    worldId,
                    mode);

            if (outputResult)
                *outputResult = result;

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (outputResult)
                *outputResult = 0;

            return false;
        }
    }

    uintptr_t ResolveStrictObject(
        uintptr_t registry,
        uint32_t handle)
    {
        if (!registry)
            return 0;

        if ((handle & 0x0FFF0000u) == 0)
            return 0;

        const uint16_t index =
            static_cast<uint16_t>(
                handle);

        uint32_t count = 0;

        uintptr_t handleTable = 0;
        uintptr_t objectTable = 0;

        if (!SafeRead(
            registry +
            kObjectCountOffset,
            count))
        {
            return 0;
        }

        if (index >= count)
            return 0;

        if (!SafeRead(
            registry +
            kHandleTableOffset,
            handleTable) ||
            !handleTable)
        {
            return 0;
        }

        if (!SafeRead(
            registry +
            kObjectTableOffset,
            objectTable) ||
            !objectTable)
        {
            return 0;
        }

        const uintptr_t handleEntryAddress =
            handleTable +
            static_cast<uintptr_t>(index) *
            sizeof(uint32_t);

        uint32_t storedHandle = 0;

        if (!SafeRead(
            handleEntryAddress,
            storedHandle))
        {
            return 0;
        }

        if (storedHandle != handle)
            return 0;

        const uintptr_t objectEntryAddress =
            objectTable +
            static_cast<uintptr_t>(index) *
            sizeof(uintptr_t);

        uintptr_t object = 0;

        if (!SafeRead(
            objectEntryAddress,
            object) ||
            !object)
        {
            return 0;
        }

        uint8_t flags = 0;

        if (!SafeRead(
            object +
            kObjectFlagsOffset,
            flags))
        {
            return 0;
        }

        if ((flags & 1u) == 0)
            return 0;

        return object;
    }

    bool IsPendingIndex(
        uintptr_t manager,
        int index)
    {
        if (!manager ||
            index < kMinimumRecordIndex)
        {
            return false;
        }

        uintptr_t entries = 0;
        int count = 0;

        if (!SafeRead(
            manager +
            kManagerPendingEntriesOffset,
            entries))
        {
            return false;
        }

        if (!SafeRead(
            manager +
            kManagerPendingCountOffset,
            count))
        {
            return false;
        }

        if (!entries ||
            count <= 0 ||
            count >
            kMaximumReasonableRecordCount)
        {
            return false;
        }

        for (int i = 0;
            i < count;
            ++i)
        {
            int pendingIndex = -1;

            const uintptr_t entryAddress =
                entries +
                static_cast<uintptr_t>(i) *
                sizeof(int);

            if (SafeRead(
                entryAddress,
                pendingIndex) &&
                pendingIndex == index)
            {
                return true;
            }
        }

        return false;
    }

    uint32_t ReadFallbackDefinitionId(
        uintptr_t definitionReference)
    {
        if (!definitionReference)
            return 0;

        uintptr_t first = 0;

        if (!SafeRead(
            definitionReference,
            first) ||
            !first)
        {
            return 0;
        }

        uint32_t definitionId = 0;

        if (!SafeRead(
            first,
            definitionId))
        {
            return 0;
        }

        return definitionId;
    }

    void ClearSnapshot(
        const std::string& status)
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        g_manager = 0;
        g_recordCount = 0;

        g_localRecordIndex = -1;
        g_localRecordAddress = 0;
        g_localDefinitionId = 0;

        g_records.clear();
        g_status = status;
    }

    void PerformRefresh()
    {
        const uintptr_t managerGlobalAddress =
            RuntimeAddress(
                kRuntimeRecordManagerGlobalIda);

        if (!managerGlobalAddress)
        {
            ClearSnapshot(
                "Runtime-record manager address is unavailable");

            return;
        }

        /*
            dword_19F4DF0 is used directly as a manager object by the
            researched game functions.

            Do not read another pointer from that address here. The runtime
            address itself is the manager base.
        */
        const uintptr_t manager =
            managerGlobalAddress;

        int count = 0;
        uintptr_t entries = 0;

        if (!SafeRead(
            manager +
            kManagerRecordCountOffset,
            count))
        {
            ClearSnapshot(
                "Waiting for runtime-record manager");

            return;
        }

        if (!SafeRead(
            manager +
            kManagerEntriesOffset,
            entries))
        {
            ClearSnapshot(
                "Runtime-record entry table is unavailable");

            return;
        }

        if (!entries ||
            count < kMinimumRecordIndex ||
            count >
            kMaximumReasonableRecordCount)
        {
            std::lock_guard<std::mutex> lock(
                g_mutex);

            g_manager = manager;
            g_recordCount = 0;

            g_localRecordIndex = -1;
            g_localRecordAddress = 0;
            g_localDefinitionId = 0;

            g_records.clear();

            g_status =
                "Runtime-record table is unavailable";

            return;
        }

        const uintptr_t runtimeRootAddress =
            RuntimeAddress(
                kRuntimeRootGlobalIda);

        /*
            dword_19FEC38 is also used as a runtime-root object directly in
            the researched functions.

            The object registry begins at runtimeRoot + 0x2238.
        */
        const uintptr_t registry =
            runtimeRootAddress
            ? runtimeRootAddress +
            kRuntimeRootObjectRegistryOffset
            : 0;

        const uint32_t localHandle =
            PositionTracker::GetObjectHandle();

        std::vector<RuntimeInspector::Record> records;

        records.reserve(
            static_cast<size_t>(
                std::min(
                    count,
                    kMaximumSnapshotReserve)));

        int localIndex = -1;
        uintptr_t localAddress = 0;
        uint32_t localDefinition = 0;

        for (int index =
            kMinimumRecordIndex;
            index < count;
            ++index)
        {
            const uintptr_t recordSlotAddress =
                entries +
                static_cast<uintptr_t>(index) *
                sizeof(uintptr_t);

            uintptr_t recordAddress = 0;

            if (!SafeRead(
                recordSlotAddress,
                recordAddress) ||
                !recordAddress)
            {
                continue;
            }

            RuntimeInspector::Record record{};

            record.Index =
                index;

            record.Address =
                recordAddress;

            SafeRead(
                recordAddress +
                kRecordPersistentHandleOffset,
                record.PersistentHandle);

            SafeRead(
                recordAddress +
                kRecordObjectHandleOffset,
                record.ObjectHandle);

            SafeRead(
                recordAddress +
                kRecordRuntimeStateOffset,
                record.RuntimeState);

            SafeRead(
                recordAddress +
                kRecordFlagsOffset,
                record.Flags);

            SafeRead(
                recordAddress +
                kRecordSpawnFlagsOffset,
                record.SpawnFlags);

            SafeRead(
                recordAddress +
                kRecordDefinitionReferenceOffset,
                record.DefinitionReference);

            record.DefinitionId =
                ReadFallbackDefinitionId(
                    record.DefinitionReference);

            record.RuntimeObject =
                ResolveStrictObject(
                    registry,
                    record.ObjectHandle);

            record.Instantiated =
                record.RuntimeObject != 0;

            record.Pending =
                IsPendingIndex(
                    manager,
                    index);

            if (record.RuntimeObject)
            {
                SafeRead(
                    record.RuntimeObject +
                    kRuntimeDefinitionRawOffset,
                    record.RuntimeDefinitionRaw);

                if (!record.DefinitionId)
                {
                    record.DefinitionId =
                        record.RuntimeDefinitionRaw;
                }
            }

            record.IsLocalPlayer =
                localHandle != 0 &&
                record.ObjectHandle ==
                localHandle;

            if (record.IsLocalPlayer)
            {
                localIndex =
                    index;

                localAddress =
                    recordAddress;

                localDefinition =
                    record.DefinitionId;
            }

            records.push_back(
                record);
        }

        for (RuntimeInspector::Record& record :
            records)
        {
            record.SameDefinitionAsLocal =
                localDefinition != 0 &&
                record.DefinitionId ==
                localDefinition;
        }

        std::ostringstream status;

        status
            << "Scanned "
            << records.size()
            << " records";

        if (localIndex >= 0)
        {
            status
                << "; local record #"
                << localIndex;
        }
        else
        {
            status
                << "; local record not found";
        }

        {
            std::lock_guard<std::mutex> lock(
                g_mutex);

            g_manager =
                manager;

            g_recordCount =
                count;

            g_localRecordIndex =
                localIndex;

            g_localRecordAddress =
                localAddress;

            g_localDefinitionId =
                localDefinition;

            g_records =
                std::move(records);

            g_status =
                status.str();
        }
    }

    void SetLastAction(
        const std::string& text)
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        g_lastAction =
            text;
    }
}

namespace RuntimeInspector
{
    void Initialize()
    {
        g_running.store(
            true,
            std::memory_order_release);

        g_refreshRequested.store(
            true,
            std::memory_order_release);

        g_lastRefreshTick = 0;

        Logger::Write(
            Logger::Level::Success,
            "Runtime-record inspector initialized "
            "(read-only by default)");
    }

    void Shutdown()
    {
        g_running.store(
            false,
            std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(
                g_mutex);

            g_manager = 0;
            g_recordCount = 0;

            g_localRecordIndex = -1;
            g_localRecordAddress = 0;
            g_localDefinitionId = 0;

            g_records.clear();

            g_status =
                "Stopped";

            g_lastAction =
                "Runtime inspector stopped";
        }

        Logger::Write(
            "RuntimeInspector shutdown");
    }

    void Update()
    {
        if (!g_running.load(
            std::memory_order_acquire))
        {
            return;
        }

        const ULONGLONG now =
            GetTickCount64();

        const bool manuallyRequested =
            g_refreshRequested.exchange(
                false,
                std::memory_order_acq_rel);

        const bool timerElapsed =
            g_lastRefreshTick == 0 ||
            now - g_lastRefreshTick >= 1000;

        if (!manuallyRequested &&
            !timerElapsed)
        {
            return;
        }

        g_lastRefreshTick =
            now;

        PerformRefresh();
    }

    void RefreshNow()
    {
        g_refreshRequested.store(
            true,
            std::memory_order_release);
    }

    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_status;
    }

    uintptr_t GetManager()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_manager;
    }

    int GetManagerRecordCount()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_recordCount;
    }

    int GetLocalRecordIndex()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_localRecordIndex;
    }

    uintptr_t GetLocalRecordAddress()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_localRecordAddress;
    }

    uint32_t GetLocalDefinitionId()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_localDefinitionId;
    }

    std::vector<Record> GetRecords()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_records;
    }

    std::string GetLastActionText()
    {
        std::lock_guard<std::mutex> lock(
            g_mutex);

        return g_lastAction;
    }

    bool QueueExistingRecord(
        int recordIndex,
        bool requireSameDefinition)
    {
        uintptr_t manager = 0;

        Record selected{};
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(
                g_mutex);

            manager =
                g_manager;

            for (const Record& record :
                g_records)
            {
                if (record.Index !=
                    recordIndex)
                {
                    continue;
                }

                selected =
                    record;

                found =
                    true;

                break;
            }
        }

        if (!found ||
            !manager)
        {
            SetLastAction(
                "Queue rejected: selected record "
                "is unavailable");

            return false;
        }

        if (selected.IsLocalPlayer)
        {
            SetLastAction(
                "Queue rejected: local-player "
                "record cannot be queued");

            return false;
        }

        if (!selected.Address)
        {
            SetLastAction(
                "Queue rejected: selected record "
                "address is null");

            return false;
        }

        if (selected.Instantiated ||
            selected.ObjectHandle != 0)
        {
            SetLastAction(
                "Queue rejected: record already "
                "has a runtime object");

            return false;
        }

        if ((selected.Flags & 0x3u) != 0)
        {
            SetLastAction(
                "Queue rejected: record flags "
                "indicate removed/blocked state");

            return false;
        }

        if (selected.Pending)
        {
            SetLastAction(
                "Queue rejected: record is "
                "already pending");

            return false;
        }

        if (requireSameDefinition &&
            !selected.SameDefinitionAsLocal)
        {
            SetLastAction(
                "Queue rejected: definition does "
                "not match local player");

            return false;
        }

        const uintptr_t functionAddress =
            RuntimeAddress(
                kQueueInstantiationIda);

        if (!functionAddress)
        {
            SetLastAction(
                "Queue rejected: sub_EA5240 "
                "address is unavailable");

            return false;
        }

        const auto function =
            reinterpret_cast<
            QueueInstantiationFn>(
                functionAddress);

        int result = 0;

        const bool callSucceeded =
            GuardedQueueInstantiationCall(
                function,
                reinterpret_cast<void*>(
                    manager),
                reinterpret_cast<void*>(
                    selected.Address),
                selected.Index,
                -1,
                1,
                &result);

        if (!callSucceeded)
        {
            SetLastAction(
                "Queue call raised an exception; "
                "automatic calls remain disabled");

            Logger::Write(
                Logger::Level::Error,
                "Experimental runtime-record queue "
                "call raised an exception");

            return false;
        }

        std::ostringstream message;

        message
            << "Queued record #"
            << selected.Index
            << " through sub_EA5240; result="
            << result;

        SetLastAction(
            message.str());

        Logger::Write(
            Logger::Level::Warning,
            message.str());

        RefreshNow();

        return true;
    }
}