#pragma once

#include <cstddef>
#include <cstdint>

namespace EngineRuntime
{
    uintptr_t RuntimeAddress(uintptr_t idaAddress);
    uintptr_t GetRuntimeRoot();
    uintptr_t ResolveActorHandle(std::uint32_t handle);
    uintptr_t GetComponent(uintptr_t runtimeObject, unsigned int componentId, bool requireActive = true);
    std::uint32_t GetActorDefinitionHandle(uintptr_t runtimeObject);
    bool IsActiveObject(uintptr_t runtimeObject);
    bool IsActiveComponent(uintptr_t component);

    bool ReadBytes(uintptr_t address, void* destination, std::size_t size);

    template <typename T>
    bool Read(uintptr_t address, T& value)
    {
        value = {};
        return ReadBytes(address, &value, sizeof(T));
    }
}
