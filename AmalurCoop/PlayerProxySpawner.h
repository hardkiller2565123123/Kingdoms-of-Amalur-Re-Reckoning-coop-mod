#pragma once

#include <cstdint>
#include <string>

namespace PlayerProxySpawner
{
    struct SpawnOptions
    {
        std::uint32_t GamePlayerIndex = 0;
        std::uint32_t PreferredDefinitionHandle = 0;
        std::uint32_t PlayerSetupValue = 0;
        bool TryBindPlayerContext = false;
    };

    struct SpawnResult
    {
        bool Success = false;
        bool PlayerPostSetupApplied = false;
        bool ActorStateResolved = false;
        bool PlayerContextFound = false;
        bool PlayerContextBound = false;
        bool PlayerContextActivated = false;
        bool AdditionalPlayerStateApplied = false;
        bool ComponentCloneApplied = false;
        bool ComponentFinalizeApplied = false;
        bool LateActorFinalizeApplied = false;
        bool AppearanceRefreshApplied = false;

        std::uint32_t GamePlayerIndex = 0;
        std::uint32_t ObjectHandle = 0;
        std::uint32_t PersistentActorHandle = 0;
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t PlayerSetupValue = 0;
        std::uint32_t ActorStateHandle = 0;

        uintptr_t PlayerContext = 0;
        uintptr_t RuntimeObject = 0;
        uintptr_t TransformComponent = 0;
        uintptr_t ActorStateComponent = 0;
        uintptr_t EquipmentComponent = 0;
        uintptr_t AppearanceComponent = 0;
        uintptr_t RigSwapComponent = 0;
        uintptr_t DefinitionRecord = 0;
        uintptr_t DescriptorA = 0;
        uintptr_t DescriptorB = 0;
        uintptr_t ActorStateEntry = 0;

        std::string Status;
    };

    SpawnResult SpawnFromLocalPlayer();
    SpawnResult SpawnFromLocalPlayer(const SpawnOptions& options);
}
