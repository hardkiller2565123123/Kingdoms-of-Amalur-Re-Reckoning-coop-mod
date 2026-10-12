#pragma once

#include <cstdint>
#include <string>

namespace PlayerContextRuntime
{
    struct BindingResult
    {
        bool ContextFound = false;
        bool Bound = false;
        bool Activated = false;
        bool AdditionalStateApplied = false;
        uintptr_t PlayerContext = 0;
        std::uint32_t ExistingActorHandle = 0;
        std::string Status;
    };

    BindingResult BindRemoteActor(
        std::uint32_t gamePlayerIndex,
        std::uint32_t actorHandle,
        uintptr_t runtimeObject,
        std::uint32_t playerSetupValue);
}
