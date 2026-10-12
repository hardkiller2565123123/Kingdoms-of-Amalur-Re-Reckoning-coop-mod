#pragma once

namespace MainMenuMultiplayer
{
    // Enables the game's own hidden Multiplayer frontend path. This module does
    // not draw anything and does not consume input; all UI is owned by KOA.
    bool Initialize();
    void Shutdown();
    bool IsReady();
}
