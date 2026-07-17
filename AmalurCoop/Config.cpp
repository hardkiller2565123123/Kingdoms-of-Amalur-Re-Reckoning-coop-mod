#include "Config.h"
#include "Paths.h"
#include "Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace
{
    Config::Settings g_settings;
    std::mutex g_mutex;

    int ReadInt(const char* section, const char* key, int fallback)
    {
        return GetPrivateProfileIntA(section, key, fallback, Paths::GetConfigPath().c_str());
    }

    float ReadFloat(const char* section, const char* key, float fallback)
    {
        char buffer[64]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), Paths::GetConfigPath().c_str());
        return buffer[0] ? static_cast<float>(std::atof(buffer)) : fallback;
    }

    int ToIniBool(bool value)
    {
        return value ? 1 : 0;
    }

    Config::Settings Sanitize(Config::Settings value)
    {
        value.MaxPlayers = std::clamp(value.MaxPlayers, 1, 16);
        value.BossHealthPerExtraPlayer = std::clamp(value.BossHealthPerExtraPlayer, 0.0f, 5.0f);
        value.EnemyHealthPerExtraPlayer = std::clamp(value.EnemyHealthPerExtraPlayer, 0.0f, 5.0f);
        value.BossDamagePerExtraPlayer = std::clamp(value.BossDamagePerExtraPlayer, 0.0f, 5.0f);
        value.EnemyDamagePerExtraPlayer = std::clamp(value.EnemyDamagePerExtraPlayer, 0.0f, 5.0f);
        value.MenuScale = std::clamp(value.MenuScale, 0.75f, 1.75f);
        return value;
    }

    void WriteInt(const char* section, const char* key, int value)
    {
        WritePrivateProfileStringA(section, key, std::to_string(value).c_str(), Paths::GetConfigPath().c_str());
    }

    void WriteFloat(const char* section, const char* key, float value)
    {
        char buffer[64]{};
        sprintf_s(buffer, "%.3f", value);
        WritePrivateProfileStringA(section, key, buffer, Paths::GetConfigPath().c_str());
    }
}

namespace Config
{
    void Load()
    {
        CreateDirectoryA(Paths::GetModFolder().c_str(), nullptr);

        Settings loaded{};
        loaded.MaxPlayers = ReadInt("Multiplayer", "MaxPlayers", loaded.MaxPlayers);

        loaded.EnableScaling = ReadInt("Scaling", "Enabled", ToIniBool(loaded.EnableScaling)) != 0;
        loaded.BossHealthPerExtraPlayer = ReadFloat("Scaling", "BossHealthPerExtraPlayer", loaded.BossHealthPerExtraPlayer);
        loaded.EnemyHealthPerExtraPlayer = ReadFloat("Scaling", "EnemyHealthPerExtraPlayer", loaded.EnemyHealthPerExtraPlayer);
        loaded.BossDamagePerExtraPlayer = ReadFloat("Scaling", "BossDamagePerExtraPlayer", loaded.BossDamagePerExtraPlayer);
        loaded.EnemyDamagePerExtraPlayer = ReadFloat("Scaling", "EnemyDamagePerExtraPlayer", loaded.EnemyDamagePerExtraPlayer);

        loaded.MenuStartsOpen = ReadInt("Interface", "MenuStartsOpen", ToIniBool(loaded.MenuStartsOpen)) != 0;
        loaded.CaptureInput = ReadInt("Interface", "CaptureInput", ToIniBool(loaded.CaptureInput)) != 0;
        loaded.ForceCursorVisible = ReadInt("Interface", "ForceCursorVisible", ToIniBool(loaded.ForceCursorVisible)) != 0;
        loaded.MenuScale = ReadFloat("Interface", "MenuScale", loaded.MenuScale);

        loaded.AutoScanOnStartup = ReadInt("Debug", "AutoScanOnStartup", ToIniBool(loaded.AutoScanOnStartup)) != 0;
        loaded.DebugConsoleOnStartup = ReadInt("Debug", "ConsoleOnStartup", ToIniBool(loaded.DebugConsoleOnStartup)) != 0;

        const bool missing = GetFileAttributesA(Paths::GetConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        Set(loaded);

        if (missing)
        {
            Save();
            Logger::Write("Default config.ini created");
        }

        Logger::Write("Config loaded");
    }

    void Save()
    {
        CreateDirectoryA(Paths::GetModFolder().c_str(), nullptr);
        const Settings value = Get();

        WriteInt("Multiplayer", "MaxPlayers", value.MaxPlayers);

        WriteInt("Scaling", "Enabled", ToIniBool(value.EnableScaling));
        WriteFloat("Scaling", "BossHealthPerExtraPlayer", value.BossHealthPerExtraPlayer);
        WriteFloat("Scaling", "EnemyHealthPerExtraPlayer", value.EnemyHealthPerExtraPlayer);
        WriteFloat("Scaling", "BossDamagePerExtraPlayer", value.BossDamagePerExtraPlayer);
        WriteFloat("Scaling", "EnemyDamagePerExtraPlayer", value.EnemyDamagePerExtraPlayer);

        WriteInt("Interface", "MenuStartsOpen", ToIniBool(value.MenuStartsOpen));
        WriteInt("Interface", "CaptureInput", ToIniBool(value.CaptureInput));
        WriteInt("Interface", "ForceCursorVisible", ToIniBool(value.ForceCursorVisible));
        WriteFloat("Interface", "MenuScale", value.MenuScale);

        WriteInt("Debug", "AutoScanOnStartup", ToIniBool(value.AutoScanOnStartup));
        WriteInt("Debug", "ConsoleOnStartup", ToIniBool(value.DebugConsoleOnStartup));

        Logger::Write("Config saved");
    }

    void ResetToDefaults()
    {
        Set(Settings{});
        Save();
        Logger::Write("Config reset to defaults");
    }

    Settings Get()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_settings;
    }

    void Set(const Settings& settings)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_settings = Sanitize(settings);
    }
}
