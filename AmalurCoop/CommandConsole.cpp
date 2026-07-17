#include "CommandConsole.h"

#include "Logger.h"
#include "LobbyManager.h"
#include "CombatScaler.h"
#include "NetworkManager.h"
#include "MemoryScanner.h"
#include "GameState.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include "RemotePlayerManager.h"

namespace
{
    std::atomic<bool> g_running = false;
    HANDLE g_thread = nullptr;

    void OpenConsoleWindow()
    {
        if (!GetConsoleWindow())
            AllocConsole();

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);

        SetConsoleTitleA("AmalurCoop // Debug Console");

        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(output, &mode))
            SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    uintptr_t ParseHexAddress(const std::string& text, bool& ok)
    {
        ok = false;

        try
        {
            uintptr_t address = static_cast<uintptr_t>(
                std::stoull(text, nullptr, 16)
                );

            ok = true;
            return address;
        }
        catch (...)
        {
            return 0;
        }
    }

    void PrintStartupScreen()
    {
        system("cls");

        std::cout << "\x1b[38;5;208m";

        std::cout << R"(

    ============================================================================
    ||                                                                        ||
    ||                         A M A L U R  C O O P                           ||
    ||                                                                        ||
    ||                  Kingdoms of Amalur Multiplayer Mod                     ||
    ||                                                                        ||
    ============================================================================

)";

        std::cout << "\x1b[0m";

        std::cout << "\x1b[90m[\x1b[36mCORE\x1b[90m]\x1b[0m AmalurCoop loaded\n";
        std::cout << "\x1b[90m[\x1b[36mLOG\x1b[90m]\x1b[0m Logger initialized\n";
        std::cout << "\x1b[90m[\x1b[36mCONFIG\x1b[90m]\x1b[0m Config loaded\n";
        std::cout << "\x1b[90m[\x1b[36mLOBBY\x1b[90m]\x1b[0m Lobby manager initialized\n";
        std::cout << "\x1b[90m[\x1b[36mSCALER\x1b[90m]\x1b[0m Combat scaler initialized\n";
        std::cout << "\x1b[90m[\x1b[36mHOOKS\x1b[90m]\x1b[0m Hook manager initialized\n";
        std::cout << "\x1b[90m[\x1b[36mMEMORY\x1b[90m]\x1b[0m Memory scanner initialized\n";
        std::cout << "\x1b[90m[\x1b[36mGAME\x1b[90m]\x1b[0m Game state initialized\n";
        std::cout << "\x1b[90m[\x1b[36mNETWORK\x1b[90m]\x1b[0m Network manager initialized\n";
        std::cout << "\x1b[90m[\x1b[36mCOMMANDS\x1b[90m]\x1b[0m Command console ready. Type help.\n\n";

        std::cout << "\x1b[90m[\x1b[36mPLAYERS\x1b[90m]\x1b[0m "
            << LobbyManager::GetPlayerCount()
            << "/"
            << LobbyManager::GetMaxPlayers()
            << "\n";

        std::cout << "\x1b[90m[\x1b[36mBOSS SCALE\x1b[90m]\x1b[0m 100 HP -> "
            << CombatScaler::ScaleBossHealth(100.0f)
            << " HP\n";

        std::cout << "\x1b[90m[\x1b[36mENEMY SCALE\x1b[90m]\x1b[0m 100 HP -> "
            << CombatScaler::ScaleEnemyHealth(100.0f)
            << " HP\n\n";

        std::cout << "\x1b[90m[\x1b[36mSTATUS\x1b[90m]\x1b[0m Foundation initialized successfully.\n";
        std::cout << "\x1b[90m[\x1b[36mSTATUS\x1b[90m]\x1b[0m Type help to show available commands.\n";
    }

    void PrintHelp()
    {
        std::cout << "\nAvailable Commands\n";
        std::cout << "------------------\n";
        std::cout << "  host              Start hosting on port 7777\n";
        std::cout << "  join <ip>         Join a host by IP address\n";
        std::cout << "  players <count>   Set lobby player count for scaling tests\n";
        std::cout << "  status            Show current status\n";
        std::cout << "  clear             Clear command output only\n";
        std::cout << "  modules           List loaded modules\n";
        std::cout << "  watch <hexaddr>   Read float/int at address\n";
        std::cout << "  setpos <hexaddr>  Set player XYZ position address\n";
        std::cout << "  gamestate         Show tracked game state\n";      
        std::cout << "  scanfloat <value> Search koa.exe for float value\n";
        std::cout << "  nextfloat <value> Filter previous float scan\n";
        std::cout << "  scanlist          Show float scan results\n";
        std::cout << "  scanreset         Clear float scan results\n";
        std::cout << "  dummy             Spawn visible game-managed proxy near local player\n";
        std::cout << "  dummypos          Show fake remote player position\n";
    }

    void PrintStatus()
    {
        std::cout << "\n[Status]\n";
        std::cout << "  Network: " << NetworkManager::GetStatusText() << "\n";

        std::cout << "  Players: "
            << LobbyManager::GetPlayerCount()
            << "/"
            << LobbyManager::GetMaxPlayers()
            << "\n";

        std::cout << "  Boss 100 HP -> "
            << CombatScaler::ScaleBossHealth(100.0f)
            << " HP\n";

        std::cout << "  Enemy 100 HP -> "
            << CombatScaler::ScaleEnemyHealth(100.0f)
            << " HP\n";

        if (GameState::HasPlayerPosition())
        {
            GameState::Update();
            GameState::Vec3 pos = GameState::GetPlayerPosition();

            std::cout << "  Player Position: "
                << pos.X << ", "
                << pos.Y << ", "
                << pos.Z << "\n";
        }
        else
        {
            std::cout << "  Player Position: not set\n";
        }

        std::cout << "\n";
    }

    void HandleCommand(const std::string& line)
    {
        std::stringstream ss(line);

        std::string cmd;
        ss >> cmd;

        if (cmd.empty())
            return;

        if (cmd == "clear")
        {
            PrintStartupScreen();
            return;
        }

        if (cmd == "dummy")
        {
            RemotePlayerManager::SpawnDummyNearLocalPlayer();
            return;
        }

        if (cmd == "dummypos")
        {
            RemotePlayerManager::PrintDummy();
            return;
        }

        if (cmd == "scanfloat")
        {
            float value = 0.0f;
            ss >> value;

            if (ss.fail())
            {
                std::cout << "[Usage] scanfloat 100.0\n";
                return;
            }

            MemoryScanner::FirstFloatScan(value);
            return;
        }

        if (cmd == "nextfloat")
        {
            float value = 0.0f;
            ss >> value;

            if (ss.fail())
            {
                std::cout << "[Usage] nextfloat 95.0\n";
                return;
            }

            MemoryScanner::NextFloatScan(value);
            return;
        }

        if (cmd == "scanlist")
        {
            MemoryScanner::PrintFloatScanResults(50);
            return;
        }

        if (cmd == "scanreset")
        {
            MemoryScanner::ResetFloatScan();
            return;
        }

        if (cmd == "help")
        {
            PrintHelp();
            return;
        }

        if (cmd == "host")
        {
            if (NetworkManager::Host(7777))
                std::cout << "[Network] Hosting on port 7777\n";
            else
                std::cout << "[Network] Failed to host\n";

            return;
        }

        if (cmd == "join")
        {
            std::string ip;
            ss >> ip;

            if (ip.empty())
            {
                std::cout << "[Usage] join 127.0.0.1\n";
                return;
            }

            if (NetworkManager::Join(ip, 7777))
                std::cout << "[Network] Sent join request to " << ip << ":7777\n";
            else
                std::cout << "[Network] Failed to join\n";

            return;
        }

        if (cmd == "players")
        {
            int count = 1;
            ss >> count;

            LobbyManager::SetPlayerCount(count);

            std::cout << "[Lobby] Players set to "
                << LobbyManager::GetPlayerCount()
                << "/"
                << LobbyManager::GetMaxPlayers()
                << "\n";

            std::cout << "[Scaler] Boss 100 HP -> "
                << CombatScaler::ScaleBossHealth(100.0f)
                << " HP\n";

            std::cout << "[Scaler] Enemy 100 HP -> "
                << CombatScaler::ScaleEnemyHealth(100.0f)
                << " HP\n";

            return;
        }

        if (cmd == "status")
        {
            PrintStatus();
            return;
        }

        if (cmd == "modules")
        {
            MemoryScanner::PrintModules();
            return;
        }

        if (cmd == "watch")
        {
            std::string addressText;
            ss >> addressText;

            if (addressText.empty())
            {
                std::cout << "[Usage] watch 0x12345678\n";
                return;
            }

            bool ok = false;
            uintptr_t address = ParseHexAddress(addressText, ok);

            if (!ok)
            {
                std::cout << "[Error] Invalid address\n";
                return;
            }

            MemoryScanner::WatchAddress(address);
            return;
        }

        if (cmd == "setpos")
        {
            std::string addressText;
            ss >> addressText;

            if (addressText.empty())
            {
                std::cout << "[Usage] setpos 0x12345678\n";
                return;
            }

            bool ok = false;
            uintptr_t address = ParseHexAddress(addressText, ok);

            if (!ok)
            {
                std::cout << "[Error] Invalid address\n";
                return;
            }

            GameState::SetPlayerPositionAddress(address);
            return;
        }

        if (cmd == "gamestate")
        {
            GameState::PrintState();
            return;
        }

        std::cout << "[Unknown Command] " << cmd << "\n";
        std::cout << "Type help for commands.\n";
    }

    DWORD WINAPI ConsoleThread(LPVOID)
    {
        PrintStartupScreen();

        while (g_running.load())
        {
            std::cout << "\nAmalurCoop> ";

            std::string line;
            std::getline(std::cin, line);

            if (!g_running.load())
                break;

            if (!std::cin.good())
            {
                std::cin.clear();
                Sleep(50);
                continue;
            }

            HandleCommand(line);
        }

        return 0;
    }
}

namespace CommandConsole
{
    void Initialize()
    {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true))
            return;

        OpenConsoleWindow();
        g_thread = CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, nullptr);

        if (!g_thread)
        {
            g_running.store(false);
            FreeConsole();
            Logger::Write("CommandConsole failed to create thread");
            return;
        }

        Logger::Write("CommandConsole initialized");
    }

    void Shutdown()
    {
        if (!g_running.exchange(false))
            return;

        FreeConsole();

        if (g_thread)
        {
            WaitForSingleObject(g_thread, 250);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }

        Logger::Write("CommandConsole shutdown");
    }

    void Update()
    {
    }

    bool IsRunning()
    {
        return g_running.load();
    }
}
