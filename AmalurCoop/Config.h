#pragma once

namespace Config
{
    struct Settings
    {
        int MaxPlayers = 4;
        bool EnableScaling = true;

        float BossHealthPerExtraPlayer = 0.65f;
        float EnemyHealthPerExtraPlayer = 0.35f;
        float BossDamagePerExtraPlayer = 0.15f;
        float EnemyDamagePerExtraPlayer = 0.10f;

        bool MenuStartsOpen = false;
        bool CaptureInput = true;
        bool ForceCursorVisible = true;
        bool AutoScanOnStartup = false;
        bool DebugConsoleOnStartup = false;
        float MenuScale = 1.0f;
    };

    void Load();
    void Save();
    void ResetToDefaults();

    Settings Get();
    void Set(const Settings& settings);
}
