#pragma once

namespace LobbyManager
{
    void Initialize();
    void Shutdown();
    void Update();

    int GetPlayerCount();
    int GetMaxPlayers();

    void SetPlayerCount(int count);
    void SetMaxPlayers(int count);
}
