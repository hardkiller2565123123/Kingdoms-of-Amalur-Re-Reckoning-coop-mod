#include "AmalurCoop.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include "CombatScaler.h"
#include "Config.h"
#include "GameState.h"
#include "HookManager.h"
#include "ImGuiOverlay.h"
#include "LobbyManager.h"
#include "Logger.h"
#include "MemoryScanner.h"
#include "NetworkManager.h"
#include "PositionTracker.h"
#include "RemotePlayerManager.h"
#include "RuntimeInspector.h"
#include "ResearchLab.h"
#include "DInputProxy.h"

#include <atomic>
#include <exception>

namespace
{
    HMODULE g_module = nullptr;
    HANDLE g_workerThread = nullptr;

    std::atomic<bool> g_started{ false };
    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_initialized{ false };

    void LogStartupInformation()
    {
        Logger::Write(
            Logger::Level::Info,
            "============================================================");

        Logger::Write(
            Logger::Level::Success,
            "AmalurCoop DINPUT8 proxy loaded");

        Logger::WriteFormat(
            Logger::Level::Info,
            "Process ID: %lu",
            static_cast<unsigned long>(GetCurrentProcessId()));

        Logger::WriteFormat(
            Logger::Level::Info,
            "Worker thread ID: %lu",
            static_cast<unsigned long>(GetCurrentThreadId()));

        Logger::WriteFormat(
            Logger::Level::Info,
            "Game module base: 0x%p",
            GetModuleHandleW(nullptr));

        Logger::WriteFormat(
            Logger::Level::Info,
            "Proxy module base: 0x%p",
            g_module);

        Logger::Write(
            Logger::Level::Info,
            "Proxy filename: DINPUT8.dll");

        Logger::Write(
            Logger::Level::Info,
            "Log file: AmalurCoop.log");

        Logger::Write(
            Logger::Level::Info,
            "============================================================");
    }

    bool InitializeSubsystems()
    {
        // Always create the CMD logger.
        //
        // The second argument enables AmalurCoop.log in the game directory.
        if (!Logger::Initialize(
            L"AmalurCoop - Debug Logger",
            true))
        {
            OutputDebugStringA(
                "[AmalurCoop] Logger initialization failed\n");

            return false;
        }

        Logger::ShowConsole(true);
        LogStartupInformation();

        Logger::Write(
            Logger::Level::Info,
            "Loading system DirectInput 8 runtime...");

        if (!DInputProxy::Initialize())
        {
            Logger::Write(
                Logger::Level::Error,
                "System DirectInput 8 did not initialize completely");
        }
        else
        {
            Logger::Write(
                Logger::Level::Success,
                "System DirectInput 8 initialized successfully");
        }

        Logger::Write(
            Logger::Level::Info,
            "Loading configuration...");

        Config::Load();

        const Config::Settings settings =
            Config::Get();

        Logger::WriteFormat(
            Logger::Level::Info,
            "Configured maximum players: %d",
            settings.MaxPlayers);

        Logger::WriteFormat(
            Logger::Level::Info,
            "Automatic memory scan: %s",
            settings.AutoScanOnStartup ? "enabled" : "disabled");

        Logger::Write(
            Logger::Level::Info,
            "Initializing lobby manager...");

        LobbyManager::Initialize();
        LobbyManager::SetMaxPlayers(
            settings.MaxPlayers);

        Logger::Write(
            Logger::Level::Success,
            "Lobby manager initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing combat scaler...");

        CombatScaler::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Combat scaler initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing memory scanner...");

        MemoryScanner::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Memory scanner initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing game-state manager...");

        GameState::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Game-state manager initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing position tracker...");

        PositionTracker::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Position tracker initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing runtime-record inspector...");

        RuntimeInspector::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Runtime-record inspector initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing remote-player manager...");

        RemotePlayerManager::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Remote-player manager initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing network manager...");

        NetworkManager::Initialize();

        Logger::Write(
            Logger::Level::Success,
            "Network manager initialized");

        Logger::Write(
            Logger::Level::Info,
            "Initializing MinHook...");

        HookManager::Initialize();

        if (!HookManager::IsReady())
        {
            Logger::Write(
                Logger::Level::Error,
                "MinHook initialization failed; hooks are unavailable");
        }
        else
        {
            Logger::Write(
                Logger::Level::Success,
                "MinHook initialized successfully");

            ResearchLab::Initialize();

            Logger::Write(
                Logger::Level::Info,
                "Installing Direct3D 11 ImGui overlay hooks...");

            if (!ImGuiOverlay::Initialize())
            {
                Logger::Write(
                    Logger::Level::Error,
                    "ImGui overlay hook failed to initialize");
            }
            else
            {
                Logger::Write(
                    Logger::Level::Success,
                    "ImGui overlay initialized successfully");
            }
        }

        if (settings.AutoScanOnStartup)
        {
            Logger::Write(
                Logger::Level::Warning,
                "Automatic position scanning is disabled; using the verified runtime tracker instead");
        }


        g_initialized.store(
            true,
            std::memory_order_release);

        Logger::Write(
            Logger::Level::Success,
            "AmalurCoop initialization completed");

        Logger::Write(
            Logger::Level::Info,
            "Press F1 to open or close the ImGui menu");

        return true;
    }

