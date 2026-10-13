#pragma once

namespace MainMenuMultiplayer
{
    // Patches the verified main_menu script in memory; KOA owns UI and input.
    bool Initialize();
    void Shutdown();
    bool IsReady();
    // Worker thread only: buttons queue socket/DNS work here.
    void PumpNetworkCommands();
}
