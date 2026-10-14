#pragma once

namespace Config
{
    struct Settings
    {
        int MaxPlayers = 4;
        bool AutoConnect = false; // legacy INI field; sessions require Host/Join
        char ServerAddress[64] = "auto";
        int ServerPort = 7777;
        bool EnableScaling = true;

        float BossHealthPerExtraPlayer = 0.65f;
        float EnemyHealthPerExtraPlayer = 0.35f;
        float BossDamagePerExtraPlayer = 0.15f;
        float EnemyDamagePerExtraPlayer = 0.10f;

        bool AutoScanOnStartup = false;
        bool DebugConsoleOnStartup = false;
        int ForegroundFPS = 60;
        int BackgroundFPS = 30;
    };

    void Load();
    void Save();
    void ResetToDefaults();

    Settings Get();
    void Set(const Settings& settings);
}
