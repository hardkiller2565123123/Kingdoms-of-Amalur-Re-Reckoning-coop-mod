#include "GameState.h"

#include "Logger.h"
#include "MemoryScanner.h"

#include <Windows.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace
{
    uintptr_t g_playerPositionAddress = 0;
    GameState::Vec3 g_playerPosition{};
    std::mutex g_mutex;

    bool IsGoodCoord(float value)
    {
        return std::isfinite(value) && value > -100000.0f && value < 100000.0f;
    }

    bool ReadVec3(uintptr_t address, GameState::Vec3& out)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        if (!MemoryScanner::ReadFloat(address + 0x0, x) ||
            !MemoryScanner::ReadFloat(address + 0x4, y) ||
            !MemoryScanner::ReadFloat(address + 0x8, z))
        {
            return false;
        }

        if (!IsGoodCoord(x) || !IsGoodCoord(y) || !IsGoodCoord(z))
            return false;

        out = { x, y, z };
        return true;
    }
}

namespace GameState
{
    void Initialize()
    {
        Logger::Write("GameState initialized");
    }

    void Shutdown()
    {
        Logger::Write("GameState shutdown");
    }

    void Update()
    {
        uintptr_t address = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            address = g_playerPositionAddress;
        }

        // The automatic PositionTracker owns background discovery. GameState only
        // reads an explicitly selected address so the main worker never performs a
        // full executable scan every few seconds.
        if (!address)
            return;

        Vec3 position{};
        if (!ReadVec3(address, position))
            return;

        std::lock_guard<std::mutex> lock(g_mutex);
        g_playerPosition = position;
    }

    void AutoDetectPlayerPosition()
    {
        const auto modules = MemoryScanner::GetLoadedModules();
        uintptr_t base = 0;
        size_t size = 0;

        for (const auto& module : modules)
        {
            if (_stricmp(module.Name.c_str(), "koa.exe") == 0)
            {
                base = module.Base;
                size = module.Size;
                break;
            }
        }

        if (!base || !size)
            return;

        const uintptr_t end = base + size;
        for (uintptr_t address = base; address < end; address += sizeof(float))
        {
            Vec3 position{};
            if (!ReadVec3(address, position))
                continue;

            if (std::fabs(position.X) < 0.001f &&
                std::fabs(position.Y) < 0.001f &&
                std::fabs(position.Z) < 0.001f)
            {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_playerPositionAddress = address;
                g_playerPosition = position;
            }

            Logger::Write("GameState auto position candidate found at 0x" + std::to_string(address));
            return;
        }
    }

    void ClearPlayerPositionAddress()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_playerPositionAddress = 0;
        g_playerPosition = {};
        Logger::Write("Player position address cleared");
    }

    void SetPlayerPositionAddress(uintptr_t address)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_playerPositionAddress = address;
        Logger::Write("Player position address set to 0x" + std::to_string(address));
    }

    uintptr_t GetPlayerPositionAddress()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_playerPositionAddress;
    }

    bool HasPlayerPosition()
    {
        return GetPlayerPositionAddress() != 0;
    }

    Vec3 GetPlayerPosition()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_playerPosition;
    }

    void PrintState()
    {
        const uintptr_t address = GetPlayerPositionAddress();
        const Vec3 position = GetPlayerPosition();

        std::cout << "\n[GameState]\n";
        if (!address)
        {
            std::cout << "  Player position address: not set\n\n";
            return;
        }

        std::cout << "  Player position address: 0x" << std::hex << address << std::dec << "\n";
        std::cout << "  X: " << position.X << "\n";
        std::cout << "  Y: " << position.Y << "\n";
        std::cout << "  Z: " << position.Z << "\n\n";
    }
}
