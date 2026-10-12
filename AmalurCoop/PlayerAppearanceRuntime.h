#pragma once

#include <cstdint>
#include <string>

namespace PlayerAppearanceRuntime
{
    struct RefreshResult
    {
        bool Attempted = false;
        bool SourceReady = false;
        bool ComponentsCopied = false;
        bool ComponentsFinalized = false;
        bool LateActorFinalizeApplied = false;
        bool EngineRefreshApplied = false;
        bool PositionRestored = false;
        bool AppearanceComponentFound = false;
        bool InitializationApplied = false;

        std::uint32_t SourceActorHandle = 0;
        uintptr_t SourceRuntimeObject = 0;
        uintptr_t SourceEquipmentComponent = 0;
        uintptr_t SourceAppearanceComponent = 0;
        uintptr_t SourceRigSwapComponent = 0;

        uintptr_t EquipmentComponent = 0;
        uintptr_t AppearanceComponent = 0;
        uintptr_t RigSwapComponent = 0;

        std::string Status;
    };

    RefreshResult RefreshRemotePlayer(std::uint32_t actorHandle, uintptr_t runtimeObject);
}
