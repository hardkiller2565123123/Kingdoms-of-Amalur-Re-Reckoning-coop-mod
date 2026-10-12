#include "PlayerProxySpawner.h"

#include "EngineRuntime.h"
#include "Logger.h"
#include "PlayerContextRuntime.h"
#include "PlayerAppearanceRuntime.h"
#include "PositionTracker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <sstream>

namespace
{
#if defined(_WIN32)
    static_assert(sizeof(void*) == 4, "AmalurCoop player proxy spawning requires the Win32 game build.");
#endif

    constexpr uintptr_t kDefinitionManagerGlobalIda = 0x019F4DF8;
    constexpr uintptr_t kActorStateManagerGlobalIda = 0x019FDF54;

    constexpr uintptr_t kPlayerPersistentKeyIda = 0x00B4C080;
    constexpr uintptr_t kResolvePersistentActorHandleIda = 0x00A70B80;
    constexpr uintptr_t kResolveManagedHandleIda = 0x00A71E80;
    constexpr uintptr_t kSpawnActorIda = 0x00FC6170;
    constexpr uintptr_t kApplyPlayerTransformIda = 0x00F6A660;
    constexpr uintptr_t kResolveActorStateIda = 0x00C8AD60;

    constexpr uintptr_t kRuntimePersistentActorManagerOffset = 0x1754;
    constexpr uintptr_t kDefinitionRecordVisualBlockOffset = 0x33C;
    constexpr uintptr_t kActorStateHandleOffset = 0x9C;

    constexpr unsigned int kTransformComponentId = 6;
    constexpr unsigned int kActorStateComponentId = 7;
    constexpr unsigned int kAppearanceComponentId = 7;

    using GetPlayerPersistentKeyFn = std::uint32_t(__thiscall*)(void*);
    using ResolvePersistentActorHandleFn = std::uint32_t(__thiscall*)(void*, std::uint32_t);
    using ResolveManagedHandleFn = void*(__thiscall*)(void*, std::uint32_t, int);
    using SpawnActorFn = int*(__thiscall*)(
        void* runtimeRoot,
        int* outActorHandle,
        int* persistentActorHandle,
        int* definitionHandle,
        const void* descriptorA,
        const void* descriptorB,
        int arg6,
        int arg7,
        int arg8,
        int arg9);
    using ApplyPlayerTransformFn = void(__thiscall*)(void*, const void*);
    using ComponentStateInitFn = void(__thiscall*)(void*, int*);
    using ResolveActorStateFn = void*(__thiscall*)(void*, const std::uint32_t*);

