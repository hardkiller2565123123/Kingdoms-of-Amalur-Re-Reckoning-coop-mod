#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace MemoryScanner
{
    struct ModuleInfo
    {
        std::string Name;
        uintptr_t Base = 0;
        size_t Size = 0;
    };

    struct ScanResult
    {
        uintptr_t Address = 0;
        float Value = 0.0f;
    };

    void Initialize();
    void Shutdown();

    std::vector<ModuleInfo> GetLoadedModules();

    bool IsReadableAddress(uintptr_t address);
    bool ReadFloat(uintptr_t address, float& outValue);
    bool ReadInt(uintptr_t address, int& outValue);

    void PrintModules();
    void WatchAddress(uintptr_t address);

    void ResetFloatScan();
    void FirstFloatScan(float value);
    void NextFloatScan(float value);
    void PrintFloatScanResults(int maxResults = 50);
    std::vector<ScanResult> GetFloatScanResults(size_t maxResults = 50);
    size_t GetFloatScanCount();

    bool IsScanRunning();
    std::string GetScanStatus();

    void StartAutoScan();
}