#include "PlayerAppearanceRuntime.h"
#include "EngineRuntime.h"

namespace PlayerAppearanceRuntime
{
    RefreshResult RefreshRemotePlayer(std::uint32_t actorHandle, uintptr_t runtimeObject)
    {
        RefreshResult result{};
        if (!actorHandle || EngineRuntime::ResolveActorHandle(actorHandle) != runtimeObject ||
            !EngineRuntime::IsActiveObject(runtimeObject))
        {
            result.Status = "Remote actor is not active";
            return result;
        }
        // Factory FA5D80 proves C9=inventory, C15=physics, C42=motion.
        // None is a clothing importer. Never copy local inventory/physics into
        // a remote actor. PlayerLifecycleSync applies the peer's render archive.
        result.EquipmentComponent = EngineRuntime::GetComponent(runtimeObject, 9);
        result.AppearanceComponent = EngineRuntime::GetComponent(runtimeObject, 7);
        result.RigSwapComponent = EngineRuntime::GetComponent(runtimeObject, 42);
        result.AppearanceComponentFound = result.AppearanceComponent != 0;
        result.LateActorFinalizeApplied = true;
        result.Status = "Independent actor ready; waiting for peer rendering archive";
        return result;
    }
}
