#include "ResearchLab.h"
#include "Logger.h"
#include "MinHook.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uintptr_t kIdaBase = 0x00400000;

    struct CatalogEntry
    {
        const char* Name;
        uintptr_t Ida;
        const char* Category;
        const char* Description;
        bool Traceable;
        void* Detour;
        void** Original;
        std::atomic<uint64_t> Calls{0};
        bool Hooked = false;
    };

    std::mutex g_mutex;
    std::vector<ResearchLab::TraceEntry> g_trace;
    std::atomic<uint64_t> g_sequence{0};
    std::atomic<bool> g_initialized{false};
    std::atomic<bool> g_unsafeGate{false};
    std::string g_status = "Not initialized";

    using F6CA70Fn = void(__thiscall*)(void*, int);
    using F5DD50Fn = int(__thiscall*)(void*, void*);
    using FE65B0Fn = int(__thiscall*)(void*, int*, int, int, int, int, void*);
    using FE1D80Fn = int(__thiscall*)(void*);
    using E033D0Fn = int(__thiscall*)(void*, int*, int);
    using EF6470Fn = int(__stdcall*)(int, int, int, int, int);
    using E4EC50Fn = int(__thiscall*)(void*, int);
    using F86E20Fn = int*(__thiscall*)(void*, int*, int, void*, int, int, void*, int, int, int*, int, int);
    using FE0860Fn = void*(__stdcall*)(int, int);
    using FE0FE0Fn = void*(__stdcall*)(int, int);
    using FC8150Fn = int(__thiscall*)(int*);
    using FE6950Fn = int(__thiscall*)(void*);
    using FE8D80Fn = int(__thiscall*)(void*);
    using FC7F20Fn = int(__thiscall*)(void*, int*, int, int);
    using FC7E10Fn = int(__thiscall*)(void*, int*, int, int);
    using FD72B0Fn = int(__thiscall*)(void*, int, int);
    using FE5AB0Fn = int(__thiscall*)(void*, int, int, int, int, int);
    using FBEF30Fn = void(__thiscall*)(void*, int, int);
    using FC8790Fn = void(__thiscall*)(void*, int);
    using FE1EB0Fn = char(__thiscall*)(void*, int);

    F6CA70Fn oF6CA70 = nullptr;
    F5DD50Fn oF5DD50 = nullptr;
    FE65B0Fn oFE65B0 = nullptr;
    FE1D80Fn oFE1D80 = nullptr;
    E033D0Fn oE033D0 = nullptr;
    EF6470Fn oEF6470 = nullptr;
    E4EC50Fn oE4EC50 = nullptr;
    F86E20Fn oF86E20 = nullptr;
    FE0860Fn oFE0860 = nullptr;
    FE0FE0Fn oFE0FE0 = nullptr;
    FC8150Fn oFC8150 = nullptr;
    FE6950Fn oFE6950 = nullptr;
    FE8D80Fn oFE8D80 = nullptr;
    FC7F20Fn oFC7F20 = nullptr;
    FC7E10Fn oFC7E10 = nullptr;
    FD72B0Fn oFD72B0 = nullptr;
    FE5AB0Fn oFE5AB0 = nullptr;
    FBEF30Fn oFBEF30 = nullptr;
    FC8790Fn oFC8790 = nullptr;
    FE1EB0Fn oFE1EB0 = nullptr;

    void PushTrace(const char* function, const char* format, ...)
    {
        char details[512]{};
        va_list args;
        va_start(args, format);
        vsnprintf_s(details, sizeof(details), _TRUNCATE, format, args);
        va_end(args);

        ResearchLab::TraceEntry entry;
        entry.Sequence = ++g_sequence;
        entry.ThreadId = GetCurrentThreadId();
        entry.Tick = GetTickCount64();
        entry.Function = function;
        entry.Details = details;

        std::lock_guard<std::mutex> lock(g_mutex);
        g_trace.push_back(std::move(entry));
        if (g_trace.size() > 2048)
            g_trace.erase(g_trace.begin(), g_trace.begin() + 512);
    }

    void __fastcall hF6CA70(void* self, void*, int delta)
    {
        PushTrace("sub_F6CA70", "this=%p secondary-resource delta=%d", self, delta);
        oF6CA70(self, delta);
    }

    int __fastcall hF5DD50(void* self, void*, void* transform)
    {
        float x = 0, y = 0, z = 0;
        __try
        {
            if (transform)
            {
                x = *reinterpret_cast<float*>(transform);
                y = *reinterpret_cast<float*>(static_cast<unsigned char*>(transform) + 4);
                z = *reinterpret_cast<float*>(static_cast<unsigned char*>(transform) + 8);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        PushTrace("sub_F5DD50", "component=%p transform=%p xyz=(%.3f, %.3f, %.3f)", self, transform, x, y, z);
        return oF5DD50(self, transform);
    }

    int __fastcall hFE65B0(void* self, void*, int* definition, int source, int target, int parameter, int runtime, void* context)
    {
        const int def = definition ? *definition : 0;
        PushTrace("sub_FE65B0", "instance=%p def=%08X source=%08X target=%08X param=%08X runtime=%08X context=%p",
            self, def, source, target, parameter, runtime, context);
        return oFE65B0(self, definition, source, target, parameter, runtime, context);
    }

    int __fastcall hFE1D80(void* self, void*)
    {
        PushTrace("sub_FE1D80", "instance=%p stop/reset", self);
        return oFE1D80(self);
    }

    int __fastcall hE033D0(void* self, void*, int* handle, int value)
    {
        PushTrace("sub_E033D0", "registry=%p actor=%08X value=%08X", self, handle ? *handle : 0, value);
        return oE033D0(self, handle, value);
    }

    int __stdcall hEF6470(int spawned, int source, int a3, int a4, int a5)
    {
        PushTrace("sub_EF6470", "spawned=%08X source=%08X p3=%08X p4=%08X flags=%08X", spawned, source, a3, a4, a5);
        return oEF6470(spawned, source, a3, a4, a5);
    }

    int __fastcall hE4EC50(void* self, void*, int type)
    {
        PushTrace("sub_E4EC50", "event=%p type=%d", self, type);
        return oE4EC50(self, type);
    }

    int* __fastcall hF86E20(void* self, void*, int* outActor, int a3, void* definition, int a5, int a6,
        void* transform, int a8, int a9, int* resource, int reuseActor, int a12)
    {
        PushTrace("sub_F86E20", "manager=%p reuse=%08X def=%p source=%08X transform=%p resource=%08X flags=%d",
            self, reuseActor, definition, a5, transform, resource ? *resource : 0, a12);
        int* result = oF86E20(self, outActor, a3, definition, a5, a6, transform, a8, a9, resource, reuseActor, a12);
        PushTrace("sub_F86E20", "result actor=%08X", result ? *result : 0);
        return result;
    }

    void* __stdcall hFE0860(int source, int target)
    {
        PushTrace("sub_FE0860", "action-start source=%08X target=%08X", source, target);
        return oFE0860(source, target);
    }

    void* __stdcall hFE0FE0(int source, int target)
    {
        PushTrace("sub_FE0FE0", "action-end source=%08X target=%08X", source, target);
        return oFE0FE0(source, target);
    }

    int __fastcall hFC8150(int* self, void*)
    {
        PushTrace("sub_FC8150", "instance=%p remove execution registration", self);
        return oFC8150(self);
    }

    int __fastcall hFE6950(void* self, void*)
    {
        PushTrace("sub_FE6950", "instance=%p collect affected actors", self);
        const int result = oFE6950(self);
        PushTrace("sub_FE6950", "instance=%p result=%d", self, result);
        return result;
    }

    int __fastcall hFE8D80(void* self, void*)
    {
        PushTrace("sub_FE8D80", "instance=%p spawn associated actor", self);
        const int result = oFE8D80(self);
        PushTrace("sub_FE8D80", "instance=%p result=%08X", self, result);
        return result;
    }

    int __fastcall hFC7F20(void* self, void*, int* definition, int source, int target)
    {
        PushTrace("sub_FC7F20", "definition=%08X source=%08X target=%08X", definition ? *definition : 0, source, target);
        return oFC7F20(self, definition, source, target);
    }

    int __fastcall hFC7E10(void* self, void*, int* definition, int source, int target)
    {
        PushTrace("sub_FC7E10", "definition=%08X source=%08X target=%08X", definition ? *definition : 0, source, target);
        return oFC7E10(self, definition, source, target);
    }

    int __fastcall hFD72B0(void* self, void*, int value, int mode)
    {
        const int slot = oFD72B0(self, value, mode);
        PushTrace("sub_FD72B0", "manager=%p value=%08X mode=%d slot=%d", self, value, mode, slot);
        return slot;
    }

    int __fastcall hFE5AB0(void* self, void*, int source, int target, int outList, int context, int mode)
    {
        PushTrace("sub_FE5AB0", "definition=%p type=%d source=%08X target=%08X out=%08X context=%08X mode=%d",
            self, self ? *reinterpret_cast<int*>(static_cast<unsigned char*>(self) + 416) : -1, source, target, outList, context, mode);
        return oFE5AB0(self, source, target, outList, context, mode);
    }

    void __fastcall hFBEF30(void* self, void*, int actor, int actionId)
    {
        PushTrace("sub_FBEF30", "definition=%p actor=%08X action=%08X remove granted talents", self, actor, actionId);
        oFBEF30(self, actor, actionId);
    }

    void __fastcall hFC8790(void* self, void*, int eventContext)
    {
        PushTrace("sub_FC8790", "dispatcher=%p event=%08X", self, eventContext);
        oFC8790(self, eventContext);
    }

    char __fastcall hFE1EB0(void* self, void*, int instanceId)
    {
        PushTrace("sub_FE1EB0", "manager=%p stop instance=%d", self, instanceId);
        return oFE1EB0(self, instanceId);
    }

    CatalogEntry g_catalog[] =
    {
        {"sub_F817C0", 0x00F817C0, "Actor", "Acquire pooled/new actor object", false, nullptr, nullptr},
        {"sub_F79010", 0x00F79010, "Actor", "Actor object constructor", false, nullptr, nullptr},
        {"sub_F1AF50", 0x00F1AF50, "Actor", "Reset base actor state", false, nullptr, nullptr},
        {"sub_F86E20", 0x00F86E20, "Actor", "Core actor create/reinitialize path", true, reinterpret_cast<void*>(&hF86E20), reinterpret_cast<void**>(&oF86E20)},
        {"sub_FC6170", 0x00FC6170, "Player", "Player-style actor spawn wrapper used by DD15C0", false, nullptr, nullptr},
        {"sub_FC6220", 0x00FC6220, "Actor", "Limited actor/entity duplication wrapper; not a full player clone", false, nullptr, nullptr},
        {"sub_DD15C0", 0x00DD15C0, "Player", "Create player actor and apply player-specific post-spawn setup", false, nullptr, nullptr},
        {"sub_FBC830", 0x00FBC830, "Player", "Ensure/create primary player actor through DD15C0", false, nullptr, nullptr},
        {"sub_F6A660", 0x00F6A660, "Player", "Apply player transform/component-six post-spawn setup", false, nullptr, nullptr},
        {"sub_F7A270", 0x00F7A270, "Player", "Player-context owner binding path; unsafe on a raw proxy actor pointer", false, nullptr, nullptr},
        {"sub_FD5ED0", 0x00FD5ED0, "Actor", "Register actor with manager/system", false, nullptr, nullptr},
        {"sub_F323A0", 0x00F323A0, "Actor", "Late model/render finalization", false, nullptr, nullptr},
        {"sub_D70410", 0x00D70410, "Animation", "Resolve actor-state entry and attach live runtime", false, nullptr, nullptr},
        {"sub_D70180", 0x00D70180, "Animation", "Attach live actor runtime and build child animation hierarchy", false, nullptr, nullptr},
        {"sub_D6EFE0", 0x00D6EFE0, "Animation", "Build per-node runtime objects for character hierarchy", false, nullptr, nullptr},
        {"sub_D632B0", 0x00D632B0, "Animation", "Bind runtime hierarchy entries into live actor object", false, nullptr, nullptr},
        {"sub_D6F230", 0x00D6F230, "Animation", "Build/register additional live character runtime objects", false, nullptr, nullptr},
        {"sub_D1D840", 0x00D1D840, "Animation", "Motion request and resolver entry used for network replay", false, nullptr, nullptr},
        {"sub_D1D1C0", 0x00D1D1C0, "Animation", "Queue constructed 0x84-byte runtime motion command", false, nullptr, nullptr},
        {"sub_F8D070", 0x00F8D070, "Death", "Actor death pipeline", false, nullptr, nullptr},
        {"sub_F57450", 0x00F57450, "Death", "Actor respawn pipeline", false, nullptr, nullptr},
        {"sub_DED360", 0x00DED360, "Death", "Build corpse/death snapshot", false, nullptr, nullptr},
        {"sub_DE9560", 0x00DE9560, "Death", "Append corpse snapshot record", false, nullptr, nullptr},
        {"sub_F6CA70", 0x00F6CA70, "Resource", "Modify bounded secondary resource", true, reinterpret_cast<void*>(&hF6CA70), reinterpret_cast<void**>(&oF6CA70)},
        {"sub_F8FED0", 0x00F8FED0, "Resource", "Modify primary health/resource", false, nullptr, nullptr},
        {"sub_F418A0", 0x00F418A0, "Resource", "Recalculate resource intervals", false, nullptr, nullptr},
        {"sub_F5DD50", 0x00F5DD50, "Transform", "Set actor transform through engine path", true, reinterpret_cast<void*>(&hF5DD50), reinterpret_cast<void**>(&oF5DD50)},
        {"sub_F3BCF0", 0x00F3BCF0, "Transform", "Rebuild derived transform", false, nullptr, nullptr},
        {"sub_F23ED0", 0x00F23ED0, "Replication", "Submit/propagate component update", false, nullptr, nullptr},
        {"sub_FCDFD0", 0x00FCDFD0, "Action", "Validate action request", false, nullptr, nullptr},
        {"sub_FE1F20", 0x00FE1F20, "Action", "Acquire reusable action instance", false, nullptr, nullptr},
        {"sub_FE65B0", 0x00FE65B0, "Action", "Initialize action instance", true, reinterpret_cast<void*>(&hFE65B0), reinterpret_cast<void**>(&oFE65B0)},
        {"sub_FEB340", 0x00FEB340, "Action", "Refresh action runtime state", false, nullptr, nullptr},
        {"sub_FE5AB0", 0x00FE5AB0, "Action", "Dispatch affected-actor query by action type", true, reinterpret_cast<void*>(&hFE5AB0), reinterpret_cast<void**>(&oFE5AB0)},
        {"sub_FE6950", 0x00FE6950, "Action", "Collect affected actors", true, reinterpret_cast<void*>(&hFE6950), reinterpret_cast<void**>(&oFE6950)},
        {"sub_FE8D80", 0x00FE8D80, "Action", "Spawn action-associated actor", true, reinterpret_cast<void*>(&hFE8D80), reinterpret_cast<void**>(&oFE8D80)},
        {"sub_EF6470", 0x00EF6470, "Action", "Link spawned actor (wrapper to FE8590)", true, reinterpret_cast<void*>(&hEF6470), reinterpret_cast<void**>(&oEF6470)},
        {"sub_FE1EB0", 0x00FE1EB0, "Action", "Stop active action instance by ID", true, reinterpret_cast<void*>(&hFE1EB0), reinterpret_cast<void**>(&oFE1EB0)},
        {"sub_FE1D80", 0x00FE1D80, "Action", "Stop and reset action instance", true, reinterpret_cast<void*>(&hFE1D80), reinterpret_cast<void**>(&oFE1D80)},
        {"sub_FC8150", 0x00FC8150, "Action", "Remove source/target execution registration", true, reinterpret_cast<void*>(&hFC8150), reinterpret_cast<void**>(&oFC8150)},
        {"sub_FD72B0", 0x00FD72B0, "Action", "Allocate runtime state/timing slot", true, reinterpret_cast<void*>(&hFD72B0), reinterpret_cast<void**>(&oFD72B0)},
        {"sub_FE0860", 0x00FE0860, "Event", "Dispatch action-started event", true, reinterpret_cast<void*>(&hFE0860), reinterpret_cast<void**>(&oFE0860)},
        {"sub_FE0FE0", 0x00FE0FE0, "Event", "Dispatch action-ended event", true, reinterpret_cast<void*>(&hFE0FE0), reinterpret_cast<void**>(&oFE0FE0)},
        {"sub_E4EC50", 0x00E4EC50, "Event", "Set event type", true, reinterpret_cast<void*>(&hE4EC50), reinterpret_cast<void**>(&oE4EC50)},
        {"sub_FC8790", 0x00FC8790, "Event", "Dispatch event to listeners", true, reinterpret_cast<void*>(&hFC8790), reinterpret_cast<void**>(&oFC8790)},
        {"sub_E033D0", 0x00E033D0, "Modifier", "Register actor modifier/effect record", true, reinterpret_cast<void*>(&hE033D0), reinterpret_cast<void**>(&oE033D0)},
        {"sub_FC7F20", 0x00FC7F20, "Modifier", "Register action callbacks", true, reinterpret_cast<void*>(&hFC7F20), reinterpret_cast<void**>(&oFC7F20)},
        {"sub_FC7E10", 0x00FC7E10, "Modifier", "Unregister action callbacks", true, reinterpret_cast<void*>(&hFC7E10), reinterpret_cast<void**>(&oFC7E10)},
        {"sub_FBEF30", 0x00FBEF30, "Modifier", "Remove action-granted talent records", true, reinterpret_cast<void*>(&hFBEF30), reinterpret_cast<void**>(&oFBEF30)},
        {"sub_D3EC40", 0x00D3EC40, "Action", "Validated action selection/execution routing", false, nullptr, nullptr},
        {"sub_F31340", 0x00F31340, "Items", "Build basic item display name", false, nullptr, nullptr},
        {"sub_F31420", 0x00F31420, "Items", "Build affixed item display name", false, nullptr, nullptr},
    };

    CatalogEntry* Find(const std::string& name)
    {
        for (auto& entry : g_catalog)
            if (name == entry.Name)
                return &entry;
        return nullptr;
    }
}

namespace ResearchLab
{
    uintptr_t RuntimeAddress(uintptr_t idaAddress)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        return (base && idaAddress >= kIdaBase) ? base + (idaAddress - kIdaBase) : 0;
    }

    void Initialize()
    {
        g_initialized = true;
        g_status = "Ready; trace hooks are opt-in";
        Logger::Write(Logger::Level::Success, "ResearchLab initialized (all gameplay traces disabled by default)");
    }

    void Shutdown()
    {
        DisableAllTraces();
        g_initialized = false;
        g_status = "Shutdown";
    }

    std::vector<FunctionInfo> GetFunctions()
    {
        std::vector<FunctionInfo> output;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& e : g_catalog)
        {
            uint64_t calls = 0;
            for (const auto& trace : g_trace)
                if (trace.Function == e.Name)
                    ++calls;
            output.push_back({e.Name, e.Ida, e.Category, e.Description, e.Traceable, e.Hooked, calls});
        }
        return output;
    }

    std::vector<TraceEntry> GetTraceEntries(size_t maximum)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const size_t begin = g_trace.size() > maximum ? g_trace.size() - maximum : 0;
        return std::vector<TraceEntry>(g_trace.begin() + begin, g_trace.end());
    }

    void ClearTrace()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_trace.clear();
    }

    bool EnableTrace(const std::string& name)
    {
        CatalogEntry* e = Find(name);
        if (!e || !e->Traceable || e->Hooked || !e->Detour || !e->Original)
            return e && e->Hooked;
        void* target = reinterpret_cast<void*>(RuntimeAddress(e->Ida));
        if (!target)
            return false;
        const MH_STATUS create = MH_CreateHook(target, e->Detour, e->Original);
        if (create != MH_OK && create != MH_ERROR_ALREADY_CREATED)
        {
            g_status = std::string("CreateHook failed for ") + name;
            return false;
        }
        const MH_STATUS enable = MH_EnableHook(target);
        if (enable != MH_OK && enable != MH_ERROR_ENABLED)
        {
            g_status = std::string("EnableHook failed for ") + name;
            return false;
        }
        e->Hooked = true;
        g_status = std::string("Tracing ") + name;
        Logger::Write("ResearchLab trace enabled: " + name);
        return true;
    }

    bool DisableTrace(const std::string& name)
    {
        CatalogEntry* e = Find(name);
        if (!e || !e->Hooked)
            return true;
        void* target = reinterpret_cast<void*>(RuntimeAddress(e->Ida));
        MH_DisableHook(target);
        MH_RemoveHook(target);
        e->Hooked = false;
        Logger::Write("ResearchLab trace disabled: " + name);
        return true;
    }

    void DisableAllTraces()
    {
        for (auto& e : g_catalog)
            if (e.Hooked)
                DisableTrace(e.Name);
    }

    bool IsAnyTraceEnabled()
    {
        for (auto& e : g_catalog)
            if (e.Hooked)
                return true;
        return false;
    }

    bool IsUnsafeGateEnabled() { return g_unsafeGate.load(); }
    void SetUnsafeGateEnabled(bool enabled) { g_unsafeGate = enabled; }
    std::string GetStatusText() { return g_status; }
}
