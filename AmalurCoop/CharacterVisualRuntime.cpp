#include "CharacterVisualRuntime.h"
#include "CharacterVisualArchive.h"
#include "EngineRuntime.h"
#include "PositionTracker.h"
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <set>

namespace
{
    struct Archive
    {
        void** Vtable = nullptr;
        void* DynamicBuffer = nullptr;
        std::uint8_t* Buffer = nullptr;
        std::uint32_t Size = 0;
        std::uint32_t Offset = 0;
        std::uint32_t* Chunks = nullptr;
        std::uint32_t Depth = 0;
        std::uint32_t Capacity = 0;
        std::uint16_t Allocator = 0;
        std::int16_t Growth = -1;
    };
    static_assert(sizeof(void*) == 4);
    static_assert(offsetof(Archive, Offset) == 0x10);
    static_assert(offsetof(Archive, Chunks) == 0x14);
    void __fastcall ReadArchive(Archive* stream, void*, void* destination, std::uint32_t count)
    {
        if (stream->Offset > stream->Size || count > stream->Size - stream->Offset)
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        std::memcpy(destination, stream->Buffer + stream->Offset, count);
        stream->Offset += count;
    }
    constexpr unsigned TextureOffsets[]{ 0x2C, 0x34, 0x38 };
    constexpr unsigned ColorOffsets[]{ 0x30, 0x5C, 0x3C, 0x48, 0x40, 0x44, 0x4C, 0x50, 0x54, 0x58 };
    struct Materials { std::uint32_t Keys[3]{}; std::uint32_t Colors[10]{}; };
    static_assert(sizeof(Materials) == 52);
    bool CaptureMaterials(uintptr_t stats, Materials* result)
    {
        uintptr_t vt = 0;
        if (!EngineRuntime::Read(stats, vt) || vt != EngineRuntime::RuntimeAddress(0x017562E4)) return false;
        __try
        {
            using Key = std::uint32_t(__thiscall*)(void*);
            for (unsigned i = 0; i < 3; ++i)
            {
                const auto handle = *reinterpret_cast<int*>(stats + TextureOffsets[i]);
                if (handle >= 2)
                {
                    if (handle > 32767) return false;
                    std::uint32_t descriptor = static_cast<std::uint32_t>(handle) | (3u << 16);
                    result->Keys[i] = reinterpret_cast<Key>(EngineRuntime::RuntimeAddress(0x00A6FE10))(&descriptor);
                    if (!result->Keys[i]) return false;
                }
            }
            for (unsigned i = 0; i < 10; ++i)
                result->Colors[i] = *reinterpret_cast<std::uint32_t*>(stats + ColorOffsets[i]);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ResolveMaterials(const Materials* materials, int* handles)
    {
        __try
        {
            auto* manager = *reinterpret_cast<void**>(EngineRuntime::RuntimeAddress(0x019F4D3C));
            if (!manager) return false;
            using Resolve = int(__thiscall*)(void*, std::uint32_t);
            for (unsigned i = 0; i < 3; ++i)
            {
                handles[i] = materials->Keys[i] ? reinterpret_cast<Resolve>(
                    EngineRuntime::RuntimeAddress(0x00A70B80))(manager, materials->Keys[i]) : 0;
                if (materials->Keys[i] && handles[i] < 2) return false;
                if (handles[i] >= 2)
                {
                    using Lookup = void*(__thiscall*)(void*, int, int);
                    if (!reinterpret_cast<Lookup>(EngineRuntime::RuntimeAddress(0x00A71E80))(manager, handles[i], 1)) return false;
                    const auto flags = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<uintptr_t>(manager) + 0x28);
                    if ((flags[handles[i]] & 0x14) != 4) return false;
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ApplyMaterials(uintptr_t stats, uintptr_t rendering, const Materials* materials, const int* handles)
    {
        __try
        {
            auto* manager = *reinterpret_cast<void**>(EngineRuntime::RuntimeAddress(0x019F4D3C));
            const auto vt = *reinterpret_cast<uintptr_t*>(manager);
            using Ref = void(__thiscall*)(void*, int);
            const auto retain = *reinterpret_cast<Ref*>(vt + 0xA4);
            const auto release = *reinterpret_cast<Ref*>(vt + 0xA8);
            for (unsigned i = 0; i < 3; ++i)
            {
                auto& previous = *reinterpret_cast<int*>(stats + TextureOffsets[i]);
                if (previous == handles[i]) continue;
                if (handles[i] >= 2) retain(manager, handles[i]);
                const int old = previous; previous = handles[i];
                if (old >= 2) release(manager, old);
            }
            for (unsigned i = 0; i < 10; ++i)
                *reinterpret_cast<std::uint32_t*>(stats + ColorOffsets[i]) = materials->Colors[i];
            using Refresh = void(__thiscall*)(void*);
            // This invokes the normal PartStats material listener for the new tree.
            reinterpret_cast<Refresh>(EngineRuntime::RuntimeAddress(0x00F757C0))(reinterpret_cast<void*>(rendering));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool MatchesMappedVisualABI()
    {
        struct Signature { uintptr_t Address; std::uint8_t Bytes[8]; };
        static constexpr Signature signatures[]{
            { 0x00F11950, { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x1C, 0x57 } },
            { 0x00F1DA50, { 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1, 0x8B, 0x46 } },
            { 0x00F757C0, { 0x83, 0xEC, 0x08, 0x55, 0x56, 0x8B, 0xF1, 0x83 } },
            { 0x00F3DBD0, { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x8B, 0x46 } },
            { 0x00A6FE10, { 0x0F, 0xB7, 0x01, 0x66, 0x83, 0xF8, 0x02, 0x7C } },
            { 0x00A70B80, { 0x83, 0x7C, 0x24, 0x04, 0x00, 0x56, 0x8B, 0xF1 } },
            { 0x00A71E80, { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x28, 0x57, 0x8B } },
        };
        for (const auto& signature : signatures)
        {
            std::uint8_t actual[8]{};
            if (!EngineRuntime::ReadBytes(EngineRuntime::RuntimeAddress(signature.Address), actual, sizeof(actual)) ||
                std::memcmp(actual, signature.Bytes, sizeof(actual))) return false;
        }
        return true;
    }
    bool IsRendering(uintptr_t component)
    {
        uintptr_t vtable = 0;
        return EngineRuntime::IsActiveComponent(component) && EngineRuntime::Read(component, vtable) &&
            vtable == EngineRuntime::RuntimeAddress(0x017560E4);
    }
    bool SaveNative(uintptr_t rendering, Archive* stream)
    {
        __try
        {
            using Save = void(__thiscall*)(void*, void*, void*);
            reinterpret_cast<Save>(EngineRuntime::RuntimeAddress(0x00F11950))(
                reinterpret_cast<void*>(rendering), stream, nullptr);
            return stream->Depth == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool CheckCaptureTree(std::uint32_t handle, unsigned depth, unsigned& nodes,
        std::set<std::uint32_t>& active)
    {
        if (handle < 2) return true;
        if (depth > 16 || ++nodes > 64 || !active.insert(handle).second) return false;
        uintptr_t manager = 0, table = 0, actor = 0, children = 0;
        std::uint32_t capacity = 0, count = 0;
        if (!EngineRuntime::Read(EngineRuntime::RuntimeAddress(0x019FDF54), manager) || !manager ||
            !EngineRuntime::Read(manager + 0xC8, capacity) || handle >= capacity ||
            !EngineRuntime::Read(manager + 0xC4, table) || !table ||
            !EngineRuntime::Read(table + handle * 4, actor) || !actor ||
            !EngineRuntime::Read(actor + 0x28, count) || count > 32 ||
            !EngineRuntime::Read(actor + 0x24, children) || (count && !children)) return false;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint32_t child = 0;
            if (!EngineRuntime::Read(children + i * 4, child) ||
                !CheckCaptureTree(child, depth + 1, nodes, active)) return false;
        }
        active.erase(handle); return true;
    }
    bool LoadNative(uintptr_t rendering, Archive* stream)
    {
        __try
        {
            using Load = void(__thiscall*)(void*, void*);
            using Refresh = void(__thiscall*)(void*);
            reinterpret_cast<Load>(EngineRuntime::RuntimeAddress(0x00F1DA50))(
                reinterpret_cast<void*>(rendering), stream);
            if (stream->Offset != stream->Size || stream->Depth) return false;
            // Native PartRendering post-load: bind this actor's owner to the
            // newly created render tree, attach stats callbacks, update location.
            reinterpret_cast<Refresh>(EngineRuntime::RuntimeAddress(0x00F757C0))(
                reinterpret_cast<void*>(rendering));
            reinterpret_cast<Refresh>(EngineRuntime::RuntimeAddress(0x00F3DBD0))(
                reinterpret_cast<void*>(rendering));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ResolveModel(std::uint32_t key)
    {
        uintptr_t assets = 0, manager = 0;
        if (!EngineRuntime::Read(EngineRuntime::RuntimeAddress(0x019F4D80), assets) || !assets ||
            !EngineRuntime::Read(EngineRuntime::RuntimeAddress(0x019FDF54), manager) || !manager)
            return false;
        __try
        {
            using Resolve = int(__thiscall*)(void*, std::uint32_t);
            using Lookup = void*(__thiscall*)(void*, int, int);
            const int handle = reinterpret_cast<Resolve>(EngineRuntime::RuntimeAddress(0x00A70B80))(
                reinterpret_cast<void*>(assets), key);
            if (handle < 2 || !reinterpret_cast<Lookup>(EngineRuntime::RuntimeAddress(0x00A71E80))(
                reinterpret_cast<void*>(manager), handle, 1)) return false;
            // A71E80(...,1) loads a registered resource if necessary. Reject its
            // fallback placeholder (flag 0x10), which is not the peer's model.
            const auto flags = *reinterpret_cast<std::uint8_t**>(manager + 0x28);
            return (flags[handle] & 0x14) == 4;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ResolveAnimation(uintptr_t assets, std::uint32_t key)
    {
        __try
        {
            using Resolve = int(__thiscall*)(void*, std::uint32_t);
            return reinterpret_cast<Resolve>(EngineRuntime::RuntimeAddress(0x00A70B80))(
                reinterpret_cast<void*>(assets), key) >= 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Called only after structural validation. Resolve every attached model
    // before touching the existing remote render tree.
    bool PreflightActor(CharacterVisualArchive::Cursor& in)
    {
        in.At += 9;
        if (!in.Bytes[in.At++]) return true;
        std::uint32_t key = 0;
        in.Read32(key);
        if (!ResolveModel(key)) return false;
        in.At += 9 + 110;
        std::uint32_t animationKey = 0; in.Read32(animationKey);
        if (animationKey)
        {
            uintptr_t assets = 0;
            if (!EngineRuntime::Read(EngineRuntime::RuntimeAddress(0x019F4D44), assets) || !assets ||
                !ResolveAnimation(assets, animationKey)) return false;
        }
        std::uint32_t count = 0; in.Read32(count);
        for (std::uint32_t i = 0; i < count; ++i)
            if (!PreflightActor(in)) return false;
        return true;
    }
}
namespace CharacterVisualRuntime
{
    bool Capture(std::vector<std::uint8_t>& output)
    {
        output.clear();
        if (!MatchesMappedVisualABI()) return false;
        const auto object = PositionTracker::GetRuntimeObject();
        if (!PositionTracker::HasPosition() || !EngineRuntime::IsActiveObject(object)) return false;
        const auto rendering = EngineRuntime::GetComponent(object, 7);
        std::uint32_t handle = 0;
        if (!IsRendering(rendering) || !EngineRuntime::Read(rendering + 0x9C, handle) || handle < 2)
            return false;
        unsigned nodes = 0;
        std::set<std::uint32_t> active;
        std::uint32_t overrides = 0;
        if (!EngineRuntime::Read(rendering + 0x90, overrides) || overrides > 128 ||
            !CheckCaptureTree(handle, 0, nodes, active)) return false;
        // Spare bytes protect the native writer's size backpatch if its fixed
        // buffer fills exactly. A full buffer is rejected below.
        std::vector<std::uint8_t> scratch(CharacterVisualArchive::MaxBytes + 16);
        std::array<std::uint32_t, 512> chunks{};
        Archive stream{};
        stream.Buffer = scratch.data(); stream.Size = CharacterVisualArchive::MaxBytes;
        stream.Chunks = chunks.data(); stream.Capacity = 256;
        if (!SaveNative(rendering, &stream) || stream.Offset >= stream.Size) return false;
        scratch.resize(stream.Offset);
        Materials materials{};
        if (!CaptureMaterials(EngineRuntime::GetComponent(object, 8), &materials) ||
            !CharacterVisualArchive::Normalize(scratch, RenderingTag, true, output) ||
            output.size() + CharacterVisualArchive::MaterialBytes > CharacterVisualArchive::MaxBytes) return false;
        CharacterVisualArchive::Write32(output, CharacterVisualArchive::MaterialTag);
        const auto* data = reinterpret_cast<const std::uint8_t*>(&materials);
        output.insert(output.end(), data, data + sizeof(materials));
        return true;
    }
    bool Apply(std::uint32_t actorHandle, uintptr_t runtime, const std::vector<std::uint8_t>& bytes)
    {
        if (!MatchesMappedVisualABI()) return false;
        if (!runtime || actorHandle == PositionTracker::GetObjectHandle() ||
            EngineRuntime::ResolveActorHandle(actorHandle) != runtime || !EngineRuntime::IsActiveObject(runtime))
            return false;
        const auto rendering = EngineRuntime::GetComponent(runtime, 7);
        if (!IsRendering(rendering)) return false;
        std::vector<std::uint8_t> checked;
        if (!CharacterVisualArchive::Normalize(bytes, RenderingTag, false, checked)) return false;
        CharacterVisualArchive::Cursor cursor{ checked, 10 };
        std::uint32_t count = 0; cursor.Read32(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint32_t key = 0; cursor.Read32(key); cursor.At += 4;
            if (key && !ResolveModel(key)) return false;
        }
        // A ready snapshot must contain a body, not the serializer's null actor.
        if (cursor.At + 9 >= checked.size() || !checked[cursor.At + 9] || !PreflightActor(cursor))
            return false;
        std::uint32_t nativeLength = 0;
        std::memcpy(&nativeLength, checked.data() + 5, 4);
        nativeLength += 9;
        if (checked.size() != nativeLength + CharacterVisualArchive::MaterialBytes) return false;
        Materials materials{}; int textureHandles[3]{};
        std::memcpy(&materials, checked.data() + nativeLength + 4, sizeof(materials));
        const auto stats = EngineRuntime::GetComponent(runtime, 8);
        uintptr_t statsVtable = 0;
        if (!EngineRuntime::Read(stats, statsVtable) || statsVtable != EngineRuntime::RuntimeAddress(0x017562E4) ||
            !ResolveMaterials(&materials, textureHandles)) return false;
        std::array<std::uint32_t, 512> chunks{};
        void* vtable[2]{ nullptr, reinterpret_cast<void*>(&ReadArchive) };
        Archive stream{};
        stream.Vtable = vtable; stream.Buffer = checked.data();
        stream.Size = nativeLength;
        stream.Chunks = chunks.data(); stream.Capacity = 256;
        return LoadNative(rendering, &stream) && ApplyMaterials(stats, rendering, &materials, textureHandles);
    }
}
