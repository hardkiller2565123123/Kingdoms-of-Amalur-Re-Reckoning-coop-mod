#include "MemoryScanner.h"

#include "Logger.h"
#include "Paths.h"

#include <Windows.h>
#include <Psapi.h>

#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <mutex>
#include <cmath>

#pragma comment(lib, "Psapi.lib")

namespace
{
    std::vector<MemoryScanner::ScanResult> g_floatResults;
    std::mutex g_scanMutex;

    std::atomic<bool> g_scanRunning = false;
    std::atomic<bool> g_autoScanStarted = false;

    std::string g_scanStatus = "Idle";

    float g_scanTarget = 0.0f;
    bool g_scanIsFirst = true;

    HANDLE g_scanThread = nullptr;
    HANDLE g_autoScanThread = nullptr;

    bool FloatClose(float a, float b)
    {
        return std::fabs(a - b) <= 0.001f;
    }

    void SetStatus(const std::string& status)
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanStatus = status;
    }

    std::vector<MemoryScanner::ScanResult> CopyResults()
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        return g_floatResults;
    }

    void ReplaceResults(const std::vector<MemoryScanner::ScanResult>& results)
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_floatResults = results;
    }

    std::string AutoScanLogPath()
    {
        return Paths::GetLogsFolder() + "\\AutoScan.log";
    }

    void AutoLog(const std::string& text)
    {
        CreateDirectoryA(Paths::GetModFolder().c_str(), nullptr);
        CreateDirectoryA(Paths::GetLogsFolder().c_str(), nullptr);

        std::ofstream file(AutoScanLogPath(), std::ios::app);
        file << text << "\n";
    }

    bool IsGoodFloat(float value)
    {
        if (!std::isfinite(value))
            return false;

        if (value < -1000000.0f || value > 1000000.0f)
            return false;

        return true;
    }

    void ScanModuleForFloat(
        const MemoryScanner::ModuleInfo& mod,
        float target,
        std::vector<MemoryScanner::ScanResult>& results,
        size_t maxResults
    )
    {
        uintptr_t start = mod.Base;
        uintptr_t end = mod.Base + mod.Size;

        for (uintptr_t address = start; address < end; address += sizeof(float))
        {
            float current = 0.0f;

            if (!MemoryScanner::ReadFloat(address, current))
                continue;

            if (!IsGoodFloat(current))
                continue;

            if (FloatClose(current, target))
            {
                MemoryScanner::ScanResult result{};
                result.Address = address;
                result.Value = current;
                results.push_back(result);

                if (results.size() >= maxResults)
                    return;
            }

            if ((address & 0xFFFF) == 0)
                Sleep(1);
        }
    }

    DWORD WINAPI FloatScanThread(LPVOID)
    {
        g_scanRunning.store(true);

        if (g_scanIsFirst)
        {
            SetStatus("Scanning koa.exe for float " + std::to_string(g_scanTarget));

            std::vector<MemoryScanner::ScanResult> results;
            auto modules = MemoryScanner::GetLoadedModules();

            for (const auto& mod : modules)
            {
                if (mod.Name != "koa.exe")
                    continue;

                ScanModuleForFloat(mod, g_scanTarget, results, 25000);
                break;
            }

            ReplaceResults(results);
            SetStatus("Scan complete. Results: " + std::to_string(results.size()));
        }
        else
        {
            SetStatus("Filtering previous results for " + std::to_string(g_scanTarget));

            std::vector<MemoryScanner::ScanResult> oldResults = CopyResults();
            std::vector<MemoryScanner::ScanResult> filtered;

            for (const auto& result : oldResults)
            {
                float current = 0.0f;

                if (!MemoryScanner::ReadFloat(result.Address, current))
                    continue;

                if (!IsGoodFloat(current))
                    continue;

                if (FloatClose(current, g_scanTarget))
                {
                    MemoryScanner::ScanResult next{};
                    next.Address = result.Address;
                    next.Value = current;
                    filtered.push_back(next);
                }
            }

            ReplaceResults(filtered);
            SetStatus("Filter complete. Results: " + std::to_string(filtered.size()));
        }

        g_scanRunning.store(false);
        return 0;
    }

    void StartScan(float value, bool first)
    {
        if (g_scanRunning.load())
        {
            std::cout << "[Scan] Already running. Type scanstatus.\n";
            return;
        }

        g_scanTarget = value;
        g_scanIsFirst = first;

        g_scanThread = CreateThread(
            nullptr,
            0,
            FloatScanThread,
            nullptr,
            0,
            nullptr
        );

        if (g_scanThread)
        {
            CloseHandle(g_scanThread);
            g_scanThread = nullptr;
        }
        else
        {
            std::cout << "[Scan] Failed to start scan thread\n";
        }
    }

    DWORD WINAPI AutoScanThread(LPVOID)
    {
        Sleep(5000);

        AutoLog("========================================");
        AutoLog("AmalurCoop Auto Scan Started");
        AutoLog("========================================");

        auto modules = MemoryScanner::GetLoadedModules();

        MemoryScanner::ModuleInfo gameModule{};
        bool foundGame = false;

        for (const auto& mod : modules)
        {
            if (mod.Name == "koa.exe")
            {
                gameModule = mod;
                foundGame = true;
                break;
            }
        }

        if (!foundGame)
        {
            AutoLog("[AutoScan] koa.exe not found");
            SetStatus("AutoScan failed: koa.exe not found");
            return 0;
        }

        AutoLog("[AutoScan] koa.exe Base: 0x" + std::to_string(gameModule.Base));
        AutoLog("[AutoScan] koa.exe Size: " + std::to_string(gameModule.Size));

        const float targets[] =
        {
            100.0f,
            1.0f,
            0.0f
        };

        for (float target : targets)
        {
            std::vector<MemoryScanner::ScanResult> results;

            SetStatus("AutoScan searching for " + std::to_string(target));
            AutoLog("");
            AutoLog("[AutoScan] Searching for float " + std::to_string(target));

            ScanModuleForFloat(gameModule, target, results, 500);

            AutoLog("[AutoScan] Results: " + std::to_string(results.size()));

            int printed = 0;

            for (const auto& result : results)
            {
                char line[256]{};

                sprintf_s(
                    line,
                    "0x%08X = %.3f",
                    static_cast<unsigned int>(result.Address),
                    result.Value
                );

                AutoLog(line);

                printed++;

                if (printed >= 100)
                    break;
            }
        }

        SetStatus("AutoScan complete. See logs\\AutoScan.log");
        AutoLog("");
        AutoLog("[AutoScan] Complete");

        return 0;
    }
}