    std::uint32_t ResolvePlayerPersistentActorHandle(uintptr_t runtimeRoot, uintptr_t playerManager)
    {
        if (!runtimeRoot || !playerManager)
            return 0;

        const auto getKey = reinterpret_cast<GetPlayerPersistentKeyFn>(
            EngineRuntime::RuntimeAddress(kPlayerPersistentKeyIda));
        const auto resolveHandle = reinterpret_cast<ResolvePersistentActorHandleFn>(
            EngineRuntime::RuntimeAddress(kResolvePersistentActorHandleIda));
        if (!getKey || !resolveHandle)
            return 0;

        std::uint32_t key = 0;
        std::uint32_t handle = 0;
        __try
        {
            key = getKey(reinterpret_cast<void*>(playerManager));
            if (key)
            {
                handle = resolveHandle(
                    reinterpret_cast<void*>(runtimeRoot + kRuntimePersistentActorManagerOffset),
                    key);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }

        return handle >= 2 ? handle : 0;
    }

    void* CallResolveManagedHandle(
        ResolveManagedHandleFn function,
        void* manager,
        std::uint32_t handle)
    {
        if (!function || !manager || handle < 2)
            return nullptr;

        void* result = nullptr;
        __try
        {
            result = function(manager, handle, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = nullptr;
        }
        return result;
    }

    bool ResolveDefinition(
        ResolveManagedHandleFn resolveManaged,
        uintptr_t definitionManager,
        std::uint32_t definitionHandle,
        uintptr_t& definitionRecord,
        uintptr_t& descriptorA,
        uintptr_t& descriptorB)
    {
        definitionRecord = 0;
        descriptorA = 0;
        descriptorB = 0;

        void* record = CallResolveManagedHandle(
            resolveManaged,
            reinterpret_cast<void*>(definitionManager),
            definitionHandle);
        if (!record)
            return false;

        definitionRecord = reinterpret_cast<uintptr_t>(record);
        uintptr_t visualBlock = 0;
        if (!EngineRuntime::Read(
                definitionRecord + kDefinitionRecordVisualBlockOffset,
                visualBlock) ||
            !visualBlock)
        {
            return false;
        }

        descriptorA = visualBlock + 0x04;
        descriptorB = visualBlock + 0x14;
        return true;
    }

    bool CallSpawnActor(
        SpawnActorFn function,
        void* runtimeRoot,
        int* outActorHandle,
        int* persistentActorHandle,
        int* definitionHandle,
        const void* descriptorA,
        const void* descriptorB)
    {
        if (!function || !runtimeRoot || !outActorHandle || !persistentActorHandle ||
            !definitionHandle || !descriptorA || !descriptorB)
        {
            return false;
        }

        __try
        {
            int* result = function(
                runtimeRoot,
                outActorHandle,
                persistentActorHandle,
                definitionHandle,
                descriptorA,
                descriptorB,
                0,
                0,
                0,
                0);
            return result != nullptr && *outActorHandle != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallApplyPlayerTransform(
        ApplyPlayerTransformFn function,
        uintptr_t component,
        const void* descriptor)
    {
        if (!function || !component || !descriptor)
            return false;

        __try
        {
            function(reinterpret_cast<void*>(component), descriptor);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallComponentStateInit(uintptr_t component)
    {
        if (!component)
            return false;

        uintptr_t vtable = 0;
        uintptr_t methodAddress = 0;
        if (!EngineRuntime::Read(component, vtable) || !vtable ||
            !EngineRuntime::Read(vtable + 0x34, methodAddress) || !methodAddress)
        {
            return false;
        }

        auto method = reinterpret_cast<ComponentStateInitFn>(methodAddress);
        int state = 0;
        __try
        {
            method(reinterpret_cast<void*>(component), &state);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t ResolveActorStateEntry(std::uint32_t actorStateHandle)
    {
        if (actorStateHandle < 2)
            return 0;

        uintptr_t manager = 0;
        if (!EngineRuntime::Read(
                EngineRuntime::RuntimeAddress(kActorStateManagerGlobalIda),
                manager) ||
            !manager)
        {
            return 0;
        }

        const auto resolve = reinterpret_cast<ResolveActorStateFn>(
            EngineRuntime::RuntimeAddress(kResolveActorStateIda));
        if (!resolve)
            return 0;

        void* entry = nullptr;
        __try
        {
            entry = resolve(reinterpret_cast<void*>(manager), &actorStateHandle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            entry = nullptr;
        }
        return reinterpret_cast<uintptr_t>(entry);
    }
}

namespace PlayerProxySpawner
{
    SpawnResult SpawnFromLocalPlayer()
    {
        return SpawnFromLocalPlayer(SpawnOptions{});
    }

    SpawnResult SpawnFromLocalPlayer(const SpawnOptions& options)
    {
        SpawnResult result{};
        result.GamePlayerIndex = options.GamePlayerIndex;

        const uintptr_t playerManager = PositionTracker::GetPlayerManager();
        const uintptr_t playerContext = PositionTracker::GetPlayerContext();
        const uintptr_t localRuntimeObject = PositionTracker::GetRuntimeObject();
        const std::uint32_t localObjectHandle = PositionTracker::GetObjectHandle();
        const uintptr_t runtimeRoot = EngineRuntime::GetRuntimeRoot();

        if (!playerManager || !playerContext || !localRuntimeObject || !localObjectHandle || !runtimeRoot)
        {
            result.Status = "Local player runtime is not ready";
            return result;
        }

        result.PersistentActorHandle = ResolvePlayerPersistentActorHandle(
            runtimeRoot, playerManager);
        if (result.PersistentActorHandle < 2)
        {
            result.Status = "Player persistent actor handle is unavailable";
            return result;
        }

        uintptr_t definitionManager = 0;
        if (!EngineRuntime::Read(
                EngineRuntime::RuntimeAddress(kDefinitionManagerGlobalIda),
                definitionManager) ||
            !definitionManager)
        {
            result.Status = "Player definition manager is unavailable";
            return result;
        }

        const auto resolveManaged = reinterpret_cast<ResolveManagedHandleFn>(
            EngineRuntime::RuntimeAddress(kResolveManagedHandleIda));
        if (!resolveManaged)
        {
            result.Status = "Definition resolver is unavailable";
            return result;
        }

        // A remote definition handle is accepted only when it resolves through
        // this process's definition manager and contains the expected visual
        // descriptor block. Otherwise use the verified local player definition.
        std::uint32_t chosenDefinition = options.PreferredDefinitionHandle;
        if (chosenDefinition >= 2 && ResolveDefinition(
                resolveManaged,
                definitionManager,
                chosenDefinition,
                result.DefinitionRecord,
                result.DescriptorA,
                result.DescriptorB))
        {
            result.DefinitionHandle = chosenDefinition;
        }
        else
        {
            chosenDefinition = EngineRuntime::GetActorDefinitionHandle(localRuntimeObject);
            if (chosenDefinition < 2 || !ResolveDefinition(
                    resolveManaged,
                    definitionManager,
                    chosenDefinition,
                    result.DefinitionRecord,
                    result.DescriptorA,
                    result.DescriptorB))
            {
                result.Status = "Local player definition/visual descriptors are unavailable";
                return result;
            }
            result.DefinitionHandle = chosenDefinition;
        }

        int outActorHandle = 0;
        int persistentActorHandle = static_cast<int>(result.PersistentActorHandle);
        int definitionHandle = static_cast<int>(result.DefinitionHandle);
        const auto spawn = reinterpret_cast<SpawnActorFn>(
            EngineRuntime::RuntimeAddress(kSpawnActorIda));

        if (!CallSpawnActor(
                spawn,
                reinterpret_cast<void*>(runtimeRoot),
                &outActorHandle,
                &persistentActorHandle,
                &definitionHandle,
                reinterpret_cast<const void*>(result.DescriptorA),
                reinterpret_cast<const void*>(result.DescriptorB)))
        {
            result.Status = "Player-style FC6170 spawn failed";
            return result;
        }

        result.ObjectHandle = static_cast<std::uint32_t>(outActorHandle);
        if (!result.ObjectHandle || result.ObjectHandle == localObjectHandle)
        {
            result.Status = "Spawn returned an invalid/local player handle";
            return result;
        }

        result.RuntimeObject = EngineRuntime::ResolveActorHandle(result.ObjectHandle);
        if (!result.RuntimeObject || !EngineRuntime::IsActiveObject(result.RuntimeObject))
        {
            result.Status = "Spawned actor did not resolve as an active runtime object";
            return result;
        }

        result.TransformComponent = EngineRuntime::GetComponent(
            result.RuntimeObject, kTransformComponentId, true);
        result.ActorStateComponent = EngineRuntime::GetComponent(
            result.RuntimeObject, kActorStateComponentId, true);
        result.AppearanceComponent = EngineRuntime::GetComponent(
            result.RuntimeObject, kAppearanceComponentId, true);

        if (!result.TransformComponent)
        {
            result.Status = "Spawned actor has no active transform component";
            return result;
        }

        const auto applyTransform = reinterpret_cast<ApplyPlayerTransformFn>(
            EngineRuntime::RuntimeAddress(kApplyPlayerTransformIda));
        const bool transformSetup = CallApplyPlayerTransform(
            applyTransform,
            result.TransformComponent,
            reinterpret_cast<const void*>(result.DescriptorA));

        bool stateComponentSetup = false;
        if (result.ActorStateComponent)
            stateComponentSetup = CallComponentStateInit(result.ActorStateComponent);

        result.PlayerSetupValue = options.PlayerSetupValue
            ? options.PlayerSetupValue
            : PositionTracker::GetPlayerSetupValue();

        PlayerContextRuntime::BindingResult binding{};
        if (options.TryBindPlayerContext)
        {
            binding = PlayerContextRuntime::BindRemoteActor(
                options.GamePlayerIndex,
                result.ObjectHandle,
                result.RuntimeObject,
                result.PlayerSetupValue);
        }
        else
        {
            binding.Status = "No engine Player slot requested; raw F7A270 path skipped";
        }

        result.PlayerContext = binding.PlayerContext;
        result.PlayerContextFound = binding.ContextFound;
        result.PlayerContextBound = binding.Bound;
        result.PlayerContextActivated = binding.Activated;
        result.AdditionalPlayerStateApplied = false;

        // FC6170 -> F86E20 already completed the native actor registration and
        // F323A0 finalization. Peer rendering is applied later from portable
        // asset-keyed archives; local inventory and physics are never copied.
        result.LateActorFinalizeApplied = true;
        const auto appearance = PlayerAppearanceRuntime::RefreshRemotePlayer(
            result.ObjectHandle,
            result.RuntimeObject);
        result.EquipmentComponent = appearance.EquipmentComponent;
        result.AppearanceComponent = appearance.AppearanceComponent
            ? appearance.AppearanceComponent
            : result.AppearanceComponent;
        result.RigSwapComponent = appearance.RigSwapComponent;
        result.ComponentCloneApplied = appearance.ComponentsCopied;
        result.ComponentFinalizeApplied = appearance.ComponentsFinalized;
        result.AppearanceRefreshApplied = appearance.InitializationApplied;

        if (result.ActorStateComponent)
        {
            EngineRuntime::Read(
                result.ActorStateComponent + kActorStateHandleOffset,
                result.ActorStateHandle);
        }

        result.ActorStateEntry = ResolveActorStateEntry(result.ActorStateHandle);
        result.ActorStateResolved = result.ActorStateEntry != 0;

        result.PlayerPostSetupApplied =
            transformSetup &&
            stateComponentSetup &&
            result.LateActorFinalizeApplied;
        result.Success = true;

        std::ostringstream status;
        status << "Player-style proxy spawned";
        if (options.TryBindPlayerContext)
        {
            if (result.PlayerContextBound)
                status << "; engine Player slot " << options.GamePlayerIndex << " bound";
            else if (!binding.Status.empty())
                status << "; " << binding.Status;
        }
        if (!result.ActorStateComponent)
            status << "; component 7 pending";
        if (!result.AppearanceComponent)
            status << "; PartRendering pending";
        if (!result.AppearanceRefreshApplied)
            status << "; player visual rebuild pending";
        if (!result.ActorStateResolved)
            status << "; actor-state entry pending";
        result.Status = status.str();

        Logger::WriteFormat(
            Logger::Level::Success,
            "Player proxy spawn: netSlot=%u handle=0x%08X object=0x%08X persistent=0x%08X def=0x%08X c6=0x%08X c7=0x%08X inventoryC9=0x%08X renderingC7=0x%08X motionC42=0x%08X visualImport=%s visualActivate=%s nativeFinalize=%s visualRebuild=%s playerCtx=0x%08X bound=%s stateHandle=0x%08X state=0x%08X",
            result.GamePlayerIndex,
            result.ObjectHandle,
            static_cast<unsigned int>(result.RuntimeObject),
            result.PersistentActorHandle,
            result.DefinitionHandle,
            static_cast<unsigned int>(result.TransformComponent),
            static_cast<unsigned int>(result.ActorStateComponent),
            static_cast<unsigned int>(result.EquipmentComponent),
            static_cast<unsigned int>(result.AppearanceComponent),
            static_cast<unsigned int>(result.RigSwapComponent),
            result.ComponentCloneApplied ? "yes" : "no",
            result.ComponentFinalizeApplied ? "yes" : "no",
            result.LateActorFinalizeApplied ? "yes" : "no",
            result.AppearanceRefreshApplied ? "yes" : "no",
            static_cast<unsigned int>(result.PlayerContext),
            result.PlayerContextBound ? "yes" : "no",
            result.ActorStateHandle,
            static_cast<unsigned int>(result.ActorStateEntry));

        return result;
    }
}
