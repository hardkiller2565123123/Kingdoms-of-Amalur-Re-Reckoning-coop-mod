#pragma once
#include <cstdint>

namespace NativeVisualCalls
{
    // F3DBD0 is a virtual callback ending in RET 4, unlike the zero-argument
    // F757C0 post-load callback. Its one stack argument is unused in this build.
    using UpdateWorldTransformFn = void(__thiscall*)(void*, std::uint32_t);
    inline void UpdateWorldTransform(uintptr_t address, void* rendering)
    {
        reinterpret_cast<UpdateWorldTransformFn>(address)(rendering, 0);
    }
}
