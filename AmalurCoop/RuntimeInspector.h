#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RuntimeInspector
{
    struct Record
    {
        int Index = -1;
        uintptr_t Address = 0;
        uint32_t PersistentHandle = 0;
        uint32_t ObjectHandle = 0;
        uint32_t RuntimeState = 0;
        uint16_t Flags = 0;
        uint16_t SpawnFlags = 0;
        uintptr_t DefinitionReference = 0;
        uint32_t DefinitionId = 0;
        uint16_t RuntimeDefinitionRaw = 0;
        uintptr_t RuntimeObject = 0;
        bool Instantiated = false;
        bool IsLocalPlayer = false;
        bool SameDefinitionAsLocal = false;
        bool Pending = false;
    };

    void Initialize();
    void Shutdown();
    void Update();
    void RefreshNow();

    std::string GetStatusText();
    uintptr_t GetManager();
    int GetManagerRecordCount();
    int GetLocalRecordIndex();
    uintptr_t GetLocalRecordAddress();
    uint32_t GetLocalDefinitionId();
    std::vector<Record> GetRecords();

    // Experimental: queues an existing inactive runtime record through the
    // game's normal sub_EA5240 streaming path. The request is rejected unless
    // the record is inactive, non-local, and the explicit unsafe-action gate is enabled.
    bool QueueExistingRecord(int recordIndex, bool requireSameDefinition);
    std::string GetLastActionText();
}
