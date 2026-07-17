#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ResearchLab
{
    struct FunctionInfo
    {
        const char* Name;
        uintptr_t IdaAddress;
        const char* Category;
        const char* Description;
        bool Traceable;
        bool Hooked;
        uint64_t Calls;
    };

    struct TraceEntry
    {
        uint64_t Sequence = 0;
        uint32_t ThreadId = 0;
        uint64_t Tick = 0;
        std::string Function;
        std::string Details;
    };

    void Initialize();
    void Shutdown();

    uintptr_t RuntimeAddress(uintptr_t idaAddress);
    std::vector<FunctionInfo> GetFunctions();
    std::vector<TraceEntry> GetTraceEntries(size_t maximum = 256);
    void ClearTrace();

    bool EnableTrace(const std::string& functionName);
    bool DisableTrace(const std::string& functionName);
    void DisableAllTraces();
    bool IsAnyTraceEnabled();

    bool IsUnsafeGateEnabled();
    void SetUnsafeGateEnabled(bool enabled);

    std::string GetStatusText();
}
