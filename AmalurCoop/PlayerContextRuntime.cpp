#include "PlayerContextRuntime.h"

#include "EngineRuntime.h"
#include "Logger.h"
#include "PositionTracker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <sstream>

namespace
{
#if defined(_WIN32)
    static_assert(sizeof(void*) == 4, "AmalurCoop Player context binding requires the Win32 game build.");
#endif

    constexpr uintptr_t kPlayerActorHandleOffset = 0x1EC;

    using BindActorFn = void(__thiscall*)(void*, std::uint32_t);
    using ActivatePlayerFn = void(__thiscall*)(void*, int);

    bool ReadPlayerMethod(uintptr_t playerContext, uintptr_t slotOffset, uintptr_t& methodAddress)
    {
        uintptr_t vtable = 0;
        return playerContext &&
            EngineRuntime::Read(playerContext, vtable) && vtable &&
            EngineRuntime::Read(vtable + slotOffset, methodAddress) && methodAddress;
    }

    bool CallBindAndActivate(
        uintptr_t playerContext,
        uintptr_t bindAddress,
        uintptr_t activateAddress,
        std::uint32_t actorHandle)
    {
        if (!playerContext || !bindAddress || !activateAddress || !actorHandle)
            return false;

        const auto bind = reinterpret_cast<BindActorFn>(bindAddress);
        const auto activate = reinterpret_cast<ActivatePlayerFn>(activateAddress);
        __try
        {
            bind(reinterpret_cast<void*>(playerContext), actorHandle);
            activate(reinterpret_cast<void*>(playerContext), 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace PlayerContextRuntime
{
    BindingResult BindRemoteActor(
        std::uint32_t gamePlayerIndex,
        std::uint32_t actorHandle,
        uintptr_t runtimeObject,
        std::uint32_t playerSetupValue)
    {
        BindingResult result{};
        (void)runtimeObject;
        (void)playerSetupValue;

        if (!actorHandle)
        {
            result.Status = "Remote actor is unavailable";
            return result;
        }

        // F7A270 is deliberately not called here. The raw assembly shows that
        // routine expects a specific engine-owned context layout and reads
        // fields at +0x30/+0x38. A spawned actor runtime object is not that
        // layout, so calling F7A270 on the actor can corrupt state.
        result.AdditionalStateApplied = false;

        if (gamePlayerIndex == 0)
        {
            result.Status = "No engine Player slot requested; unsafe raw F7A270 path skipped";
            return result;
        }

        result.PlayerContext = PositionTracker::FindPlayerContextByIndex(
            static_cast<int>(gamePlayerIndex));
        result.ContextFound = result.PlayerContext != 0;

        if (!result.PlayerContext)
        {
            std::ostringstream status;
            status << "No engine Player context for slot " << gamePlayerIndex
                << "; component-clone initialization will be used";
            result.Status = status.str();
            return result;
        }

        EngineRuntime::Read(
            result.PlayerContext + kPlayerActorHandleOffset,
            result.ExistingActorHandle);

        if (result.ExistingActorHandle && result.ExistingActorHandle != actorHandle)
        {
            std::ostringstream status;
            status << "Engine Player slot " << gamePlayerIndex
                << " is already occupied by actor 0x" << std::hex
                << result.ExistingActorHandle << "; left unchanged";
            result.Status = status.str();
            return result;
        }

        uintptr_t bindAddress = 0;
        uintptr_t activateAddress = 0;
        if (!ReadPlayerMethod(result.PlayerContext, 0x3C, bindAddress) ||
            !ReadPlayerMethod(result.PlayerContext, 0x20, activateAddress))
        {
            result.Status = "Engine Player context vtable is incomplete";
            return result;
        }

        if (!CallBindAndActivate(
                result.PlayerContext, bindAddress, activateAddress, actorHandle))
        {
            result.Status = "Engine Player bind/activate raised an exception";
            return result;
        }

        result.Bound = true;
        result.Activated = true;

        std::uint32_t verifiedHandle = 0;
        if (!EngineRuntime::Read(
                result.PlayerContext + kPlayerActorHandleOffset,
                verifiedHandle) ||
            verifiedHandle != actorHandle)
        {
            result.Bound = false;
            result.Activated = false;
            result.Status = "Engine Player bind did not retain the remote actor handle";
            return result;
        }

        result.Status = "Engine Player context bound/activated; raw F7A270 path skipped";

        Logger::WriteFormat(
            Logger::Level::Debug,
            "PlayerContextRuntime slot=%u context=0x%08X actor=0x%08X bound=%s active=%s F7A270=skipped",
            gamePlayerIndex,
            static_cast<unsigned int>(result.PlayerContext),
            actorHandle,
            result.Bound ? "yes" : "no",
            result.Activated ? "yes" : "no");

        return result;
    }
}