namespace MemoryScanner
{
    void Initialize()
    {
        Logger::Write("MemoryScanner initialized");
    }

    void Shutdown()
    {
        Logger::Write("MemoryScanner shutdown");
    }

    std::vector<ModuleInfo> GetLoadedModules()
    {
        std::vector<ModuleInfo> modules;

        HMODULE moduleHandles[1024]{};
        DWORD needed = 0;

        HANDLE process = GetCurrentProcess();

        if (!EnumProcessModules(process, moduleHandles, sizeof(moduleHandles), &needed))
        {
            Logger::Write("EnumProcessModules failed");
            return modules;
        }

        unsigned int count = needed / sizeof(HMODULE);

        for (unsigned int i = 0; i < count; i++)
        {
            char name[MAX_PATH]{};
            MODULEINFO info{};

            GetModuleBaseNameA(process, moduleHandles[i], name, MAX_PATH);

            if (!GetModuleInformation(process, moduleHandles[i], &info, sizeof(info)))
                continue;

            ModuleInfo mod{};
            mod.Name = name;
            mod.Base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
            mod.Size = static_cast<size_t>(info.SizeOfImage);

            modules.push_back(mod);
        }

        return modules;
    }

    bool IsReadableAddress(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION mbi{};

        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & PAGE_NOACCESS)
            return false;

