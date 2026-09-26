#pragma once

#include <cstdint>
#include <string>

namespace PlayerProxySpawner
{
    struct SpawnResult
    {
        bool Success = false;
        bool PlayerPostSetupApplied = false;
        bool ActorStateResolved = false;

        std::uint32_t ObjectHandle = 0;
        std::uint32_t PersistentActorHandle = 0;
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t PlayerSetupValue = 0;
        std::uint32_t ActorStateHandle = 0;

        uintptr_t RuntimeObject = 0;
        uintptr_t TransformComponent = 0;
        uintptr_t ActorStateComponent = 0;
        uintptr_t AppearanceComponent = 0;
        uintptr_t DefinitionRecord = 0;
        uintptr_t DescriptorA = 0;
        uintptr_t DescriptorB = 0;
        uintptr_t ActorStateEntry = 0;

        std::string Status;
    };

    SpawnResult SpawnFromLocalPlayer();
}
