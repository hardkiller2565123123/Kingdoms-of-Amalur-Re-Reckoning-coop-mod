#include "EngineRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr uintptr_t kIdaImageBase = 0x00400000;
    constexpr uintptr_t kRuntimeRootGlobalIda = 0x019FEC38;

    constexpr uintptr_t kObjectRegistryOffset = 0x2238;
    constexpr uintptr_t kObjectTableOffset = 0x0C;
    constexpr uintptr_t kHandleTableOffset = 0x1C;
    constexpr uintptr_t kObjectCountOffset = 0x20;
    constexpr uintptr_t kObjectFlagsOffset = 0x10C;
    constexpr uintptr_t kComponentArrayOffset = 0x3C;
    constexpr uintptr_t kComponentFlagsOffset = 0x20;
    constexpr uintptr_t kActorDefinitionHandleOffset = 0xEC;

    bool IsReadableProtection(DWORD protect)
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;

        const DWORD readable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (protect & readable) != 0;
    }
}

namespace EngineRuntime
{
    uintptr_t RuntimeAddress(uintptr_t idaAddress)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return (base && idaAddress >= kIdaImageBase)
            ? base + (idaAddress - kIdaImageBase)
            : 0;
    }

    bool ReadBytes(uintptr_t address, void* destination, std::size_t size)
    {
        if (!address || !destination || !size)
            return false;

        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || !IsReadableProtection(info.Protect))
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (address > regionEnd || size > regionEnd - address)
            return false;

        __try
        {
            std::memcpy(destination, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::memset(destination, 0, size);
            return false;
        }
    }

    uintptr_t GetRuntimeRoot()
    {
        uintptr_t root = 0;
        Read(RuntimeAddress(kRuntimeRootGlobalIda), root);
        return root;
    }

    uintptr_t ResolveActorHandle(std::uint32_t handle)
    {
        if ((handle & 0x0FFF0000u) == 0)
            return 0;

        const uintptr_t root = GetRuntimeRoot();
        if (!root)
            return 0;

        const uintptr_t registry = root + kObjectRegistryOffset;
        const std::uint16_t index = static_cast<std::uint16_t>(handle);

        std::uint32_t count = 0;
        uintptr_t handleTable = 0;
        uintptr_t objectTable = 0;
        std::uint32_t storedHandle = 0;
        uintptr_t object = 0;

        if (!Read(registry + kObjectCountOffset, count) || index >= count ||
            !Read(registry + kHandleTableOffset, handleTable) || !handleTable ||
            !Read(registry + kObjectTableOffset, objectTable) || !objectTable ||
            !Read(handleTable + static_cast<uintptr_t>(index) * sizeof(std::uint32_t), storedHandle) ||
            storedHandle != handle ||
            !Read(objectTable + static_cast<uintptr_t>(index) * sizeof(uintptr_t), object))
        {
            return 0;
        }

        return object;
    }

    bool IsActiveObject(uintptr_t runtimeObject)
    {
        std::uint16_t flags = 0;
        return runtimeObject && Read(runtimeObject + kObjectFlagsOffset, flags) && (flags & 1) != 0;
    }

    bool IsActiveComponent(uintptr_t component)
    {
        std::uint8_t flags = 0;
        return component && Read(component + kComponentFlagsOffset, flags) && (flags & 1) != 0;
    }

    uintptr_t GetComponent(uintptr_t runtimeObject, unsigned int componentId, bool requireActive)
    {
        if (!runtimeObject)
            return 0;

        uintptr_t component = 0;
        if (!Read(
                runtimeObject + kComponentArrayOffset +
                    static_cast<uintptr_t>(componentId) * sizeof(uintptr_t),
                component) ||
            !component)
        {
            return 0;
        }

        if (requireActive && !IsActiveComponent(component))
            return 0;

        return component;
    }

    std::uint32_t GetActorDefinitionHandle(uintptr_t runtimeObject)
    {
        std::uint32_t handle = 0;
        if (!runtimeObject || !Read(runtimeObject + kActorDefinitionHandleOffset, handle))
            return 0;
        return handle;
    }
}