    void UpdateSubsystems()
    {
        LobbyManager::Update();
        CombatScaler::Update();
        PositionTracker::Update();
        RuntimeInspector::Update();
        RemotePlayerManager::Update();
        NetworkManager::Update();
        GameState::Update();
    }

    void ShutdownSubsystems()
    {
        if (!Logger::IsInitialized())
            return;

        Logger::Write(
            Logger::Level::Warning,
            "AmalurCoop shutdown requested");

        if (!g_initialized.exchange(
            false,
            std::memory_order_acq_rel))
        {
            Logger::Write(
                Logger::Level::Warning,
                "Subsystems were not fully initialized");

            DInputProxy::Shutdown();
            Logger::Shutdown();
            return;
        }

        Logger::Write(
            Logger::Level::Info,
            "Shutting down ImGui overlay...");

        ImGuiOverlay::Shutdown();

        ResearchLab::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down hook manager...");

        HookManager::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down network manager...");

        NetworkManager::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down remote-player manager...");

        RemotePlayerManager::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down runtime-record inspector...");

        RuntimeInspector::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down position tracker...");

        PositionTracker::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down game-state manager...");

        GameState::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down memory scanner...");

        MemoryScanner::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down combat scaler...");

        CombatScaler::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down lobby manager...");

        LobbyManager::Shutdown();

        Logger::Write(
            Logger::Level::Info,
            "Shutting down DirectInput forwarding...");

        DInputProxy::Shutdown();

        Logger::Write(
            Logger::Level::Success,
            "AmalurCoop shutdown completed");

        Logger::Write(
            Logger::Level::Info,
            "============================================================");

        Logger::Shutdown();
    }

    DWORD WINAPI MainThread(LPVOID)
    {
        try
        {
            if (!InitializeSubsystems())
            {
                g_running.store(
                    false,
                    std::memory_order_release);

                g_started.store(
                    false,
                    std::memory_order_release);

                return 1;
            }

            while (g_running.load(
                std::memory_order_acquire))
            {
                UpdateSubsystems();

                // Roughly 30 updates per second.
                Sleep(33);
            }

            ShutdownSubsystems();
        }
        catch (const std::exception& exception)
        {
            if (Logger::IsInitialized())
            {
                Logger::WriteFormat(
                    Logger::Level::Error,
                    "Unhandled C++ exception in worker thread: %s",
                    exception.what());

                ShutdownSubsystems();
            }
        }
        catch (...)
        {
            if (Logger::IsInitialized())
            {
                Logger::Write(
                    Logger::Level::Error,
                    "Unhandled unknown exception in worker thread");

                ShutdownSubsystems();
            }
        }

        g_running.store(
            false,
            std::memory_order_release);

        g_started.store(
            false,
            std::memory_order_release);

        g_module = nullptr;

        return 0;
    }
}

namespace AmalurCoop
{
    void Start(HMODULE module)
    {
        bool expected = false;

        if (!g_started.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel))
        {
            return;
        }

        g_module = module;

        g_running.store(
            true,
            std::memory_order_release);

        g_workerThread = CreateThread(
            nullptr,
            0,
            MainThread,
            nullptr,
            0,
            nullptr);

        if (!g_workerThread)
        {
            g_running.store(
                false,
                std::memory_order_release);

            g_started.store(
                false,
                std::memory_order_release);

            g_module = nullptr;

            OutputDebugStringA(
                "[AmalurCoop] Failed to create worker thread\n");

            return;
        }

        // Closing the handle does not terminate the thread. The thread
        // continues running until g_running becomes false.
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }

    void Stop()
    {
        g_running.store(
            false,
            std::memory_order_release);
    }
}