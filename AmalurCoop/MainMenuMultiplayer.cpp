#include "MainMenuMultiplayer.h"
#include "MainMenuScript.h"
#include "EngineRuntime.h"
#include "NetworkManager.h"
#include "Config.h"
#include "Logger.h"
#include "MinHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // Verified x86 cdecl ABIs in the mapped koa.exe. 10D5960 (used by the
    // game's script loader) forwards to this five-argument load-buffer API.
    using LoadBuffer = int(__cdecl*)(void*, const void*, const char*, unsigned, const char*);
    using Print = int(__cdecl*)(void*);
    LoadBuffer g_load = nullptr;
    Print g_print = nullptr;
    void* g_loadTarget = nullptr;
    void* g_printTarget = nullptr;
    std::atomic<bool> g_ready{ false };
    enum class Command { None, Host, Join };
    std::atomic<Command> g_command{ Command::None };

    void Append32(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
        out.push_back(static_cast<std::uint8_t>(value >> 24));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }
    void AppendString(std::vector<std::uint8_t>& out, const char* text)
    {
        out.push_back(4); // HKS string constant
        const auto length = std::strlen(text) + 1;
        Append32(out, static_cast<std::uint32_t>(length));
        out.insert(out.end(), text, text + length);
    }
    std::uint32_t ABC(unsigned op, unsigned a, unsigned b, unsigned c)
    { return (op << 25) | (b << 17) | (c << 8) | a; }
    std::uint32_t ABx(unsigned op, unsigned a, unsigned bx)
    { return (op << 25) | (bx << 8) | a; }

    bool PatchMainMenu(const char* buffer, unsigned size, std::vector<std::uint8_t>& out)
    {
        // Exact fingerprints of base and patched main_menu assets. The current
        // installed game overrides initial_0.pak's version (67 vs 66 functions).
        // Fixed offsets are used only after the entire original chunk matches.
        // No assets are replaced on disk and other Lua chunks are untouched.
        if ((size != 56302 && size != 56770) || !buffer) return false;
        std::uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < size; ++i) hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 16777619u;
        const bool baseMenu = size == 56302 && hash == 0x1898DF81u;
        const bool updatedMenu = size == 56770 && hash == 0x83744CB7u;
        if (!baseMenu && !updatedMenu) return false;
        const unsigned instructions = baseMenu ? 713 : 719;
        const unsigned constants = baseMenu ? 304 : 307;
        const unsigned returnOffset = baseMenu ? 3119 : 3143;
        const unsigned constantsStart = returnOffset + 8;
        const unsigned childrenStart = baseMenu ? 8013 : 8125;
        out.reserve(size + sizeof(kMultiplayerMenuScript) + 128);
        out.insert(out.end(), buffer, buffer + 267);
        Append32(out, instructions + 7); // replace final RET with 8 instructions
        out.insert(out.end(), buffer + 271, buffer + returnOffset);
        // pcall(loadstring(our_source)): original module runs first; protected
        // patch execution cannot abort the original menu's initialization.
        Append32(out, ABx(85, 0, constants + 4)); // GETGLOBAL_MEM pcall
        Append32(out, ABx(70, 20, constants + 5)); // DATA, reserved memoization constants
        Append32(out, ABx(85, 1, constants)); // GETGLOBAL_MEM loadstring
        Append32(out, ABx(70, 20, constants + 1));
        Append32(out, ABx(25, 2, constants + 3)); // LOADK source
        Append32(out, ABC(2, 1, 2, 2)); // CALL loadstring, one result
        Append32(out, ABC(2, 0, 2, 1)); // CALL pcall, no results
        Append32(out, ABC(9, 0, 1, 0)); // RETURN
        Append32(out, constants + 7);
        out.insert(out.end(), buffer + constantsStart, buffer + childrenStart);
        AppendString(out, "loadstring"); out.push_back(0); out.push_back(0);
        AppendString(out, kMultiplayerMenuScript);
        AppendString(out, "pcall"); out.push_back(0); out.push_back(0);
        out.insert(out.end(), buffer + childrenStart, buffer + size);
        return true;
    }

    int LoadOwnSource(void* state, const void* settings, const char* bytes, unsigned size, const char* name)
    {
        // 10D434A rejects source when global_State+DC is 2 (secure state).
        // This path is restricted to our exact embedded source. Restore the
        // state mode even on an exception; other game scripts keep their mode.
        auto* global = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(state) + 8);
        auto* mode = reinterpret_cast<unsigned*>(global + 0xDC);
        const unsigned saved = *mode;
        int result = -1;
        __try
        {
            if (saved == 2) *mode = 1;
            result = g_load(state, settings, bytes, size, name);
        }
        __finally { *mode = saved; }
        return result;
    }

    int __cdecl HookLoad(void* state, const void* settings, const char* bytes, unsigned size, const char* name)
    {
        if (bytes && size == sizeof(kMultiplayerMenuScript) - 1 &&
            !std::memcmp(bytes, kMultiplayerMenuScript, size))
        {
            const int result = LoadOwnSource(state, settings, bytes, size, name);
            Logger::WriteFormat(result ? Logger::Level::Error : Logger::Level::Info,
                "Native Multiplayer: embedded source compiler result=%d", result);
            return result;
        }
        std::vector<std::uint8_t> patched;
        if (!PatchMainMenu(bytes, size, patched)) return g_load(state, settings, bytes, size, name);
        Logger::WriteFormat(Logger::Level::Info, "Native Multiplayer: verified main_menu chunk patched (%u bytes); waiting for script initialization", size);
        const int result = g_load(state, settings, reinterpret_cast<const char*>(patched.data()), static_cast<unsigned>(patched.size()), name);
        if (result) Logger::WriteFormat(Logger::Level::Error, "Native Multiplayer: patched chunk load failed, result=%d", result);
        return result;
    }

    // Read only one literal string argument; conversion of arbitrary Lua values
    // is deliberately left to the game's original print implementation.
    bool ReadCommand(void* state, char (&command)[512])
    {
        __try
        {
            const auto address = reinterpret_cast<uintptr_t>(state);
            const auto top = *reinterpret_cast<uintptr_t*>(address + 0x24);
            const auto first = *reinterpret_cast<uintptr_t*>(address + 0x28);
            if (!first || top - first != 8 || (*reinterpret_cast<unsigned*>(first) & 15) != 4) return false;
            using ToString = const char*(__cdecl*)(void*, void*, unsigned*);
            unsigned length = 0;
            const char* text = reinterpret_cast<ToString>(EngineRuntime::RuntimeAddress(0x010C01F0))(state, reinterpret_cast<void*>(first), &length);
            if (!text || length >= sizeof(command)) return false;
            std::memcpy(command, text, length); command[length] = 0;
            return std::strncmp(command, "__AmalurCoop:", 13) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int ReturnString(void* state, const std::string& text)
    {
        // AA3340 is the verified push-lstring API: it writes a TValue at L+24
        // and advances the stack by 8. One callback result is returned to Lua.
        using PushString = void(__cdecl*)(void*, const char*, unsigned);
        reinterpret_cast<PushString>(EngineRuntime::RuntimeAddress(0x00AA3340))(state, text.data(), static_cast<unsigned>(text.size()));
        return 1;
    }
    int __cdecl HookPrint(void* state)
    {
        char command[512]{};
        if (!ReadCommand(state, command)) return g_print(state);
        if (std::strncmp(command, "__AmalurCoop:error:", 19) == 0)
        {
            Logger::WriteFormat(Logger::Level::Error, "Native Multiplayer script error: %s", command + 19);
            return 0;
        }
        if (std::strcmp(command, "__AmalurCoop:ready") == 0)
        {
            Logger::Write(Logger::Level::Success, "Native Multiplayer button tree initialized: Multiplayer -> Host / Join");
            return 0;
        }
        if (std::strcmp(command, "__AmalurCoop:status") == 0)
            return ReturnString(state, NetworkManager::GetStatusText());
        Command next = Command::None;
        if (std::strcmp(command, "__AmalurCoop:host") == 0) next = Command::Host;
        if (std::strcmp(command, "__AmalurCoop:join") == 0) next = Command::Join;
        if (next == Command::None) return g_print(state);
        g_command.store(next, std::memory_order_release);
        return ReturnString(state, next == Command::Host ? "Starting host..." : "Connecting...");
    }
    template <std::size_t N>
    bool Matches(uintptr_t address, const std::uint8_t (&expected)[N])
    {
        std::uint8_t actual[N]{};
        return EngineRuntime::ReadBytes(address, actual, N) && !std::memcmp(actual, expected, N);
    }
    void RemoveHooks()
    {
        if (g_loadTarget) { MH_DisableHook(g_loadTarget); MH_RemoveHook(g_loadTarget); }
        if (g_printTarget) { MH_DisableHook(g_printTarget); MH_RemoveHook(g_printTarget); }
        g_loadTarget = g_printTarget = nullptr;
    }
}

namespace MainMenuMultiplayer
{
    bool Initialize()
    {
        if (g_ready.load()) return true;
        const auto load = EngineRuntime::RuntimeAddress(0x010D5860);
        const auto print = EngineRuntime::RuntimeAddress(0x00AA9470);
        const std::uint8_t loadBytes[]{ 0x81, 0xEC, 0x10, 0x01, 0, 0 };
        const std::uint8_t printBytes[]{ 0x83, 0xEC, 0x20, 0x8B, 0x44, 0x24, 0x24 };
        if (!Matches(load, loadBytes) || !Matches(print, printBytes))
        {
            Logger::Write(Logger::Level::Error, "Native Multiplayer: Lua ABI mismatch; menu hooks disabled");
            return false;
        }
        // Record ownership only after creation succeeds. A failed create must
        // not remove a hook that another component already owns.
        if (MH_CreateHook(reinterpret_cast<void*>(print), reinterpret_cast<void*>(&HookPrint), reinterpret_cast<void**>(&g_print)) == MH_OK)
            g_printTarget = reinterpret_cast<void*>(print);
        if (g_printTarget && MH_CreateHook(reinterpret_cast<void*>(load), reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_load)) == MH_OK)
            g_loadTarget = reinterpret_cast<void*>(load);
        if (!g_printTarget || !g_loadTarget ||
            MH_EnableHook(g_printTarget) != MH_OK || MH_EnableHook(g_loadTarget) != MH_OK)
        {
            RemoveHooks();
            Logger::Write(Logger::Level::Error, "Native Multiplayer: could not install Lua hooks");
            return false;
        }
        g_ready.store(true);
        Logger::Write("Native Multiplayer hooks ready; awaiting the verified main_menu asset");
        return true;
    }
    void PumpNetworkCommands()
    {
        const auto command = g_command.exchange(Command::None, std::memory_order_acq_rel);
        if (command == Command::None) return;
        const auto settings = Config::Get();
        if (command == Command::Host) NetworkManager::Host(static_cast<unsigned short>(settings.ServerPort));
        else NetworkManager::Join(settings.ServerAddress, static_cast<unsigned short>(settings.ServerPort));
    }
    void Shutdown()
    {
        g_ready.store(false); g_command.store(Command::None); RemoveHooks();
    }
    bool IsReady() { return g_ready.load(std::memory_order_acquire); }
}