        if (mbi.Protect & PAGE_GUARD)
            return false;

        return true;
    }

    bool ReadFloat(uintptr_t address, float& outValue)
    {
        if (!IsReadableAddress(address))
            return false;

        __try
        {
            outValue = *reinterpret_cast<float*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadInt(uintptr_t address, int& outValue)
    {
        if (!IsReadableAddress(address))
            return false;

        __try
        {
            outValue = *reinterpret_cast<int*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void PrintModules()
    {
        auto modules = GetLoadedModules();

        std::cout << "\nLoaded Modules\n";
        std::cout << "--------------\n";

        for (const auto& mod : modules)
        {
            std::cout
                << "  "
                << std::left
                << std::setw(32)
                << mod.Name
                << " Base: 0x"
                << std::hex
                << mod.Base
                << " Size: 0x"
                << mod.Size
                << std::dec
                << "\n";
        }

        std::cout << "\n";
    }

    void WatchAddress(uintptr_t address)
    {
        float floatValue = 0.0f;
        int intValue = 0;

        std::cout << "\n[Watch] 0x"
            << std::hex
            << address
            << std::dec
            << "\n";

        if (ReadFloat(address, floatValue))
            std::cout << "  float: " << floatValue << "\n";
        else
            std::cout << "  float: unreadable\n";

        if (ReadInt(address, intValue))
            std::cout << "  int:   " << intValue << "\n";
        else
            std::cout << "  int:   unreadable\n";

        std::cout << "\n";
    }

    void ResetFloatScan()
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_floatResults.clear();
        g_scanStatus = "Idle";

        std::cout << "[Scan] Float scan reset\n";
        Logger::Write("Float scan reset");
    }

    void FirstFloatScan(float value)
    {
        std::cout << "[Scan] Started first scan for float " << value << "\n";
        StartScan(value, true);
    }

    void NextFloatScan(float value)
    {
        if (GetFloatScanCount() == 0)
        {
            std::cout << "[Scan] No existing results. Use scanfloat first.\n";
            return;
        }

        std::cout << "[Scan] Started filter scan for float " << value << "\n";
        StartScan(value, false);
    }

    void PrintFloatScanResults(int maxResults)
    {
        auto results = CopyResults();

        std::cout << "\n[Float Scan Results]\n";
        std::cout << "Status: " << GetScanStatus() << "\n";
        std::cout << "Count: " << results.size() << "\n";

        int printed = 0;

        for (const auto& result : results)
        {
            if (printed >= maxResults)
                break;

            std::cout << "  0x"
                << std::hex
                << result.Address
                << std::dec
                << " = "
                << result.Value
                << "\n";

            printed++;
        }

        if (results.size() > static_cast<size_t>(maxResults))
            std::cout << "  ...more results hidden\n";

        std::cout << "\n";
    }

    std::vector<ScanResult> GetFloatScanResults(size_t maxResults)
    {
        auto results = CopyResults();
        if (results.size() > maxResults)
            results.resize(maxResults);
        return results;
    }

    size_t GetFloatScanCount()
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        return g_floatResults.size();
    }

    bool IsScanRunning()
    {
        return g_scanRunning.load();
    }

    std::string GetScanStatus()
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        return g_scanStatus;
    }

    void StartAutoScan()
    {
        if (g_autoScanStarted.load())
            return;

        g_autoScanStarted.store(true);

        g_autoScanThread = CreateThread(
            nullptr,
            0,
            AutoScanThread,
            nullptr,
            0,
            nullptr
        );

        if (g_autoScanThread)
        {
            CloseHandle(g_autoScanThread);
            g_autoScanThread = nullptr;
        }

        Logger::Write("AutoScan thread started");
    }
}