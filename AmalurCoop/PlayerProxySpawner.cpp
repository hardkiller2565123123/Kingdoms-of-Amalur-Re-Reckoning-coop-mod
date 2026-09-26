#include "PlayerProxySpawner.h"

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
    constexpr uintptr_t kPlayerSetupValueOffset = 0x2BC;
    constexpr uintptr_t kActorStateHandleOffset = 0x9C;

    constexpr unsigned int kTransformComponentId = 6;
    constexpr unsigned int kActorStateComponentId = 7;
    constexpr unsigned int kAppearanceComponentId = 15;

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

    bool CallApplyPlayerTransform(ApplyPlayerTransformFn function, uintptr_t component, const void* descriptor)
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
        if (!EngineRuntime::Read(EngineRuntime::RuntimeAddress(kActorStateManagerGlobalIda), manager) || !manager)
            return 0;

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
        SpawnResult result{};

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

        // DD15C0 resolves this handle separately from the actor-definition handle
        // and passes both by address to FC6170. Supplying zero here was the last
        // major difference between the game's player path and the old proxy path.
        result.PersistentActorHandle = ResolvePlayerPersistentActorHandle(
            runtimeRoot, playerManager);
        if (result.PersistentActorHandle < 2)
        {
            result.Status = "Player persistent actor handle is unavailable";
            return result;
        }

        result.DefinitionHandle = EngineRuntime::GetActorDefinitionHandle(localRuntimeObject);
        if (result.DefinitionHandle < 2)
        {
            result.Status = "Local player definition handle is unavailable";
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

        void* definitionRecord = CallResolveManagedHandle(
            resolveManaged,
            reinterpret_cast<void*>(definitionManager),
            result.DefinitionHandle);

        result.DefinitionRecord = reinterpret_cast<uintptr_t>(definitionRecord);
        if (!result.DefinitionRecord)
        {
            result.Status = "Player definition record did not resolve";
            return result;
        }

        uintptr_t visualBlock = 0;
        if (!EngineRuntime::Read(
                result.DefinitionRecord + kDefinitionRecordVisualBlockOffset,
                visualBlock) ||
            !visualBlock)
        {
            result.Status = "Player visual descriptor block is unavailable";
            return result;
        }

        // Verified from the game's DDFFB1/DDFFBA player path (FA9570/FA9580).
        result.DescriptorA = visualBlock + 0x04;
        result.DescriptorB = visualBlock + 0x14;

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

        bool transformSetup = false;
        const auto applyTransform = reinterpret_cast<ApplyPlayerTransformFn>(
            EngineRuntime::RuntimeAddress(kApplyPlayerTransformIda));
        transformSetup = CallApplyPlayerTransform(
            applyTransform,
            result.TransformComponent,
            reinterpret_cast<const void*>(result.DescriptorA));

        bool stateComponentSetup = false;
        if (result.ActorStateComponent)
            stateComponentSetup = CallComponentStateInit(result.ActorStateComponent);

        // DD15C0 calls F7A270 only after the real player-context owner has
        // received the spawned actor through its vtable +0x3C/+0x20 methods.
        // The supplied F7A270 assembly then reads owner fields at +0x30/+0x38,
        // so it is not safe to call with a generic actor runtime pointer.
        // Remote proxies intentionally skip the local-player binding calls.
        EngineRuntime::Read(playerContext + kPlayerSetupValueOffset, result.PlayerSetupValue);

        if (result.ActorStateComponent)
        {
            EngineRuntime::Read(
                result.ActorStateComponent + kActorStateHandleOffset,
                result.ActorStateHandle);
        }

        result.ActorStateEntry = ResolveActorStateEntry(result.ActorStateHandle);
        result.ActorStateResolved = result.ActorStateEntry != 0;

        result.PlayerPostSetupApplied = transformSetup && stateComponentSetup;
        result.Success = true;

        std::ostringstream status;
        status << "Player-style proxy spawned";
        if (!result.ActorStateComponent)
            status << "; component 7 pending";
        if (!result.AppearanceComponent)
            status << "; component 15 pending";
        if (!result.PlayerPostSetupApplied)
            status << "; partial safe post-setup";
        if (!result.ActorStateResolved)
            status << "; actor-state entry pending";
        result.Status = status.str();

        Logger::WriteFormat(
            Logger::Level::Success,
            "Player proxy spawn: handle=0x%08X object=0x%08X persistent=0x%08X def=0x%08X descA=0x%08X descB=0x%08X c6=0x%08X c7=0x%08X c15=0x%08X stateHandle=0x%08X state=0x%08X safePost=%s stateResolved=%s",
            result.ObjectHandle,
            static_cast<unsigned int>(result.RuntimeObject),
            result.PersistentActorHandle,
            result.DefinitionHandle,
            static_cast<unsigned int>(result.DescriptorA),
            static_cast<unsigned int>(result.DescriptorB),
            static_cast<unsigned int>(result.TransformComponent),
            static_cast<unsigned int>(result.ActorStateComponent),
            static_cast<unsigned int>(result.AppearanceComponent),
            result.ActorStateHandle,
            static_cast<unsigned int>(result.ActorStateEntry),
            result.PlayerPostSetupApplied ? "yes" : "partial",
            result.ActorStateResolved ? "yes" : "pending");

        return result;
    }
}
