#pragma once
#include <Windows.h>
namespace WindowFocusPolicy
{
    // Let Windows process real focus, but don't deliver automatic
    // deactivation to the game's pause handler. No synthetic keyboard input.
    inline bool IsAutomaticDeactivation(UINT message, WPARAM value)
    {
        return message == WM_KILLFOCUS ||
            (message == WM_ACTIVATEAPP && value == FALSE) ||
            (message == WM_ACTIVATE && LOWORD(value) == WA_INACTIVE);
    }
}
