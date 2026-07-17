#include "CombatScaler.h"
#include "LobbyManager.h"
#include "Config.h"
#include "Logger.h"

namespace
{
    float ScaleValue(float baseValue, float perExtraPlayer)
    {
        int players = LobbyManager::GetPlayerCount();

        if (players <= 1)
            return baseValue;

        int extraPlayers = players - 1;

        return baseValue * (1.0f + (extraPlayers * perExtraPlayer));
    }
}

namespace CombatScaler
{
    void Initialize()
    {
        Logger::Write("CombatScaler initialized");
        Logger::Write("Boss HP scale ready");
        Logger::Write("Enemy HP scale ready");
    }

    void Shutdown()
    {
        Logger::Write("CombatScaler shutdown");
    }

    void Update()
    {}

    float ScaleBossHealth(float baseHealth)
    {
        if (!Config::Get().EnableScaling)
            return baseHealth;

        return ScaleValue(
            baseHealth,
            Config::Get().BossHealthPerExtraPlayer
        );
    }

    float ScaleEnemyHealth(float baseHealth)
    {
        if (!Config::Get().EnableScaling)
            return baseHealth;

        return ScaleValue(
            baseHealth,
            Config::Get().EnemyHealthPerExtraPlayer
        );
    }

    float ScaleBossDamage(float baseDamage)
    {
        if (!Config::Get().EnableScaling)
            return baseDamage;

        return ScaleValue(
            baseDamage,
            Config::Get().BossDamagePerExtraPlayer
        );
    }

    float ScaleEnemyDamage(float baseDamage)
    {
        if (!Config::Get().EnableScaling)
            return baseDamage;

        return ScaleValue(
            baseDamage,
            Config::Get().EnemyDamagePerExtraPlayer
        );
    }
}