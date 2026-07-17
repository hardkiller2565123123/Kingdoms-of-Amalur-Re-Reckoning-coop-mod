#include "LobbyManager.h"
#include "Config.h"
#include "Logger.h"

#include <algorithm>
#include <atomic>

namespace
{
    std::atomic<int> g_playerCount = 1;
    std::atomic<int> g_maxPlayers = 4;
}

namespace LobbyManager
{
    void Initialize()
    {
        SetMaxPlayers(Config::Get().MaxPlayers);
        SetPlayerCount(1);
        Logger::Write("LobbyManager initialized");
    }

    void Shutdown()
    {
        Logger::Write("LobbyManager shutdown");
    }

    void Update()
    {
    }

    int GetPlayerCount()
    {
        return g_playerCount.load();
    }

    int GetMaxPlayers()
    {
        return g_maxPlayers.load();
    }

    void SetPlayerCount(int count)
    {
        g_playerCount.store(std::clamp(count, 1, GetMaxPlayers()));
    }

    void SetMaxPlayers(int count)
    {
        g_maxPlayers.store(std::clamp(count, 1, 16));
        SetPlayerCount(GetPlayerCount());
    }
}
