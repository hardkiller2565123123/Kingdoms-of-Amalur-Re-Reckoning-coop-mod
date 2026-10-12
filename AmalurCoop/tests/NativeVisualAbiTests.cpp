#include "../NativeVisualCalls.h"
#include "../WindowFocusPolicy.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

static void* observedThis;
static std::uint32_t observedArgument;
static unsigned calls;
// Match the executable's ABI, not a compiler-inferred C++ function signature.
__declspec(naked) void NativeTransformFixture()
{
    __asm {
        mov observedThis, ecx
        mov eax, [esp + 4]
        mov observedArgument, eax
        inc calls
        ret 4
    }
}
int main()
{
    static_assert(sizeof(void*) == 4);
    using WindowFocusPolicy::IsAutomaticDeactivation;
    assert(IsAutomaticDeactivation(WM_KILLFOCUS, 0));
    assert(IsAutomaticDeactivation(WM_ACTIVATEAPP, FALSE));
    assert(IsAutomaticDeactivation(WM_ACTIVATE, WA_INACTIVE));
    assert(!IsAutomaticDeactivation(WM_ACTIVATEAPP, TRUE));
    assert(!IsAutomaticDeactivation(WM_ACTIVATE, WA_ACTIVE));
    assert(!IsAutomaticDeactivation(WM_KEYDOWN, VK_ESCAPE));
    assert(!IsAutomaticDeactivation(WM_CLOSE, 0));
    int rendering = 0;
    std::uintptr_t before = 0, after = 0;
    for (unsigned i = 0; i < 1000; ++i)
    {
        __asm mov before, esp
        NativeVisualCalls::UpdateWorldTransform(
            reinterpret_cast<uintptr_t>(&NativeTransformFixture), &rendering);
        __asm mov after, esp
        assert(before == after);
        assert(observedThis == &rendering);
        assert(observedArgument == 0);
    }
    assert(calls == 1000);
    puts("PASS: production F3DBD0 wrapper preserves ESP and passes this + one stack argument (1000 calls, RTC enabled)");
}
